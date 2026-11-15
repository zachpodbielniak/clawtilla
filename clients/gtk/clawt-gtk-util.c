/*
 * clawt-gtk-util.c - Small shared helpers for the GTK client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawt-gtk.h"

#include <gio/gio.h>
#include <stdarg.h>
#include <string.h>

const gchar *
clawt_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    if (object == NULL || key == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_STRING)
        return fallback;

    return json_object_get_string_member(object, key);
}

gint64
clawt_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    if (object == NULL || key == NULL || !json_object_has_member(object, key))
        return fallback;

    if (json_node_get_value_type(json_object_get_member(object, key)) !=
        G_TYPE_INT64)
        return fallback;

    return json_object_get_int_member(object, key);
}

gboolean
clawt_json_boolean(JsonObject *object, const gchar *key, gboolean fallback)
{
    JsonNode *member;

    if (object == NULL || !json_object_has_member(object, key))
        return fallback;

    member = json_object_get_member(object, key);

    if (JSON_NODE_TYPE(member) != JSON_NODE_VALUE ||
        json_node_get_value_type(member) != G_TYPE_BOOLEAN)
        return fallback;

    return json_object_get_boolean_member(object, key);
}

/*
 * Thin names over libclawt's readers, kept because every page in this
 * client spells them this way.  The rules themselves -- an object type
 * holding no object, a NULL value dropping its member -- live in
 * src/ipc/clawt-ipc-proto.c, shared with clawtilla-lrg.
 */
JsonObject *
clawt_payload_of(JsonNode *reply)
{
    return clawt_ipc_reply_object(reply);
}

JsonNode *
clawt_build_payload(const gchar *first_key, ...)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *key = first_key;
    va_list args;

    /*
     * Not a call to clawt_ipc_payload_new(): C cannot forward a va_list
     * into a variadic function, and a v-variant in the public API for
     * one caller is more surface than this loop.  The NULL-drops rule is
     * the same and test-client-rules.c pins both.
     */
    json_builder_begin_object(builder);

    va_start(args, first_key);

    while (key != NULL) {
        const gchar *value = va_arg(args, const gchar *);

        if (value != NULL) {
            json_builder_set_member_name(builder, key);
            json_builder_add_string_value(builder, value);
        }

        key = va_arg(args, const gchar *);
    }

    va_end(args);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* ── Profile pictures ────────────────────────────────────────────── */

/*
 * Two tables rather than one holding %NULL for "no picture": a
 * #GHashTable configured to free a #GdkTexture value would call
 * g_object_unref() on that %NULL too if the two were ever mixed up, and
 * "known to have nothing" has to survive a lookup that also has to be
 * able to say "never asked".
 */
static GHashTable *avatar_textures = NULL; /* agent id -> GdkTexture, owned */
static GHashTable *avatar_absent = NULL;   /* agent id -> nothing; a set */

/*
 * The bytes, decoded to @decode_size.
 *
 * Shared by the cached row texture and the uncached preview, because
 * the only difference between them is that number and where the answer
 * is kept -- and two copies of "fetch, base64-decode, scale, wrap"
 * would drift the first time one of them learned something.
 */
static GdkTexture *
fetch_avatar_texture(ClawtClient *client, const gchar *agent_id,
                     gint decode_size)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;
    JsonObject *result;
    const gchar *base64;
    g_autofree guchar *raw = NULL;
    gsize raw_length = 0;
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GInputStream) stream = NULL;
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    g_autoptr(GBytes) pixels = NULL;

    /*
     * clawt_client_request() rather than clawt_window_request(): the
     * common case here is an agent with no picture, and that answers
     * NOT_FOUND -- which clawt_window_request() would toast on every
     * single call, for every agent, on every redraw that has not
     * populated the cache yet.
     */
    reply = clawt_client_request(
        client, "agent.avatar",
        clawt_build_payload("agent", agent_id, NULL), &error);

    if (reply == NULL)
        return NULL;

    result = clawt_payload_of(reply);
    base64 = clawt_json_string(result, "base64", NULL);

    if (base64 == NULL)
        return NULL;

    raw = g_base64_decode(base64, &raw_length);
    bytes = g_bytes_new_take(g_steal_pointer(&raw), raw_length);
    stream = g_memory_input_stream_new_from_bytes(bytes);

    /*
     * Decoded straight to @decode_size rather than at full resolution
     * and shrunk by a widget afterwards -- a size request is a minimum
     * in GTK, never a maximum, so a widget never actually shrinks
     * anything.
     */
    pixbuf = gdk_pixbuf_new_from_stream_at_scale(
        stream, decode_size, decode_size, TRUE, NULL, &error);

    if (pixbuf == NULL)
        return NULL;

    /*
     * A memory texture from the pixbuf's own pixels.
     * gdk_texture_new_for_pixbuf() would say this in one line and is
     * deprecated.
     */
    pixels = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                         gdk_pixbuf_get_byte_length(pixbuf));

    return gdk_memory_texture_new(
        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8
                                        : GDK_MEMORY_R8G8B8,
        pixels, (gsize)gdk_pixbuf_get_rowstride(pixbuf));
}

GdkTexture *
clawt_gtk_avatar_texture(ClawtClient *client, const gchar *agent_id)
{
    GdkTexture *cached;
    GdkTexture *texture;

    g_return_val_if_fail(client != NULL, NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    if (avatar_textures == NULL)
        avatar_textures = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_object_unref);

    if (avatar_absent == NULL)
        avatar_absent = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);

    cached = g_hash_table_lookup(avatar_textures, agent_id);

    if (cached != NULL)
        return g_object_ref(cached);

    if (g_hash_table_contains(avatar_absent, agent_id))
        return NULL;

    texture = fetch_avatar_texture(client, agent_id,
                                   CLAWT_AVATAR_DECODE_SIZE);

    if (texture == NULL) {
        g_hash_table_add(avatar_absent, g_strdup(agent_id));
        return NULL;
    }

    g_hash_table_insert(avatar_textures, g_strdup(agent_id),
                        g_object_ref(texture));

    return texture;
}

GdkTexture *
clawt_gtk_avatar_preview_texture(ClawtClient *client, const gchar *agent_id)
{
    g_return_val_if_fail(client != NULL, NULL);
    g_return_val_if_fail(agent_id != NULL, NULL);

    /*
     * Neither table is consulted nor written.  The absent set would be
     * a false negative here -- it remembers that the *row* fetch found
     * nothing, which is the same answer, but a click is also the moment
     * somebody most wants a stale "no picture" re-asked.
     */
    return fetch_avatar_texture(client, agent_id,
                                CLAWT_AVATAR_PREVIEW_SIZE);
}

void
clawt_gtk_avatar_invalidate(const gchar *agent_id)
{
    if (agent_id == NULL) {
        if (avatar_textures != NULL)
            g_hash_table_remove_all(avatar_textures);

        if (avatar_absent != NULL)
            g_hash_table_remove_all(avatar_absent);

        return;
    }

    if (avatar_textures != NULL)
        g_hash_table_remove(avatar_textures, agent_id);

    if (avatar_absent != NULL)
        g_hash_table_remove(avatar_absent, agent_id);
}

/* ── QR codes ────────────────────────────────────────────────────── */

GdkTexture *
clawt_gtk_qr_texture(const gchar *text, guint scale, GError **error)
{
    g_autoptr(GBytes) modules = NULL;
    g_autoptr(GBytes) bytes = NULL;
    const guint8 *dark;
    guint8 *pixels;
    guint size = 0;
    gsize side;
    gsize stride;
    gsize x;
    gsize y;

    g_return_val_if_fail(text != NULL, NULL);
    g_return_val_if_fail(scale > 0, NULL);

    modules = clawt_qr_encode(text, &size, error);

    if (modules == NULL)
        return NULL;

    dark = g_bytes_get_data(modules, NULL);
    side = ((gsize)size + 2 * CLAWT_QR_QUIET_ZONE) * scale;
    stride = side * 3;

    /* white everywhere first, so the quiet zone costs nothing to draw */
    pixels = g_malloc(stride * side);
    memset(pixels, 0xff, stride * side);

    for (y = 0; y < side; y++) {
        gssize my = (gssize)(y / scale) - CLAWT_QR_QUIET_ZONE;

        if (my < 0 || my >= (gssize)size)
            continue;

        for (x = 0; x < side; x++) {
            gssize mx = (gssize)(x / scale) - CLAWT_QR_QUIET_ZONE;

            if (mx >= 0 && mx < (gssize)size &&
                dark[(gsize)my * size + (gsize)mx] != 0)
                memset(pixels + y * stride + x * 3, 0x00, 3);
        }
    }

    bytes = g_bytes_new_take(pixels, stride * side);

    return gdk_memory_texture_new((gint)side, (gint)side, GDK_MEMORY_R8G8B8,
                                  bytes, stride);
}

/*
 * A style class painting an avatar in one configured colour.
 *
 * One provider on the display carrying a rule per colour, rather than a
 * provider per widget: gtk_style_context_add_provider() is deprecated,
 * and adding one provider per avatar would leave a sheet on the display
 * for every message ever drawn. The class name is derived from the
 * colour, so two agents sharing one produce one rule and a colour that
 * has already been seen costs a hash lookup.
 *
 * @color reached clawt_color_ink() before this, which is what makes it
 * safe to splice: nothing but `#rgb` and `#rrggbb` gets this far.
 *
 * Above the appearance sheet, at PRIORITY_APPLICATION + 2, because a
 * tint is about one particular agent and a palette is an opinion about
 * surfaces in general. A single `avatar { background-color: ... }` in a
 * theme would otherwise flatten every agent to one swatch -- and it
 * would win despite being the less specific selector, because a
 * provider's priority decides before specificity is consulted.
 *
 * Still below PRIORITY_USER, so somebody who does want uniform avatars
 * can say so. The stack ascends from the most general to the closest to
 * the data, with the person on top.
 */
static GHashTable     *avatar_tints = NULL;
static GtkCssProvider *avatar_tint_provider = NULL;

static gchar *
avatar_tint_class(const gchar *color, const gchar *ink)
{
    gchar *name = g_strdup_printf("clawt-tint-%s", color + 1);

    if (avatar_tints == NULL)
        avatar_tints = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    if (!g_hash_table_contains(avatar_tints, name)) {
        g_autoptr(GString) sheet = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_insert(avatar_tints, g_strdup(name),
                            g_strdup_printf("%s %s", color, ink));

        g_hash_table_iter_init(&iter, avatar_tints);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_auto(GStrv) pair = g_strsplit(value, " ", 2);

            /*
             * On the inner node.  #AdwAvatar is a `widget` holding an
             * `avatar` gizmo, and the gizmo is what carries `.colorN`
             * and paints the gradient -- so `avatar.<tint>`, the rule
             * this used to write, matched no node at all, and every
             * face (a configured colour included) was #AdwAvatar's own
             * hash of whatever text it was handed.  Measured on
             * libadwaita 1.9: widget.<tint> > avatar.color4.
             */
            g_string_append_printf(
                sheet,
                ".%s > avatar { background-image: none; "
                "background-color: %s; color: %s; }\n",
                (const gchar *)key, pair[0], pair[1]);
        }

        if (avatar_tint_provider == NULL) {
            avatar_tint_provider = gtk_css_provider_new();
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(),
                GTK_STYLE_PROVIDER(avatar_tint_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
        }

        gtk_css_provider_load_from_string(avatar_tint_provider, sheet->str);
    }

    return name;
}

/*
 * The popover's items for clawt_gtk_combo_row_new().  A label that wraps
 * and a checkmark, as #AdwComboRow's own item has, the checkmark kept in
 * step with the row's selection while the popover is open.
 */
#define COMBO_ITEM_LABEL "clawt-combo-label"
#define COMBO_ITEM_CHECK "clawt-combo-check"
#define COMBO_ITEM_HANDLER "clawt-combo-handler"

/* Whether the item at @item's position is the row's selection. */
static void
combo_item_sync_check(GtkListItem *item, AdwComboRow *row)
{
    GtkWidget *check = g_object_get_data(G_OBJECT(item), COMBO_ITEM_CHECK);

    if (check == NULL)
        return;

    gtk_widget_set_opacity(
        check, (adw_combo_row_get_selected(row) ==
                gtk_list_item_get_position(item)) ? 1.0 : 0.0);
}

static void
on_combo_row_selected(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    combo_item_sync_check(GTK_LIST_ITEM(user_data), ADW_COMBO_ROW(row));
}

static void
combo_item_setup(GtkSignalListItemFactory *factory, GObject *object,
                 gpointer user_data)
{
    GtkListItem *item = GTK_LIST_ITEM(object);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(NULL);
    GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");

    /*
     * Wrapped rather than ellipsized, at a measure that keeps a short
     * sentence on one line: the popover grows to the widest item up to
     * that, and a longer one breaks over lines instead of losing its end.
     */
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);

    /* Hidden by opacity, so a selection never changes an item's width. */
    gtk_widget_set_opacity(check, 0.0);
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), check);
    gtk_list_item_set_child(item, box);

    g_object_set_data(G_OBJECT(item), COMBO_ITEM_LABEL, label);
    g_object_set_data(G_OBJECT(item), COMBO_ITEM_CHECK, check);
}

static void
combo_item_bind(GtkSignalListItemFactory *factory, GObject *object,
                gpointer user_data)
{
    GtkListItem *item = GTK_LIST_ITEM(object);
    AdwComboRow *row = ADW_COMBO_ROW(user_data);
    GtkWidget *label = g_object_get_data(G_OBJECT(item), COMBO_ITEM_LABEL);
    GObject *choice = gtk_list_item_get_item(item);
    g_autofree gchar *text = NULL;
    gulong handler;

    /*
     * The string the row itself would show: a string list's own, or the
     * row's expression evaluated on the item, which is how a model of
     * objects names its choices.
     */
    if (GTK_IS_STRING_OBJECT(choice)) {
        text = g_strdup(gtk_string_object_get_string(
            GTK_STRING_OBJECT(choice)));
    } else if (adw_combo_row_get_expression(row) != NULL) {
        GValue value = G_VALUE_INIT;

        if (gtk_expression_evaluate(adw_combo_row_get_expression(row),
                                    choice, &value)) {
            text = g_value_dup_string(&value);
            g_value_unset(&value);
        }
    }

    gtk_label_set_text(GTK_LABEL(label), text != NULL ? text : "");

    combo_item_sync_check(item, row);
    handler = g_signal_connect_object(row, "notify::selected",
                                      G_CALLBACK(on_combo_row_selected),
                                      item, 0);
    g_object_set_data(G_OBJECT(item), COMBO_ITEM_HANDLER,
                      GSIZE_TO_POINTER((gsize)handler));
}

static void
combo_item_unbind(GtkSignalListItemFactory *factory, GObject *object,
                  gpointer user_data)
{
    gulong handler = (gulong)GPOINTER_TO_SIZE(
        g_object_get_data(object, COMBO_ITEM_HANDLER));

    if (handler != 0)
        g_signal_handler_disconnect(user_data, handler);

    g_object_set_data(object, COMBO_ITEM_HANDLER, NULL);
}

GtkWidget *
clawt_gtk_combo_row_new(void)
{
    GtkWidget *row = adw_combo_row_new();
    g_autoptr(GtkListItemFactory) factory =
        gtk_signal_list_item_factory_new();

    /*
     * The row is not reffed by its own factory's handlers: the row owns
     * the factory, so the handlers cannot outlive it.
     */
    g_signal_connect(factory, "setup", G_CALLBACK(combo_item_setup), row);
    g_signal_connect(factory, "bind", G_CALLBACK(combo_item_bind), row);
    g_signal_connect(factory, "unbind", G_CALLBACK(combo_item_unbind), row);
    adw_combo_row_set_list_factory(ADW_COMBO_ROW(row), factory);

    return row;
}

GtkWidget *
clawt_gtk_build_avatar(
    ClawtClient  *client,
    const gchar  *name,
    const gchar  *agent_id,
    gboolean      has_avatar,
    const gchar  *color,
    gint          size
){
    GtkWidget *avatar = adw_avatar_new(size, name, TRUE);
    const gchar *ink;

    gtk_widget_add_css_class(avatar, "clawt-avatar");

    if (has_avatar && agent_id != NULL) {
        g_autoptr(GdkTexture) texture =
            clawt_gtk_avatar_texture(client, agent_id);

        if (texture != NULL) {
            adw_avatar_set_custom_image(ADW_AVATAR(avatar),
                                        GDK_PAINTABLE(texture));
            return avatar;
        }
    }

    /*
     * Always painted, never left to #AdwAvatar's own colour.  That one
     * is a hash of whatever text the widget was handed, and the sidebar
     * handed it the display name while the transcript handed it the
     * sender id, so one agent was two colours.  clawt_avatar_color() is
     * the configured colour when clawt_color_ink() accepts it -- the
     * check before anything typed into YAML reaches a stylesheet -- and
     * otherwise the tone every client derives from the agent's id.
     */
    color = clawt_avatar_color(agent_id, name, color);
    ink = clawt_color_ink(color);

    if (ink != NULL) {
        g_autofree gchar *class_name = avatar_tint_class(color, ink);

        gtk_widget_add_css_class(avatar, class_name);
    }

    return avatar;
}
