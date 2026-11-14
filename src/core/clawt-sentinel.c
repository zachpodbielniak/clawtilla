/*
 * clawt-sentinel.c - A read-only scan of the event log for recurring trouble
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-sentinel.h"

#include <yaml-glib.h>
#include <string.h>

/* The report's own version, so a reader can refuse a shape it does not know. */
#define SENTINEL_VERSION (1)

/*
 * A line longer than this is not an event the daemon wrote, and is
 * skipped rather than parsed: the bound is on what is handed to the
 * parser, which is where the memory would go.
 */
#define SENTINEL_MAX_LINE (1024 * 1024)

/* At most this many issues in a report, worst first. */
#define SENTINEL_MAX_ISSUES (200)

/* How much of an event's detail an issue quotes. */
#define SENTINEL_SAMPLE_CHARS (200)

/* One issue while it is being counted. */
typedef struct {
	gchar  *kind;
	gchar  *subject;
	gchar  *key;
	gint64  count;
	gint64  first_at;    /* microseconds */
	gint64  last_at;
	gint64  value;
	gchar  *sample;
	gboolean bad;
} Bucket;

static void
bucket_free(
	gpointer data
){
	Bucket *bucket = data;

	g_free(bucket->kind);
	g_free(bucket->subject);
	g_free(bucket->key);
	g_free(bucket->sample);
	g_free(bucket);
}

typedef struct {
	const ClawtSentinelOptions *options;
	gint64      now_usec;
	GHashTable *buckets;   /* "kind\037subject\037key" -> Bucket* */
	GHashTable *queued;    /* agent -> GQueue of gint64* (queued at, usec) */
	GHashTable *spend;     /* "agent\037day" -> gint64* micros */
	GHashTable *spend_at;  /* same key -> gint64* last usec */
	gint64      events;
	gint64      skipped;
} Scan;

static void
queue_free(
	gpointer data
){
	g_queue_free_full(data, g_free);
}

/* A sentinel.* default, read out of the schema rather than restated. */
static gint64
schema_default(
	const gchar *key
){
	const ClawtSchemaEntry *entry = clawt_config_schema_lookup(key);

	return (entry != NULL && entry->default_value != NULL)
		? g_ascii_strtoll(entry->default_value, NULL, 10) : 0;
}

void
clawt_sentinel_options_init(
	ClawtSentinelOptions *options
){
	g_return_if_fail(options != NULL);

	options->days = (guint)MAX(schema_default("sentinel.days"), 0);
	options->min_failures =
		(guint)MAX(schema_default("sentinel.min_failures"), 0);
	options->queue_wait_seconds =
		(guint)MAX(schema_default("sentinel.queue_wait_seconds"), 0);
	options->daily_spend_micros =
		MAX(schema_default("sentinel.daily_spend_micros"), 0);
	options->now_usec = 0;
}

static Bucket *
bucket_for(
	Scan        *scan,
	const gchar *kind,
	const gchar *subject,
	const gchar *key,
	gint64       at
){
	g_autofree gchar *id = g_strdup_printf("%s\037%s\037%s", kind,
		subject != NULL ? subject : "", key != NULL ? key : "");
	Bucket *bucket = g_hash_table_lookup(scan->buckets, id);

	if (bucket == NULL) {
		bucket = g_new0(Bucket, 1);
		bucket->kind = g_strdup(kind);
		bucket->subject = g_strdup(subject != NULL ? subject : "");
		bucket->key = g_strdup(key != NULL ? key : "");
		bucket->first_at = at;
		g_hash_table_insert(scan->buckets, g_steal_pointer(&id), bucket);
	}

	bucket->count++;
	bucket->first_at = MIN(bucket->first_at, at);
	bucket->last_at = MAX(bucket->last_at, at);

	return bucket;
}

static void
bucket_sample(
	Bucket      *bucket,
	const gchar *text
){
	if (text == NULL || *text == '\0')
		return;

	g_free(bucket->sample);
	bucket->sample = clawt_clip_line(text, SENTINEL_SAMPLE_CHARS);
}

static const gchar *
detail(
	JsonObject  *event,
	const gchar *key
){
	JsonObject *details;
	JsonNode *node;

	if (!json_object_has_member(event, "detail"))
		return NULL;

	node = json_object_get_member(event, "detail");

	if (!JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	details = json_node_get_object(node);

	if (details == NULL || !json_object_has_member(details, key))
		return NULL;

	node = json_object_get_member(details, key);

	if (!JSON_NODE_HOLDS_VALUE(node) ||
		json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

/* Event details are strings on the wire; an integer one is spelled out. */
static gint64
detail_int(
	JsonObject  *event,
	const gchar *key,
	gint64       fallback
){
	const gchar *text = detail(event, key);
	gchar *end = NULL;
	gint64 value;

	if (text == NULL || *text == '\0')
		return fallback;

	value = g_ascii_strtoll(text, &end, 10);

	return (end != NULL && *end == '\0') ? value : fallback;
}

static gchar *
day_of(
	gint64 usec
){
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(
		usec / G_USEC_PER_SEC);

	return (when != NULL) ? g_date_time_format(when, "%Y-%m-%d")
		: g_strdup("unknown");
}

/* ── One event ───────────────────────────────────────────────────── */

static void
note_queue_wait(
	Scan        *scan,
	const gchar *agent,
	gint64       queued_at,
	gint64       delivered_at
){
	gint64 waited = (delivered_at - queued_at) / G_USEC_PER_SEC;
	Bucket *bucket;

	if (waited < (gint64)scan->options->queue_wait_seconds)
		return;

	bucket = bucket_for(scan, "queue_wait", agent, "delivered late",
		delivered_at);
	bucket->value = MAX(bucket->value, waited);
}

static void
take_event(
	Scan       *scan,
	JsonObject *event
){
	const gchar *kind = json_object_has_member(event, "kind")
		? json_object_get_string_member(event, "kind") : NULL;
	const gchar *subject = json_object_has_member(event, "subject")
		? json_object_get_string_member(event, "subject") : NULL;
	gint64 at = json_object_has_member(event, "ts")
		? json_object_get_int_member(event, "ts") : 0;

	if (kind == NULL) {
		scan->skipped++;
		return;
	}

	scan->events++;

	/* ── A tool that keeps failing ── */
	if (g_strcmp0(kind, "turn.step") == 0) {
		Bucket *bucket;

		if (detail_int(event, "failed", 0) == 0)
			return;

		bucket = bucket_for(scan, "tool_failure", subject,
			detail(event, "tool") != NULL ? detail(event, "tool") : "a tool",
			at);
		bucket_sample(bucket, detail(event, "detail") != NULL
			? detail(event, "detail") : detail(event, "text"));
		return;
	}

	if (g_strcmp0(kind, "computer.exec") == 0) {
		Bucket *bucket;

		if (detail_int(event, "exit", 0) == 0)
			return;

		bucket = bucket_for(scan, "tool_failure", subject,
			"clawtilla_computer_exec", at);
		bucket_sample(bucket, detail(event, "command"));
		return;
	}

	/* ── A queue that keeps somebody waiting ── */
	if (g_strcmp0(kind, "mailbox.queued") == 0 && subject != NULL) {
		GQueue *queue = g_hash_table_lookup(scan->queued, subject);
		gint64 *when = g_new(gint64, 1);

		if (queue == NULL) {
			queue = g_queue_new();
			g_hash_table_insert(scan->queued, g_strdup(subject), queue);
		}

		*when = at;
		g_queue_push_tail(queue, when);
		return;
	}

	if (g_strcmp0(kind, "mailbox.delivered") == 0 && subject != NULL) {
		GQueue *queue = g_hash_table_lookup(scan->queued, subject);
		g_autofree gint64 *when = (queue != NULL)
			? g_queue_pop_head(queue) : NULL;

		if (when != NULL)
			note_queue_wait(scan, subject, *when, at);
		return;
	}

	/* ── Spending ── */
	if (g_strcmp0(kind, "turn.cost") == 0 && subject != NULL) {
		g_autofree gchar *day = day_of(at);
		g_autofree gchar *id = g_strdup_printf("%s\037%s", subject, day);
		gint64 *sum = g_hash_table_lookup(scan->spend, id);
		gint64 *last = g_hash_table_lookup(scan->spend_at, id);
		gint64 cost = detail_int(event, "cost_micros", 0);

		if (cost <= 0)
			return;

		if (sum == NULL) {
			sum = g_new0(gint64, 1);
			last = g_new0(gint64, 1);
			g_hash_table_insert(scan->spend, g_strdup(id), sum);
			g_hash_table_insert(scan->spend_at, g_strdup(id), last);
		}

		*sum += cost;
		*last = MAX(*last, at);
		return;
	}

	if (g_strcmp0(kind, "spending.limit-reached") == 0) {
		Bucket *bucket = bucket_for(scan, "spend", subject, "daily cap", at);

		bucket->bad = TRUE;
		bucket->value = MAX(bucket->value,
			detail_int(event, "cost_micros", 0));
		return;
	}

	/* ── Turns that stop or loop ── */
	if (g_strcmp0(kind, "turn.timed_out") == 0 ||
		g_strcmp0(kind, "turn.repeating") == 0 ||
		g_strcmp0(kind, "exchange.stalled") == 0 ||
		g_strcmp0(kind, "message.refused") == 0) {
		Bucket *bucket = bucket_for(scan, "stall", subject, kind, at);

		if (g_strcmp0(kind, "turn.timed_out") == 0)
			bucket->bad = TRUE;

		bucket_sample(bucket, detail(event, "detail") != NULL
			? detail(event, "detail") : detail(event, "reason"));
	}
}

/* ── Reading the files, read-only ───────────────────────────────── */

static void
take_file(
	Scan        *scan,
	const gchar *path
){
	g_autoptr(GMappedFile) mapped = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	const gchar *data;
	const gchar *end;
	const gchar *line;
	gsize length;

	/*
	 * Mapped, and mapped read-only: `writable` FALSE is the property
	 * this whole file rests on.  A scanner that could write would be a
	 * second writer into the record it exists to read.
	 */
	mapped = g_mapped_file_new(path, FALSE, NULL);

	if (mapped == NULL) {
		scan->skipped++;
		return;
	}

	data = g_mapped_file_get_contents(mapped);
	length = g_mapped_file_get_length(mapped);

	if (data == NULL || length == 0)
		return;

	end = data + length;
	line = data;

	while (line < end) {
		const gchar *newline = memchr(line, '\n', (gsize)(end - line));
		const gchar *stop = (newline != NULL) ? newline : end;
		gsize size = (gsize)(stop - line);

		if (size > 0 && size <= SENTINEL_MAX_LINE &&
			json_parser_load_from_data(parser, line, (gssize)size, NULL) &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)) &&
			json_node_get_object(json_parser_get_root(parser)) != NULL)
			take_event(scan,
				json_node_get_object(json_parser_get_root(parser)));
		else if (size > 0)
			scan->skipped++;

		line = stop + 1;
	}
}

/* ── The report ──────────────────────────────────────────────────── */

static gchar *
title_for(
	Bucket *bucket
){
	const gchar *who = (bucket->subject != NULL && *bucket->subject != '\0')
		? bucket->subject : "the fleet";

	if (g_strcmp0(bucket->kind, "tool_failure") == 0)
		return g_strdup_printf("%s failed %" G_GINT64_FORMAT " times for %s",
			bucket->key, bucket->count, who);

	if (g_strcmp0(bucket->kind, "queue_wait") == 0) {
		if (g_strcmp0(bucket->key, "still queued") == 0)
			return g_strdup_printf("%" G_GINT64_FORMAT " message%s still "
				"waiting for %s, the oldest for %" G_GINT64_FORMAT " min",
				bucket->count, bucket->count == 1 ? "" : "s", who,
				bucket->value / 60);

		return g_strdup_printf("%s kept messages waiting %" G_GINT64_FORMAT
			" time%s, up to %" G_GINT64_FORMAT " min", who, bucket->count,
			bucket->count == 1 ? "" : "s", bucket->value / 60);
	}

	if (g_strcmp0(bucket->kind, "spend") == 0) {
		g_autofree gchar *cost = clawt_usage_format_cost(bucket->value);

		if (g_strcmp0(bucket->key, "daily cap") == 0)
			return g_strdup_printf("%s reached a daily spending cap "
				"%" G_GINT64_FORMAT " time%s (%s)", who, bucket->count,
				bucket->count == 1 ? "" : "s", cost);

		return g_strdup_printf("%s spent %s on %s", who, cost, bucket->key);
	}

	if (g_strcmp0(bucket->key, "turn.timed_out") == 0)
		return g_strdup_printf("%s's turn was stopped by the watchdog "
			"%" G_GINT64_FORMAT " time%s", who, bucket->count,
			bucket->count == 1 ? "" : "s");

	if (g_strcmp0(bucket->key, "turn.repeating") == 0)
		return g_strdup_printf("%s repeated the same tool call "
			"%" G_GINT64_FORMAT " time%s", who, bucket->count,
			bucket->count == 1 ? "" : "s");

	if (g_strcmp0(bucket->key, "exchange.stalled") == 0)
		return g_strdup_printf("An exchange in %s stalled %" G_GINT64_FORMAT
			" time%s", who, bucket->count, bucket->count == 1 ? "" : "s");

	return g_strdup_printf("Messages to %s were refused %" G_GINT64_FORMAT
		" time%s", who, bucket->count, bucket->count == 1 ? "" : "s");
}

/* Worst first: bad before warning, then the most frequent, then newest. */
static gint
worst_first(
	gconstpointer a,
	gconstpointer b
){
	const Bucket *x = *(const Bucket * const *)a;
	const Bucket *y = *(const Bucket * const *)b;

	if (x->bad != y->bad)
		return x->bad ? -1 : 1;
	if (x->count != y->count)
		return (x->count > y->count) ? -1 : 1;
	if (x->last_at != y->last_at)
		return (x->last_at > y->last_at) ? -1 : 1;

	return g_strcmp0(x->subject, y->subject);
}

/* Which buckets are issues, and how bad, once everything is counted. */
static GPtrArray *
collect(
	Scan *scan
){
	GPtrArray *out = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	/* What is still queued when the log ends, and has waited too long. */
	g_hash_table_iter_init(&iter, scan->queued);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GQueue *queue = value;
		GList *l;

		for (l = queue->head; l != NULL; l = l->next) {
			gint64 waited = (scan->now_usec - *(gint64 *)l->data) /
				G_USEC_PER_SEC;
			Bucket *bucket;

			if (waited < (gint64)scan->options->queue_wait_seconds)
				continue;

			bucket = bucket_for(scan, "queue_wait", key, "still queued",
				*(gint64 *)l->data);
			bucket->value = MAX(bucket->value, waited);
		}
	}

	/* One agent's day of spend, against the line. */
	if (scan->options->daily_spend_micros > 0) {
		g_hash_table_iter_init(&iter, scan->spend);

		while (g_hash_table_iter_next(&iter, &key, &value)) {
			gint64 sum = *(gint64 *)value;
			gint64 *last = g_hash_table_lookup(scan->spend_at, key);
			g_auto(GStrv) parts = NULL;
			Bucket *bucket;

			if (sum < scan->options->daily_spend_micros)
				continue;

			parts = g_strsplit(key, "\037", 2);
			bucket = bucket_for(scan, "spend", parts[0],
				parts[1] != NULL ? parts[1] : "", last != NULL ? *last : 0);
			bucket->value = sum;
			bucket->bad = sum >= 2 * scan->options->daily_spend_micros;
		}
	}

	g_hash_table_iter_init(&iter, scan->buckets);

	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Bucket *bucket = value;

		if (g_strcmp0(bucket->kind, "tool_failure") == 0) {
			if (bucket->count < (gint64)MAX(scan->options->min_failures, 1))
				continue;

			bucket->bad = bucket->count >=
				3 * (gint64)MAX(scan->options->min_failures, 1);
		}

		if (g_strcmp0(bucket->kind, "queue_wait") == 0)
			bucket->bad = bucket->value >=
				4 * (gint64)MAX(scan->options->queue_wait_seconds, 1);

		g_ptr_array_add(out, bucket);
	}

	g_ptr_array_sort(out, worst_first);

	return out;
}

static JsonNode *
build_report(
	Scan        *scan,
	const gchar *since,
	guint        files
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) issues = collect(scan);
	guint i;

	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "sentinel");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, SENTINEL_VERSION);
	json_builder_set_member_name(builder, "scanned_at");
	json_builder_add_int_value(builder, scan->now_usec / G_USEC_PER_SEC);
	json_builder_set_member_name(builder, "since");
	json_builder_add_string_value(builder, since != NULL ? since : "");
	json_builder_set_member_name(builder, "files");
	json_builder_add_int_value(builder, files);
	json_builder_set_member_name(builder, "events");
	json_builder_add_int_value(builder, scan->events);
	json_builder_set_member_name(builder, "skipped");
	json_builder_add_int_value(builder, scan->skipped);
	json_builder_set_member_name(builder, "truncated");
	json_builder_add_int_value(builder,
		issues->len > SENTINEL_MAX_ISSUES
			? (gint64)(issues->len - SENTINEL_MAX_ISSUES) : 0);
	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "issues");
	json_builder_begin_array(builder);

	for (i = 0; i < issues->len && i < SENTINEL_MAX_ISSUES; i++) {
		Bucket *bucket = g_ptr_array_index(issues, i);
		g_autofree gchar *title = title_for(bucket);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, bucket->kind);
		json_builder_set_member_name(builder, "severity");
		json_builder_add_string_value(builder,
			bucket->bad ? "bad" : "warning");
		json_builder_set_member_name(builder, "subject");
		json_builder_add_string_value(builder, bucket->subject);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, bucket->key);
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, title);
		json_builder_set_member_name(builder, "count");
		json_builder_add_int_value(builder, bucket->count);
		json_builder_set_member_name(builder, "first_at");
		json_builder_add_int_value(builder, bucket->first_at / G_USEC_PER_SEC);
		json_builder_set_member_name(builder, "last_at");
		json_builder_add_int_value(builder, bucket->last_at / G_USEC_PER_SEC);
		json_builder_set_member_name(builder, "value");
		json_builder_add_int_value(builder, bucket->value);
		json_builder_set_member_name(builder, "sample");
		json_builder_add_string_value(builder,
			bucket->sample != NULL ? bucket->sample : "");
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static gint
by_name(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

JsonNode *
clawt_sentinel_scan(
	const gchar                *events_dir,
	const ClawtSentinelOptions *options,
	GError                    **error
){
	ClawtSentinelOptions defaults;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autofree gchar *since = NULL;
	Scan scan = { 0 };
	JsonNode *report;
	const gchar *name;
	guint files = 0;
	guint i;

	g_return_val_if_fail(events_dir != NULL, NULL);

	if (options == NULL) {
		clawt_sentinel_options_init(&defaults);
		options = &defaults;
	}

	scan.options = options;
	scan.now_usec = options->now_usec > 0 ? options->now_usec
		: g_get_real_time();

	dir = g_dir_open(events_dir, 0, error);

	if (dir == NULL)
		return NULL;

	/*
	 * The files are named for their UTC day, so the window is a string
	 * comparison against the first day it includes.
	 */
	if (options->days > 0)
		since = day_of(scan.now_usec -
			(gint64)(options->days - 1) * 86400 * G_USEC_PER_SEC);

	names = g_ptr_array_new_with_free_func(g_free);

	while ((name = g_dir_read_name(dir)) != NULL) {
		if (!g_str_has_suffix(name, ".ndjson"))
			continue;

		if (since != NULL && g_strcmp0(name, since) < 0)
			continue;

		g_ptr_array_add(names, g_strdup(name));
	}

	/* Oldest first, so a queue's first-in first-out pairing is in order. */
	g_ptr_array_sort(names, by_name);

	scan.buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		bucket_free);
	scan.queued = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		queue_free);
	scan.spend = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		g_free);
	scan.spend_at = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		g_free);

	for (i = 0; i < names->len; i++) {
		g_autofree gchar *path = g_build_filename(events_dir,
			(const gchar *)g_ptr_array_index(names, i), NULL);

		take_file(&scan, path);
		files++;
	}

	report = build_report(&scan, since, files);

	g_hash_table_unref(scan.buckets);
	g_hash_table_unref(scan.queued);
	g_hash_table_unref(scan.spend);
	g_hash_table_unref(scan.spend_at);

	return report;
}

/* ── YAML on disk ────────────────────────────────────────────────── */

gchar *
clawt_sentinel_report_to_yaml(
	JsonNode  *report,
	GError   **error
){
	g_autoptr(YamlNode) root = NULL;
	g_autoptr(YamlGenerator) generator = NULL;

	g_return_val_if_fail(report != NULL, NULL);

	root = yaml_node_from_json_node(report);
	generator = yaml_generator_new();
	yaml_generator_set_root(generator, root);

	return yaml_generator_to_data(generator, NULL, error);
}

/*
 * YAML has no integer type of its own on the way back through a plain
 * scalar, so a number may come back as the string "42".  The report's
 * numeric members are known, and are made numbers again here rather
 * than in every reader -- a client reading a count with the integer
 * reader would otherwise get its fallback, silently.
 */
static void
renumber(
	JsonObject  *object,
	const gchar *const *members
){
	guint i;

	for (i = 0; object != NULL && members[i] != NULL; i++) {
		JsonNode *node;

		if (!json_object_has_member(object, members[i]))
			continue;

		node = json_object_get_member(object, members[i]);

		if (JSON_NODE_HOLDS_VALUE(node) &&
			json_node_get_value_type(node) == G_TYPE_STRING)
			json_object_set_int_member(object, members[i],
				g_ascii_strtoll(json_node_get_string(node), NULL, 10));
	}
}

JsonNode *
clawt_sentinel_report_from_yaml(
	const gchar  *text,
	GError      **error
){
	static const gchar *const header[] = { "version", "scanned_at", "files",
		"events", "skipped", "truncated", NULL };
	static const gchar *const issue[] = { "count", "first_at", "last_at",
		"value", NULL };
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(JsonNode) report = NULL;
	JsonObject *root;
	JsonArray *issues;
	YamlNode *node;
	guint i;

	g_return_val_if_fail(text != NULL, NULL);

	if (!yaml_parser_load_from_data(parser, text, -1, error))
		return NULL;

	node = yaml_parser_get_root(parser);

	if (node == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_PARSE,
			"the sentinel report is empty");
		return NULL;
	}

	report = yaml_node_to_json_node(node);
	root = JSON_NODE_HOLDS_OBJECT(report) ? json_node_get_object(report)
		: NULL;

	if (root == NULL || !json_object_has_member(root, "sentinel") ||
		!JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "sentinel"))) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_PARSE,
			"that file is not a sentinel report");
		return NULL;
	}

	renumber(json_object_get_object_member(root, "sentinel"), header);

	issues = (json_object_has_member(root, "issues") &&
		JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "issues")))
		? json_object_get_array_member(root, "issues") : NULL;

	/* An empty sequence can come back as nothing at all; say [] instead. */
	if (issues == NULL) {
		json_object_set_array_member(root, "issues", json_array_new());
		issues = json_object_get_array_member(root, "issues");
	}

	for (i = 0; i < json_array_get_length(issues); i++) {
		JsonNode *element = json_array_get_element(issues, i);

		if (JSON_NODE_HOLDS_OBJECT(element))
			renumber(json_node_get_object(element), issue);
	}

	return g_steal_pointer(&report);
}

/* Whether @path is @dir or anything under it, on the canonical spelling. */
static gboolean
path_is_inside(
	const gchar *path,
	const gchar *dir
){
	g_autofree gchar *canonical_dir = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *canonical_parent = NULL;
	gsize length;

	if (dir == NULL)
		return FALSE;

	canonical_dir = g_canonicalize_filename(dir, NULL);
	parent = g_path_get_dirname(path);
	canonical_parent = g_canonicalize_filename(parent, NULL);

	/* Resolve symlinks where they exist, so a link into the log is caught. */
	{
		g_autofree gchar *real_dir = realpath(canonical_dir, NULL);
		g_autofree gchar *real_parent = realpath(canonical_parent, NULL);

		if (real_dir != NULL) {
			g_free(canonical_dir);
			canonical_dir = g_steal_pointer(&real_dir);
		}

		if (real_parent != NULL) {
			g_free(canonical_parent);
			canonical_parent = g_steal_pointer(&real_parent);
		}
	}

	length = strlen(canonical_dir);

	return g_str_has_prefix(canonical_parent, canonical_dir) &&
		(canonical_parent[length] == '\0' || canonical_parent[length] == '/');
}

gboolean
clawt_sentinel_write_report(
	JsonNode     *report,
	const gchar  *path,
	const gchar  *events_dir,
	GError      **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;

	g_return_val_if_fail(report != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (path_is_inside(path, events_dir)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
			"%s is inside the event log (%s); the sentinel only reads "
			"there, so write the report somewhere else", path, events_dir);
		return FALSE;
	}

	text = clawt_sentinel_report_to_yaml(report, error);

	if (text == NULL)
		return FALSE;

	dir = g_path_get_dirname(path);

	if (!clawt_ensure_dir(dir, 0700, error))
		return FALSE;

	return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

/* ── Words ───────────────────────────────────────────────────────── */

const gchar *
clawt_sentinel_kind_label(
	const gchar *kind
){
	if (g_strcmp0(kind, "tool_failure") == 0)
		return "Failing tool";
	if (g_strcmp0(kind, "queue_wait") == 0)
		return "Slow queue";
	if (g_strcmp0(kind, "spend") == 0)
		return "Spending";
	if (g_strcmp0(kind, "stall") == 0)
		return "Stalled turns";

	return (kind != NULL) ? kind : "Issue";
}

gchar *
clawt_sentinel_age_text(
	gint64 scanned_at,
	gint64 now
){
	g_autofree gchar *ago = NULL;

	if (scanned_at <= 0)
		return g_strdup("Not scanned yet");

	ago = clawt_time_ago_label(scanned_at * G_USEC_PER_SEC,
		now * G_USEC_PER_SEC);

	return g_strdup_printf("Scanned %s", ago);
}
