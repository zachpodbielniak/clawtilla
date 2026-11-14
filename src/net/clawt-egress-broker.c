/*
 * clawt-egress-broker.c - Provider keys that never enter the sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "net/clawt-egress-broker.h"
#include "net/clawt-egress-gate.h"
#include "clawt-http.h"

#include <errno.h>
#include <string.h>
#include <libreclaw.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

/* ── What can be brokered ────────────────────────────────────────── */

/*
 * An endpoint a CLI can be pointed away from, and which CLIs read the
 * variable that points it.  A CLI that is not listed is assumed *not* to
 * read it: brokering a key for a CLI that ignores the base URL would
 * hand it a token it sends to the real provider, which fails -- and a
 * failure that reads as "invalid API key" sends somebody to the wrong
 * place.  Passed through and reported unbrokered instead.
 */
typedef struct {
	const gchar *grant;
	const gchar *base_variable;
	const gchar *upstream;
	const gchar *clis;            /* comma-separated libreclaw cli types */
} BrokerEndpoint;

static const BrokerEndpoint endpoints[] = {
	/*
	 * Claude Code and libreclaw's own ai-glib both read
	 * ANTHROPIC_BASE_URL and append /v1/messages to it.
	 */
	{ "anthropic", "ANTHROPIC_BASE_URL", "https://api.anthropic.com",
	  "claude-code,claude-tmux" },
	/* Codex reads OPENAI_BASE_URL with the /v1 already in it. */
	{ "openai", "OPENAI_BASE_URL", "https://api.openai.com/v1",
	  "codex-cli" }
};

typedef struct {
	const gchar *variable;
	const gchar *grant;
	const gchar *header;          /* lower case */
	gboolean     bearer;
} BrokerKey;

static const BrokerKey known_keys[] = {
	{ "ANTHROPIC_API_KEY",    "anthropic", "x-api-key",     FALSE },
	{ "ANTHROPIC_AUTH_TOKEN", "anthropic", "authorization", TRUE },
	{ "OPENAI_API_KEY",       "openai",    "authorization", TRUE }
};

/* Headers that can carry a credential, never forwarded from the agent. */
static const gchar *const credential_headers[] = {
	"authorization", "x-api-key", "x-goog-api-key", "proxy-authorization",
	"cookie", NULL
};

static const gchar *const hop_by_hop[] = {
	"connection", "keep-alive", "proxy-connection", "transfer-encoding",
	"te", "trailer", "upgrade", "host", "content-length", NULL
};

static const BrokerEndpoint *
endpoint_for(
	const gchar *grant
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(endpoints); i++)
		if (g_strcmp0(endpoints[i].grant, grant) == 0)
			return &endpoints[i];

	return NULL;
}

static const BrokerKey *
key_for(
	const gchar *variable
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known_keys); i++)
		if (g_strcmp0(known_keys[i].variable, variable) == 0)
			return &known_keys[i];

	return NULL;
}

static gboolean
cli_reads(
	const BrokerEndpoint *endpoint,
	const gchar          *cli
){
	g_auto(GStrv) clis = g_strsplit(endpoint->clis, ",", -1);

	return cli != NULL && g_strv_contains((const gchar * const *)clis, cli);
}

/*
 * Whether a variable looks like a credential, for reporting what is
 * *not* brokered.  Generous on purpose: the cost of listing a variable
 * that turns out to be harmless is a line nobody needed; the cost of
 * missing a key is a sentence saying everything is protected.
 */
static gboolean
looks_like_key(
	const gchar *name
){
	return g_str_has_suffix(name, "_API_KEY") ||
	       g_str_has_suffix(name, "_AUTH_TOKEN") ||
	       g_str_has_suffix(name, "_ACCESS_TOKEN") ||
	       g_str_has_suffix(name, "_TOKEN") ||
	       g_str_has_suffix(name, "_SECRET") ||
	       g_str_has_suffix(name, "_ACCESS_KEY") ||
	       g_strcmp0(name, "API_KEY") == 0;
}

void
clawt_egress_grant_free(
	ClawtEgressGrant *grant
){
	if (grant == NULL)
		return;

	g_free(grant->name);
	g_free(grant->upstream);
	g_clear_pointer(&grant->headers, g_hash_table_unref);
	g_free(grant);
}

static void
egress_key_free(
	gpointer data
){
	ClawtEgressKey *key = data;

	g_free(key->name);
	g_free(key->reason);
	g_free(key);
}

static ClawtEgressGrant *
grant_copy(
	const ClawtEgressGrant *grant
){
	ClawtEgressGrant *copy = g_new0(ClawtEgressGrant, 1);
	GHashTableIter iter;
	gpointer k;
	gpointer v;

	copy->name = g_strdup(grant->name);
	copy->upstream = g_strdup(grant->upstream);
	copy->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      g_free);

	g_hash_table_iter_init(&iter, grant->headers);

	while (g_hash_table_iter_next(&iter, &k, &v))
		g_hash_table_insert(copy->headers, g_strdup(k), g_strdup(v));

	return copy;
}

void
clawt_egress_broker_plan_free(
	ClawtEgressBrokerPlan *plan
){
	if (plan == NULL)
		return;

	g_clear_pointer(&plan->environment, g_hash_table_unref);
	g_clear_pointer(&plan->keys, g_ptr_array_unref);
	g_clear_pointer(&plan->grants, g_ptr_array_unref);
	g_free(plan);
}

static void
add_key(
	ClawtEgressBrokerPlan *plan,
	const gchar           *name,
	gchar                 *reason
){
	ClawtEgressKey *key = g_new0(ClawtEgressKey, 1);

	key->name = g_strdup(name);
	key->reason = reason;
	g_ptr_array_add(plan->keys, key);
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

ClawtEgressBrokerPlan *
clawt_egress_broker_plan(
	GHashTable          *environment,
	const gchar         *provider,
	ClawtRuntimeNetwork  network,
	const gchar         *token
){
	ClawtEgressBrokerPlan *plan;
	g_autoptr(GPtrArray) names = NULL;
	const gchar *cli;
	GHashTableIter iter;
	gpointer k;
	gpointer v;
	guint i;

	plan = g_new0(ClawtEgressBrokerPlan, 1);
	plan->environment = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_free);
	plan->keys = g_ptr_array_new_with_free_func(egress_key_free);
	plan->grants = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_egress_grant_free);

	names = g_ptr_array_new();

	if (environment != NULL) {
		g_hash_table_iter_init(&iter, environment);

		while (g_hash_table_iter_next(&iter, &k, &v)) {
			g_hash_table_insert(plan->environment, g_strdup(k),
			                    g_strdup(v));
			g_ptr_array_add(names, k);
		}
	}

	/* In name order, so a listing reads the same on every call. */
	g_ptr_array_sort(names, compare_names);

	cli = lc_provider_type_normalize(provider, "agent");

	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		const gchar *value = g_hash_table_lookup(environment, name);
		const BrokerKey *key = key_for(name);
		const BrokerEndpoint *endpoint;
		const gchar *existing_base;
		const gchar *upstream;
		ClawtEgressGrant *grant = NULL;
		guint g;

		if (key == NULL) {
			if (looks_like_key(name))
				add_key(plan, name, g_strdup_printf(
					"clawtilla knows no base-URL variable that would "
					"send %s through the broker, so the agent holds the "
					"real value", name));
			continue;
		}

		/* An empty key is no key, and brokering it would invent one. */
		if (value != NULL && *value == '\0')
			continue;

		if (network == CLAWT_RUNTIME_NETWORK_OPEN) {
			add_key(plan, name, g_strdup(
				"the network is open, so nothing is brokered: the agent "
				"could reach the provider directly with any key it held"));
			continue;
		}

		endpoint = endpoint_for(key->grant);

		if (!cli_reads(endpoint, cli)) {
			add_key(plan, name, g_strdup_printf(
				"%s does not read %s, so it cannot be pointed at the "
				"broker; the agent holds the real key",
				cli != NULL ? cli : "this CLI", endpoint->base_variable));
			continue;
		}

		/*
		 * An operator who already pointed the CLI somewhere -- a
		 * LiteLLM in front of the provider, say -- has the broker pin
		 * that instead, over https only: the real key does not travel
		 * in clear text on the operator's behalf either.
		 */
		existing_base = (environment != NULL)
		                ? g_hash_table_lookup(environment,
		                                      endpoint->base_variable)
		                : NULL;
		upstream = endpoint->upstream;

		if (existing_base != NULL && *existing_base != '\0') {
			if (!g_str_has_prefix(existing_base, "https://")) {
				add_key(plan, name, g_strdup_printf(
					"%s is already set to an address that is not "
					"https://, which the broker will not send a real key "
					"to", endpoint->base_variable));
				continue;
			}

			upstream = existing_base;
		}

		for (g = 0; g < plan->grants->len; g++) {
			ClawtEgressGrant *candidate = g_ptr_array_index(plan->grants, g);

			if (g_strcmp0(candidate->name, endpoint->grant) == 0)
				grant = candidate;
		}

		if (grant == NULL) {
			grant = g_new0(ClawtEgressGrant, 1);
			grant->name = g_strdup(endpoint->grant);
			grant->upstream = g_strdup(upstream);
			grant->headers = g_hash_table_new_full(g_str_hash, g_str_equal,
			                                       g_free, g_free);
			g_ptr_array_add(plan->grants, grant);
		}

		g_hash_table_insert(grant->headers, g_strdup(key->header),
		                    key->bearer
		                    ? g_strdup_printf("Bearer %s",
		                                      value != NULL ? value : "")
		                    : g_strdup(value != NULL ? value : ""));

		/*
		 * The swap.  With no token (a plan made to describe the agent)
		 * the variable is dropped rather than left holding the key, so
		 * a caller that forgot to pass one still cannot leak it.
		 */
		if (token != NULL)
			g_hash_table_insert(plan->environment, g_strdup(name),
			                    g_strdup(token));
		else
			g_hash_table_remove(plan->environment, name);

		g_hash_table_insert(plan->environment,
		                    g_strdup(endpoint->base_variable),
		                    g_strdup_printf("http://127.0.0.1:%d/%s",
		                                    CLAWT_EGRESS_BROKER_PORT,
		                                    endpoint->grant));

		add_key(plan, name, NULL);
	}

	return plan;
}

gboolean
clawt_egress_broker_plan_is_brokered(
	ClawtEgressBrokerPlan *plan,
	const gchar           *name
){
	guint i;

	g_return_val_if_fail(plan != NULL, FALSE);

	for (i = 0; i < plan->keys->len; i++) {
		ClawtEgressKey *key = g_ptr_array_index(plan->keys, i);

		if (g_strcmp0(key->name, name) == 0)
			return key->reason == NULL;
	}

	return FALSE;
}

gchar *
clawt_egress_broker_mint_token(
	GError **error
){
	g_autofree gchar *random = clawt_generate_token(error);

	if (random == NULL)
		return NULL;

	return g_strconcat("clawt-broker-", random, NULL);
}

static gboolean
name_in(
	const gchar        *name,
	const gchar *const *list
){
	g_autofree gchar *lower = g_ascii_strdown(name, -1);
	guint i;

	for (i = 0; list[i] != NULL; i++)
		if (g_strcmp0(lower, list[i]) == 0)
			return TRUE;

	return FALSE;
}

void
clawt_egress_broker_copy_headers(
	SoupMessageHeaders *from,
	SoupMessageHeaders *to,
	GHashTable         *credentials
){
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	g_return_if_fail(from != NULL);
	g_return_if_fail(to != NULL);

	soup_message_headers_iter_init(&iter, from);

	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		if (name_in(name, credential_headers) || name_in(name, hop_by_hop))
			continue;

		soup_message_headers_append(to, name, value);
	}

	if (credentials != NULL) {
		GHashTableIter hiter;
		gpointer k;
		gpointer v;

		g_hash_table_iter_init(&hiter, credentials);

		while (g_hash_table_iter_next(&hiter, &k, &v))
			soup_message_headers_replace(to, k, v);
	}
}

/* ── The broker ──────────────────────────────────────────────────── */

typedef struct {
	gchar     *agent_id;
	gchar     *token_hash;
	GPtrArray *grants;       /* ClawtEgressGrant */
} Registration;

struct _ClawtEgressBroker {
	GObject       parent_instance;

	GMainContext *context;
	SoupServer   *server;
	SoupSession  *session;
	gchar        *socket_path;
	GHashTable   *by_hash;      /* token hash -> Registration */
	GHashTable   *by_agent;     /* agent id -> token hash (borrowed key) */
	GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE(ClawtEgressBroker, clawt_egress_broker, G_TYPE_OBJECT)

static void
registration_free(
	gpointer data
){
	Registration *registration = data;

	g_free(registration->agent_id);
	g_free(registration->token_hash);
	g_ptr_array_unref(registration->grants);
	g_free(registration);
}

/*
 * Tokens are looked up by their SHA-256, so a lookup's timing says
 * nothing about how much of a guess was right and the daemon never holds
 * a token it could print.
 */
static gchar *
hash_token(
	const gchar *token
){
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
}

static void
clawt_egress_broker_dispose(
	GObject *object
){
	ClawtEgressBroker *self = CLAWT_EGRESS_BROKER(object);

	clawt_egress_broker_stop(self);
	g_clear_object(&self->session);

	G_OBJECT_CLASS(clawt_egress_broker_parent_class)->dispose(object);
}

static void
clawt_egress_broker_finalize(
	GObject *object
){
	ClawtEgressBroker *self = CLAWT_EGRESS_BROKER(object);

	g_clear_pointer(&self->by_agent, g_hash_table_unref);
	g_clear_pointer(&self->by_hash, g_hash_table_unref);
	g_clear_pointer(&self->context, g_main_context_unref);
	g_clear_object(&self->cancellable);
	g_free(self->socket_path);

	G_OBJECT_CLASS(clawt_egress_broker_parent_class)->finalize(object);
}

static void
clawt_egress_broker_class_init(
	ClawtEgressBrokerClass *klass
){
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = clawt_egress_broker_dispose;
	object_class->finalize = clawt_egress_broker_finalize;
}

static void
clawt_egress_broker_init(
	ClawtEgressBroker *self
){
	self->by_hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                      registration_free);
	self->by_agent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       g_free);
	self->cancellable = g_cancellable_new();
}

ClawtEgressBroker *
clawt_egress_broker_new(
	GMainContext *context
){
	ClawtEgressBroker *self = g_object_new(CLAWT_TYPE_EGRESS_BROKER, NULL);

	self->context = (context != NULL)
	                ? g_main_context_ref(context)
	                : g_main_context_ref_thread_default();

	return self;
}

const gchar *
clawt_egress_broker_get_socket_path(
	ClawtEgressBroker *self
){
	g_return_val_if_fail(CLAWT_IS_EGRESS_BROKER(self), NULL);

	return self->socket_path;
}

void
clawt_egress_broker_register(
	ClawtEgressBroker *self,
	const gchar       *agent_id,
	const gchar       *token,
	GPtrArray         *grants
){
	Registration *registration;
	guint i;

	g_return_if_fail(CLAWT_IS_EGRESS_BROKER(self));
	g_return_if_fail(agent_id != NULL);
	g_return_if_fail(token != NULL);

	clawt_egress_broker_revoke(self, agent_id);

	registration = g_new0(Registration, 1);
	registration->agent_id = g_strdup(agent_id);
	registration->token_hash = hash_token(token);
	registration->grants = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_egress_grant_free);

	for (i = 0; grants != NULL && i < grants->len; i++)
		g_ptr_array_add(registration->grants,
		                grant_copy(g_ptr_array_index(grants, i)));

	g_hash_table_insert(self->by_agent, g_strdup(agent_id),
	                    g_strdup(registration->token_hash));
	g_hash_table_insert(self->by_hash, registration->token_hash,
	                    registration);
}

void
clawt_egress_broker_revoke(
	ClawtEgressBroker *self,
	const gchar       *agent_id
){
	const gchar *hash;

	g_return_if_fail(CLAWT_IS_EGRESS_BROKER(self));

	if (agent_id == NULL)
		return;

	hash = g_hash_table_lookup(self->by_agent, agent_id);

	if (hash != NULL)
		g_hash_table_remove(self->by_hash, hash);

	g_hash_table_remove(self->by_agent, agent_id);
}

/* The token the agent sent, from whichever header its CLI uses. */
static gchar *
token_from(
	SoupMessageHeaders *headers
){
	const gchar *key = soup_message_headers_get_one(headers, "X-Api-Key");
	const gchar *auth;

	if (key != NULL && *key != '\0')
		return g_strdup(key);

	auth = soup_message_headers_get_one(headers, "Authorization");

	if (auth != NULL && g_ascii_strncasecmp(auth, "Bearer ", 7) == 0)
		return g_strstrip(g_strdup(auth + 7));

	return NULL;
}

gboolean
clawt_egress_broker_authorize(
	ClawtEgressBroker   *self,
	const gchar         *path,
	const gchar         *query,
	SoupMessageHeaders  *headers,
	gchar              **out_url,
	GHashTable         **out_credentials,
	gchar              **out_agent_id,
	guint               *out_status,
	GError             **error
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *grant_name = NULL;
	Registration *registration;
	ClawtEgressGrant *grant = NULL;
	const gchar *rest;
	guint i;

	g_return_val_if_fail(CLAWT_IS_EGRESS_BROKER(self), FALSE);
	g_return_val_if_fail(out_url != NULL, FALSE);
	g_return_val_if_fail(out_credentials != NULL, FALSE);

	*out_url = NULL;
	*out_credentials = NULL;

	if (out_status != NULL)
		*out_status = 404;

	/* /<grant> or /<grant>/<rest> */
	if (path == NULL || path[0] != '/' || path[1] == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		                    "the broker serves /<provider>/...");
		return FALSE;
	}

	rest = strchr(path + 1, '/');
	grant_name = (rest != NULL) ? g_strndup(path + 1, (gsize)(rest - path - 1))
	                            : g_strdup(path + 1);

	/* ".." in the rest would be resolved upstream against the pin. */
	if (rest != NULL && (strstr(rest, "/../") != NULL ||
	                     g_str_has_suffix(rest, "/.."))) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a path with .. in it is not forwarded");
		return FALSE;
	}

	token = token_from(headers);

	if (out_status != NULL)
		*out_status = 401;

	if (token == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
		                    "no broker token in X-Api-Key or Authorization");
		return FALSE;
	}

	hash = hash_token(token);
	registration = g_hash_table_lookup(self->by_hash, hash);

	if (registration == NULL ||
	    !clawt_secure_equals(registration->token_hash, hash)) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
		                    "the broker token is not one it issued, or the "
		                    "process it was issued to has exited");
		return FALSE;
	}

	for (i = 0; i < registration->grants->len; i++) {
		ClawtEgressGrant *candidate =
			g_ptr_array_index(registration->grants, i);

		if (g_strcmp0(candidate->name, grant_name) == 0)
			grant = candidate;
	}

	if (grant == NULL) {
		if (out_status != NULL)
			*out_status = 403;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		            "this agent's token does not reach '%s'", grant_name);
		return FALSE;
	}

	{
		GString *url = g_string_new(grant->upstream);

		while (url->len > 0 && url->str[url->len - 1] == '/')
			g_string_truncate(url, url->len - 1);

		g_string_append(url, rest != NULL ? rest : "");

		if (query != NULL && *query != '\0') {
			g_string_append_c(url, '?');
			g_string_append(url, query);
		}

		*out_url = g_string_free(url, FALSE);
	}

	*out_credentials = g_hash_table_ref(grant->headers);

	if (out_agent_id != NULL)
		*out_agent_id = g_strdup(registration->agent_id);

	if (out_status != NULL)
		*out_status = 200;

	return TRUE;
}

/* ── Forwarding ──────────────────────────────────────────────────── */

typedef struct {
	ClawtEgressBroker *broker;
	SoupServerMessage *request;
	SoupMessage       *upstream;
	GInputStream      *body;
	GCancellable      *cancellable;
	gulong             disconnected_id;
} Forward;

static void
forward_free(
	Forward *forward
){
	if (forward->disconnected_id != 0)
		g_signal_handler_disconnect(forward->request,
		                            forward->disconnected_id);

	g_clear_object(&forward->body);
	g_clear_object(&forward->upstream);
	g_clear_object(&forward->cancellable);
	g_object_unref(forward->request);
	g_object_unref(forward->broker);
	g_free(forward);
}

/*
 * A refusal the CLI will print.  JSON in the shape the providers' own
 * errors take, so a CLI that parses one shows the sentence rather than
 * "unexpected response".
 */
static void
answer_error(
	SoupServerMessage *message,
	guint              status,
	const gchar       *text
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *body = NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "error");
	json_builder_set_member_name(builder, "error");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "clawtilla_broker");
	json_builder_set_member_name(builder, "message");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	body = json_generator_to_data(generator, NULL);

	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, "application/json",
	                                 SOUP_MEMORY_COPY, body, strlen(body));
}

static void forward_read(Forward *forward);

static void
on_body_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Forward *forward = user_data;
	g_autoptr(GBytes) bytes = NULL;
	SoupMessageBody *out;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         NULL);
	out = soup_server_message_get_response_body(forward->request);

	/*
	 * An error mid-stream ends the body where it is: the status and
	 * headers are long gone, so there is nothing truer to send.
	 */
	if (bytes == NULL || g_bytes_get_size(bytes) == 0) {
		if (!g_cancellable_is_cancelled(forward->cancellable)) {
			soup_message_body_complete(out);
			soup_server_message_unpause(forward->request);
		}

		forward_free(forward);
		return;
	}

	soup_message_body_append_bytes(out, bytes);
	soup_server_message_unpause(forward->request);
	forward_read(forward);
}

static void
forward_read(
	Forward *forward
){
	g_main_context_push_thread_default(forward->broker->context);
	g_input_stream_read_bytes_async(forward->body, 16384, G_PRIORITY_DEFAULT,
	                                forward->cancellable, on_body_read,
	                                forward);
	g_main_context_pop_thread_default(forward->broker->context);
}

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Forward *forward = user_data;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *response_headers;
	SoupMessageHeaders *upstream_headers;
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	forward->body = soup_session_send_finish(SOUP_SESSION(source), result,
	                                         &error);

	if (g_cancellable_is_cancelled(forward->cancellable)) {
		forward_free(forward);
		return;
	}

	if (forward->body == NULL) {
		answer_error(forward->request, 502, error->message);
		soup_server_message_unpause(forward->request);
		forward_free(forward);
		return;
	}

	/*
	 * A redirect is not followed -- the message was built carrying
	 * credentials -- and is not passed back either: the CLI would
	 * follow it with its token, and the answer it needs is that the
	 * pinned upstream tried to send it somewhere else.
	 */
	if (!clawt_http_check_redirect(forward->upstream, &error)) {
		answer_error(forward->request, 502, error->message);
		soup_server_message_unpause(forward->request);
		g_input_stream_close(forward->body, NULL, NULL);
		forward_free(forward);
		return;
	}

	soup_server_message_set_status(forward->request,
	                               soup_message_get_status(forward->upstream),
	                               soup_message_get_reason_phrase(
	                                   forward->upstream));

	response_headers = soup_server_message_get_response_headers(
		forward->request);
	upstream_headers = soup_message_get_response_headers(forward->upstream);
	soup_message_headers_iter_init(&iter, upstream_headers);

	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		if (name_in(name, hop_by_hop))
			continue;

		soup_message_headers_append(response_headers, name, value);
	}

	/*
	 * Streamed, chunk by chunk, because a model's answer is a stream of
	 * server-sent events and a CLI waiting for the whole of it before
	 * showing a token would look hung.  Not accumulated, so a long
	 * answer is not held twice.
	 */
	soup_message_headers_set_encoding(response_headers,
	                                  SOUP_ENCODING_CHUNKED);
	soup_message_body_set_accumulate(
		soup_server_message_get_response_body(forward->request), FALSE);
	soup_server_message_unpause(forward->request);

	forward_read(forward);
}

static void
on_disconnected(
	SoupServerMessage *message,
	gpointer           user_data
){
	Forward *forward = user_data;

	g_cancellable_cancel(forward->cancellable);
}

static void
handle_request(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	ClawtEgressBroker *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) credentials = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GBytes) body = NULL;
	SoupMessageHeaders *incoming;
	SoupMessage *upstream;
	Forward *forward;
	GUri *uri;
	guint status = 500;
	const gchar *content_type;

	uri = soup_server_message_get_uri(message);
	incoming = soup_server_message_get_request_headers(message);

	if (!clawt_egress_broker_authorize(self, g_uri_get_path(uri),
	                                   g_uri_get_query(uri), incoming, &url,
	                                   &credentials, NULL, &status, &error)) {
		answer_error(message, status, error->message);
		return;
	}

	upstream = clawt_http_message_new(soup_server_message_get_method(message),
	                                  url, CLAWT_HTTP_CARRIES_CREDENTIALS);

	if (upstream == NULL) {
		answer_error(message, 502, "the broker could not build the "
		             "upstream request");
		return;
	}

	clawt_egress_broker_copy_headers(
		incoming, soup_message_get_request_headers(upstream), credentials);

	body = soup_message_body_flatten(
		soup_server_message_get_request_body(message));
	content_type = soup_message_headers_get_content_type(incoming, NULL);

	if (body != NULL && g_bytes_get_size(body) > 0)
		soup_message_set_request_body_from_bytes(upstream, content_type,
		                                         body);

	forward = g_new0(Forward, 1);
	forward->broker = g_object_ref(self);
	forward->request = g_object_ref(message);
	forward->upstream = upstream;
	forward->cancellable = g_cancellable_new();
	forward->disconnected_id = g_signal_connect(
		message, "disconnected", G_CALLBACK(on_disconnected), forward);

	g_signal_connect_object(self->cancellable, "cancelled",
	                        G_CALLBACK(g_cancellable_cancel),
	                        forward->cancellable, G_CONNECT_SWAPPED);

	soup_server_message_pause(message);

	g_main_context_push_thread_default(self->context);
	soup_session_send_async(self->session, upstream, G_PRIORITY_DEFAULT,
	                        forward->cancellable, on_sent, forward);
	g_main_context_pop_thread_default(self->context);
}

gboolean
clawt_egress_broker_listen(
	ClawtEgressBroker  *self,
	const gchar        *socket_path,
	GError            **error
){
	g_autoptr(GSocketAddress) address = NULL;
	g_autofree gchar *dir = NULL;
	gboolean ok;

	g_return_val_if_fail(CLAWT_IS_EGRESS_BROKER(self), FALSE);
	g_return_val_if_fail(socket_path != NULL, FALSE);

	if (self->server != NULL)
		return TRUE;

	if (!clawt_check_socket_path(socket_path, error))
		return FALSE;

	dir = g_path_get_dirname(socket_path);

	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "could not create %s: %s", dir, g_strerror(errno));
		return FALSE;
	}

	g_unlink(socket_path);

	/*
	 * Both made under the broker's context: a SoupSession is bound to
	 * the context that was thread-default when it was created, and a
	 * SoupServer attaches its listener where it listens.  Built under
	 * another one, each sends and never reads the answer.
	 */
	g_main_context_push_thread_default(self->context);

	if (self->session == NULL) {
		self->session = soup_session_new();

		/*
		 * Bytes pass through as the provider sent them.  libsoup's
		 * decoder would inflate a gzip body and leave the
		 * Content-Encoding header saying it was still compressed.
		 */
		soup_session_remove_feature_by_type(self->session,
		                                    SOUP_TYPE_CONTENT_DECODER);
	}

	self->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(self->server, NULL, handle_request, self, NULL);
	address = g_unix_socket_address_new(socket_path);
	ok = soup_server_listen(self->server, address, 0, error);

	g_main_context_pop_thread_default(self->context);

	if (!ok) {
		g_clear_object(&self->server);
		g_prefix_error(error, "the provider-key broker: ");
		return FALSE;
	}

	if (g_chmod(socket_path, 0600) != 0) {
		soup_server_disconnect(self->server);
		g_clear_object(&self->server);
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "could not restrict %s: %s", socket_path,
		            g_strerror(errno));
		return FALSE;
	}

	g_free(self->socket_path);
	self->socket_path = g_strdup(socket_path);

	return TRUE;
}

void
clawt_egress_broker_stop(
	ClawtEgressBroker *self
){
	g_return_if_fail(CLAWT_IS_EGRESS_BROKER(self));

	if (self->cancellable != NULL) {
		g_cancellable_cancel(self->cancellable);
		g_clear_object(&self->cancellable);
		self->cancellable = g_cancellable_new();
	}

	if (self->server != NULL) {
		soup_server_disconnect(self->server);
		g_clear_object(&self->server);
	}

	if (self->socket_path != NULL)
		g_unlink(self->socket_path);
}
