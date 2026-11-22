/*
 * daemon-office-generate.c - A character made by the daemon itself
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The custodial team makes characters through a connector, with a model
 * deciding what to ask for.  This is the other road: with
 * `office.asset_api` naming a service whose REST API ai-glib speaks
 * (Higgsfield, AiHiggsfieldClient), the daemon makes a character with
 * no model in the loop -- one portrait, from the agent's own picture
 * when it has one, imported as a one-frame character.
 *
 * Money is asked for first, always: the service's estimate is put to
 * the operator as a decision, and nothing is submitted until a person
 * answers "approve" -- the same rule a rebuild follows, and for the same
 * reason: a charge that happened because nobody answered is the one
 * thing this must not do.  Nothing here waits on the network on the
 * daemon's loop: the estimate, the upload, the job and the download are
 * all asynchronous, on the daemon's own context.
 */

#include "clawtilla.h"
#include "clawt-daemon-private.h"
#include "office/clawt-office-pack.h"

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define GENERATE_PREFIX "office-generate-"

static void tell_operator(ClawtDaemon *self, const gchar *agent_id, const gchar *body);

/* ── The key ────────────────────────────────────────────────────── */

static gchar *
key_path(ClawtDaemon *self, const gchar *service)
{
    g_autofree gchar *secrets = clawt_config_get_path_value(self->config, "secrets.dir");
    g_autofree gchar *file = g_strdup_printf("%s.key", service);

    return g_build_filename(secrets, "office", file, NULL);
}

gchar *
clawt_daemon_office_store_api_key(ClawtDaemon  *self,
                                  const gchar  *service,
                                  const gchar  *key,
                                  GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *dir = NULL;

    if (g_strcmp0(service, "higgsfield") != 0 && g_strcmp0(service, "comfyui") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the office makes characters itself through higgsfield or comfyui "
                    "(office.asset_api)");
        return NULL;
    }
    /* a ComfyUI behind an auth proxy takes a bearer token: any one line */
    if (g_strcmp0(service, "comfyui") == 0 &&
        (key == NULL || *key == '\0' || strpbrk(key, "\r\n") != NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a ComfyUI token is one line: the bearer token its proxy expects");
        return NULL;
    }
    if (g_strcmp0(service, "higgsfield") == 0 &&
        (key == NULL || strchr(key, ':') == NULL || key[0] == ':' ||
         key[strlen(key) - 1] == ':')) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "a Higgsfield key is KEY_ID:KEY_SECRET, from console.higgsfield.ai");
        return NULL;
    }
    path = key_path(self, service);
    dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents_full(path, key, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  error))
        return NULL;
    return g_steal_pointer(&path);
}

/*
 * The connector whose sign-in pays on the MCP road: office.asset_generator
 * when it names one, else the fleet's only higgsfield connector.  Two
 * with neither named is refused rather than guessed: they may be two
 * accounts, and which one is charged is the person's to say.
 */
static gchar *
mcp_connector_name(ClawtDaemon *self, GError **error)
{
    const gchar *named = clawt_config_get_string(self->config, "office.asset_generator");
    GPtrArray *all = clawt_config_get_integrations(self->config);
    g_autofree gchar *found = NULL;
    guint count = 0;
    guint i;

    if (named != NULL && *named != '\0')
        return g_strdup(named);
    for (i = 0; all != NULL && i < all->len; i++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(all, i);

        if (g_strcmp0(clawt_integration_config_get_type_id(instance), "connector") != 0 ||
            g_strcmp0(clawt_integration_config_get_string(instance, NULL, "provider"),
                      "higgsfield") != 0)
            continue;
        count++;
        g_free(found);
        found = g_strdup(clawt_integration_config_get_name(instance));
    }
    if (count == 1)
        return g_steal_pointer(&found);
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                count == 0
                ? "office.asset_api: higgsfield-mcp needs a Higgsfield connector: "
                  "clawtilla connector add hf --provider higgsfield"
                : "there are %u Higgsfield connectors: name the one that pays with "
                  "office.asset_generator", count);
    return NULL;
}

/*
 * The generator: the test's when it set one, else a Higgsfield client
 * with the stored key.  The key is read here, never kept, and never in
 * a reply or a log.
 */
static AiMediaGenerator *
generator_for(ClawtDaemon *self, GError **error)
{
    const gchar *service = clawt_config_get_string(self->config, "office.asset_api");
    g_autofree gchar *path = NULL;
    g_autofree gchar *key = NULL;
    g_auto(GStrv) parts = NULL;

    if (self->office_generator_factory != NULL)
        return self->office_generator_factory(self);

    /*
     * A self-hosted ComfyUI: the FOSS road.  The workflow is the
     * person's own, exported in API format with {{prompt}} (and
     * {{image}}, {{seed}}) where the office's values go; a token only
     * when an auth proxy sits in front of it.
     */
    if (g_strcmp0(service, "comfyui") == 0) {
        g_autofree gchar *workflow_path = clawt_config_get_path_value(self->config,
                                                                      "office.comfyui_workflow");
        g_autofree gchar *workflow = NULL;
        g_autoptr(AiComfyuiClient) client = ai_comfyui_client_new(
            clawt_config_get_string(self->config, "office.comfyui_url"));
        g_autofree gchar *token = NULL;

        if (workflow_path == NULL || !g_file_get_contents(workflow_path, &workflow, NULL, NULL)) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                                "office.comfyui_workflow must name a workflow exported in "
                                "API format (docs/office-custodial-team.org)");
            return NULL;
        }
        if (!ai_comfyui_client_set_workflow(client, workflow, error))
            return NULL;
        path = key_path(self, service);
        if (g_file_get_contents(path, &token, NULL, NULL)) {
            g_strstrip(token);
            ai_comfyui_client_set_token(client, token);
            memset(token, 0, strlen(token));
        }
        return AI_MEDIA_GENERATOR(g_steal_pointer(&client));
    }
    /*
     * Higgsfield through a connector's sign-in: the account's app
     * credits, where the REST road below spends a separate API balance.
     */
    if (g_strcmp0(service, "higgsfield-mcp") == 0) {
        g_autofree gchar *name = mcp_connector_name(self, error);
        g_autoptr(ClawtConnectorPlan) plan = NULL;
        g_autoptr(ClawtMcpHttpClient) client = NULL;
        ClawtHiggsfieldMcp *generator;

        if (name == NULL)
            return NULL;
        plan = clawt_connector_plan_for_name(self->config, name, NULL, error);
        if (plan == NULL)
            return NULL;
        if (plan->url == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "the connector '%s' is not a server reached by URL", name);
            return NULL;
        }
        if (plan->header_value == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "the connector '%s' is not signed in: clawtilla connector connect %s",
                        name, name);
            return NULL;
        }
        client = clawt_mcp_http_client_new(name, plan->url, plan->header_name,
                                           plan->header_value);
        generator = clawt_higgsfield_mcp_new(client);
        clawt_higgsfield_mcp_set_connector(generator, name);
        return AI_MEDIA_GENERATOR(generator);
    }
    if (g_strcmp0(service, "higgsfield") != 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no service is set to make characters directly "
                            "(office.asset_api: comfyui, higgsfield or higgsfield-mcp)");
        return NULL;
    }
    path = key_path(self, service);
    if (!g_file_get_contents(path, &key, NULL, NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "no Higgsfield key is stored: `clawtilla office api-key higgsfield`");
        return NULL;
    }
    g_strstrip(key);
    parts = g_strsplit(key, ":", 2);
    if (parts[0] == NULL || parts[1] == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                            "the stored Higgsfield key is not KEY_ID:KEY_SECRET; store it again");
        return NULL;
    }
    return AI_MEDIA_GENERATOR(ai_higgsfield_client_new_with_credentials(parts[0], parts[1]));
}

/*
 * The agent's own picture, as bytes, with the name it is uploaded under
 * ("clawtilla-<agent>.<ext>").  Any format the avatar code accepts, from
 * agents.avatar or the auto-detected profile picture alike: a PNG
 * named in the config was the only one ever read, so a picture set from
 * a client -- written to the workspace, not the config -- was never the
 * likeness a character was made from.  FALSE, quietly, for an agent with
 * no picture.
 */
static gboolean
agent_picture(ClawtAgentConfig *agent, GBytes **out_bytes, gchar **out_name,
              gchar **out_mime)
{
    g_autofree gchar *path = clawt_daemon_office_agent_picture(agent);
    g_autofree gchar *data = NULL;
    const gchar *mime;
    const gchar *ext;
    gsize length = 0;

    if (path == NULL || !g_file_get_contents(path, &data, &length, NULL))
        return FALSE;
    mime = clawt_avatar_sniff_mime_type((const guchar *)data, length);
    ext = clawt_avatar_extension_for_mime_type(mime);
    if (mime == NULL || ext == NULL)
        return FALSE;
    if (out_name != NULL)
        *out_name = g_strdup_printf("clawtilla-%s.%s", clawt_agent_config_get_id(agent), ext);
    if (out_mime != NULL)
        *out_mime = g_strdup(mime);
    if (out_bytes != NULL)
        *out_bytes = g_bytes_new_take(g_steal_pointer(&data), length);
    return TRUE;
}

/* The service's name as the card and the reply say it. */
static const gchar *
service_label(AiMediaGenerator *generator)
{
    if (AI_IS_COMFYUI_CLIENT(generator))
        return "ComfyUI";
    if (AI_IS_HIGGSFIELD_CLIENT(generator) || CLAWT_IS_HIGGSFIELD_MCP(generator))
        return "Higgsfield";
    return G_OBJECT_TYPE_NAME(generator);
}

/*
 * Which money pays, as the card says it.  Higgsfield bills its two
 * roads separately, so "your Higgsfield balance" would not say which.
 */
static gchar *
balance_label(AiMediaGenerator *generator)
{
    if (CLAWT_IS_HIGGSFIELD_MCP(generator))
        return g_strdup_printf("Higgsfield app credits (the account signed in to the "
                               "connector '%s')",
                               clawt_higgsfield_mcp_get_connector(
                                   CLAWT_HIGGSFIELD_MCP(generator)));
    if (AI_IS_HIGGSFIELD_CLIENT(generator))
        return g_strdup("Higgsfield API balance (console.higgsfield.ai)");
    return g_strdup_printf("%s balance", service_label(generator));
}

/* Where the remembered estimates live: per agent, until the job ends. */
static GHashTable *
estimates(ClawtDaemon *self)
{
    GHashTable *table = g_object_get_data(G_OBJECT(self), "clawt-office-estimates");

    if (table == NULL) {
        table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_object_set_data_full(G_OBJECT(self), "clawt-office-estimates", table,
                               (GDestroyNotify)g_hash_table_unref);
    }
    return table;
}

/* What is asked for: the agent, as the sidebar knows it, as a person. */
static AiMediaRequest *
request_for(ClawtDaemon *self, ClawtAgentConfig *agent)
{
    const gchar *id = clawt_agent_config_get_id(agent);
    const gchar *name = clawt_agent_config_get_string(agent, "name");
    const gchar *description = clawt_agent_config_get_string(agent, "description");
    const gchar *color = clawt_avatar_color(id, name, clawt_agent_config_get_string(agent,
                                                                                    "color"));
    g_autofree gchar *prompt = g_strdup_printf(
        "A full-body character for a cosy top-down office game: %s%s%s.  Standing, facing "
        "the viewer, friendly, simple clean shapes, a plain light background.  Their main "
        "colour is %s.",
        name != NULL ? name : id,
        description != NULL && *description != '\0' ? ", who " : "",
        description != NULL ? description : "", color);
    const gchar *model = clawt_config_get_string(self->config, "office.asset_model");
    /* each road's own spelling: one's model name is refused by the other */
    const gchar *fallback = g_strcmp0(clawt_config_get_string(self->config, "office.asset_api"),
                                      "higgsfield-mcp") == 0
        ? CLAWT_HIGGSFIELD_MCP_DEFAULT_MODEL : AI_HIGGSFIELD_MODEL_SOUL_V2;

    return ai_media_request_new(model != NULL && *model != '\0' ? model : fallback, prompt);
}

/* ── Asking ─────────────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon *self;
    gchar       *agent;
    gchar       *model;
    gchar       *service;
    gboolean     self_hosted;
} Ask;

static void
ask_free(Ask *ask)
{
    g_object_unref(ask->self);
    g_free(ask->agent);
    g_free(ask->model);
    g_free(ask->service);
    g_free(ask);
}

static void
on_estimate(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Ask *ask = user_data;
    ClawtDaemon *self = ask->self;
    static const gchar *const options[] = { "approve", "decline", NULL };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDecision) decision = NULL;
    g_autofree gchar *uuid = g_uuid_string_random();
    g_autofree gchar *id = g_strconcat(GENERATE_PREFIX, uuid, NULL);
    g_autofree gchar *cost = NULL;
    g_autofree gchar *question = NULL;
    gint64 micro = -1;

    if (ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(source), result, &micro,
                                           &error) && micro == 0 && ask->self_hosted) {
        /*
         * Free -- a self-hosted ComfyUI says so -- so there is nothing to
         * approve: the card exists to keep a model or a click from
         * spending money, and none is spent.  Only a self-hosted server
         * earns this: a paid service's zero is a rounding or free credits,
         * which are still the person's to spend.
         */
        clawt_daemon_office_generate_start(self, ask->agent);
        ask_free(ask);
        return;
    }
    if (ask->self_hosted) {
        /*
         * A self-hosted server's estimate is zero or a refusal of the
         * request -- its workflow will not take what the office fills
         * in.  A card about money would be about nothing: say why, and
         * ask nobody.
         */
        g_warning("office: %s's character was not made: %s refused the request: %s",
                  ask->agent, ask->service,
                  error != NULL ? error->message : "it named a cost");
        ask_free(ask);
        return;
    }
    if (g_error_matches(error, CLAWT_ERROR, CLAWT_ERROR_AUTH)) {
        /* signed out: a card would ask about money nobody can spend */
        g_autofree gchar *body = g_strdup_printf(
            "%s's office character was not asked for: %s.", ask->agent, error->message);

        g_message("office: %s", body);
        tell_operator(self, ask->agent, body);
        ask_free(ask);
        return;
    }
    if (error == NULL && CLAWT_IS_HIGGSFIELD_MCP(source) &&
        clawt_higgsfield_mcp_get_estimate_credits(CLAWT_HIGGSFIELD_MCP(source)) >= 0.0) {
        /* the MCP road quotes credits, the money it actually spends */
        cost = g_strdup_printf("%g credits",
                               clawt_higgsfield_mcp_get_estimate_credits(
                                   CLAWT_HIGGSFIELD_MCP(source)));
    } else if (error != NULL || micro < 0) {
        /* no estimate is not a free generation: the card says so */
        g_message("office: no estimate for %s's character: %s", ask->agent,
                  error != NULL ? error->message : "the service gave none");
        cost = g_strdup("an amount the service would not estimate");
    } else {
        /*
         * Enough digits to be true: Higgsfield prices a Soul portrait at
         * $0.004, which "%.2f" printed as "about $0.00" -- a card saying a
         * real charge costs nothing.
         */
        cost = micro >= 10000 ? g_strdup_printf("about $%.2f", (gdouble)micro / 1e6)
                              : g_strdup_printf("about $%.4f", (gdouble)micro / 1e6);
    }
    {
        g_autofree gchar *balance = balance_label(AI_MEDIA_GENERATOR(source));

        question = g_strdup_printf(
            "Make a character for %s with %s (%s)?  It costs %s, from your %s.  "
            "Approve submits one image from %s's picture and imports it as its "
            "character; decline spends nothing.",
            ask->agent, ask->service, ask->model, cost, balance, ask->agent);
        /* remembered for the provenance: a job's status names no price */
        g_hash_table_replace(estimates(self), g_strdup(ask->agent),
                             g_strdup_printf("%s, from the %s", cost, balance));
    }
    decision = clawt_decision_new(id, ask->agent, question);
    clawt_decision_set_options(decision, options);
    /* no deadline: nothing spends money but a person's yes */
    clawt_decision_set_default(decision, "decline", "nobody approved it, so nothing was spent");
    {
        /* post answers the stored id, which is ours to free */
        g_autofree gchar *stored = clawt_decision_store_post(self->decisions, decision,
                                                             &error);

        if (stored == NULL)
            g_warning("office: could not ask about %s's character: %s", ask->agent,
                      error->message);
        else if (self->bus != NULL)
            clawt_event_bus_emit(self->bus, "decision.asked", ask->agent);
    }
    ask_free(ask);
}

gboolean
clawt_daemon_office_generate_ask(ClawtDaemon  *self,
                                 const gchar  *agent_id,
                                 gchar       **out_service,
                                 GError      **error)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(AiMediaGenerator) generator = NULL;
    g_autoptr(AiMediaRequest) request = NULL;
    Ask *ask;

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "there is no agent '%s'",
                    agent_id != NULL ? agent_id : "");
        return FALSE;
    }
    generator = generator_for(self, error);
    if (generator == NULL)
        return FALSE;

    request = request_for(self, agent);

    /*
     * A ComfyUI workflow that loads the agent's picture ({{image}}) is
     * estimated with the name the upload will give it: the estimate
     * builds the graph, and a graph missing its image is refused --
     * which is the right answer for an agent with no picture, and is
     * given here, where the person asking can still read it.
     */
    if (AI_IS_COMFYUI_CLIENT(generator)) {
        const gchar *workflow = ai_comfyui_client_get_workflow(AI_COMFYUI_CLIENT(generator));
        g_autofree gchar *name = NULL;

        if (workflow != NULL && strstr(workflow, "{{image}}") != NULL) {
            if (!agent_picture(agent, NULL, &name, NULL)) {
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the ComfyUI workflow loads the agent's picture ({{image}}) and "
                            "%s has none: give it one (`clawtilla agent avatar set %s FILE`) "
                            "or use a workflow without {{image}}", agent_id, agent_id);
                return FALSE;
            }
            ai_media_request_set_image_url(request, name);
        }
    }

    ask = g_new0(Ask, 1);
    ask->self = g_object_ref(self);
    ask->agent = g_strdup(agent_id);
    ask->model = g_strdup(ai_media_request_get_model(request));
    ask->service = g_strdup(service_label(generator));
    ask->self_hosted = AI_IS_COMFYUI_CLIENT(generator);
    if (out_service != NULL) {
        const gchar *road = clawt_config_get_string(self->config, "office.asset_api");

        *out_service = g_strdup(road != NULL && *road != '\0' ? road : "");
    }

    /* on the daemon's context: the estimate is a network call */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    ai_media_generator_estimate_async(generator, request, NULL, on_estimate, ask);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
    return TRUE;
}

/* ── Making it ──────────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon      *self;
    AiMediaGenerator *generator;
    AiMediaRequest   *request;
    gchar            *agent;
    gint64            cost;
} Job;

static void
job_free(Job *job)
{
    g_object_unref(job->self);
    g_object_unref(job->generator);
    g_object_unref(job->request);
    g_free(job->agent);
    g_free(job);
}

/*
 * Tells the person what became of a generation they approved, in their
 * own conversation with the agent: a warning in the daemon's log is a
 * report nobody who pressed Approve will read.  Recorded, never sent --
 * no mailbox, so no turn and no reply.
 */
static void
tell_operator(ClawtDaemon *self, const gchar *agent_id, const gchar *body)
{
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GError) error = NULL;
    ClawtRoom *room;

    if (self->router == NULL || self->rooms == NULL)
        return;
    room = clawt_room_manager_get_direct(self->rooms, "user", agent_id);
    if (room == NULL)
        return;
    message = clawt_message_new(clawt_room_get_id(room), CLAWT_SYSTEM_SENDER, body);
    clawt_message_set_depth(message, 0);
    clawt_message_set_invites_reply(message, FALSE);
    if (!clawt_mailbox_router_record(self->router, message, &error))
        g_warning("office: could not record the outcome for %s: %s", agent_id,
                  error->message);
}

static void
job_failed(Job *job, const gchar *what, const GError *error)
{
    g_autofree gchar *body = g_strdup_printf(
        "%s's office character was not made: %s: %s.  Nothing was imported.",
        job->agent, what, error != NULL ? error->message : "no reason given");

    /* expected (a service out of credits), and the room note is the report */
    g_message("office: %s", body);
    tell_operator(job->self, job->agent, body);
    job_free(job);
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    ClawtDaemon *self = job->self;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = ai_media_generator_download_finish(AI_MEDIA_GENERATOR(source),
                                                                 result, &error);
    g_autofree gchar *root = clawt_daemon_office_characters_dir(self);
    g_autofree gchar *tmp = NULL;
    g_autofree gchar *provenance = NULL;
    g_autoptr(JsonObject) animations = json_object_new();
    JsonObject *idle = json_object_new();
    gint fd;
    guint width = 0;
    guint height = 0;
    gsize length = 0;
    const guint8 *data;

    if (bytes == NULL) {
        job_failed(job, "the download failed", error);
        return;
    }
    data = g_bytes_get_data(bytes, &length);
    fd = g_file_open_tmp("clawt-character-XXXXXX.png", &tmp, &error);
    if (fd < 0) {
        job_failed(job, "no temporary file", error);
        return;
    }
    close(fd);
    if (!g_file_set_contents(tmp, (const gchar *)data, (gssize)length, &error) ||
        !clawt_office_png_size(tmp, &width, &height)) {
        g_unlink(tmp);
        job_failed(job, "the result is not a PNG", error);
        return;
    }
    {
        /*
         * What it cost, as the approved card said: a job's status names
         * no price, and "cost unknown" beside a price somebody approved
         * is a record that knows less than its reader did.
         */
        g_autofree gchar *estimate = g_strdup(g_hash_table_lookup(estimates(self), job->agent));
        g_autofree gchar *spent = estimate != NULL
            ? g_strdup_printf(", estimated at %s", estimate)
            : g_strdup(job->cost >= 0 ? "" : ", cost unknown");

        g_hash_table_remove(estimates(self), job->agent);
        provenance = AI_IS_COMFYUI_CLIENT(job->generator)
            ? g_strdup_printf("generated by a self-hosted ComfyUI (%s) with the operator's own "
                              "workflow; the licence is that of the models it runs",
                              ai_comfyui_client_get_base_url(AI_COMFYUI_CLIENT(job->generator)))
            : g_strdup_printf("generated by Higgsfield (%s%s) for clawtilla's office%s; "
                              "Higgsfield's terms apply (docs/higgsfield.org)",
                              ai_media_request_get_model(job->request),
                              CLAWT_IS_HIGGSFIELD_MCP(job->generator)
                              ? ", through its MCP server" : ", through its REST API",
                              spent);
    }
    json_object_set_int_member(idle, "row", 0);
    json_object_set_int_member(idle, "frames", 1);
    json_object_set_object_member(animations, "idle", idle);
    if (!clawt_office_character_import_asset(root, job->agent, tmp, "generated/portrait.png",
                                             NULL, provenance, NULL, &error) ||
        !clawt_office_character_set_sheet(root, job->agent, "generated/portrait.png",
                                          width, height, animations, &error) ||
        /* a generator paints a backdrop; drawn as it is, it is a box round the person */
        !clawt_office_character_set_background(root, job->agent, "corners", &error)) {
        g_unlink(tmp);
        job_failed(job, "it could not be imported", error);
        return;
    }
    g_unlink(tmp);
    g_message("office: made %s's character", job->agent);
    {
        g_autofree gchar *body = g_strdup_printf(
            "%s's office character is made (%s) and the office draws it now.", job->agent,
            AI_IS_COMFYUI_CLIENT(job->generator) ? "on your ComfyUI"
            : CLAWT_IS_HIGGSFIELD_MCP(job->generator)
            ? "by Higgsfield, from your app credits"
            : "by Higgsfield, from your API balance");

        tell_operator(self, job->agent, body);
    }
    if (self->bus != NULL)
        clawt_event_bus_emit(self->bus, "office.changed", job->agent);
    job_free(job);
}

static void
on_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiMediaJob) done = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(source),
                                                                result, &error);
    const gchar *const *urls;

    if (done == NULL || ai_media_job_get_state(done) != AI_MEDIA_JOB_STATE_DONE ||
        ai_media_job_get_n_result_urls(done) == 0) {
        job_failed(job, "the job did not finish with a result", error);
        return;
    }
    job->cost = ai_media_job_get_cost_micro_usd(done);
    urls = ai_media_job_get_result_urls(done);
    ai_media_generator_download_async(job->generator, urls[0], NULL, on_downloaded, job);
}

static void
on_submitted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(source), result,
                                                            &error);
    guint first = job->self->office_generate_poll_ms > 0 ? job->self->office_generate_poll_ms
                                                         : 3000;

    if (id == NULL) {
        job_failed(job, "it was not accepted", error);
        return;
    }
    ai_media_generator_wait_async(job->generator, id, first, MAX(first, 20000u),
                                  10 * 60 * 1000, NULL, on_finished, job);
}

static void
submit(Job *job)
{
    ai_media_request_ensure_idempotency_key(job->request);
    ai_media_generator_submit_async(job->generator, job->request, NULL, on_submitted, job);
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *url = ai_higgsfield_client_upload_finish(AI_HIGGSFIELD_CLIENT(source),
                                                               result, &error);

    /* a reference that would not upload costs the likeness, not the character */
    if (url != NULL)
        ai_media_request_set_image_url(job->request, url);
    else
        g_message("office: %s's picture was not uploaded, so the character is made from "
                  "the description alone: %s", job->agent,
                  error != NULL ? error->message : "no reason given");
    submit(job);
}

static void
on_mcp_uploaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *media = clawt_higgsfield_mcp_upload_finish(CLAWT_HIGGSFIELD_MCP(source),
                                                                 result, &error);

    /* a media id, never a URL: Higgsfield's image tools refuse URLs */
    if (media != NULL)
        ai_media_request_set_image_url(job->request, media);
    else
        g_message("office: %s's picture was not uploaded, so the character is made from "
                  "the description alone: %s", job->agent,
                  error != NULL ? error->message : "no reason given");
    submit(job);
}

static void
on_comfyui_uploaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *name = ai_comfyui_client_upload_finish(AI_COMFYUI_CLIENT(source), result,
                                                             &error);

    /* the name in the server's input folder is what {{image}} becomes */
    if (name != NULL)
        ai_media_request_set_image_url(job->request, name);
    else
        g_message("office: %s's picture was not uploaded, so the character is made from "
                  "the description alone: %s", job->agent,
                  error != NULL ? error->message : "no reason given");
    submit(job);
}

/* Approved (or free): upload the agent's own picture if it has one, then submit. */
void
clawt_daemon_office_generate_start(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(self->config, agent_id);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) picture = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *mime = NULL;
    Job *job;
    AiMediaGenerator *generator;

    if (agent == NULL)
        return;
    generator = generator_for(self, &error);
    if (generator == NULL) {
        g_warning("office: %s's character was approved and cannot be made: %s", agent_id,
                  error->message);
        return;
    }
    job = g_new0(Job, 1);
    job->self = g_object_ref(self);
    job->generator = generator;
    job->request = request_for(self, agent);
    job->agent = g_strdup(agent_id);
    job->cost = -1;

    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    if (agent_picture(agent, &picture, &name, &mime)) {
        if (AI_IS_HIGGSFIELD_CLIENT(generator)) {
            ai_higgsfield_client_upload_async(AI_HIGGSFIELD_CLIENT(generator), picture, mime,
                                              NULL, on_uploaded, job);
            goto out;
        }
        if (AI_IS_COMFYUI_CLIENT(generator)) {
            ai_comfyui_client_upload_async(AI_COMFYUI_CLIENT(generator), picture, name, NULL,
                                           on_comfyui_uploaded, job);
            goto out;
        }
        if (CLAWT_IS_HIGGSFIELD_MCP(generator)) {
            clawt_higgsfield_mcp_upload_async(CLAWT_HIGGSFIELD_MCP(generator), picture, name,
                                              mime, NULL, on_mcp_uploaded, job);
            goto out;
        }
    }
    submit(job);
out:
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
}

gboolean
clawt_daemon_office_generate_settle(ClawtDaemon   *self,
                                    ClawtDecision *decision,
                                    const gchar   *how)
{
    const gchar *answer;

    if (decision == NULL ||
        !g_str_has_prefix(clawt_decision_get_id(decision), GENERATE_PREFIX))
        return FALSE;
    answer = clawt_decision_get_answer(decision);

    /* only the exact word spends money -- "maybe later" is not a yes */
    if (g_strcmp0(how, "answered") == 0 &&
        g_ascii_strcasecmp(answer != NULL ? answer : "", "approve") == 0)
        clawt_daemon_office_generate_start(self, clawt_decision_get_agent(decision));
    return TRUE;
}
