/*
 * lrg-schedule.c - The Schedule page (mirrors clients/gtk/gtk-schedule.c)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Day, Week, Month and Upcoming.  The rows, the window and the refused
 * bound's sentence are lrg-schedule-model.c.  This file draws them and
 * asks routine.occurrences / routine.upcoming.  A refused window is the
 * daemon's sentence, in the banner, not a shorter one and not a retry.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-schedule-model.h"

#define MONTH_FIT (3)

static struct {
	ClawtLrgApp	       *app;
	LrgWidget	       *banner;
	LrgWidget	       *zone;
	LrgWidget	       *heading;
	LrgWidget	       *detail;
	LrgWidget	       *list;
	LrgWidget	       *unscheduled;
	LrgWidget	       *agent_slot;
	LrgWidget	       *team_slot;
	LrgWidget	       *agent;
	LrgWidget	       *team;
	LrgWidget	       *disabled;
	LrgWidget	       *views[4];
	ClawtLrgSchedView	view;
	gint			y;
	gint			m;
	gint			d;
	gboolean		have_day;
	gboolean		snapped;
	gchar		       *zone_name;
	GHashTable	       *expanded;
	GHashTable	       *tasks;
	gboolean		filling;
	gboolean		filters_ready;
	guint			seq;
	JsonNode	       *cached;
} sched;

static void ask(ClawtLrgApp *app, gboolean allow_realign);

static const gchar *
badge_tone(const gchar *tone)
{
	if (g_strcmp0(tone, "success") == 0)
		return "good";
	if (g_strcmp0(tone, "error") == 0)
		return "bad";
	if (g_strcmp0(tone, "warning") == 0)
		return "warn";
	if (g_strcmp0(tone, "accent") == 0)
		return "info";

	return "neutral";
}

static gboolean
expanded_has(const gchar *key)
{
	return key != NULL && sched.expanded != NULL &&
		g_hash_table_contains(sched.expanded, key);
}

static void
note_task(const gchar *task)
{
	if (task != NULL && *task != '\0')
		g_hash_table_add(sched.tasks, g_strdup(task));
}

static const gchar *
filter_id(LrgWidget *combo)
{
	const gchar *id;

	if (combo == NULL)
		return NULL;

	id = clawt_lrg_combo_id(combo);

	if (id != NULL && *id == '\0')
		return NULL;

	return id;
}

static void
on_open(LrgButton *button, gpointer user_data)
{
	ClawtLrgApp *app = user_data;
	ClawtLrgSchedAction action = (ClawtLrgSchedAction)GPOINTER_TO_INT(
		g_object_get_data(G_OBJECT(button), "action"));
	const gchar *task = g_object_get_data(G_OBJECT(button), "task");
	const gchar *routine = g_object_get_data(G_OBJECT(button), "routine");
	const gchar *tooltip = g_object_get_data(G_OBJECT(button), "tooltip");
	const gchar *key = g_object_get_data(G_OBJECT(button), "expand-key");

	switch (action) {
	case CLAWT_LRG_SCHED_ACT_TASK:
		clawt_lrg_tasks_show(app, task);
		break;
	case CLAWT_LRG_SCHED_ACT_ROUTINE:
		clawt_lrg_routines_edit(app, routine);
		break;
	case CLAWT_LRG_SCHED_ACT_DETAIL:
		clawt_lrg_paragraph_set_text(sched.detail,
					     tooltip != NULL ? tooltip : "");
		break;
	case CLAWT_LRG_SCHED_ACT_EXPAND:
		if (expanded_has(key))
			g_hash_table_remove(sched.expanded, key);
		else if (key != NULL)
			g_hash_table_add(sched.expanded, g_strdup(key));
		ask(app, FALSE);
		break;
	case CLAWT_LRG_SCHED_ACT_NONE:
		break;
	}
}

/*
 * Expand redraws the reply already held.  ask() would fetch again, and
 * a fetch is the wrong answer for a layout toggle.  The button above
 * calls ask only when it has no cache; draw_cached covers the rest.
 */
static void draw(ClawtLrgApp *app);

static void
on_open_expand(LrgButton *button, gpointer user_data)
{
	const gchar *key = g_object_get_data(G_OBJECT(button), "expand-key");
	ClawtLrgSchedAction action = (ClawtLrgSchedAction)GPOINTER_TO_INT(
		g_object_get_data(G_OBJECT(button), "action"));

	if (action != CLAWT_LRG_SCHED_ACT_EXPAND || sched.cached == NULL) {
		on_open(button, user_data);
		return;
	}

	if (expanded_has(key))
		g_hash_table_remove(sched.expanded, key);
	else if (key != NULL)
		g_hash_table_add(sched.expanded, g_strdup(key));

	draw(user_data);
}

static void
stamp(LrgWidget *button, const ClawtLrgSchedRow *row, const gchar *key)
{
	g_object_set_data(G_OBJECT(button), "action",
			  GINT_TO_POINTER(row->action));
	g_object_set_data_full(G_OBJECT(button), "task", g_strdup(row->task),
			       g_free);
	g_object_set_data_full(G_OBJECT(button), "routine",
			       g_strdup(row->routine), g_free);
	g_object_set_data_full(G_OBJECT(button), "tooltip",
			       g_strdup(row->tooltip), g_free);
	g_object_set_data_full(G_OBJECT(button), "expand-key", g_strdup(key),
			       g_free);
}

static const gchar *
verb_for(ClawtLrgSchedAction action)
{
	switch (action) {
	case CLAWT_LRG_SCHED_ACT_TASK:
		return "Task";
	case CLAWT_LRG_SCHED_ACT_ROUTINE:
		return "Edit";
	case CLAWT_LRG_SCHED_ACT_EXPAND:
		return "Expand";
	case CLAWT_LRG_SCHED_ACT_DETAIL:
		return "Details";
	case CLAWT_LRG_SCHED_ACT_NONE:
		return NULL;
	}

	return NULL;
}

static void
append_row(ClawtLrgApp *app, LrgWidget *into, const ClawtLrgSchedRow *row)
{
	g_autofree gchar *key = NULL;
	g_autofree gchar *subtitle = NULL;
	LrgWidget *body = clawt_lrg_vbox(4.0f);
	LrgWidget *line;
	const gchar *verb;
	const gchar *title = row->title;

	if (row->group) {
		key = g_strdup_printf("group\t%s\t%s",
				      row->routine != NULL ? row->routine : "",
				      row->day != NULL ? row->day : "");
		if (row->summary != NULL)
			title = row->summary;
	}

	subtitle = g_strdup_printf("%s  %s%s%s",
				   row->agent != NULL ? row->agent : "",
				   row->when != NULL ? row->when : "",
				   row->counts != NULL ? "  " : "",
				   row->counts != NULL ? row->counts : "");
	line = clawt_lrg_row(title != NULL ? title : "", subtitle);

	if (row->source_note != NULL)
		clawt_lrg_row_add_prefix(line,
					 clawt_lrg_badge(row->source_note,
							 "neutral"));

	if (row->status_label != NULL)
		clawt_lrg_row_add_suffix(line,
					 clawt_lrg_badge(row->status_label,
							 badge_tone(row->tone)));

	verb = verb_for(row->action);

	if (verb != NULL) {
		LrgWidget *button = clawt_lrg_button(
			verb, "flat",
			G_CALLBACK(row->action == CLAWT_LRG_SCHED_ACT_EXPAND
				   ? on_open_expand : on_open),
			app);

		stamp(button, row, key);
		clawt_lrg_row_add_suffix(line, button);
	}

	clawt_lrg_take(body, line);

	if (row->tooltip != NULL && *row->tooltip != '\0')
		clawt_lrg_take(body, clawt_lrg_paragraph(row->tooltip,
							 "muted"));

	clawt_lrg_take(into, clawt_lrg_card(body));

	if (row->action == CLAWT_LRG_SCHED_ACT_TASK)
		note_task(row->task);

	if (row->group && expanded_has(key) && row->children != NULL) {
		guint i;

		for (i = 0; i < row->children->len; i++)
			append_row(app, into,
				   g_ptr_array_index(row->children, i));
	}
}

static GDateTime *
noon_of(const gchar *ymd)
{
	g_autofree gchar *iso = NULL;

	if (ymd == NULL)
		return NULL;

	iso = g_strdup_printf("%sT12:00:00Z", ymd);
	return g_date_time_new_from_iso8601(iso, NULL);
}

static void
rows_for_day(GPtrArray *rows, const gchar *day, GPtrArray *out)
{
	guint i;

	for (i = 0; rows != NULL && i < rows->len; i++) {
		ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);

		if (g_strcmp0(row->day, day) == 0)
			g_ptr_array_add(out, row);
	}
}

static void
draw_month(ClawtLrgApp *app, GPtrArray *rows, const gchar *from)
{
	g_autoptr(GDateTime) start = noon_of(from);
	gint week;

	for (week = 0; week < 6; week++) {
		LrgWidget *line = clawt_lrg_hbox(6.0f);
		gint column;

		for (column = 0; column < 7; column++) {
			g_autoptr(GDateTime) day = NULL;
			g_autofree gchar *ymd = NULL;
			g_autofree gchar *num = NULL;
			g_autofree gchar *more_key = NULL;
			LrgWidget *cell = clawt_lrg_vbox(2.0f);
			g_autoptr(GPtrArray) mine = g_ptr_array_new();
			guint shown;
			guint i;

			if (start == NULL)
				continue;

			day = g_date_time_add_days(start, week * 7 + column);
			ymd = g_date_time_format(day, "%Y-%m-%d");
			num = g_date_time_format(day, "%-d");
			clawt_lrg_take(cell, clawt_lrg_label(num, "heading"));
			rows_for_day(rows, ymd, mine);
			more_key = g_strdup_printf("more\t%s", ymd);
			shown = mine->len;

			if (shown > MONTH_FIT && !expanded_has(more_key))
				shown = MONTH_FIT;

			for (i = 0; i < shown; i++)
				append_row(app, cell,
					   g_ptr_array_index(mine, i));

			if (mine->len > shown) {
				g_autofree gchar *label = g_strdup_printf(
					"+%u more", mine->len - shown);
				LrgWidget *more = clawt_lrg_button(
					label, "flat",
					G_CALLBACK(on_open_expand), app);
				ClawtLrgSchedRow fake = { 0 };

				fake.action = CLAWT_LRG_SCHED_ACT_EXPAND;
				stamp(more, &fake, more_key);
				clawt_lrg_take(cell, more);
			}

			clawt_lrg_take(line, clawt_lrg_card(cell));
		}

		clawt_lrg_take(sched.list, line);
	}
}

static void
draw_list(ClawtLrgApp *app, GPtrArray *rows)
{
	const gchar *last = NULL;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);

		if (g_strcmp0(row->day, last) != 0 && row->day != NULL) {
			clawt_lrg_take(sched.list,
				       clawt_lrg_section(row->day));
			last = row->day;
		}

		append_row(app, sched.list, row);
	}

	if (rows->len == 0)
		clawt_lrg_take(sched.list,
			       clawt_lrg_empty_state("Nothing in this window",
						     "This range has no runs."));
}

static void
draw(ClawtLrgApp *app)
{
	g_autoptr(GPtrArray) rows = clawt_lrg_schedule_rows(sched.cached);
	g_autoptr(GPtrArray) extra = clawt_lrg_schedule_unscheduled(sched.cached);
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *title = NULL;
	guint i;

	if (sched.list == NULL)
		return;

	g_hash_table_remove_all(sched.tasks);
	clawt_lrg_clear(sched.list);
	clawt_lrg_clear(sched.unscheduled);
	line = clawt_lrg_schedule_zone_line(sched.cached);
	clawt_lrg_paragraph_set_text(sched.zone, line);

	if (sched.view == CLAWT_LRG_SCHED_UPCOMING)
		title = g_strdup("Upcoming");
	else if (sched.view == CLAWT_LRG_SCHED_MONTH)
		title = g_strdup_printf("%04d-%02d", sched.y, sched.m);
	else if (sched.view == CLAWT_LRG_SCHED_DAY)
		title = g_strdup_printf("%04d-%02d-%02d", sched.y, sched.m,
					sched.d);
	else {
		clawt_lrg_schedule_range(sched.view, sched.y, sched.m, sched.d,
					 &from, &to);
		title = g_strdup_printf("%s to %s", from, to);
	}

	clawt_lrg_paragraph_set_text(sched.heading, title);

	if (sched.view == CLAWT_LRG_SCHED_MONTH) {
		g_clear_pointer(&from, g_free);
		clawt_lrg_schedule_range(sched.view, sched.y, sched.m, sched.d,
					 &from, &to);
		draw_month(app, rows, from);
	} else {
		draw_list(app, rows);
	}

	if (extra->len > 0)
		clawt_lrg_take(sched.unscheduled,
			       clawt_lrg_section("Not on the calendar"));

	for (i = 0; i < extra->len; i++)
		append_row(app, sched.unscheduled,
			   g_ptr_array_index(extra, i));

	app->dirty = TRUE;
}

static void
set_banner(const gchar *text)
{
	clawt_lrg_paragraph_set_text(sched.banner, text != NULL ? text : "");
}

static void
on_reply(ClawtLrgApp *app, JsonNode *reply, const GError *error,
	 gpointer user_data)
{
	guint token = GPOINTER_TO_UINT(user_data);
	gboolean allow = (token & 1u) != 0;
	JsonObject *root;
	const gchar *zone;

	/* token is sched.seq, whose low bit is allow_realign. */
	if (token != sched.seq || sched.list == NULL)
		return;

	if (reply == NULL) {
		g_autofree gchar *text = clawt_lrg_schedule_error_text(error);

		set_banner(text);
		g_clear_pointer(&sched.cached, json_node_unref);
		draw(app);
		return;
	}

	set_banner("");
	root = clawt_ipc_reply_object(reply);
	zone = (root != NULL) ? clawt_ipc_payload_string(root, "time_zone")
			      : NULL;

	if (allow && sched.snapped && zone != NULL &&
	    g_strcmp0(zone, sched.zone_name) != 0) {
		gint y;
		gint m;
		gint d;

		g_free(sched.zone_name);
		sched.zone_name = g_strdup(zone);
		clawt_lrg_schedule_today(sched.zone_name, &y, &m, &d);

		if (y != sched.y || m != sched.m || d != sched.d) {
			sched.y = y;
			sched.m = m;
			sched.d = d;
			ask(app, FALSE);
			return;
		}
	} else if (zone != NULL && sched.zone_name == NULL) {
		sched.zone_name = g_strdup(zone);
	}

	g_clear_pointer(&sched.cached, json_node_unref);
	sched.cached = json_node_ref(reply);
	draw(app);
}

static void
ask(ClawtLrgApp *app, gboolean allow_realign)
{
	JsonNode *payload;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	guint token;

	if (sched.list == NULL)
		return;

	if (!sched.have_day) {
		clawt_lrg_schedule_today(sched.zone_name, &sched.y, &sched.m,
					 &sched.d);
		sched.have_day = TRUE;
		sched.snapped = TRUE;
	}

	/*
	 * seq counts by two so the low bit can carry allow_realign
	 * through to the reply without a second allocation.
	 */
	sched.seq = (sched.seq + 2u) | (allow_realign ? 1u : 0u);
	token = sched.seq;

	if (sched.view == CLAWT_LRG_SCHED_UPCOMING) {
		payload = clawt_lrg_schedule_upcoming_payload(
			filter_id(sched.agent), filter_id(sched.team));
		clawt_lrg_request(app, "routine.upcoming", payload,
				  CLAWT_LRG_QUIET, on_reply,
				  GUINT_TO_POINTER(token), NULL);
		return;
	}

	clawt_lrg_schedule_range(sched.view, sched.y, sched.m, sched.d,
				 &from, &to);
	payload = clawt_lrg_schedule_occurrences_payload(
		from, to, filter_id(sched.agent), filter_id(sched.team),
		sched.disabled != NULL &&
			clawt_lrg_switch_active(sched.disabled),
		clawt_lrg_schedule_wants_collapse(sched.view));
	clawt_lrg_request(app, "routine.occurrences", payload,
			  CLAWT_LRG_QUIET, on_reply,
			  GUINT_TO_POINTER(token), NULL);
}

static void
mark_view(void)
{
	guint i;

	for (i = 0; i < 4; i++) {
		if (sched.views[i] == NULL)
			continue;

		if (i == (guint)sched.view)
			lrg_widget_add_class(sched.views[i], "primary");
		else
			lrg_widget_remove_class(sched.views[i], "primary");
	}
}

static void
on_view(LrgButton *button, gpointer user_data)
{
	sched.view = (ClawtLrgSchedView)GPOINTER_TO_INT(
		g_object_get_data(G_OBJECT(button), "view"));
	mark_view();
	g_hash_table_remove_all(sched.expanded);
	ask(user_data, TRUE);
}

static void
on_move(LrgButton *button, gpointer user_data)
{
	gint direction = GPOINTER_TO_INT(
		g_object_get_data(G_OBJECT(button), "direction"));
	gint ty;
	gint tm;
	gint td;

	clawt_lrg_schedule_today(sched.zone_name, &ty, &tm, &td);
	sched.snapped = direction == 0;
	clawt_lrg_schedule_move(sched.view, direction, ty, tm, td,
				&sched.y, &sched.m, &sched.d);
	sched.have_day = TRUE;
	g_hash_table_remove_all(sched.expanded);
	ask(user_data, TRUE);
}

static void
on_filter(GObject *object, gpointer user_data)
{
	(void)object;

	if (sched.filling)
		return;

	ask(user_data, TRUE);
}

static void
combo_lists(JsonArray *items, const gchar *id_key, const gchar *label_key,
	    gchar ***ids_out, gchar ***labels_out)
{
	GPtrArray *ids = g_ptr_array_new();
	GPtrArray *labels = g_ptr_array_new();
	guint i;

	g_ptr_array_add(ids, g_strdup(""));
	g_ptr_array_add(labels, g_strdup("All"));

	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		const gchar *id = clawt_ipc_payload_string(item, id_key);
		const gchar *label = clawt_ipc_payload_string(item, label_key);

		if (id == NULL || *id == '\0')
			continue;

		if (label == NULL || *label == '\0')
			label = id;

		g_ptr_array_add(ids, g_strdup(id));
		g_ptr_array_add(labels, g_strdup(label));
	}

	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(labels, NULL);
	*ids_out = (gchar **)g_ptr_array_free(ids, FALSE);
	*labels_out = (gchar **)g_ptr_array_free(labels, FALSE);
}

static void
install_combo(LrgWidget **where, LrgWidget *slot, gchar **ids, gchar **labels,
	      const gchar *keep, ClawtLrgApp *app)
{
	LrgWidget *combo;

	sched.filling = TRUE;
	clawt_lrg_clear(slot);
	combo = clawt_lrg_combo((const gchar *const *)ids,
				(const gchar *const *)labels, keep);
	g_signal_connect(combo, "changed", G_CALLBACK(on_filter), app);
	clawt_lrg_add(slot, combo);
	*where = combo;
	g_object_unref(combo);
	sched.filling = FALSE;
	g_strfreev(ids);
	g_strfreev(labels);
}

static void
on_agents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
	  gpointer user_data)
{
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *items;
	gchar **ids = NULL;
	gchar **labels = NULL;
	const gchar *keep;

	(void)error;
	(void)user_data;

	if (sched.agent_slot == NULL || root == NULL)
		return;

	items = json_object_has_member(root, "agents")
		? json_object_get_array_member(root, "agents") : NULL;
	keep = filter_id(sched.agent);
	combo_lists(items, "id", "id", &ids, &labels);
	install_combo(&sched.agent, sched.agent_slot, ids, labels,
		      keep != NULL ? keep : "", app);
}

static void
on_teams(ClawtLrgApp *app, JsonNode *reply, const GError *error,
	 gpointer user_data)
{
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *items;
	gchar **ids = NULL;
	gchar **labels = NULL;
	const gchar *keep;

	(void)error;
	(void)user_data;

	if (sched.team_slot == NULL || root == NULL)
		return;

	items = json_object_has_member(root, "teams")
		? json_object_get_array_member(root, "teams") : NULL;
	keep = filter_id(sched.team);
	combo_lists(items, "id", "name", &ids, &labels);
	install_combo(&sched.team, sched.team_slot, ids, labels,
		      keep != NULL ? keep : "", app);
}

static void
ensure_filters(ClawtLrgApp *app)
{
	if (sched.filters_ready)
		return;

	sched.filters_ready = TRUE;
	clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET,
			  on_agents, NULL, NULL);
	clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET,
			  on_teams, NULL, NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
	LrgWidget *page = clawt_lrg_page_new("Schedule", NULL);
	LrgWidget *nav = clawt_lrg_hbox(6.0f);
	LrgWidget *body = clawt_lrg_vbox(8.0f);
	LrgWidget *previous;
	LrgWidget *today;
	LrgWidget *next;
	const gchar *all_ids[] = { "", NULL };
	const gchar *all_labels[] = { "All", NULL };
	guint i;

	sched.app = app;
	sched.view = CLAWT_LRG_SCHED_WEEK;
	sched.expanded = g_hash_table_new_full(g_str_hash, g_str_equal,
					       g_free, NULL);
	sched.tasks = g_hash_table_new_full(g_str_hash, g_str_equal,
					    g_free, NULL);

	sched.banner = clawt_lrg_paragraph("", "caption");
	lrg_widget_add_class(sched.banner, "clawt-tone-bad");
	sched.heading = clawt_lrg_paragraph("", "title");
	sched.zone = clawt_lrg_paragraph("", "muted");
	sched.detail = clawt_lrg_paragraph("", "caption");
	sched.list = clawt_lrg_vbox(6.0f);
	sched.unscheduled = clawt_lrg_vbox(6.0f);
	sched.agent_slot = clawt_lrg_hbox(0.0f);
	sched.team_slot = clawt_lrg_hbox(0.0f);
	sched.agent = clawt_lrg_combo(all_ids, all_labels, "");
	sched.team = clawt_lrg_combo(all_ids, all_labels, "");
	sched.disabled = clawt_lrg_switch("Show turned-off routines", FALSE);
	clawt_lrg_add(sched.agent_slot, sched.agent);
	clawt_lrg_add(sched.team_slot, sched.team);
	g_object_unref(sched.agent);
	g_object_unref(sched.team);
	g_signal_connect(sched.agent, "changed", G_CALLBACK(on_filter), app);
	g_signal_connect(sched.team, "changed", G_CALLBACK(on_filter), app);
	g_signal_connect(sched.disabled, "toggled", G_CALLBACK(on_filter),
			 app);

	for (i = 0; i < clawt_lrg_schedule_view_count(); i++) {
		sched.views[i] = clawt_lrg_button(
			clawt_lrg_schedule_view_label((ClawtLrgSchedView)i),
			NULL, G_CALLBACK(on_view), app);
		g_object_set_data(G_OBJECT(sched.views[i]), "view",
				  GINT_TO_POINTER((gint)i));
		clawt_lrg_add(nav, sched.views[i]);
		g_object_unref(sched.views[i]);
	}

	previous = clawt_lrg_button("Previous", "flat", G_CALLBACK(on_move),
				    app);
	today = clawt_lrg_button("Today", "flat", G_CALLBACK(on_move), app);
	next = clawt_lrg_button("Next", "flat", G_CALLBACK(on_move), app);
	g_object_set_data(G_OBJECT(previous), "direction",
			  GINT_TO_POINTER(-1));
	g_object_set_data(G_OBJECT(today), "direction", GINT_TO_POINTER(0));
	g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
	clawt_lrg_take(nav, previous);
	clawt_lrg_take(nav, today);
	clawt_lrg_take(nav, next);
	mark_view();

	clawt_lrg_take(clawt_lrg_page_toolbar(page), nav);
	clawt_lrg_take(body, sched.banner);
	clawt_lrg_take(body, sched.heading);
	clawt_lrg_take(body, sched.zone);
	clawt_lrg_take(body, sched.agent_slot);
	clawt_lrg_take(body, sched.team_slot);
	clawt_lrg_take(body, sched.disabled);
	clawt_lrg_take(body, sched.detail);
	clawt_lrg_add(body, sched.list);
	clawt_lrg_add(body, sched.unscheduled);
	g_object_unref(sched.list);
	g_object_unref(sched.unscheduled);
	clawt_lrg_take(clawt_lrg_page_body(page), body);

	return page;
}

static void
refresh(ClawtLrgApp *app)
{
	ensure_filters(app);
	ask(app, TRUE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
	const gchar *kind = clawt_event_get_kind(ev);
	const gchar *subject = clawt_event_get_subject(ev);

	if (app->page != CLAWT_PAGE_SCHEDULE)
		return;

	if (g_strcmp0(kind, "routine.ran") == 0 ||
	    g_strcmp0(kind, "routine.changed") == 0 ||
	    g_strcmp0(kind, "routine.turned_off") == 0 ||
	    (g_strcmp0(kind, "task.changed") == 0 && subject != NULL &&
	     sched.tasks != NULL &&
	     g_hash_table_contains(sched.tasks, subject)))
		ask(app, FALSE);
}

static const ClawtLrgPage page_impl = {
	CLAWT_PAGE_SCHEDULE, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_schedule_page(void)
{
	return &page_impl;
}
