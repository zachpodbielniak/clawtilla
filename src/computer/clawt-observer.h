/*
 * clawt-observer.h - Grabbing frames, but only while somebody is looking
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The rule this type exists to hold is that **a frame costs the agent**.
 * Grabbing shares the one channel the agent is using for its own work --
 * an SSH connection into its guest, or a compositor's socket -- so every
 * picture is latency taken from the task somebody is watching.  A
 * preview that ran all the time would make every agent with a desktop
 * permanently slower, in exchange for pictures nobody was looking at.
 *
 * So: nothing is captured unless a client has subscribed, the last
 * unsubscribe stops it, two clients watching the same agent share one
 * grab, and the interval timer and an explicit refresh go through one
 * capture path with a minimum gap between them.  There is one capture in
 * flight at a time; a second request while one is running is dropped
 * rather than queued, because a queue of screenshots delivers a picture
 * of a moment that has passed.
 *
 * Frames are written under the state directory and named in an event,
 * exactly as an attachment is.  A client may be on another machine, so
 * it fetches the bytes rather than opening the path -- the path is how
 * the daemon finds them again, not how a client does.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "computer/clawt-computer.h"
#include "computer/clawt-observable.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_OBSERVER (clawt_observer_get_type())

G_DECLARE_FINAL_TYPE(ClawtObserver, clawt_observer, CLAWT, OBSERVER, GObject)

/**
 * clawt_observer_new:
 * @frame_dir: where to write frames, under the daemon's state directory
 * @context: (nullable): the main context the timer and the answers
 *   belong to, or %NULL for the thread-default at the time of the call
 *
 * Returns: (transfer full): a new #ClawtObserver watching nothing
 */
ClawtObserver *clawt_observer_new(const gchar  *frame_dir,
                                  GMainContext *context);

/**
 * clawt_observer_subscribe:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 * @computer: (transfer none): the agent's computer
 * @fps: frames a second, before clamping
 * @error: (out) (optional): return location for a #GError
 *
 * Starts watching, or joins somebody already watching.
 *
 * The first subscriber starts the timer; a second one shares it, so two
 * clients on one agent produce one grab per interval rather than two.
 * That is not an optimisation -- two grabs would be two round trips
 * through the agent's own connection, and whether the agent slows down
 * would depend on how many people had the tab open.
 *
 * @watcher names *who* is watching, and calling this twice with the same
 * name pushes that watcher's lease forward rather than counting it
 * twice. Both halves matter: a window subscribes once and unsubscribes,
 * while a browser tab can only keep asking -- and a closed tab says
 * nothing at all, so a count nobody decremented would leave the screen
 * grabbed for ever. See %CLAWT_OBSERVE_LEASE_SECONDS.
 *
 * A computer with no screen is refused here, naming its type, rather
 * than accepted and then never producing anything.
 *
 * Returns: %TRUE if the screen is now being watched
 */
gboolean clawt_observer_subscribe(ClawtObserver  *self,
                                  const gchar    *agent_id,
                                  ClawtComputer  *computer,
                                  const gchar    *watcher,
                                  gint64          fps,
                                  GError        **error);

/**
 * clawt_observer_unsubscribe:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * One watcher fewer.  The last one stops the timer and tells the
 * backend.
 *
 * @watcher must be the name that subscribed. A client letting go of
 * somebody else's watch would stop a screen a second person was still
 * looking at.
 *
 * Returns: the number of watchers left
 */
guint clawt_observer_unsubscribe(ClawtObserver *self,
                                 const gchar   *agent_id,
                                 const gchar   *watcher);

/**
 * clawt_observer_subscribers:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * Returns: how many clients are watching
 */
guint clawt_observer_subscribers(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_refresh:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * Asks for a frame now, rather than at the next tick.
 *
 * Subject to the same minimum gap and the same one-at-a-time rule as the
 * timer, and through the same code: a refresh button with a path of its
 * own would be a way to make an agent's connection unusable by holding
 * a key down.
 *
 * Returns: %TRUE if a capture was started
 */
gboolean clawt_observer_refresh(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_note_touched:
 * @self: a #ClawtObserver
 * @agent_id: which agent
 *
 * Records that the agent has just done something to its screen.
 *
 * Called from the one place that already sees every desktop action the
 * agent takes -- the check in front of the MCP relay -- rather than from
 * a hook of its own, because a second place to notice would be a second
 * place to forget.
 */
void clawt_observer_note_touched(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_settle_turn:
 * @self: a #ClawtObserver
 * @agent_id: which agent
 *
 * The agent's turn has ended; take a last frame if the turn touched the
 * screen.
 *
 * The condition is the point.  A turn that never went near the desktop
 * has nothing new to show, and grabbing anyway would end every one-word
 * reply with a fresh picture of an idle desktop -- a cost paid on the
 * agent's own connection for an image identical to the one already
 * there.
 */
void clawt_observer_settle_turn(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_get_frame_path:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * Returns: (transfer none) (nullable): where the last frame was written
 */
const gchar *clawt_observer_get_frame_path(ClawtObserver *self,
                                           const gchar   *agent_id);

/**
 * clawt_observer_get_frame_stamp:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * When the last frame was taken.
 *
 * Answered so that a client can say how old the picture is.  A stale
 * frame labelled with its age is honest; the same frame presented as
 * current is somebody watching a machine that stopped answering ten
 * minutes ago and not knowing it.
 *
 * Returns: microseconds since the epoch, or 0 if there is no frame
 */
gint64 clawt_observer_get_frame_stamp(ClawtObserver *self,
                                      const gchar   *agent_id);

/**
 * clawt_observer_get_last_error:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * Returns: (transfer none) (nullable): why the last grab failed, in the
 *   backend's own words
 */
const gchar *clawt_observer_get_last_error(ClawtObserver *self,
                                           const gchar   *agent_id);

/**
 * clawt_observer_get_sizes:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 * @frame_width: (out) (optional): the last frame's width in pixels
 * @frame_height: (out) (optional): its height
 * @screen_width: (out) (optional): the screen's own width, or 0 when it
 *   is not known
 * @screen_height: (out) (optional): its height, or 0
 *
 * The two sizes a click has to be scaled between.
 *
 * Reported rather than assumed equal. The frame is downscaled where the
 * pixels are, so on a HiDPI guest the picture is a third of the screen's
 * width -- and a client that clicked at the coordinate it read off the
 * picture would miss by that factor, which looks like the pointer being
 * unreliable rather than like arithmetic. Zero for the screen means
 * unknown, which a client shows plainly instead of guessing.
 */
void clawt_observer_get_sizes(ClawtObserver *self,
                              const gchar   *agent_id,
                              guint         *frame_width,
                              guint         *frame_height,
                              guint         *screen_width,
                              guint         *screen_height);

/**
 * clawt_observer_get_viewer:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * The address a real remote-desktop viewer can open, as of the last
 * grab.
 *
 * Cached rather than asked, because asking is not free: for a VM it is
 * read out of the running domain's XML, and `computer.vm.uri` can name
 * a libvirt on another machine over qemu+ssh. A status handler that
 * asked directly would put that round trip on the daemon's main context
 * once per poll, for every client with the Screen tab open -- which is
 * the rule this tree has already had to apply at four other call sites.
 *
 * %NULL until the first grab has finished, and %NULL again whenever the
 * machine stops: an address for a VM that is off sends whoever clicks it
 * to debug their viewer.
 *
 * Returns: (transfer none) (nullable): the URI
 */
const gchar *clawt_observer_get_viewer(ClawtObserver *self,
                                       const gchar   *agent_id);

/**
 * clawt_observer_get_fps:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 *
 * Returns: the clamped rate this screen is being grabbed at, or 0
 */
guint clawt_observer_get_fps(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_send_input_async:
 * @self: a #ClawtObserver
 * @agent_id: whose screen
 * @computer: (transfer none): the agent's computer
 * @event: (transfer none): what to do
 * @callback: (scope async): called when it has been sent
 * @user_data: data for @callback
 *
 * Sends one event, on a worker thread.
 *
 * Here rather than in the IPC handler because this is the same wait as a
 * frame grab -- an SSH round trip into a guest -- and the rule this tree
 * keeps having to re-learn is that the wait belongs at the function, not
 * at whichever call site somebody noticed it from. A keystroke held on
 * the daemon's context is every agent's messages stopped for the length
 * of a network round trip, once per key.
 *
 * The answer arrives on the context the observer was built with, for the
 * reason every other async call here names one: a task created from an
 * IPC dispatch would otherwise complete on a loop nobody runs.
 */
void clawt_observer_send_input_async(ClawtObserver       *self,
                                     const gchar         *agent_id,
                                     ClawtComputer       *computer,
                                     ClawtInputEvent     *event,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);

/**
 * clawt_observer_send_input_finish:
 * @self: a #ClawtObserver
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the event was sent
 */
gboolean clawt_observer_send_input_finish(ClawtObserver  *self,
                                          GAsyncResult   *result,
                                          GError        **error);

/**
 * clawt_observer_drop_agent:
 * @self: a #ClawtObserver
 * @agent_id: which agent
 *
 * Stops watching and forgets everything about this agent.
 */
void clawt_observer_drop_agent(ClawtObserver *self, const gchar *agent_id);

/**
 * clawt_observer_stop_all:
 * @self: a #ClawtObserver
 *
 * Stops every watch.  Called at daemon shutdown, so no timer outlives
 * the context it was attached to.
 */
void clawt_observer_stop_all(ClawtObserver *self);

G_END_DECLS
