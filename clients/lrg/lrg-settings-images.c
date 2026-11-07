/*
 * lrg-settings-images.c - Settings: Cloud images
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The disk images a VM agent boots from: what is on this machine, what
 * is still arriving, and where to fetch another.  Mirrors the Cloud
 * images page of GTK's Settings (clawt-window.c, on_settings_activate).
 *
 * A download runs in the daemon and reports itself with `image.progress`
 * and `image.finished`.  Settings pages are not ClawtLrgPage entries,
 * so the shell does not hand them events; this page listens on the
 * client itself while it exists, and re-hooks whenever it is shown on a
 * client it has not heard from yet (a connection switch replaces the
 * client, and with it every handler on the old one).
 *
 * Progress moves the bar and the subtitle of the row it belongs to and
 * touches nothing else, for GTK's reason: a hundred rebuilds during one
 * download would fight whoever is using the dialog.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *bar;       /* LrgProgressBar */
    LrgWidget *subtitle;  /* LrgLabel */
} ImageRow;

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *list;
    LrgWidget   *catalog_slot;
    LrgWidget   *catalog_combo;   /* nullable until the catalogue arrives */
    LrgWidget   *url_entry;
    GHashTable  *rows;            /* image name -> ImageRow* */
    ClawtClient *watched;         /* weak: the client we listen on */
    gulong       event_id;
    guint        generation;
} Images;

static Images images;

static void refresh(ClawtLrgApp *app);

/* ── Rows ─────────────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *name;
} ImageAction;

static void
image_action_free(gpointer data)
{
    ImageAction *action = data;

    g_free(action->name);
    g_free(action);
}

static void
on_image_changed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    if (reply != NULL)
        refresh(app);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    ImageAction *action = user_data;

    clawt_lrg_request(app, "image.vm_remove",
                      clawt_ipc_payload_new("name", action->name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_image_changed, NULL, NULL);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    ImageAction *action = user_data;
    ImageAction *copy = g_new0(ImageAction, 1);
    g_autofree gchar *title =
        g_strdup_printf("Delete %s?", action->name);

    copy->name = g_strdup(action->name);

    /*
     * Asked first, which GTK does not: an overlay keeps a VM's writes
     * but pins this file as its base, so deleting it strands every agent
     * built on it until its overlay is rebuilt from another.
     */
    clawt_lrg_confirm(action->app, title,
                      "Agents whose VM was built on this image need a "
                      "rebuild before they boot again.",
                      "Delete", TRUE, on_remove_confirmed, copy,
                      image_action_free);
}

static void
on_cancel_clicked(LrgButton *button, gpointer user_data)
{
    ImageAction *action = user_data;

    clawt_lrg_request(action->app, "image.vm_cancel",
                      clawt_ipc_payload_new("name", action->name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_image_changed, NULL, NULL);
}

static void
image_row_free(gpointer data)
{
    g_free(data);
}

static LrgWidget *
image_row(ClawtLrgApp *app, JsonObject *image)
{
    const gchar *name = clawt_ipc_payload_string(image, "name");
    gboolean downloading = clawt_lrg_settings_image_downloading(image);
    g_autofree gchar *subtitle = clawt_lrg_settings_image_subtitle(image);
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgWidget *text = clawt_lrg_vbox(2.0f);
    LrgWidget *subtitle_label = clawt_lrg_label(subtitle, "muted");
    ImageAction *action = g_new0(ImageAction, 1);
    LrgWidget *button;

    if (name == NULL)
        name = "?";

    action->app = app;
    action->name = g_strdup(name);

    lrg_widget_add_class(row, "clawt-row");
    lrg_container_set_padding(LRG_CONTAINER(row), 10.0f);
    lrg_widget_set_fill_weight(text, 1.0f);
    clawt_lrg_take(text, clawt_lrg_label(name, NULL));
    clawt_lrg_add(text, subtitle_label);
    clawt_lrg_take(row, text);

    if (downloading) {
        LrgProgressBar *bar = lrg_progress_bar_new();
        ImageRow *entry = g_new0(ImageRow, 1);
        gdouble fraction = clawt_lrg_settings_progress_fraction(
            clawt_ipc_payload_int(image, "bytes", 0),
            clawt_ipc_payload_int(image, "total", 0));

        lrg_progress_bar_set_max(bar, 1.0);

        if (fraction < 0.0)
            lrg_progress_bar_set_indeterminate(bar, TRUE);
        else
            lrg_progress_bar_set_value(bar, fraction);

        lrg_widget_set_min_size(LRG_WIDGET(bar), 180.0f, 8.0f);
        lrg_widget_set_valign(LRG_WIDGET(bar), LRG_ALIGN_CENTER);
        clawt_lrg_add(row, LRG_WIDGET(bar));

        /* weak: the row owns both; a rebuild empties the table first */
        entry->bar = LRG_WIDGET(bar);
        entry->subtitle = subtitle_label;
        g_hash_table_replace(images.rows, g_strdup(name), entry);
        g_object_unref(bar);

        button = clawt_lrg_button("Stop", "flat",
                                  G_CALLBACK(on_cancel_clicked), action);
    } else {
        button = clawt_lrg_button("Delete", "flat",
                                  G_CALLBACK(on_remove_clicked), action);
    }

    g_object_set_data_full(G_OBJECT(button), "clawt-image-action", action,
                           image_action_free);
    lrg_widget_set_valign(button, LRG_ALIGN_CENTER);
    clawt_lrg_take(row, button);
    g_object_unref(subtitle_label);

    return row;
}

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    guint i;

    /* an older refresh landing after a newer one would undo it */
    if (GPOINTER_TO_UINT(user_data) != images.generation || reply == NULL)
        return;

    g_hash_table_remove_all(images.rows);
    clawt_lrg_clear(images.list);

    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "images");

    if (list == NULL || json_array_get_length(list) == 0) {
        clawt_lrg_take(images.list, clawt_lrg_row(
            "No images yet",
            "A VM needs one. Fedora is the safe pick; anything with "
            "cloud-init works."));
        clawt_lrg_app_wake(app);
        return;
    }

    for (i = 0; i < json_array_get_length(list); i++) {
        LrgWidget *row = image_row(app,
                                   json_array_get_object_element(list, i));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(images.list, row);
    }

    clawt_lrg_app_wake(app);
}

/* ── Events ───────────────────────────────────────────────────────── */

static void
on_client_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *kind = clawt_event_get_kind(event);
    const gchar *name = clawt_event_get_subject(event);

    if (g_strcmp0(kind, "image.progress") == 0) {
        ImageRow *row = (name != NULL)
            ? g_hash_table_lookup(images.rows, name) : NULL;
        gint64 done = clawt_event_get_detail_int(event, "done");
        gint64 total = clawt_event_get_detail_int(event, "total");
        gdouble fraction;
        g_autofree gchar *text = NULL;

        /*
         * No row means a download begun somewhere else -- the CLI, or
         * another window -- so the list is read once to give it one, and
         * every later event finds it.
         */
        if (row == NULL) {
            refresh(app);
            return;
        }

        fraction = clawt_lrg_settings_progress_fraction(done, total);

        if (fraction >= 0.0) {
            lrg_progress_bar_set_indeterminate(LRG_PROGRESS_BAR(row->bar),
                                               FALSE);
            lrg_progress_bar_set_value(LRG_PROGRESS_BAR(row->bar), fraction);
        }

        text = clawt_lrg_settings_progress_text(done, total);
        lrg_label_set_text(LRG_LABEL(row->subtitle), text);
        clawt_lrg_app_wake(app);
        return;
    }

    if (g_strcmp0(kind, "image.finished") == 0) {
        g_autofree gchar *failure = clawt_lrg_settings_image_failed_text(
            name, clawt_event_get_detail(event, "error"));

        /*
         * A toast with a way back to the page: GTK files this in its
         * alerts panel, which this client's shell owns and settings
         * pages cannot reach.  It is only said while this page has been
         * opened at least once, which is when anybody started one here.
         */
        if (failure != NULL)
            clawt_lrg_toast(app, failure);

        refresh(app);
    }
}

/*
 * Listen on whichever client the app has now.  A switch replaced the
 * client and the old one took our handler with it, so this is asked
 * every time the page is shown rather than once.
 */
static void
watch_client(ClawtLrgApp *app)
{
    if (images.watched == app->client)
        return;

    if (images.watched != NULL && images.event_id != 0)
        g_signal_handler_disconnect(images.watched, images.event_id);

    images.event_id = 0;
    g_clear_weak_pointer(&images.watched);

    if (app->client == NULL)
        return;

    images.event_id = g_signal_connect(app->client, "event",
                                       G_CALLBACK(on_client_event), app);
    g_set_weak_pointer(&images.watched, app->client);
}

/* ── Fetching ─────────────────────────────────────────────────────── */

static void
start_download(ClawtLrgApp *app, const gchar *url)
{
    if (url == NULL || *url == '\0') {
        clawt_lrg_toast(app, "Give it a URL to fetch.");
        return;
    }

    clawt_lrg_request(app, "image.vm_download",
                      clawt_ipc_payload_new("url", url, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_image_changed, NULL, NULL);
}

static void
on_download_catalog(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (images.catalog_combo == NULL)
        return;

    /*
     * The catalogue id rather than the URL behind it, so the daemon
     * resolves the newest compose itself; a URL resolved here would pin
     * whatever this client happened to see.
     */
    start_download(app, clawt_lrg_combo_id(images.catalog_combo));
}

static void
on_download_url(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *url =
        g_strdup(clawt_lrg_entry_text(images.url_entry));

    g_strstrip(url);
    start_download(app, url);
    lrg_text_input_set_text(LRG_TEXT_INPUT(images.url_entry), "");
}

static void
on_catalog(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonArray *sources = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "sources");
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    if (reply == NULL)
        return;

    for (i = 0; sources != NULL && i < json_array_get_length(sources); i++) {
        JsonObject *source = json_array_get_object_element(sources, i);
        const gchar *id = clawt_ipc_payload_string(source, "id");
        const gchar *name = clawt_ipc_payload_string(source, "name");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, (gpointer)id);
        g_ptr_array_add(labels, (gpointer)((name != NULL) ? name : id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_clear(images.catalog_slot);
    images.catalog_combo = clawt_lrg_combo(
        (const gchar *const *)ids->pdata, (const gchar *const *)labels->pdata,
        (ids->len > 1) ? g_ptr_array_index(ids, 0) : NULL);
    lrg_widget_set_fill_weight(images.catalog_combo, 1.0f);
    clawt_lrg_take(images.catalog_slot, images.catalog_combo);
    clawt_lrg_app_wake(app);
}

/* ── The page ─────────────────────────────────────────────────────── */

/* A card with a heading, a paragraph and a body to fill. */
static LrgWidget *
group_card(const gchar *title, const gchar *description,
           LrgWidget **out_body)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *body = clawt_lrg_vbox(6.0f);

    clawt_lrg_take(box, clawt_lrg_label(title, "heading"));

    if (description != NULL)
        clawt_lrg_take(box, clawt_lrg_paragraph(description, "muted"));

    clawt_lrg_add(box, body);
    *out_body = body;
    g_object_unref(body);

    return clawt_lrg_card(box);
}

static void
refresh(ClawtLrgApp *app)
{
    if (images.list == NULL)
        return;

    watch_client(app);
    images.generation++;
    clawt_lrg_request(app, "image.vm_list", NULL, CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(images.generation), NULL);

    /* the catalogue once it answers; asked again only until it does */
    if (images.catalog_combo == NULL)
        clawt_lrg_request(app, "image.vm_catalog", NULL, CLAWT_LRG_QUIET,
                          on_catalog, NULL, NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Cloud images", "The disks VM agents boot from");
    LrgWidget *body = clawt_lrg_page_body(page);
    LrgWidget *fetch_body;
    LrgWidget *catalog_row = clawt_lrg_hbox(8.0f);
    LrgWidget *url_row = clawt_lrg_hbox(8.0f);

    images.app = app;
    images.page = page;
    images.rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        image_row_free);

    clawt_lrg_take(body, group_card(
        "On this machine",
        "One copy serves every agent: a VM writes to an overlay backed by "
        "the image, never to the image itself.", &images.list));

    clawt_lrg_take(body, group_card(
        "Fetch another",
        "These are suggestions, not a restriction. Any https URL to a "
        "qcow2 works, and an image with cloud-init needs no preparing.",
        &fetch_body));

    /* the suggested sources, filled when the catalogue answers */
    images.catalog_slot = clawt_lrg_hbox(0.0f);
    lrg_widget_set_fill_weight(images.catalog_slot, 1.0f);
    clawt_lrg_take(images.catalog_slot,
                   clawt_lrg_label("Asking for suggestions…", "muted"));
    clawt_lrg_add(catalog_row, images.catalog_slot);
    g_object_unref(images.catalog_slot);
    clawt_lrg_take(catalog_row,
                   clawt_lrg_button("Download", "primary",
                                    G_CALLBACK(on_download_catalog), app));

    {
        LrgWidget *form = clawt_lrg_form_new();

        clawt_lrg_form_add(form, "Suggested", catalog_row, NULL);

        images.url_entry = clawt_lrg_entry(NULL, "https://…/image.qcow2");
        lrg_widget_set_fill_weight(images.url_entry, 1.0f);
        clawt_lrg_add(url_row, images.url_entry);
        g_object_unref(images.url_entry);
        clawt_lrg_take(url_row,
                       clawt_lrg_button("Download", NULL,
                                        G_CALLBACK(on_download_url), app));
        clawt_lrg_form_add(form, "Or a URL of your own", url_row, NULL);

        clawt_lrg_take(fetch_body, form);
    }

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "images", "Cloud images", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_images_page(void)
{
    return &page_impl;
}
