/*
 * lrg-alerts.c - The notifications drawer: what wants you, and what happened
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors GTK's notifications panel (build_alerts_panel() in
 * clawt-window.c), and like it is a view of the daemon's notification
 * list (`notice.list`, docs/clients.org "Notifications"): conversations
 * with something unread, decisions waiting, Grok jobs that need somebody,
 * and the alerts the daemon recorded from its own events.  Nothing is
 * kept here but the last reply -- reading, dismissing or muting in any
 * client shows here on the next `notice.changed`.
 *
 * Every row has a right-click menu (lrg_widget_set_context_menu()) whose
 * actions carry their target in the action itself, so the menu acts on
 * the row it was opened on however the drawer has been rebuilt since.
 * Older events -- the routine stream -- come from the daemon's event log
 * (`event.list`), on request.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-chat-model.h"

typedef struct {
    JsonNode  *reply;           /* the last notice.list, nullable */
    LrgWidget *drawer;
    LrgWidget *list;
    LrgWidget *bell;
    LrgWidget *history;
    gchar     *scope_agent;     /* nullable: only this agent's */
    gboolean   showing;
} Alerts;

static Alerts alerts_panel;

static void refresh_alerts(ClawtLrgApp *app);
static void notice_verb(ClawtLrgApp *app, const gchar *kind,
                        JsonNode *payload);
static JsonNode *all_payload(void);

static JsonObject *
reply_payload(void)
{
    return (alerts_panel.reply != NULL)
        ? clawt_ipc_reply_object(alerts_panel.reply) : NULL;
}

static JsonArray *
reply_array(const gchar *member)
{
    JsonObject *payload = reply_payload();

    if (payload == NULL || !json_object_has_member(payload, member) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(payload, member)))
        return NULL;

    return json_object_get_array_member(payload, member);
}

static void
update_bell(ClawtLrgApp *app)
{
    guint count = clawt_notice_list_badge(reply_payload());
    g_autofree gchar *label = NULL;

    if (alerts_panel.bell == NULL)
        return;

    label = (count > 0) ? g_strdup_printf("Alerts  %u", count)
                        : g_strdup("Alerts");
    lrg_button_set_text(LRG_BUTTON(alerts_panel.bell), label);

    if (count > 0)
        lrg_widget_add_class(alerts_panel.bell, "clawt-unread");
    else
        lrg_widget_remove_class(alerts_panel.bell, "clawt-unread");

    clawt_lrg_app_wake(app);
}

/* ── The list, from the daemon ───────────────────────────────────── */

static void
on_notices(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    g_autoptr(GHashTable) muted = NULL;

    (void)error;
    (void)user_data;

    /* a daemon too old to keep the list: nothing to draw but history */
    if (reply == NULL || clawt_ipc_reply_object(reply) == NULL)
        return;

    g_clear_pointer(&alerts_panel.reply, json_node_unref);
    alerts_panel.reply = json_node_ref(reply);

    /*
     * The muted set dims sidebar pills and leaves every total, so the
     * sidebar redraws when it moved -- and only then, since every
     * message lands here.
     */
    muted = clawt_notice_list_mutes(reply_payload());

    if (clawt_lrg_muted_take(app->muted, muted))
        clawt_lrg_sidebar_refresh(app);

    /*
     * One that lands in front of somebody has been seen as it arrived,
     * so it is read at once; otherwise the bell counts a row they watched
     * appear (clawt_alert_arrives_read()'s rule, on the daemon's list).
     */
    if (alerts_panel.showing) {
        if (clawt_ipc_payload_int(reply_payload(), "unread", 0) > 0)
            notice_verb(app, "notice.read", all_payload());

        refresh_alerts(app);
    }

    update_bell(app);
}

void
clawt_lrg_alerts_refresh(ClawtLrgApp *app)
{
    /*
     * Unscoped, always: the bell counts the whole fleet whatever the
     * drawer is narrowed to, and the narrowing is applied when drawing.
     */
    clawt_lrg_request(app, "notice.list", NULL, CLAWT_LRG_QUIET, on_notices,
                      NULL, NULL);
}

/* A notice verb, fire and forget: `notice.changed` redraws every client. */
static void
notice_verb(ClawtLrgApp *app, const gchar *kind, JsonNode *payload)
{
    clawt_lrg_request(app, kind, payload, CLAWT_LRG_QUIET, NULL, NULL, NULL);
}

static JsonNode *
all_payload(void)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "all");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

void
clawt_lrg_set_agent_muted(ClawtLrgApp *app, const gchar *agent_id,
                          gboolean muted)
{
    if (agent_id == NULL)
        return;

    notice_verb(app, muted ? "notice.mute" : "notice.unmute",
                clawt_ipc_payload_new("agent", agent_id, NULL));
}

/* ── Row menus ───────────────────────────────────────────────────── */

static void
on_grok_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "grok.remove",
                      clawt_ipc_payload_new("job", (const gchar *)user_data,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, NULL, NULL, NULL);
}

/*
 * Every row's verbs.  The target -- a room, a notice id, an agent, a job
 * -- rides as the action's string parameter, so there is no "current
 * row" for a rebuild to change under an open menu.
 */
static void
on_row_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));
    const gchar *target = (parameter != NULL)
        ? g_variant_get_string(parameter, NULL) : NULL;

    if (g_strcmp0(name, "chat-read") == 0) {
        if (clawt_lrg_unread_clear(app->unread, target))
            clawt_lrg_sidebar_refresh(app);

        clawt_lrg_mark_room_read(app, target, NULL);
    } else if (g_strcmp0(name, "read") == 0) {
        notice_verb(app, "notice.read",
                    clawt_ipc_payload_new("id", target, NULL));
    } else if (g_strcmp0(name, "dismiss") == 0) {
        notice_verb(app, "notice.dismiss",
                    clawt_ipc_payload_new("id", target, NULL));
    } else if (g_strcmp0(name, "mute") == 0) {
        clawt_lrg_set_agent_muted(app, target, TRUE);
    } else if (g_strcmp0(name, "open-agent") == 0) {
        clawt_lrg_select_agent(app, target);
    } else if (g_strcmp0(name, "open-room") == 0) {
        clawt_lrg_select_room(app, target);
    } else if (g_strcmp0(name, "decisions") == 0) {
        clawt_lrg_show_page(app, CLAWT_PAGE_DECISIONS);
    } else if (g_strcmp0(name, "grok") == 0) {
        clawt_lrg_show_page(app, CLAWT_PAGE_GROK);
    } else if (g_strcmp0(name, "grok-remove") == 0) {
        JsonArray *jobs = reply_array("grok");
        const gchar *question = NULL;
        guint i;

        /* in the daemon's words for that job's state, as the Grok page */
        for (i = 0; jobs != NULL && i < json_array_get_length(jobs); i++) {
            JsonObject *job = json_array_get_object_element(jobs, i);

            if (g_strcmp0(clawt_ipc_payload_string(job, "id"), target) == 0)
                question = clawt_ipc_payload_string(job, "remove_question");
        }

        clawt_lrg_confirm(app, "Remove this job?",
                          question != NULL ? question
                                           : "It goes from the Grok page.",
                          "Remove", TRUE, on_grok_remove_confirmed,
                          g_strdup(target), g_free);
    }

    clawt_lrg_app_wake(app);
}

/*
 * The one action group every row shares: string-parameter verbs and two
 * plain ones.  Built per row because lrg_widget_insert_action_group()
 * takes the group into the row it is inserted on.
 */
static void
attach_menu(ClawtLrgApp *app, LrgWidget *row, GMenu *menu)
{
    static const gchar *const targeted[] = {
        "chat-read", "read", "dismiss", "mute", "open-agent", "open-room",
        "grok-remove"
    };
    static const gchar *const plain[] = { "decisions", "grok" };
    g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(targeted); i++) {
        g_autoptr(GSimpleAction) action =
            g_simple_action_new(targeted[i], G_VARIANT_TYPE_STRING);

        g_signal_connect(action, "activate", G_CALLBACK(on_row_action), app);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    for (i = 0; i < G_N_ELEMENTS(plain); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(plain[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_row_action), app);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    }

    lrg_widget_insert_action_group(row, "notice", G_ACTION_GROUP(group));
    lrg_widget_set_context_menu(row, G_MENU_MODEL(menu));
}

static void
menu_add(GMenu *menu, const gchar *label, const gchar *action,
         const gchar *target)
{
    g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);

    if (target != NULL)
        g_menu_item_set_action_and_target_value(item, action,
                                                g_variant_new_string(target));
    else
        g_menu_item_set_detailed_action(item, action);

    g_menu_append_item(menu, item);
}

/* ── Rows ─────────────────────────────────────────────────────────── */

static void
on_scope_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *agent = g_object_get_data(G_OBJECT(button), "agent");

    /* the agent's name is the one control the scope needs */
    g_free(alerts_panel.scope_agent);
    alerts_panel.scope_agent = g_strdup(agent);
    refresh_alerts(app);
}

static void
on_scope_cleared(LrgButton *button, gpointer user_data)
{
    (void)button;

    g_clear_pointer(&alerts_panel.scope_agent, g_free);
    refresh_alerts(user_data);
}

static void
on_dismiss_clicked(LrgButton *button, gpointer user_data)
{
    notice_verb(user_data, "notice.dismiss",
                clawt_ipc_payload_new("id",
                                      g_object_get_data(G_OBJECT(button),
                                                        "id"),
                                      NULL));
}

/* A section's heading, drawn above its first row. */
static void
add_heading(const gchar *title)
{
    clawt_lrg_take(alerts_panel.list, clawt_lrg_label(title, "heading"));
}

/*
 * An alert.  Errors and notices get a card and a tone, an unread one a
 * "new" badge; the dismiss button is always there rather than revealed,
 * and the menu has it too, beside Mark read and Mute.
 */
static LrgWidget *
alert_row(ClawtLrgApp *app, JsonObject *notice, guint index)
{
    LrgWidget *box = clawt_lrg_vbox(4.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *meta = clawt_lrg_hbox(6.0f);
    LrgWidget *dismiss;
    const gchar *agent = clawt_ipc_payload_string(notice, "agent");
    gboolean error = g_strcmp0(clawt_ipc_payload_string(notice, "tier"),
                               "error") == 0;
    gboolean read = clawt_ipc_payload_boolean(notice, "read", TRUE);
    g_autofree gchar *id = g_strdup_printf(
        "%" G_GINT64_FORMAT, clawt_ipc_payload_int(notice, "id", 0));
    g_autofree gchar *when = clawt_lrg_ago(
        clawt_ipc_payload_int(notice, "created_at", 0) * G_USEC_PER_SEC);
    g_autoptr(GMenu) menu = g_menu_new();

    clawt_lrg_take(head, clawt_lrg_badge(error ? "error" : "notice",
                                         error ? "bad" : "info"));

    if (!read)
        clawt_lrg_take(head, clawt_lrg_badge("new", "warn"));

    clawt_lrg_take(box, head);
    clawt_lrg_take(box, clawt_lrg_paragraph(
        clawt_ipc_payload_string(notice, "title"), NULL));
    lrg_widget_add_class(box, "clawt-card");
    lrg_container_set_padding(LRG_CONTAINER(box), 12.0f);

    if (agent != NULL && *agent != '\0') {
        LrgWidget *who = clawt_lrg_button(agent, "flat",
                                          G_CALLBACK(on_scope_clicked), app);

        g_object_set_data_full(G_OBJECT(who), "agent", g_strdup(agent),
                               g_free);
        clawt_lrg_take(meta, who);
    }

    clawt_lrg_take(meta, clawt_lrg_label(when, "caption"));

    dismiss = clawt_lrg_button("Dismiss", "flat",
                               G_CALLBACK(on_dismiss_clicked), app);
    g_object_set_data_full(G_OBJECT(dismiss), "id", g_strdup(id), g_free);
    clawt_lrg_take(meta, dismiss);
    clawt_lrg_take(box, meta);

    if (!read)
        menu_add(menu, "Mark read", "notice.read", id);

    menu_add(menu, "Dismiss", "notice.dismiss", id);

    if (agent != NULL && !g_hash_table_contains(app->muted, agent)) {
        g_autofree gchar *label = clawt_agent_menu_mute_label(FALSE);

        menu_add(menu, label, "notice.mute", agent);
    }

    attach_menu(app, box, menu);
    clawt_lrg_appear(app, box, index);

    return box;
}

/* A plain row: a line and a caption, and a menu. */
static LrgWidget *
plain_row(ClawtLrgApp *app, const gchar *line, const gchar *caption,
          GMenu *menu, guint index)
{
    LrgWidget *row = clawt_lrg_row(line, caption);

    attach_menu(app, row, menu);
    clawt_lrg_appear(app, row, index);

    return row;
}

static guint
append_chats(ClawtLrgApp *app, guint shown)
{
    JsonArray *chats = reply_array("chats");
    guint count = 0;
    guint i;

    for (i = 0; chats != NULL && i < json_array_get_length(chats); i++) {
        JsonObject *chat = json_array_get_object_element(chats, i);
        const gchar *agent = clawt_ipc_payload_string(chat, "agent");
        const gchar *room = clawt_ipc_payload_string(chat, "room");
        gint64 unread = clawt_ipc_payload_int(chat, "unread", 0);
        g_autofree gchar *line = NULL;
        g_autofree gchar *read = NULL;
        g_autoptr(GMenu) menu = g_menu_new();

        if (room == NULL ||
            !clawt_lrg_notice_in_scope(agent, alerts_panel.scope_agent))
            continue;

        if (count == 0)
            add_heading("Chats");

        line = g_strdup_printf("%s · %" G_GINT64_FORMAT " unread",
                               agent != NULL ? agent
                                   : clawt_ipc_payload_string(chat, "name"),
                               unread);

        if (agent != NULL)
            menu_add(menu, "Open", "notice.open-agent", agent);
        else
            menu_add(menu, "Open", "notice.open-room", room);

        read = clawt_agent_menu_read_label((guint)unread);
        menu_add(menu, read, "notice.chat-read", room);

        if (agent != NULL) {
            g_autofree gchar *mute = clawt_agent_menu_mute_label(FALSE);

            menu_add(menu, mute, "notice.mute", agent);
        }

        clawt_lrg_take(alerts_panel.list,
                       plain_row(app, line, NULL, menu, shown + count));
        count++;
    }

    return count;
}

static guint
append_decisions(ClawtLrgApp *app, guint shown)
{
    JsonArray *decisions = reply_array("decisions");
    guint count = 0;
    guint i;

    for (i = 0; decisions != NULL && i < json_array_get_length(decisions);
         i++) {
        JsonObject *decision = json_array_get_object_element(decisions, i);
        const gchar *agent = clawt_ipc_payload_string(decision, "agent");
        g_autoptr(GMenu) menu = g_menu_new();

        if (!clawt_lrg_notice_in_scope(agent, alerts_panel.scope_agent))
            continue;

        if (count == 0)
            add_heading("Decisions waiting");

        menu_add(menu, "Open Decisions", "notice.decisions", NULL);
        clawt_lrg_take(alerts_panel.list,
                       plain_row(app,
                                 clawt_ipc_payload_string(decision,
                                                          "question"),
                                 agent, menu, shown + count));
        count++;
    }

    return count;
}

static guint
append_grok(ClawtLrgApp *app, guint shown)
{
    JsonArray *jobs = reply_array("grok");
    guint count = 0;
    guint i;

    for (i = 0; jobs != NULL && i < json_array_get_length(jobs); i++) {
        JsonObject *job = json_array_get_object_element(jobs, i);
        const gchar *agent = clawt_ipc_payload_string(job, "agent");
        const gchar *note = clawt_ipc_payload_string(job, "note");
        g_autofree gchar *line = NULL;
        g_autoptr(GMenu) menu = g_menu_new();

        if (!clawt_lrg_notice_in_scope(agent, alerts_panel.scope_agent))
            continue;

        if (count == 0)
            add_heading("Grok jobs needing attention");

        line = g_strdup_printf("%s · %s",
                               clawt_ipc_payload_string(job, "bot"),
                               clawt_ipc_payload_string(job, "state"));
        menu_add(menu, "Open Grok", "notice.grok", NULL);
        menu_add(menu, "Remove…", "notice.grok-remove",
                 clawt_ipc_payload_string(job, "id"));
        clawt_lrg_take(alerts_panel.list,
                       plain_row(app, line, note != NULL ? note : agent, menu,
                                 shown + count));
        count++;
    }

    return count;
}

static guint
append_alerts(ClawtLrgApp *app, guint shown)
{
    JsonArray *notices = reply_array("notices");
    guint count = 0;
    guint i;

    for (i = 0; notices != NULL && i < json_array_get_length(notices); i++) {
        JsonObject *notice = json_array_get_object_element(notices, i);

        if (!clawt_lrg_notice_in_scope(
                clawt_ipc_payload_string(notice, "agent"),
                alerts_panel.scope_agent))
            continue;

        if (count == 0)
            add_heading("Alerts");

        clawt_lrg_take(alerts_panel.list,
                       alert_row(app, notice, shown + count));
        count++;
    }

    return count;
}

static void
on_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *events;
    guint i;

    (void)app;
    (void)error;
    (void)user_data;

    if (alerts_panel.history == NULL)
        return;

    clawt_lrg_clear(alerts_panel.history);

    if (root == NULL || !json_object_has_member(root, "events"))
        return;

    events = json_object_get_array_member(root, "events");

    /* newest first, like the list above it */
    for (i = json_array_get_length(events); i > 0; i--) {
        JsonObject *one = json_array_get_object_element(events, i - 1);
        const gchar *kind = clawt_ipc_payload_string(one, "kind");
        const gchar *subject = clawt_ipc_payload_string(one, "subject");
        g_autofree gchar *text = g_strdup_printf(
            "%s · %s", (kind != NULL) ? kind : "?",
            (subject != NULL) ? subject : "the fleet");

        clawt_lrg_take(alerts_panel.history, clawt_lrg_label(text, "caption"));
    }
}

static void
on_history_toggled(LrgExpander *expander, gboolean expanded,
                   gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    (void)expander;

    if (!expanded)
        return;

    /* a round trip, so only when somebody asks for it */
    clawt_lrg_request(app, "event.list",
                      clawt_ipc_payload_new("limit", "50",
                                            alerts_panel.scope_agent != NULL
                                                ? "subject" : NULL,
                                            alerts_panel.scope_agent, NULL),
                      CLAWT_LRG_QUIET, on_history, NULL, NULL);
}

static void
refresh_alerts(ClawtLrgApp *app)
{
    guint shown = 0;

    if (alerts_panel.list == NULL)
        return;

    clawt_lrg_clear(alerts_panel.list);

    if (alerts_panel.scope_agent != NULL) {
        g_autofree gchar *label =
            g_strdup_printf("Only %s  ×", alerts_panel.scope_agent);

        clawt_lrg_take(alerts_panel.list,
                       clawt_lrg_button(label, "flat",
                                        G_CALLBACK(on_scope_cleared), app));
    }

    /* by how much each wants somebody, as GTK orders them */
    shown += append_chats(app, shown);
    shown += append_decisions(app, shown);
    shown += append_grok(app, shown);
    shown += append_alerts(app, shown);

    if (shown == 0)
        clawt_lrg_take(alerts_panel.list,
                       clawt_lrg_empty_state("Nothing has needed you.",
                                             NULL));

    /* the routine stream, from the daemon's own event log, on request */
    {
        LrgExpander *expander =
            lrg_expander_new("Earlier, from the daemon's log");

        alerts_panel.history = clawt_lrg_vbox(2.0f);
        lrg_expander_set_child(expander, alerts_panel.history);
        g_object_unref(alerts_panel.history);
        g_signal_connect(expander, "toggled", G_CALLBACK(on_history_toggled),
                         app);
        clawt_lrg_take(alerts_panel.list, LRG_WIDGET(expander));
    }

    clawt_lrg_app_wake(app);
}

/* "Mark all read": every alert, and every conversation. */
static void
on_read_all_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;

    notice_verb(user_data, "notice.read", all_payload());
    clawt_lrg_mark_all_read(user_data);
}

/* "Clear": every alert showing, or only the scoped agent's. */
static void
on_clear_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;

    notice_verb(user_data, "notice.clear",
                clawt_ipc_payload_new("agent", alerts_panel.scope_agent,
                                      NULL));
}

static void
on_close_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;

    clawt_lrg_alerts_toggle(user_data);
}

static void
on_bell_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;

    clawt_lrg_alerts_toggle(user_data);
}

LrgWidget *
clawt_lrg_alerts_bell(ClawtLrgApp *app)
{
    LrgWidget *bell = clawt_lrg_button("Alerts", "flat",
                                       G_CALLBACK(on_bell_clicked), app);

    alerts_panel.bell = bell;
    lrg_widget_set_valign(bell, LRG_ALIGN_CENTER);
    return bell;
}

LrgWidget *
build_alerts_panel(ClawtLrgApp *app)
{
    LrgWidget *drawer = clawt_lrg_vbox(10.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *verbs = clawt_lrg_hbox(8.0f);
    LrgWidget *title = clawt_lrg_label("Notifications", "heading");
    LrgScrollBox *scroll = lrg_scroll_box_new();

    lrg_widget_add_class(drawer, "clawt-chrome");
    lrg_container_set_padding(LRG_CONTAINER(drawer), 14.0f);
    lrg_widget_set_min_size(drawer, 340.0f * (gfloat)app->scale, 0.0f);
    lrg_widget_set_max_size(drawer, 340.0f * (gfloat)app->scale, -1.0f);

    lrg_widget_set_fill_weight(title, 1.0f);
    clawt_lrg_take(head, title);
    clawt_lrg_take(head, clawt_lrg_button("Close", "flat",
                                          G_CALLBACK(on_close_clicked), app));
    clawt_lrg_take(drawer, head);

    /* the list's two verbs, as GTK's panel has them */
    clawt_lrg_take(verbs, clawt_lrg_button("Mark all read", "flat",
                                           G_CALLBACK(on_read_all_clicked),
                                           app));
    clawt_lrg_take(verbs, clawt_lrg_button("Clear", "flat",
                                           G_CALLBACK(on_clear_clicked),
                                           app));
    clawt_lrg_take(drawer, verbs);

    alerts_panel.list = clawt_lrg_vbox(8.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, alerts_panel.list);
    g_object_unref(alerts_panel.list);
    lrg_widget_set_fill_weight(LRG_WIDGET(scroll), 1.0f);
    clawt_lrg_take(drawer, LRG_WIDGET(scroll));

    lrg_widget_set_visible(drawer, FALSE);
    lrg_widget_set_appear(drawer, LRG_APPEAR_FLY_LEFT, 0.0f);
    alerts_panel.drawer = drawer;

    refresh_alerts(app);
    return drawer;
}

void
clawt_lrg_alerts_toggle(ClawtLrgApp *app)
{
    if (alerts_panel.drawer == NULL)
        return;

    alerts_panel.showing = !alerts_panel.showing;
    lrg_widget_set_visible(alerts_panel.drawer, alerts_panel.showing);

    /*
     * Opening it is seeing the alerts -- in every client, since the read
     * is the daemon's.  A conversation is read by opening it, not this.
     */
    if (alerts_panel.showing) {
        if (clawt_ipc_payload_int(reply_payload(), "unread", 0) > 0)
            notice_verb(app, "notice.read", all_payload());

        lrg_widget_play_appear(alerts_panel.drawer);
        refresh_alerts(app);
    }

    update_bell(app);
}

void
clawt_lrg_alerts_reset(void)
{
    g_clear_pointer(&alerts_panel.reply, json_node_unref);
    g_clear_pointer(&alerts_panel.scope_agent, g_free);
    alerts_panel.drawer = NULL;
    alerts_panel.list = NULL;
    alerts_panel.bell = NULL;
    alerts_panel.history = NULL;
    alerts_panel.showing = FALSE;
}
