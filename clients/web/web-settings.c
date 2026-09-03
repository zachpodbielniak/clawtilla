/*
 * web-settings.c - Everything behind the Settings link
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same six pages the GTK client's preferences dialog has: VM images,
 * teams, spending, integrations, connectors and appearance.  Appearance
 * is the one that belongs to the browser rather than to the fleet, for
 * the same reason it belongs to the client there -- fonts and a theme are
 * about the screen somebody is sitting at, not about the agents.
 */

#include "web-pages.h"

#include <string.h>

typedef struct {
    const gchar *slug;
    const gchar *title;
    const gchar *summary;
} SettingsPage;

static const SettingsPage settings_pages[] = {
    { "fleet",        "Fleet",
      "Hold the fleet before a restart: stop delivery, let the turns "
      "that are running finish." },
    { "images",       "VM images",
      "Disk images a VM agent can boot. clawtilla ships none: a VM with "
      "no image defines, starts, and boots nothing." },
    { "teams",        "Teams",
      "Who may hand work to whom." },
    { "folders",      "Shared folders",
      "Directories every agent's computer gets." },
    { "spending",     "Spending",
      "What the fleet has cost, per agent." },
    { "integrations", "Integrations",
      "Matrix, email, webhooks and notifications." },
    { "connectors",   "Connectors",
      "Outside services an agent can reach, and the credentials for them." },
    { "appearance",   "Appearance",
      "How this browser draws the client." },
    { "connections",  "Connections",
      "Which daemon this server talks to." }
};

/* ── The frame ───────────────────────────────────────────────────── */

static HtmxElement *
settings_shell(const gchar *current, HtmxElement *content)
{
    g_autoptr(HtmxDiv) page = htmx_div_new();
    g_autoptr(HtmxElement) nav = HTMX_ELEMENT(htmx_nav_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(page), "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Settings"));

    htmx_element_add_class(nav, "tabs");

    {
        g_autoptr(HtmxA) back = htmx_a_new_with_href("/");

        htmx_element_add_class(HTMX_ELEMENT(back), "tab");
        htmx_node_set_text_content(HTMX_NODE(back), "← Fleet");
        htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(back));
    }

    for (i = 0; i < G_N_ELEMENTS(settings_pages); i++) {
        g_autofree gchar *url = g_strdup_printf("/settings/%s",
                                                settings_pages[i].slug);
        g_autoptr(HtmxA) tab = htmx_a_new_with_href(url);

        htmx_element_add_class(HTMX_ELEMENT(tab), "tab");
        htmx_node_set_text_content(HTMX_NODE(tab), settings_pages[i].title);

        if (g_strcmp0(settings_pages[i].slug, current) == 0)
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current",
                                       "page");

        htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(tab));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(nav));

    for (i = 0; i < G_N_ELEMENTS(settings_pages); i++) {
        if (g_strcmp0(settings_pages[i].slug, current) != 0)
            continue;

        if (settings_pages[i].summary != NULL)
            clawt_web_add(pad, clawt_web_text(settings_pages[i].summary,
                                              "lede"));
        break;
    }

    if (content != NULL)
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(content));

    htmx_node_add_child(HTMX_NODE(page), HTMX_NODE(pad));

    return HTMX_ELEMENT(g_steal_pointer(&page));
}

/*
 * Renders one settings page.
 *
 * @content is borrowed, not taken: htmx_node_add_child() refs what it is
 * given, so the caller's g_autoptr still owns its own reference and
 * releases it at the end of the scope. Handing this a stolen pointer --
 * which half these call sites did -- leaks the element, because nothing
 * here ever drops that second reference.
 */
static HtmxResponse *
settings_response(ClawtWebApp *app, HtmxRequest *request, const gchar *slug,
                  HtmxElement *content, const gchar *toast, gboolean bad)
{
    g_autoptr(HtmxDiv) wrap = htmx_div_new();
    g_autoptr(HtmxElement) shell = NULL;
    g_autofree gchar *html = NULL;
    const gchar *title = slug;
    guint i;

    /*
     * Copied first, for the same reason page_with_banner() copies: a
     * caller passes clawt_web_app_last_error(), and the content it
     * builds beforehand has already made calls of its own that freed it.
     */
    g_autofree gchar *said = g_strdup(toast);

    if (said != NULL) {
        g_autoptr(HtmxDiv) note = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(note), "toast");

        if (bad)
            htmx_element_add_class(HTMX_ELEMENT(note), "notice-bad");

        htmx_node_set_text_content(HTMX_NODE(note), said);
        htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(note));
    }

    shell = settings_shell(slug, content);
    htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(shell));

    for (i = 0; i < G_N_ELEMENTS(settings_pages); i++) {
        if (g_strcmp0(settings_pages[i].slug, slug) == 0)
            title = settings_pages[i].title;
    }

    html = clawt_web_shell_page(app, title, HTMX_ELEMENT(wrap), request);

    return clawt_web_html_response(html);
}

/* ── VM images ───────────────────────────────────────────────────── */

static gchar *
human_size(gint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return g_strdup_printf("%.1f GB", (gdouble)bytes / (1024.0 * 1024 * 1024));
    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.0f MB", (gdouble)bytes / (1024.0 * 1024));

    return g_strdup_printf("%" G_GINT64_FORMAT " B", bytes);
}

static HtmxElement *
images_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) cached = clawt_web_app_call(app, "image.vm_list", NULL);
    g_autoptr(JsonNode) catalog = clawt_web_app_call(app, "image.vm_catalog",
                                                     NULL);
    JsonArray *images = clawt_web_member_array(clawt_web_root(cached),
                                               "images");
    JsonArray *sources = clawt_web_member_array(clawt_web_root(catalog),
                                                "sources");
    guint i;

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("On this machine", NULL);
        HtmxElement *body = clawt_web_card_body(card);

        if (images == NULL || json_array_get_length(images) == 0) {
            clawt_web_add(body, clawt_web_empty(
                "No images downloaded",
                "Fetch one below. Until then a VM agent has no disk and "
                "cannot boot."));
        }

        for (i = 0; images != NULL && i < json_array_get_length(images); i++) {
            JsonObject *image = json_array_get_object_element(images, i);
            const gchar *name = clawt_web_member(image, "name", "?");
            gboolean downloading = clawt_web_member_bool(image, "downloading",
                                                         FALSE);
            gint64 got = clawt_web_member_int(image, "bytes", 0);
            gint64 total = clawt_web_member_int(image, "total", 0);
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
            g_autofree gchar *size = human_size(got);

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(label),
                                       "list-item-title");
                htmx_node_set_text_content(HTMX_NODE(label), name);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
            }

            if (downloading) {
                g_autofree gchar *progress = (total > 0)
                    ? g_strdup_printf("%" G_GINT64_FORMAT "%%",
                                      got * 100 / total)
                    : g_strdup("downloading");

                clawt_web_add(head, clawt_web_badge(progress, "info"));
            } else {
                clawt_web_add(head, clawt_web_badge(size, "neutral"));
            }

            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

            clawt_web_add(row, clawt_web_text(
                clawt_web_member(image, "path", ""), "small muted mono"));

            {
                g_autoptr(HtmxDiv) actions = htmx_div_new();
                g_autofree gchar *action = g_strdup_printf(
                    downloading ? "/settings/images/%s/cancel"
                                : "/settings/images/%s/remove", escaped);

                htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
                clawt_web_add(actions, clawt_web_post_button(
                    downloading ? "Cancel" : "Remove", action,
                    downloading ? "default" : "danger",
                    downloading ? NULL
                    /*
                     * Deleting an image an agent's overlay was made from
                     * stops that VM booting, with an error from qemu
                     * about a missing backing file a long way from this
                     * button. The daemon refuses it; the confirmation is
                     * so nobody is surprised by the refusal.
                     */
                    : "Remove this image? An agent whose disk was made "
                      "from it will stop booting."));
                htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
            }

            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Fetch an image",
            "Cloud images only. An Arch `basic` qcow2 boots perfectly and "
            "admits nobody, because it has no cloud-init -- which is "
            "indistinguishable from a VM that failed to boot.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/images/download");
        g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);

        for (i = 0; sources != NULL && i < json_array_get_length(sources);
             i++) {
            JsonObject *source = json_array_get_object_element(sources, i);
            const gchar *id = clawt_web_member(source, "id", NULL);
            g_autofree gchar *label = NULL;

            if (id == NULL)
                continue;

            label = g_strdup_printf("%s — %s",
                                    clawt_web_member(source, "name", id),
                                    clawt_web_member(source, "note", ""));

            g_ptr_array_add(ids, g_strdup(id));
            g_ptr_array_add(names, g_steal_pointer(&label));
        }

        g_ptr_array_add(ids, NULL);
        g_ptr_array_add(names, NULL);

        clawt_web_add(form, clawt_web_select_field(
            "From the catalog", "name",
            (const gchar *const *)ids->pdata,
            (const gchar *const *)names->pdata, NULL));
        clawt_web_add(form, clawt_web_field(
            "Or a URL", "url", NULL,
            "https://example.org/some-cloud-image.qcow2"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) go = clawt_web_button("Download",
                                                        "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Teams ───────────────────────────────────────────────────────── */

static HtmxElement *
teams_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "team.list", NULL);
    JsonArray *teams = clawt_web_member_array(clawt_web_root(reply), "teams");
    guint i;

    /*
     * Fleet-level mistakes -- two leads on one team, an agent naming a
     * team nobody declared -- are warnings rather than errors, because a
     * fleet is edited by hand and half-built states are ordinary. The
     * symptom otherwise is work quietly going nowhere.
     */
    clawt_web_warnings(HTMX_ELEMENT(box), reply);

    if (teams == NULL || json_array_get_length(teams) == 0)
        clawt_web_add(box, clawt_web_empty(
            "No teams",
            "Without one, only the chief of staff can hand work out."));

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *id = clawt_web_member(team, "id", "?");
        g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/settings/teams/%s/save",
                                                   escaped);
        g_autofree gchar *remove = g_strdup_printf("/settings/teams/%s/remove",
                                                   escaped);
        g_autofree gchar *members = NULL;
        g_autoptr(HtmxDiv) card = NULL;
        HtmxElement *body;
        g_autoptr(HtmxForm) form = NULL;
        JsonArray *member_array = clawt_web_member_array(team, "members");
        g_autoptr(GString) joined = g_string_new(NULL);
        guint m;

        for (m = 0; member_array != NULL &&
                    m < json_array_get_length(member_array); m++)
            g_string_append_printf(joined, "%s%s", m > 0 ? ", " : "",
                                   json_array_get_string_element(member_array,
                                                                 m));

        members = g_strdup_printf(
            "%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " running · %s",
            clawt_web_member_int(team, "running", 0),
            clawt_web_member_int(team, "total", 0),
            joined->len > 0 ? joined->str : "no members");

        card = clawt_web_card(clawt_web_member(team, "name", id), members);
        body = clawt_web_card_body(card);
        form = clawt_web_form(action);

        clawt_web_add(form, clawt_web_field(
            "Name", "name", clawt_web_member(team, "name", ""), NULL));
        clawt_web_add(form, clawt_web_field(
            "Description", "description",
            clawt_web_member(team, "description", ""),
            "What this team is for. The agents read it."));
        clawt_web_add(form, clawt_web_field(
            "Colour", "color", clawt_web_member(team, "color", ""), NULL));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");
            g_autofree gchar *confirm = g_strdup_printf(
                "Remove the team %s? Its agents stay, without a team.", id);

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
            clawt_web_add(row, clawt_web_post_button("Remove", remove,
                                                     "danger", confirm));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("New team", NULL);
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/teams/add");

        clawt_web_add(form, clawt_web_field("Id", "id", NULL, "research"));
        clawt_web_add(form, clawt_web_field("Name", "name", NULL,
                                            "Research"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Create", "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Spending ────────────────────────────────────────────────────── */

static const struct {
    const gchar *slug;
    const gchar *label;
    gint64       seconds;
} periods[] = {
    { "all",   "All time",    0 },
    { "today", "Today",       0 },
    { "7",     "Last 7 days", 7 * 86400 },
    { "30",    "Last 30 days", 30 * 86400 }
};

static gint64
since_for(const gchar *slug)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (g_strcmp0(slug, "today") == 0) {
        g_autoptr(GDateTime) midnight = NULL;
        g_autoptr(GDateTime) local = g_date_time_new_now_local();

        midnight = g_date_time_new_local(g_date_time_get_year(local),
                                         g_date_time_get_month(local),
                                         g_date_time_get_day_of_month(local),
                                         0, 0, 0.0);

        return g_date_time_to_unix(midnight);
    }

    for (i = 0; i < G_N_ELEMENTS(periods); i++) {
        if (g_strcmp0(periods[i].slug, slug) == 0 && periods[i].seconds > 0)
            return now - periods[i].seconds;
    }

    return 0;
}

static HtmxElement *
spending_content(ClawtWebApp *app, const gchar *period)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *agents;
    JsonObject *total;
    g_autoptr(HtmxElement) wrap = HTMX_ELEMENT(htmx_div_new());
    g_autoptr(HtmxTable) table = htmx_table_new();
    g_autoptr(HtmxThead) head = htmx_thead_new();
    g_autoptr(HtmxTbody) rows = htmx_tbody_new();
    g_autoptr(HtmxTfoot) foot = htmx_tfoot_new();
    guint i;

    {
        g_autoptr(HtmxElement) nav = HTMX_ELEMENT(htmx_nav_new());

        htmx_element_add_class(nav, "tabs");

        for (i = 0; i < G_N_ELEMENTS(periods); i++) {
            g_autofree gchar *url = g_strdup_printf(
                "/settings/spending?period=%s", periods[i].slug);
            g_autoptr(HtmxA) tab = htmx_a_new_with_href(url);

            htmx_element_add_class(HTMX_ELEMENT(tab), "tab");
            htmx_node_set_text_content(HTMX_NODE(tab), periods[i].label);

            if (g_strcmp0(periods[i].slug, period) == 0)
                htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current",
                                           "page");

            htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(tab));
        }

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(nav));
    }

    clawt_web_payload_set_int(payload, "since", since_for(period));
    reply = clawt_web_app_call(app, "usage.summary",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    root = clawt_web_root(reply);
    agents = clawt_web_member_array(root, "agents");
    total = clawt_web_member_object(root, "total");

    {
        g_autoptr(HtmxTr) tr = htmx_tr_new();

        clawt_web_add(tr, htmx_th_new_with_text("Agent"));
        clawt_web_add(tr, htmx_th_new_with_text("Turns"));
        clawt_web_add(tr, htmx_th_new_with_text("In"));
        clawt_web_add(tr, htmx_th_new_with_text("Out"));
        clawt_web_add(tr, htmx_th_new_with_text("Cost"));
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(tr));
    }

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        g_autoptr(HtmxTr) tr = htmx_tr_new();
        g_autofree gchar *turns = g_strdup_printf(
            "%" G_GINT64_FORMAT, clawt_web_member_int(agent, "turns", 0));
        g_autofree gchar *in = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_web_member_int(agent, "input_tokens", 0));
        g_autofree gchar *out = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_web_member_int(agent, "output_tokens", 0));
        g_autofree gchar *cost = clawt_usage_format_cost(
            clawt_web_member_int(agent, "cost_micros", 0));

        clawt_web_add(tr, htmx_td_new_with_text(
            clawt_web_member(agent, "name",
                             clawt_web_member(agent, "id", "?"))));

        {
            const gchar *values[] = { turns, in, out, cost };
            guint c;

            for (c = 0; c < G_N_ELEMENTS(values); c++) {
                g_autoptr(HtmxTd) td = htmx_td_new_with_text(values[c]);

                htmx_element_add_class(HTMX_ELEMENT(td), "num");
                htmx_node_add_child(HTMX_NODE(tr), HTMX_NODE(td));
            }
        }

        htmx_node_add_child(HTMX_NODE(rows), HTMX_NODE(tr));
    }

    {
        g_autoptr(HtmxTr) tr = htmx_tr_new();
        g_autofree gchar *turns = g_strdup_printf(
            "%" G_GINT64_FORMAT, clawt_web_member_int(total, "turns", 0));
        g_autofree gchar *in = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_web_member_int(total, "input_tokens", 0));
        g_autofree gchar *out = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_web_member_int(total, "output_tokens", 0));
        g_autofree gchar *cost = clawt_usage_format_cost(
            clawt_web_member_int(total, "cost_micros", 0));
        const gchar *values[] = { turns, in, out, cost };
        guint c;

        clawt_web_add(tr, htmx_td_new_with_text("Fleet"));

        for (c = 0; c < G_N_ELEMENTS(values); c++) {
            g_autoptr(HtmxTd) td = htmx_td_new_with_text(values[c]);

            htmx_element_add_class(HTMX_ELEMENT(td), "num");
            htmx_node_add_child(HTMX_NODE(tr), HTMX_NODE(td));
        }

        htmx_node_add_child(HTMX_NODE(foot), HTMX_NODE(tr));
    }

    htmx_node_add_child(HTMX_NODE(table), HTMX_NODE(head));
    htmx_node_add_child(HTMX_NODE(table), HTMX_NODE(rows));
    htmx_node_add_child(HTMX_NODE(table), HTMX_NODE(foot));

    htmx_element_add_class(wrap, "table-wrap");
    htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(table));
    htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(wrap));

    /*
     * Said here, because somebody reconciling these against an invoice
     * should not have to work it out: IN counts *new* input tokens, and a
     * CLI backend bills cache reads and writes that never appear in it.
     * The cost column is the provider's own figure.
     */
    clawt_web_add(box, clawt_web_text(
        "Cost is the figure the provider reported for each turn, not an "
        "estimate. IN counts only new input tokens -- cached context is "
        "billed and is not reported as tokens, so a turn can honestly "
        "show eight input tokens against a bill of two cents.",
        "small muted"));

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Integrations ────────────────────────────────────────────────── */

static HtmxElement *
integrations_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "integration.list",
                                                   NULL);
    g_autoptr(JsonNode) types = clawt_web_app_call(app, "integration.types",
                                                   NULL);
    JsonArray *list = clawt_web_member_array(clawt_web_root(reply),
                                             "integrations");
    JsonArray *type_list = clawt_web_member_array(clawt_web_root(types),
                                                  "types");
    guint i;

    /*
     * A shared account is a fleet-level bug no single agent can see: two
     * agents on one Matrix login receive each other's messages and answer
     * as the same person, which reads as the fleet misbehaving.
     */
    clawt_web_warnings(HTMX_ELEMENT(box), reply);

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_web_add(box, clawt_web_empty("No integrations", NULL));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *integration = json_array_get_object_element(list, i);
        const gchar *name = clawt_web_member(integration, "name", "?");
        const gchar *type = clawt_web_member(integration, "type", "?");
        g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
        g_autoptr(HtmxDiv) card = clawt_web_card(name, type);
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autofree gchar *health = g_strdup_printf(
            "/settings/integrations/%s/health", escaped);
        g_autofree gchar *test = g_strdup_printf(
            "/settings/integrations/%s/test", escaped);
        g_autofree gchar *remove = g_strdup_printf(
            "/settings/integrations/%s/remove", escaped);

        clawt_web_add(body, clawt_web_row(
            "Scope", clawt_web_member(integration, "scope", "—")));
        clawt_web_add(body, clawt_web_row(
            "Enabled", clawt_web_member_bool(integration, "enabled", TRUE)
                       ? "yes" : "no"));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        clawt_web_add(row, clawt_web_post_button("Check health", health,
                                                 "default", NULL));

        /*
         * A notifier is correct precisely when nothing happens, which
         * makes it the one thing you cannot tell is working by looking
         * at it. The test ignores both the event list and the quiet
         * hours on purpose.
         */
        if (g_strcmp0(type, "notify") == 0)
            clawt_web_add(row, clawt_web_post_button("Send a test", test,
                                                     "default", NULL));

        clawt_web_add(row, clawt_web_post_button(
            "Remove", remove, "danger", "Remove this integration?"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card("Add one", NULL);
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/integrations/add");
        g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);

        for (i = 0; type_list != NULL && i < json_array_get_length(type_list);
             i++) {
            JsonObject *type = json_array_get_object_element(type_list, i);
            const gchar *id = clawt_web_member(type, "id", NULL);

            if (id == NULL)
                continue;

            g_ptr_array_add(ids, g_strdup(id));

            /*
             * The name a person would say, then what it is for.  This
             * read "mcp — Give agents the tools of any MCP server":
             * a lowercase config value in front of the sentence that
             * explains it.
             */
            g_ptr_array_add(labels, g_strdup_printf(
                "%s — %s", clawt_integration_type_label(id),
                clawt_web_member(type, "summary", "")));
        }

        g_ptr_array_add(ids, NULL);
        g_ptr_array_add(labels, NULL);

        clawt_web_add(form, clawt_web_field(
            "Name", "name", NULL, "work-matrix"));
        clawt_web_add(form, clawt_web_text(
            "How you refer to it later. For an MCP server it is also the "
            "key it gets in every agent's .mcp.json.", "small muted"));
        clawt_web_add(form, clawt_web_select_field(
            "What it is", "type", (const gchar *const *)ids->pdata,
            (const gchar *const *)labels->pdata, NULL));

        /*
         * What each one will ask for, before it is chosen.
         *
         * A select shows one option at a time, so the summary beside a
         * type is only visible while the list is open -- and what it
         * *needs* was not said anywhere until the editor was already
         * open.
         */
        for (i = 0; type_list != NULL && i < json_array_get_length(type_list);
             i++) {
            JsonObject *type = json_array_get_object_element(type_list, i);
            const gchar *id = clawt_web_member(type, "id", NULL);
            g_autofree gchar *needs = NULL;
            g_autofree gchar *line = NULL;

            if (id == NULL)
                continue;

            needs = clawt_integration_needs_summary(id);

            if (needs == NULL)
                continue;

            line = g_strdup_printf("%s: %s",
                                   clawt_integration_type_label(id), needs);
            clawt_web_add(form, clawt_web_text(line, "small muted"));
        }

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Add", "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Connectors ──────────────────────────────────────────────────── */

static HtmxElement *
connectors_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "connector.list",
                                                   NULL);
    g_autoptr(JsonNode) catalog = clawt_web_app_call(app, "connector.catalog",
                                                     NULL);
    JsonArray *list = clawt_web_member_array(clawt_web_root(reply),
                                             "connectors");
    JsonArray *available = clawt_web_member_array(clawt_web_root(catalog),
                                                  "connectors");
    guint i;

    {
        g_autoptr(HtmxDiv) registry_row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(registry_row), "btn-row");
        clawt_web_add(registry_row, clawt_web_post_button(
            "Import registry", "/settings/connectors/registry-refresh",
            "default", NULL));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(registry_row));
    }

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_web_add(box, clawt_web_empty("No connectors", NULL));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *connector = json_array_get_object_element(list, i);
        const gchar *name = clawt_web_member(connector, "name", "?");
        g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
        /*
         * The members connector.list actually sends.  This read `type`,
         * `status` and `scopes` -- none of which it has ever sent -- so
         * the subtitle was blank, the status was the literal fallback
         * "not authorised" for every connector including a working one,
         * and the granted scopes never appeared.
         */
        g_autoptr(HtmxDiv) card = clawt_web_card(
            name, clawt_web_member(connector, "provider", NULL));
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        gint64 expires = clawt_web_member_int(connector, "expires_at", 0);
        const gchar *status = clawt_connector_state_label(
            clawt_connector_state(
                clawt_web_member_bool(connector, "connected", FALSE),
                expires,
                clawt_web_member_bool(connector, "renewable", FALSE), 0));
        g_autofree gchar *refresh = g_strdup_printf(
            "/settings/connectors/%s/refresh", escaped);
        g_autofree gchar *revoke = g_strdup_printf(
            "/settings/connectors/%s/revoke", escaped);
        g_autofree gchar *remove = g_strdup_printf(
            "/settings/connectors/%s/remove", escaped);

        clawt_web_add(body, clawt_web_row("Status", status));

        if (expires > 0) {
            g_autoptr(GDateTime) when =
                g_date_time_new_from_unix_local(expires);
            g_autofree gchar *text = (when != NULL)
                ? g_date_time_format(when, "%Y-%m-%d %H:%M") : NULL;

            clawt_web_add(body, clawt_web_row("Expires",
                                              text != NULL ? text : ""));
        }

        if (clawt_web_member(connector, "granted_scopes", NULL) != NULL)
            clawt_web_add(body, clawt_web_row(
                "Granted scopes",
                clawt_web_member(connector, "granted_scopes", "")));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");

        {
            g_autofree gchar *begin = g_strdup_printf(
                "/settings/connectors/%s/authorize", escaped);
            g_autoptr(HtmxA) link = htmx_a_new_with_href(begin);

            htmx_element_add_class(HTMX_ELEMENT(link), "btn");
            htmx_element_add_class(HTMX_ELEMENT(link), "btn-primary");
            htmx_node_set_text_content(HTMX_NODE(link), "Authorize");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(link));
        }

        clawt_web_add(row, clawt_web_post_button("Refresh", refresh,
                                                 "default", NULL));
        clawt_web_add(row, clawt_web_post_button(
            "Revoke", revoke, "default",
            "Revoke this credential? Where the provider offers it this "
            "calls their revocation endpoint; where it does not, only our "
            "copy goes."));
        clawt_web_add(row, clawt_web_post_button(
            "Remove", remove, "danger", "Remove this connector?"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Add a connector",
            "The credential never reaches the agent: the relay reads the "
            "0600 file itself and hands it to the server it starts.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/connectors/add");
        g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);

        for (i = 0; available != NULL &&
                    i < json_array_get_length(available); i++) {
            JsonObject *entry = json_array_get_object_element(available, i);
            const gchar *id = clawt_web_member(entry, "id", NULL);

            if (id == NULL)
                continue;

            g_ptr_array_add(ids, g_strdup(id));
            g_ptr_array_add(labels, g_strdup_printf(
                "%s — %s", clawt_web_member(entry, "name", id),
                clawt_web_member(entry, "summary", "")));
        }

        g_ptr_array_add(ids, NULL);
        g_ptr_array_add(labels, NULL);

        clawt_web_add(form, clawt_web_field("Name", "name", NULL,
                                            "my-slack"));
        clawt_web_add(form, clawt_web_select_field(
            "Service", "type", (const gchar *const *)ids->pdata,
            (const gchar *const *)labels->pdata, NULL));
        clawt_web_add(form, clawt_web_field(
            "Client id", "client_id", NULL,
            "from the service's own developer settings"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Add", "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/*
 * The registry is imported by id, not by name, so this page's own list
 * of *connected* accounts never changes from a refresh -- what changes
 * is what "Add a connector" offers, which already re-asks
 * `connector.catalog` on every render.  This handler's only job is to
 * say what happened, in a fleet's own words: how many, or why not (most
 * often that connectors.registry_enabled is still off).
 */
static HtmxResponse *
on_registry_refresh(HtmxRequest *request, GHashTable *params,
                    gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    g_autofree gchar *done = NULL;
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    reply = clawt_web_app_call(app, "connector.registry_refresh", NULL);

    if (reply == NULL) {
        failure = g_strdup(clawt_web_app_last_error(app));
    } else {
        gint64 imported = clawt_web_member_int(clawt_web_root(reply),
                                               "imported", 0);

        done = g_strdup_printf(
            "Imported %" G_GINT64_FORMAT " connector%s from the registry.",
            imported, imported == 1 ? "" : "s");
    }

    content = connectors_content(app);

    if (failure != NULL)
        return settings_response(app, request, "connectors", content,
                                 failure, TRUE);

    return settings_response(app, request, "connectors", content, done,
                             FALSE);
}

/*
 * Who a shared folder reaches, in a phrase.
 *
 * Said on every card rather than only the scoped ones: a list where most
 * rows say nothing and one says "team backend" invites reading the
 * silent ones as unknown rather than as everybody.
 */
static gchar *
folders_describe_scope(JsonObject *mount)
{
    const gchar *scope = clawt_web_member(mount, "scope", "all");
    g_autoptr(GString) out = NULL;
    static const gchar *const keys[] = { "agents", "teams", NULL };
    gsize k;

    if (g_strcmp0(scope, "all") == 0)
        return g_strdup("every agent");

    if (g_strcmp0(scope, "none") == 0)
        return g_strdup("nobody");

    out = g_string_new(NULL);

    for (k = 0; keys[k] != NULL; k++) {
        JsonArray *items = clawt_web_member_array(mount, keys[k]);
        guint i;

        for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
            if (out->len > 0)
                g_string_append(out, ", ");

            g_string_append_printf(out, "%s %s",
                                   g_strcmp0(keys[k], "teams") == 0
                                       ? "team" : "agent",
                                   json_array_get_string_element(items, i));
        }
    }

    /*
     * A `selected` scope naming nothing reaches nobody, and saying so
     * matters: it is the state a half-finished edit leaves behind.
     */
    return out->len > 0 ? g_strdup(out->str) : g_strdup("nobody (no list)");
}

/* ── Shared folders ──────────────────────────────────────────────── */

static HtmxElement *
folders_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "defaults.mount.list",
                                                   NULL);
    JsonArray *mounts = clawt_web_member_array(clawt_web_root(reply),
                                               "mounts");
    guint i;

    /*
     * A folder scoped to an agent or a team that is not there. Above the
     * list, because it is the reason somebody came looking.
     */
    clawt_web_warnings(HTMX_ELEMENT(box), reply);

    if (mounts == NULL || json_array_get_length(mounts) == 0)
        clawt_web_add(box, clawt_web_empty(
            "Nothing shared yet",
            "A directory added here reaches every agent that has a "
            "computer, including ones you create later."));

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *source = clawt_web_member(mount, "source", "");
        const gchar *target = clawt_web_member(mount, "target", "");
        const gchar *mode = clawt_web_member(mount, "mode", "rw");
        g_autoptr(HtmxDiv) card = clawt_web_card(source, NULL);
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/folders/remove");
        g_autoptr(HtmxDiv) row = htmx_div_new();

        /*
         * Both paths, always. An agent's own tools run on the host and
         * its shell runs inside the computer, so a shared folder has two
         * names -- and telling somebody only one is how they go looking
         * for a file at a path that does not exist on the machine they
         * are on.
         */
        clawt_web_add(body, clawt_web_row("Path inside", target));
        clawt_web_add(body, clawt_web_row(
            "Mode", g_strcmp0(mode, "ro") == 0 ? "read-only" : "writable"));

        /*
         * Said on every card rather than only the scoped ones. A list
         * where most rows say nothing and one says "team backend"
         * invites reading the silent ones as unknown rather than as
         * everybody.
         */
        {
            g_autofree gchar *who = folders_describe_scope(mount);

            clawt_web_add(body, clawt_web_row("Who gets it", who));
        }

        /*
         * The target travels as a form field, not in the path.
         *
         * It is always an absolute path, so a route parameter holding
         * one is always percent-encoded -- and an encoded slash does not
         * match a `:target` segment, so the first version of this button
         * answered 404 for every folder there could ever be. A form
         * field has no such problem and needs no escaping decision.
         */
        {
            g_autoptr(HtmxButton) stop =
                clawt_web_button("Stop sharing", "danger");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(stop), "type", "submit");
            htmx_element_set_attribute(
                HTMX_ELEMENT(stop), "hx-confirm",
                "Stop sharing this folder with every agent?");

            clawt_web_add(form, clawt_web_hidden("target", target));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(stop));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        }

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        static const gchar *const modes[] = { "rw", "ro", NULL };
        static const gchar *const mode_labels[] = {
            "Writable", "Read-only", NULL
        };
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Share a folder",
            "Container, distrobox and VM agents get it. A host agent does "
            "not: there a mount is the confinement allowlist rather than a "
            "shared folder, and widening that is not something a default "
            "should do quietly.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/folders/add");

        clawt_web_add(form, clawt_web_field("Path on this machine", "source",
                                            NULL, "~/source"));
        clawt_web_add(form, clawt_web_field(
            "Path inside", "target", NULL,
            "empty means the same path, which is usually what you want"));
        clawt_web_add(form, clawt_web_select_field("Mode", "mode", modes,
                                                   mode_labels, "rw"));
        /*
         * One field, like the GTK dialog.  A name is a team or an agent
         * and the fleet already knows which, so two boxes ask somebody
         * to classify something the daemon can -- and getting it wrong
         * shares the folder with nobody, silently, which is exactly what
         * happened.
         */
        clawt_web_add(form, clawt_web_field(
            "Teams or agents", "who", NULL,
            "empty means every agent; otherwise names, comma separated"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Share", "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

        clawt_web_add(body, clawt_web_text(
            "Naming a team covers everyone on it, including whoever joins "
            "later -- which is the whole reason teams exist. Name neither "
            "and it goes to every agent that has a computer.",
            "small muted"));
        clawt_web_add(body, clawt_web_text(
            "An agent that declares its own folder at the same path wins "
            "there, and one can decline all of them with "
            "computer.default_mounts: false. A folder reaches an agent's "
            "computer when it is next started.", "small muted"));

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Appearance ──────────────────────────────────────────────────── */

static HtmxElement *
appearance_content(HtmxRequest *request)
{
    g_autoptr(GPtrArray) themes = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) theme_labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(ClawtWebLook) look = clawt_web_look_from_request(request);
    guint t;
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(HtmxForm) form = clawt_web_form("/settings/appearance");
    g_autofree gchar *font_size = NULL;
    g_autofree gchar *mono_size = NULL;

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Theme",
            "Kept in this browser rather than in clawtilla.yaml, for the "
            "same reason the GTK client keeps its own: a theme is about "
            "the screen you are sitting at, not about the fleet. Connect "
            "to another daemon and it comes with you.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxDiv) row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "field");

        /*
         * Built from the library's list rather than named here. The copy
         * that used to sit at the top of this function offered three
         * schemes while the GTK combo offered four, so the palette added
         * to clawt-appearance.c was selectable in one client and not the
         * other -- and `make parity` saw nothing, because a colour
         * scheme sends no IPC frame and is no slash command.
         */
        for (t = 0; t < clawt_appearance_scheme_count(); t++) {
            g_ptr_array_add(themes,
                            g_strdup(clawt_appearance_scheme_nth_nick(t)));
            g_ptr_array_add(theme_labels,
                            g_strdup(clawt_appearance_scheme_nth_label(t)));
        }

        g_ptr_array_add(themes, NULL);
        g_ptr_array_add(theme_labels, NULL);

        clawt_web_add(row, clawt_web_select_field(
            "Colour scheme", "theme",
            (const gchar *const *)themes->pdata,
            (const gchar *const *)theme_labels->pdata,
            look->theme != NULL
                ? look->theme
                : clawt_appearance_theme_nick(CLAWT_THEME_SYSTEM)));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));

        /*
         * "Follow the system" means exactly that. It emits no rule at
         * all, rather than naming whatever the browser currently uses --
         * the two look identical on screen and diverge for ever
         * afterwards, because one keeps following and the other has
         * quietly frozen.
         */
        clawt_web_add(body, clawt_web_text(
            "Following the system keeps following it. Choosing light or "
            "dark freezes it here.", "small muted"));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(card));
    }

    font_size = (look->font_size > 0)
                ? g_strdup_printf("%d", look->font_size) : NULL;
    mono_size = (look->mono_size > 0)
                ? g_strdup_printf("%d", look->mono_size) : NULL;

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Fonts",
            "Left empty, each of these follows whatever this browser is "
            "set to. A name here is a CSS family, so anything installed "
            "on the machine looking at the page works.");
        HtmxElement *body = clawt_web_card_body(card);

        clawt_web_add(body, clawt_web_field(
            "Interface font", "font", look->font,
            "empty follows the browser -- try Cantarell, or Helvetica Neue"));
        clawt_web_add(body, clawt_web_field(
            "Interface size (px)", "font_size", font_size,
            "empty follows the browser; 8 to 32"));
        clawt_web_add(body, clawt_web_field(
            "Code font", "mono", look->mono,
            "empty follows the browser -- try JetBrains Mono, or Menlo"));
        clawt_web_add(body, clawt_web_field(
            "Code size (px)", "mono_size", mono_size,
            "empty follows the browser; 8 to 32"));

        /*
         * The code font reaches chat messages as well as the console.
         * In the GTK client it did not for a long time, because Pango's
         * <tt> resolves through fontconfig's generic monospace alias and
         * is not reachable from GTK CSS -- so a code font applied
         * everywhere except where people actually read code.
         */
        clawt_web_add(body, clawt_web_text(
            "The code font applies to the exec console, inline code in "
            "messages, and every value shown in monospace.",
            "small muted"));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Reading",
            "How wide the conversation runs and how far apart one "
            "person's messages sit from the next. Left empty, both "
            "follow the shipped values.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(GPtrArray) unit_nicks =
            g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) unit_labels =
            g_ptr_array_new_with_free_func(g_free);
        g_autofree gchar *measure = (look->measure > 0)
                                    ? g_strdup_printf("%d", look->measure)
                                    : NULL;
        g_autofree gchar *run_gap = (look->run_gap > 0)
                                    ? g_strdup_printf("%d", look->run_gap)
                                    : NULL;
        g_autofree gchar *measure_hint = g_strdup_printf(
            "%d to %d percent, %d to %d characters, or %d to %d pixels",
            CLAWT_APPEARANCE_MIN_PERCENT, CLAWT_APPEARANCE_MAX_PERCENT,
            CLAWT_APPEARANCE_MIN_COLUMNS, CLAWT_APPEARANCE_MAX_COLUMNS,
            CLAWT_APPEARANCE_MIN_MEASURE, CLAWT_APPEARANCE_MAX_MEASURE);
        g_autofree gchar *gap_hint = g_strdup_printf(
            "empty follows the shipped gap; up to %d",
            CLAWT_APPEARANCE_MAX_RUN_SPACING);
        guint u;

        /*
         * Walked from the library rather than named here, for the
         * reason the colour schemes above already record: two
         * hand-written lists is how a value came to be offered by one
         * client and not the other, with `make parity` reporting OK
         * because a choice like this sends no IPC frame.
         */
        for (u = 0; u < clawt_measure_unit_count(); u++) {
            ClawtMeasureUnit at = clawt_measure_unit_nth(u);

            g_ptr_array_add(unit_nicks,
                            g_strdup(clawt_measure_unit_nick(at)));
            g_ptr_array_add(unit_labels,
                            g_strdup(clawt_measure_unit_label(at)));
        }

        g_ptr_array_add(unit_nicks, NULL);
        g_ptr_array_add(unit_labels, NULL);

        clawt_web_add(body, clawt_web_select_field(
            "Column width", "measure_unit",
            (const gchar *const *)unit_nicks->pdata,
            (const gchar *const *)unit_labels->pdata,
            clawt_measure_unit_nick(look->measure_unit)));
        clawt_web_add(body, clawt_web_field(
            "How much", "measure", measure, measure_hint));
        clawt_web_add(body, clawt_web_field(
            "Gap between runs (px)", "run_gap", run_gap, gap_hint));

        /*
         * The bounds come from the library, not from numbers written
         * here.  Two clients each with their own idea of what is
         * allowed is exactly the drift the parity check exists for, and
         * a hint that disagreed with the clamp would be worse than no
         * hint at all.
         */
        clawt_web_add(body, clawt_web_text(
            "The conversation takes nine tenths of the window unless you "
            "say otherwise. A share follows the window, a character "
            "count follows your font, and a pixel width follows neither "
            "and is exact.",
            "small muted"));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Agent list",
            "A description is what tells you which agent to ask. A fleet "
            "of them is also the reason the list stops fitting on a "
            "screen.");
        HtmxElement *body = clawt_web_card_body(card);
        static const gchar *const nicks[] = { "1", "0", NULL };
        static const gchar *const labels[] = {
            "Under each name", "On hover only", NULL
        };

        /*
         * A select rather than a checkbox, because an unchecked box
         * sends no field at all -- so "off" and "the form did not carry
         * this" would arrive identically, and the handler could not
         * tell somebody turning descriptions off from a request that
         * never mentioned them.
         */
        clawt_web_add(body, clawt_web_select_field(
            "Descriptions", "agent_desc", nicks, labels,
            look->hide_descriptions ? "0" : "1"));

        clawt_web_add(body, clawt_web_text(
            "On hover only keeps the description on the row's tooltip "
            "rather than losing it.",
            "small muted"));

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Apply", "primary");
        g_autoptr(HtmxButton) reset = clawt_web_post_button(
            "Follow the browser for everything", "/settings/appearance/reset",
            "default", NULL);

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(reset));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(form));

    return HTMX_ELEMENT(g_steal_pointer(&box));
}


/* ── Connections ─────────────────────────────────────────────────── */

/*
 * The profiles live in the client's own config, never in clawtilla.yaml.
 *
 * The point of a profile is to reach a daemon somewhere else, and a
 * laptop connecting to a workstation may have no fleet and no
 * clawtilla.yaml at all -- so reading the daemon's config to find out how
 * to reach a different daemon is backwards. The file is 0600 and holds
 * bearer tokens on purpose: the token *is* the thing being remembered,
 * and a second file to manage would mean most people keeping it in shell
 * history instead.
 */
static HtmxElement *
connections_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autofree gchar *path = clawt_connection_list_default_path();
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = clawt_connection_list_load(path, &error);
    const gchar *current = clawt_web_app_get_connection_name(app);
    guint i;

    /*
     * Said before anything else, because it is the one way this differs
     * from the GTK client: there is one connection here and many
     * browsers, so switching moves everybody.
     */
    clawt_web_add(box, clawt_web_notice(
        "There is one connection for this whole server. Switching moves "
        "every browser looking at it, not just this one.", "info"));

    if (error != NULL)
        clawt_web_add(box, clawt_web_notice(error->message, "bad"));

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Saved daemons", path);
        HtmxElement *body = clawt_web_card_body(card);

        if (list == NULL || list->len == 0)
            clawt_web_add(body, clawt_web_empty(
                "No profiles saved",
                "Add one below. Until then this server talks to whatever "
                "--socket it was started with."));

        for (i = 0; list != NULL && i < list->len; i++) {
            ClawtConnection *connection = g_ptr_array_index(list, i);
            const gchar *name = clawt_connection_get_name(connection);
            g_autofree gchar *described =
                clawt_connection_describe(connection);
            g_autofree gchar *escaped = g_uri_escape_string(name, NULL,
                                                            FALSE);
            g_autofree gchar *use = g_strdup_printf(
                "/settings/connections/%s/use", escaped);
            g_autofree gchar *forget = g_strdup_printf(
                "/settings/connections/%s/forget", escaped);
            g_autofree gchar *check = g_strdup_printf(
                "/settings/connections/%s/check", escaped);
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxDiv) head = htmx_div_new();
            g_autoptr(HtmxDiv) actions = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
            htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

            {
                g_autoptr(HtmxSpan) label = htmx_span_new();

                htmx_element_add_class(HTMX_ELEMENT(label),
                                       "list-item-title");
                htmx_node_set_text_content(HTMX_NODE(label), name);
                htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
            }

            if (g_strcmp0(name, current) == 0)
                clawt_web_add(head, clawt_web_badge("in use", "good"));

            if (clawt_connection_get_token(connection) != NULL)
                clawt_web_add(head, clawt_web_badge("token", "info"));

            /*
             * Whether that machine is up, as far as anybody has asked.
             *
             * Every entry was drawn identically whether the daemon
             * behind it was running, stopped, unreachable or holding a
             * token that no longer matched -- and the only way to find
             * out was to switch to it and fail, which is a destructive
             * way to ask a read-only question.
             *
             * Nothing is drawn until somebody presses Check: an absent
             * verdict is honest, and "unreachable" about a host nobody
             * asked about would not be.
             */
            {
                ClawtConnectionStatus *status =
                    clawt_web_app_connection_status(app, name);

                if (status != NULL)
                    clawt_web_add(head, clawt_web_badge(
                        clawt_reachability_word(status->reach),
                        status->reach == CLAWT_REACH_REACHABLE ? "good"
                                                               : "bad"));

                if (status != NULL &&
                    status->reach == CLAWT_REACH_REACHABLE &&
                    status->version != NULL) {
                    g_autofree gchar *detail = g_strdup_printf(
                        "clawtillad %s, %u agent%s", status->version,
                        status->agents, status->agents == 1 ? "" : "s");

                    clawt_web_add(head,
                                  clawt_web_badge(detail, "neutral"));
                }
            }

            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

            /*
             * describe() names the socket or the host and port. It never
             * names the token -- nothing prints a bearer token from a
             * listing.
             */
            clawt_web_add(row, clawt_web_text(described, "small muted mono"));

            htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
            /*
             * Read-only, and first: it is the question somebody has
             * before they decide whether to press Connect, and until now
             * pressing Connect was the only way to ask it.
             */
            clawt_web_add(actions, clawt_web_post_button(
                "Check", check, NULL, NULL));
            clawt_web_add(actions, clawt_web_post_button(
                "Connect", use, "primary",
                "Point this server at that daemon? Every open page moves "
                "with it."));
            clawt_web_add(actions, clawt_web_post_button(
                "Forget", forget, "danger",
                "Remove this profile? The daemon itself is untouched."));
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));

            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Add a daemon",
            "A socket path for one on this machine, or a host and port "
            "for one somewhere else.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form("/settings/connections/add");

        clawt_web_add(form, clawt_web_field("Name", "name", NULL,
                                            "workstation"));
        clawt_web_add(form, clawt_web_field(
            "Socket path", "socket", NULL,
            "for a daemon on this machine; leave empty for a remote one"));
        clawt_web_add(form, clawt_web_field(
            "Host", "host", NULL, "100.72.0.41, or a tailnet name"));
        clawt_web_add(form, clawt_web_field("Port", "port", NULL, "7654"));
        clawt_web_add(form, clawt_web_field(
            "Bearer token", "token", NULL,
            "what that daemon expects; stored 0600 and never shown again"));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) add = clawt_web_button("Save", "primary");

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(add), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(add));
            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

static HtmxResponse *
on_connection_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *name = clawt_web_form_value(request, "name");
    const gchar *socket_path = clawt_web_form_value(request, "socket");
    const gchar *host = clawt_web_form_value(request, "host");
    const gchar *port = clawt_web_form_value(request, "port");
    const gchar *token = clawt_web_form_value(request, "token");
    g_autofree gchar *path = clawt_connection_list_default_path();
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = NULL;
    g_autoptr(HtmxElement) content = NULL;
    ClawtConnection *fresh;

    (void)params;

    if (name == NULL || *name == '\0') {
        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 "It needs a name.", TRUE);
    }

    list = clawt_connection_list_load(path, NULL);

    if (list == NULL)
        list = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_connection_free);

    if (socket_path != NULL && *socket_path != '\0') {
        fresh = clawt_connection_new_local(name, socket_path);
    } else if (host != NULL && *host != '\0') {
        fresh = clawt_connection_new_remote(
            name, host,
            (guint16)(port != NULL ? g_ascii_strtoll(port, NULL, 10) : 0),
            (token != NULL && *token != '\0') ? token : NULL);
    } else {
        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 "Give it a socket path or a host.", TRUE);
    }

    g_ptr_array_add(list, fresh);

    if (!clawt_connection_list_save(path, list, &error)) {
        g_autofree gchar *failure = g_strdup(error->message);

        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 failure, TRUE);
    }

    content = connections_content(app);

    return settings_response(app, request, "connections", content,
                             "Saved.", FALSE);
}

static HtmxResponse *
on_connection_use(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "connection");
    g_autofree gchar *path = clawt_connection_list_default_path();
    g_autoptr(GPtrArray) list = clawt_connection_list_load(path, NULL);
    g_autoptr(GError) error = NULL;
    ClawtConnection *chosen;
    g_autoptr(HtmxElement) content = NULL;

    chosen = (list != NULL) ? clawt_connection_list_find(list, name) : NULL;

    if (chosen == NULL) {
        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 "There is no profile by that name.", TRUE);
    }

    if (!clawt_web_app_switch(app, chosen, &error)) {
        g_autofree gchar *failure = g_strdup(error->message);

        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 failure, TRUE);
    }

    /*
     * Back to the fleet rather than staying here: everything the browser
     * was showing belonged to the previous daemon, and agent ids are
     * per-daemon.
     */
    return clawt_web_redirect(request, "/");
}

static HtmxResponse *
on_connection_forget(HtmxRequest *request, GHashTable *params,
                     gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "connection");
    g_autofree gchar *path = clawt_connection_list_default_path();
    g_autoptr(GPtrArray) list = clawt_connection_list_load(path, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(HtmxElement) content = NULL;
    guint i;

    for (i = 0; list != NULL && i < list->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(list, i);

        if (g_strcmp0(clawt_connection_get_name(connection), name) != 0)
            continue;

        g_ptr_array_remove_index(list, i);
        break;
    }

    if (list != NULL && !clawt_connection_list_save(path, list, &error)) {
        g_autofree gchar *failure = g_strdup(error->message);

        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 failure, TRUE);
    }

    content = connections_content(app);

    return settings_response(app, request, "connections", content,
                             "Forgotten. The daemon itself is untouched.",
                             FALSE);
}

static HtmxResponse *
on_connection_check(HtmxRequest *request, GHashTable *params,
                    gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "connection");
    g_autofree gchar *path = clawt_connection_list_default_path();
    g_autoptr(GPtrArray) list = clawt_connection_list_load(path, NULL);
    g_autoptr(HtmxElement) content = NULL;
    g_autofree gchar *notice = NULL;
    ClawtConnection *found = NULL;
    ClawtConnectionStatus *status;
    guint i;

    for (i = 0; list != NULL && i < list->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(list, i);

        if (g_strcmp0(clawt_connection_get_name(connection), name) == 0) {
            found = connection;
            break;
        }
    }

    if (found == NULL) {
        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 "There is no profile by that name.", TRUE);
    }

    /*
     * One connection, because a reader asked about one. Probing the
     * whole list here would put every saved daemon's timeout in front of
     * this page, including the ones nobody is thinking about.
     */
    status = clawt_connection_probe(found);

    notice = (status->reach == CLAWT_REACH_REACHABLE)
        ? g_strdup_printf("%s is reachable: clawtillad %s, %u agent%s.",
                          name,
                          status->version != NULL ? status->version : "?",
                          status->agents, status->agents == 1 ? "" : "s")
        : g_strdup_printf("%s is %s. %s", name,
                          clawt_reachability_word(status->reach),
                          status->detail != NULL ? status->detail : "");

    {
        gboolean bad = status->reach != CLAWT_REACH_REACHABLE;

        clawt_web_app_note_connection_status(app, name, status);

        content = connections_content(app);

        return settings_response(app, request, "connections", content,
                                 notice, bad);
    }
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_settings_index(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;
    (void)user_data;

    return clawt_web_redirect(request, "/settings/images");
}

/*
 * Holding the fleet.
 *
 * Two buttons rather than one toggle: a control labelled "Pause" while
 * the fleet is already held is a control whose label is the opposite of
 * the truth, and the state above them is what says which one to press.
 *
 * The whole page is about the difference between a hold and a stop,
 * because that difference is the only thing somebody needs to know
 * before pressing it -- and "pause" carries the wrong promise in most
 * software, so an operator who thinks it kills work will not use it.
 */
static HtmxElement *
fleet_content(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "control.status",
                                                   NULL);
    JsonObject *hold = clawt_web_member_object(clawt_web_root(reply), "hold");
    gboolean held = hold != NULL &&
                    clawt_web_member_bool(hold, "held", FALSE);
    gint64 draining = (hold != NULL)
        ? clawt_web_member_int(hold, "draining", 0) : 0;
    g_autoptr(HtmxDiv) card = clawt_web_card("Hold", NULL);
    HtmxElement *body = clawt_web_card_body(card);

    if (!held) {
        clawt_web_add(body, clawt_web_row("State", "running"));
    } else if (draining > 0) {
        g_autofree gchar *text = g_strdup_printf(
            "draining -- %" G_GINT64_FORMAT " turn(s) still in flight",
            draining);

        clawt_web_add(body, clawt_web_row("State", text));
    } else {
        clawt_web_add(body,
                      clawt_web_row("State",
                                    "held, nothing in flight -- safe to "
                                    "restart"));
    }

    clawt_web_add(body, clawt_web_row(
        "What a hold does",
        "Stops delivery and leaves every process alive. The turn that is "
        "running finishes; nothing new starts; queued work stays queued. "
        "It survives a restart, and what was running comes back."));

    {
        g_autoptr(HtmxForm) form = clawt_web_form(
            held ? "/settings/fleet/resume" : "/settings/fleet/pause");
        g_autoptr(HtmxButton) button = clawt_web_button(
            held ? "Resume the fleet" : "Hold the fleet",
            held ? "primary" : "danger");

        htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
        clawt_web_add(form, g_steal_pointer(&button));
        clawt_web_add(body, g_steal_pointer(&form));
    }

    clawt_web_add(box, g_steal_pointer(&card));

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

static HtmxResponse *
on_fleet_hold(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    gboolean pausing = strstr(htmx_request_get_path(request),
                              "/pause") != NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *failure = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    reply = clawt_web_app_call(app,
                               pausing ? "control.pause" : "control.resume",
                               NULL);

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = fleet_content(app);

    return settings_response(app, request, "fleet", content, failure, TRUE);
}

static HtmxResponse *
on_settings_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *slug = clawt_web_param(params, "page");
    g_autoptr(HtmxElement) content = NULL;

    if (g_strcmp0(slug, "fleet") == 0)
        content = fleet_content(app);
    else if (g_strcmp0(slug, "images") == 0)
        content = images_content(app);
    else if (g_strcmp0(slug, "teams") == 0)
        content = teams_content(app);
    else if (g_strcmp0(slug, "folders") == 0)
        content = folders_content(app);
    else if (g_strcmp0(slug, "spending") == 0) {
        const gchar *period = htmx_request_get_query_param(request, "period");

        content = spending_content(app, period != NULL ? period : "all");
    } else if (g_strcmp0(slug, "integrations") == 0)
        content = integrations_content(app);
    else if (g_strcmp0(slug, "connectors") == 0)
        content = connectors_content(app);
    else if (g_strcmp0(slug, "appearance") == 0)
        content = appearance_content(request);
    else if (g_strcmp0(slug, "connections") == 0)
        content = connections_content(app);
    else
        return clawt_web_redirect(request, "/settings/fleet");

    return settings_response(app, request, slug, content, NULL, FALSE);
}

/*
 * One handler for the many one-frame actions on these pages.
 *
 * They differ in the frame, the payload key and what they say afterwards.
 * A handler each would be a dozen near-identical functions, and the one
 * that forgot to report the daemon's refusal would be the one nobody
 * pressed until it mattered.
 */
typedef struct {
    ClawtWebApp *app;
    const gchar *kind;
    const gchar *param;     /* which path parameter carries the subject */
    const gchar *member;    /* what the daemon calls it */
    const gchar *page;
    const gchar *done;
} SettingsAction;

static HtmxResponse *
on_settings_action(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    SettingsAction *action = user_data;
    g_autofree gchar *subject = clawt_web_param(params, action->param);
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    clawt_web_payload_set(payload, action->member, subject);

    reply = clawt_web_app_call(action->app, action->kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(action->app));

    if (g_strcmp0(action->page, "images") == 0)
        content = images_content(action->app);
    else if (g_strcmp0(action->page, "teams") == 0)
        content = teams_content(action->app);
    else if (g_strcmp0(action->page, "integrations") == 0)
        content = integrations_content(action->app);
    else
        content = connectors_content(action->app);

    if (failure != NULL)
        return settings_response(action->app, request, action->page, content,
                                 failure, TRUE);

    return settings_response(action->app, request, action->page, content,
                             action->done, FALSE);
}

static SettingsAction *
settings_action_new(ClawtWebApp *app, const gchar *kind, const gchar *param,
                    const gchar *member, const gchar *page,
                    const gchar *done)
{
    SettingsAction *action = g_new0(SettingsAction, 1);

    action->app = app;
    action->kind = kind;
    action->param = param;
    action->member = member;
    action->page = page;
    action->done = done;

    return action;
}

static HtmxResponse *
on_image_download(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    const gchar *name = clawt_web_form_value(request, "name");
    const gchar *url = clawt_web_form_value(request, "url");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    if (url != NULL && *url != '\0')
        clawt_web_payload_set(payload, "url", url);

    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "image.vm_download",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    /*
     * Taken before the content is built. Building it makes calls of its
     * own, and each one replaces the app's last error -- so reporting it
     * afterwards reports whichever read happened last, not the action
     * that failed.
     */
    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = images_content(app);

    if (failure != NULL)
        return settings_response(app, request, "images", content, failure,
                                 TRUE);

    return settings_response(app, request, "images", content,
                             "Downloading. It keeps going if you leave "
                             "this page.", FALSE);
}

static HtmxResponse *
on_team_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "id");
    const gchar *name = clawt_web_form_value(request, "name");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    clawt_web_payload_set(payload, "id", id);
    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "team.create",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = teams_content(app);

    if (failure != NULL)
        return settings_response(app, request, "teams", content, failure,
                                 TRUE);

    return settings_response(app, request, "teams", content,
                             "Team created.", FALSE);
}

static HtmxResponse *
on_team_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "team");
    g_autoptr(HtmxElement) content = NULL;
    static const gchar *const keys[] = { "name", "description", "color" };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(keys); i++) {
        const gchar *value = clawt_web_form_value(request, keys[i]);
        g_autoptr(ClawtWebPayload) payload = NULL;
        g_autoptr(JsonNode) reply = NULL;

        if (value == NULL)
            continue;

        payload = clawt_web_payload_new();
        clawt_web_payload_set(payload, "team", id);
        clawt_web_payload_set(payload, "key", keys[i]);
        clawt_web_payload_set(payload, "value", value);

        reply = clawt_web_app_call(app, "team.set",
                                   clawt_web_payload_take(
                                       g_steal_pointer(&payload)));

        if (reply == NULL) {
            g_autofree gchar *failure =
                g_strdup(clawt_web_app_last_error(app));

            content = teams_content(app);

            return settings_response(app, request, "teams", content, failure,
                                     TRUE);
        }
    }

    content = teams_content(app);

    return settings_response(app, request, "teams", content,
                             "Saved.", FALSE);
}

static HtmxResponse *
on_integration_add(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    clawt_web_payload_set(payload, "name",
                          clawt_web_form_value(request, "name"));
    clawt_web_payload_set(payload, "type",
                          clawt_web_form_value(request, "type"));

    reply = clawt_web_app_call(app, "integration.add",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = integrations_content(app);

    if (failure != NULL)
        return settings_response(app, request, "integrations", content,
                                 failure, TRUE);

    return settings_response(app, request, "integrations", content,
                             "Added. Fill in its settings on the agent that "
                             "should use it.", FALSE);
}

static HtmxResponse *
on_connector_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    /*
     * A connector is an integration whose type is "connector"; the
     * service goes in `provider`. There is no connector.add frame, and
     * inventing one here would be a second creation path -- which is
     * exactly how validation got skipped for agents before.
     */
    clawt_web_payload_set(payload, "name",
                          clawt_web_form_value(request, "name"));
    clawt_web_payload_set(payload, "type", "connector");
    clawt_web_payload_set(payload, "provider",
                          clawt_web_form_value(request, "type"));
    clawt_web_payload_set(payload, "scope", "all");
    clawt_web_payload_set(payload, "client_id",
                          clawt_web_form_value(request, "client_id"));

    reply = clawt_web_app_call(app, "integration.add",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = connectors_content(app);

    if (failure != NULL)
        return settings_response(app, request, "connectors", content,
                                 failure, TRUE);

    return settings_response(app, request, "connectors", content,
                             "Added. Authorize it to get a credential.",
                             FALSE);
}

/*
 * Starting an authorization is two frames on purpose.
 *
 * `connector.begin` answers as soon as there is a code to show, and
 * `connector.await` is the one that waits -- a device code is good for
 * fifteen minutes, and deferring the whole thing would mean showing the
 * person nothing until it was over.
 */
static HtmxResponse *
on_connector_authorize(HtmxRequest *request, GHashTable *params,
                       gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "connector");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) box = htmx_div_new();
    JsonObject *root;

    (void)request;

    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "connector.begin",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *failure = g_strdup(clawt_web_app_last_error(app));
        g_autoptr(HtmxElement) content = connectors_content(app);

        return settings_response(app, request, "connectors", content, failure,
                                 TRUE);
    }

    root = clawt_web_root(reply);

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Authorize", "Finish this in the service's own window.");
        HtmxElement *body = clawt_web_card_body(card);
        const gchar *url = clawt_web_member(root, "url", NULL);
        const gchar *code = clawt_web_member(root, "user_code", NULL);
        const gchar *flow = clawt_web_member(root, "flow", NULL);

        if (code != NULL)
            clawt_web_add(body, clawt_web_row("Code", code));

        if (url != NULL) {
            g_autoptr(HtmxA) link = htmx_a_new_with_href(url);

            htmx_element_add_class(HTMX_ELEMENT(link), "btn");
            htmx_element_add_class(HTMX_ELEMENT(link), "btn-primary");
            htmx_element_set_attribute(HTMX_ELEMENT(link), "target",
                                       "_blank");
            htmx_element_set_attribute(HTMX_ELEMENT(link), "rel",
                                       "noopener noreferrer");
            htmx_node_set_text_content(HTMX_NODE(link), "Open the service");
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(link));
        }

        if (flow != NULL) {
            g_autofree gchar *escaped = g_uri_escape_string(flow, NULL,
                                                            FALSE);
            g_autofree gchar *await = g_strdup_printf(
                "/settings/connectors/%s/await", escaped);
            g_autoptr(HtmxDiv) row = htmx_div_new();

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            clawt_web_add(row, clawt_web_post_button(
                "I have finished", await, "default", NULL));
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        }

        clawt_web_add(body, clawt_web_text(
            "A poll for a code nobody has typed yet is answered with 400 "
            "and 'authorization pending'. That is the normal case, not a "
            "failure.", "small muted"));

        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));
    }

    return settings_response(app, request, "connectors", HTMX_ELEMENT(box), NULL,
                             FALSE);
}

static HtmxResponse *
on_connector_await(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    g_autofree gchar *flow = clawt_web_param(params, "connector");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)request;

    clawt_web_payload_set(payload, "flow", flow);

    reply = clawt_web_app_call(app, "connector.await",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = connectors_content(app);

    if (failure != NULL)
        return settings_response(app, request, "connectors", content,
                                 failure, TRUE);

    return settings_response(app, request, "connectors", content,
                             "Authorized.", FALSE);
}

/*
 * Writes one appearance cookie.
 *
 * An empty value clears it with Max-Age=0 rather than storing "", so
 * that "follow the browser" is the *absence* of a setting rather than a
 * setting whose value happens to be empty. The renderer emits no rule
 * for an absent one, which is what makes following keep following.
 */
static void
set_look_cookie(HtmxResponse *response, const gchar *name, const gchar *value)
{
    g_autofree gchar *cookie = NULL;

    if (value == NULL || *value == '\0') {
        cookie = g_strdup_printf("%s=; Path=/; Max-Age=0; SameSite=Lax",
                                 name);
    } else {
        g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

        /*
         * A year, and SameSite=Lax. There is nothing secret in these --
         * they say light, or a font name -- but a cookie another site
         * can cause to be sent is a cookie worth not having.
         */
        cookie = g_strdup_printf(
            "%s=%s; Path=/; Max-Age=31536000; SameSite=Lax", name, escaped);
    }

    /*
     * Appended, not added: htmx_response_add_header() keeps one value per
     * name, so five cookies set through it arrive as one and the other
     * four are silently not set. Set-Cookie is the header whose whole
     * purpose is to repeat.
     */
    htmx_response_append_header(response, "Set-Cookie", cookie);
}

static HtmxResponse *
on_appearance(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *theme = clawt_web_form_value(request, "theme");
    g_autoptr(HtmxElement) content = NULL;
    HtmxResponse *response;

    (void)params;

    content = appearance_content(request);
    response = settings_response(app, request, "appearance", content,
                                 "Applied.", FALSE);

    /*
     * Round-tripped through the library rather than stored as typed, so
     * only a nick the library knows is ever written. It came off a form
     * and ends up on the root element's data-theme; an unrecognised one
     * becomes "system", which is the same answer clawt-appearance gives
     * a config file naming a palette this build has not got.
     */
    /*
     * Round-tripped through the library so only a scheme it knows is
     * written -- including a palette read from a file, which has no
     * ClawtTheme value and would have been flattened to "system" by the
     * enum-based round trip this replaced.
     */
    {
        g_autoptr(ClawtAppearance) probe = clawt_appearance_new();

        clawt_appearance_set_scheme(probe, theme);
        set_look_cookie(response, "clawt_theme",
                        clawt_appearance_get_scheme(probe));
    }
    set_look_cookie(response, "clawt_font",
                    clawt_web_form_value(request, "font"));
    set_look_cookie(response, "clawt_font_size",
                    clawt_web_form_value(request, "font_size"));
    set_look_cookie(response, "clawt_mono",
                    clawt_web_form_value(request, "mono"));
    set_look_cookie(response, "clawt_mono_size",
                    clawt_web_form_value(request, "mono_size"));
    /*
     * The unit and the amount arrive as two fields and are stored as
     * one self-describing value, through the library's own formatter.
     * Two cookies would let them arrive apart -- a browser that kept
     * the amount and dropped the unit would read 640 as a percentage
     * and clamp it to the whole window, which is a setting nobody chose
     * and no error anywhere.
     */
    {
        ClawtMeasureUnit unit = clawt_measure_unit_from_nick(
            clawt_web_form_value(request, "measure_unit"));
        const gchar *amount = clawt_web_form_value(request, "measure");
        g_autofree gchar *stored = clawt_measure_to_string(
            unit, amount != NULL ? (gint)g_ascii_strtoll(amount, NULL, 10)
                                 : 0);

        set_look_cookie(response, "clawt_measure", stored);
    }
    /*
     * "1" is not written as a cookie at all -- an absent cookie already
     * means "show", which is the shipped behaviour, so choosing the
     * default leaves nothing behind rather than pinning it.  The same
     * reason an empty font is stored as no cookie rather than as the
     * browser's current family.
     */
    {
        const gchar *choice = clawt_web_form_value(request, "agent_desc");

        set_look_cookie(response, "clawt_agent_desc",
                        g_strcmp0(choice, "0") == 0 ? "0" : NULL);
    }

    set_look_cookie(response, "clawt_run_gap",
                    clawt_web_form_value(request, "run_gap"));

    /*
     * Reloaded rather than swapped: the palette and the fonts are
     * applied by <style> blocks in a head that has already been parsed.
     */
    htmx_response_add_header(response, "HX-Refresh", "true");

    return response;
}

static HtmxResponse *
on_appearance_reset(HtmxRequest *request, GHashTable *params,
                    gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(HtmxElement) content = NULL;
    HtmxResponse *response;
    static const gchar *const names[] = {
        "clawt_theme", "clawt_font", "clawt_font_size",
        "clawt_mono", "clawt_mono_size",
        "clawt_measure", "clawt_run_gap", "clawt_agent_desc"
    };
    guint i;

    (void)params;

    content = appearance_content(request);
    response = settings_response(app, request, "appearance", content,
                                 "Following the browser again.", FALSE);

    for (i = 0; i < G_N_ELEMENTS(names); i++)
        set_look_cookie(response, names[i], NULL);

    htmx_response_add_header(response, "HX-Refresh", "true");

    return response;
}

static HtmxResponse *
on_folder_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    const gchar *source = clawt_web_form_value(request, "source");
    const gchar *target = clawt_web_form_value(request, "target");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    /*
     * An empty target means the same path inside, which is what people
     * mean nine times out of ten -- an agent told about ~/source finds
     * it at a path that reads the same in both places, so a note about
     * a file is a note either of you can follow.
     */
    clawt_web_payload_set(payload, "source", source);
    clawt_web_payload_set(payload, "target",
                          (target != NULL && *target != '\0') ? target
                                                              : source);
    clawt_web_payload_set(payload, "mode",
                          clawt_web_form_value(request, "mode"));

    /*
     * One list, and the *daemon* sorts it into teams and agents. One
     * place decides what naming a team means, and it is the one all
     * three clients talk to -- a client answering it from `team.list`
     * misses a team that agents are on but nobody declared, and files
     * the name where it matches nothing.
     */
    {
        const gchar *raw = clawt_web_form_value(request, "who");

        if (raw != NULL && *raw != '\0') {
            g_auto(GStrv) items = g_strsplit(raw, ",", -1);
            gsize n;

            for (n = 0; items[n] != NULL; n++)
                g_strstrip(items[n]);

            clawt_web_payload_set_list(payload, "who",
                                       (const gchar *const *)items);
        }
    }

    reply = clawt_web_app_call(app, "defaults.mount.add",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = folders_content(app);

    if (failure != NULL)
        return settings_response(app, request, "folders", content, failure,
                                 TRUE);

    return settings_response(app, request, "folders", content,
                             "Shared with every agent. Restart one for it "
                             "to reach its computer.", FALSE);
}

static HtmxResponse *
on_folder_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autofree gchar *failure = NULL;
    ClawtWebApp *app = user_data;
    const gchar *target = clawt_web_form_value(request, "target");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxElement) content = NULL;

    (void)params;

    clawt_web_payload_set(payload, "target", target);

    reply = clawt_web_app_call(app, "defaults.mount.remove",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    content = folders_content(app);

    if (failure != NULL)
        return settings_response(app, request, "folders", content, failure,
                                 TRUE);

    return settings_response(app, request, "folders", content,
                             "No longer shared.", FALSE);
}

void
clawt_web_register_settings(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/settings", on_settings_index, app);
    htmx_router_get(router, "/settings/:page", on_settings_page, app);

    htmx_router_post(router, "/settings/fleet/pause", on_fleet_hold, app);
    htmx_router_post(router, "/settings/fleet/resume", on_fleet_hold, app);

    htmx_router_post(router, "/settings/images/download", on_image_download,
                     app);
    htmx_router_post(router, "/settings/images/:image/remove",
                     on_settings_action,
                     settings_action_new(app, "image.vm_remove", "image",
                                         "name", "images", "Removed."));
    htmx_router_post(router, "/settings/images/:image/cancel",
                     on_settings_action,
                     settings_action_new(app, "image.vm_cancel", "image",
                                         "name", "images", "Cancelled."));

    htmx_router_post(router, "/settings/folders/add", on_folder_add, app);
    htmx_router_post(router, "/settings/folders/remove", on_folder_remove,
                     app);
    htmx_router_post(router, "/settings/teams/add", on_team_add, app);
    htmx_router_post(router, "/settings/teams/:team/save", on_team_save, app);
    htmx_router_post(router, "/settings/teams/:team/remove",
                     on_settings_action,
                     settings_action_new(app, "team.remove", "team", "team",
                                         "teams", "Team removed."));

    htmx_router_post(router, "/settings/integrations/add", on_integration_add,
                     app);
    htmx_router_post(router, "/settings/integrations/:integration/health",
                     on_settings_action,
                     settings_action_new(app, "integration.health",
                                         "integration", "integration",
                                         "integrations", "Healthy."));
    htmx_router_post(router, "/settings/integrations/:integration/test",
                     on_settings_action,
                     settings_action_new(app, "integration.notify_test",
                                         "integration", "integration",
                                         "integrations", "Test sent."));
    htmx_router_post(router, "/settings/integrations/:integration/remove",
                     on_settings_action,
                     settings_action_new(app, "integration.remove",
                                         "integration", "name",
                                         "integrations", "Removed."));

    htmx_router_post(router, "/settings/connectors/add", on_connector_add,
                     app);
    htmx_router_post(router, "/settings/connectors/registry-refresh",
                     on_registry_refresh, app);
    htmx_router_get(router, "/settings/connectors/:connector/authorize",
                    on_connector_authorize, app);
    htmx_router_post(router, "/settings/connectors/:connector/await",
                     on_connector_await, app);
    htmx_router_post(router, "/settings/connectors/:connector/refresh",
                     on_settings_action,
                     settings_action_new(app, "connector.refresh",
                                         "connector", "name", "connectors",
                                         "Refreshed."));
    htmx_router_post(router, "/settings/connectors/:connector/revoke",
                     on_settings_action,
                     settings_action_new(app, "connector.revoke", "connector",
                                         "name", "connectors", "Revoked."));
    htmx_router_post(router, "/settings/connectors/:connector/remove",
                     on_settings_action,
                     settings_action_new(app, "integration.remove",
                                         "connector", "name", "connectors",
                                         "Removed."));

    htmx_router_post(router, "/settings/appearance", on_appearance, app);
    htmx_router_post(router, "/settings/appearance/reset",
                     on_appearance_reset, app);

    htmx_router_post(router, "/settings/connections/add", on_connection_add,
                     app);
    htmx_router_post(router, "/settings/connections/:connection/use",
                     on_connection_use, app);
    htmx_router_post(router, "/settings/connections/:connection/check",
                     on_connection_check, app);
    htmx_router_post(router, "/settings/connections/:connection/forget",
                     on_connection_forget, app);
}
