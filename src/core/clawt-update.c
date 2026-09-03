/*
 * clawt-update.c - Knowing that a newer version exists
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon knew its own version and nothing ever asked whether it was
 * the current one.  In practice that meant finding out a fix existed by
 * reading the upstream log by hand -- and a defect was once diagnosed,
 * worked around and written up as unfixed while the fix had been sitting
 * upstream for seven commits.
 *
 * This is the checking and reporting half only.  Applying an update is
 * deliberately not here: an unattended update must not restart the
 * daemon under running turns, which is the fleet-quiesce problem
 * automated and unsupervised, so the apply step waits on a hold that can
 * drain.  Being *told* is most of the value anyway -- an operator who
 * knows an update exists no longer misdiagnoses a fixed bug.
 */

#include "clawt-update.h"

#include "clawt-error.h"
#include "clawt-util.h"

#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_USER_AGENT "clawtilla-update-check"

/* An hour is already far more often than a release appears. */
#define UPDATE_MIN_INTERVAL_HOURS (1)

/*
 * How long after start the first check happens.
 *
 * Not at start: an IPC handler must not wait on the network and neither
 * may daemon start, and a fetch there would make every fixture that
 * builds a daemon reach out.  Not a whole interval either -- somebody
 * who has just turned this on wants to know today, and a feature whose
 * first result is a day away is one nobody can tell is working.
 */
#define UPDATE_FIRST_DELAY_SECONDS (60)

/*
 * A release listing is small; a source answering with something huge is
 * a source we should not be reading.  Bounded before the body is
 * accumulated rather than after, since a limit applied to what has
 * already been read is not a limit.
 */
#define UPDATE_MAX_REPLY_BYTES (256 * 1024)

struct _ClawtUpdateCheck {
    GObject parent_instance;

    gchar       *current;
    gchar       *url;
    gint         interval_hours;

    gchar       *latest;
    gchar       *error;
    gint64       checked_at;      /* microseconds, 0 when never */
    gboolean     announced;       /* whether `latest` has been reported */

    GMainContext *context;        /* where the timer lives, borrowed */
    GSource      *timer;
    gboolean      first_done;     /* whether the short opening wait is over */
    GCancellable *cancellable;
    SoupSession  *session;
};

enum {
    SIGNAL_FOUND,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtUpdateCheck, clawt_update_check, G_TYPE_OBJECT)

/* ── Version arithmetic ──────────────────────────────────────────── */

/*
 * Reads one dotted component, and says where the digits stopped.
 *
 * The tail matters: "0.3.0-rc1" and "0.3.0" have the same numbers, and
 * the only thing that separates them is what follows.
 */
static gint64
component_at(const gchar *version, gsize index, const gchar **tail_out)
{
    const gchar *p = version;
    gsize seen = 0;
    gint64 value = 0;
    gchar *end = NULL;

    *tail_out = "";

    if (p == NULL)
        return 0;

    /* A release tag usually carries one and our own version never does. */
    if (*p == 'v' || *p == 'V')
        p++;

    while (seen < index) {
        p = strchr(p, '.');

        if (p == NULL)
            return 0;

        p++;
        seen++;
    }

    if (!g_ascii_isdigit(*p))
        return 0;

    value = g_ascii_strtoll(p, &end, 10);
    *tail_out = (end != NULL) ? end : "";

    return value;
}

/*
 * How many dotted components a version has, counting only the leading
 * run of numeric ones.  "0.3.0-rc1" has three.
 */
static gsize
component_count(const gchar *version)
{
    gsize i;

    for (i = 0; i < 8; i++) {
        const gchar *tail = NULL;
        const gchar *p = version;
        gsize seen = 0;

        if (p == NULL)
            return i;

        if (*p == 'v' || *p == 'V')
            p++;

        while (seen < i && p != NULL) {
            p = strchr(p, '.');

            if (p != NULL) {
                p++;
                seen++;
            }
        }

        if (p == NULL || !g_ascii_isdigit(*p))
            return i;

        (void)component_at(version, i, &tail);
    }

    return 8;
}

static gboolean
version_parses(const gchar *version)
{
    const gchar *p = version;

    if (p == NULL || *p == '\0')
        return FALSE;

    if (*p == 'v' || *p == 'V')
        p++;

    return g_ascii_isdigit(*p);
}

gint
clawt_update_version_compare(const gchar *a, const gchar *b)
{
    gsize n;
    gsize i;
    const gchar *tail_a = "";
    const gchar *tail_b = "";

    /*
     * A version that does not parse sorts older than one that does, and
     * two of them are equal.  That direction is not arbitrary: the
     * caller spends this as "is the source's version newer than mine",
     * so a source that answered with rubbish must never win.
     */
    if (!version_parses(a))
        return version_parses(b) ? -1 : 0;

    if (!version_parses(b))
        return 1;

    n = MAX(component_count(a), component_count(b));

    for (i = 0; i < n; i++) {
        gint64 va = component_at(a, i, &tail_a);
        gint64 vb = component_at(b, i, &tail_b);

        if (va != vb)
            return (va < vb) ? -1 : 1;
    }

    /*
     * Same numbers.  What follows the last one decides, and an empty
     * tail wins: 0.3.0 is the release and 0.3.0-rc1 is not it yet.
     */
    if (*tail_a == '\0' && *tail_b == '\0')
        return 0;

    if (*tail_a == '\0')
        return 1;

    if (*tail_b == '\0')
        return -1;

    return CLAMP(g_strcmp0(tail_a, tail_b), -1, 1);
}

/* ── Reading whatever the source answered with ───────────────────── */

static gchar *
version_from_object(JsonObject *object)
{
    static const gchar *const keys[] = { "version", "tag_name", "name" };
    gsize i;

    if (object == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(keys); i++) {
        JsonNode *member;

        if (!json_object_has_member(object, keys[i]))
            continue;

        member = json_object_get_member(object, keys[i]);

        /*
         * Checked rather than assumed: a JSON null is a member that is
         * present, and json_node_get_string() on one is a critical.
         */
        if (!JSON_NODE_HOLDS_VALUE(member) ||
            json_node_get_value_type(member) != G_TYPE_STRING)
            continue;

        if (version_parses(json_node_get_string(member)))
            return g_strdup(json_node_get_string(member));
    }

    return NULL;
}

gchar *
clawt_update_version_from_json(JsonNode *root)
{
    if (root == NULL)
        return NULL;

    /* A file somebody publishes, holding just the version. */
    if (JSON_NODE_HOLDS_VALUE(root) &&
        json_node_get_value_type(root) == G_TYPE_STRING)
        return version_parses(json_node_get_string(root))
                   ? g_strdup(json_node_get_string(root)) : NULL;

    /*
     * A type check is not a pointer check: json_node_new(JSON_NODE_OBJECT)
     * answers JSON_NODE_HOLDS_OBJECT() with TRUE and get_object() with
     * NULL, and this is reading somebody else's server.
     */
    if (JSON_NODE_HOLDS_OBJECT(root))
        return version_from_object(json_node_get_object(root));

    /*
     * Forgejo, Gitea and GitLab all answer a releases endpoint with an
     * array, newest first.  Walked rather than indexed at 0, because a
     * draft or a malformed entry at the front must not answer for the
     * whole list.
     */
    if (JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *array = json_node_get_array(root);
        guint i;

        for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
            JsonNode *element = json_array_get_element(array, i);
            gchar *found;

            if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element))
                continue;

            found = version_from_object(json_node_get_object(element));

            if (found != NULL)
                return found;
        }
    }

    return NULL;
}

/* ── The check itself ────────────────────────────────────────────── */

static void
record_failure(ClawtUpdateCheck *self, const gchar *reason)
{
    g_free(self->error);
    self->error = g_strdup(reason);
    self->checked_at = g_get_real_time();

    /*
     * Left in `latest`: a check that fails today does not un-discover
     * what yesterday's found, and clearing it would make an available
     * update disappear from every client the moment the network blinked.
     */
    g_warning("update check: %s", reason);
}

static void
on_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(ClawtUpdateCheck) self = CLAWT_UPDATE_CHECK(user_data);
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autofree gchar *found = NULL;
    const gchar *text;
    gsize length = 0;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    if (body == NULL) {
        record_failure(self, error != NULL ? error->message
                                           : "the source said nothing");
        return;
    }

    if (g_bytes_get_size(body) > UPDATE_MAX_REPLY_BYTES) {
        record_failure(self, "the source answered with more than a version");
        return;
    }

    text = g_bytes_get_data(body, &length);
    parser = json_parser_new();

    if (text == NULL || length == 0 ||
        !json_parser_load_from_data(parser, text, (gssize)length, &error)) {
        /*
         * Not JSON.  A plain text file holding a version is a perfectly
         * ordinary way to publish one, so it is tried before giving up.
         */
        g_autofree gchar *plain = g_strndup(text != NULL ? text : "", length);

        g_strstrip(plain);

        if (version_parses(plain)) {
            found = g_steal_pointer(&plain);
        } else {
            record_failure(self,
                           "the source's answer is neither JSON nor a "
                           "version");
            return;
        }
    } else {
        found = clawt_update_version_from_json(json_parser_get_root(parser));

        if (found == NULL) {
            record_failure(self,
                           "the source answered, but nothing in it looks "
                           "like a version");
            return;
        }
    }

    g_clear_pointer(&self->error, g_free);
    self->checked_at = g_get_real_time();

    if (g_strcmp0(self->latest, found) != 0) {
        g_free(self->latest);
        self->latest = g_strdup(found);
        self->announced = FALSE;
    }

    /*
     * Announced once per version, not once per check.  The timer fires
     * for as long as the daemon is up, and a notification every interval
     * about the same release is how somebody turns notifications off.
     */
    if (!self->announced &&
        clawt_update_version_compare(self->latest, self->current) > 0) {
        self->announced = TRUE;
        g_signal_emit(self, signals[SIGNAL_FOUND], 0, self->latest);
    }
}

static void arm(ClawtUpdateCheck *self, guint seconds);

static gboolean
on_timer(gpointer user_data)
{
    ClawtUpdateCheck *self = CLAWT_UPDATE_CHECK(user_data);
    g_autoptr(SoupMessage) message = NULL;

    /*
     * The opening wait is short and the rest are the configured
     * interval, so the first firing replaces its own source.  Done
     * before the request rather than after it: a check that fails must
     * still leave the repeating timer armed, or one unreachable moment
     * at startup stops the daemon ever asking again.
     */
    if (!self->first_done) {
        self->first_done = TRUE;
        arm(self, (guint)self->interval_hours * 3600u);
    }

    if (self->url == NULL || *self->url == '\0')
        return G_SOURCE_REMOVE;

    message = soup_message_new(SOUP_METHOD_GET, self->url);

    if (message == NULL) {
        record_failure(self, "the update URL is not a URL");
        return G_SOURCE_CONTINUE;
    }

    soup_session_send_and_read_async(self->session, message, G_PRIORITY_LOW,
                                     self->cancellable, on_reply,
                                     g_object_ref(self));

    return G_SOURCE_CONTINUE;
}

/*
 * Replaces whatever source is armed with one firing in @seconds.
 *
 * The context is the one captured where the first source was attached,
 * not whatever is thread-default when this runs -- on_timer() is
 * dispatched *from* a source, and dispatching one does not push its
 * context.
 */
static void
arm(ClawtUpdateCheck *self, guint seconds)
{
    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_clear_pointer(&self->timer, g_source_unref);
    }

    self->timer = g_timeout_source_new_seconds(seconds);
    g_source_set_callback(self->timer, on_timer, self, NULL);
    g_source_attach(self->timer, self->context);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clawt_update_check_dispose(GObject *object)
{
    ClawtUpdateCheck *self = CLAWT_UPDATE_CHECK(object);

    clawt_update_check_stop(self);

    g_clear_object(&self->cancellable);
    g_clear_object(&self->session);
    g_clear_pointer(&self->context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_update_check_parent_class)->dispose(object);
}

static void
clawt_update_check_finalize(GObject *object)
{
    ClawtUpdateCheck *self = CLAWT_UPDATE_CHECK(object);

    g_clear_pointer(&self->current, g_free);
    g_clear_pointer(&self->url, g_free);
    g_clear_pointer(&self->latest, g_free);
    g_clear_pointer(&self->error, g_free);

    G_OBJECT_CLASS(clawt_update_check_parent_class)->finalize(object);
}

static void
clawt_update_check_class_init(ClawtUpdateCheckClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_update_check_dispose;
    object_class->finalize = clawt_update_check_finalize;

    /**
     * ClawtUpdateCheck::found:
     * @self: the checker
     * @version: the newer version
     *
     * Emitted once per newly-discovered version, never once per check.
     */
    signals[SIGNAL_FOUND] =
        g_signal_new("found", CLAWT_TYPE_UPDATE_CHECK, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_update_check_init(ClawtUpdateCheck *self)
{
    self->interval_hours = UPDATE_MIN_INTERVAL_HOURS;
}

ClawtUpdateCheck *
clawt_update_check_new(const gchar *current, const gchar *url,
                       gint interval_hours)
{
    ClawtUpdateCheck *self = g_object_new(CLAWT_TYPE_UPDATE_CHECK, NULL);

    self->current = g_strdup(current);
    self->url = g_strdup(url);
    self->interval_hours = MAX(interval_hours, UPDATE_MIN_INTERVAL_HOURS);

    return self;
}

void
clawt_update_check_start(ClawtUpdateCheck *self)
{
    g_return_if_fail(CLAWT_IS_UPDATE_CHECK(self));

    if (self->timer != NULL)
        return;

    /*
     * Captured here, in the function that attaches the source, rather
     * than at a caller.  An embedded daemon's ambient context is a loop
     * nobody runs, and naming the context at the call site has failed
     * five times in this tree.
     */
    self->context = g_main_context_ref_thread_default();

    if (self->session == NULL)
        self->session = soup_session_new_with_options(
            "user-agent", UPDATE_USER_AGENT, NULL);

    if (self->cancellable == NULL)
        self->cancellable = g_cancellable_new();

    arm(self, UPDATE_FIRST_DELAY_SECONDS);

    g_info("update check: asking %s in %d seconds, then every %d hour(s)",
           self->url != NULL ? self->url : "(nowhere)",
           UPDATE_FIRST_DELAY_SECONDS, self->interval_hours);
}

void
clawt_update_check_stop(ClawtUpdateCheck *self)
{
    g_return_if_fail(CLAWT_IS_UPDATE_CHECK(self));

    if (self->cancellable != NULL)
        g_cancellable_cancel(self->cancellable);

    if (self->timer != NULL) {
        g_source_destroy(self->timer);
        g_clear_pointer(&self->timer, g_source_unref);
    }
}

const gchar *
clawt_update_check_get_latest(ClawtUpdateCheck *self)
{
    g_return_val_if_fail(CLAWT_IS_UPDATE_CHECK(self), NULL);

    return self->latest;
}

void
clawt_update_check_describe(ClawtUpdateCheck *self, JsonBuilder *builder)
{
    g_return_if_fail(CLAWT_IS_UPDATE_CHECK(self));
    g_return_if_fail(builder != NULL);

    json_builder_set_member_name(builder, "update");
    json_builder_begin_object(builder);

    /*
     * Whether an update is available is answered here rather than by
     * each client comparing two strings.  Three clients each writing a
     * version comparison is three chances to get "0.10.0 is older than
     * 0.9.0" wrong, and a wrong answer here reads as a working one.
     */
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(
        builder, self->latest != NULL &&
                 clawt_update_version_compare(self->latest,
                                              self->current) > 0);

    if (self->latest != NULL) {
        json_builder_set_member_name(builder, "latest");
        json_builder_add_string_value(builder, self->latest);
    }

    /*
     * Always written, including 0 for "never asked".  A client with
     * nothing to draw draws nothing, and nothing reads as "up to date" --
     * which is exactly how a check that has been erroring for a month
     * becomes worse than no check at all.
     */
    json_builder_set_member_name(builder, "checked_at");
    json_builder_add_int_value(builder, self->checked_at);

    if (self->error != NULL) {
        json_builder_set_member_name(builder, "error");
        json_builder_add_string_value(builder, self->error);
    }

    json_builder_end_object(builder);
}
