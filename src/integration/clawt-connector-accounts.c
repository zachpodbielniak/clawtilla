/*
 * clawt-connector-accounts.c - Several accounts on one service
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-connector-accounts.h"

#include <string.h>

#include <json-glib/json-glib.h>

/* JSON-RPC's "method not found", which is what a refused tool is. */
#define JSONRPC_METHOD_NOT_FOUND (-32601)

gchar *
clawt_connector_account_slug(
	const gchar *name,
	const gchar *account
){
	const gchar *source;
	GString *out;
	const gchar *p;
	gboolean underscore = FALSE;

	source = (account != NULL && *account != '\0') ? account : name;
	out = g_string_new(NULL);

	/*
	 * Runs of anything else become one underscore, and none is left at
	 * either end -- "My Work!" is `my_work`, not `my_work_`, which
	 * would put two underscores between it and the tool's own name.
	 */
	for (p = (source != NULL) ? source : ""; *p != '\0'; p++) {
		gchar c = g_ascii_tolower(*p);

		if (g_ascii_isalnum(c)) {
			if (underscore && out->len > 0)
				g_string_append_c(out, '_');

			g_string_append_c(out, c);
			underscore = FALSE;
		} else {
			underscore = TRUE;
		}
	}

	if (out->len == 0)
		g_string_append(out, "account");

	return g_string_free(out, FALSE);
}

static gboolean
binding_is_connector(
	ClawtIntegrationBinding *binding
){
	const ClawtIntegrationInfo *info = clawt_integration_binding_get_info(binding);

	return info != NULL && g_strcmp0(info->id, "connector") == 0;
}

static gchar *
binding_slug(
	ClawtIntegrationBinding *binding
){
	return clawt_connector_account_slug(
		clawt_integration_binding_get_name(binding),
		clawt_integration_binding_get_string(binding, "account"));
}

gchar *
clawt_connector_account_prefix(
	GPtrArray                *bindings,
	ClawtIntegrationBinding  *binding,
	GStrv                    *out_siblings,
	GError                  **error
){
	g_autoptr(GPtrArray) siblings = NULL;
	g_autofree gchar *mine = NULL;
	const gchar *provider;
	guint same = 0;
	guint i;

	g_return_val_if_fail(binding != NULL, NULL);

	if (out_siblings != NULL)
		*out_siblings = NULL;

	if (bindings == NULL || !binding_is_connector(binding))
		return NULL;

	provider = clawt_integration_binding_get_string(binding, "provider");

	if (provider == NULL)
		return NULL;

	mine = binding_slug(binding);
	siblings = g_ptr_array_new_with_free_func(g_free);

	/*
	 * Every account on the service is counted, this one included, so
	 * the answer is the same whichever of them asks -- one relay must
	 * never decide it is alone while its sibling decides it is not.
	 */
	for (i = 0; i < bindings->len; i++) {
		ClawtIntegrationBinding *other = g_ptr_array_index(bindings, i);

		if (!binding_is_connector(other))
			continue;

		if (g_strcmp0(clawt_integration_binding_get_string(other, "provider"),
		              provider) != 0)
			continue;

		same++;
		g_ptr_array_add(siblings, binding_slug(other));
	}

	if (same < 2)
		return NULL;

	/*
	 * A prefix two accounts share is no prefix at all: both relays would
	 * advertise `work_create_issue`, and the agent's call would run as
	 * whichever it happened to reach.  Said by naming both, because the
	 * fix is a label on one of them.
	 */
	for (i = 0; i < bindings->len; i++) {
		ClawtIntegrationBinding *other = g_ptr_array_index(bindings, i);
		g_autofree gchar *theirs = NULL;

		if (other == binding || !binding_is_connector(other))
			continue;

		if (g_strcmp0(clawt_integration_binding_get_string(other, "provider"),
		              provider) != 0)
			continue;

		if (g_strcmp0(clawt_integration_binding_get_name(other),
		              clawt_integration_binding_get_name(binding)) == 0)
			continue;

		theirs = binding_slug(other);

		if (g_strcmp0(theirs, mine) == 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "the connectors '%s' and '%s' are both %s accounts "
			            "labelled '%s', so neither can be told apart from "
			            "the other; give one of them a different account "
			            "label",
			            clawt_integration_binding_get_name(binding),
			            clawt_integration_binding_get_name(other), provider,
			            mine);
			return NULL;
		}
	}

	if (out_siblings != NULL) {
		g_ptr_array_add(siblings, NULL);
		*out_siblings = (GStrv)g_ptr_array_free(g_steal_pointer(&siblings),
		                                        FALSE);
	}

	return g_steal_pointer(&mine);
}

gboolean
clawt_connector_check_new_account(
	GPtrArray               *integrations,
	ClawtIntegrationConfig  *candidate,
	GError                 **error
){
	g_autofree gchar *mine = NULL;
	const gchar *provider;
	const gchar *account;
	guint i;

	g_return_val_if_fail(candidate != NULL, FALSE);

	if (g_strcmp0(clawt_integration_config_get_type_id(candidate),
	              "connector") != 0)
		return TRUE;

	provider = clawt_integration_config_get_string(candidate, NULL,
	                                               "provider");
	account = clawt_integration_config_get_string(candidate, NULL, "account");

	if (provider == NULL || integrations == NULL)
		return TRUE;

	mine = clawt_connector_account_slug(
		clawt_integration_config_get_name(candidate), account);

	for (i = 0; i < integrations->len; i++) {
		ClawtIntegrationConfig *other = g_ptr_array_index(integrations, i);
		const gchar *other_account;
		g_autofree gchar *theirs = NULL;

		if (other == candidate ||
		    g_strcmp0(clawt_integration_config_get_name(other),
		              clawt_integration_config_get_name(candidate)) == 0)
			continue;

		if (g_strcmp0(clawt_integration_config_get_type_id(other),
		              "connector") != 0)
			continue;

		if (g_strcmp0(clawt_integration_config_get_string(other, NULL,
		                                                  "provider"),
		              provider) != 0)
			continue;

		/*
		 * A second account must say which it is.  The label is what an
		 * agent's tools are prefixed with and what every client shows on
		 * the row, so an unlabelled second account is one nobody can
		 * tell from the first -- and the operator is the only one who
		 * knows which is which.
		 */
		if (account == NULL || *account == '\0') {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "there is already a %s account here ('%s'); give "
			            "this one an account label, such as work or "
			            "personal, so the two can be told apart",
			            provider, clawt_integration_config_get_name(other));
			return FALSE;
		}

		other_account = clawt_integration_config_get_string(other, NULL,
		                                                    "account");
		theirs = clawt_connector_account_slug(
			clawt_integration_config_get_name(other), other_account);

		if (g_strcmp0(theirs, mine) == 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
			            "the %s account '%s' is already labelled '%s'; "
			            "pick a label no other %s account uses",
			            provider, clawt_integration_config_get_name(other),
			            theirs, provider);
			return FALSE;
		}
	}

	return TRUE;
}

/* ── The relay's half ────────────────────────────────────────────── */

static JsonObject *
parse_object(
	JsonParser  *parser,
	const gchar *line
){
	JsonNode *root;

	if (line == NULL || !json_parser_load_from_data(parser, line, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_node_get_object(root);
}

static gchar *
render(
	JsonObject *object
){
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

	json_node_set_object(node, object);
	json_generator_set_root(generator, node);

	return json_generator_to_data(generator, NULL);
}

gchar *
clawt_connector_accounts_inbound(
	const gchar *line,
	const gchar *prefix
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *object;
	JsonObject *result;
	JsonArray *tools;
	guint i;

	g_return_val_if_fail(line != NULL, NULL);

	object = parse_object(parser, line);

	if (prefix == NULL || object == NULL ||
	    !json_object_has_member(object, "result") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "result")))
		return g_strdup(line);

	result = json_object_get_object_member(object, "result");

	if (result == NULL || !json_object_has_member(result, "tools") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(result, "tools")))
		return g_strdup(line);

	tools = json_object_get_array_member(result, "tools");

	for (i = 0; i < json_array_get_length(tools); i++) {
		JsonNode *element = json_array_get_element(tools, i);
		JsonObject *tool;
		const gchar *name;
		const gchar *description;
		g_autofree gchar *renamed = NULL;
		g_autofree gchar *said = NULL;

		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;

		tool = json_node_get_object(element);
		name = json_object_get_string_member_with_default(tool, "name", NULL);

		if (name == NULL)
			continue;

		renamed = g_strdup_printf("%s_%s", prefix, name);

		/*
		 * The description says it too, because a model choosing between
		 * two tools reads the sentence before it reads the name.
		 */
		description = json_object_get_string_member_with_default(
			tool, "description", NULL);
		said = (description != NULL && *description != '\0')
			? g_strdup_printf("[%s account] %s", prefix, description)
			: g_strdup_printf("[%s account]", prefix);

		json_object_set_string_member(tool, "name", renamed);
		json_object_set_string_member(tool, "description", said);
	}

	return render(object);
}

/* A JSON-RPC error answering @request, which has an id. */
static gchar *
refuse(
	JsonObject  *request,
	const gchar *message
){
	g_autoptr(JsonObject) reply = json_object_new();
	JsonObject *error = json_object_new();

	json_object_set_string_member(reply, "jsonrpc", "2.0");
	json_object_set_member(reply, "id",
	                       json_node_copy(json_object_get_member(request,
	                                                             "id")));
	json_object_set_int_member(error, "code", JSONRPC_METHOD_NOT_FOUND);
	json_object_set_string_member(error, "message", message);
	json_object_set_object_member(reply, "error", error);

	return render(reply);
}

gchar *
clawt_connector_accounts_outbound(
	const gchar  *line,
	const gchar  *prefix,
	GStrv         siblings,
	const gchar  *conflict,
	gchar       **refusal
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *expected = NULL;
	JsonObject *object;
	JsonObject *params;
	const gchar *tool;

	g_return_val_if_fail(line != NULL, NULL);

	if (refusal != NULL)
		*refusal = NULL;

	object = parse_object(parser, line);

	if (object == NULL ||
	    g_strcmp0(json_object_get_string_member_with_default(object, "method",
	                                                         ""),
	              "tools/call") != 0 ||
	    !json_object_has_member(object, "params") ||
	    !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "params")))
		return g_strdup(line);

	params = json_object_get_object_member(object, "params");
	tool = json_object_get_string_member_with_default(params, "name", NULL);

	if (tool == NULL)
		return g_strdup(line);

	/* Colliding labels: refuse, never route to whichever came first. */
	if (prefix == NULL) {
		if (refusal != NULL && json_object_has_member(object, "id")) {
			g_autofree gchar *message = g_strdup_printf(
				"clawtilla refused '%s': %s. Nothing ran.", tool,
				conflict != NULL ? conflict
				                 : "two accounts on this service share a label");

			*refusal = refuse(object, message);
		}

		return NULL;
	}

	expected = g_strconcat(prefix, "_", NULL);

	if (g_str_has_prefix(tool, expected) && tool[strlen(expected)] != '\0') {
		json_object_set_string_member(params, "name",
		                              tool + strlen(expected));
		return render(object);
	}

	if (refusal != NULL && json_object_has_member(object, "id")) {
		g_autofree gchar *others = (siblings != NULL)
			? g_strjoinv(", ", siblings) : g_strdup(prefix);
		g_autofree gchar *message = g_strdup_printf(
			"clawtilla refused '%s': this agent holds several accounts on "
			"this service (%s), so every tool is named for the account it "
			"acts as. This server is the '%s' account; call %s%s to use "
			"it. Nothing ran.", tool, others, prefix, expected, tool);

		*refusal = refuse(object, message);
	}

	return NULL;
}
