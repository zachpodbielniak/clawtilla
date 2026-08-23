/*
 * clawt-matrix.c - Signing in to Matrix, and finding the rooms
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-matrix.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <string.h>

/*
 * A room listing costs one request per room, so a very large account
 * would otherwise sit there making thousands of them while a person
 * watches a spinner.  The cap is announced in the listing rather than
 * applied silently -- a truncated list that says nothing reads as "you
 * are not in that room".
 */
#define MAX_ROOMS_LISTED (250)

#define USER_AGENT \
    "clawtilla/" G_STRINGIFY(CLAWT_VERSION_MAJOR) "." \
    G_STRINGIFY(CLAWT_VERSION_MINOR) "." G_STRINGIFY(CLAWT_VERSION_MICRO)

/* ── Boxed types ─────────────────────────────────────────────────── */

ClawtMatrixLogin *
clawt_matrix_login_copy(ClawtMatrixLogin *self)
{
    ClawtMatrixLogin *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtMatrixLogin, 1);
    copy->user_id = g_strdup(self->user_id);
    copy->device_id = g_strdup(self->device_id);
    copy->access_token = g_strdup(self->access_token);

    return copy;
}

void
clawt_matrix_login_free(ClawtMatrixLogin *self)
{
    if (self == NULL)
        return;

    /*
     * Wiped rather than merely freed.  This is the one struct in
     * clawtilla that holds a live credential in memory, and it exists for
     * the few milliseconds between a homeserver answering and the token
     * reaching a 0600 file.
     */
    if (self->access_token != NULL)
        memset(self->access_token, 0, strlen(self->access_token));

    g_free(self->user_id);
    g_free(self->device_id);
    g_free(self->access_token);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtMatrixLogin, clawt_matrix_login,
                    clawt_matrix_login_copy, clawt_matrix_login_free)

ClawtMatrixRoom *
clawt_matrix_room_copy(ClawtMatrixRoom *self)
{
    ClawtMatrixRoom *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtMatrixRoom, 1);
    copy->id = g_strdup(self->id);
    copy->name = g_strdup(self->name);
    copy->alias = g_strdup(self->alias);

    return copy;
}

void
clawt_matrix_room_free(ClawtMatrixRoom *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->name);
    g_free(self->alias);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtMatrixRoom, clawt_matrix_room,
                    clawt_matrix_room_copy, clawt_matrix_room_free)

gchar *
clawt_matrix_room_describe(ClawtMatrixRoom *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    if (self->name != NULL && *self->name != '\0')
        return g_strdup(self->name);

    if (self->alias != NULL && *self->alias != '\0')
        return g_strdup(self->alias);

    return g_strdup(self->id);
}

/* ── Pure helpers ────────────────────────────────────────────────── */

gchar *
clawt_matrix_base_url(const gchar *homeserver)
{
    g_autofree gchar *with_scheme = NULL;
    g_autoptr(GUri) uri = NULL;
    const gchar *path;
    gsize length;

    if (homeserver == NULL || *homeserver == '\0')
        return NULL;

    if (strstr(homeserver, "://") == NULL)
        with_scheme = g_strconcat("https://", homeserver, NULL);
    else
        with_scheme = g_strdup(homeserver);

    uri = g_uri_parse(with_scheme, G_URI_FLAGS_NONE, NULL);

    if (uri == NULL || g_uri_get_host(uri) == NULL ||
        *g_uri_get_host(uri) == '\0')
        return NULL;

    if (g_strcmp0(g_uri_get_scheme(uri), "http") != 0 &&
        g_strcmp0(g_uri_get_scheme(uri), "https") != 0)
        return NULL;

    path = g_uri_get_path(uri);

    /*
     * A real path is refused rather than kept or stripped.  Keeping it
     * produces `/_matrix/_matrix/...` for the person who helpfully pasted
     * the API root; stripping it silently ignores a server genuinely
     * hosted under a prefix.  Neither is a guess worth making on
     * somebody's behalf.
     *
     * Slashes are not a path, though: `https://matrix.example.org//` is
     * the root typed twice, and refusing it would report a perfectly
     * good address as not being one.
     */
    if (path != NULL) {
        const gchar *p = path;

        while (*p == '/')
            p++;

        if (*p != '\0')
            return NULL;
    }

    length = strlen(with_scheme);

    while (length > 0 && with_scheme[length - 1] == '/')
        with_scheme[--length] = '\0';

    return g_steal_pointer(&with_scheme);
}

/*
 * Turns a Matrix error body into a GError.
 *
 * The server's own message is used whenever there is one: "Invalid
 * password" is the answer, and replacing it with "login failed (403)"
 * throws away the only part a person can act on.
 */
static void
set_matrix_error(GError **error, guint status, const gchar *body)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    const gchar *message = NULL;
    const gchar *code = NULL;

    if (body != NULL && json_parser_load_from_data(parser, body, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);

        if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);

            if (json_object_has_member(object, "error"))
                message = json_object_get_string_member(object, "error");

            if (json_object_has_member(object, "errcode"))
                code = json_object_get_string_member(object, "errcode");
        }
    }

    if (message != NULL && code != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "%s (%s)", message, code);
        return;
    }

    if (message != NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AUTH, message);
        return;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                "the homeserver answered %u and said nothing more", status);
}

ClawtMatrixLogin *
clawt_matrix_parse_login(const gchar *json, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;
    ClawtMatrixLogin *login;

    if (json == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver sent an empty reply");
        return NULL;
    }

    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver sent something that is not JSON");
        return NULL;
    }

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver sent something that is not a "
                            "login response");
        return NULL;
    }

    object = json_node_get_object(root);

    /*
     * A body carrying an errcode is an error whatever the status was.
     * Some deployments answer 200 with one, which would otherwise become
     * a login with no token that fails much later and elsewhere.
     */
    if (json_object_has_member(object, "errcode")) {
        set_matrix_error(error, 0, json);
        return NULL;
    }

    if (!json_object_has_member(object, "access_token")) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver accepted the login and returned "
                            "no access token");
        return NULL;
    }

    login = g_new0(ClawtMatrixLogin, 1);
    login->access_token =
        g_strdup(json_object_get_string_member(object, "access_token"));

    if (json_object_has_member(object, "user_id"))
        login->user_id =
            g_strdup(json_object_get_string_member(object, "user_id"));

    if (json_object_has_member(object, "device_id"))
        login->device_id =
            g_strdup(json_object_get_string_member(object, "device_id"));

    return login;
}

GPtrArray *
clawt_matrix_parse_joined_rooms(const gchar *json, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    GPtrArray *rooms;
    JsonNode *root;
    JsonObject *object;
    JsonArray *array;
    guint i;
    guint length;

    if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver sent something that is not JSON");
        return NULL;
    }

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the homeserver sent something that is not a "
                            "room list");
        return NULL;
    }

    object = json_node_get_object(root);

    if (json_object_has_member(object, "errcode")) {
        set_matrix_error(error, 0, json);
        return NULL;
    }

    if (!json_object_has_member(object, "joined_rooms") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object,
                                                      "joined_rooms"))) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the reply has no joined_rooms");
        return NULL;
    }

    array = json_object_get_array_member(object, "joined_rooms");
    length = json_array_get_length(array);
    rooms = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_matrix_room_free);

    for (i = 0; i < length && i < MAX_ROOMS_LISTED; i++) {
        const gchar *id = json_array_get_string_element(array, i);
        ClawtMatrixRoom *room;

        if (id == NULL || *id == '\0')
            continue;

        room = g_new0(ClawtMatrixRoom, 1);
        room->id = g_strdup(id);
        g_ptr_array_add(rooms, room);
    }

    if (length > MAX_ROOMS_LISTED)
        g_warning("this account is in %u rooms; listing the first %d",
                  length, MAX_ROOMS_LISTED);

    return rooms;
}

/* ── The session ─────────────────────────────────────────────────── */

/*
 * One session for every Matrix call clawtilla makes.
 *
 * Named, for the same reason the image store's is: a request with no
 * User-Agent is refused outright by some deployments, and the refusal
 * arrives as a body that parses into nothing useful.
 */
static SoupSession *
matrix_session(void)
{
    static SoupSession *session = NULL;

    if (session == NULL)
        session = soup_session_new_with_options("user-agent", USER_AGENT,
                                                "timeout", 20, NULL);

    return session;
}

/* ── Login ───────────────────────────────────────────────────────── */

static void
on_login_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GTask) task = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    ClawtMatrixLogin *login;
    SoupMessage *message = g_task_get_task_data(task);
    gsize size = 0;
    const gchar *data;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    data = g_bytes_get_data(body, &size);
    text = g_strndup(data != NULL ? data : "", size);

    if (soup_message_get_status(message) != SOUP_STATUS_OK) {
        set_matrix_error(&error, soup_message_get_status(message), text);
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    login = clawt_matrix_parse_login(text, &error);

    if (login == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_pointer(task, login,
                          (GDestroyNotify)clawt_matrix_login_free);
}

void
clawt_matrix_login_async(const gchar         *homeserver,
                         const gchar         *user,
                         const gchar         *password,
                         const gchar         *device_name,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_autofree gchar *base = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *payload = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;
    g_autoptr(GBytes) bytes = NULL;
    SoupMessage *message;

    g_task_set_source_tag(task, clawt_matrix_login_async);

    base = clawt_matrix_base_url(homeserver);

    if (base == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "'%s' is not a homeserver address",
                                homeserver != NULL ? homeserver : "");
        g_object_unref(task);
        return;
    }

    if (user == NULL || *user == '\0' || password == NULL ||
        *password == '\0') {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "a user and a password are both needed");
        g_object_unref(task);
        return;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "m.login.password");
    json_builder_set_member_name(builder, "identifier");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "m.id.user");
    json_builder_set_member_name(builder, "user");
    json_builder_add_string_value(builder, user);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "password");
    json_builder_add_string_value(builder, password);

    /*
     * Named on purpose.  The account's device list is where a person
     * revokes this later, and "clawtilla (researcher)" beside a date is
     * the difference between confidently signing out one session and
     * signing out of everything to be sure.
     */
    json_builder_set_member_name(builder, "initial_device_display_name");
    json_builder_add_string_value(builder,
                                  device_name != NULL ? device_name
                                                      : "clawtilla");
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    payload = json_generator_to_data(generator, NULL);

    url = g_strconcat(base, "/_matrix/client/v3/login", NULL);
    message = soup_message_new("POST", url);

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "%s is not a URL this can post to", url);
        g_object_unref(task);
        return;
    }

    bytes = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(message, "application/json",
                                             bytes);

    g_task_set_task_data(task, message, g_object_unref);

    soup_session_send_and_read_async(matrix_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_login_response, task);

    /*
     * The password is wiped from the body we built as soon as it is on
     * its way.  soup has its own copy in the GBytes by now, which we do
     * not control, but there is no reason for a second one to sit in the
     * daemon's heap until the allocator happens to reuse it.
     */
    memset(payload, 0, strlen(payload));
}

ClawtMatrixLogin *
clawt_matrix_login_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Rooms ───────────────────────────────────────────────────────── */

typedef struct {
    GTask       *task;        /* unowned; the listing owns itself through it */
    gchar       *base;
    gchar       *token;
    SoupMessage *message;     /* the joined_rooms request, owned */
    GPtrArray   *rooms;       /* ClawtMatrixRoom* */
    guint        outstanding;
    gboolean     listed;
} Listing;

static void
listing_free(Listing *self)
{
    if (self == NULL)
        return;

    g_free(self->base);
    g_clear_object(&self->message);

    if (self->token != NULL)
        memset(self->token, 0, strlen(self->token));

    g_free(self->token);
    g_clear_pointer(&self->rooms, g_ptr_array_unref);
    g_free(self);
}

/*
 * Sorted by what a person will see, so the picker is not in whatever
 * order the homeserver happened to return.
 */
static gint
compare_rooms(gconstpointer a, gconstpointer b)
{
    ClawtMatrixRoom *left = *(ClawtMatrixRoom **)a;
    ClawtMatrixRoom *right = *(ClawtMatrixRoom **)b;
    g_autofree gchar *left_label = clawt_matrix_room_describe(left);
    g_autofree gchar *right_label = clawt_matrix_room_describe(right);

    return g_utf8_collate(left_label, right_label);
}

static void
listing_maybe_finish(Listing *listing)
{
    GTask *task;

    if (!listing->listed || listing->outstanding > 0)
        return;

    g_ptr_array_sort(listing->rooms, compare_rooms);

    task = listing->task;
    g_task_return_pointer(task, g_steal_pointer(&listing->rooms),
                          (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
}

/*
 * One outstanding state lookup.
 *
 * Allocated per request rather than stashed on the session, because the
 * session is shared and every room's lookups are in flight at once: a
 * pointer parked on it would be overwritten by the next request before
 * the first one's reply arrived, and every callback would decorate the
 * same room.
 */
typedef struct {
    Listing         *listing;
    ClawtMatrixRoom *room;
    gboolean         is_name;
    SoupMessage     *message;   /* owned */
} StateRequest;

static void
state_request_free(StateRequest *self)
{
    if (self == NULL)
        return;

    g_clear_object(&self->message);
    g_free(self);
}

static void
on_room_state(GObject *source, GAsyncResult *result, gpointer user_data)
{
    StateRequest *request = user_data;
    Listing *listing = request->listing;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    gsize size = 0;
    const gchar *data;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);
    listing->outstanding--;

    /*
     * A room with no name gives a 404, which is the ordinary case for a
     * direct chat.  Every failure here leaves the room in the list with
     * whatever it already has, because one unnamed room is not a reason
     * to fail a listing somebody is waiting on.
     */
    if (body == NULL)
        goto out;

    if (soup_message_get_status(request->message) != SOUP_STATUS_OK)
        goto out;

    data = g_bytes_get_data(body, &size);
    text = g_strndup(data != NULL ? data : "", size);

    if (json_parser_load_from_data(parser, text, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);

        if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);
            const gchar *field = request->is_name ? "name" : "alias";

            if (json_object_has_member(object, field)) {
                const gchar *value =
                    json_object_get_string_member(object, field);

                if (request->is_name)
                    request->room->name = g_strdup(value);
                else
                    request->room->alias = g_strdup(value);
            }
        }
    }

out:
    state_request_free(request);
    listing_maybe_finish(listing);
}

/*
 * Asks one room for one piece of state.
 *
 * Fired for every room at once rather than in sequence: the round trips
 * are independent, and a person picking rooms should not wait for two
 * hundred of them end to end.
 */
static void
fetch_room_state(Listing *listing, ClawtMatrixRoom *room,
                 const gchar *event, gboolean is_name,
                 GCancellable *cancellable)
{
    g_autofree gchar *escaped = g_uri_escape_string(room->id, NULL, FALSE);
    g_autofree gchar *url = NULL;
    g_autofree gchar *bearer = NULL;
    StateRequest *request;
    SoupMessage *message;

    url = g_strdup_printf("%s/_matrix/client/v3/rooms/%s/state/%s",
                          listing->base, escaped, event);
    message = soup_message_new("GET", url);

    if (message == NULL)
        return;

    bearer = g_strconcat("Bearer ", listing->token, NULL);
    soup_message_headers_replace(soup_message_get_request_headers(message),
                                 "Authorization", bearer);
    memset(bearer, 0, strlen(bearer));

    request = g_new0(StateRequest, 1);
    request->listing = listing;
    request->room = room;
    request->is_name = is_name;
    request->message = message;

    listing->outstanding++;

    soup_session_send_and_read_async(matrix_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_room_state, request);
}

static void
on_joined_rooms(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Listing *listing = user_data;
    GTask *task = listing->task;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    gsize size = 0;
    const gchar *data;
    guint i;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    data = g_bytes_get_data(body, &size);
    text = g_strndup(data != NULL ? data : "", size);

    if (soup_message_get_status(listing->message) != SOUP_STATUS_OK) {
        set_matrix_error(&error, soup_message_get_status(listing->message),
                         text);
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    listing->rooms = clawt_matrix_parse_joined_rooms(text, &error);

    if (listing->rooms == NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    listing->listed = TRUE;

    for (i = 0; i < listing->rooms->len; i++) {
        ClawtMatrixRoom *room = g_ptr_array_index(listing->rooms, i);

        fetch_room_state(listing, room, "m.room.name", TRUE,
                         g_task_get_cancellable(task));
        fetch_room_state(listing, room, "m.room.canonical_alias", FALSE,
                         g_task_get_cancellable(task));
    }

    listing_maybe_finish(listing);
}

void
clawt_matrix_rooms_async(const gchar         *homeserver,
                         const gchar         *access_token,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_autofree gchar *base = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *bearer = NULL;
    SoupMessage *message;
    Listing *listing;

    g_task_set_source_tag(task, clawt_matrix_rooms_async);

    base = clawt_matrix_base_url(homeserver);

    if (base == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "'%s' is not a homeserver address",
                                homeserver != NULL ? homeserver : "");
        g_object_unref(task);
        return;
    }

    if (access_token == NULL || *access_token == '\0') {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                                "there is no access token to list rooms with");
        g_object_unref(task);
        return;
    }

    url = g_strconcat(base, "/_matrix/client/v3/joined_rooms", NULL);
    message = soup_message_new("GET", url);

    if (message == NULL) {
        g_task_return_new_error(task, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                                "%s is not a URL this can fetch", url);
        g_object_unref(task);
        return;
    }

    bearer = g_strconcat("Bearer ", access_token, NULL);
    soup_message_headers_replace(soup_message_get_request_headers(message),
                                 "Authorization", bearer);
    memset(bearer, 0, strlen(bearer));

    listing = g_new0(Listing, 1);
    listing->task = task;
    listing->base = g_steal_pointer(&base);
    listing->token = g_strdup(access_token);
    listing->message = message;

    g_task_set_task_data(task, listing, (GDestroyNotify)listing_free);

    soup_session_send_and_read_async(matrix_session(), message,
                                     G_PRIORITY_DEFAULT, cancellable,
                                     on_joined_rooms, listing);
}

GPtrArray *
clawt_matrix_rooms_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
