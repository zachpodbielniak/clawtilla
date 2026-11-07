/*
 * lrg-util.c - Avatars for clawtilla-lrg
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An avatar is a circle in the agent's colour with its initials, and the
 * agent's picture over that when it has one.  The picture is fetched once
 * per agent, decoded at CLAWT_LRG_AVATAR_DECODE_SIZE and shared by every
 * avatar of that agent on screen -- a sidebar, a header and a transcript
 * full of one agent's messages are one texture, not a fetch each.
 *
 * Fetching is async and quiet: the common answer for an agent with no
 * picture is NOT_FOUND, and toasting that for every agent on every redraw
 * would bury the window (the GTK client hit exactly this).  Until the
 * picture arrives, the initials show; when it does, every avatar of that
 * agent already on screen gets it.
 */

#include "clawt-lrg.h"

#include <glib/gstdio.h>
#include <graylib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GrlTexture *texture;   /* nullable: none, or not loaded yet */
    gboolean    asked;     /* a fetch is in flight or has answered */
    GPtrArray  *images;    /* weak LrgImage pointers waiting for it */
} AvatarEntry;

static GHashTable *avatars = NULL; /* agent id -> AvatarEntry */

static void
weak_image_gone(gpointer data, GObject *where_the_object_was)
{
    GPtrArray *images = data;

    g_ptr_array_remove(images, where_the_object_was);
}

static void
avatar_entry_free(gpointer data)
{
    AvatarEntry *entry = data;
    guint i;

    for (i = 0; i < entry->images->len; i++)
        g_object_weak_unref(g_ptr_array_index(entry->images, i),
                            weak_image_gone, entry->images);

    g_ptr_array_unref(entry->images);
    g_clear_object(&entry->texture);
    g_free(entry);
}

static AvatarEntry *
avatar_entry(const gchar *agent_id)
{
    AvatarEntry *entry;

    if (avatars == NULL)
        avatars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        avatar_entry_free);

    entry = g_hash_table_lookup(avatars, agent_id);

    if (entry == NULL) {
        entry = g_new0(AvatarEntry, 1);
        entry->images = g_ptr_array_new();
        g_hash_table_insert(avatars, g_strdup(agent_id), entry);
    }

    return entry;
}

/*
 * Decodes image bytes to a texture at the decode size.
 *
 * Through a temporary file because graylib loads images by filename and
 * picks the decoder from its extension; the extension comes from the
 * bytes' own magic number, never from anything the daemon claimed.
 */
static GrlTexture *
decode_texture(const guchar *data, gsize length)
{
    const gchar *mime = clawt_avatar_sniff_mime_type(data, length);
    g_autofree gchar *template = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GrlImage) image = NULL;
    GrlTexture *texture = NULL;
    gint fd;

    if (mime == NULL)
        return NULL;

    template = clawt_lrg_image_template("clawtilla-avatar", mime);
    if (template == NULL)
        return NULL;
    fd = g_file_open_tmp(template, &path, &error);

    if (fd < 0) {
        g_debug("avatar: %s", error->message);
        return NULL;
    }

    close(fd);

    if (g_file_set_contents(path, (const gchar *)data, (gssize)length,
                            &error)) {
        image = grl_image_new_from_file(path);

        if (image != NULL) {
            grl_image_resize(image, CLAWT_LRG_AVATAR_DECODE_SIZE,
                             CLAWT_LRG_AVATAR_DECODE_SIZE);
            texture = grl_texture_new_from_image(image);
        }
    }

    g_unlink(path);
    return texture;
}

typedef struct {
    gchar *agent_id;
} AvatarFetch;

static void
avatar_fetch_free(gpointer data)
{
    AvatarFetch *fetch = data;

    g_free(fetch->agent_id);
    g_free(fetch);
}

static void
on_avatar(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    AvatarFetch *fetch = user_data;
    AvatarEntry *entry = avatar_entry(fetch->agent_id);
    const gchar *base64;
    g_autofree guchar *raw = NULL;
    gsize length = 0;
    guint i;

    base64 = clawt_ipc_payload_string(clawt_ipc_reply_object(reply),
                                      "base64");

    if (base64 == NULL)
        return;

    raw = g_base64_decode(base64, &length);
    g_clear_object(&entry->texture);
    entry->texture = decode_texture(raw, length);

    if (entry->texture == NULL)
        return;

    for (i = 0; i < entry->images->len; i++) {
        LrgImage *image = g_ptr_array_index(entry->images, i);

        lrg_image_set_texture(image, entry->texture);
        lrg_widget_set_visible(LRG_WIDGET(image), TRUE);
    }

    clawt_lrg_app_wake(app);
}

void
clawt_lrg_avatar_invalidate(const gchar *agent_id)
{
    AvatarEntry *entry;
    guint i;

    if (avatars == NULL || agent_id == NULL)
        return;

    entry = g_hash_table_lookup(avatars, agent_id);

    if (entry == NULL)
        return;

    /* the next avatar drawn asks again; those on screen drop the old one */
    entry->asked = FALSE;
    g_clear_object(&entry->texture);

    for (i = 0; i < entry->images->len; i++)
        lrg_widget_set_visible(g_ptr_array_index(entry->images, i), FALSE);
}

/* Up to two initials from @name's first two words, upper-cased. */
static gchar *
initials_of(const gchar *name)
{
    GString *out = g_string_new(NULL);
    gboolean at_word = TRUE;
    const gchar *p;
    guint taken = 0;

    for (p = name; p != NULL && *p != '\0' && taken < 2;
         p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        if (!g_unichar_isalnum(c)) {
            at_word = TRUE;
            continue;
        }

        if (at_word) {
            g_string_append_unichar(out, g_unichar_toupper(c));
            taken++;
        }

        at_word = FALSE;
    }

    if (out->len == 0)
        g_string_append_c(out, '?');

    return g_string_free(out, FALSE);
}

static gboolean
hex_color(const gchar *hex, GrlColor *out)
{
    gdouble rgba[4];
    g_autofree gchar *css = NULL;

    if (hex == NULL)
        return FALSE;

    css = g_strdup_printf("@define-color c %s;", hex);

    if (!clawt_palette_color(css, "c", rgba))
        return FALSE;

    out->r = (guint8)(rgba[0] * 255.0);
    out->g = (guint8)(rgba[1] * 255.0);
    out->b = (guint8)(rgba[2] * 255.0);
    out->a = 255;

    return TRUE;
}

LrgWidget *
clawt_lrg_avatar(ClawtLrgApp *app, const gchar *name, const gchar *agent_id,
                 gboolean has_avatar, const gchar *color, gfloat size,
                 gboolean live)
{
    LrgOverlay *overlay = lrg_overlay_new();
    LrgPanel *disc = lrg_panel_new();
    LrgImage *image = lrg_image_new();
    g_autofree gchar *initials = initials_of(name);
    LrgWidget *label = clawt_lrg_label(initials, NULL);
    GrlColor fill = { 0x6c, 0x70, 0x86, 0xff };
    GrlColor ink = { 0xff, 0xff, 0xff, 0xff };
    LrgPartLook look;

    hex_color(color, &fill);
    hex_color(clawt_color_ink(color), &ink);

    lrg_widget_set_min_size(LRG_WIDGET(overlay), size, size);
    lrg_widget_set_max_size(LRG_WIDGET(overlay), size, size);
    lrg_widget_set_valign(LRG_WIDGET(overlay), LRG_ALIGN_CENTER);

    /*
     * The disc carries the glow: a running agent's ring lights up in its
     * own colour, so a sidebar of a busy fleet reads at a glance.
     */
    lrg_part_look_init(&look);
    lrg_part_look_set_background(&look, &fill);
    lrg_part_look_set_radius(&look, size / 2.0f);

    if (live) {
        GrlColor glow = fill;

        glow.a = 0xa0;
        lrg_part_look_set_glow(&look, &glow, size * 0.32f);
        lrg_part_look_set_border(&look, &ink, MAX(1.5f, size * 0.045f));
    }

    lrg_widget_set_part_look(LRG_WIDGET(disc), LRG_UI_PART_BOX,
                             LRG_WIDGET_STATE_NONE, &look);
    clawt_lrg_take(LRG_WIDGET(overlay), LRG_WIDGET(disc));

    lrg_label_set_color(LRG_LABEL(label), &ink);
    lrg_label_set_font_size(LRG_LABEL(label), size * 0.40f);
    lrg_label_set_alignment(LRG_LABEL(label), LRG_TEXT_ALIGN_CENTER);
    lrg_widget_set_halign(label, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(label, LRG_ALIGN_CENTER);
    clawt_lrg_take(LRG_WIDGET(overlay), label);

    lrg_image_set_corner_radius(image, size / 2.0f);
    lrg_image_set_scale_mode(image, LRG_IMAGE_SCALE_MODE_FILL);
    lrg_widget_set_visible(LRG_WIDGET(image), FALSE);

    if (has_avatar && agent_id != NULL) {
        AvatarEntry *entry = avatar_entry(agent_id);

        g_ptr_array_add(entry->images, image);
        g_object_weak_ref(G_OBJECT(image), weak_image_gone, entry->images);

        if (entry->texture != NULL) {
            lrg_image_set_texture(image, entry->texture);
            lrg_widget_set_visible(LRG_WIDGET(image), TRUE);
        } else if (!entry->asked) {
            AvatarFetch *fetch = g_new0(AvatarFetch, 1);

            entry->asked = TRUE;
            fetch->agent_id = g_strdup(agent_id);
            clawt_lrg_request(app, "agent.avatar",
                              clawt_ipc_payload_new("agent", agent_id, NULL),
                              CLAWT_LRG_QUIET, on_avatar, fetch,
                              avatar_fetch_free);
        }
    }

    clawt_lrg_take(LRG_WIDGET(overlay), LRG_WIDGET(image));

    return LRG_WIDGET(overlay);
}
