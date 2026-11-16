/*
 * test-mcp-app.c - An MCP server's app, shown in the conversation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The relay's side is driven message by message through the same policy
 * functions the stdio relay and the HTTP bridge call: a tool list that
 * names a view, a call of that tool, the relay's own read of the view,
 * and the report the daemon would be handed.  The policy a view runs
 * under is checked for what it refuses as much as for what it allows.
 */

#include <clawtilla.h>

#include <string.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "clawt-test-util.h"

static JsonObject *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_nonnull(text);
	g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

	return json_object_ref(json_node_get_object(
		json_parser_get_root(parser)));
}

/* ── Tool definitions ────────────────────────────────────────────── */

static void
test_tool_resource(void)
{
	g_autoptr(JsonObject) nested = parse(
		"{\"name\":\"draw\",\"_meta\":{\"ui\":{\"resourceUri\":"
		"\"ui://draw/view.html\"}}}");
	g_autoptr(JsonObject) flat = parse(
		"{\"name\":\"draw\",\"_meta\":{\"ui/resourceUri\":"
		"\"ui://draw/old.html\"}}");
	g_autoptr(JsonObject) web = parse(
		"{\"name\":\"draw\",\"_meta\":{\"ui\":{\"resourceUri\":"
		"\"https://example.com/view.html\"}}}");
	g_autoptr(JsonObject) plain = parse("{\"name\":\"list\"}");
	g_autofree gchar *a = clawt_mcp_app_tool_resource(nested);
	g_autofree gchar *b = clawt_mcp_app_tool_resource(flat);
	g_autofree gchar *c = clawt_mcp_app_tool_resource(web);
	g_autofree gchar *d = clawt_mcp_app_tool_resource(plain);

	g_assert_cmpstr(a, ==, "ui://draw/view.html");
	g_assert_cmpstr(b, ==, "ui://draw/old.html");

	/* Only a ui:// resource is a view; a web page is not one. */
	g_assert_null(c);
	g_assert_null(d);
}

static void
test_visibility(void)
{
	g_autoptr(JsonObject) app_only = parse(
		"{\"name\":\"save\",\"_meta\":{\"ui\":{\"visibility\":[\"app\"]}}}");
	g_autoptr(JsonObject) both = parse(
		"{\"name\":\"draw\",\"_meta\":{\"ui\":{\"visibility\":[\"model\","
		"\"app\"]}}}");
	g_autoptr(JsonObject) plain = parse("{\"name\":\"list\"}");

	g_assert_false(clawt_mcp_app_tool_model_visible(app_only));
	g_assert_true(clawt_mcp_app_tool_model_visible(both));
	g_assert_true(clawt_mcp_app_tool_model_visible(plain));
}

/* ── The policy and the document ─────────────────────────────────── */

static void
test_domain_filter(void)
{
	static const struct {
		const gchar *entry;
		gboolean     websocket;
		gboolean     loopback;
		gboolean     ok;
	} cases[] = {
		{ "https://esm.sh",                FALSE, FALSE, TRUE  },
		{ "https://cdn.example.com:8443",  FALSE, FALSE, TRUE  },
		{ "https://*.example.com",         FALSE, FALSE, TRUE  },
		{ "wss://live.example.com",        TRUE,  FALSE, TRUE  },
		{ "wss://live.example.com",        FALSE, FALSE, FALSE },
		{ "https://example.com/path",      FALSE, FALSE, FALSE },
		{ "https://example.com?x=1",       FALSE, FALSE, FALSE },
		{ "https://example.com;",          FALSE, FALSE, FALSE },
		{ "https://exa mple.com",          FALSE, FALSE, FALSE },
		{ "https://example.com'",          FALSE, FALSE, FALSE },
		{ "https://example.com\"",         FALSE, FALSE, FALSE },
		{ "https://example.com,https://x.y", FALSE, FALSE, FALSE },
		{ "https://localhost",             FALSE, FALSE, FALSE },
		{ "https://intranet",              FALSE, FALSE, FALSE },
		{ "*",                             FALSE, FALSE, FALSE },
		{ "http://example.com",            FALSE, FALSE, FALSE },
		{ "http://example.com",            FALSE, TRUE,  FALSE },
		{ "http://localhost:3000",         FALSE, FALSE, FALSE },
		{ "http://localhost:3000",         FALSE, TRUE,  TRUE  },
		{ "http://127.0.0.1",              FALSE, TRUE,  TRUE  },
		{ "https://example.com:",          FALSE, FALSE, FALSE },
		{ "'self'",                        FALSE, FALSE, FALSE },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		gboolean got = clawt_mcp_app_domain_ok(cases[i].entry,
		                                       cases[i].websocket,
		                                       cases[i].loopback);

		if (got != cases[i].ok)
			g_error("%s (ws %d, loopback %d): expected %d, got %d",
			        cases[i].entry, cases[i].websocket, cases[i].loopback,
			        cases[i].ok, got);
	}
}

static void
test_csp(void)
{
	g_autoptr(GString) many = g_string_new(
		"{\"csp\":{\"connectDomains\":[\"wss://live.example.com\"],"
		"\"resourceDomains\":[\"https://esm.sh\",\"https://evil.com/x\","
		"\"https://a.b; script-src *\"");
	g_autoptr(JsonObject) ui = NULL;
	g_auto(GStrv) refused = NULL;
	g_autofree gchar *policy = NULL;
	g_autofree gchar *strict = NULL;
	guint i;

	/* Past sixteen, the rest are refused however good they are. */
	for (i = 0; i < 20; i++)
		g_string_append_printf(many, ",\"https://cdn%u.example.com\"", i);

	g_string_append(many, "]}}");
	ui = parse(many->str);
	policy = clawt_mcp_app_build_csp(ui, FALSE, &refused);

	g_assert_true(g_str_has_prefix(policy, "default-src 'none'; "));
	g_assert_nonnull(strstr(policy, "script-src 'unsafe-inline' 'unsafe-eval' "
	                                "'wasm-unsafe-eval' https://esm.sh"));
	g_assert_nonnull(strstr(policy, "connect-src wss://live.example.com;"));
	g_assert_nonnull(strstr(policy, "frame-src 'none'"));
	g_assert_nonnull(strstr(policy, "form-action 'none'"));
	g_assert_nonnull(strstr(policy, "base-uri 'none'"));
	g_assert_null(strstr(policy, "'self'"));
	g_assert_null(strstr(policy, "evil.com"));
	g_assert_null(strstr(policy, "script-src *"));
	g_assert_nonnull(strstr(policy, "https://cdn14.example.com"));
	g_assert_null(strstr(policy, "https://cdn15.example.com"));

	g_assert_true(g_strv_contains((const gchar * const *)refused,
	                              "https://evil.com/x"));
	g_assert_true(g_strv_contains((const gchar * const *)refused,
	                              "https://cdn19.example.com"));

	/* A view that asks for nothing can reach nothing. */
	strict = clawt_mcp_app_build_csp(NULL, FALSE, NULL);
	g_assert_nonnull(strstr(strict, "connect-src 'none';"));
	g_assert_nonnull(strstr(strict, "img-src data: blob:;"));
}

static void
test_document(void)
{
	g_autofree gchar *policy = clawt_mcp_app_build_csp(NULL, FALSE, NULL);
	g_autofree gchar *document = clawt_mcp_app_build_document(
		"\xef\xbb\xbf  <!doctype html><html><body>hi</body></html>", policy);
	const gchar *start = "<!DOCTYPE html><meta http-equiv=\"Content-Security-"
	                     "Policy\" content=\"";

	/* The policy is the first element; nothing of the view's is before it. */
	g_assert_true(g_str_has_prefix(document, start));
	g_assert_null(strstr(document, "<!doctype"));
	g_assert_null(strstr(document, "\xef\xbb\xbf"));
	g_assert_nonnull(strstr(document, "<html><body>hi</body></html>"));

	/* The policy's quotes cannot end the attribute it sits in. */
	{
		g_autofree gchar *hostile = clawt_mcp_app_build_document(
			"x", "default-src 'none'\"><script>alert(1)</script>");

		g_assert_null(strstr(hostile, "\"><script>"));
		g_assert_nonnull(strstr(hostile, "&quot;&gt;&lt;script&gt;"));
	}
}

static void
test_ids_and_links(void)
{
	g_autofree gchar *id = clawt_mcp_app_new_id();
	g_autofree gchar *path = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *label = clawt_mcp_app_link_label("Excalidraw");
	g_autofree gchar *bare = clawt_mcp_app_link_label(NULL);

	g_assert_true(clawt_mcp_app_id_is_valid(id));
	g_assert_false(clawt_mcp_app_id_is_valid("../a0000000000000000"));
	g_assert_false(clawt_mcp_app_id_is_valid("A0000000000000000"));
	g_assert_false(clawt_mcp_app_id_is_valid(NULL));

	path = clawt_mcp_app_web_path("oryx", "a0123456789abcdef");
	g_assert_cmpstr(path, ==, "/a/oryx/app/a0123456789abcdef");

	url = clawt_mcp_app_web_url("http://box.tail:9000/", "oryx",
	                            "a0123456789abcdef");
	g_assert_cmpstr(url, ==, "http://box.tail:9000/a/oryx/app/a0123456789abcdef");

	fallback = clawt_mcp_app_web_url(NULL, "oryx", "a0123456789abcdef");
	g_assert_true(g_str_has_prefix(fallback, "http://127.0.0.1:8790/a/"));

	g_assert_cmpstr(label, ==, "Open Excalidraw in the browser");
	g_assert_cmpstr(bare, ==, "Open the app in the browser");
}

/* ── The relay's side ────────────────────────────────────────────── */

typedef struct {
	GPtrArray *reports;
} Seen;

static void
on_seen(JsonObject *report, gpointer user_data)
{
	Seen *seen = user_data;

	g_ptr_array_add(seen->reports, json_object_ref(report));
}

#define TOOL_LIST \
	"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[" \
	"{\"name\":\"draw\",\"title\":\"Draw a diagram\",\"_meta\":{\"ui\":" \
	"{\"resourceUri\":\"ui://draw/view.html\"}}}," \
	"{\"name\":\"save_checkpoint\",\"_meta\":{\"ui\":{\"visibility\":" \
	"[\"app\"]}}}," \
	"{\"name\":\"list\"}]}}"

#define VIEW_ANSWER(id) \
	"{\"jsonrpc\":\"2.0\",\"id\":\"" id "\",\"result\":{\"contents\":[{" \
	"\"uri\":\"ui://draw/view.html\",\"mimeType\":" \
	"\"text/html;profile=mcp-app\",\"text\":\"<p>diagram</p>\"," \
	"\"_meta\":{\"ui\":{\"csp\":{\"resourceDomains\":[\"https://esm.sh\"]}}}" \
	"}]}}"

static void
test_relay_flow(void)
{
	g_autoptr(ClawtMcpApps) apps = clawt_mcp_apps_new("draw", FALSE);
	ClawtMcpRelayPolicy policy = { 0 };
	Seen seen = { g_ptr_array_new_with_free_func(
		(GDestroyNotify)json_object_unref) };
	g_autofree gchar *to_server = NULL;
	g_autofree gchar *to_client = NULL;
	g_autoptr(JsonObject) sent = NULL;
	JsonObject *report;

	clawt_mcp_apps_set_seen_func(apps, on_seen, &seen);
	policy.apps = apps;

	/* initialize says the client can show apps. */
	clawt_mcp_relay_policy_outbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
		"\"params\":{\"capabilities\":{}}}", &to_server, &to_client);
	sent = parse(to_server);
	{
		JsonObject *extensions = json_object_get_object_member(
			json_object_get_object_member(
				json_object_get_object_member(sent, "params"),
				"capabilities"),
			"extensions");
		JsonObject *ui = json_object_get_object_member(
			extensions, CLAWT_MCP_APP_EXTENSION);

		g_assert_cmpstr(json_array_get_string_element(
			json_object_get_array_member(ui, "mimeTypes"), 0), ==,
			CLAWT_MCP_APP_MIME);
	}
	g_clear_pointer(&to_server, g_free);

	/* The tool list: the app's own tool is not the model's to see ... */
	clawt_mcp_relay_policy_inbound(&policy, TOOL_LIST, &to_client,
	                               &to_server);
	g_assert_nonnull(to_client);
	g_assert_null(to_server);
	g_assert_nonnull(strstr(to_client, "\"draw\""));
	g_assert_nonnull(strstr(to_client, "\"list\""));
	g_assert_null(strstr(to_client, "save_checkpoint"));
	g_clear_pointer(&to_client, g_free);

	/* ... nor to call: a wall with its door. */
	clawt_mcp_relay_policy_outbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"save_checkpoint\",\"arguments\":{}}}",
		&to_server, &to_client);
	g_assert_null(to_server);
	g_assert_nonnull(strstr(to_client, "belongs to an app"));
	g_clear_pointer(&to_client, g_free);

	/* A call of the tool with a view goes through untouched. */
	clawt_mcp_relay_policy_outbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"draw\",\"arguments\":{\"shape\":\"box\"}}}",
		&to_server, &to_client);
	g_assert_nonnull(to_server);
	g_assert_null(to_client);
	g_clear_pointer(&to_server, g_free);

	/* Its answer reaches the client, and the relay asks for the view. */
	clawt_mcp_relay_policy_inbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{\"type\":"
		"\"text\",\"text\":\"drawn\"}]}}", &to_client, &to_server);
	g_assert_nonnull(strstr(to_client, "drawn"));
	g_assert_nonnull(to_server);
	g_assert_nonnull(strstr(to_server, "\"method\":\"resources/read\""));
	g_assert_nonnull(strstr(to_server, "ui://draw/view.html"));
	g_assert_cmpuint(seen.reports->len, ==, 0);
	g_clear_pointer(&to_client, g_free);
	g_clear_pointer(&to_server, g_free);

	/* The view's answer is the relay's own: kept from the client. */
	clawt_mcp_relay_policy_inbound(&policy,
	                               VIEW_ANSWER("clawtilla-app-read-1"),
	                               &to_client, &to_server);
	g_assert_null(to_client);
	g_assert_null(to_server);
	g_assert_cmpuint(seen.reports->len, ==, 1);

	report = g_ptr_array_index(seen.reports, 0);
	g_assert_cmpstr(json_object_get_string_member(report, "connector"), ==,
	                "draw");
	g_assert_cmpstr(json_object_get_string_member(report, "tool"), ==, "draw");
	g_assert_cmpstr(json_object_get_string_member(report, "title"), ==,
	                "Draw a diagram");
	g_assert_cmpstr(json_object_get_string_member(report, "html"), ==,
	                "<p>diagram</p>");
	g_assert_nonnull(strstr(json_object_get_string_member(report, "csp"),
	                        "https://esm.sh"));
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(report, "input"), "shape"), ==, "box");
	g_assert_true(json_object_has_member(report, "result"));

	/* A second call uses the view already read: no second read. */
	clawt_mcp_relay_policy_outbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"draw\",\"arguments\":{}}}",
		&to_server, &to_client);
	g_clear_pointer(&to_server, g_free);
	clawt_mcp_relay_policy_inbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[]}}",
		&to_client, &to_server);
	g_assert_null(to_server);
	g_assert_cmpuint(seen.reports->len, ==, 2);
	g_clear_pointer(&to_client, g_free);

	/* A tool with no view is never reported. */
	clawt_mcp_relay_policy_outbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
		"\"params\":{\"name\":\"list\",\"arguments\":{}}}",
		&to_server, &to_client);
	g_clear_pointer(&to_server, g_free);
	clawt_mcp_relay_policy_inbound(
		&policy,
		"{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"content\":[]}}",
		&to_client, &to_server);
	g_assert_null(to_server);
	g_assert_cmpuint(seen.reports->len, ==, 2);

	g_ptr_array_unref(seen.reports);
}

static void
test_blocked_views(void)
{
	g_autoptr(ClawtMcpApps) apps = clawt_mcp_apps_new("draw", FALSE);
	Seen seen = { g_ptr_array_new_with_free_func(
		(GDestroyNotify)json_object_unref) };
	g_autofree gchar *line = NULL;
	g_autofree gchar *to_server = NULL;
	g_autofree gchar *to_client = NULL;
	g_autofree gchar *encoded = g_base64_encode((const guchar *)"<b>blob</b>",
	                                            11);
	g_autofree gchar *blob_answer = NULL;
	JsonObject *report;

	clawt_mcp_apps_set_seen_func(apps, on_seen, &seen);
	line = clawt_mcp_apps_inbound(apps, TOOL_LIST, &to_server);
	g_clear_pointer(&line, g_free);

	/* A view sent as a base64 blob is decoded. */
	clawt_mcp_apps_outbound(apps,
	                        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":"
	                        "\"tools/call\",\"params\":{\"name\":\"draw\"}}",
	                        &to_server, &to_client);
	g_clear_pointer(&to_server, g_free);
	line = clawt_mcp_apps_inbound(apps,
	                              "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":"
	                              "{\"content\":[]}}", &to_server);
	g_assert_nonnull(to_server);
	g_clear_pointer(&line, g_free);
	g_clear_pointer(&to_server, g_free);

	blob_answer = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":\"clawtilla-app-read-1\",\"result\":"
		"{\"contents\":[{\"uri\":\"ui://draw/view.html\",\"mimeType\":"
		"\"text/html;profile=mcp-app\",\"blob\":\"%s\"}]}}", encoded);
	line = clawt_mcp_apps_inbound(apps, blob_answer, &to_server);
	g_assert_null(line);
	report = g_ptr_array_index(seen.reports, 0);
	g_assert_cmpstr(json_object_get_string_member(report, "html"), ==,
	                "<b>blob</b>");

	/* A server that answers with no HTML is still a call, marked blocked. */
	{
		g_autoptr(ClawtMcpApps) other = clawt_mcp_apps_new("draw", FALSE);

		clawt_mcp_apps_set_seen_func(other, on_seen, &seen);
		line = clawt_mcp_apps_inbound(other, TOOL_LIST, &to_server);
		g_clear_pointer(&line, g_free);
		clawt_mcp_apps_outbound(other,
		                        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
		                        "\"tools/call\",\"params\":{\"name\":"
		                        "\"draw\"}}", &to_server, &to_client);
		g_clear_pointer(&to_server, g_free);
		line = clawt_mcp_apps_inbound(other,
		                              "{\"jsonrpc\":\"2.0\",\"id\":1,"
		                              "\"result\":{\"content\":[]}}",
		                              &to_server);
		g_clear_pointer(&line, g_free);
		g_clear_pointer(&to_server, g_free);
		line = clawt_mcp_apps_inbound(other,
		                              "{\"jsonrpc\":\"2.0\",\"id\":"
		                              "\"clawtilla-app-read-1\",\"error\":"
		                              "{\"code\":-32002,\"message\":\"no\"}}",
		                              &to_server);
		g_assert_null(line);
		report = g_ptr_array_index(seen.reports, 1);
		g_assert_false(json_object_has_member(report, "html"));
		g_assert_nonnull(json_object_get_string_member(report, "blocked"));
	}

	g_ptr_array_unref(seen.reports);
}

/* ── The store ───────────────────────────────────────────────────── */

static void
test_store(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = g_dir_make_tmp("clawt-mcp-app-XXXXXX", &error);
	g_autofree gchar *mine = g_build_filename(root, "oryx", "mcp-apps", NULL);
	g_autofree gchar *theirs = g_build_filename(root, "kudu", "mcp-apps",
	                                            NULL);
	g_autoptr(JsonObject) record = parse("{\"tool\":\"draw\","
	                                     "\"html\":\"<p>x</p>\"}");
	g_autoptr(JsonObject) loaded = NULL;
	g_autoptr(JsonObject) missing = NULL;
	g_autofree gchar *path = NULL;
	const gchar *id = "a0123456789abcdef";
	GStatBuf st;

	g_assert_no_error(error);
	g_assert_true(clawt_mcp_app_save(mine, id, record, &error));
	g_assert_no_error(error);

	path = g_build_filename(mine, "a0123456789abcdef.json", NULL);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_stat(mine, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0700);

	loaded = clawt_mcp_app_load(mine, id, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(loaded, "html"), ==,
	                "<p>x</p>");

	/* Another agent's conversation has no such app, whatever the id. */
	missing = clawt_mcp_app_load(theirs, id, &error);
	g_assert_null(missing);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_assert_null(clawt_mcp_app_load(mine, "../oryx/mcp-apps/x", &error));
	g_clear_error(&error);

	/* Kept for thirty days. */
	g_assert_cmpuint(clawt_mcp_app_sweep(mine, g_get_real_time() /
	                                     G_USEC_PER_SEC + 31 * 86400,
	                                     CLAWT_MCP_APP_KEEP_COUNT,
	                                     CLAWT_MCP_APP_KEEP_SECONDS), ==, 1);

	clawt_test_remove_tree(root);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

	g_test_add_func("/mcp-app/tool-resource", test_tool_resource);
	g_test_add_func("/mcp-app/visibility", test_visibility);
	g_test_add_func("/mcp-app/domain-filter", test_domain_filter);
	g_test_add_func("/mcp-app/csp", test_csp);
	g_test_add_func("/mcp-app/document", test_document);
	g_test_add_func("/mcp-app/ids-and-links", test_ids_and_links);
	g_test_add_func("/mcp-app/relay-flow", test_relay_flow);
	g_test_add_func("/mcp-app/blocked-views", test_blocked_views);
	g_test_add_func("/mcp-app/store", test_store);

	return g_test_run();
}
