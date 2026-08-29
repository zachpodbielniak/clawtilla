/*
 * clawt-trigger-provider.c - How one forge proves a delivery is its own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "trigger/clawt-trigger-provider.h"

#include <json-glib/json-glib.h>

#include <string.h>

G_DEFINE_INTERFACE(ClawtTriggerHandler, clawt_trigger_handler, G_TYPE_OBJECT)

static void
clawt_trigger_handler_default_init(ClawtTriggerHandlerInterface *iface)
{
    /*
     * Deliberately empty.  Every vfunc stays NULL, and the wrappers
     * below refuse when one is -- rather than a default that answers
     * TRUE, which for verify() would be an endpoint anybody can call.
     */
    (void)iface;
}

/* ── Headers ─────────────────────────────────────────────────────── */

GHashTable *
clawt_trigger_headers_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void
clawt_trigger_headers_add(GHashTable  *headers,
                          const gchar *name,
                          const gchar *value)
{
    g_return_if_fail(headers != NULL);
    g_return_if_fail(name != NULL);

    if (value == NULL)
        return;

    g_hash_table_insert(headers, g_ascii_strdown(name, -1),
                        g_strdup(value));
}

static const gchar *
header(GHashTable *headers, const gchar *name)
{
    if (headers == NULL)
        return NULL;

    return g_hash_table_lookup(headers, name);
}

/* ── Shared verification ─────────────────────────────────────────── */

/*
 * Whether @presented is an HMAC-SHA256 of @body under @secret.
 *
 * The digest is taken over the bytes as they arrived.  Anything that has
 * been parsed and re-serialised has had its key order, whitespace and
 * number formatting decided by the serialiser rather than by the sender,
 * so it is a different message and hashes differently -- and the failure
 * looks exactly like a wrong secret.
 *
 * The presented hex is lowercased before comparison. That transformation
 * depends only on what the caller sent, never on the secret, so it
 * cannot be timed for information about the digest.
 */
static gboolean
hmac_matches(const gchar  *secret,
             const guchar *body,
             gsize         body_length,
             const gchar  *presented,
             const gchar  *prefix)
{
    g_autofree gchar *expected = NULL;
    g_autofree gchar *offered = NULL;

    if (secret == NULL || *secret == '\0' || presented == NULL)
        return FALSE;

    if (prefix != NULL) {
        if (!g_str_has_prefix(presented, prefix))
            return FALSE;

        presented += strlen(prefix);
    }

    offered = g_ascii_strdown(presented, -1);

    expected = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
                                       (const guchar *)secret,
                                       strlen(secret),
                                       body != NULL ? body
                                                    : (const guchar *)"",
                                       body_length);

    if (expected == NULL)
        return FALSE;

    return clawt_secure_equals(expected, offered);
}

/*
 * The one refusal every handler gives, so they cannot drift apart.
 *
 * It says the signature did not match and nothing else: which header was
 * missing, how long the secret is, and whether the trigger exists are
 * all things a caller learns by being told, and none of them are things
 * an unauthenticated caller should learn.
 */
static gboolean
refuse(GError **error)
{
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "the delivery did not prove it knows the secret");

    return FALSE;
}

/* ── Shared normalisation ────────────────────────────────────────── */

static JsonObject *
body_object(JsonParser **out_parser, const guchar *body, gsize body_length)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;

    *out_parser = parser;

    if (body == NULL || body_length == 0 ||
        !json_parser_load_from_data(parser, (const gchar *)body,
                                    (gssize)body_length, NULL))
        return NULL;

    root = json_parser_get_root(parser);

    /*
     * A node can hold the object *type* and no object, so the type check
     * is not a pointer check -- json_node_get_object() still answers
     * NULL and every read below would be a critical.
     */
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;

    return json_node_get_object(root);
}

/* A member of a member, without asserting on anything absent. */
static const gchar *
nested(JsonObject *object, const gchar *outer, const gchar *inner)
{
    JsonObject *child;

    if (object == NULL || !json_object_has_member(object, outer))
        return NULL;

    child = json_object_get_object_member(object, outer);

    if (child == NULL || !json_object_has_member(child, inner))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(child, inner)) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(child, inner);
}

static const gchar *
plain(JsonObject *object, const gchar *name)
{
    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    if (json_node_get_value_type(json_object_get_member(object, name)) !=
        G_TYPE_STRING)
        return NULL;

    return json_object_get_string_member(object, name);
}

/*
 * A number that may be spelled as a number or as a string.
 *
 * Forges disagree: GitHub sends `number` as an integer and GitLab sends
 * `iid` as one, while some payloads quote it. A placeholder is text
 * either way, so both are rendered here rather than at the template.
 */
static gchar *
number_of(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(node));

    if (json_node_get_value_type(node) == G_TYPE_INT64)
        return g_strdup_printf("%" G_GINT64_FORMAT,
                               json_node_get_int(node));

    return NULL;
}

static gchar *
nested_number(JsonObject *object, const gchar *outer, const gchar *inner)
{
    JsonObject *child;

    if (object == NULL || !json_object_has_member(object, outer))
        return NULL;

    child = json_object_get_object_member(object, outer);

    return number_of(child, inner);
}

/*
 * The shape GitHub defined and Forgejo and Gitea both follow.
 *
 * One function for three forges because the payloads really are the
 * same; where they diverge is in the headers and the signature, which is
 * where the three handlers differ.
 */
static ClawtTriggerEvent *
normalise_github_shaped(ClawtTriggerProvider  provider,
                        GHashTable           *headers,
                        const gchar          *event_header,
                        const gchar          *delivery_header,
                        const guchar         *body,
                        gsize                 body_length)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *object;
    ClawtTriggerEvent *event;

    object = body_object(&parser, body, body_length);

    event = clawt_trigger_event_new(provider, header(headers, event_header),
                                    header(headers, delivery_header));

    clawt_trigger_event_set_payload_bytes(event, body, body_length);

    if (object == NULL)
        return event;

    clawt_trigger_event_set_repo(event,
                                 nested(object, "repository", "full_name"));
    clawt_trigger_event_set_ref(event, plain(object, "ref"));

    /*
     * The pusher first and the sender second. On a push they are the
     * same person often enough that either would look right, and
     * different exactly when somebody pushed on another's behalf --
     * which is the case an operator reading the prompt wants named
     * correctly.
     */
    {
        const gchar *actor = nested(object, "pusher", "name");

        if (actor == NULL)
            actor = nested(object, "sender", "login");

        clawt_trigger_event_set_actor(event, actor);
    }

    {
        const gchar *title = nested(object, "pull_request", "title");
        const gchar *url = nested(object, "pull_request", "html_url");
        g_autofree gchar *number = nested_number(object, "pull_request",
                                                 "number");

        if (title == NULL) {
            title = nested(object, "issue", "title");
            url = nested(object, "issue", "html_url");
            g_free(g_steal_pointer(&number));
            number = nested_number(object, "issue", "number");
        }

        if (title == NULL)
            title = nested(object, "release", "name");

        if (url == NULL)
            url = plain(object, "compare_url");

        if (url == NULL)
            url = plain(object, "compare");

        if (number == NULL)
            number = number_of(object, "number");

        clawt_trigger_event_set_title(event, title);
        clawt_trigger_event_set_url(event, url);
        clawt_trigger_event_set_number(event, number);
    }

    return event;
}

/* ── Forgejo ─────────────────────────────────────────────────────── */

#define CLAWT_TYPE_FORGEJO_HANDLER (clawt_forgejo_handler_get_type())
G_DECLARE_FINAL_TYPE(ClawtForgejoHandler, clawt_forgejo_handler,
                     CLAWT, FORGEJO_HANDLER, GObject)

struct _ClawtForgejoHandler { GObject parent_instance; };

static gboolean
forgejo_verify(ClawtTriggerHandler  *self,
               const gchar          *secret,
               GHashTable           *headers,
               const guchar         *body,
               gsize                 body_length,
               GError              **error)
{
    (void)self;

    /*
     * Forgejo signs the raw body with HMAC-SHA256 and sends the hex
     * bare, with no algorithm prefix. It also emits GitHub- and
     * Gitea-shaped headers for compatibility, so its own header is tried
     * first and the others are accepted as the same digest rather than
     * as a different scheme -- they carry the same bytes.
     */
    if (hmac_matches(secret, body, body_length,
                     header(headers, "x-forgejo-signature"), NULL) ||
        hmac_matches(secret, body, body_length,
                     header(headers, "x-gitea-signature"), NULL) ||
        hmac_matches(secret, body, body_length,
                     header(headers, "x-hub-signature-256"), "sha256="))
        return TRUE;

    return refuse(error);
}

static gchar *
forgejo_event_name(ClawtTriggerHandler *self, GHashTable *headers)
{
    const gchar *name = header(headers, "x-forgejo-event");

    (void)self;

    if (name == NULL)
        name = header(headers, "x-gitea-event");

    return g_strdup(name);
}

static gchar *
forgejo_delivery_id(ClawtTriggerHandler *self, GHashTable *headers)
{
    const gchar *id = header(headers, "x-forgejo-delivery");

    (void)self;

    if (id == NULL)
        id = header(headers, "x-gitea-delivery");

    return g_strdup(id);
}

static ClawtTriggerEvent *
forgejo_normalise(ClawtTriggerHandler *self, GHashTable *headers,
                  const guchar *body, gsize body_length)
{
    ClawtTriggerEvent *event;
    g_autofree gchar *name = forgejo_event_name(self, headers);
    g_autofree gchar *delivery = forgejo_delivery_id(self, headers);

    event = normalise_github_shaped(CLAWT_TRIGGER_PROVIDER_FORGEJO, headers,
                                    "x-forgejo-event", "x-forgejo-delivery",
                                    body, body_length);

    clawt_trigger_event_set_identity(event, name, delivery);

    return event;
}

static void
clawt_forgejo_handler_iface_init(ClawtTriggerHandlerInterface *iface)
{
    iface->verify = forgejo_verify;
    iface->event_name = forgejo_event_name;
    iface->delivery_id = forgejo_delivery_id;
    iface->normalise = forgejo_normalise;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtForgejoHandler, clawt_forgejo_handler, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TRIGGER_HANDLER,
                          clawt_forgejo_handler_iface_init))

static void clawt_forgejo_handler_init(ClawtForgejoHandler *self) { (void)self; }
static void
clawt_forgejo_handler_class_init(ClawtForgejoHandlerClass *klass)
{
    (void)klass;
}

/* ── Gitea ───────────────────────────────────────────────────────── */

#define CLAWT_TYPE_GITEA_HANDLER (clawt_gitea_handler_get_type())
G_DECLARE_FINAL_TYPE(ClawtGiteaHandler, clawt_gitea_handler,
                     CLAWT, GITEA_HANDLER, GObject)

struct _ClawtGiteaHandler { GObject parent_instance; };

static gboolean
gitea_verify(ClawtTriggerHandler  *self,
             const gchar          *secret,
             GHashTable           *headers,
             const guchar         *body,
             gsize                 body_length,
             GError              **error)
{
    (void)self;

    /* The same scheme as Forgejo, under its own header. */
    if (hmac_matches(secret, body, body_length,
                     header(headers, "x-gitea-signature"), NULL))
        return TRUE;

    return refuse(error);
}

static gchar *
gitea_event_name(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    return g_strdup(header(headers, "x-gitea-event"));
}

static gchar *
gitea_delivery_id(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    return g_strdup(header(headers, "x-gitea-delivery"));
}

static ClawtTriggerEvent *
gitea_normalise(ClawtTriggerHandler *self, GHashTable *headers,
                const guchar *body, gsize body_length)
{
    (void)self;

    return normalise_github_shaped(CLAWT_TRIGGER_PROVIDER_GITEA, headers,
                                   "x-gitea-event", "x-gitea-delivery",
                                   body, body_length);
}

static void
clawt_gitea_handler_iface_init(ClawtTriggerHandlerInterface *iface)
{
    iface->verify = gitea_verify;
    iface->event_name = gitea_event_name;
    iface->delivery_id = gitea_delivery_id;
    iface->normalise = gitea_normalise;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtGiteaHandler, clawt_gitea_handler, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TRIGGER_HANDLER,
                          clawt_gitea_handler_iface_init))

static void clawt_gitea_handler_init(ClawtGiteaHandler *self) { (void)self; }
static void
clawt_gitea_handler_class_init(ClawtGiteaHandlerClass *klass)
{
    (void)klass;
}

/* ── GitHub ──────────────────────────────────────────────────────── */

#define CLAWT_TYPE_GITHUB_HANDLER (clawt_github_handler_get_type())
G_DECLARE_FINAL_TYPE(ClawtGithubHandler, clawt_github_handler,
                     CLAWT, GITHUB_HANDLER, GObject)

struct _ClawtGithubHandler { GObject parent_instance; };

static gboolean
github_verify(ClawtTriggerHandler  *self,
              const gchar          *secret,
              GHashTable           *headers,
              const guchar         *body,
              gsize                 body_length,
              GError              **error)
{
    (void)self;

    /*
     * The same digest as Forgejo and Gitea, behind `sha256=`.  The
     * prefix is required rather than optional: accepting a bare hex here
     * would mean this handler also accepted a Forgejo delivery, and the
     * point of naming the provider is that it does not.
     *
     * `X-Hub-Signature` -- the SHA-1 one -- is deliberately not read.
     * GitHub still sends it and it is long dead.
     */
    if (hmac_matches(secret, body, body_length,
                     header(headers, "x-hub-signature-256"), "sha256="))
        return TRUE;

    return refuse(error);
}

static gchar *
github_event_name(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    return g_strdup(header(headers, "x-github-event"));
}

static gchar *
github_delivery_id(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    return g_strdup(header(headers, "x-github-delivery"));
}

static ClawtTriggerEvent *
github_normalise(ClawtTriggerHandler *self, GHashTable *headers,
                 const guchar *body, gsize body_length)
{
    (void)self;

    return normalise_github_shaped(CLAWT_TRIGGER_PROVIDER_GITHUB, headers,
                                   "x-github-event", "x-github-delivery",
                                   body, body_length);
}

static void
clawt_github_handler_iface_init(ClawtTriggerHandlerInterface *iface)
{
    iface->verify = github_verify;
    iface->event_name = github_event_name;
    iface->delivery_id = github_delivery_id;
    iface->normalise = github_normalise;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtGithubHandler, clawt_github_handler, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TRIGGER_HANDLER,
                          clawt_github_handler_iface_init))

static void clawt_github_handler_init(ClawtGithubHandler *self) { (void)self; }
static void
clawt_github_handler_class_init(ClawtGithubHandlerClass *klass)
{
    (void)klass;
}

/* ── GitLab ──────────────────────────────────────────────────────── */

#define CLAWT_TYPE_GITLAB_HANDLER (clawt_gitlab_handler_get_type())
G_DECLARE_FINAL_TYPE(ClawtGitlabHandler, clawt_gitlab_handler,
                     CLAWT, GITLAB_HANDLER, GObject)

struct _ClawtGitlabHandler { GObject parent_instance; };

static gboolean
gitlab_verify(ClawtTriggerHandler  *self,
              const gchar          *secret,
              GHashTable           *headers,
              const guchar         *body,
              gsize                 body_length,
              GError              **error)
{
    const gchar *token = header(headers, "x-gitlab-token");

    (void)self;
    (void)body;
    (void)body_length;

    /*
     * GitLab signs nothing.  It sends the shared secret itself, in the
     * clear, and the check is a string comparison -- which is precisely
     * why it has to be a constant-time one: an endpoint that can be
     * called repeatedly and answers a little faster for each correct
     * leading byte hands the secret over a few hundred requests.
     *
     * An empty configured secret is refused by clawt_secure_equals()
     * only if the header is also absent, so it is refused here too:
     * `secret: ""` must not mean "anybody may call this".
     */
    if (secret == NULL || *secret == '\0')
        return refuse(error);

    if (clawt_secure_equals(secret, token))
        return TRUE;

    /*
     * Standard Webhooks, which newer GitLab can send instead. The same
     * secret, this time as an HMAC over the id, the timestamp and the
     * body -- so a replay carries its own age.
     */
    {
        const gchar *signature = header(headers, "webhook-signature");
        const gchar *id = header(headers, "webhook-id");
        const gchar *stamp = header(headers, "webhook-timestamp");

        if (signature != NULL && id != NULL && stamp != NULL) {
            g_autofree gchar *signed_payload = NULL;
            g_autofree gchar *expected = NULL;
            g_auto(GStrv) offered = NULL;
            guint i;

            signed_payload = g_strdup_printf(
                "%s.%s.%.*s", id, stamp, (int)body_length,
                body != NULL ? (const gchar *)body : "");

            expected = g_compute_hmac_for_data(
                G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret),
                (const guchar *)signed_payload, strlen(signed_payload));

            /*
             * The header carries a space-separated list, because a
             * rotation publishes both secrets at once. Every candidate
             * is compared; the loop does not stop at the first match,
             * so the number of comparisons does not depend on which one
             * matched.
             */
            offered = g_strsplit(signature, " ", -1);

            for (i = 0; offered != NULL && offered[i] != NULL; i++) {
                const gchar *hex = offered[i];
                g_autofree gchar *decoded = NULL;
                gsize decoded_length = 0;

                if (g_str_has_prefix(hex, "v1,"))
                    hex += strlen("v1,");

                decoded = (gchar *)g_base64_decode(hex, &decoded_length);

                if (decoded == NULL || decoded_length == 0)
                    continue;

                {
                    g_autofree gchar *as_hex =
                        g_malloc0(decoded_length * 2 + 1);
                    gsize k;

                    for (k = 0; k < decoded_length; k++)
                        g_snprintf(as_hex + (k * 2), 3, "%02x",
                                   (guchar)decoded[k]);

                    if (clawt_secure_equals(expected, as_hex))
                        return TRUE;
                }
            }
        }
    }

    return refuse(error);
}

static gchar *
gitlab_event_name(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    /*
     * GitLab's event names are prose -- "Push Hook", "Merge Request
     * Hook" -- and they are kept as sent. Rewriting them to look like
     * GitHub's would mean an allowlist copied from GitLab's own
     * documentation matched nothing.
     */
    return g_strdup(header(headers, "x-gitlab-event"));
}

static gchar *
gitlab_delivery_id(ClawtTriggerHandler *self, GHashTable *headers)
{
    const gchar *id = header(headers, "x-gitlab-event-uuid");

    (void)self;

    if (id == NULL)
        id = header(headers, "webhook-id");

    return g_strdup(id);
}

static ClawtTriggerEvent *
gitlab_normalise(ClawtTriggerHandler *self, GHashTable *headers,
                 const guchar *body, gsize body_length)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *object;
    ClawtTriggerEvent *event;
    g_autofree gchar *name = gitlab_event_name(self, headers);
    g_autofree gchar *delivery = gitlab_delivery_id(self, headers);

    object = body_object(&parser, body, body_length);

    event = clawt_trigger_event_new(CLAWT_TRIGGER_PROVIDER_GITLAB, name,
                                    delivery);
    clawt_trigger_event_set_payload_bytes(event, body, body_length);

    if (object == NULL)
        return event;

    clawt_trigger_event_set_repo(
        event, nested(object, "project", "path_with_namespace"));
    clawt_trigger_event_set_ref(event, plain(object, "ref"));

    {
        const gchar *actor = plain(object, "user_username");

        if (actor == NULL)
            actor = plain(object, "user_name");

        if (actor == NULL)
            actor = nested(object, "user", "username");

        clawt_trigger_event_set_actor(event, actor);
    }

    {
        g_autofree gchar *number = nested_number(object, "object_attributes",
                                                 "iid");

        clawt_trigger_event_set_title(
            event, nested(object, "object_attributes", "title"));
        clawt_trigger_event_set_url(
            event, nested(object, "object_attributes", "url"));
        clawt_trigger_event_set_number(event, number);
    }

    return event;
}

static void
clawt_gitlab_handler_iface_init(ClawtTriggerHandlerInterface *iface)
{
    iface->verify = gitlab_verify;
    iface->event_name = gitlab_event_name;
    iface->delivery_id = gitlab_delivery_id;
    iface->normalise = gitlab_normalise;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtGitlabHandler, clawt_gitlab_handler, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TRIGGER_HANDLER,
                          clawt_gitlab_handler_iface_init))

static void clawt_gitlab_handler_init(ClawtGitlabHandler *self) { (void)self; }
static void
clawt_gitlab_handler_class_init(ClawtGitlabHandlerClass *klass)
{
    (void)klass;
}

/* ── Generic ─────────────────────────────────────────────────────── */

#define CLAWT_TYPE_GENERIC_HANDLER (clawt_generic_handler_get_type())
G_DECLARE_FINAL_TYPE(ClawtGenericHandler, clawt_generic_handler,
                     CLAWT, GENERIC_HANDLER, GObject)

struct _ClawtGenericHandler { GObject parent_instance; };

static gboolean
generic_verify(ClawtTriggerHandler  *self,
               const gchar          *secret,
               GHashTable           *headers,
               const guchar         *body,
               gsize                 body_length,
               GError              **error)
{
    const gchar *authorization = header(headers, "authorization");

    (void)self;
    (void)body;
    (void)body_length;

    if (secret == NULL || *secret == '\0')
        return refuse(error);

    /*
     * A bearer token, and also the bare token under `X-Clawtilla-Token`
     * -- podomation, curl and a shell script all reach for a plain
     * header before they reach for an Authorization one, and this
     * provider exists precisely for callers that are not a forge.
     */
    if (authorization != NULL &&
        g_ascii_strncasecmp(authorization, "Bearer ", 7) == 0 &&
        clawt_secure_equals(secret, authorization + 7))
        return TRUE;

    if (clawt_secure_equals(secret, header(headers, "x-clawtilla-token")))
        return TRUE;

    /*
     * And the same HMAC the forges use, for a caller that would rather
     * sign than send. Offered rather than required, since a generic
     * sender that could compute an HMAC would usually be a forge.
     */
    if (hmac_matches(secret, body, body_length,
                     header(headers, "x-clawtilla-signature"), NULL))
        return TRUE;

    return refuse(error);
}

static gchar *
generic_event_name(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    /*
     * The configured header is applied by the caller, which is the only
     * place that knows the trigger. Here is the default, so a sender
     * that names nothing still has an event to be filtered on.
     */
    return g_strdup(header(headers, "x-clawtilla-event"));
}

static gchar *
generic_delivery_id(ClawtTriggerHandler *self, GHashTable *headers)
{
    (void)self;

    return g_strdup(header(headers, "x-clawtilla-delivery"));
}

static ClawtTriggerEvent *
generic_normalise(ClawtTriggerHandler *self, GHashTable *headers,
                  const guchar *body, gsize body_length)
{
    g_autoptr(JsonParser) parser = NULL;
    JsonObject *object;
    ClawtTriggerEvent *event;
    g_autofree gchar *name = generic_event_name(self, headers);
    g_autofree gchar *delivery = generic_delivery_id(self, headers);

    object = body_object(&parser, body, body_length);

    event = clawt_trigger_event_new(CLAWT_TRIGGER_PROVIDER_GENERIC, name,
                                    delivery);
    clawt_trigger_event_set_payload_bytes(event, body, body_length);

    if (object == NULL)
        return event;

    /*
     * Best effort, and only from names a sender would have chosen on
     * purpose. Guessing harder would fill a template with whatever
     * happened to be in the body, which reads to an agent as fact.
     */
    clawt_trigger_event_set_repo(event, plain(object, "repo"));
    clawt_trigger_event_set_ref(event, plain(object, "ref"));
    clawt_trigger_event_set_actor(event, plain(object, "actor"));
    clawt_trigger_event_set_title(event, plain(object, "title"));
    clawt_trigger_event_set_url(event, plain(object, "url"));

    {
        g_autofree gchar *number = number_of(object, "number");

        clawt_trigger_event_set_number(event, number);
    }

    return event;
}

static void
clawt_generic_handler_iface_init(ClawtTriggerHandlerInterface *iface)
{
    iface->verify = generic_verify;
    iface->event_name = generic_event_name;
    iface->delivery_id = generic_delivery_id;
    iface->normalise = generic_normalise;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtGenericHandler, clawt_generic_handler, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_TRIGGER_HANDLER,
                          clawt_generic_handler_iface_init))

static void clawt_generic_handler_init(ClawtGenericHandler *self) { (void)self; }
static void
clawt_generic_handler_class_init(ClawtGenericHandlerClass *klass)
{
    (void)klass;
}

/* ── The wrappers ────────────────────────────────────────────────── */

ClawtTriggerHandler *
clawt_trigger_handler_for(ClawtTriggerProvider provider)
{
    static ClawtTriggerHandler *handlers[5];
    static gsize once = 0;

    if (g_once_init_enter(&once)) {
        handlers[CLAWT_TRIGGER_PROVIDER_GENERIC] =
            g_object_new(CLAWT_TYPE_GENERIC_HANDLER, NULL);
        handlers[CLAWT_TRIGGER_PROVIDER_FORGEJO] =
            g_object_new(CLAWT_TYPE_FORGEJO_HANDLER, NULL);
        handlers[CLAWT_TRIGGER_PROVIDER_GITEA] =
            g_object_new(CLAWT_TYPE_GITEA_HANDLER, NULL);
        handlers[CLAWT_TRIGGER_PROVIDER_GITHUB] =
            g_object_new(CLAWT_TYPE_GITHUB_HANDLER, NULL);
        handlers[CLAWT_TRIGGER_PROVIDER_GITLAB] =
            g_object_new(CLAWT_TYPE_GITLAB_HANDLER, NULL);

        g_once_init_leave(&once, 1);
    }

    if ((guint)provider >= G_N_ELEMENTS(handlers))
        return handlers[CLAWT_TRIGGER_PROVIDER_GENERIC];

    return handlers[provider];
}

gboolean
clawt_trigger_handler_verify(ClawtTriggerHandler  *self,
                             const gchar          *secret,
                             GHashTable           *headers,
                             const guchar         *body,
                             gsize                 body_length,
                             GError              **error)
{
    ClawtTriggerHandlerInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_HANDLER(self), FALSE);

    iface = CLAWT_TRIGGER_HANDLER_GET_IFACE(self);

    /*
     * A missing vfunc refuses and says whose it is.  Answering TRUE
     * would be an endpoint that starts an agent for anybody who finds
     * it, which is the worst thing in this file to get wrong.
     */
    if (iface->verify == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s cannot authenticate a delivery",
                    G_OBJECT_TYPE_NAME(self));
        return FALSE;
    }

    return iface->verify(self, secret, headers, body, body_length, error);
}

gchar *
clawt_trigger_handler_event_name(ClawtTriggerHandler *self,
                                 GHashTable          *headers)
{
    ClawtTriggerHandlerInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_HANDLER(self), NULL);

    iface = CLAWT_TRIGGER_HANDLER_GET_IFACE(self);

    if (iface->event_name == NULL)
        return NULL;

    return iface->event_name(self, headers);
}

gchar *
clawt_trigger_handler_delivery_id(ClawtTriggerHandler *self,
                                  GHashTable          *headers)
{
    ClawtTriggerHandlerInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_HANDLER(self), NULL);

    iface = CLAWT_TRIGGER_HANDLER_GET_IFACE(self);

    if (iface->delivery_id == NULL)
        return NULL;

    return iface->delivery_id(self, headers);
}

ClawtTriggerEvent *
clawt_trigger_handler_normalise(ClawtTriggerHandler *self,
                                GHashTable          *headers,
                                const guchar        *body,
                                gsize                body_length)
{
    ClawtTriggerHandlerInterface *iface;

    g_return_val_if_fail(CLAWT_IS_TRIGGER_HANDLER(self), NULL);

    iface = CLAWT_TRIGGER_HANDLER_GET_IFACE(self);

    if (iface->normalise == NULL)
        return NULL;

    return iface->normalise(self, headers, body, body_length);
}

gboolean
clawt_trigger_sniff_provider(GHashTable           *headers,
                             ClawtTriggerProvider *out_provider)
{
    g_return_val_if_fail(out_provider != NULL, FALSE);

    if (headers == NULL)
        return FALSE;

    /*
     * Most specific first.  Forgejo sends `X-Forgejo-Event` *and*
     * `X-Gitea-Event` *and* `X-GitHub-Event`, so testing GitHub's header
     * first would identify every Forgejo delivery as GitHub -- and then
     * demand a `sha256=` prefix Forgejo does not send, so the delivery
     * would be refused with a message about the signature rather than
     * about the provider.
     */
    if (header(headers, "x-forgejo-event") != NULL) {
        *out_provider = CLAWT_TRIGGER_PROVIDER_FORGEJO;
        return TRUE;
    }

    if (header(headers, "x-gitea-event") != NULL) {
        *out_provider = CLAWT_TRIGGER_PROVIDER_GITEA;
        return TRUE;
    }

    if (header(headers, "x-github-event") != NULL) {
        *out_provider = CLAWT_TRIGGER_PROVIDER_GITHUB;
        return TRUE;
    }

    if (header(headers, "x-gitlab-event") != NULL) {
        *out_provider = CLAWT_TRIGGER_PROVIDER_GITLAB;
        return TRUE;
    }

    return FALSE;
}

/* ── A secret in the URL ─────────────────────────────────────────── */

gboolean
clawt_trigger_provider_accepts_url_secret(ClawtTriggerProvider provider)
{
    /*
     * Named exhaustively rather than defaulted, so -Wswitch says
     * something the next time a provider is added.  The permissive
     * answer must never be the one a new value falls into: a forge that
     * arrived while nobody was looking would silently start accepting a
     * query string in place of a signature.
     */
    switch (provider) {
    case CLAWT_TRIGGER_PROVIDER_GENERIC:
        return TRUE;

    case CLAWT_TRIGGER_PROVIDER_FORGEJO:
    case CLAWT_TRIGGER_PROVIDER_GITEA:
    case CLAWT_TRIGGER_PROVIDER_GITHUB:
    case CLAWT_TRIGGER_PROVIDER_GITLAB:
        return FALSE;
    }

    return FALSE;
}

gboolean
clawt_trigger_verify_url_secret(ClawtTriggerProvider   provider,
                                const gchar           *secret,
                                const gchar           *presented,
                                GError               **error)
{
    if (presented == NULL || *presented == '\0')
        return refuse(error);

    if (!clawt_trigger_provider_accepts_url_secret(provider))
        return refuse(error);

    if (secret == NULL || *secret == '\0')
        return refuse(error);

    if (!clawt_secure_equals(secret, presented))
        return refuse(error);

    return TRUE;
}
