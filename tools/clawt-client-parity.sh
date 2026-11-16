#!/usr/bin/env bash
#
# clawt-client-parity.sh - The graphical clients (GTK, web and, when it is
# in the tree, libregnum) answer for the same daemon, so they should reach
# the same parts of it.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Two clients drifting apart is invisible: nothing breaks, nothing warns,
# and somebody finds out by reaching for the half that was not built.  So
# the rule -- a feature in one exists in the other -- is checked rather
# than remembered, in the same spirit as the stale-config-key and
# undefined-tool checks `make docs-check` already runs.
#
# Two things are compared.
#
# The set of IPC frame kinds each client sends is a proxy for "feature",
# not a definition of one: a client could name a kind and do nothing
# useful with it.  But every real feature has to talk to the daemon to do
# anything, so a kind one client sends and the other never mentions is a
# capability that exists in one place only.
#
# The set of slash commands is compared because the frame-kind check
# missed them entirely -- thirteen of the eighteen were absent from the
# web client and nothing said so, since /files and /agents and /export are
# all built out of frames both clients already sent.  A check that only
# looks at one layer will keep finding nothing at the others.
#
# Two more layers, added after that sentence came true a second time.
# Catppuccin Mocha went into libclawt, which both clients link, and was
# selectable in one of them; the unread marker was built for the GTK
# transcript and not the web one.  Neither sends a frame or answers a
# slash command, so the two checks above reported OK throughout.
#
#   3. **Choice enumerations.**  A `_count()`/`_nth()` family in the
#      public API exists so a client can offer a set of values without
#      naming them.  If either client walks one, both must -- that is
#      exactly the Mocha shape, and it needs nobody to declare anything.
#
#   4. **Hardcoded vocabulary.**  The cause, rather than the symptom: a
#      client naming a value the library enumerates has a copy of the
#      list, and a copy is what drifts.  The values are read out of the
#      library's own table, so a palette added there is checked from the
#      moment it exists.
#
#   5. **Declared affordances.**  Some capabilities are pure interface --
#      the unread marker is a rule and a pill -- and touch no shared
#      symbol at all.  Those are declared here with a marker per client.
#      This layer is honest about its limit: it catches one half being
#      removed or never written, and it cannot catch a feature nobody
#      declared.  Layers 3 and 4 need no declaration; this one does.
#
# What layer 2 cannot see, stated rather than implied: it compares
# `"/name"` *literals*, so a command whose name comes from a skill the
# agent happens to have is invisible to it.  Neither client contains
# those names.  They are declared as affordances instead -- which is
# weaker, and is the honest amount of checking available.

set -euo pipefail

CDPATH=

api_text=""

# CDPATH is cleared above, so a bare cd cannot echo the directory it
# resolved to and have the command substitution capture it as well as pwd's
# own output.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
#
# The daemon's client surface, which is one file per verb family.
#
# A glob rather than a name, because the dispatch used to be one chain in
# clawt-daemon.c and moving it out left this reading a file with no kinds
# in it at all -- which reported OK for every client, since a client
# cannot be missing a kind that nothing found.
#
DAEMON_SOURCES=("${ROOT}"/src/core/clawt-daemon.c "${ROOT}"/src/core/daemon-*.c)

#
# Kinds a client is allowed to lack, each with the reason.
#
# An entry here is a decision, not a to-do.  Anything without a reason
# somebody would agree with belongs in the client instead.
#
# Read through a nameref in report_missing(), which shellcheck cannot see.
# shellcheck disable=SC2034
declare -A WEB_MAY_LACK=(
    ["config.render"]="reading a rendered libreclaw config is a debugging step; \`clawtilla config render\` is the surface for it"
)

# shellcheck disable=SC2034
declare -A GTK_MAY_LACK=(
    ["agent.file_read"]="same capability, different mechanism: /edit opens the file in \$EDITOR there, which is the better answer when the client and the daemon share a machine"
    ["agent.file_write"]="the other half of the same; \$EDITOR writes the file directly"
    ["browser.list"]="the Browser view's line comes from browser.frame's reply here; the web client reads browser.list for it because its picture is a separate request, and two frames per poll would be two screenshots"
    ["connector.app_call"]="an MCP app's own tools/call comes from its view, and only the web client can draw a view; GTK's Open ... in the browser link opens the web client's page of the same app, where the call is made (docs/connectors.org, \"Apps in chat\")"
)

#
# Where the library keeps a set of values a person chooses between, and
# how to read the values out of it.
#
# Each entry is a file and a sed script that prints one value per line.
# A client is not allowed to contain any of them as a string literal:
# having the list is what makes a client able to disagree with it.
#
# shellcheck disable=SC2034
declare -A VOCABULARIES=(
    ["colour scheme"]="src/config/clawt-appearance.c|s/.*{ *CLAWT_THEME_[A-Z_]*, *\"\([a-z-]*\)\".*/\1/p"
    #
    # CLAWT_MEASURE_DEFAULT is deliberately not in this list. Its nick is
    # "default", which is also a button style in the web client and a
    # dozen other things besides -- so it is evidence of nothing, and a
    # check that reports a hardcoding which is not there is one people
    # learn to ignore. The units that can actually go missing from a
    # client are the three with values.
    #
    ["measure unit"]="src/config/clawt-appearance.c|s/.*{ *CLAWT_MEASURE_\(PERCENT\|COLUMNS\|PIXELS\), *\"\([a-z]*\)\".*/\2/p"
)

#
# Capabilities that are interface and nothing else, with the marker that
# proves each client has one.
#
# A marker is a symbol or a class the feature cannot work without, not a
# label somebody might reword.  Adding a row here is the declaration; the
# check is that both halves answer to it.
#
# shellcheck disable=SC2034
declare -A AFFORDANCES=(
    ["searchable navigation"]="clawt_gtk_build_navigation|navigation-search|navigation_search_changed"
    ["unread marker"]="unread_marker|unread-rule|unread_rule_new"
    ["live turn steps"]="clawt_gtk_steps_add|turn_steps|steps_add_live"
    ["tool run collapsing"]="clawt_turn_step_run_summary|clawt_turn_step_run_summary|clawt_turn_step_run_summary"
    ["step tone"]="clawt_turn_step_tone|clawt_turn_step_tone|clawt_turn_step_tone"
    ["expand a tool run"]="gtk_expander_set_label_widget|htmx_summary_new_with_text|lrg_expander_new"
    ["steps merged into history"]="clawt_turn_step_precedes|clawt_turn_step_precedes|clawt_turn_step_precedes"
    ["jump to latest"]="jump_revealer|jump-pill|on_jump_clicked"
    ["transcript measure"]="adw_clamp_new|transcript-inner|clawt_measure_resolve_px"
    ["markdown rendering"]="clawt_markdown_to_pango|clawt_markdown_to_html|clawt_lrg_markdown"
    ["attachment previews"]="append_attachment_previews|attachment|attachment_previews_append"
    ["unread count"]="clawt-unread-badge|unread-badge|unread-badge"
    ["unread tab total"]="adw_view_stack_page_set_badge_number|unread_total|clawt_lrg_unread_badge"
    ["message runs"]="clawt_chat_run_is_start|clawt_chat_run_is_start|clawt_chat_run_is_start"
    ["transcript stamps"]="clawt_chat_time_label|clawt_chat_time_label|clawt_chat_time_label"
    ["day dividers"]="day_divider|day-divider|day_divider_new"
    ["sender avatars"]="adw_avatar_new|msg-avatar|sender_avatar"
    ["own-turn bubbles"]="clawt-bubble|msg-self|own_bubble_new"
    ["alerts surface"]="build_alerts_panel|alert-row|build_alerts_panel"
    ["alert severity tiers"]="clawt_alert_tier_for_event|clawt_alert_tier_for_event|clawt_alert_tier_for_event"
    ["unread rule"]="clawt_unread_should_count|clawt_unread_should_count|clawt_unread_should_count"
    ["team tally"]="clawt_team_tally|clawt_team_tally|clawt_team_tally"
    ["fleet warnings"]="append_warning_rows|clawt_web_warnings|append_warning_rows"
    ["peer conversations"]="fill_conversation_menu|conversation_switcher|conversation_picker"
    ["team busy marker"]="clawt-team-busy|clawt-team-busy|clawt-team-busy"
    ["follow the live edge"]="clawt_transcript_is_at_bottom|transcript-end|clawt_transcript_is_at_bottom"
    ["alert filter"]="alerts_show_all|show_all|alerts_show_all"
    ["agent attachments"]="fetch_attachment|attachment_element|fetch_agent_attachment"
    ["fleet activity ages"]="clawt_activity_elapsed_label|clawt_activity_elapsed_label|clawt_activity_elapsed_label"
    ["stop is agent-wide"]="clawt_activity_stop_prompt|clawt_activity_stop_prompt|clawt_activity_stop_prompt"
    ["steer only where it reaches"]="\"steerable\"|\"steerable\"|\"steerable\""
    ["flow groups by sender"]="flow_run_sender|flow_run_sender|flow_run_sender"
    ["alert arrives read"]="clawt_alert_arrives_read|clawt_alert_arrives_read|clawt_alert_arrives_read"
    ["composer on the message column"]="CHAT_BODY_INSET|chat-gutter|CHAT_BODY_INSET"
    ["reading measurements"]="build_reading_group|clawt_run_gap|build_reading_card"
    ["measure units"]="clawt_measure_unit_count|clawt_measure_unit_count|clawt_measure_unit_count"
    ["computer types"]="clawt_computer_type_count|clawt_computer_type_count|clawt_computer_type_count"
    ["import modes"]="clawt_import_mode_count|clawt_import_mode_count|clawt_import_mode_count"
    ["palettes from disk"]="clawt_appearance_scheme_count|clawt_appearance_scheme_count|clawt_appearance_scheme_count"
    ["message boundary in a run"]="CHAT_MESSAGE_SPACING|msg-time|CHAT_MESSAGE_SPACING"
    ["decision inbox"]="build_decision_page|decision-row|decision-card"
    #
    # The count on the top-level tab, which is a different thing from
    # the page: the GTK client sums a section's pages onto its own tab
    # and the web client draws a pill on Work.  Neither touches a shared
    # symbol, so layers 3 and 4 cannot see it -- and the web half did
    # not exist until the daemon started saying when a decision was
    # settled, without which a count could only ever go up.
    #
    ["decision count on a tab"]="clawt_gtk_set_page_badge|clawt_web_app_open_decisions|CLAWT_PAGE_DECISIONS, open"
    ["connection reachability"]="clawt_connection_probe|clawt_connection_probe|clawt_connection_probe"
    ["identity size"]="clawt-identity-size|clawt-identity-size|clawt-identity-size"
    ["connection banner"]="connection_banner|clawt-connection-banner|clawt_lrg_update_banner"
    ["daemon version check"]="note_daemon_version|note_daemon_version|note_daemon_version"
    ["connection notice"]="clawt_connection_notice_text|clawt_connection_notice_text|clawt_connection_notice_text"
    ["start on a remote daemon"]="opt_profile|opt_profile|opt_profile"
    ["resync after a gap"]="\"resync\"|\"resync\"|\"resync\""
    ["stop the running turn"]="on_stop_turn|stop_turn_button|on_stop_turn_clicked"
    ["decision options stack"]="decision_option_button|decision-options|decision_option_button"
    #
    # A decision drawn inline in the room it was asked from, as a second
    # view of the inbox's record: the card, its placement between the
    # messages by the shared rule, the sentence saying how it ended, and
    # a press that tells the daemon it came from the chat.  A client
    # missing any of the four draws a question the others place, settle
    # or attribute differently.
    #
    ["inline decision in chat"]="clawt_gtk_inline_decision_append|clawt_web_decision_inline|inline_card_append"
    ["inline decision placement"]="clawt_decision_precedes|clawt_decision_precedes|clawt_decision_precedes"
    ["inline decision outcome"]="clawt_decision_outcome_text|clawt_decision_outcome_text|clawt_decision_outcome_text"
    ["inline press says where"]="CLAWT_DECISION_VIA_CHAT|CLAWT_DECISION_VIA_CHAT|CLAWT_DECISION_VIA_CHAT"
    ["inline view follows settling"]="clawt_decision_apply_event|sse:fleet|clawt_lrg_inline_decision_event"
    ["sidebar face"]="clawt_gtk_build_avatar|agent-face|clawt_lrg_avatar(app, agent->name"
    #
    # The fleet sidebar folds away.  GTK's split view and its toggle;
    # the web client's drawer at phone width (its desktop layout has no
    # collapse, by decision -- docs/web-client.org); the lrg toggle,
    # which is a drawer below CLAWT_LRG_NARROW_WIDTH.  GTK and lrg save
    # the choice as sidebar_collapsed; the web client has none to save.
    #
    ["sidebar collapses"]="clawt_appearance_set_sidebar_collapsed|nav-toggle|clawt_lrg_window_toggle_sidebar"
    #
    # The operator's own tasks (since 0.36.0): writing one down, for an
    # agent or for the backlog; assigning one off the backlog; and a
    # task's page -- its thread, its history and a box to comment in.
    #
    ["new task"]="on_new_task_create|/tasks/new|on_new_task_response"
    ["assign from the backlog"]="on_assign_confirm|/tasks/%s/assign|on_assign_response"
    ["task thread and comment"]="on_comment_send|/tasks/%s/comment|on_comment_response"
    ["task history"]="\"journal\", \"true\"|clawt_web_payload_set_bool(payload, \"journal\", TRUE)|\"journal\", \"true\""
    #
    # Per-agent computer settings that apply to some computer types only.
    # Each client draws the row from the agent member the daemon reports
    # for exactly those types, so the member name is the marker: a client
    # that stopped reading it would offer the setting everywhere or
    # nowhere.  The limits are marked by each key a client saves: the
    # swap key, the one that has to be ordered against memory on a save,
    # and the process and memory limits beside it.  The closing quote is
    # part of the memory marker, so `memory_swap` cannot satisfy it.
    #
    ["computer start choice"]="\"computer_start\"|\"computer_start\"|\"computer_start\""
    ["container limits"]="\"computer.container.memory_swap\"|\"computer.container.memory_swap\"|\"computer.container.memory_swap\""
    ["container process limit"]="\"computer.container.pids_limit\"|\"computer.container.pids_limit\"|\"computer.container.pids_limit\""
    ["container memory limit"]="\"computer.container.memory\"|\"computer.container.memory\"|\"computer.container.memory\""
    ["ssh wake command"]="\"ssh_wake_command\"|\"ssh_wake_command\"|\"ssh_wake_command\""
    #
    # The agent view's "Waiting for" list. The frames (wait.list,
    # wait.cancel) are layer 1's; this is the schedule line, which every
    # client must draw from the one library function rather than its own
    # wording of "next check in".
    #
    ["waiting for schedule"]="clawt_wait_schedule_label|clawt_wait_schedule_label|clawt_wait_schedule_label"
    #
    # The Fleet page: what each agent costs and which sit idle, the last
    # fourteen days, and stopping the idle ones -- one at a time and all
    # at once.  The text is libclawt's in all three.
    #
    ["fleet cost by agent"]="clawt_usage_fleet_row_subtitle|clawt_usage_fleet_row_subtitle|clawt_usage_fleet_row_subtitle"
    ["fleet days strip"]="clawt_usage_fleet_strip|clawt_usage_fleet_strip|clawt_usage_fleet_strip"
    ["stop an idle agent"]="on_fleet_stop|/fleet-usage/%s/stop|on_stop_clicked"
    ["stop the idle ones"]="on_stop_idle_confirmed|/fleet-usage/stop-idle|on_stop_idle_confirmed"
    #
    # The Spending page's ledger: the grouping is a library enumeration,
    # so layer 3 already sees the selector -- but the unpriced-turn
    # sentence and the CSV export are each one call, sent no frame of
    # their own (usage.ledger is shared) and spell out nothing.  The
    # export is a save dialog in GTK and lrg and a download in the web
    # client; each writes libclawt's document, so the marker is that.
    #
    ["spending unpriced turns"]="clawt_usage_ledger_unpriced_line|clawt_usage_ledger_unpriced_line|clawt_usage_ledger_unpriced_line"
    ["spending csv export"]="clawt_usage_ledger_csv(root)|clawt_usage_ledger_csv(root)|clawt_usage_ledger_csv(root)"
    #
    # The ledger's Group by control itself.  Layer 3 sees a client walk
    # clawt_usage_group_*, which a client could do for a column heading
    # and still offer no way to choose; the control is each client's own.
    #
    ["spending group by"]="on_spending_group_changed|spending_tabs(\"Group by\"|clawt_lrg_form_add(form, \"Group by\""
    ["acknowledged answers"]="clawt_message_ack_label|clawt_message_ack_label|clawt_message_ack_label"
    # A teammate's answer handed back to the conversation that asked
    # (answer_from), and what a turn that asked ended with (waiting_on).
    ["an answer handed back"]="clawt_message_answer_label|clawt_message_answer_label|clawt_message_answer_label"
    ["waiting on a teammate"]="clawt_message_waiting_note|clawt_message_waiting_note|clawt_message_waiting_note"
    ["merged deliveries as sections"]="clawt_merge_split|clawt_merge_split|clawt_merge_split"
    #
    # Delegation modes: a consultation's question and answer are marked
    # in the chat, a consulted task carries the badge, a task delegated
    # with `thread: new` shows its own room and opens it, and a decision
    # raised inside delegated work says who it was put to first.  The
    # wording is libclawt's in all three; the room is opened by each
    # client's own control, so that marker is the handler.
    #
    ["consultation in the chat"]="clawt_message_consult_label|clawt_message_consult_label|clawt_message_consult_label"
    ["consultation task badge"]="clawt_task_consult_label|clawt_task_consult_label|clawt_task_consult_label"
    ["a task's own room"]="clawt_task_thread_summary|clawt_task_thread_summary|clawt_task_thread_summary"
    ["open a task's own room"]="on_task_open_room|htmx_a_new_with_href(url)|on_open_room_clicked"
    ["decision routed to"]="clawt_decision_routed_label|clawt_decision_routed_label|clawt_decision_routed_label"
    ["decision routed to, inline"]="clawt_decision_routed_text|clawt_decision_routed_text|clawt_decision_routed_text"
    #
    # A turn a restart or a stop cut short, drawn as the daemon's notice
    # with libclawt's label in all three; and an agent's rebuild request,
    # drawn as a decision that says what it destroys (the `loses` member)
    # beside Approve and Decline.
    #
    ["interrupted-turn notices"]="clawt_message_interrupted_label|clawt_message_interrupted_label|clawt_message_interrupted_label"
    #
    # Steering (agents.steer): a message handed to the turn already
    # running is drawn as steered, not as one waiting for a turn, with
    # libclawt's label; and the steer level is explained in the agent's
    # settings in libclawt's words, so the three say what the daemon does.
    #
    ["steered messages"]="clawt_message_steered_label|clawt_message_steered_label|clawt_message_steered_label"
    ["explain an agent's steer level"]="clawt_steer_policy_explanation|clawt_steer_policy_explanation|clawt_steer_policy_explanation"
    #
    # Cross-talk summaries (orchestration.crosstalk_summary): the
    # daemon's note of an agent-to-agent exchange is drawn as one, with
    # libclawt's badge and a "View in Flow" action per exchange that
    # opens the conversation on the Flow page; and the per-agent setting
    # is titled and explained in libclawt's words in every client.
    #
    ["cross-talk summary badge"]="clawt_crosstalk_mark_label|clawt_crosstalk_mark_label|clawt_crosstalk_mark_label"
    ["cross-talk summary opens Flow"]="clawt_crosstalk_flow_label|clawt_crosstalk_flow_label|clawt_crosstalk_flow_label"
    ["tell me about agent-to-agent conversations"]="clawt_crosstalk_summary_explanation|clawt_crosstalk_summary_explanation|clawt_crosstalk_summary_explanation"
    # Where those summaries go (crosstalk_summary_to): back to the Matrix
    # room or group the request came from, or always the chat.
    ["where cross-talk summaries go"]="clawt_crosstalk_summary_to_explanation|clawt_crosstalk_summary_to_explanation|clawt_crosstalk_summary_to_explanation"
    #
    # A generic ACP agent (model.provider: acp): the command that is the
    # agent, named and explained in libclawt's words in all three.
    #
    ["an acp agent's command"]="clawt_model_catalog_acp_command_hint|clawt_model_catalog_acp_command_hint|clawt_model_catalog_acp_command_hint"
    ["rebuild requests say what is lost"]="\"loses\"|\"loses\"|\"loses\""
    ["rebuild requests are badged"]="clawt_gtk_badge(\"rebuild\"|clawt_web_badge(\"rebuild\"|clawt_lrg_badge(\"rebuild\""
    # ...and say it once: the question is drawn without the loss
    # paragraph each client draws on its own.
    ["rebuild question without its loss"]="clawt_decision_question_text|clawt_decision_question_text|clawt_decision_question_text"
    ["ack-check model choices"]="clawt_ack_check_model_nth_value|clawt_ack_check_model_nth_value|clawt_ack_check_model_nth_value"
    #
    # Rounds in a group room and the budget per task tree: a divider
    # where each round starts and the note under a withheld mention,
    # both worded by libclawt; the two settings on the Orchestration
    # page (the budget through orchestration.set, each room's cap
    # through room.set, neither a frame of its own); and the alert that
    # names an exhausted tree.
    #
    ["round dividers"]="clawt_round_label|clawt_round_label|clawt_round_label"
    ["withheld mentions say why"]="clawt_round_skip_note|clawt_round_skip_note|clawt_round_skip_note"
    ["tree budget setting"]="\"tree_message_budget\"|\"tree_message_budget\"|\"tree_message_budget\""
    ["thread cap per room"]="\"max_chain\"|\"max_chain\"|\"max_chain\""
    ["tree budget alert names the tree"]="clawt_loop_guard_tree_alert_text|clawt_loop_guard_tree_alert_text|clawt_loop_guard_tree_alert_text"
    #
    # The Grok page: the bots and who may reach them, handing one a job on
    # an agent's behalf, the jobs in libclawt's tones, the routine prompt
    # and the MCP key -- and the badge, counted by the daemon in all three.
    #
    ["grok bots"]="gtk_list_box_append(self->grok_bot_list, bot_row|bot_element(json_array_get_object_element|bot_row(app, json_array_get_object_element"
    ["grok send a job"]="\"grok.send\"|\"grok.send\"|\"grok.send\""
    ["grok jobs"]="clawt_grok_job_state_tone|clawt_grok_job_state_tone|clawt_grok_job_state_tone"
    ["grok routine prompt"]="\"grok.routine_prompt\"|\"grok.routine_prompt\"|\"grok.routine_prompt\""
    ["grok mcp key"]="\"grok.mcp_key\"|\"grok.mcp_key\"|\"grok.mcp_key\""
    ["grok count on a tab"]="clawt_gtk_refresh_grok_badge|clawt_web_app_grok_attention|REFRESH_GROK"
    ["browser view"]="clawt_gtk_build_browser_view|/f/a/:id/browser|build_browser_view"
    ["click in the browser"]="on_picture_pressed|/browser/click|on_browser_click"
    ["open a page in the browser"]="\"browser.open\"|/browser/open|\"browser.open\""
    ["choose the secrets an agent may have filled"]="on_grant_toggled|/browser/secret|on_browser_grant"
    ["list the vault entries by name"]="clawt_vault_entry_label|clawt_vault_entry_label|clawt_vault_entry_label"
    #
    # When and where a secret was last filled.  The line is part of
    # libclawt's session line (clawt_browser_session_line() appends
    # clawt_browser_fill_line()), so the session line is the marker: a
    # client that wrote its own would lose the fill.
    #
    ["vault last fill on the browser line"]="clawt_browser_session_line|clawt_browser_session_line|clawt_browser_session_line"
    #
    # Servers with many tools, on the Connectors page: each connector's
    # tool count and whether agents see it as a directory (libclawt's
    # sentence in all three), and the per-connector Auto / Always / Never
    # choice.  The choice walks clawt_tool_directory_mode_*, which layer 3
    # already compares; this row is the control it lives in.  The
    # thresholds are connector.settings, layer 1's.
    #
    ["connector tool count"]="clawt_mcp_directory_status_label|clawt_mcp_directory_status_label|clawt_mcp_directory_status_label"
    ["present as a directory"]="build_directory_choice|directory_form|directory_choice"
    #
    # The result budget (docs/connectors.org, "Results bigger than the
    # budget"): each connector's own field, the fleet's beside the
    # directory thresholds, and the notice a trimmed result leaves in the
    # transcript -- one library predicate, so all three recognise it the
    # same way.
    #
    ["connector result budget"]="build_budget_field|clawt_web_budget_form|budget_field"
    ["fleet result budget"]="settings_max_result_bytes|clawt_web_fleet_budget|fleet_budget_field"
    ["trimmed result notice"]="clawt_mcp_result_is_notice|clawt_mcp_result_is_notice|clawt_mcp_result_is_notice"
    #
    # MCP apps in chat (docs/connectors.org, "Apps in chat").  Recorded as
    # affordances, not an exception, by decision: the web client draws the
    # app in its sandboxed frame; GTK and lrg have no HTML engine and link
    # to the web client's view of the same app.  All three ask
    # connector.app_get, which layer 1 compares.
    #
    ["show apps in chat"]="build_apps_switch|clawt_web_apps_form|apps_toggle"
    ["MCP app in chat"]="app_link_button|clawt_web_app_frame|app_link"
    # An app calling its own server's tools (connector.app_call): the
    # web client's bridge makes the call; GTK and lrg reach it through the
    # same link to the web client's page of the app (layer 1's exception
    # map says so for the frame kind).
    ["MCP app calls its own tools"]="app_link_button|data-app-call|app_link"
    #
    # A group room drawn beside the agents, and the two things you can
    # only do to one from a client.
    #
    # None of this is visible to the other layers: a room row sends no
    # frame of its own -- it opens with the same room.history every
    # agent chat uses -- and "the composer works here" is the absence of
    # a rule rather than a call.  Layer 5 is the only one that can see
    # that a client draws rooms at all.
    #
    ["rooms in the sidebar"]="room_row|room_row|room_row"
    ["making a room"]="clawt_gtk_on_new_room|on_new_room_submit|on_room_response"
    # The wording is libclawt's (clawt_room_sidebar_subtitle()), so the
    # marker is the call: a client spelling its own line had "1 members"
    # and promised answers in a "Problems to look at" room.
    ["a room says how it delivers"]="clawt_room_sidebar_subtitle|clawt_room_sidebar_subtitle|clawt_room_sidebar_subtitle"
    #
    # Putting a room in a team's group.
    #
    # The web route for this existed and nothing on the page posted to
    # it, so a GTK operator could drag a room onto a heading and a web
    # operator could not do it at all -- and layer 1 saw `room.set` in
    # both clients, because GTK sends it for a rename too.  A dead route
    # is exactly what layer 5 is for.
    #
    ["moving a room to a team"]="move_room_to_team|team_picker_for(\"r\"|\"room.set\""
    ["picture editor"]="build_avatar_group|on_avatar_set|build_avatar_group"
    #
    # A description under each agent's name, and the setting that keeps
    # it for the pointer instead.
    #
    # The GTK sidebar has drawn this as a row subtitle since it was
    # written and the web sidebar drew nothing at all -- for as long,
    # and reported by nothing, because a subtitle sends no frame,
    # answers no command and spells out no library value.  Layer 5 is
    # the only one that can see it, which is exactly what layer 5 is
    # for.
    #
    ["agent descriptions"]="clawt-agent-row|agent-desc|clawt-agent-row"
    ["hide agent descriptions"]="build_fleet_list_group|clawt_agent_desc|build_agent_list_card"
    #
    # Which standing is drawn beside a name.  The rule itself is in the
    # library, so layer 3 would catch a client that stopped walking it
    # -- but neither client walked it for a year while `team_role` sat
    # unread in every `agent.list` reply, so the *drawing* is declared
    # too.
    #
    ["team standing badges"]="CLAWT_TEAM_BADGE_LEAD|CLAWT_TEAM_BADGE_LEAD|CLAWT_TEAM_BADGE_LEAD"
    #
    # Clicking a face to see the picture properly.
    #
    # Neither half shares a symbol with the other -- one opens a
    # GtkWindow, the other toggles an overlay from the page-head script
    # -- so layers 1 to 4 are all blind to it.
    #
    ["enlarge a profile picture"]="on_avatar_preview_clicked|avatar-zoom|avatar_preview_open"
    ["trigger secret shown once"]="secret_shown_once|secret_shown_once|secret_shown_once"
    ["trigger verification capture"]="on_trigger_capture|on_trigger_capture|on_trigger_capture"
    #
    # A trigger's delivery mode, pending cap and payload filter.  The mode
    # list is walked from the library (layer 3) and trigger.match_test is
    # a frame (layer 1), but neither sees the controls themselves: a
    # client could send `max_pending` from nowhere, or walk the modes into
    # a help text and offer no selector.  So each control is declared.
    #
    ["trigger delivery mode"]="on_mode_changed|add_mode_field|clawt_lrg_trigger_mode_hints"
    ["trigger pending cap"]="pending_row|add_pending_field|editor->pending"
    ["trigger payload filter"]="match_row_add|add_match_editor|match_line_add"
    ["filter path from a delivery"]="on_match_field_picked|htmx_datalist_new_with_id|on_field_picked"
    ["composer drafts"]="clawt_gtk_persist_draft|draft_key_for|chat_persist_draft"
    ["steer a busy agent"]="\"steered\"|\"steered\"|\"steered\""
    #
    # What an agent is doing, as opposed to whether its process is up.
    #
    # Both clients call the same library function, so the marker is that
    # call in each -- layer 3 does not see it because the rule is a
    # helper rather than a _count()/_nth() enumeration, and layer 4 does
    # not because neither client spells any of its output out.
    #
    ["agent activity"]="clawt_agent_activity_label|clawt_agent_activity_label|clawt_agent_activity_label"
    #
    # A routine that can never fire says why.
    #
    # The daemon has answered this as `problem` since routine.list was
    # written and only the web client drew it -- and the GTK row was not
    # merely silent, it drew "only when you ask", which is what it draws
    # for a `manual` routine.  Layers 1 to 4 are all blind to it: the
    # frame kind is shared, no command names it, it is not an
    # enumeration, and neither client spells the message out.
    #
    # Matched on each client's own rendering rather than on the member
    # name, because `"problem"` is also how an integration binding
    # reports its own trouble in three other files.
    #
    ["a broken routine says why"]="will never run|routine, \"problem\"|will never run"
    #
    # routines.max_start_failures: the limit in the editor with libclawt's
    # explanation, a routine turned off for it saying why, the one click
    # that turns it back on, and the alert.  The frame is routine.update
    # in every client -- which each already sends for Save -- and the
    # wording is a library call, so layers 1 to 4 see none of it.
    #
    ["routine start-failure limit"]="clawt_routine_start_failures_hint|clawt_routine_start_failures_hint|clawt_routine_start_failures_hint"
    ["a routine turned off says why"]="clawt_routine_turned_off_note|clawt_routine_turned_off_note|clawt_routine_turned_off_note"
    ["turn a routine back on"]="on_routine_turn_on|/routines/%s/enable|on_routine_turn_on"
    ["routine turned-off alert"]="clawt_routine_turned_off_alert|clawt_routine_turned_off_alert|clawt_routine_turned_off_alert"
    #
    # An update the daemon found.
    #
    # Both clients read it out of control.status and hand it to the same
    # library sentence, so the marker is the field each keeps it in.
    # Layer 1 cannot see it -- control.status is a frame both already
    # send, and this is a member inside its reply.
    #
    ["an available update"]="daemon_update|daemon_update|daemon_update"
    #
    # Holding the fleet, and saying which agents are held.
    #
    # The frame kinds are caught by layer 1 -- both clients send
    # control.pause and control.resume as literals -- but the *badge*
    # that says an agent is held is not a frame, so it is declared.
    #
    ["a held agent is drawn as held"]="clawt_hold_label|clawt_hold_label|clawt_hold_label"
    #
    # Sandboxing an agent's own tools (runtime.confine).
    #
    # The key is in the schema, so a generated form shows it whether or
    # not anybody declared it -- which is exactly why it is declared: the
    # setting is only usable with the library's label and sentence on it,
    # and with the warning that says the agent is a shadow because
    # bubblewrap is missing.  Neither is a frame, an enumeration walk or a
    # vocabulary, so layers 1 to 4 cannot see a client that dropped them.
    #
    ["sandbox an agent's own tools"]="clawt_agent_confine_label|clawt_agent_confine_label|clawt_agent_confine_label"
    ["why a sandboxed agent cannot start"]="clawt_agent_confine_notice|clawt_agent_confine_notice|clawt_agent_confine_notice"
    #
    # The egress gate (runtime.network, egress.allow).
    #
    # The network choice is a schema enum, so layers 1 to 4 see a form
    # field whether or not a client named its choices -- which is the
    # part that makes it usable: "Through clawtilla's gate" rather than
    # "gated", and the reason it is greyed while the sandbox is off.
    # The agent view's "egress: gated (N refused today)", its refusal
    # lines and its brokered-key rows are reply members, not frames.
    # The allowlist's frames are layer 1's; its label and sentence are
    # declared here so all three say the same thing about it.
    #
    ["name an agent's network choices"]="clawt_egress_network_choice_label|clawt_egress_network_choice_label|clawt_egress_network_choice_label"
    ["why the network cannot be gated"]="clawt_egress_network_notice|clawt_egress_network_notice|clawt_egress_network_notice"
    ["an agent's egress summary"]="clawt_egress_status_line|clawt_egress_status_line|clawt_egress_status_line"
    ["an agent's egress refusals"]="clawt_egress_refusal_line|clawt_egress_refusal_line|clawt_egress_refusal_line"
    ["which keys the broker holds"]="\"brokered\"|\"brokered\"|\"brokered\""
    ["the fleet's egress allowlist"]="clawt_egress_allowlist_label|clawt_egress_allowlist_label|clawt_egress_allowlist_label"
    #
    # Integration forms drawn from the shared field table.
    #
    # Not a frame kind and not an enumeration -- both clients send
    # integration.update either way -- so layers 1 to 4 are blind to it,
    # and the web client spent a long time rendering three of the eight
    # types as "this type declares no settings of its own" while GTK had
    # full editors for them.
    #
    ["integration fields"]="clawt_integration_fields|clawt_integration_fields|clawt_integration_fields"
    # A credential field is asked for as a reference and never shown
    # back -- an apprise notifier's URL is one, and so is every token.
    ["integration secret field as a reference"]="CLAWT_FIELD_SECRET|CLAWT_FIELD_SECRET|CLAWT_FIELD_SECRET"
    #
    # A skill's `/name` is dynamic, so layer 2 cannot see it.
    #
    # That layer compares `"/name"` string literals, which is exactly
    # right for the built-in commands and useless here: these come from
    # whatever skills the selected agent has been assigned, and neither
    # client contains their names at all.  Declared here instead, with
    # the limitation stated rather than left for somebody to discover
    # when the check reports OK through a client that lost the feature
    # entirely.
    #
    ["skill commands in the composer"]="clawt_gtk_skill_commands|slash-popover|skill_commands_cached"
    # The `@` completion. Both markers name the thing that offers the
    # names, not the matcher: clawt_mention_prefix_at() is in the
    # library and both clients would match it, so a client that lost
    # its completion entirely would still report OK.
    ["mention completion"]="fill_mention_list|mention-popover|mention_rows_fill"
    # Arrowing through a completion list and taking one with Return or
    # Tab. A separate affordance from the list itself, because a client
    # can perfectly well draw the names and offer no way to reach them
    # without the mouse -- which is what both did until it was added.
    ["completion keyboard navigation"]="completion_move|cmp_move|completion_move"
    # Slash commands answered in a room. Both clients run the fleet-wide
    # ones there and refuse the agent-scoped ones with the same
    # sentence; the web one used to post them into the room as messages
    # instead, so `/help` arrived as a message reading "/help".
    # The marker is a fragment both sources hold on one line: the GTK
    # sentence is split across two string literals, so a longer phrase
    # matches in the web client and nowhere in the other.
    ["commands in a room"]="about one agent|about one agent|about one agent"
    # Return sends and Shift+Return is a newline. The web composer is a
    # textarea, which does not submit its form on Return the way a
    # single-line input does, so this had to be written rather than
    # inherited -- until it was, the only way to send from that client
    # was the mouse, and Shift+Return did the same thing as Return.
    ["enter sends, shift+enter newlines"]="GDK_SHIFT_MASK|shiftKey|LRG_INPUT_MODIFIER_SHIFT"
    ["skill command expansion"]="clawt_gtk_skill_expand|skill.expand|\"skill.expand\""
    ["skills library"]="clawt_gtk_refresh_skills|clawt_web_skills_body|refresh_skills_library"
    ["skill provenance"]="Provenance|sha256|Provenance"
    ["skill scan warnings"]="Not copied|Not copied|Not copied"
    #
    # Skills agents propose (clawtilla_skill_propose).  The frames are
    # layer 1's (skill.proposals, skill.proposal_resolve); these are the
    # three things a person needs that no frame proves: the decision card
    # recognising a proposal by the library's own rule, the card showing
    # the proposed text (or the diff) with Approve / Reject / Edit, and
    # the Skills page listing what is pending, who proposed it and why.
    #
    ["skill proposal decisions"]="clawt_skill_proposal_id_is_valid|clawt_skill_proposal_id_is_valid|clawt_skill_proposal_id_is_valid"
    ["skill proposal review"]="clawt_gtk_proposal_review|web_proposal_review|lrg_proposal_review"
    ["pending skill proposals"]="append_pending_proposals|web_pending_proposals|lrg_pending_proposals"
    ["fleet recall"]="recall_entry|recall-form|recall_entry"
    ["recall results"]="refresh_recall_once|recall-hit|recall-hit"
    ["operator profile"]="operator_view|operator-profile|operator-profile"
    ["screen preview"]="screen_picture|screen-frame|screen_picture"
    ["screen takeover"]="on_screen_take|on_screen_take|on_screen_take"
    ["screen input"]="on_screen_input|on_screen_input|on_screen_input"
    ["stale frame label"]="\"stale\"|\"stale\"|\"stale\""
    ["vnc viewer"]="remote-viewer|viewer.vv|remote-viewer"
    ["teach recordings"]="teach_group|add_teach_section|teach_group"
    #
    # The caveat, which is the one part of this feature that must not be
    # in only one client.
    #
    # A recording of a demonstration is credential material until
    # somebody has read it, and the sentence saying so is carried on the
    # trace precisely so that every surface shows it. A client that drew
    # the recordings and not the caveat would be the one somebody used
    # to decide a trace was safe to share.
    #
    ["teach caveats"]="caveats|caveats|caveats"
    #
    # Per-device tokens.  The four device.* kinds are compared by layer 1;
    # these are the halves that send nothing of their own.  Pairing with
    # a code goes through the library on the *connecting* side, inside
    # ClawtClient's handshake, so no client spells `device.redeem` and
    # layer 1 is blind to whether a connection screen offers it at all.
    # The countdown and the last-seen phrasing are libclawt's, so the
    # marker is the call in each.
    #
    ["pair with a code"]="clawt_connection_new_paired|/settings/connections/pair|clawt_connection_pair(job->connection"
    # Memory review: the journal's words for an action's reversal are the
    # library's, so a row's button reads the same in all three.
    # The sentinel's Issues: the report's age and each kind's name are the
    # library's words, so a stale report reads as stale in all three.
    ["issues report age"]="clawt_sentinel_age_text|clawt_sentinel_age_text|clawt_sentinel_age_text"
    ["issue kind label"]="clawt_sentinel_kind_label|clawt_sentinel_kind_label|clawt_sentinel_kind_label"
    # First-reply rescue: the offer's sentence and each switch's label are
    # the library's, so no client claims more than the daemon can see.
    ["first-reply rescue offer"]="clawt_rescue_offer_text|clawt_rescue_offer_text|clawt_rescue_offer_text"
    ["first-reply switch label"]="clawt_rescue_switch_label|clawt_rescue_switch_label|clawt_rescue_switch_label"
    ["memory review undo"]="clawt_memory_journal_undo_label|clawt_memory_journal_undo_label|clawt_memory_journal_undo_label"
    # The periodic memory review (memories.review.model): the proposals
    # list with Accept and Reject sends memory.proposal_* (layer 1), but
    # the fleet's model choice is a walk of the ack-check catalogue that
    # another page already makes, so layer 3 cannot tell whether this
    # card offers it.  The group is the marker.
    # Saved steps under the Screen view (docs/computers.org, "Saved
    # steps"): the section itself, and the per-list switch between
    # "picture and title" and "title only", whose words are the daemon's
    # (check_line, other_check_label) so no client spells a check.
    ["saved steps"]="clawt_gtk_build_saved_steps|clawt_web_add_saved_steps|clawt_lrg_steps_build"
    ["saved steps check"]="\"steps.set_check\"|/steps/check|\"steps.set_check\""
    ["memory review proposals"]="clawt_gtk_build_memory_proposals|clawt_web_add_memory_proposals_card|build_memory_proposals"
    ["memory review model"]="\"memory.review_set\"|\"memory.review_set\"|\"memory.review_set\""
    ["pairing countdown"]="clawt_device_countdown_text|clawt_device_countdown_text|clawt_device_countdown_text"
    ["device last seen"]="clawt_device_seen_text|clawt_device_seen_text|clawt_device_seen_text"
    # Revoke is the owner's; a paired device signs itself out.  Each
    # client asks the library which button a row gets.
    ["revoke or sign out per row"]="clawt_device_row_action|clawt_device_row_action|clawt_device_row_action"
    ["a code on screen learns it was used"]="\"device.paired\"|/settings/devices/pair-status|\"device.paired\""
    # The pairing link `device.pair` returns, drawn as a QR code for a
    # phone to scan (or shown as text in a build without libqrencode).
    # It arrives in a reply every client already requests, so layer 1
    # cannot see whether a dialog draws it; the marker is each client's
    # call into the library's encoder.
    ["pairing QR code"]="clawt_gtk_qr_texture|clawt_qr_render_svg|clawt_qr_encode"
    #
    # model.fallbacks: the ordered editor, the badge on an agent running
    # on one, and the alert that says why in the daemon's own sentence.
    # A list editable in one client and a three-field text box in another
    # is the drift this table exists for; so is an agent marked as away in
    # one sidebar and not the next.
    #
    ["fallback list editor"]="fallback_editor_draw|clawt_web_fallback_card|draw_fallbacks"
    ["on fallback badge"]="clawt_gtk_failover_badge|clawt_web_failover_badge|clawt_lrg_badge(agent->fallback"
    # The same badge on the Fleet page, from usage.summary's `failover`.
    ["on fallback badge on the Fleet page"]="fleet_fallback|fleet_fallback|fleet_fallback"
    ["alert in the event's words"]="clawt_event_get_detail(event, \"message\")|clawt_event_get_detail(event, \"message\")|clawt_event_get_detail(event, \"message\")"
    #
    # Connector sign-in and several accounts: a remote server added by its
    # URL alone (the name proposed from the host), a Sign in that names
    # where it happens when discovery found a server or the server refused
    # the agent, and the account label on every connector row -- the one
    # thing that tells two accounts on one service apart.  Each is the
    # library's own function, so the marker is the rule, not its wording.
    #
    ["connector added by URL"]="clawt_connector_name_for_url|clawt_connector_name_for_url|clawt_connector_name_for_url"
    ["connector sign in at its server"]="clawt_connector_sign_in_label|clawt_connector_sign_in_label|clawt_connector_sign_in_label"
    ["account label on connector rows"]="clawt_connector_row_subtitle|clawt_connector_row_subtitle|clawt_connector_row_subtitle"
    ["account label asked for a second account"]="required for a second account|required for a second account|unique, for a second account"
    #
    # The integration broker's row on the Connectors page: its state in
    # the library's word for it, the gateway's dashboard (where SaaS
    # sign-ins happen), and start, stop and restart.  Layer 1 sees the
    # four broker.* frames; it cannot see that a client draws the state
    # or offers the dashboard, which send nothing of their own.
    #
    ["broker state on connectors"]="clawt_broker_state_label|clawt_broker_state_label|clawt_broker_state_label"
    ["broker dashboard"]="\"dashboard_url\"|\"dashboard_url\"|\"dashboard_url\""
    #
    # Each team's "Problems to look at" room has a sidebar row.  It sends
    # no frame of its own -- it is room.list read with a different
    # filter -- so layer 1 cannot see a client that lists only declared
    # rooms, and the incidents would then be readable nowhere but the
    # Tasks page.  The marker is the library predicate every listing
    # site calls.
    #
    ["incidents rooms in the sidebar"]="clawt_room_is_listed|clawt_room_is_listed|clawt_room_is_listed"
    #
    # The Tasks page's "Problems to look at" section, its Retry on each
    # open incident, and Retry on a failed task row.  task.retry and
    # incident.list are layer 1's; the section, and which rows offer the
    # button, are not -- a failed row is offered Retry by the library's
    # rule, so the button and the daemon's refusal cannot disagree.
    #
    ["problems to look at section"]="\"Problems to look at\"|\"Problems to look at\"|\"Problems to look at\""
    ["retry an incident"]="retry_button(self, \"incident\"|/incidents/%s/retry|retry_button(\"incident\""
    ["retry a failed task"]="clawt_task_state_is_retryable|clawt_task_state_is_retryable|clawt_task_state_is_retryable"
    #
    # Another agent's long request folds to one line in a peer
    # conversation, opens in place with "Show less", and "View exchange"
    # lists that peer's requests and this agent's answers.  No frame of
    # its own -- the exchange is room.history read through a library
    # filter -- so layer 1 cannot see a client that skipped it.  The
    # markers are the library rule, its label and the filter, each of
    # which the feature cannot work without.
    #
    ["fold long peer requests"]="clawt_chat_should_fold|clawt_chat_should_fold|clawt_chat_should_fold"
    ["show less on a folded request"]="clawt_chat_fold_show_less_label|clawt_chat_fold_show_less_label|clawt_chat_fold_show_less_label"
    ["view a peer exchange"]="clawt_chat_exchange(|clawt_chat_exchange(|clawt_chat_exchange("
    #
    # An open-room link -- `clawtilla-gtk --open`, `/r/<room>`,
    # `clawtilla-lrg --open` -- checked against the rooms that exist.
    #
    ["open-room links"]="clawt_open_target_resolve|clawt_open_target_resolve|clawt_lrg_open_resolve"
)

usage () {
    cat <<'USAGE'
clawt-client-parity.sh - check the graphical clients cover the same daemon

Usage:
  clawt-client-parity.sh [--list] [--help]

  --list    print each client's coverage instead of only the differences
  --help    this text

Exits non-zero when the clients (gtk, web, and lrg when clients/lrg
exists) differ at any of five layers:

  1. an IPC frame kind one sends and another never mentions
  2. a slash command the GTK composer answers and another does not
  3. a library choice enumeration one walks and another does not
     (clawt_lrg_* families are clawtilla-lrg's alone)
  4. a value from a library vocabulary that any client spells out
  5. a declared interface affordance missing from any of them

Layers 1-4 need nothing declared.  Layer 5 does, and cannot catch a
feature nobody declared -- it is there for the capabilities that are pure
interface and touch no shared symbol at all.

An exception map in this script records the deliberate differences.  An
entry there is a decision, not a to-do.

Examples:
  # As part of the build
  make parity

  # See what each client actually reaches
  tools/clawt-client-parity.sh --list
USAGE
}

#
# Every frame kind the daemon answers.
#
# Taken from the daemon rather than from a list here, so a kind added
# there is compared from the moment it exists.
#
# Comments removed first, for the same reason the client sources are read
# that way: this file is full of prose quoting the code it describes, and
# a kind named only in a paragraph is a kind the daemon does not answer.
#
daemon_kinds () {
    strip_comments "${DAEMON_SOURCES[@]}" \
        | grep -o 'kind, "[a-z_.]*"' \
        | sed 's/kind, "//; s/"//' \
        | sort -u
}

#
# The frame kinds a client's code -- comments already removed -- mentions.
#
# Intersected with the daemon's own list, because a client is full of
# dotted strings that are not frame kinds -- css classes, file names,
# member names.
#
# It takes the stripped corpus rather than a directory because it used to
# take a directory and grep the sources raw, while layers 3 to 5 all went
# through strip_comments().  That is the rule this script states twice
# and applied in three of five places, and the failure it produces is the
# one already written down here: a comment explaining that something was
# *removed* still contains its name, so the check reads the removed thing
# as present and reports OK.  Nothing was being masked when this was
# fixed -- every kind and command in a client comment was also in that
# client's code -- which is exactly how long a latent one waits.
#
client_kinds () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_kinds()` requires 1 positional argument' >&2
        echo 'client_kinds <stripped-source-file>' >&2
        exit 1
    fi

    comm -12 \
        <(grep -o '"[a-z_]\+\.[a-z_.]\+"' "${1}" \
            | tr -d '"' | sort -u) \
        <(daemon_kinds)
}

#
# The slash commands a client answers.
#
# Both keep them in a table of "/name" literals, which is enough to
# compare without either having to declare anything for this script's
# benefit.  Read from the stripped corpus, for the reason above: the GTK
# client's own comments name commands, and a comment is not an answer.
#
client_commands () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_commands()` requires 1 positional argument' >&2
        echo 'client_commands <stripped-source-file>' >&2
        exit 1
    fi

    #
    # A client with no slash commands at all is an empty set, not a
    # failure -- and under `set -o pipefail` grep's "no matches" exit
    # would otherwise take the whole script down before it printed
    # anything, which is a parity check that reports nothing and exits 1.
    # Exit 1 is "found none"; exit 2 is a real error and still counts.
    #
    { grep -ohE '"/[a-z][a-z-]*"' "${1}" || [[ $? -eq 1 ]]; } \
        | tr -d '"' \
        | sort -u
}

#
# The public headers with the comments taken out.
#
# For the same reason the client sources are read that way, one layer
# down -- and it came true here too.  clawt_page_count()'s own doc
# comment explains that there is deliberately no clawt_page_nth() beside
# it, and naming the function it does not have was enough to make this
# read the pair as present: the family was then reported as walked by
# one client and not the other, which is a real failure mode being
# announced about an API that does not exist.
#
# Built once, in main(), beside every other temporary file this script
# makes -- and *not* here, which is where it used to be.
#
# It was created lazily inside a `public_api()` that every caller invoked
# as `api="$(public_api)"`.  A command substitution is a subshell, so the
# assignment never reached the parent: the memoisation it was written for
# never once took effect, and -- worse -- the EXIT trap below saw an
# empty `api_text` and removed nothing.  Each run left two 330KB copies
# of every public header in /tmp, and 202 of them had accumulated before
# anybody counted.  The comment ten lines up about locals and the EXIT
# trap is the same hazard, spotted for the other eleven files and missed
# for this one.
#

#
# The choice enumerations the public API offers.
#
# A `_count()` with a matching `_nth()` is the library saying "here is a
# set, walk it".  Read from the headers rather than listed here, so one
# added later is compared from the moment it exists.
#
enumerations () {
    local api
    local family

    api="${api_text}"

    for family in $(grep -ohE '\bclawt_[a-z0-9_]+_count[[:space:]]*\(' \
                        "${api}" \
                    | tr -d ' (' | sed 's/_count$//' | sort -u)
    do
        #
        # Only the ones that are a *set of choices*.  clawt_memory_store_count()
        # and clawt_room_get_message_count() are counts of things that
        # happened, and have no _nth() beside them precisely because
        # there is nothing to offer.
        #
        if grep -qhE "\b${family}_nth[[:space:]]*\(" "${api}"
        then
            echo "${family}"
        fi
    done
}

#
# Whether a client walks a given enumeration.
#
# Takes the client's comment-stripped code rather than its directory,
# for the third time in this file and the same reason each time: prose
# that *names* a function is not a call to it.  The topbar's comment
# says it is "walked out of clawt_section_count()", and with the raw
# sources that sentence alone was enough to report the enumeration as
# walked -- deleting the loop under it changed nothing.
#
# The two other layers already read stripped code; this one did not, so
# the rule was about those call sites rather than about the check.
#
walks_enumeration () {
    if [[ $# -ne 2 ]]
    then
        # shellcheck disable=SC2016
        echo '`walks_enumeration()` requires 2 positional arguments' >&2
        echo 'walks_enumeration <code-file> <family>' >&2
        exit 1
    fi

    grep -qE "\b${2}_(count|nth)[[:space:]]*\(" "${1}"
}

#
# A client's sources with the comments taken out.
#
# Layers 4 and 5 look for a literal, and this file is full of prose that
# quotes the very things they look for -- the comment explaining why the
# stylesheet spells `data-theme="light"` contains "light", and the one
# explaining the fallback contains "system".  Grepping the prose reported
# two hardcodings that were not there and would have taught somebody to
# ignore the check.
#
# Written to a file and grepped, never piped into `grep -q`.  Under
# `set -o pipefail` that pipeline reports *failure on success*: grep
# exits the moment it matches, awk dies of SIGPIPE with 141, and the
# pipeline takes the worst status -- so every marker that was present
# reported missing.  It is also read once per client rather than once
# per value.
#
# Only `/*  */`, because that is the only comment form the codebase uses.
# A `/*` inside a string literal would end the visible text early; there
# are none, and the failure mode is a missed hit rather than a false one.
#
strip_comments () {
    awk '
    {
        line = $0
        out = ""

        while (length(line) > 0) {
            if (incomment) {
                p = index(line, "*/")
                if (p == 0) { line = "" }
                else { incomment = 0; line = substr(line, p + 2) }
            } else {
                p = index(line, "/*")
                if (p == 0) { out = out line; line = "" }
                else {
                    out = out substr(line, 1, p - 1)
                    incomment = 1
                    line = substr(line, p + 2)
                }
            }
        }

        print out
    }' "$@"
}

#
# Every line of a client's own code, comments removed.
#
client_code () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_code()` requires 1 positional argument' >&2
        exit 1
    fi

    strip_comments "${1}"/*.c
}

#
# The same, minus the stylesheet.
#
# A CSS rule is not a capability.  A class the sheet styles and nothing
# renders is dead CSS -- and it satisfied the affordance check on its own,
# because the marker for a drawn thing is usually the class name and the
# class name is in both files.  Eight of the declared markers were in
# web-style.c as well as in a renderer, so deleting the renderer half of
# any of them reported OK: the exact shape of gap this check exists to
# catch, in the check.
#
# Layers 1 to 4 keep the whole corpus.  A stylesheet can genuinely reach a
# frame kind (it cannot) and can genuinely hardcode a library vocabulary
# value (it does, in each client's own dialect, on purpose) -- so the
# narrowing is for layer 5 alone, which is the layer about things being
# *drawn*.
#
client_render_code () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`client_render_code()` requires 1 positional argument' >&2
        exit 1
    fi

    local files=()
    local file

    for file in "${1}"/*.c
    do
        #
        # clawtilla-lrg's stylesheet is its theme document: the style
        # classes it paints are no more a capability than a CSS rule is.
        #
        [[ "${file}" == *-style.c ]] && continue
        [[ "${file}" == */lrg-theme-model.c ]] && continue
        files+=("${file}")
    done

    strip_comments "${files[@]}"
}

#
# Every value a library vocabulary holds.
#
vocabulary_values () {
    if [[ $# -ne 1 ]]
    then
        # shellcheck disable=SC2016
        echo '`vocabulary_values()` requires 1 positional argument' >&2
        exit 1
    fi

    local spec="${1}"
    local file="${spec%%|*}"
    local script="${spec#*|}"

    sed -n "${script}" "${ROOT}/${file}" | sort -u
}

report_missing () {
    if [[ $# -ne 4 ]]
    then
        # shellcheck disable=SC2016
        echo '`report_missing()` requires 4 positional arguments' >&2
        echo 'report_missing <label> <other> <missing-file> <exception-map>' >&2
        exit 1
    fi

    local label="${1}"
    local other="${2}"
    local missing_file="${3}"
    local -n exceptions="${4}"
    local failures=0
    local kind

    while read -r kind
    do
        [[ -z "${kind}" ]] && continue

        if [[ -v exceptions["${kind}"] ]]
        then
            printf '  %-28s allowed: %s\n' \
                "${kind}" "${exceptions[${kind}]}"
            continue
        fi

        printf '  %-28s in %s, missing from %s\n' \
            "${kind}" "${other}" "${label}"
        failures=$((failures + 1))
    done < "${missing_file}"

    return "${failures}"
}

#
# Choice enumerations the web client is allowed not to walk, each with
# the reason.  Same rule as the maps above: a decision, not a to-do.
# GTK and clawtilla-lrg are still compared with each other for these.
#
# shellcheck disable=SC2034
declare -A WEB_MAY_NOT_WALK=(
    ["clawt_window_buttons"]="minimize, maximize and close belong to a desktop window; the web client's window is the browser's, and it draws none to hide"
)

#
# Kinds clawtilla-lrg is allowed to lack, each with the reason.  Same rule
# as the two maps above: a decision, not a to-do.
#
# shellcheck disable=SC2034
declare -A LRG_MAY_LACK=(
    ["agent.file_read"]="same capability as GTK's, same mechanism: the file opens in \$EDITOR, which is the better answer when the client and the daemon share a machine"
    ["agent.file_write"]="the other half of the same; \$EDITOR writes the file directly"
    ["browser.list"]="the Browser view's line comes from browser.frame's reply here, as in GTK; the web client reads browser.list because its picture is a separate request"
    ["connector.app_call"]="as in GTK: an app's own call comes from its view, which only the web client draws; lrg links to the web client's page of the app, where the call is made"
)

#
# The clients, in the order they are reported, and where each lives.
#
# A table rather than a pair because there are three now, and the old
# script's shape -- `gtk` and `web` variables, `comm` between exactly two
# sets, a marker split on one `|` -- had the number two built into every
# line.  clawtilla-lrg is optional and its directory may not exist in a
# tree that never built it, so it is compared only when it is there.
#
CLIENT_NAMES=(gtk web)
declare -A CLIENT_DIRS=(
    [gtk]="${ROOT}/clients/gtk"
    [web]="${ROOT}/clients/web"
    [lrg]="${ROOT}/clients/lrg"
)

if [[ -d "${CLIENT_DIRS[lrg]}" ]]
then
    CLIENT_NAMES+=(lrg)
fi

# Per-client temporary files, filled in main().
#
# new_tmp() stores the name in the variable it is given rather than
# printing it.  `x="$(new_tmp)"` would run it in a subshell, recording the
# file in a TMPFILES the EXIT trap never sees -- the leak this script's
# api_text once had, 202 copies of the public headers deep.
declare -A KINDS_FILE=()
declare -A CODE_FILE=()
declare -A RENDER_FILE=()
declare -A CMDS_FILE=()
TMPFILES=()

cleanup () {
    if [[ ${#TMPFILES[@]} -gt 0 ]]
    then
        rm -f "${TMPFILES[@]}"
    fi
}

trap cleanup EXIT

new_tmp () {
    local file

    file="$(mktemp)"
    TMPFILES+=("${file}")
    printf -v "${1}" '%s' "${file}"
}

# The index of a client in CLIENT_NAMES, which is its column in a marker.
client_column () {
    local i

    for i in "${!CLIENT_NAMES[@]}"
    do
        [[ "${CLIENT_NAMES[${i}]}" == "${1}" ]] && { echo "${i}"; return; }
    done

    echo -1
}

main () {
    local list=0
    local client
    local tmp

    while [[ $# -gt 0 ]]
    do
        case "${1}" in
            --list) list=1 ;;
            -h|--help) usage; exit 0 ;;
            *) echo "clawt-client-parity: unknown option ${1}" >&2
               usage >&2
               exit 2 ;;
        esac
        shift
    done

    #
    # Not locals: the EXIT trap fires after main() has returned, and a
    # local is gone by then -- which under `set -u` turns a clean run into
    # "unbound variable" printed after the result.
    #
    new_tmp tmp; api_text="${tmp}"

    for client in "${CLIENT_NAMES[@]}"
    do
        new_tmp tmp; CODE_FILE[${client}]="${tmp}"
        new_tmp tmp; RENDER_FILE[${client}]="${tmp}"
        new_tmp tmp; KINDS_FILE[${client}]="${tmp}"
        new_tmp tmp; CMDS_FILE[${client}]="${tmp}"

        client_code "${CLIENT_DIRS[${client}]}" > "${CODE_FILE[${client}]}"
        client_render_code "${CLIENT_DIRS[${client}]}" \
            > "${RENDER_FILE[${client}]}"
    done

    #
    # The public API's own text, for the enumeration layer.  Here rather
    # than where it is read, so that the EXIT trap can reach it -- see
    # the note above enumerations().
    #
    strip_comments < <(cat "${ROOT}"/src/*.h "${ROOT}"/src/*/*.h) \
        > "${api_text}"

    #
    # And it has to have found something.
    #
    # Layer 3 walks whatever is in here, so an empty corpus makes it
    # compare nothing and report OK -- which is indistinguishable from
    # every enumeration agreeing.  A moved header, a changed glob or a
    # strip_comments that stopped matching would all land here, and each
    # would quietly retire a whole layer of the check.
    #
    if [[ ! -s "${api_text}" ]]
    then
        echo "client parity: the public API corpus came out empty." >&2
        echo "  Nothing was read from ${ROOT}/src/*.h or ${ROOT}/src/*/*.h," >&2
        echo "  so the enumeration layer would have compared nothing and" >&2
        echo "  said OK.  Refusing rather than reporting a pass." >&2
        exit 1
    fi

    local union
    new_tmp tmp; union="${tmp}"

    for client in "${CLIENT_NAMES[@]}"
    do
        client_kinds "${CODE_FILE[${client}]}" > "${KINDS_FILE[${client}]}"
        client_commands "${CODE_FILE[${client}]}" > "${CMDS_FILE[${client}]}"
    done

    for client in "${CLIENT_NAMES[@]}"
    do
        cat "${KINDS_FILE[${client}]}"
    done | sort -u > "${union}"

    if [[ "${list}" -eq 1 ]]
    then
        for client in "${CLIENT_NAMES[@]}"
        do
            echo "clawtilla-${client} reaches $(wc -l < "${KINDS_FILE[${client}]}") frame kinds:"
            sed 's/^/  /' "${KINDS_FILE[${client}]}"
            echo
        done

        printf 'slash commands:'
        for client in "${CLIENT_NAMES[@]}"
        do
            printf ' %s in %s' "$(wc -l < "${CMDS_FILE[${client}]}")" "${client}"
        done
        echo
        echo

        echo "choice enumerations, and who walks them:"
        local shown
        for shown in $(enumerations)
        do
            local who=""
            for client in "${CLIENT_NAMES[@]}"
            do
                walks_enumeration "${CODE_FILE[${client}]}" "${shown}" \
                    && who="${who:+${who}+}${client}"
            done
            printf '  %-38s %s\n' "${shown}()" "${who:-nobody}"
        done
        echo

        echo "library vocabularies no client may copy:"
        local vocab
        for vocab in "${!VOCABULARIES[@]}"
        do
            printf '  %-24s %s\n' "${vocab}" \
                "$(vocabulary_values "${VOCABULARIES[${vocab}]}" | tr '\n' ' ')"
        done
        echo

        echo "declared affordances ($(IFS='|'; echo "${CLIENT_NAMES[*]}") markers):"
        local aff
        for aff in "${!AFFORDANCES[@]}"
        do
            printf '  %-24s %s\n' "${aff}" "${AFFORDANCES[${aff}]}"
        done
        echo
    fi

    local failures=0

    #
    # Layer 1: every kind any client sends, every client sends -- or has
    # a reason not to in its exception map.  Compared against the union
    # rather than pairwise, because with three clients a pairwise check
    # needs three comparisons to say what this says in one.
    #
    local missing
    local one_kind
    local other
    local holders

    new_tmp one_kind

    for client in "${CLIENT_NAMES[@]}"
    do
        new_tmp tmp; missing="${tmp}"
        comm -23 "${union}" "${KINDS_FILE[${client}]}" > "${missing}"

        [[ -s "${missing}" ]] || continue

        while read -r kind
        do
            [[ -z "${kind}" ]] && continue
            holders=""

            for other in "${CLIENT_NAMES[@]}"
            do
                grep -qx -- "${kind}" "${KINDS_FILE[${other}]}" \
                    && holders="${holders:+${holders}, }clawtilla-${other}"
            done

            echo "${kind}" > "${one_kind}"
            report_missing "clawtilla-${client}" "${holders}" "${one_kind}" \
                "${client^^}_MAY_LACK" || failures=$((failures + $?))
        done < "${missing}"
    done

    #
    # Layer 2: commands, checked from the GTK composer outwards.  GTK is
    # the one with the established set; another client answering
    # something extra is a thing to notice by reading, not a failure.
    #
    for client in "${CLIENT_NAMES[@]}"
    do
        [[ "${client}" == "gtk" ]] && continue

        while read -r command
        do
            [[ -z "${command}" ]] && continue

            printf '  %-28s a command in clawtilla-gtk, missing from clawtilla-%s\n' \
                "${command}" "${client}"
            failures=$((failures + 1))
        done < <(comm -23 "${CMDS_FILE[gtk]}" "${CMDS_FILE[${client}]}")
    done

    #
    # Layer 3: a set of choices the library offers must be offered by
    # every client, or by none.
    #
    # Except the `clawt_lrg_*` families, which are clawtilla-lrg's own
    # settings (motion, material, background) by decision: GTK follows
    # the desktop's animation setting and the web client the browser's,
    # and neither has springs or glass to tune.  Those must be walked by
    # clawtilla-lrg when it is present, and are nobody else's to walk.
    #
    local family
    local walked
    local not_walked

    for family in $(enumerations)
    do
        if [[ "${family}" == clawt_lrg_* ]]
        then
            if [[ -n "${CODE_FILE[lrg]:-}" ]] && \
               ! walks_enumeration "${CODE_FILE[lrg]}" "${family}"
            then
                printf '  %-28s clawtilla-lrg'"'"'s own setting, not walked by clawtilla-lrg\n' \
                    "${family}()"
                failures=$((failures + 1))
            fi
            continue
        fi

        walked=""
        not_walked=""

        for client in "${CLIENT_NAMES[@]}"
        do
            if [[ "${client}" == web && -n "${WEB_MAY_NOT_WALK[${family}]:-}" ]]
            then
                continue
            fi

            if walks_enumeration "${CODE_FILE[${client}]}" "${family}"
            then
                walked="${walked:+${walked}, }clawtilla-${client}"
            else
                not_walked="${not_walked:+${not_walked}, }clawtilla-${client}"
            fi
        done

        [[ -z "${walked}" || -z "${not_walked}" ]] && continue

        printf '  %-28s walked by %s, not by %s\n' \
            "${family}()" "${walked}" "${not_walked}"
        failures=$((failures + 1))
    done

    #
    # Layer 4: and no client may keep its own copy of the values.
    #
    local vocabulary
    local value

    for vocabulary in "${!VOCABULARIES[@]}"
    do
        for value in $(vocabulary_values "${VOCABULARIES[${vocabulary}]}")
        do
            for client in "${CLIENT_NAMES[@]}"
            do
                grep -qF "\"${value}\"" "${CODE_FILE[${client}]}" || continue

                printf '  %-28s clawtilla-%s names it; walk the %s list\n' \
                    "${value}" "${client}" "${vocabulary}"
                failures=$((failures + 1))
            done
        done
    done

    #
    # Layer 5: the declared interface affordances, every client's marker
    # present.  A marker is the field in the client's column; an empty
    # or absent field is a declaration that does not cover that client,
    # which is a failure rather than a pass -- otherwise adding a client
    # would satisfy every affordance by having no markers at all.
    #
    local affordance
    local column
    local -a markers

    for affordance in "${!AFFORDANCES[@]}"
    do
        IFS='|' read -r -a markers <<< "${AFFORDANCES[${affordance}]}"

        for client in "${CLIENT_NAMES[@]}"
        do
            column="$(client_column "${client}")"

            if [[ -z "${markers[${column}]:-}" ]]
            then
                printf '  %-28s declared without a clawtilla-%s marker\n' \
                    "${affordance}" "${client}"
                failures=$((failures + 1))
                continue
            fi

            grep -qF -- "${markers[${column}]}" "${RENDER_FILE[${client}]}" || {
                printf '  %-28s declared, missing from clawtilla-%s\n' \
                    "${affordance}" "${client}"
                failures=$((failures + 1))
            }
        done
    done

    if [[ "${failures}" -gt 0 ]]
    then
        echo
        echo "${failures} capability(ies) exist in some clients and not others."
        echo "Build it in every client, or record the reason in the exception"
        echo "map in tools/clawt-client-parity.sh"
        exit 1
    fi

    #
    # Read once here rather than inside the summary, so that a layer
    # which found nothing to compare can refuse instead of being printed
    # as a cheerful zero.
    #
    local n_enums
    n_enums="$(enumerations | wc -l)"

    if [[ "${n_enums}" -eq 0 ]]
    then
        echo "client parity: no enumerations were found to compare." >&2
        echo "  Every _count()/_nth() pair in the public API is one, and" >&2
        echo "  this tree has a dozen -- so zero means the layer stopped" >&2
        echo "  looking rather than that the clients agree." >&2
        exit 1
    fi

    local summary=""

    for client in "${CLIENT_NAMES[@]}"
    do
        summary="${summary:+${summary}, }$(wc -l < "${KINDS_FILE[${client}]}") kinds and $(wc -l < "${CMDS_FILE[${client}]}") commands in ${client}"
    done

    echo "client parity: OK (${summary}; ${n_enums} enumeration(s), \
${#VOCABULARIES[@]} vocabulary(ies), ${#AFFORDANCES[@]} affordance(s))"
}

main "$@"
