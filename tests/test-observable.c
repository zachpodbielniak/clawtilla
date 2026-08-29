/*
 * test-observable.c - Watching a screen, and taking it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything here runs without a compositor, a VM or a session bus, and
 * that is the point: the failures this covers -- a frame that never
 * arrives, a click that lands in the wrong place, an agent refused for
 * ever by a lease nobody released -- all look identical from outside to
 * a screen where nothing is happening.
 *
 * The observable is driven through a fake backend rather than through a
 * real one.  A test against gowl or a guest would need the platform this
 * is meant to work without, and it would remove the thing being tested:
 * the *rules* about when a grab happens, which belong to
 * #ClawtObserver and not to either compositor.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

/* ── A computer with a screen, and one without ───────────────────── */

#define TEST_TYPE_SCREEN (test_screen_get_type())

G_DECLARE_FINAL_TYPE(TestScreen, test_screen, TEST, SCREEN, ClawtComputer)

struct _TestScreen {
    ClawtComputer parent_instance;

    guint     grabs;
    guint     starts;
    guint     stops;
    guint     inputs;
    gboolean  refuse_start;
    gboolean  fail_frame;
    gboolean  unchanged;
    gchar    *hash;
    gchar    *last_input;
    gchar    *viewer;
    guint     screen_width;
    guint     screen_height;
};

static void test_screen_observable_init(ClawtObservableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    TestScreen, test_screen, CLAWT_TYPE_COMPUTER,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_OBSERVABLE, test_screen_observable_init))

/*
 * A one-pixel PNG, which is enough for the only thing the observer reads
 * out of a frame: the IHDR that says how big the picture is.
 */
static const guchar tiny_png[] = {
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
    0x00, 0x00, 0x02, 0x80,   /* width  640 */
    0x00, 0x00, 0x01, 0xe0,   /* height 480 */
    0x08, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

static gboolean
test_screen_start(ClawtObservable *observable, guint fps, GError **error)
{
    TestScreen *self = TEST_SCREEN(observable);

    (void)fps;

    self->starts++;

    if (self->refuse_start) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this fake has no screen today");
        return FALSE;
    }

    return TRUE;
}

static void
test_screen_stop(ClawtObservable *observable)
{
    TEST_SCREEN(observable)->stops++;
}

static GBytes *
test_screen_frame(ClawtObservable  *observable,
                  const gchar      *if_changed_from,
                  gint64           *stamp_out,
                  gchar           **hash_out,
                  GError          **error)
{
    TestScreen *self = TEST_SCREEN(observable);

    self->grabs++;

    if (stamp_out != NULL)
        *stamp_out = g_get_real_time();

    if (self->fail_frame) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the compositor said no");
        return NULL;
    }

    if (hash_out != NULL)
        *hash_out = g_strdup(self->hash);

    if (self->unchanged &&
        g_strcmp0(if_changed_from, self->hash) == 0)
        return NULL;

    return g_bytes_new(tiny_png, sizeof(tiny_png));
}

static gboolean
test_screen_can_input(ClawtObservable *observable)
{
    (void)observable;

    return TRUE;
}

static gboolean
test_screen_send_input(ClawtObservable  *observable,
                       ClawtInputEvent  *event,
                       GError          **error)
{
    TestScreen *self = TEST_SCREEN(observable);

    (void)error;

    self->inputs++;
    g_free(self->last_input);
    self->last_input = clawt_input_event_describe(event);

    return TRUE;
}

static gchar *
test_screen_viewer_uri(ClawtObservable *observable)
{
    return g_strdup(TEST_SCREEN(observable)->viewer);
}

static gboolean
test_screen_geometry(ClawtObservable *observable, guint *width, guint *height)
{
    TestScreen *self = TEST_SCREEN(observable);

    if (self->screen_width == 0)
        return FALSE;

    if (width != NULL)
        *width = self->screen_width;

    if (height != NULL)
        *height = self->screen_height;

    return TRUE;
}

static void
test_screen_observable_init(ClawtObservableInterface *iface)
{
    iface->observe_start = test_screen_start;
    iface->observe_stop = test_screen_stop;
    iface->observe_frame = test_screen_frame;
    iface->observe_can_input = test_screen_can_input;
    iface->observe_send_input = test_screen_send_input;
    iface->observe_viewer_uri = test_screen_viewer_uri;
    iface->observe_geometry = test_screen_geometry;
}

static ClawtComputerType
test_screen_get_computer_type(ClawtComputer *computer)
{
    (void)computer;

    return CLAWT_COMPUTER_VM;
}

static void
test_screen_finalize(GObject *object)
{
    TestScreen *self = TEST_SCREEN(object);

    g_free(self->hash);
    g_free(self->last_input);
    g_free(self->viewer);

    G_OBJECT_CLASS(test_screen_parent_class)->finalize(object);
}

static void
test_screen_class_init(TestScreenClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = test_screen_finalize;
    CLAWT_COMPUTER_CLASS(klass)->get_computer_type =
        test_screen_get_computer_type;
}

static void
test_screen_init(TestScreen *self)
{
    self->hash = g_strdup("hash-one");
    self->screen_width = 1920;
    self->screen_height = 1080;
    self->viewer = g_strdup("vnc://127.0.0.1:5901");
}

/* ── A computer without the interface at all ─────────────────────── */

#define TEST_TYPE_BLIND (test_blind_get_type())

G_DECLARE_FINAL_TYPE(TestBlind, test_blind, TEST, BLIND, ClawtComputer)

struct _TestBlind {
    ClawtComputer parent_instance;
};

G_DEFINE_FINAL_TYPE(TestBlind, test_blind, CLAWT_TYPE_COMPUTER)

static ClawtComputerType
test_blind_get_computer_type(ClawtComputer *computer)
{
    (void)computer;

    return CLAWT_COMPUTER_CONTAINER;
}

static void
test_blind_class_init(TestBlindClass *klass)
{
    CLAWT_COMPUTER_CLASS(klass)->get_computer_type =
        test_blind_get_computer_type;
}

static void
test_blind_init(TestBlind *self)
{
    (void)self;
}

/* ── A computer that implements the interface and fills nothing in ── */

#define TEST_TYPE_HALF (test_half_get_type())

G_DECLARE_FINAL_TYPE(TestHalf, test_half, TEST, HALF, ClawtComputer)

struct _TestHalf {
    ClawtComputer parent_instance;
};

static void
test_half_observable_init(ClawtObservableInterface *iface)
{
    /*
     * Deliberately empty. A backend that says "I have a screen" and
     * implements half of what that means is an ordinary thing to have
     * mid-development, and the half it left out must refuse rather than
     * report success.
     */
    (void)iface;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(
    TestHalf, test_half, CLAWT_TYPE_COMPUTER,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_OBSERVABLE, test_half_observable_init))

static void
test_half_class_init(TestHalfClass *klass)
{
    (void)klass;
}

static void
test_half_init(TestHalf *self)
{
    (void)self;
}

/* ── The interface's defaults ────────────────────────────────────── */

/*
 * A backend that implements the interface and leaves a vfunc out must be
 * *refused*, not answered TRUE.
 *
 * This is the CALL_OR_TRUE bug one layer up: clawt_computer_teardown()
 * once reported a VM as removed while its domain and its disk stayed on
 * disk, because a missing vfunc answered success. Here the equivalent
 * would be a screen that reports a keystroke sent and sends none, which
 * is worse -- the person watching has been told the picture in front of
 * them is what the machine did.
 */
static void
test_a_missing_vfunc_refuses_and_names_the_type(void)
{
    g_autoptr(ClawtComputer) computer = g_object_new(TEST_TYPE_HALF, NULL);
    g_autoptr(ClawtInputEvent) event =
        clawt_input_event_new(CLAWT_INPUT_CLICK);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *hash = NULL;
    gint64 stamp = 0;

    g_assert_null(clawt_observable_frame(CLAWT_OBSERVABLE(computer), NULL,
                                         &stamp, &hash, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    /* The type is named, so a report says which backend is missing it. */
    g_assert_nonnull(strstr(error->message, "TestHalf"));

    g_clear_error(&error);
    g_assert_false(clawt_observable_send_input(CLAWT_OBSERVABLE(computer),
                                               event, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    /* And no viewer, rather than an address that reaches nothing. */
    g_assert_null(clawt_observable_viewer_uri(CLAWT_OBSERVABLE(computer)));
}

/*
 * A computer that cannot show a screen does not implement the interface,
 * so a client asking is told so rather than handed a blank frame.
 */
static void
test_a_computer_without_a_screen_is_not_observable(void)
{
    g_autoptr(ClawtComputer) blind = g_object_new(TEST_TYPE_BLIND, NULL);

    g_assert_false(CLAWT_IS_OBSERVABLE(blind));
}

/*
 * has_screen() names every type, so a backend added later cannot be
 * forgotten -- and it agrees with which of them actually implements the
 * interface.
 */
static void
test_has_screen_covers_every_type(void)
{
    guint i;

    g_assert_true(clawt_computer_type_has_screen(CLAWT_COMPUTER_HOST));
    g_assert_true(clawt_computer_type_has_screen(CLAWT_COMPUTER_VM));
    g_assert_false(clawt_computer_type_has_screen(CLAWT_COMPUTER_NONE));
    g_assert_false(clawt_computer_type_has_screen(CLAWT_COMPUTER_CONTAINER));
    g_assert_false(clawt_computer_type_has_screen(CLAWT_COMPUTER_DISTROBOX));
    g_assert_false(clawt_computer_type_has_screen(CLAWT_COMPUTER_SSH));

    /*
     * Walked from the type system rather than from a list here, so a
     * type added to the enumeration is asked about from the moment it
     * exists rather than when somebody remembers this file.
     */
    for (i = 0; i < clawt_computer_type_count(); i++)
        (void)clawt_computer_type_has_screen(clawt_computer_type_nth(i));
}

/*
 * The two backends that do implement it are exactly the two the
 * predicate names.
 *
 * Asked of the type system rather than by constructing a computer of
 * each kind, which would need podman and libvirt.
 */
static void
test_the_backends_that_implement_it_are_the_ones_named(void)
{
    g_assert_true(g_type_is_a(CLAWT_TYPE_VM_COMPUTER, CLAWT_TYPE_OBSERVABLE));
    g_assert_true(g_type_is_a(CLAWT_TYPE_HOST_COMPUTER,
                              CLAWT_TYPE_OBSERVABLE));

    g_assert_false(g_type_is_a(CLAWT_TYPE_CONTAINER_COMPUTER,
                               CLAWT_TYPE_OBSERVABLE));
    g_assert_false(g_type_is_a(CLAWT_TYPE_DISTROBOX_COMPUTER,
                               CLAWT_TYPE_OBSERVABLE));
    g_assert_false(g_type_is_a(CLAWT_TYPE_SSH_COMPUTER,
                               CLAWT_TYPE_OBSERVABLE));
    g_assert_false(g_type_is_a(CLAWT_TYPE_NULL_COMPUTER,
                               CLAWT_TYPE_OBSERVABLE));
}

/* ── The rate ────────────────────────────────────────────────────── */

/*
 * Zero means the default, not "never".
 *
 * An integer key nobody set reads as zero, and a client that subscribed
 * and got no frame at all would be looking at a blank panel with nothing
 * to explain it.
 */
static void
test_the_rate_is_clamped_at_both_ends(void)
{
    g_assert_cmpuint(clawt_observe_clamp_fps(0), ==, 1);
    g_assert_cmpuint(clawt_observe_clamp_fps(-5), ==, 1);
    g_assert_cmpuint(clawt_observe_clamp_fps(1), ==, 1);
    g_assert_cmpuint(clawt_observe_clamp_fps(4), ==, 4);
    g_assert_cmpuint(clawt_observe_clamp_fps(60), ==, CLAWT_OBSERVE_MAX_FPS);
    g_assert_cmpuint(clawt_observe_clamp_fps(1000000), ==,
                     CLAWT_OBSERVE_MAX_FPS);
}

/* ── Staleness ───────────────────────────────────────────────────── */

/*
 * A stale frame is labelled; a frame that never existed is not.
 *
 * Both clients ask this, so it lives here rather than twice -- and the
 * "never existed" half is the one that would otherwise draw "55 years
 * ago" over an empty panel.
 */
static void
test_an_old_frame_is_stale_and_a_missing_one_is_not(void)
{
    gint64 now = g_get_real_time();

    g_assert_false(clawt_frame_is_stale(0, now));
    g_assert_false(clawt_frame_is_stale(now, now));
    g_assert_false(clawt_frame_is_stale(now - G_USEC_PER_SEC, now));
    g_assert_true(clawt_frame_is_stale(
        now - ((gint64)(CLAWT_FRAME_STALE_SECONDS + 1) * G_USEC_PER_SEC),
        now));
}

/* ── The observer ────────────────────────────────────────────────── */

typedef struct {
    gchar         *root;
    GMainContext  *context;
    ClawtObserver *observer;
    ClawtComputer *computer;
    guint          frames;
    guint          failures;
    guint          baseline;
    gchar         *failure;
} ObserverFixture;

static void
on_frame(ClawtObserver *observer, const gchar *agent_id, const gchar *path,
         gpointer user_data)
{
    ObserverFixture *fixture = user_data;

    (void)observer;
    (void)agent_id;
    (void)path;

    fixture->frames++;
}

static void
on_failed(ClawtObserver *observer, const gchar *agent_id,
          const gchar *message, gpointer user_data)
{
    ObserverFixture *fixture = user_data;

    (void)observer;
    (void)agent_id;

    fixture->failures++;
    g_free(fixture->failure);
    fixture->failure = g_strdup(message);
}

static void
observer_setup(ObserverFixture *fixture)
{
    fixture->root = g_build_filename(g_get_tmp_dir(), "clawt-observe-XXXXXX",
                                     NULL);
    g_assert_nonnull(g_mkdtemp(fixture->root));

    fixture->context = g_main_context_new();
    g_main_context_push_thread_default(fixture->context);

    fixture->observer = clawt_observer_new(fixture->root, fixture->context);
    fixture->computer = g_object_new(TEST_TYPE_SCREEN, NULL);
    fixture->frames = 0;
    fixture->failures = 0;
    fixture->failure = NULL;

    g_signal_connect(fixture->observer, "frame", G_CALLBACK(on_frame),
                     fixture);
    g_signal_connect(fixture->observer, "failed", G_CALLBACK(on_failed),
                     fixture);
}

static void
observer_teardown(ObserverFixture *fixture)
{
    clawt_observer_stop_all(fixture->observer);

    g_clear_object(&fixture->observer);
    g_clear_object(&fixture->computer);

    g_main_context_pop_thread_default(fixture->context);
    g_clear_pointer(&fixture->context, g_main_context_unref);

    clawt_test_remove_tree(fixture->root);
    g_clear_pointer(&fixture->root, g_free);
    g_clear_pointer(&fixture->failure, g_free);
}

/*
 * Runs the fixture's context until @predicate holds, or gives up.
 *
 * A watchdog rather than an open wait: a test that can hang is worse
 * than one that fails, because a hung suite reports nothing at all.
 */
static gboolean
pump_until(ObserverFixture *fixture, gboolean (*ready)(ObserverFixture *),
           gint64 timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;

    while (!ready(fixture)) {
        if (g_get_monotonic_time() > deadline)
            return FALSE;

        g_main_context_iteration(fixture->context, FALSE);
        g_usleep(1000);
    }

    return TRUE;
}

static gboolean
have_a_frame(ObserverFixture *fixture)
{
    return fixture->frames > 0;
}

static gboolean
have_a_failure(ObserverFixture *fixture)
{
    return fixture->failures > 0;
}

static gboolean
grabs_rose(ObserverFixture *fixture)
{
    return TEST_SCREEN(fixture->computer)->grabs > fixture->baseline;
}

/*
 * Runs the loop for a bounded time and reports whether a grab happened.
 *
 * Both directions are needed and neither can be asserted synchronously:
 * a capture runs on a worker thread, so the counter moves after the call
 * that asked for it returns.
 */
static gboolean
grab_within(ObserverFixture *fixture, gint64 window_us)
{
    fixture->baseline = TEST_SCREEN(fixture->computer)->grabs;

    return pump_until(fixture, grabs_rose, window_us);
}

/*
 * Nothing is grabbed until somebody subscribes, and the first
 * subscriber gets a frame without waiting for the interval.
 */
static void
test_nothing_is_grabbed_until_somebody_subscribes(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    TestScreen *screen;

    observer_setup(&fixture);
    screen = TEST_SCREEN(fixture.computer);

    /*
     * The whole point of the design: an agent with a desktop is not
     * slower for having one, because a frame is only taken while
     * somebody is looking.
     */
    g_assert_cmpuint(screen->grabs, ==, 0);
    g_assert_cmpuint(screen->starts, ==, 0);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_cmpuint(screen->starts, ==, 1);
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));
    g_assert_cmpuint(screen->grabs, >=, 1);

    observer_teardown(&fixture);
}

/*
 * Two clients watching one agent produce one grab, not two.
 *
 * Not an optimisation: two grabs are two round trips through the
 * connection the agent works over, so whether it slowed down would
 * depend on how many people had the tab open.
 */
static void
test_two_watchers_share_one_grab(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    TestScreen *screen;
    guint after_first;

    observer_setup(&fixture);
    screen = TEST_SCREEN(fixture.computer);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));
    after_first = screen->grabs;

    /*
     * The second subscriber must not start a grab of its own, and must
     * not start a second backend either.
     */
    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "two", 1,
                                           &error));
    g_assert_cmpuint(screen->grabs, ==, after_first);
    g_assert_cmpuint(screen->starts, ==, 1);
    g_assert_cmpuint(clawt_observer_subscribers(fixture.observer, "chief"),
                     ==, 2);

    /* And the first one leaving does not stop it for the second. */
    g_assert_cmpuint(
        clawt_observer_unsubscribe(fixture.observer, "chief", "one"), ==, 1);
    g_assert_cmpuint(screen->stops, ==, 0);

    g_assert_cmpuint(
        clawt_observer_unsubscribe(fixture.observer, "chief", "two"), ==, 0);
    g_assert_cmpuint(screen->stops, ==, 1);

    observer_teardown(&fixture);
}

/*
 * Subscribing twice under one name is one watcher.
 *
 * The web client's poll subscribes on every request, because a browser
 * has no way to say goodbye -- so a count that went up each time would
 * leave the screen grabbed for ever after somebody closed a tab.
 */
static void
test_the_same_watcher_twice_is_one_watcher(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "web", 1,
                                           &error));
    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "web", 1,
                                           &error));
    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "web", 1,
                                           &error));

    g_assert_cmpuint(clawt_observer_subscribers(fixture.observer, "chief"),
                     ==, 1);
    g_assert_cmpuint(
        clawt_observer_unsubscribe(fixture.observer, "chief", "web"), ==, 0);

    observer_teardown(&fixture);
}

/*
 * A grab that fails reports the backend's own words.
 *
 * Not a summary of them: "automation is disabled" and "no such
 * interface" have completely different remedies, and a message of our
 * own would hide which of the two happened.
 */
static void
test_a_failing_grab_reports_the_backends_message(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);
    TEST_SCREEN(fixture.computer)->fail_frame = TRUE;

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_failure, 5 * G_USEC_PER_SEC));

    g_assert_cmpstr(fixture.failure, ==, "the compositor said no");
    g_assert_cmpstr(clawt_observer_get_last_error(fixture.observer, "chief"),
                    ==, "the compositor said no");
    g_assert_null(clawt_observer_get_frame_path(fixture.observer, "chief"));

    observer_teardown(&fixture);
}

/*
 * A frame the backend says has not changed is not written again, and the
 * stamp still moves forward.
 *
 * The second half matters as much as the first: a still desktop reported
 * with an ever-older stamp would be labelled stale, which tells somebody
 * supervising a working machine that it has stopped answering.
 */
static void
test_an_unchanged_screen_is_not_written_again(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *first_path = NULL;
    gint64 first_stamp;

    observer_setup(&fixture);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 4,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    first_path = g_strdup(clawt_observer_get_frame_path(fixture.observer,
                                                        "chief"));
    first_stamp = clawt_observer_get_frame_stamp(fixture.observer, "chief");
    g_assert_nonnull(first_path);
    g_assert_cmpint(first_stamp, >, 0);

    TEST_SCREEN(fixture.computer)->unchanged = TRUE;
    fixture.frames = 0;

    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    g_assert_cmpstr(clawt_observer_get_frame_path(fixture.observer, "chief"),
                    ==, first_path);
    g_assert_cmpint(
        clawt_observer_get_frame_stamp(fixture.observer, "chief"), >=,
        first_stamp);

    observer_teardown(&fixture);
}

/*
 * Both sizes are reported, because a click has to be scaled between them.
 *
 * The frame's comes from the PNG's own header rather than from what was
 * asked for -- the compositor does not upscale, so a screen narrower
 * than the ceiling arrives at its own width, and a client scaling by the
 * requested size would be scaling by a number no file ever had.
 */
static void
test_the_frame_and_the_screen_are_both_measured(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    guint frame_width = 0;
    guint frame_height = 0;
    guint screen_width = 0;
    guint screen_height = 0;

    observer_setup(&fixture);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    clawt_observer_get_sizes(fixture.observer, "chief", &frame_width,
                             &frame_height, &screen_width, &screen_height);

    g_assert_cmpuint(frame_width, ==, 640);
    g_assert_cmpuint(frame_height, ==, 480);
    g_assert_cmpuint(screen_width, ==, 1920);
    g_assert_cmpuint(screen_height, ==, 1080);

    observer_teardown(&fixture);
}

/*
 * The viewer's address is cached from the grab, not asked when a client
 * looks.
 *
 * Asking is a read of the running domain's XML, and a VM's libvirt can
 * be on another machine over qemu+ssh -- so a status handler that asked
 * would put an SSH round trip on the daemon's main context once per
 * redraw. And a machine that stops must stop offering one: an address
 * for a VM that is off sends whoever clicks it to debug their viewer.
 */
static void
test_the_viewer_address_follows_the_machine(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    TestScreen *screen;

    observer_setup(&fixture);
    screen = TEST_SCREEN(fixture.computer);

    /* Nothing before the first grab, rather than a guess. */
    g_assert_null(clawt_observer_get_viewer(fixture.observer, "chief"));

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    g_assert_cmpstr(clawt_observer_get_viewer(fixture.observer, "chief"),
                    ==, "vnc://127.0.0.1:5901");

    /* The machine goes; so does the address, on the next grab. */
    g_clear_pointer(&screen->viewer, g_free);
    g_usleep((CLAWT_OBSERVE_MIN_GAP_MS + 50) * 1000);
    fixture.frames = 0;
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    g_assert_null(clawt_observer_get_viewer(fixture.observer, "chief"));

    observer_teardown(&fixture);
}

/*
 * A turn that never touched the screen settles no frame.
 *
 * Otherwise every one-word reply would end with a fresh picture of an
 * idle desktop, grabbed down the connection the agent works over.
 *
 * Every step here is bounded well inside the one-second interval, so a
 * tick of the timer is never mistaken for a settle -- which is why this
 * is two tests rather than one long one.
 */
static void
test_a_turn_that_touched_nothing_settles_no_frame(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    /*
     * Waited out, because a grab issued inside the minimum gap is
     * dropped by design -- and this is about whether one is asked for,
     * not about the gap.
     */
    g_usleep((CLAWT_OBSERVE_MIN_GAP_MS + 50) * 1000);

    clawt_observer_settle_turn(fixture.observer, "chief");
    g_assert_false(grab_within(&fixture, 200 * 1000));

    observer_teardown(&fixture);
}

/*
 * A turn that did touch it settles one, and the flag is spent.
 *
 * The second half matters as much: a "touched" that was never cleared
 * would grab at the end of every turn for the rest of the session, which
 * is the cost this whole mechanism exists to avoid.
 */
static void
test_a_turn_that_touched_the_screen_settles_one_frame(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));

    g_usleep((CLAWT_OBSERVE_MIN_GAP_MS + 50) * 1000);

    fixture.baseline = TEST_SCREEN(fixture.computer)->grabs;
    clawt_observer_note_touched(fixture.observer, "chief");
    clawt_observer_settle_turn(fixture.observer, "chief");
    g_assert_true(pump_until(&fixture, grabs_rose, 200 * 1000));

    g_usleep((CLAWT_OBSERVE_MIN_GAP_MS + 50) * 1000);

    clawt_observer_settle_turn(fixture.observer, "chief");
    g_assert_false(grab_within(&fixture, 150 * 1000));

    observer_teardown(&fixture);
}

/*
 * A refresh inside the minimum gap is dropped.
 *
 * The refresh button and the interval timer share one capture path, so a
 * gap enforced only on the timer would leave the button as a way to make
 * an agent's connection unusable by holding a key down.
 */
static void
test_a_refresh_inside_the_minimum_gap_is_dropped(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;
    TestScreen *screen;
    guint before;

    observer_setup(&fixture);
    screen = TEST_SCREEN(fixture.computer);

    g_assert_true(clawt_observer_subscribe(fixture.observer, "chief",
                                           fixture.computer, "one", 1,
                                           &error));
    g_assert_true(pump_until(&fixture, have_a_frame, 5 * G_USEC_PER_SEC));
    before = screen->grabs;

    g_assert_false(clawt_observer_refresh(fixture.observer, "chief"));
    g_assert_false(clawt_observer_refresh(fixture.observer, "chief"));
    g_assert_cmpuint(screen->grabs, ==, before);

    /* And one after the gap is honoured. */
    g_usleep((CLAWT_OBSERVE_MIN_GAP_MS + 50) * 1000);
    g_assert_true(clawt_observer_refresh(fixture.observer, "chief"));

    observer_teardown(&fixture);
}

/*
 * A computer with no screen is refused when somebody subscribes, naming
 * its type -- rather than accepted and then producing nothing for ever.
 */
static void
test_subscribing_to_a_blind_computer_is_refused(void)
{
    ObserverFixture fixture;
    g_autoptr(ClawtComputer) blind = g_object_new(TEST_TYPE_BLIND, NULL);
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);

    g_assert_false(clawt_observer_subscribe(fixture.observer, "chief", blind,
                                            "one", 1, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "container"));

    observer_teardown(&fixture);
}

/*
 * A backend that refuses to start is not left as a watch nobody can see
 * failing.
 */
static void
test_a_backend_that_refuses_to_start_is_not_watched(void)
{
    ObserverFixture fixture;
    g_autoptr(GError) error = NULL;

    observer_setup(&fixture);
    TEST_SCREEN(fixture.computer)->refuse_start = TRUE;

    g_assert_false(clawt_observer_subscribe(fixture.observer, "chief",
                                            fixture.computer, "one", 1,
                                            &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_assert_cmpuint(clawt_observer_subscribers(fixture.observer, "chief"),
                     ==, 0);

    observer_teardown(&fixture);
}

/* ── The takeover ────────────────────────────────────────────────── */

/*
 * While a person holds the screen the agent's input is refused, and the
 * refusal says the three things it has to say.
 */
static void
test_the_refusal_says_what_the_agent_needs(void)
{
    const gchar *text = clawt_takeover_refusal_text();

    /* Nothing happened. */
    g_assert_nonnull(strstr(text, "nothing happened"));

    /* Do not retry. */
    g_assert_nonnull(strstr(text, "Do not retry"));

    /* And how to wait properly, by name. */
    g_assert_nonnull(strstr(text, "clawtilla_request_hands"));

    /*
     * Invariant, so every refusal is the same words. A message
     * assembled per call site would drift, and the half that drifted
     * would be the "do not retry" half -- which produces an agent that
     * retries once a second against a screen a person is using.
     */
    g_assert_true(text == clawt_takeover_refusal_text());
}

static void
test_a_hold_blocks_the_agent_and_releasing_settles_the_request(void)
{
    g_autoptr(ClawtTakeover) takeover = clawt_takeover_new();
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_takeover_is_held(takeover, "chief"));

    /* The agent may ask, and asking hands over nothing. */
    g_assert_true(clawt_takeover_request(takeover, "chief",
                                         "the dialog needs a password"));
    g_assert_cmpstr(clawt_takeover_get_request(takeover, "chief"), ==,
                    "the dialog needs a password");
    g_assert_false(clawt_takeover_is_held(takeover, "chief"));

    /* Only a person takes it. */
    g_assert_true(clawt_takeover_take(takeover, "chief", "zach", 60,
                                      &error));
    g_assert_true(clawt_takeover_is_held(takeover, "chief"));
    g_assert_cmpstr(clawt_takeover_get_holder(takeover, "chief"), ==,
                    "zach");

    /*
     * And letting go settles the ask, so there is no separate "done
     * helping" step to forget -- an ask left open after the person has
     * finished is an agent waiting for something that already happened.
     */
    g_assert_true(clawt_takeover_release(takeover, "chief"));
    g_assert_false(clawt_takeover_is_held(takeover, "chief"));
    g_assert_null(clawt_takeover_get_request(takeover, "chief"));
}

/*
 * The lease lapses, so a browser tab closed mid-takeover cannot lock an
 * agent out for good.
 *
 * Asserted on the *read* rather than on a timer firing, because that is
 * how it is implemented and how it has to be: a timer is a promise about
 * when the loop next runs a source, and under load an agent would go on
 * being refused after its lease had run out.
 */
static void
test_a_lease_lapses_on_its_own(void)
{
    g_autoptr(ClawtTakeover) takeover = clawt_takeover_new();
    g_autoptr(GError) error = NULL;

    /*
     * A lease of one second, and the wait is real. A negative or zero
     * lease is floored rather than trusted -- an already-expired hold
     * would make taking the screen appear to succeed and change
     * nothing.
     */
    g_assert_true(clawt_takeover_take(takeover, "chief", "zach", 1, &error));
    g_assert_true(clawt_takeover_is_held(takeover, "chief"));

    g_usleep(1100 * 1000);

    g_assert_false(clawt_takeover_is_held(takeover, "chief"));
    g_assert_null(clawt_takeover_get_holder(takeover, "chief"));
    g_assert_cmpint(clawt_takeover_get_expires_at(takeover, "chief"), ==, 0);
}

/*
 * Taking a screen somebody else holds is refused and names them, rather
 * than stolen. Taking one you already hold extends it.
 */
static void
test_a_second_person_is_refused_and_the_holder_may_extend(void)
{
    g_autoptr(ClawtTakeover) takeover = clawt_takeover_new();
    g_autoptr(GError) error = NULL;
    gint64 first;

    g_assert_true(clawt_takeover_take(takeover, "chief", "zach", 60,
                                      &error));
    first = clawt_takeover_get_expires_at(takeover, "chief");

    g_assert_false(clawt_takeover_take(takeover, "chief", "somebody else",
                                       60, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull(strstr(error->message, "zach"));
    g_clear_error(&error);

    /* Still theirs. */
    g_assert_cmpstr(clawt_takeover_get_holder(takeover, "chief"), ==,
                    "zach");

    g_usleep(20 * 1000);
    g_assert_true(clawt_takeover_take(takeover, "chief", "zach", 60,
                                      &error));
    g_assert_cmpint(clawt_takeover_get_expires_at(takeover, "chief"), >,
                    first);
}

/*
 * Which desktop tools a takeover actually stops.
 *
 * Acting and spawning, and not observing: an agent that can still see
 * the screen can wait usefully, and one that has been blinded as well
 * simply stops. Read from the same table the permission check uses, so
 * the two cannot disagree about what "acting" means.
 */
static void
test_only_the_acting_tools_are_gated(void)
{
    g_assert_true(clawt_desktop_tool_is_acting("mouse_click"));
    g_assert_true(clawt_desktop_tool_is_acting("type_text"));
    g_assert_true(clawt_desktop_tool_is_acting("key_combo"));
    g_assert_true(clawt_desktop_tool_is_acting("spawn"));

    g_assert_false(clawt_desktop_tool_is_acting("screenshot"));
    g_assert_false(clawt_desktop_tool_is_acting("screenshot_frame"));
    g_assert_false(clawt_desktop_tool_is_acting("list_windows"));
    g_assert_false(clawt_desktop_tool_is_acting("get_enabled"));
    g_assert_false(clawt_desktop_tool_is_acting(NULL));
}

/* ── The wire to a compositor ────────────────────────────────────── */

/*
 * gdbus prints type annotations, and `uint32` contains a digit run.
 *
 * A parser that scanned for digits read the 32 out of "uint32" as the
 * width, which produced a frame reported as 32 pixels wide by a version
 * of this that looked perfectly plausible.
 */
static void
test_the_frame_tuple_is_parsed_past_its_type_names(void)
{
    ClawtScreenFrameInfo info = { 0 };
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_screen_parse_gdbus_frame(
        "('/tmp/gnome-mcp/frame.png', uint32 1280, uint32 800, "
        "'abc123', int64 1788000000000000)\n", &info, &error));

    g_assert_cmpstr(info.path, ==, "/tmp/gnome-mcp/frame.png");
    g_assert_cmpuint(info.width, ==, 1280);
    g_assert_cmpuint(info.height, ==, 800);
    g_assert_cmpstr(info.hash, ==, "abc123");
    g_assert_cmpint(info.stamp, ==, 1788000000000000LL);

    clawt_screen_frame_info_clear(&info);
}

/*
 * Anything that is not a frame tuple is refused rather than half-read.
 *
 * The thing most likely to arrive instead is a gdbus error message, and
 * a lenient parser would take the first quoted fragment of one as a path
 * and go looking for a file named after an exception.
 */
static void
test_an_error_from_gdbus_is_not_read_as_a_frame(void)
{
    ClawtScreenFrameInfo info = { 0 };
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_screen_parse_gdbus_frame(
        "Error: GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: "
        "The name org.gnome.Shell was not provided", &info, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
    g_assert_null(info.path);

    g_clear_error(&error);
    g_assert_false(clawt_screen_parse_gdbus_frame(NULL, &info, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
}

/*
 * The frame request never asks for the cursor.
 *
 * `includeCursor` composites the pointer into the image, so two frames
 * of a perfectly still screen differ whenever the mouse has moved a
 * pixel -- and the hash that makes polling cheap would change on every
 * grab.
 */
static void
test_the_frame_request_leaves_the_cursor_out(void)
{
    g_auto(GStrv) argv = clawt_screen_gnome_frame_argv(1280, FALSE);
    g_autofree gchar *joined = g_strjoinv(" ", argv);

    g_assert_cmpstr(argv[0], ==, "gdbus");
    g_assert_nonnull(strstr(joined,
                            CLAWT_SCREEN_GNOME_INTERFACE
                            ".ScreenshotFrame"));
    g_assert_nonnull(strstr(joined, "1280"));
    g_assert_nonnull(strstr(joined, "false"));
}

/*
 * A guest command finds the session bus, and every word of it is quoted.
 *
 * ssh arrives with no DBUS_SESSION_BUS_ADDRESS, so a bare gdbus over it
 * fails with "Cannot autolaunch D-Bus without X11" -- which reads as the
 * extension being missing rather than as the bus being unfindable. And a
 * string somebody typed into a takeover must not become shell syntax on
 * the way.
 */
static void
test_a_guest_command_carries_its_own_bus_and_quotes_everything(void)
{
    g_autoptr(ClawtInputEvent) event =
        clawt_input_event_new(CLAWT_INPUT_TEXT);
    g_auto(GStrv) tail = NULL;
    g_auto(GStrv) wrapped = NULL;

    clawt_input_event_set_text(event, "hi; rm -rf ~");
    tail = clawt_screen_gnome_input_argv(event);
    g_assert_nonnull(tail);

    wrapped = clawt_screen_in_session_argv((const gchar * const *)tail);

    g_assert_cmpstr(wrapped[0], ==, "sh");
    g_assert_cmpstr(wrapped[1], ==, "-c");
    g_assert_nonnull(strstr(wrapped[2], "DBUS_SESSION_BUS_ADDRESS"));
    g_assert_nonnull(strstr(wrapped[2], "/run/user/$(id -u)/bus"));

    /*
     * The dangerous half: the text arrives quoted, so the semicolon is
     * a character in an argument rather than a command separator.
     *
     * Asserted on the *unquoted* form being absent -- a bare argument
     * follows a space -- rather than on the text being absent, which it
     * obviously is not: the quoted form contains it.
     */
    g_assert_nonnull(strstr(wrapped[2], "'hi; rm -rf ~'"));
    g_assert_null(strstr(wrapped[2], " hi; rm -rf ~"));
}

/*
 * A scroll is written in the C locale.
 *
 * g_strdup_printf("%f") writes a decimal comma in half the world's
 * locales, and gdbus parses the argument as a GVariant double -- so a
 * scroll from a German desktop was a syntax error nobody could
 * reproduce.
 */
static void
test_a_scroll_is_written_in_the_c_locale(void)
{
    g_autoptr(ClawtInputEvent) event =
        clawt_input_event_new(CLAWT_INPUT_SCROLL);
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;

    event->dy = 1.5;
    argv = clawt_screen_gnome_input_argv(event);
    joined = g_strjoinv(" ", argv);

    g_assert_nonnull(strstr(joined, "1.5"));
    g_assert_null(strstr(joined, "1,5"));
}

/*
 * A takeover's audit line says what was done and not what was typed.
 *
 * Somebody driving their own desktop is not something to write down
 * keystroke by keystroke, and a trail that did would be a keylogger
 * written by accident in the one place nobody would look for one.
 */
static void
test_a_typed_string_is_counted_and_not_recorded(void)
{
    g_autoptr(ClawtInputEvent) event =
        clawt_input_event_new(CLAWT_INPUT_TEXT);
    g_autofree gchar *described = NULL;

    clawt_input_event_set_text(event, "hunter2");
    described = clawt_input_event_describe(event);

    g_assert_null(strstr(described, "hunter2"));
    g_assert_nonnull(strstr(described, "7"));
}

/*
 * A PNG's size comes from its own header.
 *
 * The compositor does not upscale, so a screen narrower than the ceiling
 * arrives at its own width -- and a client scaling a click by the size
 * that was *requested* would be scaling by a number no file ever had.
 */
static void
test_a_frames_size_comes_from_the_image(void)
{
    g_autoptr(GBytes) png = g_bytes_new(tiny_png, sizeof(tiny_png));
    g_autoptr(GBytes) rubbish = g_bytes_new("not a png at all", 16);
    guint width = 0;
    guint height = 0;

    g_assert_true(clawt_screen_png_size(png, &width, &height));
    g_assert_cmpuint(width, ==, 640);
    g_assert_cmpuint(height, ==, 480);

    g_assert_false(clawt_screen_png_size(rubbish, &width, &height));
    g_assert_cmpuint(width, ==, 0);
    g_assert_false(clawt_screen_png_size(NULL, NULL, NULL));
}

/* ── The sub-views ───────────────────────────────────────────────── */

/*
 * The Computer page's four halves, walked rather than listed.
 *
 * Both clients build their tabs from this, so a fifth added here appears
 * in the window and in the browser or in neither.
 */
static void
test_the_computer_sub_views_round_trip(void)
{
    guint i;

    g_assert_cmpuint(clawt_computer_view_count(), ==, 4);

    for (i = 0; i < clawt_computer_view_count(); i++) {
        const gchar *nick = clawt_computer_view_nth_nick(i);

        g_assert_nonnull(nick);
        g_assert_nonnull(clawt_computer_view_nth_label(i));
        g_assert_cmpint(clawt_computer_view_from_nick(nick), ==,
                        clawt_computer_view_nth(i));
    }

    /*
     * A name from a path somebody typed is the console rather than an
     * error: it is the one sub-view every computer has.
     */
    g_assert_cmpint(clawt_computer_view_from_nick("nonsense"), ==,
                    CLAWT_COMPUTER_VIEW_SHELL);
    g_assert_cmpint(clawt_computer_view_from_nick(NULL), ==,
                    CLAWT_COMPUTER_VIEW_SHELL);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/observable/missing-vfunc-refuses",
                    test_a_missing_vfunc_refuses_and_names_the_type);
    g_test_add_func("/observable/no-screen-is-not-observable",
                    test_a_computer_without_a_screen_is_not_observable);
    g_test_add_func("/observable/has-screen-covers-every-type",
                    test_has_screen_covers_every_type);
    g_test_add_func("/observable/implementers-match-the-predicate",
                    test_the_backends_that_implement_it_are_the_ones_named);
    g_test_add_func("/observable/fps-is-clamped",
                    test_the_rate_is_clamped_at_both_ends);
    g_test_add_func("/observable/stale-frame-is-labelled",
                    test_an_old_frame_is_stale_and_a_missing_one_is_not);

    g_test_add_func("/observer/nothing-until-subscribed",
                    test_nothing_is_grabbed_until_somebody_subscribes);
    g_test_add_func("/observer/two-watchers-one-grab",
                    test_two_watchers_share_one_grab);
    g_test_add_func("/observer/same-watcher-twice-is-one",
                    test_the_same_watcher_twice_is_one_watcher);
    g_test_add_func("/observer/failure-reports-the-backend",
                    test_a_failing_grab_reports_the_backends_message);
    g_test_add_func("/observer/unchanged-is-not-rewritten",
                    test_an_unchanged_screen_is_not_written_again);
    g_test_add_func("/observer/both-sizes-are-measured",
                    test_the_frame_and_the_screen_are_both_measured);
    g_test_add_func("/observer/viewer-follows-the-machine",
                    test_the_viewer_address_follows_the_machine);
    g_test_add_func("/observer/untouched-turn-settles-nothing",
                    test_a_turn_that_touched_nothing_settles_no_frame);
    g_test_add_func("/observer/touched-turn-settles-one",
                    test_a_turn_that_touched_the_screen_settles_one_frame);
    g_test_add_func("/observer/refresh-honours-the-gap",
                    test_a_refresh_inside_the_minimum_gap_is_dropped);
    g_test_add_func("/observer/blind-computer-is-refused",
                    test_subscribing_to_a_blind_computer_is_refused);
    g_test_add_func("/observer/a-refused-start-is-not-watched",
                    test_a_backend_that_refuses_to_start_is_not_watched);

    g_test_add_func("/takeover/refusal-says-three-things",
                    test_the_refusal_says_what_the_agent_needs);
    g_test_add_func("/takeover/release-settles-the-request",
                    test_a_hold_blocks_the_agent_and_releasing_settles_the_request);
    g_test_add_func("/takeover/lease-lapses",
                    test_a_lease_lapses_on_its_own);
    g_test_add_func("/takeover/second-person-refused",
                    test_a_second_person_is_refused_and_the_holder_may_extend);
    g_test_add_func("/takeover/only-acting-tools-are-gated",
                    test_only_the_acting_tools_are_gated);

    g_test_add_func("/screen/frame-tuple-past-type-names",
                    test_the_frame_tuple_is_parsed_past_its_type_names);
    g_test_add_func("/screen/gdbus-error-is-not-a-frame",
                    test_an_error_from_gdbus_is_not_read_as_a_frame);
    g_test_add_func("/screen/frame-request-omits-the-cursor",
                    test_the_frame_request_leaves_the_cursor_out);
    g_test_add_func("/screen/guest-command-quotes-everything",
                    test_a_guest_command_carries_its_own_bus_and_quotes_everything);
    g_test_add_func("/screen/scroll-uses-the-c-locale",
                    test_a_scroll_is_written_in_the_c_locale);
    g_test_add_func("/screen/typed-text-is-not-recorded",
                    test_a_typed_string_is_counted_and_not_recorded);
    g_test_add_func("/screen/size-comes-from-the-image",
                    test_a_frames_size_comes_from_the_image);

    g_test_add_func("/computer/sub-views-round-trip",
                    test_the_computer_sub_views_round_trip);

    return g_test_run();
}
