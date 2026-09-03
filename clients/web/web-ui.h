/*
 * web-ui.h - The page shell and the pieces every view is built from
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Elements are built with htmx-glib's typed classes rather than by
 * appending to a string, which is not only the library's own house style
 * -- it is what escapes agent output.  Every name, description and
 * message body in this client was written by a person or a model and is
 * served back over HTTP, so a "<" that reaches the page unescaped is
 * script injection into whoever opened it.  htmx_node_set_text_content()
 * escapes; a g_string_append() does not, and forgetting once is enough.
 */

#pragma once

#include "clawt-web.h"

G_BEGIN_DECLS

/* ── Views ───────────────────────────────────────────────────────── */

/*
 * Which page this client is showing is a #ClawtPage from the library,
 * and the groups it draws them in are #ClawtSection.
 *
 * There was an enum of nine here with its own slug table, and it had
 * already drifted: the GTK client had eleven pages, its doc comment said
 * eight, and no check could see any of it because each client's list was
 * its own.  The list belongs where both clients read it -- which is the
 * same rule the colour palettes and the computer sub-views already
 * follow, and what `make parity` layer 3 enforces.
 */

/* ── The document ────────────────────────────────────────────────── */

/**
 * clawt_web_page:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the selected agent
 * @view: which page to show
 * @body: (transfer none): the view's own content
 * @request: (nullable): the request being answered, whose cookies say
 *   what this browser should look like
 *
 * Wraps @body in the whole document: stylesheet, sidebar, nav, scripts.
 *
 * Returns: (transfer full): a complete HTML document
 */
gchar *clawt_web_page(ClawtWebApp  *app,
                      const gchar  *agent_id,
                      ClawtPage  view,
                      HtmxElement  *body,
                      HtmxRequest  *request);

/**
 * clawt_web_shell_page:
 * @app: a #ClawtWebApp
 * @title: what to call the page
 * @body: (transfer none): the content
 *
 * A document with no sidebar, for settings and the standalone flows.
 *
 * Returns: (transfer full): a complete HTML document
 */
gchar *clawt_web_shell_page(ClawtWebApp *app,
                            const gchar *title,
                            HtmxElement *body,
                            HtmxRequest *request);

/**
 * clawt_web_html_response:
 * @html: (transfer none): rendered markup
 *
 * Returns: (transfer full): a 200 carrying @html as HTML
 */
HtmxResponse *clawt_web_html_response(const gchar *html);

/**
 * clawt_web_fragment_response:
 * @element: (transfer none): the fragment to render
 *
 * Returns: (transfer full): a 200 carrying @element
 */
HtmxResponse *clawt_web_fragment_response(HtmxElement *element);

/**
 * clawt_web_redirect:
 * @request: the request being answered
 * @location: where to send the browser
 *
 * A redirect that works for htmx and for a plain form post alike.
 *
 * htmx follows a 303 by fetching it and swapping the body into the
 * target, which for a whole page is the wrong shape -- so an htmx
 * request is answered with HX-Redirect, which makes the browser
 * navigate, and everything else with the ordinary 303.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_redirect(HtmxRequest *request,
                                 const gchar *location);

/**
 * clawt_web_agent_url:
 * @agent_id: (nullable): an agent
 * @view: which page
 *
 * Returns: (transfer full): the path for that agent and view
 */
gchar *clawt_web_agent_url(const gchar *agent_id, ClawtPage view);

/* ── Pieces ──────────────────────────────────────────────────────── */

/**
 * clawt_web_card:
 * @title: (nullable): a heading
 * @subtitle: (nullable): a line under it
 *
 * A bordered panel: one border, generous padding, no shadow.
 *
 * Returns: (transfer full): the card, ready to have children added
 */
HtmxDiv *clawt_web_card(const gchar *title, const gchar *subtitle);

/**
 * clawt_web_card_body:
 * @card: a card from clawt_web_card()
 *
 * Where a card's content goes, which is not the card itself -- the
 * heading is already a child of it.
 *
 * Returns: (transfer none): the body element
 */
HtmxElement *clawt_web_card_body(HtmxDiv *card);

/**
 * clawt_web_badge:
 * @text: what it says
 * @tone: one of "neutral", "good", "warn", "bad", "info"
 *
 * Returns: (transfer full): a small uppercase tag
 */
HtmxSpan *clawt_web_badge(const gchar *text, const gchar *tone);

/**
 * clawt_web_state_tone:
 * @state: an agent state nickname
 *
 * Returns: the tone a badge for @state should use
 */
const gchar *clawt_web_state_tone(const gchar *state);

/**
 * clawt_web_avatar:
 * @name: the sender's name, for the derived initials
 * @agent_id: (nullable): whose picture to draw, or %NULL to derive a
 *   face from @name alone
 * @has_avatar: whether `agent.avatar` has bytes for @agent_id -- from
 *   `agent.list`/`agent.show`'s own field, so this never has to guess
 * @color: (nullable): `agents.color`, checked here through
 *   clawt_color_ink() before it reaches a style attribute
 * @css_class: the size/position class the caller already has CSS for
 *   (the sidebar and the transcript each draw a face at a different
 *   size)
 *
 * One face, drawn the same way in the sidebar and the transcript --
 * two builders for one kind of content is how this client's avatars and
 * the GTK client's drifted apart the first time, and how a second
 * builder here would drift from the first the same way.
 *
 * Resolution order: @agent_id's picture, as an `<img src="/a/:id/avatar">`
 * -- never a filesystem path, since the browser may not be on the
 * daemon's machine -- when @has_avatar says there is one; then @color;
 * then initials and a tone from the sheet's own palette, chosen by
 * @name.
 *
 * Returns: (transfer full): an `<img>` or a `<span>`, with @css_class
 *   already applied
 */
HtmxElement *clawt_web_avatar(const gchar *name,
                              const gchar *agent_id,
                              gboolean     has_avatar,
                              const gchar *color,
                              const gchar *css_class);

/**
 * clawt_web_row:
 * @title: the label
 * @value: (nullable): what to show on the right
 *
 * A label-and-value line, the web equivalent of an AdwActionRow.
 *
 * Returns: (transfer full): the row
 */
HtmxDiv *clawt_web_row(const gchar *title, const gchar *value);

/**
 * clawt_web_empty:
 * @text: what is not here
 * @detail: (nullable): why, or what to do about it
 *
 * Returns: (transfer full): a stated absence
 *
 * Every list says why it is empty rather than drawing nothing.  An
 * agent's mailbox is empty while it is running because delivery hands
 * items straight over, and a blank panel there reads as a broken queue.
 */
HtmxDiv *clawt_web_empty(const gchar *text, const gchar *detail);

/**
 * clawt_web_button:
 * @label: the text
 * @variant: "primary", "default" or "danger"
 *
 * Returns: (transfer full): a button
 */
HtmxButton *clawt_web_button(const gchar *label, const gchar *variant);

/**
 * clawt_web_post_button:
 * @label: the text
 * @action: where to post
 * @variant: "primary", "default" or "danger"
 * @confirm: (nullable): a question to ask first
 *
 * A button that posts on its own, so an action needs no surrounding form.
 *
 * Returns: (transfer full): the button
 */
HtmxButton *clawt_web_post_button(const gchar *label,
                                  const gchar *action,
                                  const gchar *variant,
                                  const gchar *confirm);

/**
 * clawt_web_field:
 * @label: what to call it
 * @name: the form field name
 * @value: (nullable): what it holds now
 * @placeholder: (nullable): a hint
 *
 * Returns: (transfer full): a labelled text input
 */
HtmxDiv *clawt_web_field(const gchar *label,
                         const gchar *name,
                         const gchar *value,
                         const gchar *placeholder);

/**
 * clawt_web_textarea_field:
 * @label: what to call it
 * @name: the form field name
 * @value: (nullable): what it holds now
 * @rows: how tall
 *
 * Returns: (transfer full): a labelled multi-line input
 */
HtmxDiv *clawt_web_textarea_field(const gchar *label,
                                  const gchar *name,
                                  const gchar *value,
                                  guint        rows);

/**
 * clawt_web_schema_flag:
 * @object: (nullable): a reply object
 * @key: the member to read
 *
 * Reads a schema BOOLEAN out of a daemon reply, accepting both a real
 * JSON boolean and the stringified "true" that `agent.show` produces.
 *
 * Returns: %TRUE when @key is present and on
 */
gboolean clawt_web_schema_flag(JsonObject *object, const gchar *key);

/**
 * clawt_web_switch_field:
 * @label: what to call it
 * @name: the form field name
 * @subtitle: (nullable): what it means
 * @on: whether it is set
 *
 * Returns: (transfer full): a labelled checkbox
 */
HtmxDiv *clawt_web_switch_field(const gchar *label,
                                const gchar *name,
                                const gchar *subtitle,
                                gboolean     on);

/**
 * clawt_web_select_field:
 * @label: what to call it
 * @name: the form field name
 * @values: the option values
 * @labels: (nullable): what to call each, or %NULL to use @values
 * @current: (nullable): which is selected
 *
 * A combo whose current value is always among its options.
 *
 * A value that is not in @values is added to the list rather than
 * dropped.  Without that the control opens showing the first entry and
 * saving the form writes it back over whatever somebody had chosen --
 * the same failure the GTK client's screen-size row was fixed for.
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_select_field(const gchar        *label,
                                const gchar        *name,
                                const gchar *const *values,
                                const gchar *const *labels,
                                const gchar        *current);

/**
 * clawt_web_hidden:
 * @name: the field name
 * @value: its value
 *
 * Returns: (transfer full): a hidden input
 */
HtmxInput *clawt_web_hidden(const gchar *name, const gchar *value);

/**
 * clawt_web_form:
 * @action: where it posts
 *
 * A form that posts over htmx and falls back to an ordinary submit.
 *
 * Returns: (transfer full): the form
 */
HtmxForm *clawt_web_form(const gchar *action);

/**
 * clawt_web_section_title:
 * @text: the heading
 *
 * Returns: (transfer full): a section heading
 */
HtmxHeading *clawt_web_section_title(const gchar *text);

/**
 * clawt_web_text:
 * @text: (nullable): what to say
 * @css_class: (nullable): a class for it
 *
 * Returns: (transfer full): a paragraph
 */
HtmxP *clawt_web_text(const gchar *text, const gchar *css_class);

/**
 * clawt_web_add:
 * @parent: (transfer none): where it goes
 * @child: (transfer full): what to add
 *
 * Adds @child to @parent and releases the caller's reference, so a
 * builder reads as a sequence of additions rather than as a sequence of
 * autoptr blocks.
 */
void clawt_web_add(gpointer parent, gpointer child);

/**
 * ClawtWebLook:
 * @theme: "system", "light" or "dark"
 * @font: (nullable): interface font family, or %NULL to follow the browser
 * @font_size: interface size in px, or 0 to follow the browser
 * @mono: (nullable): code font family, or %NULL to follow the browser
 * @mono_size: code size in px, or 0 to follow the browser
 *
 * What this browser has been told to look like.
 *
 * Zeroed means "defer" for every field, deliberately -- and a field added
 * later therefore defaults to deferring too. Naming the browser's current
 * font instead of saying nothing is the same mistake the GTK client's
 * appearance page was fixed for: the two look identical on screen and
 * diverge for ever afterwards, because one keeps following and the other
 * has quietly frozen.
 */
typedef struct {
    gchar *theme;
    gchar *font;
    gint   font_size;
    gchar *mono;
    gint   mono_size;

    /*
     * The conversation column and the gap between runs, or the default
     * unit and 0 to follow the shipped values.  Bounded by the
     * library's own limits rather than numbers of this client's own, so
     * the two clients cannot disagree about what is allowed.
     *
     * The unit is stored beside the amount for the reason the
     * appearance file stores them together: 90 alone is nine tenths of
     * the window, ninety characters or ninety pixels, and the cookie
     * carries one self-describing spelling rather than two keys that
     * can arrive apart.
     */
    ClawtMeasureUnit measure_unit;
    gint   measure;
    gint   run_gap;

    /*
     * Whether the sidebar keeps each agent's description for the
     * pointer instead of writing it under the name.
     *
     * Stored as the negation, so the zero above still means "what the
     * client has always done" -- a boolean has no third state to spend
     * on deferring, so the choice is which of the two is free.  The
     * same shape, and the same reasoning, as the GTK client's
     * appearance file; read clawt_appearance_get_show_descriptions()
     * for the whole of it.
     */
    gboolean hide_descriptions;
} ClawtWebLook;

/**
 * clawt_web_look_free:
 * @self: (transfer full) (nullable): a #ClawtWebLook
 */
void clawt_web_look_free(ClawtWebLook *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtWebLook, clawt_web_look_free)

/**
 * clawt_web_look_from_request:
 * @request: the request, whose cookies carry the choices
 *
 * Returns: (transfer full): what this browser asked for
 */
ClawtWebLook *clawt_web_look_from_request(HtmxRequest *request);

/**
 * clawt_web_look_css:
 * @look: (nullable): what this browser asked for
 *
 * The token overrides for @look, as a CSS block, or an empty string.
 *
 * A family is *sanitised* rather than escaped: quotes, braces,
 * semicolons and angle brackets are dropped. CSS string escapes are
 * their own small language and there is nothing here to preserve -- no
 * font has a brace in its name -- while getting the escaping subtly
 * wrong would let a cookie close the declaration and open a rule of its
 * own.
 *
 * Returns: (transfer full): the CSS, never %NULL
 */
gchar *clawt_web_look_css(const ClawtWebLook *look);

/**
 * clawt_web_relative_time:
 * @timestamp: unix seconds
 *
 * Returns: (transfer full): "3m ago", or an empty string for 0
 */
gchar *clawt_web_relative_time(gint64 timestamp);

/**
 * clawt_web_one_line:
 * @text: (nullable): anything
 * @limit: how many characters to keep
 *
 * Returns: (transfer full): @text with newlines folded out, elided
 */
gchar *clawt_web_one_line(const gchar *text, glong limit);

/**
 * clawt_web_write_is_cross_site:
 * @method: the request's method
 * @sec_fetch_site: (nullable): the `Sec-Fetch-Site` header
 * @origin: (nullable): the `Origin` header
 * @referer: (nullable): the `Referer` header
 * @host: (nullable): the `Host` header
 *
 * Whether a state-changing request came from somewhere other than this
 * page, and must therefore be refused.
 *
 * There is no login here, and that is a decision: anything that can
 * reach a listed address can drive the fleet.  The trust boundary is
 * *network reachability*, and a form on any page the operator happens to
 * open quietly widens it to *anything their browser loads* -- a
 * `POST /a/x/exec` needs no reply to be read and no route to the
 * tailnet, only a tab.  So a browser's own account of where a write came
 * from is required to say "here" before it is honoured.
 *
 * Takes the header values rather than the request, so the decision can
 * be tested without standing up a server -- the shapes that matter are
 * the ones a real browser sends and a test client cannot.
 *
 * A request carrying none of the three is allowed: that is `curl`, not a
 * browser, and a browser cannot be persuaded to omit them on a
 * cross-origin write.
 *
 * Returns: %TRUE when the request must be refused
 */
gboolean clawt_web_write_is_cross_site(HtmxMethod   method,
                                       const gchar *sec_fetch_site,
                                       const gchar *origin,
                                       const gchar *referer,
                                       const gchar *host);

G_END_DECLS
