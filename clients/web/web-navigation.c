/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"


/* GET works without scripting and makes a query bookmarkable. This page
 * does not subscribe to fleet events, so results never steal typing focus. */
static HtmxResponse *
on_navigation(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	ClawtWebApp *app = (ClawtWebApp *)user_data;
	const gchar *query = htmx_request_get_query_param(request, "q");
	const gchar *selected = htmx_request_get_query_param(request, "agent");
	const gchar *room_id = htmx_request_get_query_param(request, "room");
	g_autoptr(HtmxDiv) body = htmx_div_new();
	g_autoptr(HtmxForm) form = htmx_form_new();
	g_autoptr(HtmxInput) entry = htmx_input_new(HTMX_INPUT_SEARCH);
	g_autoptr(HtmxDiv) results = htmx_div_new();
	g_autoptr(JsonNode) agents_reply = NULL;
	g_autoptr(JsonNode) rooms_reply = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *summary = NULL;
	JsonArray *agents = NULL;
	JsonArray *rooms = NULL;
	guint count = 0;
	guint i;

	(void)params;
	if (selected != NULL && *selected == '\0')
		selected = NULL;
	back = clawt_web_agent_url(selected, CLAWT_PAGE_CHAT);
	if (room_id != NULL && *room_id != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(room_id, NULL, FALSE);
		g_free(back);
		back = g_strdup_printf("/r/%s", escaped);
	}
	htmx_element_add_class(HTMX_ELEMENT(body), "navigation-page");
	{
		HtmxA *link = htmx_a_new_with_href(back);
		htmx_node_set_text_content(HTMX_NODE(link), "Back to conversations");
		htmx_element_add_class(HTMX_ELEMENT(link), "btn btn-default");
		clawt_web_add(body, link);
	}
	clawt_web_add(body, clawt_web_section_title("Go to…"));
	clawt_web_add(body, clawt_web_text("Find agents, rooms and pages by name, ID or team.", "muted"));
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/navigate");
	htmx_element_add_class(HTMX_ELEMENT(form), "navigation-search");
	htmx_element_set_attribute(HTMX_ELEMENT(entry), "name", "q");
	htmx_element_set_id(HTMX_ELEMENT(entry), "navigation-query");
	htmx_element_set_attribute(HTMX_ELEMENT(entry), "aria-label", "Find agents, rooms and pages");
	htmx_element_set_attribute(HTMX_ELEMENT(entry), "placeholder", "Search destinations…");
	htmx_element_set_attribute(HTMX_ELEMENT(entry), "value", query != NULL ? query : "");
	htmx_element_set_attribute(HTMX_ELEMENT(entry), "autofocus", "autofocus");
	htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(entry));
	if (room_id != NULL) {
		HtmxInput *room = htmx_input_new_hidden("room", room_id);
		clawt_web_add(form, room);
	}
	if (selected != NULL) {
		HtmxInput *agent = htmx_input_new(HTMX_INPUT_HIDDEN);
		htmx_element_set_attribute(HTMX_ELEMENT(agent), "name", "agent");
		htmx_element_set_attribute(HTMX_ELEMENT(agent), "value", selected);
		clawt_web_add(form, agent);
	}
	{
		HtmxButton *button = clawt_web_button("Search", "primary");
		htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
		clawt_web_add(form, button);
	}
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

	agents_reply = clawt_web_app_call(app, "agent.list", NULL);
	rooms_reply = clawt_web_app_call(app, "room.list", NULL);
	if (agents_reply != NULL)
		agents = clawt_web_member_array(clawt_web_root(agents_reply), "agents");
	if (rooms_reply != NULL)
		rooms = clawt_web_member_array(clawt_web_root(rooms_reply), "rooms");
	if (agents_reply == NULL || rooms_reply == NULL)
		clawt_web_add(body, clawt_web_empty("Some destinations are unavailable", "Search again to retry. Page navigation is still available."));
	htmx_element_add_class(HTMX_ELEMENT(results), "navigation-results");
	for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
		JsonObject *agent = json_array_get_object_element(agents, i);
		const gchar *id = clawt_web_member(agent, "id", "");
		g_autofree gchar *url = clawt_web_agent_url(id, CLAWT_PAGE_CHAT);
		g_autofree gchar *detail = g_strdup_printf("Agent · %s · %s", id, clawt_web_member(agent, "team", ""));
		count += clawt_web_navigation_result(HTMX_ELEMENT(results), query, clawt_web_member(agent, "name", id), detail, id, url);
	}
	for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
		JsonObject *room = json_array_get_object_element(rooms, i);
		const gchar *id = clawt_web_member(room, "id", "");
		g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
		g_autofree gchar *url = g_strdup_printf("/r/%s", escaped);
		g_autofree gchar *detail = g_strdup_printf("Room · %s · %s", id, clawt_web_member(room, "team", ""));
		if (clawt_web_member_bool(room, "declared", FALSE))
			count += clawt_web_navigation_result(HTMX_ELEMENT(results), query, clawt_web_member(room, "name", id), detail, id, url);
	}
	for (i = 0; i < clawt_section_count(); i++) {
		ClawtSection section = clawt_section_nth(i);
		guint j;
		for (j = 0; j < clawt_section_page_count(section); j++) {
			ClawtPage page = clawt_section_page_nth(section, j);
			g_autofree gchar *url = NULL;
			if (selected == NULL && page != CLAWT_PAGE_MEMORY)
				continue;
			url = page == CLAWT_PAGE_MEMORY ? g_strdup("/memory") :
				clawt_web_agent_url(selected, page);
			count += clawt_web_navigation_result(HTMX_ELEMENT(results), query, clawt_page_label(page), clawt_section_label(section), clawt_page_nick(page), url);
		}
	}
	summary = g_strdup_printf("%u destination%s", count, count == 1 ? "" : "s");
	clawt_web_add(body, clawt_web_text(summary, "muted navigation-count"));
	if (count == 0)
		clawt_web_add(results, clawt_web_empty("No matches", "Try a name, ID, team or page, or clear the search."));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(results));
	html = clawt_web_shell_page(app, "Go to…", HTMX_ELEMENT(body), request);
	return clawt_web_html_response(html);
}

/* Registered with fleet routes, before the agent catch-all. */
void
clawt_web_register_navigation(HtmxRouter *router, ClawtWebApp *app)
{
	htmx_router_get(router, "/navigate", on_navigation, app);
}
