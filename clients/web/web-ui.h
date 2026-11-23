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
 * clawt_web_failover_badge:
 * @agent: (nullable): an agent as agent.list or agent.show reports it
 *
 * "On fallback: codex-cli gpt-5.5" while the agent runs on one of its
 * model.fallbacks, titled with the sentence saying why and until when.
 *
 * Returns: (transfer full) (nullable): the badge, or %NULL while the
 *   agent is on its own backend
 */
HtmxSpan *clawt_web_failover_badge(JsonObject *agent);

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
 * daemon's machine -- when @has_avatar says there is one; then its
 * generated face (clawt_web_face_for()), an inline SVG in @color or the
 * derived colour, which the stylesheet blinks and the page-head script
 * turns toward the pointer; then, for `initials`, the letter on @color
 * or a tone from the sheet's own palette.
 *
 * Returns: (transfer full): an `<img>` or a `<span>`, with @css_class
 *   already applied
 */
/**
 * clawt_web_face_note:
 * @agent_id: an agent's id
 * @face: (nullable): its resolved face, `agent.list`'s `face`
 *
 * Remembers which face the daemon resolved for an agent, so the
 * transcript draws the face the sidebar drew without carrying the field
 * through every page.  %NULL forgets it.
 */
void clawt_web_face_note(const gchar *agent_id, const gchar *face);

/**
 * clawt_web_face_for:
 * @agent_id: (nullable): an agent's id
 *
 * Returns: the shape to draw, or %CLAWT_FACE_INITIALS: what
 *   clawt_web_face_note() was told, the automatic face when nothing was,
 *   and the letter disc for a shape this build does not know
 */
gint clawt_web_face_for(const gchar *agent_id);

/**
 * clawt_web_face_svg:
 * @name: (nullable): the display name, for the derived colour
 * @agent_id: (nullable): whose face; %NULL keys it on @name
 * @color: (nullable): `agents.color`, as typed
 * @size: the width and height the SVG declares
 * @animated: whether to emit the classes the stylesheet blinks
 *
 * The generated face clawt_web_avatar() draws, as SVG markup: the shape
 * clawt_web_face_for() resolves, painted in clawt_avatar_color().  The
 * Office canvas draws its heads from this too, so a face in the office
 * is the face in the sidebar rather than a second builder's idea of it.
 *
 * Returns: (transfer full) (nullable): the `<svg>`, or %NULL when the
 *   face is the letter disc (%CLAWT_FACE_INITIALS)
 */
gchar *clawt_web_face_svg(const gchar *name, const gchar *agent_id,
                          const gchar *color, guint size, gboolean animated);

/**
 * clawt_web_face_picker:
 * @agent_id: (nullable): whose face; "automatic" shows its own
 * @face: (nullable): the value to start on: a shape name, `initials`,
 *   or %NULL / "" for automatic
 * @color: (nullable): likewise, a hex colour or %NULL / "" for automatic
 *
 * The face picker as form fields: a preview, a `face` radio group (one
 * tile a shape, plus automatic and initials), a `color` radio group
 * (the automatic tone and the palette) and a Shuffle button.  Works
 * with no script at all -- the radios are the form -- and the page-head
 * script keeps the preview and the tiles' colour in step with the
 * choice.
 *
 * Returns: (transfer full): a `<fieldset class="face-picker">`
 */
/**
 * clawt_web_face_changes:
 * @saved_face: (nullable): `agents.face` when the page was drawn
 * @face: (nullable): what the picker sent; %NULL means "not sent"
 * @saved_color: (nullable): `agents.color` when the page was drawn
 * @color: (nullable): likewise
 *
 * Which of `face` and `color` the agent page's Face form should write:
 * only what was sent and differs from what was saved, so picking a
 * colour never rewrites a face somebody set by hand in the YAML, and a
 * form that did not carry a field writes nothing for it.  %NULL saved
 * values count as "".
 *
 * Returns: (transfer full) (element-type utf8): the keys, in the order
 *   face, color
 */
GPtrArray *clawt_web_face_changes(const gchar *saved_face,
                                  const gchar *face,
                                  const gchar *saved_color,
                                  const gchar *color);

HtmxElement *clawt_web_face_picker(const gchar *agent_id,
                                   const gchar *face,
                                   const gchar *color);

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
 * clawt_web_agent_row_menu:
 * @agent_id: the agent the row is for
 * @unread: how many messages wait in its conversation with the operator
 * @muted: whether the agent is muted (`notice.mute`)
 *
 * The agent list's per-row menu: a `<details class="row-menu">` drawn
 * after the row, which the page script opens on a right-click of the
 * row (the `contextmenu` handler in this file's script) so the gesture
 * that works in the GTK and lrg clients works here too.
 *
 * Two items, always: "Mark as read" -- worded by
 * clawt_agent_menu_read_label(), and disabled when there is nothing to
 * read -- posting to `/a/<id>/read`; and "Mute notifications" or
 * "Unmute" (clawt_agent_menu_mute_label()) posting to `/a/<id>/mute` or
 * `/a/<id>/unmute`.
 *
 * Returns: (transfer full) (nullable): the menu, or %NULL with no agent
 */
HtmxElement *clawt_web_agent_row_menu(const gchar *agent_id,
                                      guint        unread,
                                      gboolean     muted);

/**
 * clawt_web_notices_sections:
 * @reply: (nullable): a `notice.list` reply's payload
 * @agent: (nullable): only this agent's rows, or %NULL for the fleet
 * @now_us: now, for the alerts' relative times
 * @out_shown: (out) (optional): how many rows were drawn
 *
 * The body of the Notifications page, in the four sections every client
 * draws (docs/clients.org, "Notifications"): Chats, Decisions waiting,
 * Grok jobs needing attention, and Alerts.  A section with nothing in it
 * is left out.  Each row is followed by its `<details class="row-menu">`,
 * which the page script opens on a right-click as it does the agent
 * list's: Mark as read and Mute for a conversation, Remove (asked first)
 * for a Grok job, Mark read, Dismiss and Mute for an alert.
 *
 * Pure -- a reply in, markup out -- so tests/test-web-render.c draws it
 * without a daemon.
 *
 * Returns: (transfer full): the sections
 */
HtmxElement *clawt_web_notices_sections(JsonObject  *reply,
                                        const gchar *agent,
                                        gint64       now_us,
                                        guint       *out_shown);

/**
 * clawt_web_mark_all_read_button:
 * @total: every conversation's unread count added up
 *
 * "Mark all as read", for the Chat heading: posts to `/read-all`.
 *
 * Returns: (transfer full) (nullable): the button, or %NULL when
 *   nothing is unread
 */
HtmxElement *clawt_web_mark_all_read_button(guint total);

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
 * clawt_web_effort_field:
 * @name: the form field name
 * @current: (nullable): the value set now
 *
 * The model-effort drop-down, walked from clawt_effort_count() (0.51.3).
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_effort_field(const gchar *name, const gchar *current);

/**
 * clawt_web_weekday_field:
 * @name: the form field name
 * @current: (nullable): the value set now
 *
 * The weekly routine's day, walked from clawt_weekday_count().
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_weekday_field(const gchar *name, const gchar *current);

/**
 * clawt_web_schedule_field:
 * @name: the form field name
 * @current: (nullable): the value set now
 *
 * A routine's schedule, walked from clawt_schedule_count().
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_schedule_field(const gchar *name, const gchar *current);

/**
 * clawt_web_resolution_field:
 * @label: what to call it
 * @name: the form field name
 * @current: (nullable): the value set now; any other WxH is kept
 *
 * A screen size, walked from clawt_resolution_count().
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_resolution_field(const gchar *label, const gchar *name,
                                    const gchar *current);

/**
 * clawt_web_review_by_field:
 * @name: the form field name
 * @agents: (nullable) (array zero-terminated=1): every agent id
 * @project: %TRUE on a project's form
 * @current: (nullable): the value set now
 *
 * Who reviews: the fixed choices from clawt_review_by_count(), then each
 * agent.
 *
 * Returns: (transfer full): a labelled select
 */
HtmxDiv *clawt_web_review_by_field(const gchar        *name,
                                   const gchar *const *agents,
                                   gboolean            project,
                                   const gchar        *current);

/**
 * clawt_web_fallback_card:
 * @agent_id: (nullable): whose list it is
 * @spelled: (nullable): `model.fallbacks` as agent.show reports it
 * @own_provider: (nullable): the agent's own provider, where a new entry
 *   starts
 * @catalog: (nullable): a model.list reply
 * @accounts: (nullable): an account.list reply
 *
 * The ordered fallback editor: a row per entry with its provider, its
 * model (a text field suggesting the catalogue's) and its account, and
 * buttons to move or remove it, then a row to add one.  It posts to
 * `/a/:id/fallbacks` with `op` naming the edit; every button saves.
 *
 * A list that cannot be parsed is shown as the reason instead of a form,
 * so the page never saves over a list it could not show.
 *
 * Returns: (transfer full): the card
 */
HtmxDiv *clawt_web_fallback_card(const gchar *agent_id,
                                 const gchar *spelled,
                                 const gchar *own_provider,
                                 JsonObject  *catalog,
                                 JsonObject  *accounts);

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

    /*
     * Whether faces stay still (0.53.0): no blink, no eyes following
     * the pointer.  Negated for the same reason as hide_descriptions --
     * zero is faces that move, which is the shipped behaviour.  The
     * browser's own prefers-reduced-motion stops them too, whatever this
     * says.
     */
    gboolean still_faces;

    /*
     * The linked-fleet chips switched on in this browser (0.67.0), as
     * selector terms, plus `hidden` (draw hidden fleets), `more` (offer
     * the owner and tag chips) and `fold:ALIAS` per folded fleet.  NULL
     * is none, which draws every fleet that is not hidden.  Per browser,
     * like the GTK client's chips are per window: which fleets somebody
     * is looking at is about them, not about the fleet.
     */
    gchar **fed_filter;
} ClawtWebLook;

/**
 * clawt_web_look_free:
 * @self: (transfer full) (nullable): a #ClawtWebLook
 */
void clawt_web_look_free(ClawtWebLook *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtWebLook, clawt_web_look_free)

#define CLAWT_WEB_FED_FILTER_COOKIE "clawt_fed_filter"

/**
 * clawt_web_fed_filter_parse:
 * @value: (transfer full) (nullable): the `clawt_fed_filter` cookie
 *
 * Splits the cookie into its terms, dropping empty, overlong and
 * non-UTF-8 ones.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   terms, or %NULL for none
 */
gchar **clawt_web_fed_filter_parse(gchar *value);

/**
 * clawt_web_fed_filter_toggle:
 * @terms: (nullable) (array zero-terminated=1): the terms now
 * @term: the one to switch
 *
 * Returns: (transfer full): the cookie value with @term switched
 */
gchar *clawt_web_fed_filter_toggle(gchar       **terms,
                                   const gchar  *term);

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

/**
 * clawt_web_fold_element:
 * @body: (transfer full): the message body element, as the transcript
 *   would have drawn it
 * @agent_id: the agent whose conversation this is
 * @peer_id: the agent that sent the message
 * @message_id: (nullable): the message's id, for the element's own id
 * @text: (nullable): the message's raw text, for the folded line
 *
 * A message clawt_chat_should_fold() said to fold: a `<details>` whose
 * summary is the one-line preview, holding @body and a row with "Show
 * less" and "View exchange".
 *
 * A `<details>` so it opens and closes with scripting off.  It carries
 * an id and `hx-preserve`, because htmx swaps the whole transcript on
 * every fleet event and an opened request would otherwise snap shut
 * each time anything happened anywhere.  "View exchange" is a real link
 * to the exchange page and, with htmx, loads the same list into the
 * side panel instead.
 *
 * Returns: (transfer full): the element
 */
HtmxElement *clawt_web_fold_element(HtmxElement *body,
                                    const gchar *agent_id,
                                    const gchar *peer_id,
                                    const gchar *message_id,
                                    const gchar *text);

/**
 * clawt_web_exchange_url:
 * @agent_id: the agent whose conversation it is
 * @peer_id: the agent whose requests are listed
 * @fragment: %TRUE for the side panel's fragment, %FALSE for the page
 *
 * Returns: (transfer full): `/a/<agent>/exchange?with=<peer>`, or under
 *   `/f/` for the fragment, each part escaped
 */
gchar *clawt_web_exchange_url(const gchar *agent_id, const gchar *peer_id,
                              gboolean fragment);

/**
 * clawt_web_exchange_element:
 * @exchange: (nullable): what clawt_chat_exchange() kept
 * @agent_id: the agent whose conversation it is
 * @peer_id: the agent whose requests are listed
 * @in_panel: %TRUE inside the chat's side panel (a close button),
 *   %FALSE as a page of its own (a link back to the conversation)
 *
 * The exchange: a heading from clawt_chat_exchange_title() and one entry
 * per message, the peer's requests marked apart from the answers, each
 * body rendered from markdown like the transcript's.
 *
 * Returns: (transfer full): the element
 */
HtmxElement *clawt_web_exchange_element(JsonArray   *exchange,
                                        const gchar *agent_id,
                                        const gchar *peer_id,
                                        gboolean     in_panel);

/**
 * CLAWT_WEB_MANIFEST_PATH:
 *
 * Where the web app manifest is served, and what every page links.
 */
#define CLAWT_WEB_MANIFEST_PATH "/manifest.webmanifest"

/**
 * CLAWT_WEB_MANIFEST_TYPE:
 *
 * The manifest's media type.  Browsers accept `application/json` too,
 * but this is the registered one and the one Chromium's installability
 * check names.
 */
#define CLAWT_WEB_MANIFEST_TYPE "application/manifest+json"

/**
 * clawt_web_manifest_json:
 *
 * The web app manifest: the name, `start_url` and `scope` of `/`,
 * `display: standalone`, the theme colours, and the vendored icons
 * under `/static/icons/`.  Nothing in it names a host, so one manifest
 * is right for every address the client listens on.
 *
 * Returns: (transfer full): the manifest, as JSON
 */
gchar *clawt_web_manifest_json(void);

/**
 * clawt_web_manifest_response:
 *
 * The manifest as a response: %CLAWT_WEB_MANIFEST_TYPE, and a short
 * cache lifetime so an upgraded client's manifest is picked up.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_manifest_response(void);

/**
 * clawt_web_decision_inline:
 * @decision: one decision as `decision.list` puts it on the wire
 * @notice: (nullable): a refusal to write into the card, after a press
 *   the daemon turned down
 *
 * The inline view of a decision in the chat it was asked from: who asks,
 * the question, and while it is open the options as buttons posting
 * `decision.answer` with `via: chat` and the default with its deadline;
 * once settled, how it ended and who ended it.
 *
 * Returns: (transfer full): the card
 */
HtmxElement *clawt_web_decision_inline(JsonObject  *decision,
                                       const gchar *notice);

/**
 * clawt_web_step_element:
 * @step: one step of a turn that is not part of a tool run
 *
 * A step drawn on its own: its sentence in its tone, a trimmed-result
 * notice (clawt_mcp_result_is_notice()) marked as one, and a step that
 * shows an MCP app as the app's card -- closed, with an Open button, so
 * the page fetches nothing until somebody asks.
 *
 * Returns: (transfer full): the element
 */
HtmxElement *clawt_web_step_element(ClawtTurnStep *step);

/**
 * clawt_web_app_card:
 * @agent_id: the agent whose conversation it is in
 * @app_id: the app
 * @text: (nullable): what the step says about it
 *
 * An MCP app in the transcript, closed.  Opening it fetches the frame
 * fragment into the card; the card keeps its id and `hx-preserve`, so a
 * transcript swap does not close an app somebody is using.
 *
 * Returns: (transfer full): the card
 */
HtmxElement *clawt_web_app_card(const gchar *agent_id,
                                const gchar *app_id,
                                const gchar *text);

/**
 * clawt_web_app_frame:
 * @app: a `connector.app_get` reply
 *
 * An opened MCP app: an iframe with `sandbox` exactly
 * %CLAWT_MCP_APP_IFRAME_SANDBOX and the daemon's `document` as its
 * `srcdoc`, the tool's input and result for the page's host bridge, the
 * origins its policy left out -- or, for a view that could not be kept,
 * the reason and no frame at all.
 *
 * Returns: (transfer full): the element
 */
HtmxElement *clawt_web_app_frame(JsonObject *app);

/**
 * clawt_web_app_bridge_parse:
 * @body: (nullable): what the page's host bridge forwarded: the view's
 *   own JSON-RPC message, as it was posted
 * @tool: (out) (transfer full): the tool it asks for
 * @arguments: (out) (transfer full) (nullable): its arguments object
 * @id: (out) (transfer full) (nullable): the message's id, to answer with
 * @refusal: (out) (transfer full) (nullable): when it is not a call the
 *   host makes, the JSON-RPC answer refusing it
 *
 * Reads one message a view sent for its server.  Only `tools/call` with
 * a name, an id and (optionally) an arguments object is a call;
 * `ui/message` and `ui/open-link` -- a view writing into the
 * conversation, or sending the person to an address it chose -- and
 * anything else are refused, with the reason in @refusal.  Kept out of
 * the page's script so it is tested without a browser.
 *
 * Returns: %TRUE for a call to make
 */
gboolean clawt_web_app_bridge_parse(const gchar  *body,
                                    gchar       **tool,
                                    JsonNode    **arguments,
                                    JsonNode    **id,
                                    gchar       **refusal);

/**
 * clawt_web_app_bridge_response:
 * @id: (nullable): the view's message id
 * @result: (nullable): the tool's result object
 * @error: (nullable): why there is none
 *
 * The JSON-RPC answer the page's bridge posts back into the view.
 *
 * Returns: (transfer full): one JSON-RPC response
 */
gchar *clawt_web_app_bridge_response(JsonNode    *id,
                                     JsonObject  *result,
                                     const gchar *error);

/**
 * clawt_web_budget_form:
 * @escaped: the connector's name, URI-escaped
 * @own: its own `max_result_bytes`, -1 when it sets none
 * @effective: (nullable): what that comes to, as libclawt labels it
 *
 * Returns: (transfer full): the connector's result-budget form
 */
HtmxElement *clawt_web_budget_form(const gchar *escaped,
                                   gint64       own,
                                   const gchar *effective);

/**
 * clawt_web_apps_form:
 * @escaped: the connector's name, URI-escaped
 * @on: whether it shows its apps in chat
 *
 * Returns: (transfer full): the "Show its apps in chat" form
 */
HtmxElement *clawt_web_apps_form(const gchar *escaped,
                                 gboolean     on);

/**
 * clawt_web_fleet_budget:
 * @value: `connectors.max_result_bytes`
 *
 * Returns: (transfer full): the fleet's result-budget form
 */
HtmxElement *clawt_web_fleet_budget(gint64 value);

/* Append an escaped destination if it matches; return 1 when appended. */
guint clawt_web_navigation_result(HtmxElement *list, const gchar *query,
                                  const gchar *name, const gchar *detail,
                                  const gchar *id, const gchar *url);

G_END_DECLS
