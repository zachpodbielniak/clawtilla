/*
 * gtk-schedule.c - The Schedule page: routines across a day, week and month
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Day, Week, Month and Upcoming.  What a card says, which window a view
 * asks for, and the sentence a refused window keeps are
 * lrg-schedule-model.c, which this file is compiled with.  This file
 * only draws that and sends routine.occurrences / routine.upcoming.
 *
 * The request goes through clawt_client_request() rather than
 * clawt_window_request(): the latter toasts and drops the GError, and
 * a window past daemon.calendar_max_days or calendar_max_occurrences
 * has to be shown as the sentence the daemon wrote.
 */

#include "clawt-window-private.h"

#include "../lrg/clawt-lrg-schedule-model.h"

/* A month cell shows this many cards, then "+N more". */
#define MONTH_FIT (3)

typedef struct {
	ClawtLrgSchedAction	action;
	gchar		       *task;
	gchar		       *routine;
	gchar		       *tooltip;
	gchar		       *expand_key;
} SchedClick;

static struct {
	ClawtWindow	       *window;
	GtkWidget	       *list;
	GtkWidget	       *banner;
	GtkWidget	       *zone;
	GtkWidget	       *heading;
	GtkWidget	       *detail;
	GtkWidget	       *unscheduled;
	GtkWidget	       *views[4];
	AdwComboRow	       *agent;
	AdwComboRow	       *team;
	GtkSwitch	       *disabled;
	GPtrArray	       *agent_ids;
	GPtrArray	       *team_ids;
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
	JsonNode	       *cached;
} sched;

static void load_schedule(ClawtWindow *self, gboolean allow_realign);
static void draw_schedule(ClawtWindow *self);

static void
click_free(gpointer data)
{
	SchedClick *click = data;

	if (click == NULL)
		return;

	g_free(click->task);
	g_free(click->routine);
	g_free(click->tooltip);
	g_free(click->expand_key);
	g_free(click);
}

static const gchar *
badge_css(const gchar *tone)
{
	if (g_strcmp0(tone, "success") == 0)
		return "success";
	if (g_strcmp0(tone, "error") == 0)
		return "error";
	if (g_strcmp0(tone, "warning") == 0)
		return "warning";
	if (g_strcmp0(tone, "accent") == 0)
		return "accent";

	return "dim-label";
}

static const gchar *
selected_id(AdwComboRow *row, GPtrArray *ids)
{
	guint index;

	if (row == NULL || ids == NULL)
		return NULL;

	index = adw_combo_row_get_selected(row);

	if (index >= ids->len)
		return NULL;

	return g_ptr_array_index(ids, index);
}

static void
select_id(AdwComboRow *row, GPtrArray *ids, const gchar *id)
{
	guint i;

	if (row == NULL || ids == NULL)
		return;

	for (i = 0; i < ids->len; i++) {
		if (g_strcmp0(g_ptr_array_index(ids, i), id) == 0) {
			adw_combo_row_set_selected(row, i);
			return;
		}
	}

	adw_combo_row_set_selected(row, 0);
}

static void
fill_combo(AdwComboRow *row, GPtrArray *ids, JsonArray *items,
	   const gchar *id_key, const gchar *label_key)
{
	GtkStringList *labels = gtk_string_list_new(NULL);
	g_autofree gchar *keep = g_strdup(selected_id(row, ids));
	guint i;

	g_ptr_array_set_size(ids, 0);
	gtk_string_list_append(labels, "All");
	g_ptr_array_add(ids, g_strdup(""));

	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		const gchar *id = clawt_json_string(item, id_key, NULL);
		const gchar *label = clawt_json_string(item, label_key, id);

		if (id == NULL || *id == '\0')
			continue;

		if (label == NULL || *label == '\0')
			label = id;

		gtk_string_list_append(labels, label);
		g_ptr_array_add(ids, g_strdup(id));
	}

	sched.filling = TRUE;
	adw_combo_row_set_model(row, G_LIST_MODEL(labels));
	select_id(row, ids, keep);
	sched.filling = FALSE;
	g_object_unref(labels);
}

static void
ensure_filters(ClawtWindow *self)
{
	g_autoptr(JsonNode) agents = NULL;
	g_autoptr(JsonNode) teams = NULL;
	JsonObject *root;
	JsonArray *items;

	if (sched.filters_ready || sched.agent == NULL)
		return;

	agents = clawt_window_request_quiet(self, "agent.list", NULL);
	teams = clawt_window_request_quiet(self, "team.list", NULL);

	if (agents == NULL && teams == NULL)
		return;

	root = clawt_payload_of(agents);
	items = (root != NULL && json_object_has_member(root, "agents"))
		? json_object_get_array_member(root, "agents") : NULL;
	fill_combo(sched.agent, sched.agent_ids, items, "id", "id");

	root = clawt_payload_of(teams);
	items = (root != NULL && json_object_has_member(root, "teams"))
		? json_object_get_array_member(root, "teams") : NULL;
	fill_combo(sched.team, sched.team_ids, items, "id", "name");
	sched.filters_ready = TRUE;
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
	if (task != NULL && *task != '\0' && sched.tasks != NULL)
		g_hash_table_add(sched.tasks, g_strdup(task));
}

static void
on_card(GtkButton *button, gpointer user_data)
{
	ClawtWindow *self = user_data;
	SchedClick *click = g_object_get_data(G_OBJECT(button), "click");

	if (click == NULL)
		return;

	switch (click->action) {
	case CLAWT_LRG_SCHED_ACT_TASK:
		clawt_gtk_show_task(self, click->task);
		break;
	case CLAWT_LRG_SCHED_ACT_ROUTINE:
		clawt_gtk_edit_routine(self, click->routine);
		break;
	case CLAWT_LRG_SCHED_ACT_DETAIL:
		gtk_label_set_text(GTK_LABEL(sched.detail),
				   click->tooltip != NULL ? click->tooltip
							  : "");
		gtk_widget_set_visible(sched.detail, TRUE);
		break;
	case CLAWT_LRG_SCHED_ACT_EXPAND:
		if (expanded_has(click->expand_key))
			g_hash_table_remove(sched.expanded, click->expand_key);
		else
			g_hash_table_add(sched.expanded,
					 g_strdup(click->expand_key));
		draw_schedule(self);
		break;
	case CLAWT_LRG_SCHED_ACT_NONE:
		break;
	}
}

static GtkWidget *
card_button(ClawtWindow *self, const ClawtLrgSchedRow *row,
	    const gchar *expand_key)
{
	GtkWidget *button = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *title = gtk_label_new(row->title != NULL ? row->title : "");
	g_autofree gchar *sub = NULL;
	GtkWidget *subtitle;
	SchedClick *click = g_new0(SchedClick, 1);
	const gchar *mark = row->status_label;

	click->action = row->action;
	click->task = g_strdup(row->task);
	click->routine = g_strdup(row->routine);
	click->tooltip = g_strdup(row->tooltip);
	click->expand_key = g_strdup(expand_key);

	if (row->group && row->summary != NULL)
		gtk_label_set_text(GTK_LABEL(title), row->summary);

	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	gtk_widget_set_hexpand(title, TRUE);
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_box_append(GTK_BOX(top), title);

	if (row->source_note != NULL)
		gtk_box_append(GTK_BOX(top),
			       clawt_gtk_badge(row->source_note, "dim-label",
					       row->source_note));

	if (mark != NULL)
		gtk_box_append(GTK_BOX(top),
			       clawt_gtk_badge(mark, badge_css(row->tone),
					       row->tooltip));

	sub = g_strdup_printf("%s  %s%s%s",
			      row->agent != NULL ? row->agent : "",
			      row->when != NULL ? row->when : "",
			      row->counts != NULL ? "  " : "",
			      row->counts != NULL ? row->counts : "");
	subtitle = gtk_label_new(sub);
	gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
	gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
	gtk_widget_add_css_class(subtitle, "dim-label");
	gtk_widget_add_css_class(subtitle, "caption");

	gtk_box_append(GTK_BOX(box), top);
	gtk_box_append(GTK_BOX(box), subtitle);
	gtk_widget_set_halign(box, GTK_ALIGN_FILL);
	gtk_button_set_child(GTK_BUTTON(button), box);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_tooltip_text(button, row->tooltip);
	g_object_set_data_full(G_OBJECT(button), "click", click, click_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_card), self);

	return button;
}

static void
append_row(ClawtWindow *self, GtkWidget *into, const ClawtLrgSchedRow *row,
	   gboolean indent)
{
	g_autofree gchar *key = NULL;
	GtkWidget *button;

	if (row->group)
		key = g_strdup_printf("group\t%s\t%s",
				      row->routine != NULL ? row->routine : "",
				      row->day != NULL ? row->day : "");

	button = card_button(self, row, key);

	if (indent)
		gtk_widget_set_margin_start(button, 18);

	gtk_box_append(GTK_BOX(into), button);

	if (row->action == CLAWT_LRG_SCHED_ACT_TASK)
		note_task(row->task);

	if (row->group && expanded_has(key) && row->children != NULL) {
		guint i;

		for (i = 0; i < row->children->len; i++) {
			const ClawtLrgSchedRow *child =
				g_ptr_array_index(row->children, i);

			append_row(self, into, child, TRUE);
		}
	}
}

static void
clear_box(GtkWidget *box)
{
	GtkWidget *child = gtk_widget_get_first_child(box);

	while (child != NULL) {
		GtkWidget *next = gtk_widget_get_next_sibling(child);

		gtk_widget_unparent(child);
		child = next;
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
draw_month(ClawtWindow *self, GPtrArray *rows, const gchar *from)
{
	GtkWidget *grid = gtk_grid_new();
	g_autoptr(GDateTime) start = noon_of(from);
	gint column;
	gint week;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

	for (column = 0; column < 7; column++) {
		g_autoptr(GDateTime) sample = NULL;
		g_autofree gchar *name = NULL;
		GtkWidget *label;

		if (start == NULL)
			break;

		sample = g_date_time_add_days(start, column);
		name = g_date_time_format(sample, "%a");
		label = gtk_label_new(name);
		gtk_widget_add_css_class(label, "caption");
		gtk_widget_add_css_class(label, "dim-label");
		gtk_grid_attach(GTK_GRID(grid), label, column, 0, 1, 1);
	}

	for (week = 0; week < 6; week++) {
		for (column = 0; column < 7; column++) {
			g_autoptr(GDateTime) day = NULL;
			g_autofree gchar *ymd = NULL;
			g_autofree gchar *num = NULL;
			g_autofree gchar *more_key = NULL;
			GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL,
						      2);
			GtkWidget *heading;
			g_autoptr(GPtrArray) mine = g_ptr_array_new();
			guint shown;
			guint i;

			if (start == NULL)
				continue;

			day = g_date_time_add_days(start, week * 7 + column);
			ymd = g_date_time_format(day, "%Y-%m-%d");
			num = g_date_time_format(day, "%-d");
			heading = gtk_label_new(num);
			gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
			gtk_widget_add_css_class(heading, "heading");
			gtk_box_append(GTK_BOX(cell), heading);
			gtk_widget_add_css_class(cell, "card");
			gtk_widget_set_margin_top(cell, 2);
			gtk_widget_set_margin_bottom(cell, 2);

			rows_for_day(rows, ymd, mine);
			more_key = g_strdup_printf("more\t%s", ymd);
			shown = mine->len;

			if (shown > MONTH_FIT && !expanded_has(more_key))
				shown = MONTH_FIT;

			for (i = 0; i < shown; i++)
				append_row(self, cell,
					   g_ptr_array_index(mine, i), FALSE);

			if (mine->len > shown) {
				GtkWidget *more = gtk_button_new_with_label("");
				g_autofree gchar *label = NULL;
				SchedClick *click = g_new0(SchedClick, 1);

				label = g_strdup_printf("+%u more",
							mine->len - shown);
				gtk_button_set_label(GTK_BUTTON(more), label);
				gtk_widget_add_css_class(more, "flat");
				click->action = CLAWT_LRG_SCHED_ACT_EXPAND;
				click->expand_key = g_strdup(more_key);
				g_object_set_data_full(G_OBJECT(more), "click",
						       click, click_free);
				g_signal_connect(more, "clicked",
						 G_CALLBACK(on_card), self);
				gtk_box_append(GTK_BOX(cell), more);
			}

			gtk_grid_attach(GTK_GRID(grid), cell, column,
					week + 1, 1, 1);
		}
	}

	gtk_box_append(GTK_BOX(sched.list), grid);
}

static void
draw_list(ClawtWindow *self, GPtrArray *rows)
{
	const gchar *last = NULL;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const ClawtLrgSchedRow *row = g_ptr_array_index(rows, i);

		if (g_strcmp0(row->day, last) != 0 && row->day != NULL) {
			GtkWidget *heading = gtk_label_new(row->day);

			gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
			gtk_widget_add_css_class(heading, "heading");
			gtk_widget_set_margin_top(heading, 8);
			gtk_box_append(GTK_BOX(sched.list), heading);
			last = row->day;
		}

		append_row(self, sched.list, row, FALSE);
	}

	if (rows->len == 0) {
		GtkWidget *empty = gtk_label_new("Nothing in this window.");

		gtk_widget_add_css_class(empty, "dim-label");
		gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
		gtk_box_append(GTK_BOX(sched.list), empty);
	}
}

static void
draw_unscheduled(ClawtWindow *self)
{
	g_autoptr(GPtrArray) rows = clawt_lrg_schedule_unscheduled(sched.cached);
	guint i;

	clear_box(sched.unscheduled);
	gtk_widget_set_visible(sched.unscheduled, rows->len > 0);

	if (rows->len == 0)
		return;

	{
		GtkWidget *heading = gtk_label_new(
			"Not on the calendar");

		gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
		gtk_widget_add_css_class(heading, "heading");
		gtk_box_append(GTK_BOX(sched.unscheduled), heading);
	}

	for (i = 0; i < rows->len; i++)
		append_row(self, sched.unscheduled,
			   g_ptr_array_index(rows, i), FALSE);
}

static void
draw_schedule(ClawtWindow *self)
{
	g_autoptr(GPtrArray) rows = clawt_lrg_schedule_rows(sched.cached);
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *title = NULL;

	(void)self;

	if (sched.list == NULL)
		return;

	g_hash_table_remove_all(sched.tasks);
	clear_box(sched.list);
	line = clawt_lrg_schedule_zone_line(sched.cached);
	gtk_label_set_text(GTK_LABEL(sched.zone), line);

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

	gtk_label_set_text(GTK_LABEL(sched.heading), title);

	if (sched.view == CLAWT_LRG_SCHED_MONTH) {
		g_clear_pointer(&from, g_free);
		g_clear_pointer(&to, g_free);
		clawt_lrg_schedule_range(sched.view, sched.y, sched.m, sched.d,
					 &from, &to);
		draw_month(sched.window, rows, from);
	} else {
		draw_list(sched.window, rows);
	}

	draw_unscheduled(sched.window);
}

static void
set_banner(const gchar *text)
{
	gboolean show = text != NULL && *text != '\0';

	gtk_label_set_text(GTK_LABEL(sched.banner), show ? text : "");
	gtk_widget_set_visible(sched.banner, show);
}

static void
load_schedule(ClawtWindow *self, gboolean allow_realign)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonNode) payload = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	JsonObject *root;
	const gchar *zone;
	const gchar *agent;
	const gchar *team;
	gboolean disabled;

	if (!sched.have_day) {
		clawt_lrg_schedule_today(sched.zone_name, &sched.y, &sched.m,
					 &sched.d);
		sched.have_day = TRUE;
		sched.snapped = TRUE;
	}

	agent = selected_id(sched.agent, sched.agent_ids);
	team = selected_id(sched.team, sched.team_ids);
	disabled = sched.disabled != NULL &&
		gtk_switch_get_active(sched.disabled);

	if (sched.view == CLAWT_LRG_SCHED_UPCOMING) {
		payload = clawt_lrg_schedule_upcoming_payload(agent, team);
		reply = clawt_client_request(self->client, "routine.upcoming",
					     g_steal_pointer(&payload),
					     &error);
	} else {
		clawt_lrg_schedule_range(sched.view, sched.y, sched.m, sched.d,
					 &from, &to);
		payload = clawt_lrg_schedule_occurrences_payload(
			from, to, agent, team, disabled,
			clawt_lrg_schedule_wants_collapse(sched.view));
		reply = clawt_client_request(self->client,
					     "routine.occurrences",
					     g_steal_pointer(&payload),
					     &error);
	}

	if (reply == NULL) {
		g_autofree gchar *text = clawt_lrg_schedule_error_text(error);

		set_banner(text);
		g_clear_pointer(&sched.cached, json_node_unref);
		draw_schedule(self);
		return;
	}

	set_banner(NULL);
	root = clawt_payload_of(reply);
	zone = (root != NULL) ? clawt_json_string(root, "time_zone", NULL)
			      : NULL;

	if (allow_realign && sched.snapped && zone != NULL &&
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
			load_schedule(self, FALSE);
			return;
		}
	} else if (zone != NULL && sched.zone_name == NULL) {
		sched.zone_name = g_strdup(zone);
	}

	g_clear_pointer(&sched.cached, json_node_unref);
	sched.cached = g_steal_pointer(&reply);
	draw_schedule(self);
}

static void
mark_view(void)
{
	guint i;

	for (i = 0; i < 4; i++) {
		if (sched.views[i] == NULL)
			continue;

		if (i == (guint)sched.view)
			gtk_widget_add_css_class(sched.views[i],
						  "suggested-action");
		else
			gtk_widget_remove_css_class(sched.views[i],
						    "suggested-action");
	}
}

static void
on_view(GtkButton *button, gpointer user_data)
{
	sched.view = (ClawtLrgSchedView)GPOINTER_TO_INT(
		g_object_get_data(G_OBJECT(button), "view"));
	mark_view();
	g_hash_table_remove_all(sched.expanded);
	clawt_gtk_refresh_schedule(user_data);
}

static void
on_move(GtkButton *button, gpointer user_data)
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
	clawt_gtk_refresh_schedule(user_data);
}

static void
on_filter_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
	(void)object;
	(void)pspec;

	if (sched.filling)
		return;

	clawt_gtk_refresh_schedule(user_data);
}

void
clawt_gtk_refresh_schedule(ClawtWindow *self)
{
	if (sched.list == NULL || sched.window != self)
		return;

	if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SCHEDULE))
		return;

	do {
		ensure_filters(self);
		load_schedule(self, TRUE);
	} while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SCHEDULE));
}

gboolean
clawt_gtk_schedule_shows_task(ClawtWindow *self, const gchar *task_id)
{
	if (self == NULL || sched.window != self || task_id == NULL ||
	    sched.tasks == NULL)
		return FALSE;

	return g_hash_table_contains(sched.tasks, task_id);
}

GtkWidget *
clawt_gtk_build_schedule_page(ClawtWindow *self)
{
	GtkWidget *scroll = gtk_scrolled_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *filters = gtk_list_box_new();
	GtkWidget *disabled_row = adw_action_row_new();
	GtkWidget *previous;
	GtkWidget *today;
	GtkWidget *next;
	guint i;

	sched.window = self;
	sched.view = CLAWT_LRG_SCHED_WEEK;
	sched.agent_ids = g_ptr_array_new_with_free_func(g_free);
	sched.team_ids = g_ptr_array_new_with_free_func(g_free);
	sched.expanded = g_hash_table_new_full(g_str_hash, g_str_equal,
					       g_free, NULL);
	sched.tasks = g_hash_table_new_full(g_str_hash, g_str_equal,
					    g_free, NULL);
	g_ptr_array_add(sched.agent_ids, g_strdup(""));
	g_ptr_array_add(sched.team_ids, g_strdup(""));

	gtk_widget_set_margin_top(box, 12);
	gtk_widget_set_margin_bottom(box, 12);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);

	sched.banner = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(sched.banner), TRUE);
	gtk_label_set_xalign(GTK_LABEL(sched.banner), 0.0f);
	gtk_widget_add_css_class(sched.banner, "error");
	gtk_widget_set_visible(sched.banner, FALSE);
	gtk_box_append(GTK_BOX(box), sched.banner);

	sched.heading = gtk_label_new("");
	gtk_widget_add_css_class(sched.heading, "title-2");
	gtk_label_set_xalign(GTK_LABEL(sched.heading), 0.0f);
	gtk_box_append(GTK_BOX(box), sched.heading);

	sched.zone = gtk_label_new("");
	gtk_widget_add_css_class(sched.zone, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(sched.zone), 0.0f);
	gtk_box_append(GTK_BOX(box), sched.zone);

	for (i = 0; i < clawt_lrg_schedule_view_count(); i++) {
		sched.views[i] = gtk_button_new_with_label(
			clawt_lrg_schedule_view_label((ClawtLrgSchedView)i));
		g_object_set_data(G_OBJECT(sched.views[i]), "view",
				  GINT_TO_POINTER((gint)i));
		g_signal_connect(sched.views[i], "clicked",
				 G_CALLBACK(on_view), self);
		gtk_box_append(GTK_BOX(nav), sched.views[i]);
	}

	previous = gtk_button_new_with_label("Previous");
	today = gtk_button_new_with_label("Today");
	next = gtk_button_new_with_label("Next");
	g_object_set_data(G_OBJECT(previous), "direction",
			  GINT_TO_POINTER(-1));
	g_object_set_data(G_OBJECT(today), "direction", GINT_TO_POINTER(0));
	g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
	g_signal_connect(previous, "clicked", G_CALLBACK(on_move), self);
	g_signal_connect(today, "clicked", G_CALLBACK(on_move), self);
	g_signal_connect(next, "clicked", G_CALLBACK(on_move), self);
	gtk_box_append(GTK_BOX(nav), previous);
	gtk_box_append(GTK_BOX(nav), today);
	gtk_box_append(GTK_BOX(nav), next);
	gtk_box_append(GTK_BOX(box), nav);
	mark_view();

	sched.agent = ADW_COMBO_ROW(adw_combo_row_new());
	sched.team = ADW_COMBO_ROW(adw_combo_row_new());
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sched.agent),
				      "Agent");
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sched.team),
				      "Team");
	{
		GtkStringList *one = gtk_string_list_new(NULL);

		gtk_string_list_append(one, "All");
		adw_combo_row_set_model(sched.agent, G_LIST_MODEL(one));
		g_object_unref(one);
		one = gtk_string_list_new(NULL);
		gtk_string_list_append(one, "All");
		adw_combo_row_set_model(sched.team, G_LIST_MODEL(one));
		g_object_unref(one);
	}
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(disabled_row),
				      "Show turned-off routines");
	sched.disabled = GTK_SWITCH(gtk_switch_new());
	gtk_widget_set_valign(GTK_WIDGET(sched.disabled), GTK_ALIGN_CENTER);
	adw_action_row_add_suffix(ADW_ACTION_ROW(disabled_row),
				  GTK_WIDGET(sched.disabled));
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(filters),
					GTK_SELECTION_NONE);
	gtk_widget_add_css_class(filters, "boxed-list");
	gtk_list_box_append(GTK_LIST_BOX(filters), GTK_WIDGET(sched.agent));
	gtk_list_box_append(GTK_LIST_BOX(filters), GTK_WIDGET(sched.team));
	gtk_list_box_append(GTK_LIST_BOX(filters), disabled_row);
	gtk_box_append(GTK_BOX(box), filters);

	g_signal_connect(sched.agent, "notify::selected",
			 G_CALLBACK(on_filter_changed), self);
	g_signal_connect(sched.team, "notify::selected",
			 G_CALLBACK(on_filter_changed), self);
	g_signal_connect(sched.disabled, "notify::active",
			 G_CALLBACK(on_filter_changed), self);

	sched.detail = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(sched.detail), TRUE);
	gtk_label_set_xalign(GTK_LABEL(sched.detail), 0.0f);
	gtk_label_set_selectable(GTK_LABEL(sched.detail), TRUE);
	gtk_widget_add_css_class(sched.detail, "caption");
	gtk_widget_set_visible(sched.detail, FALSE);
	gtk_box_append(GTK_BOX(box), sched.detail);

	sched.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(box), sched.list);
	sched.unscheduled = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_visible(sched.unscheduled, FALSE);
	gtk_box_append(GTK_BOX(box), sched.unscheduled);

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	return scroll;
}
