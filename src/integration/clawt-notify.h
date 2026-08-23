/*
 * clawt-notify.h - How the fleet reaches the person running it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent that is blocked on you and cannot tell you is an agent that
 * has stopped, quietly, until you next open a client.  clawtilla_message_user
 * puts the message where you will see it *if you look*; this is what
 * makes you look.
 *
 * The rule is deliberately small.  Blocked on you is worth a buzz.
 * Broken is worth a buzz.  Everything a fleet does while it works is
 * not -- a notifier that fires on every turn is one people turn off,
 * and then it is not there for the two that mattered.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-types.h"
#include "integration/clawt-integration.h"

G_BEGIN_DECLS

/**
 * ClawtNotification:
 * @events: which single event this is
 * @agent_id: the agent it is about
 * @agent_name: (nullable): its display name
 * @title: the one line a lock screen shows
 * @body: the rest, which some backends drop
 * @room_id: (nullable): where the conversation is, for a client that can jump to it
 *
 * One thing worth telling somebody.
 */
typedef struct {
    ClawtNotifyEvents  events;
    gchar             *agent_id;
    gchar             *agent_name;
    gchar             *title;
    gchar             *body;
    gchar             *room_id;
} ClawtNotification;

#define CLAWT_TYPE_NOTIFICATION (clawt_notification_get_type())

GType clawt_notification_get_type(void) G_GNUC_CONST;

/**
 * clawt_notification_new:
 * @events: which event
 * @agent_id: the agent it is about
 * @agent_name: (nullable): its display name
 * @title: the one line
 * @body: (nullable): the rest
 *
 * Returns: (transfer full): a new #ClawtNotification
 */
ClawtNotification *clawt_notification_new(ClawtNotifyEvents  events,
                                          const gchar       *agent_id,
                                          const gchar       *agent_name,
                                          const gchar       *title,
                                          const gchar       *body);

ClawtNotification *clawt_notification_copy(ClawtNotification *self);
void               clawt_notification_free(ClawtNotification *self);

/**
 * clawt_notify_summarize:
 * @text: whatever an agent wrote
 * @max_chars: how much room there is
 *
 * One line, short enough for a lock screen, with the newlines and code
 * fences of a model's answer flattened out of it.
 *
 * Returns: (transfer full): the summary
 */
gchar *clawt_notify_summarize(const gchar *text, gsize max_chars);

/**
 * clawt_notify_events_from_strv:
 * @names: (nullable) (array zero-terminated=1): nicknames
 * @error: (out) (optional): return location for a #GError
 *
 * Turns `["question", "error"]` into a flags value.
 *
 * Returns: the events, or %CLAWT_NOTIFY_EVENTS_NONE with @error set
 */
ClawtNotifyEvents clawt_notify_events_from_strv(const gchar *const  *names,
                                                GError             **error);

/**
 * clawt_notify_parse_quiet_hours:
 * @text: a range such as `23:00-07:00`
 * @out_start: (out): minutes past midnight the silence starts
 * @out_end: (out): minutes past midnight it ends
 *
 * Returns: %TRUE if @text is a range
 */
gboolean clawt_notify_parse_quiet_hours(const gchar *text,
                                        gint        *out_start,
                                        gint        *out_end);

/**
 * clawt_notify_in_quiet_hours:
 * @start: minutes past midnight the silence starts
 * @end: minutes past midnight it ends
 * @minute_of_day: the time to test
 *
 * Whether @minute_of_day falls inside the range.
 *
 * A range that wraps midnight -- which is most of them, since people
 * sleep across it -- is inside when the time is after the start *or*
 * before the end.  Getting this backwards produces a notifier that is
 * silent all day and loud all night, which is the same bug either way
 * round and only noticed at 3am.
 *
 * Returns: %TRUE if this is a time to stay silent
 */
gboolean clawt_notify_in_quiet_hours(gint start, gint end, gint minute_of_day);

/**
 * clawt_notify_priority_for_ntfy:
 * @priority: low, normal, high or urgent
 *
 * Returns: (transfer none): ntfy's name for it
 */
const gchar *clawt_notify_priority_for_ntfy(const gchar *priority);

/**
 * clawt_notify_priority_for_gotify:
 * @priority: low, normal, high or urgent
 *
 * Returns: gotify's number for it, 0 to 10
 */
gint clawt_notify_priority_for_gotify(const gchar *priority);

/**
 * clawt_notify_priority_for_desktop:
 * @priority: low, normal, high or urgent
 *
 * Returns: the freedesktop urgency hint, 0 to 2
 */
guchar clawt_notify_priority_for_desktop(const gchar *priority);

/**
 * clawt_notify_expand_argv:
 * @command: the program to run
 * @args: (nullable) (array zero-terminated=1): its arguments
 * @notification: what to say
 *
 * Builds the argv for the `command` backend.
 *
 * `{{title}}`, `{{body}}` and `{{agent}}` are substituted wherever they
 * appear.  If none of them appears anywhere, the title and body are
 * appended as two further arguments -- so a program that simply takes
 * text works with no placeholders at all, which is most of them.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv clawt_notify_expand_argv(const gchar        *command,
                               const gchar *const *args,
                               ClawtNotification  *notification);

/**
 * clawt_notify_send_async:
 * @binding: a notify integration, as one agent has it
 * @notification: what to say
 * @token: (nullable): the credential, already resolved
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it has been handed over
 * @user_data: data for @callback
 *
 * Sends one notification.
 *
 * The token is passed in rather than resolved here, because resolving a
 * `command` reference can block for as long as its timeout and this runs
 * on the daemon's main context.  #ClawtNotifier resolves it once when
 * the configuration loads.
 */
void clawt_notify_send_async(ClawtIntegrationBinding *binding,
                             ClawtNotification       *notification,
                             const gchar             *token,
                             GCancellable            *cancellable,
                             GAsyncReadyCallback      callback,
                             gpointer                 user_data);

/**
 * clawt_notify_send_finish:
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if it was accepted
 */
gboolean clawt_notify_send_finish(GAsyncResult *result, GError **error);

/**
 * ClawtNotifier:
 *
 * The thing that decides whether an event is worth a notification, and
 * sends it to everything in scope for that agent.
 *
 * Owned by the daemon and rebuilt on every configuration reload, which
 * is also when it resolves the credentials it will need.
 */
#define CLAWT_TYPE_NOTIFIER (clawt_notifier_get_type())

G_DECLARE_FINAL_TYPE(ClawtNotifier, clawt_notifier, CLAWT, NOTIFIER, GObject)

/**
 * clawt_notifier_new:
 * @config: the fleet configuration
 *
 * Returns: (transfer full): a new #ClawtNotifier
 */
ClawtNotifier *clawt_notifier_new(ClawtConfig *config);

/**
 * clawt_notifier_reload:
 * @self: a #ClawtNotifier
 * @config: the fleet configuration
 *
 * Rereads the notify integrations and resolves their credentials.
 *
 * A credential that cannot be resolved disables *that* notifier with a
 * warning rather than failing the reload: being unable to buzz a phone
 * is not a reason to stop a fleet.
 */
void clawt_notifier_reload(ClawtNotifier *self, ClawtConfig *config);

/**
 * clawt_notifier_notify:
 * @self: a #ClawtNotifier
 * @notification: what happened
 *
 * Tells whoever should be told.
 *
 * Silently does nothing when no notify integration covers that agent,
 * when none of them wants that event, or when they are all in their
 * quiet hours -- all three of which are the ordinary case.
 */
void clawt_notifier_notify(ClawtNotifier     *self,
                           ClawtNotification *notification);

/**
 * clawt_notifier_test_async:
 * @self: a #ClawtNotifier
 * @name: an integration name
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it has been handed over
 * @user_data: data for @callback
 *
 * Sends one notification through @name, ignoring its event list and its
 * quiet hours.
 *
 * A notifier is the one thing in a fleet you cannot tell is working by
 * looking at it: it is correct precisely when nothing happens.  This is
 * the button that makes something happen.
 */
void clawt_notifier_test_async(ClawtNotifier       *self,
                               const gchar         *name,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

/**
 * clawt_notifier_test_finish:
 * @self: a #ClawtNotifier
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if it was accepted
 */
gboolean clawt_notifier_test_finish(ClawtNotifier  *self,
                                    GAsyncResult   *result,
                                    GError        **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtNotification, clawt_notification_free)

G_END_DECLS
