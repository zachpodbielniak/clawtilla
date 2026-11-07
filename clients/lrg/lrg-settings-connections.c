/*
 * lrg-settings-connections.c - Settings: Connections
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Which daemon this window is looking at, the others it could look at,
 * and whether each is up.  Mirrors the connection menu and the
 * Connections dialog of clients/gtk/clawt-window.c, as one page: the
 * current daemon and its version, the saved profiles with their
 * reachability, adding and removing one, and switching.
 *
 * The profiles live in the *client's* config (connections.yaml, 0600),
 * not in clawtilla.yaml: a laptop reaching a workstation may have no
 * fleet of its own.  Local is synthesised rather than stored -- it is
 * where the client points with no configuration at all -- and carries
 * whatever socket this process started on, so a second local daemon
 * stays reachable after a trip to a remote one.
 *
 * Reachability is asked with clawt_connection_probe(), which blocks for
 * as long as the far end takes (a sleeping laptop fails only when its
 * route times out).  So each probe runs on a worker thread through
 * g_task_run_in_thread(); the probe brings its own GMainContext and
 * never touches ours, and the GTask -- created with our context pushed
 * -- delivers the answer back on the loop that draws.  Nothing iterates
 * a context on the worker, which is the mistake clawt_connection_probe()
 * itself was once fixed for.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    ClawtLrgApp *app;
    GPtrArray   *connections;   /* ClawtConnection*, Local first */
    GHashTable  *status;        /* name -> ClawtConnectionStatus* */
    gchar       *local_socket;
    gboolean     local_known;
    guint        generation;    /* bumped on every reload */
    guint        asked;         /* bumped on every control.status */

    LrgWidget   *current;
    LrgWidget   *list;
    LrgWidget   *name;
    LrgWidget   *host;
    LrgWidget   *port;
    LrgWidget   *token;
    LrgWidget   *tls;
    LrgWidget   *insecure;
} Connections;

static Connections links;

static void rebuild_list(ClawtLrgApp *app);

/* ── Probing, off the loop ────────────────────────────────────────── */

typedef enum {
    PROBE_SHOW,         /* draw the answer */
    PROBE_SWITCH        /* and switch to it if it answered */
} ProbePurpose;

typedef struct {
    ClawtLrgApp     *app;
    gchar           *name;
    ClawtConnection *connection;   /* our own copy: the list is rebuilt */
    guint            generation;
    ProbePurpose     purpose;
} ProbeJob;

static void
probe_job_free(gpointer data)
{
    ProbeJob *job = data;

    g_clear_pointer(&job->connection, clawt_connection_free);
    g_free(job->name);
    g_free(job);
}

static void
probe_worker(GTask *task, gpointer source, gpointer data,
             GCancellable *cancellable)
{
    ProbeJob *job = data;

    g_task_return_pointer(task, clawt_connection_probe(job->connection),
                          (GDestroyNotify)clawt_connection_status_free);
}

static void ask_status(ClawtLrgApp *app);

static void
switch_to(ClawtLrgApp *app, ClawtConnection *connection)
{
    g_autofree gchar *where = clawt_connection_describe(connection);
    g_autofree gchar *text = g_strdup_printf(
        "Connected to %s (%s).", clawt_connection_get_name(connection),
        where);

    clawt_lrg_app_switch_connection(app, connection);
    clawt_lrg_toast(app, text);
    rebuild_list(app);

    /*
     * The version belongs to the daemon, not to the window: a version
     * kept from the machine somebody just left is worse than none.
     */
    ask_status(app);
}

static void
on_probed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ProbeJob *job = g_task_get_task_data(G_TASK(result));
    ClawtConnectionStatus *status =
        g_task_propagate_pointer(G_TASK(result), NULL);
    ClawtLrgApp *app = job->app;

    if (status == NULL)
        return;

    /*
     * A switch asked first, the way GTK connects the new client before
     * letting go of the old: a remote that is down or a token that is
     * wrong is the ordinary case, and dropping the working connection
     * first would leave the window connected to nowhere over a typo.
     */
    if (job->purpose == PROBE_SWITCH) {
        if (status->reach == CLAWT_REACH_REACHABLE) {
            g_hash_table_replace(links.status, g_strdup(job->name), status);
            switch_to(app, job->connection);
            return;
        }

        {
            g_autofree gchar *where =
                clawt_connection_describe(job->connection);
            g_autofree gchar *text = g_strdup_printf(
                "%s (%s): %s — %s", job->name, where,
                clawt_reachability_word(status->reach),
                (status->detail != NULL) ? status->detail : "no answer");

            clawt_lrg_toast(app, text);
        }
    }

    /* an answer about a list that has since been reloaded is still true */
    g_hash_table_replace(links.status, g_strdup(job->name), status);

    if (job->generation == links.generation)
        rebuild_list(app);
}

static void
probe(ClawtLrgApp *app, ClawtConnection *connection, ProbePurpose purpose)
{
    ProbeJob *job = g_new0(ProbeJob, 1);
    g_autoptr(GTask) task = NULL;

    job->app = app;
    job->name = g_strdup(clawt_connection_get_name(connection));
    job->connection = clawt_connection_copy(connection);
    job->generation = links.generation;
    job->purpose = purpose;

    /*
     * Pushed so the GTask returns on our context: g_task_new() takes the
     * thread-default, and this process has no other loop running.
     */
    g_main_context_push_thread_default(app->context);
    task = g_task_new(NULL, NULL, on_probed, NULL);
    g_main_context_pop_thread_default(app->context);

    g_task_set_task_data(task, job, probe_job_free);
    g_task_run_in_thread(task, probe_worker);
}

/*
 * Every saved connection, once each time the page is shown: a probe
 * nobody is looking at is a network call nobody asked for, and the
 * answer only matters while somebody is deciding where to go.
 */
static void
probe_all(ClawtLrgApp *app)
{
    guint i;

    for (i = 0; i < links.connections->len; i++)
        probe(app, g_ptr_array_index(links.connections, i), PROBE_SHOW);
}

static void
on_probe_all_clicked(LrgButton *button, gpointer user_data)
{
    probe_all(user_data);
}

/* ── The saved list ───────────────────────────────────────────────── */

static gboolean
is_current(ClawtLrgApp *app, ClawtConnection *connection)
{
    return app->connection != NULL &&
           g_strcmp0(clawt_connection_get_name(app->connection),
                     clawt_connection_get_name(connection)) == 0;
}

static void
reload_connections(ClawtLrgApp *app)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) saved = clawt_connection_list_load(NULL, &error);
    guint i;

    /*
     * The socket this process started on, remembered the first time it
     * is known: after a switch to a remote, app->connection no longer
     * says, and Local must still lead back to it.
     */
    if (!links.local_known && app->connection != NULL &&
        clawt_connection_is_local(app->connection)) {
        links.local_socket = g_strdup(
            clawt_connection_get_socket_path(app->connection));
        links.local_known = TRUE;
    }

    /* a file that cannot be read is said once, not as a missing list */
    if (saved == NULL) {
        clawt_lrg_toast(app, error->message);
        saved = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_connection_free);
    }

    g_ptr_array_set_size(links.connections, 0);
    g_ptr_array_add(links.connections,
                    clawt_connection_new_local("Local", links.local_socket));

    for (i = 0; i < saved->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(saved, i);

        /* the synthesised Local covers a file's own local entry */
        if (clawt_connection_is_local(connection))
            continue;

        g_ptr_array_add(links.connections, clawt_connection_copy(connection));
    }

    links.generation++;
}

/*
 * The remotes only: Local is synthesised at load, and writing it back
 * would add a line the next load skips.
 */
static gboolean
save_connections(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    g_autoptr(GError) error = NULL;
    guint i;

    for (i = 0; i < links.connections->len; i++) {
        ClawtConnection *connection =
            g_ptr_array_index(links.connections, i);

        if (!clawt_connection_is_local(connection))
            g_ptr_array_add(remotes, clawt_connection_copy(connection));
    }

    if (!clawt_connection_list_save(NULL, remotes, &error)) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    return TRUE;
}

static void
on_switch_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    ClawtConnection *connection =
        g_object_get_data(G_OBJECT(button), "clawt-connection");

    if (connection == NULL)
        return;

    /* already there: reconnecting would be a visible no-op */
    if (is_current(app, connection) && clawt_client_is_connected(app->client)) {
        clawt_lrg_toast(app, "Already connected there.");
        return;
    }

    clawt_lrg_toast(app, "Checking it answers first…");
    probe(app, connection, PROBE_SWITCH);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(button),
                                          "clawt-connection-name");
    guint i;

    for (i = 0; name != NULL && i < links.connections->len; i++) {
        ClawtConnection *connection =
            g_ptr_array_index(links.connections, i);

        if (clawt_connection_is_local(connection) ||
            g_strcmp0(clawt_connection_get_name(connection), name) != 0)
            continue;

        g_ptr_array_remove_index(links.connections, i);
        break;
    }

    if (!save_connections(app))
        return;

    clawt_lrg_toast(app, "Removed.");
    reload_connections(app);
    rebuild_list(app);
}

static void
rebuild_list(ClawtLrgApp *app)
{
    guint i;

    if (links.list == NULL)
        return;

    clawt_lrg_clear(links.list);

    for (i = 0; i < links.connections->len; i++) {
        ClawtConnection *connection =
            g_ptr_array_index(links.connections, i);
        const gchar *name = clawt_connection_get_name(connection);
        g_autofree gchar *where = clawt_connection_describe(connection);
        ClawtConnectionStatus *status =
            g_hash_table_lookup(links.status, name);
        ClawtReachability reach =
            (status != NULL) ? status->reach : CLAWT_REACH_UNKNOWN;
        g_autofree gchar *detail = clawt_lrg_settings_reach_detail(status);
        g_autofree gchar *subtitle = (detail != NULL)
            ? g_strdup_printf("%s \302\267 %s", where, detail)
            : g_strdup(where);
        LrgWidget *row = clawt_lrg_row(name, subtitle);
        gboolean current = is_current(app, connection);

        /*
         * Refused and unreachable are different words for a reason: one
         * is a credential problem and the other a network one, and
         * sending somebody to check the wrong one costs far more than
         * the probe did.
         */
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
            clawt_reachability_word(reach),
            clawt_lrg_settings_reach_tone(reach)));

        if (current) {
            clawt_lrg_row_add_prefix(row, clawt_lrg_badge("current",
                                                          "info"));
            lrg_widget_add_class(row, "clawt-row-selected");
        } else {
            LrgWidget *go = clawt_lrg_button("Switch", "primary",
                                             G_CALLBACK(on_switch_clicked),
                                             app);

            /* a copy: the list is rebuilt under a click that outlives it */
            g_object_set_data_full(G_OBJECT(go), "clawt-connection",
                                   clawt_connection_copy(connection),
                                   (GDestroyNotify)clawt_connection_free);
            clawt_lrg_row_add_suffix(row, go);
        }

        if (!clawt_connection_is_local(connection)) {
            LrgWidget *remove = clawt_lrg_button(
                "Remove", "flat", G_CALLBACK(on_remove_clicked), app);

            g_object_set_data_full(G_OBJECT(remove), "clawt-connection-name",
                                   g_strdup(name), g_free);
            clawt_lrg_row_add_suffix(row, remove);
        }

        clawt_lrg_take(links.list, row);
    }

    if (links.connections->len < 2)
        clawt_lrg_take(links.list, clawt_lrg_row(
            "No remote daemons yet",
            "clawtillad listens on its tailnet address by default, so a "
            "machine on your tailnet usually needs nothing set up."));

    clawt_lrg_app_wake(app);
}

/* ── The current daemon ───────────────────────────────────────────── */

static void
on_status(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    g_autofree gchar *update = NULL;
    g_autofree gchar *version = NULL;
    g_autofree gchar *mismatch = NULL;
    g_autofree gchar *where = NULL;
    g_autofree gchar *line = NULL;
    gboolean connected = clawt_client_is_connected(app->client);

    if (GPOINTER_TO_UINT(user_data) != links.asked)
        return;

    version = clawt_lrg_settings_daemon_version(reply, &update);
    mismatch = clawt_version_mismatch_text(version);
    where = (app->connection != NULL)
        ? clawt_connection_describe(app->connection) : g_strdup("?");

    clawt_lrg_clear(links.current);
    clawt_lrg_take(links.current, clawt_lrg_label(
        (app->connection != NULL)
            ? clawt_connection_get_name(app->connection) : "Local",
        "heading"));

    line = g_strdup_printf("%s \302\267 %s", where,
                           connected
                               ? ((version != NULL) ? version
                                                    : "version unknown")
                               : "not connected");
    clawt_lrg_take(links.current, clawt_lrg_paragraph(line, "muted"));

    /*
     * Said here plainly, rather than discovered as a frame kind refused
     * in whichever feature somebody opened next.  Which way round it is
     * matters: the remedies are on different machines.
     */
    if (mismatch != NULL) {
        LrgWidget *note = clawt_lrg_row(mismatch, NULL);

        clawt_lrg_row_add_prefix(note, clawt_lrg_badge("version", "warn"));
        clawt_lrg_take(links.current, note);
    }

    if (update != NULL) {
        g_autofree gchar *text = g_strdup_printf(
            "clawtilla %s is available.", update);
        LrgWidget *note = clawt_lrg_row(text, NULL);

        clawt_lrg_row_add_prefix(note, clawt_lrg_badge("update", "info"));
        clawt_lrg_take(links.current, note);
    }

    clawt_lrg_app_wake(app);
}

/* The version belongs to the daemon, so it is asked every time. */
static void
ask_status(ClawtLrgApp *app)
{
    links.asked++;
    clawt_lrg_request(app, "control.status", NULL, CLAWT_LRG_QUIET,
                      on_status, GUINT_TO_POINTER(links.asked), NULL);
}

/* ── Adding ───────────────────────────────────────────────────────── */

static void
on_save_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GError) error = NULL;
    ClawtConnection *connection;

    connection = clawt_lrg_settings_connection_from_form(
        links.connections, clawt_lrg_entry_text(links.name),
        clawt_lrg_entry_text(links.host), clawt_lrg_entry_text(links.port),
        clawt_lrg_entry_text(links.token),
        clawt_lrg_switch_active(links.tls),
        clawt_lrg_switch_active(links.insecure), &error);

    if (connection == NULL) {
        clawt_lrg_toast(app, error->message);
        return;
    }

    g_ptr_array_add(links.connections, connection);

    if (!save_connections(app)) {
        g_ptr_array_remove(links.connections, connection);
        return;
    }

    /* the token leaves the widget as soon as it is saved */
    lrg_text_input_set_text(LRG_TEXT_INPUT(links.name), "");
    lrg_text_input_set_text(LRG_TEXT_INPUT(links.host), "");
    lrg_text_input_set_text(LRG_TEXT_INPUT(links.token), "");

    clawt_lrg_toast(app, "Saved.");
    reload_connections(app);
    rebuild_list(app);
    probe_all(app);
}

static LrgWidget *
build_add_card(ClawtLrgApp *app)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *form = clawt_lrg_form_new();
    g_autofree gchar *port = g_strdup_printf("%d", CLAWT_DEFAULT_TCP_PORT);
    LrgWidget *save;

    clawt_lrg_take(box, clawt_lrg_label("Add a daemon", "heading"));
    clawt_lrg_take(box, clawt_lrg_paragraph(
        "The address the daemon reports at startup, and the token from "
        "`clawtilla daemon token` on that machine.", "muted"));

    links.name = clawt_lrg_entry(NULL, "workstation");
    links.host = clawt_lrg_entry(NULL, "100.64.0.7 or a name");
    links.port = clawt_lrg_entry(port, NULL);
    links.token = clawt_lrg_entry(NULL, "Token");
    lrg_text_input_set_password_mode(LRG_TEXT_INPUT(links.token), TRUE);
    links.tls = clawt_lrg_switch("TLS", FALSE);
    links.insecure = clawt_lrg_switch("Accept an unknown certificate", FALSE);

    clawt_lrg_form_add(form, "Name", links.name, NULL);
    clawt_lrg_form_add(form, "Host", links.host, NULL);
    clawt_lrg_form_add(form, "Port", links.port, NULL);
    clawt_lrg_form_add(form, "Token", links.token, NULL);
    clawt_lrg_form_add(form, NULL, links.tls,
                       "Only if that daemon has a certificate. Without one "
                       "the token crosses the network in the clear -- inside "
                       "a tailnet, WireGuard is already encrypting it.");
    clawt_lrg_form_add(form, NULL, links.insecure,
                       "For a self-signed certificate on a machine you "
                       "control. It turns off the check that would notice "
                       "somebody else answering.");

    save = clawt_lrg_button("Save", "primary", G_CALLBACK(on_save_clicked),
                            app);
    lrg_widget_set_halign(save, LRG_ALIGN_START);
    clawt_lrg_take(form, save);
    clawt_lrg_take(box, form);

    return clawt_lrg_card(box);
}

/* ── The page ─────────────────────────────────────────────────────── */

static void
refresh(ClawtLrgApp *app)
{
    if (links.list == NULL)
        return;

    reload_connections(app);
    rebuild_list(app);

    ask_status(app);
    probe_all(app);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Connections", "Which daemon this window is looking at");
    LrgWidget *body = clawt_lrg_page_body(page);
    LrgWidget *saved = clawt_lrg_vbox(10.0f);

    links.app = app;
    links.connections = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    links.status = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)clawt_connection_status_free);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("Check them all", "flat",
                                    G_CALLBACK(on_probe_all_clicked), app));

    /*
     * Loud about where it is pointed: every destructive verb in the
     * client acts on whichever daemon is connected, and a person who has
     * forgotten they are on another machine finds out afterwards.
     */
    links.current = clawt_lrg_vbox(6.0f);
    clawt_lrg_take(body, clawt_lrg_card(g_object_ref(links.current)));
    g_object_unref(links.current);

    clawt_lrg_take(saved, clawt_lrg_label("Saved", "heading"));
    links.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(saved, links.list);
    g_object_unref(links.list);
    clawt_lrg_take(body, clawt_lrg_card(saved));

    clawt_lrg_take(body, build_add_card(app));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "connections", "Connections", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_connections_page(void)
{
    return &page_impl;
}
