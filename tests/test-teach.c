/*
 * test-teach.c - Recording a task, and writing a skill from it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What is tested here and what is not, stated plainly rather than left
 * to be discovered, because a check finds the layer it looks at.
 *
 * **Tested:** the trace, its bounds, its caveats, the refusing base
 * class, the agent recorder end to end, both compositors' payload
 * parsers against string literals, the gdbus argv and its tuple parser,
 * and the synthesizer driven by AiMockProvider through the same tool
 * loop a real model would take.
 *
 * **Not tested, and not testable without a live compositor:** the
 * transports. The unix-socket call into gowl and the SSH-plus-gdbus call
 * into a guest are exercised by nothing here. The argv they build is
 * asserted on; whether the far end answers is not. That is why the
 * payload readers are public functions taking a string rather than
 * private steps inside the socket call -- the part with the decisions in
 * it is reachable, and the part that needs a compositor is as thin as it
 * can be made.
 */

#include <clawtilla.h>

/* ai-glib does not expose the mock through its umbrella header. */
#include <agent/ai-mock-provider.h>

#include <string.h>

#include "clawt-test-util.h"

static gchar *test_root = NULL;

static gchar *
scratch(const gchar *name)
{
    return g_build_filename(test_root, name, NULL);
}

/* ── A recorder that answers nothing, and one that answers badly ─── */

/*
 * The bare subclass: overrides nothing at all.
 *
 * Its whole job is to prove that a missing vfunc **refuses, naming the
 * type** rather than answering TRUE. A recorder that reported a
 * demonstration started and captured nothing tells the person
 * demonstrating at the end, after they have done the work twice.
 */
#define TEST_TYPE_MUTE_RECORDER (test_mute_recorder_get_type())

G_DECLARE_FINAL_TYPE(TestMuteRecorder, test_mute_recorder,
                     TEST, MUTE_RECORDER, ClawtTeachRecorder)

struct _TestMuteRecorder {
    ClawtTeachRecorder parent_instance;
};

G_DEFINE_FINAL_TYPE(TestMuteRecorder, test_mute_recorder,
                    CLAWT_TYPE_TEACH_RECORDER)

static void
test_mute_recorder_class_init(TestMuteRecorderClass *klass)
{
    (void)klass;
}

static void
test_mute_recorder_init(TestMuteRecorder *self)
{
    (void)self;
}

/*
 * The subclass whose backend goes away: start works, stop and drain
 * fail, exactly as a compositor that exited mid-demonstration does.
 */
#define TEST_TYPE_LOST_RECORDER (test_lost_recorder_get_type())

G_DECLARE_FINAL_TYPE(TestLostRecorder, test_lost_recorder,
                     TEST, LOST_RECORDER, ClawtTeachRecorder)

struct _TestLostRecorder {
    ClawtTeachRecorder parent_instance;
};

G_DEFINE_FINAL_TYPE(TestLostRecorder, test_lost_recorder,
                    CLAWT_TYPE_TEACH_RECORDER)

static gboolean
lost_start(ClawtTeachRecorder *self, GError **error)
{
    (void)self;
    (void)error;

    return TRUE;
}

static gboolean
lost_gone(ClawtTeachRecorder *self, GError **error)
{
    (void)self;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                        "the compositor is not answering");

    return FALSE;
}

static void
test_lost_recorder_class_init(TestLostRecorderClass *klass)
{
    ClawtTeachRecorderClass *recorder_class =
        CLAWT_TEACH_RECORDER_CLASS(klass);

    recorder_class->start = lost_start;
    recorder_class->stop = lost_gone;
    recorder_class->drain = lost_gone;
}

static void
test_lost_recorder_init(TestLostRecorder *self)
{
    (void)self;
}

static ClawtTeachRecorder *
make_recorder(GType type, const gchar *id, ClawtTeachSource source)
{
    ClawtTeachRecorder *recorder = g_object_new(type, NULL);
    g_autoptr(ClawtTeachTrace) trace = clawt_teach_trace_new(id, source);
    g_autofree gchar *directory = scratch(id);

    clawt_teach_trace_set_directory(trace, directory);
    clawt_teach_recorder_adopt_trace(recorder, trace);

    return recorder;
}

/* ── The trace ───────────────────────────────────────────────────── */

static void
test_steps_are_kept_in_order(void)
{
    g_autoptr(ClawtTeachTrace) trace =
        clawt_teach_trace_new("ordered", CLAWT_TEACH_SOURCE_AGENT);
    GPtrArray *steps;

    clawt_teach_trace_add_step(
        trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, "first"));
    clawt_teach_trace_add_step(
        trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, "second"));
    clawt_teach_trace_add_step(
        trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, "third"));

    steps = clawt_teach_trace_get_steps(trace);

    g_assert_cmpuint(steps->len, ==, 3);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 0)),
                    ==, "first");
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 2)),
                    ==, "third");
}

/*
 * A recording is read back long after it was made -- from another
 * machine, out of a backup, out of a state directory kept in git -- so
 * everything a reviewer needs has to survive the round trip. The
 * caveats most of all: a trace whose caveat was lost in the write is a
 * trace somebody reads as safe.
 */
static void
test_a_trace_round_trips_through_disk(void)
{
    g_autofree gchar *directory = scratch("round-trip");
    g_autoptr(ClawtTeachTrace) loaded = NULL;
    g_autoptr(GError) error = NULL;

    {
        g_autoptr(ClawtTeachTrace) trace =
            clawt_teach_trace_new("round-trip", CLAWT_TEACH_SOURCE_HOST_DEMO);
        ClawtTeachStep *step;

        clawt_teach_trace_set_directory(trace, directory);
        clawt_teach_trace_set_agent_id(trace, "builder");
        clawt_teach_trace_set_goal(trace, "cut a release");
        clawt_teach_trace_add_caveat(trace, CLAWT_TEACH_HOST_DEMO_CAVEAT);
        clawt_teach_trace_add_dropped(trace, 4);
        clawt_teach_trace_add_suppressed(trace, 2);

        step = clawt_teach_step_new(CLAWT_TEACH_STEP_KEY, "Key a");
        clawt_teach_step_set_detail(step, "into the terminal");
        clawt_teach_step_set_times(step, 1788000000000000LL, 12345);
        clawt_teach_trace_add_step(trace, step);

        g_assert_true(clawt_teach_trace_save(trace, &error));
        g_assert_no_error(error);
    }

    loaded = clawt_teach_trace_load(directory, &error);
    g_assert_no_error(error);
    g_assert_nonnull(loaded);

    g_assert_cmpstr(clawt_teach_trace_get_id(loaded), ==, "round-trip");
    g_assert_cmpint(clawt_teach_trace_get_source(loaded), ==,
                    CLAWT_TEACH_SOURCE_HOST_DEMO);
    g_assert_cmpstr(clawt_teach_trace_get_agent_id(loaded), ==, "builder");
    g_assert_cmpstr(clawt_teach_trace_get_goal(loaded), ==, "cut a release");
    g_assert_cmpuint(clawt_teach_trace_get_dropped(loaded), ==, 4);
    g_assert_cmpuint(clawt_teach_trace_get_suppressed(loaded), ==, 2);

    g_assert_cmpuint(clawt_teach_trace_get_caveats(loaded)->len, ==, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(
                                clawt_teach_trace_get_caveats(loaded), 0),
                            "IS recorded"));

    g_assert_cmpuint(clawt_teach_trace_get_steps(loaded)->len, ==, 1);
    g_assert_cmpint(
        clawt_teach_step_get_wall_us(
            g_ptr_array_index(clawt_teach_trace_get_steps(loaded), 0)),
        ==, 1788000000000000LL);
}

/*
 * A caveat arrives on every drain, so it has to be recorded once.
 * Forty copies of the same sentence is a sentence nobody reads, which
 * defeats the whole reason it is carried on the trace rather than left
 * in a document.
 */
static void
test_a_caveat_is_recorded_once(void)
{
    g_autoptr(ClawtTeachTrace) trace =
        clawt_teach_trace_new("caveats", CLAWT_TEACH_SOURCE_HOST_DEMO);

    clawt_teach_trace_add_caveat(trace, CLAWT_TEACH_HOST_DEMO_CAVEAT);
    clawt_teach_trace_add_caveat(trace, CLAWT_TEACH_HOST_DEMO_CAVEAT);
    clawt_teach_trace_add_caveat(trace, CLAWT_TEACH_HOST_DEMO_CAVEAT);
    clawt_teach_trace_add_caveat(trace, "and something else");

    g_assert_cmpuint(clawt_teach_trace_get_caveats(trace)->len, ==, 2);
}

/*
 * A frame is named by its file name inside the trace directory, never
 * by a path. A stored path resolves on exactly one machine, which is
 * the trap this tree already records for avatars and attachments.
 */
static void
test_a_frame_is_a_name_not_a_path(void)
{
    g_autoptr(ClawtTeachStep) step =
        clawt_teach_step_new(CLAWT_TEACH_STEP_DESKTOP, "click");

    clawt_teach_step_set_frame(step, "frame-0001.png");
    g_assert_cmpstr(clawt_teach_step_get_frame(step), ==, "frame-0001.png");

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*not by a path*");
    clawt_teach_step_set_frame(step, "/etc/shadow");
    g_test_assert_expected_messages();

    g_assert_cmpstr(clawt_teach_step_get_frame(step), ==, "frame-0001.png");
}

/*
 * A model handed a prefix has to be told it is a prefix. Without the
 * line it writes a procedure that stops in the middle and reads as
 * complete.
 */
static void
test_the_rendering_says_when_it_is_truncated(void)
{
    g_autoptr(ClawtTeachTrace) trace =
        clawt_teach_trace_new("long", CLAWT_TEACH_SOURCE_AGENT);
    g_autofree gchar *all = NULL;
    g_autofree gchar *some = NULL;
    guint i;

    for (i = 0; i < 10; i++) {
        g_autofree gchar *label = g_strdup_printf("step %u", i);

        clawt_teach_trace_add_step(
            trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, label));
    }

    all = clawt_teach_trace_render(trace, 0);
    some = clawt_teach_trace_render(trace, 3);

    g_assert_null(strstr(all, "more steps, not shown"));
    g_assert_nonnull(strstr(some, "7 more steps, not shown"));
    g_assert_nonnull(strstr(some, "step 2"));
    g_assert_null(strstr(some, "step 5"));
}

/*
 * The id becomes a directory under the state directory, so it is
 * checked here rather than trusted -- the same reason the observer
 * checks an agent id before writing a frame named after it.
 */
static void
test_a_recording_id_is_the_traversal_gate(void)
{
    g_assert_null(clawt_teach_trace_new("../evil", CLAWT_TEACH_SOURCE_AGENT));
    g_assert_null(clawt_teach_trace_new("a/b", CLAWT_TEACH_SOURCE_AGENT));
    g_assert_null(clawt_teach_trace_new("", CLAWT_TEACH_SOURCE_AGENT));

    {
        g_autoptr(ClawtTeachTrace) ok =
            clawt_teach_trace_new("4f2c8a1b", CLAWT_TEACH_SOURCE_AGENT);

        g_assert_nonnull(ok);
    }
}

/* ── The base class ──────────────────────────────────────────────── */

/*
 * A missing vfunc refuses and says which type refused, never TRUE.
 */
static void
test_a_missing_vfunc_refuses_naming_the_type(void)
{
    g_autoptr(ClawtTeachRecorder) recorder =
        make_recorder(TEST_TYPE_MUTE_RECORDER, "mute",
                      CLAWT_TEACH_SOURCE_AGENT);
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_teach_recorder_start(recorder, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "TestMuteRecorder"));
    g_assert_false(clawt_teach_recorder_is_active(recorder));

    g_clear_error(&error);

    /* And a stop on something that never started says so plainly. */
    g_assert_false(clawt_teach_recorder_stop(recorder, "no reason", &error));
    g_assert_nonnull(error);
}

/*
 * The event bound keeps the earliest steps and counts what it refused.
 *
 * The earliest, because the first part of a procedure is a usable
 * prefix of it while a slice out of the middle is not -- and counted,
 * because a silently truncated demonstration teaches half a task and
 * reads as a whole one.
 */
static void
test_the_event_bound_keeps_the_earliest_and_counts_the_rest(void)
{
    g_autoptr(ClawtTeachRecorder) recorder =
        make_recorder(TEST_TYPE_LOST_RECORDER, "bounded",
                      CLAWT_TEACH_SOURCE_AGENT);
    ClawtTeachTrace *trace = clawt_teach_recorder_get_trace(recorder);
    g_autoptr(GError) error = NULL;
    guint i;

    clawt_teach_recorder_set_limits(recorder, 900, 3);
    g_assert_true(clawt_teach_recorder_start(recorder, &error));
    g_assert_no_error(error);

    for (i = 0; i < 10; i++) {
        g_autofree gchar *label = g_strdup_printf("step %u", i);

        clawt_teach_recorder_note_step(
            recorder, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, label));
    }

    g_assert_cmpuint(clawt_teach_trace_get_steps(trace)->len, ==, 3);
    g_assert_cmpuint(clawt_teach_trace_get_dropped(trace), ==, 7);
    g_assert_cmpstr(clawt_teach_step_get_label(
                        g_ptr_array_index(clawt_teach_trace_get_steps(trace),
                                          0)),
                    ==, "step 0");
}

/*
 * Zero means the default, never "no limit".
 *
 * An unset integer key reads as zero, and a recording with no deadline
 * is precisely the failure the deadline exists to prevent.
 */
static void
test_zero_limits_mean_the_defaults(void)
{
    g_autoptr(ClawtTeachRecorder) recorder =
        make_recorder(TEST_TYPE_LOST_RECORDER, "defaults",
                      CLAWT_TEACH_SOURCE_AGENT);

    clawt_teach_recorder_set_limits(recorder, 0, 0);

    g_assert_cmpuint(clawt_teach_recorder_get_max_seconds(recorder), ==,
                     CLAWT_TEACH_DEFAULT_MAX_SECONDS);
    g_assert_cmpuint(clawt_teach_recorder_get_max_events(recorder), ==,
                     CLAWT_TEACH_DEFAULT_MAX_EVENTS);
}

/*
 * A compositor that exited mid-demonstration must not leave a recorder
 * nobody can end.
 *
 * The failure is reported; the recording ends anyway, and the partial
 * trace is written. The other way round -- staying "running" because
 * the backend did not answer -- would leave the indicator gone, the
 * events gone, and clawtilla still refusing to start another.
 */
static void
test_a_vanished_backend_still_ends_the_recording(void)
{
    g_autoptr(ClawtTeachRecorder) recorder =
        make_recorder(TEST_TYPE_LOST_RECORDER, "vanished",
                      CLAWT_TEACH_SOURCE_HOST_DEMO);
    g_autofree gchar *directory = scratch("vanished");
    g_autoptr(ClawtTeachTrace) written = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_teach_recorder_start(recorder, &error));
    g_assert_no_error(error);

    clawt_teach_recorder_note_step(
        recorder, clawt_teach_step_new(CLAWT_TEACH_STEP_KEY, "Key a"));

    g_assert_false(clawt_teach_recorder_stop(recorder, "you stopped it",
                                             &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "not answering"));

    /* Ended, whatever the backend said. */
    g_assert_false(clawt_teach_recorder_is_active(recorder));

    /* And what it did capture is on disk. */
    g_clear_error(&error);
    written = clawt_teach_trace_load(directory, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_teach_trace_get_steps(written)->len, ==, 1);
    g_assert_cmpstr(clawt_teach_trace_get_stop_reason(written), ==,
                    "you stopped it");
}

/* ── The agent trace recorder ────────────────────────────────────── */

static ClawtAgentTraceRecorder *
make_agent_recorder(const gchar *id)
{
    g_autofree gchar *directory = scratch(id);

    return clawt_agent_trace_recorder_new(id, directory, "builder");
}

/*
 * The steps an agent actually takes, in the two shapes they arrive in.
 *
 * `clawtilla_computer_exec` is recorded as the command rather than as a
 * tool call, because that is what it is to whoever reads the trace: the
 * step somebody would repeat is `make release`, not "the agent called a
 * tool whose second argument was make release".
 */
static void
test_an_agent_trace_records_calls_and_commands(void)
{
    g_autoptr(ClawtAgentTraceRecorder) recorder =
        make_agent_recorder("agent-steps");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    ClawtTeachTrace *trace;
    GPtrArray *steps;
    g_autoptr(GError) error = NULL;

    g_assert_nonnull(recorder);
    g_assert_true(clawt_teach_recorder_start(base, &error));
    g_assert_no_error(error);

    clawt_agent_trace_recorder_note_tool_call(recorder, "clawtilla_task_list",
                                              "{}");
    clawt_agent_trace_recorder_note_tool_call(
        recorder, "clawtilla_computer_exec",
        "{\"command\":\"make release\"}");
    clawt_agent_trace_recorder_note_desktop(recorder, "mouse_click");

    trace = clawt_teach_recorder_get_trace(base);
    steps = clawt_teach_trace_get_steps(trace);

    g_assert_cmpuint(steps->len, ==, 3);

    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 0)),
                    ==, CLAWT_TEACH_STEP_TOOL);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 0)),
                    ==, "Called clawtilla_task_list");

    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 1)),
                    ==, CLAWT_TEACH_STEP_EXEC);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 1)),
                    ==, "make release");

    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 2)),
                    ==, CLAWT_TEACH_STEP_DESKTOP);

    clawt_teach_recorder_stop(base, "done", NULL);
}

/*
 * An agent with no computer records steps and no frames, and the trace
 * says which of those it is.
 *
 * That is the whole of what `computer.type: none` means for teaching:
 * the recorder still works, there is simply no screen to photograph.
 */
static void
test_an_agent_with_no_screen_gets_steps_and_no_frames(void)
{
    g_autoptr(ClawtAgentTraceRecorder) recorder =
        make_agent_recorder("no-screen");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    ClawtTeachTrace *trace;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_teach_recorder_start(base, &error));
    g_assert_no_error(error);

    clawt_agent_trace_recorder_note_tool_call(recorder, "clawtilla_recall",
                                              "{\"query\":\"release\"}");

    trace = clawt_teach_recorder_get_trace(base);

    g_assert_cmpuint(clawt_teach_trace_get_steps(trace)->len, ==, 1);
    g_assert_cmpuint(clawt_teach_trace_count_frames(trace), ==, 0);

    /*
     * And it says what it can and cannot see. A task done mostly in the
     * agent's own shell leaves a short trace, and without this sentence
     * that reads as the agent having done very little.
     */
    g_assert_cmpuint(clawt_teach_trace_get_caveats(trace)->len, >=, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(
                                clawt_teach_trace_get_caveats(trace), 0),
                            "own file and shell tools"));

    clawt_teach_recorder_stop(base, "done", NULL);
}

/*
 * Nothing is recorded before start or after stop.
 *
 * The hooks that feed this are on the daemon's own paths and run
 * whether or not a recording exists, so the check has to be here rather
 * than at each of them.
 */
static void
test_nothing_is_recorded_outside_the_recording(void)
{
    g_autoptr(ClawtAgentTraceRecorder) recorder =
        make_agent_recorder("bounds");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    ClawtTeachTrace *trace = clawt_teach_recorder_get_trace(base);

    clawt_agent_trace_recorder_note_tool_call(recorder, "before", "{}");
    g_assert_cmpuint(clawt_teach_trace_get_steps(trace)->len, ==, 0);

    clawt_teach_recorder_start(base, NULL);
    clawt_agent_trace_recorder_note_tool_call(recorder, "during", "{}");
    clawt_teach_recorder_stop(base, "done", NULL);

    clawt_agent_trace_recorder_note_tool_call(recorder, "after", "{}");
    clawt_agent_trace_recorder_note_desktop(recorder, "mouse_click");

    g_assert_cmpuint(clawt_teach_trace_get_steps(trace)->len, ==, 1);
}

/*
 * The agent recorder's drain is an implementation, not the base's
 * refusal, and the difference is the point: there is genuinely nothing
 * buffered anywhere, because steps are pushed in as they happen. A
 * backend with nothing to do says so itself.
 */
static void
test_the_agent_recorder_drains_rather_than_refusing(void)
{
    g_autoptr(ClawtAgentTraceRecorder) recorder =
        make_agent_recorder("drainable");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    g_autoptr(GError) error = NULL;

    clawt_teach_recorder_start(base, NULL);

    g_assert_true(clawt_teach_recorder_drain(base, &error));
    g_assert_no_error(error);

    clawt_teach_recorder_stop(base, "done", NULL);
}

/* ── gowl's payloads ─────────────────────────────────────────────── */

static ClawtGowlDemoRecorder *
make_gowl_recorder(const gchar *id)
{
    g_autofree gchar *directory = scratch(id);

    return clawt_gowl_demo_recorder_new(id, directory, "/tmp/nowhere.sock");
}

/*
 * A gowl payload becomes steps -- and only the steps worth having.
 *
 * Releases are half of the keystroke that was already recorded, and
 * pointer motion is hundreds of events per drag: keeping either would
 * spend the event budget on noise and evict the clicks the
 * demonstration is about.
 */
static void
test_a_gowl_payload_becomes_steps(void)
{
    g_autoptr(ClawtGowlDemoRecorder) recorder = make_gowl_recorder("gowl");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    ClawtTeachTrace *trace = clawt_teach_recorder_get_trace(base);
    GPtrArray *steps;
    g_autoptr(GError) error = NULL;
    const gchar *payload =
        "{\"active\":true,\"dropped\":0,\"suppressed\":0,"
        "\"secret_suppression\":\"gowl cannot see inside a window.\","
        "\"events\":["
        "{\"type\":\"key\",\"state\":\"press\",\"keysym\":\"a\","
        " \"wall_us\":1788000000000000,\"offset_ms\":812.5},"
        "{\"type\":\"key\",\"state\":\"release\",\"keysym\":\"a\","
        " \"wall_us\":1788000000100000,\"offset_ms\":912.5},"
        "{\"type\":\"pointer_motion\",\"x\":842.0,\"y\":511.0,"
        " \"wall_us\":1788000000200000,\"offset_ms\":920.0},"
        "{\"type\":\"pointer_button\",\"state\":\"press\",\"button\":272,"
        " \"x\":842.0,\"y\":511.0,\"wall_us\":1788000000300000,"
        " \"offset_ms\":940.0},"
        "{\"type\":\"pointer_axis\",\"axis\":\"vertical\",\"x\":10.0,"
        " \"y\":20.0,\"wall_us\":1788000000400000,\"offset_ms\":1100.0},"
        "{\"type\":\"modifiers\",\"depressed\":64,\"offset_ms\":1200.0}"
        "]}";

    g_assert_true(clawt_gowl_demo_recorder_absorb(recorder, payload,
                                                   &error));
    g_assert_no_error(error);

    steps = clawt_teach_trace_get_steps(trace);

    g_assert_cmpuint(steps->len, ==, 3);
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 0)),
                    ==, CLAWT_TEACH_STEP_KEY);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 0)),
                    ==, "Key a");
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 1)),
                    ==, CLAWT_TEACH_STEP_POINTER);
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 2)),
                    ==, CLAWT_TEACH_STEP_SCROLL);

    /*
     * The compositor's own clock, kept as it arrived. Restamping would
     * make every event look as if it happened at the moment we drained
     * -- one clock standing in for another, which is the mistake both
     * upstream recorders were shaped to avoid.
     */
    g_assert_cmpint(clawt_teach_step_get_wall_us(g_ptr_array_index(steps, 0)),
                    ==, 1788000000000000LL);
}

/*
 * gowl repeats its caveat in every payload precisely so that it cannot
 * be lost between layers, and clawtilla says whatever gowl says rather
 * than keeping a copy that could go stale against it.
 */
static void
test_the_compositors_own_caveat_travels(void)
{
    g_autoptr(ClawtGowlDemoRecorder) recorder = make_gowl_recorder("gowl-cav");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    GPtrArray *caveats;

    g_assert_true(clawt_gowl_demo_recorder_absorb(
        recorder,
        "{\"events\":[],\"secret_suppression\":\"a sentence gowl wrote\"}",
        NULL));

    caveats = clawt_teach_trace_get_caveats(
        clawt_teach_recorder_get_trace(base));

    g_assert_cmpuint(caveats->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(caveats, 0), ==,
                    "a sentence gowl wrote");
}

/*
 * And a payload that carries no caveat still gets one.
 *
 * An older gowl, or one whose payload shape changed, must not produce a
 * trace that reads as having no limitation at all -- which is the one
 * way this feature does real harm.
 */
static void
test_a_payload_with_no_caveat_still_gets_one(void)
{
    g_autoptr(ClawtGowlDemoRecorder) recorder = make_gowl_recorder("gowl-nc");
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    GPtrArray *caveats;

    g_assert_true(clawt_gowl_demo_recorder_absorb(recorder, "{\"events\":[]}",
                                                   NULL));

    caveats = clawt_teach_trace_get_caveats(
        clawt_teach_recorder_get_trace(base));

    g_assert_cmpuint(caveats->len, ==, 1);
    g_assert_nonnull(strstr(g_ptr_array_index(caveats, 0), "IS recorded"));
}

static void
test_gowl_counters_are_carried(void)
{
    g_autoptr(ClawtGowlDemoRecorder) recorder = make_gowl_recorder("gowl-cnt");
    ClawtTeachTrace *trace =
        clawt_teach_recorder_get_trace(CLAWT_TEACH_RECORDER(recorder));

    g_assert_true(clawt_gowl_demo_recorder_absorb(
        recorder,
        "{\"events\":[],\"dropped\":12,\"suppressed\":5,"
        "\"stop_reason\":\"the deadline\"}", NULL));

    g_assert_cmpuint(clawt_teach_trace_get_dropped(trace), ==, 12);
    g_assert_cmpuint(clawt_teach_trace_get_suppressed(trace), ==, 5);
    g_assert_cmpstr(clawt_teach_trace_get_stop_reason(trace), ==,
                    "the deadline");
}

static void
test_a_payload_that_is_not_a_payload_is_refused(void)
{
    g_autoptr(ClawtGowlDemoRecorder) recorder = make_gowl_recorder("gowl-bad");
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_gowl_demo_recorder_absorb(recorder, "[1,2,3]",
                                                    &error));
    g_assert_nonnull(error);

    g_clear_error(&error);
    g_assert_false(clawt_gowl_demo_recorder_absorb(recorder, "", &error));
    g_assert_nonnull(error);
}

/* ── the guest's payloads ────────────────────────────────────────── */

static ClawtGuestDemoRecorder *
make_guest_recorder(const gchar *id, ClawtComputer *computer)
{
    g_autofree gchar *directory = scratch(id);

    return clawt_guest_demo_recorder_new(id, directory, "builder", computer);
}

/*
 * A guest demonstration needs a guest.
 *
 * Refused when the recorder is built rather than when it is started: a
 * recorder that looked armed and then failed at the moment somebody was
 * ready to demonstrate would fail at the worst possible time.
 */
static void
test_a_guest_demonstration_needs_a_vm(void)
{
    g_autoptr(ClawtComputer) none = clawt_null_computer_new("builder");
    g_autoptr(ClawtComputer) vm =
        clawt_vm_computer_new("builder", CLAWT_VM_BACKEND_QEMU, NULL);
    ClawtGuestDemoRecorder *ok;

    g_assert_null(make_guest_recorder("guest-none", none));
    g_assert_null(make_guest_recorder("guest-null", NULL));

    ok = make_guest_recorder("guest-vm", vm);
    g_assert_nonnull(ok);
    g_object_unref(ok);
}

/*
 * The extension's events become steps, and its suppression markers are
 * kept as steps of their own.
 *
 * A trace that simply omitted eight seconds reads as somebody doing
 * nothing, and a model would write a skill with a hole it cannot see.
 */
static void
test_guest_events_become_steps_and_markers(void)
{
    g_autoptr(ClawtComputer) vm =
        clawt_vm_computer_new("builder", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtGuestDemoRecorder) recorder =
        make_guest_recorder("guest-events", vm);
    ClawtTeachRecorder *base = CLAWT_TEACH_RECORDER(recorder);
    ClawtTeachTrace *trace = clawt_teach_recorder_get_trace(base);
    GPtrArray *steps;
    g_autoptr(GError) error = NULL;
    const gchar *events =
        "["
        "{\"type\":\"key_press\",\"keyval\":97,\"unicode\":97,"
        " \"wall_us\":1788000000000000,\"offset_us\":12345},"
        "{\"type\":\"key_release\",\"keyval\":97,\"unicode\":97,"
        " \"wall_us\":1788000000010000,\"offset_us\":22345},"
        "{\"type\":\"motion\",\"x\":10,\"y\":20,\"offset_us\":23000},"
        "{\"type\":\"button_press\",\"x\":842,\"y\":511,"
        " \"wall_us\":1788000000020000,\"offset_us\":32345},"
        "{\"type\":\"suppressed\",\"reason\":\"a password entry has "
        "focus\",\"wall_us\":1788000000030000,\"offset_us\":42345},"
        "{\"type\":\"resumed\",\"wall_us\":1788000000040000,"
        " \"offset_us\":52345}"
        "]";

    g_assert_true(clawt_guest_demo_recorder_absorb(recorder, events, 3,
                                                    &error));
    g_assert_no_error(error);

    steps = clawt_teach_trace_get_steps(trace);

    g_assert_cmpuint(steps->len, ==, 4);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 0)),
                    ==, "Key a");
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 1)),
                    ==, CLAWT_TEACH_STEP_POINTER);
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 2)),
                    ==, CLAWT_TEACH_STEP_MARKER);
    g_assert_nonnull(strstr(clawt_teach_step_get_label(
                                g_ptr_array_index(steps, 2)),
                            "password entry"));
    g_assert_cmpint(clawt_teach_step_get_kind(g_ptr_array_index(steps, 3)),
                    ==, CLAWT_TEACH_STEP_MARKER);

    /* The ring's losses and the pause are both counted. */
    g_assert_cmpuint(clawt_teach_trace_get_dropped(trace), ==, 3);
    g_assert_cmpuint(clawt_teach_trace_get_suppressed(trace), ==, 1);

    /* Captured time, kept as captured. */
    g_assert_cmpint(clawt_teach_step_get_offset_us(
                        g_ptr_array_index(steps, 0)), ==, 12345);
}

/*
 * A drain during a quiet moment answers with an empty list, and
 * treating that as a failure would end a demonstration because somebody
 * paused to read something.
 */
static void
test_an_empty_guest_drain_is_not_an_error(void)
{
    g_autoptr(ClawtComputer) vm =
        clawt_vm_computer_new("builder", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtGuestDemoRecorder) recorder =
        make_guest_recorder("guest-quiet", vm);
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_guest_demo_recorder_absorb(recorder, "", 2, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_guest_demo_recorder_absorb(recorder, "[]", 0,
                                                    &error));
    g_assert_no_error(error);

    /* The losses still counted, even though there were no events. */
    g_assert_cmpuint(clawt_teach_trace_get_dropped(
                         clawt_teach_recorder_get_trace(
                             CLAWT_TEACH_RECORDER(recorder))), ==, 2);
}

/*
 * A control codepoint is never written into a label.
 *
 * The label goes into a JSON file, into a chat window and into a
 * terminal, and a bell or a backspace arriving in any of those is a
 * step nobody can read and a terminal that misbehaves.
 */
static void
test_a_control_key_is_named_not_printed(void)
{
    g_autoptr(ClawtComputer) vm =
        clawt_vm_computer_new("builder", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtGuestDemoRecorder) recorder =
        make_guest_recorder("guest-ctrl", vm);
    GPtrArray *steps;

    g_assert_true(clawt_guest_demo_recorder_absorb(
        recorder,
        "[{\"type\":\"key_press\",\"keyval\":65307,\"unicode\":27}]", 0,
        NULL));

    steps = clawt_teach_trace_get_steps(
        clawt_teach_recorder_get_trace(CLAWT_TEACH_RECORDER(recorder)));

    g_assert_cmpuint(steps->len, ==, 1);
    g_assert_cmpstr(clawt_teach_step_get_label(g_ptr_array_index(steps, 0)),
                    ==, "Key (keyval 65307)");
}

/* ── The gdbus commands, and reading what they print ─────────────── */

static gchar *
argv_string(GStrv argv)
{
    return g_strjoinv(" ", argv);
}

static void
test_the_recording_argv_names_the_right_methods(void)
{
    g_auto(GStrv) start = clawt_screen_gnome_record_start_argv(900, 20000);
    g_auto(GStrv) drain = clawt_screen_gnome_record_drain_argv("tok-1");
    g_auto(GStrv) stop = clawt_screen_gnome_record_stop_argv("tok-1");
    g_auto(GStrv) status = clawt_screen_gnome_record_status_argv();
    g_autofree gchar *start_line = argv_string(start);
    g_autofree gchar *drain_line = argv_string(drain);
    g_autofree gchar *stop_line = argv_string(stop);
    g_autofree gchar *status_line = argv_string(status);

    g_assert_nonnull(strstr(start_line, ".StartRecording"));
    g_assert_nonnull(strstr(start_line, " 900 20000"));
    g_assert_nonnull(strstr(drain_line, ".DrainRecording"));
    g_assert_nonnull(strstr(drain_line, "tok-1"));
    g_assert_nonnull(strstr(stop_line, ".StopRecording"));
    g_assert_nonnull(strstr(status_line, ".GetRecordingStatus"));

    /*
     * The screenshot method is not one of these. It is an observing
     * tool and must never end up behind the recording grant.
     */
    g_assert_null(strstr(start_line, "ScreenshotFrame"));
}

/*
 * `gdbus` writes GVariant text with type annotations, and the `32` in
 * `uint32` is a digit run. A scan for digits reads that as the value --
 * which is exactly how an earlier version of the frame parser reported
 * every frame as 32 pixels wide while looking perfectly plausible.
 */
static void
test_the_events_tuple_is_parsed_not_scanned(void)
{
    g_autofree gchar *events = NULL;
    guint dropped = 0;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_screen_parse_gdbus_events(
        "('[{\"type\":\"key_press\"}]', uint32 7)", &events, &dropped,
        &error));
    g_assert_no_error(error);

    g_assert_cmpstr(events, ==, "[{\"type\":\"key_press\"}]");
    g_assert_cmpuint(dropped, ==, 7);
}

/*
 * What arrives here instead of a tuple is a gdbus error message, and a
 * lenient reader would take the first quoted fragment of an exception
 * as a list of events and hand it to a JSON parser a long way from
 * here.
 */
static void
test_a_gdbus_error_is_not_read_as_events(void)
{
    g_autofree gchar *events = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_screen_parse_gdbus_events(
        "Error: GDBus.Error:io.github.gnomemcp.RecordingConsentError: "
        "'recording-consent' is off", &events, NULL, &error));
    g_assert_nonnull(error);
    g_assert_null(events);

    g_clear_error(&error);
    g_assert_false(clawt_screen_parse_gdbus_events(NULL, &events, NULL,
                                                   &error));
    g_assert_nonnull(error);
}

/* ── The synthesizer ─────────────────────────────────────────────── */

static ClawtSkillLibrary *
make_library(const gchar *name)
{
    g_autofree gchar *directory = scratch(name);
    ClawtSkillLibrary *library;

    g_mkdir_with_parents(directory, 0700);
    library = clawt_skill_library_new(directory);
    clawt_skill_library_scan(library);

    return library;
}

static ClawtTeachTrace *
make_demo_trace(void)
{
    ClawtTeachTrace *trace =
        clawt_teach_trace_new("demo", CLAWT_TEACH_SOURCE_HOST_DEMO);

    clawt_teach_trace_set_goal(trace, "cut a release");
    clawt_teach_trace_add_caveat(trace, CLAWT_TEACH_HOST_DEMO_CAVEAT);
    clawt_teach_trace_add_step(
        trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, "make clean all"));
    clawt_teach_trace_add_step(
        trace, clawt_teach_step_new(CLAWT_TEACH_STEP_EXEC, "make test"));

    return trace;
}

static void
test_the_synthesizer_writes_a_draft(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-draft");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *preview = NULL;
    GHashTable *draft;

    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"release\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_description",
        "{\"description\":\"Use this when cutting a release.\"}");
    ai_mock_provider_push_tool_use(
        provider, "write_body",
        "{\"body\":\"Run make clean all, then make test.\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Wrote the release skill.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));

    draft = clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(draft);

    g_assert_cmpstr(g_hash_table_lookup(draft, "name"), ==, "release");

    /*
     * The preview is the rendered file, not a summary of it: the scan
     * runs over exactly these bytes, and a preview of something tidier
     * would be a preview of a different document.
     */
    preview = clawt_skill_synthesizer_preview(synth);
    g_assert_true(g_str_has_prefix(preview, "---\n"));
    g_assert_nonnull(strstr(preview, "name: release"));
    g_assert_nonnull(strstr(preview, "Run make clean all"));
}

/*
 * A draft that will not validate is refused, and the reason goes back
 * to the *model* while it still has a turn left.
 *
 * Discovering it at commit would mean running the whole conversation
 * again, which is a model call somebody pays for twice.
 */
static void
test_an_invalid_draft_is_fed_back_to_the_model(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-feedback");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;
    GHashTable *draft;

    /* Commits with no body at all -- the tool has to refuse. */
    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"release\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_description",
        "{\"description\":\"Use this when cutting a release.\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    /* ... and then, told what was missing, writes it and commits. */
    ai_mock_provider_push_tool_use(
        provider, "write_body", "{\"body\":\"Run make clean all.\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Fixed it.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));

    draft = clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(draft);

    /*
     * The refusal did not end the loop: the model went on and finished
     * the draft. If the first commit had been accepted there would be
     * no body here.
     */
    g_assert_nonnull(g_hash_table_lookup(draft, "body"));
}

/*
 * And the same check again at commit, rather than trusting the tool's.
 *
 * The draft is reachable between the two -- a second synthesis, an edit
 * in a client -- and a check at one call site is a check about that
 * call site.
 */
static void
test_commit_validates_again(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-recheck");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;

    /* Names it and stops: no description, no body. */
    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"halfdone\"}");
    ai_mock_provider_push_text(provider, "That is all I have.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));
    clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);
    g_clear_error(&error);

    g_assert_null(clawt_skill_synthesizer_commit(synth, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "body"));
}

/*
 * The draft lands **disabled**, with its provenance recorded.
 *
 * A skill clawtilla wrote is not more trusted than one somebody
 * downloaded: the model that wrote it read a trace, and a trace of a
 * demonstration is untrusted input the moment it contains anything a
 * person typed.
 */
static void
test_a_committed_draft_lands_disabled(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-disabled");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;
    ClawtSkill *skill;

    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"release\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_description",
        "{\"description\":\"Use this when cutting a release.\"}");
    ai_mock_provider_push_tool_use(
        provider, "write_body", "{\"body\":\"Run make clean all.\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));
    clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);
    g_assert_no_error(error);

    skill = clawt_skill_synthesizer_commit(synth, &error);
    g_assert_no_error(error);
    g_assert_nonnull(skill);

    g_assert_false(clawt_skill_get_enabled(skill));
    g_assert_cmpint(clawt_skill_get_source(skill), ==,
                    CLAWT_SKILL_SOURCE_TAUGHT);
    g_assert_cmpstr(clawt_skill_get_origin_url(skill), ==, "teach:demo");

    /* And it is really in the library, read back off disk. */
    clawt_skill_library_scan(library);
    g_assert_nonnull(clawt_skill_library_lookup(library, "release"));
    g_assert_false(clawt_skill_get_enabled(
        clawt_skill_library_lookup(library, "release")));
}

/*
 * The same scan an imported skill gets, applied to a synthesized one.
 *
 * A model that copied an invisible-Unicode payload out of a trace and
 * into a skill is exactly the case the scan exists for, and the
 * synthesized path must not be the one that skipped it.
 */
static void
test_a_committed_draft_is_scanned(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-scanned");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;
    ClawtSkill *skill;

    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"sneaky\"}");
    ai_mock_provider_push_tool_use(
        provider, "set_description",
        "{\"description\":\"Use this when doing the thing.\"}");
    /* A zero-width space the reviewer cannot see and the model reads. */
    ai_mock_provider_push_tool_use(
        provider, "write_body",
        "{\"body\":\"Do the thing.\\u200bThen do the other thing.\"}");
    ai_mock_provider_push_tool_use(provider, "commit", "{}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));
    clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);
    g_assert_no_error(error);

    skill = clawt_skill_synthesizer_commit(synth, &error);
    g_assert_no_error(error);
    g_assert_nonnull(skill);

    g_assert_cmpuint(clawt_skill_get_warnings(skill)->len, >=, 1);
}

/*
 * A name is the traversal gate wherever it arrives from, including from
 * a model.
 */
static void
test_the_model_cannot_name_a_skill_anything(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-traversal");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;

    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"../../etc/cron.d/x\"}");
    ai_mock_provider_push_text(provider, "I tried.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));

    g_assert_null(clawt_skill_synthesizer_synthesize(synth, trace, NULL,
                                                      &error));
    g_assert_nonnull(error);
}

/*
 * The executor is built with ai_tool_executor_new_empty(), so there is
 * no bash, read, write or edit to reach.
 *
 * It matters more here than in the designer: this model has just been
 * handed a transcript of somebody's keyboard.
 */
static void
test_the_synthesizer_has_no_shell(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-noshell");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *marker = scratch("clawt-teach-pwned");
    g_autofree gchar *command = NULL;

    command = g_strdup_printf("{\"command\":\"id > %s\"}", marker);

    ai_mock_provider_push_tool_use(provider, "bash", command);
    ai_mock_provider_push_tool_use(provider, "set_name",
                                   "{\"name\":\"harmless\"}");
    ai_mock_provider_push_text(provider, "Done.");

    clawt_skill_synthesizer_set_provider(synth, AI_PROVIDER(provider));
    clawt_skill_synthesizer_synthesize(synth, trace, NULL, &error);

    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
}

/*
 * No provider is a message naming the setting, not a crash and not a
 * silent nothing.
 */
static void
test_no_provider_says_what_to_set(void)
{
    g_autoptr(ClawtSkillLibrary) library = make_library("lib-noprov");
    g_autoptr(ClawtSkillSynthesizer) synth =
        clawt_skill_synthesizer_new(library, NULL);
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_skill_synthesizer_synthesize(synth, trace, NULL,
                                                      &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "ai_assist.provider"));
}

/*
 * The caveat reaches the model's system prompt.
 *
 * It is the sentence that tells it not to copy a credential out of a
 * recording it may not know contains one, and a prompt that lost it
 * would be the layer this whole feature is careful about.
 */
static void
test_the_trace_rendering_carries_the_caveat(void)
{
    g_autoptr(ClawtTeachTrace) trace = make_demo_trace();
    g_autofree gchar *rendered = clawt_teach_trace_render(trace, 0);

    g_assert_nonnull(strstr(rendered, "Caveat:"));
    g_assert_nonnull(strstr(rendered, "IS recorded"));
    g_assert_nonnull(strstr(rendered, "cut a release"));
}

int
main(int argc, char *argv[])
{
    gint status;

    test_root = g_dir_make_tmp("clawt-teach-XXXXXX", NULL);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/teach/steps-in-order", test_steps_are_kept_in_order);
    g_test_add_func("/teach/round-trip", test_a_trace_round_trips_through_disk);
    g_test_add_func("/teach/caveat-once", test_a_caveat_is_recorded_once);
    g_test_add_func("/teach/frame-is-a-name", test_a_frame_is_a_name_not_a_path);
    g_test_add_func("/teach/render-truncated",
                    test_the_rendering_says_when_it_is_truncated);
    g_test_add_func("/teach/id-gate", test_a_recording_id_is_the_traversal_gate);

    g_test_add_func("/teach/missing-vfunc-refuses",
                    test_a_missing_vfunc_refuses_naming_the_type);
    g_test_add_func("/teach/event-bound",
                    test_the_event_bound_keeps_the_earliest_and_counts_the_rest);
    g_test_add_func("/teach/zero-limits", test_zero_limits_mean_the_defaults);
    g_test_add_func("/teach/vanished-backend",
                    test_a_vanished_backend_still_ends_the_recording);

    g_test_add_func("/teach/agent-steps",
                    test_an_agent_trace_records_calls_and_commands);
    g_test_add_func("/teach/agent-no-screen",
                    test_an_agent_with_no_screen_gets_steps_and_no_frames);
    g_test_add_func("/teach/agent-bounds",
                    test_nothing_is_recorded_outside_the_recording);
    g_test_add_func("/teach/agent-drains",
                    test_the_agent_recorder_drains_rather_than_refusing);

    g_test_add_func("/teach/gowl-steps", test_a_gowl_payload_becomes_steps);
    g_test_add_func("/teach/gowl-caveat",
                    test_the_compositors_own_caveat_travels);
    g_test_add_func("/teach/gowl-caveat-fallback",
                    test_a_payload_with_no_caveat_still_gets_one);
    g_test_add_func("/teach/gowl-counters", test_gowl_counters_are_carried);
    g_test_add_func("/teach/gowl-bad-payload",
                    test_a_payload_that_is_not_a_payload_is_refused);

    g_test_add_func("/teach/guest-needs-a-vm",
                    test_a_guest_demonstration_needs_a_vm);
    g_test_add_func("/teach/guest-steps",
                    test_guest_events_become_steps_and_markers);
    g_test_add_func("/teach/guest-empty-drain",
                    test_an_empty_guest_drain_is_not_an_error);
    g_test_add_func("/teach/guest-control-key",
                    test_a_control_key_is_named_not_printed);

    g_test_add_func("/teach/record-argv",
                    test_the_recording_argv_names_the_right_methods);
    g_test_add_func("/teach/events-tuple",
                    test_the_events_tuple_is_parsed_not_scanned);
    g_test_add_func("/teach/gdbus-error",
                    test_a_gdbus_error_is_not_read_as_events);

    g_test_add_func("/teach/synthesize", test_the_synthesizer_writes_a_draft);
    g_test_add_func("/teach/invalid-fed-back",
                    test_an_invalid_draft_is_fed_back_to_the_model);
    g_test_add_func("/teach/commit-revalidates", test_commit_validates_again);
    g_test_add_func("/teach/lands-disabled",
                    test_a_committed_draft_lands_disabled);
    g_test_add_func("/teach/is-scanned", test_a_committed_draft_is_scanned);
    g_test_add_func("/teach/name-gate",
                    test_the_model_cannot_name_a_skill_anything);
    g_test_add_func("/teach/no-shell", test_the_synthesizer_has_no_shell);
    g_test_add_func("/teach/no-provider", test_no_provider_says_what_to_set);
    g_test_add_func("/teach/rendering-caveat",
                    test_the_trace_rendering_carries_the_caveat);

    /*
     * Made before g_test_init() runs anything and taken away after, so
     * a run leaves nothing behind wherever it happened to start -- and
     * a *failing* run's directory stays, because that is evidence.
     */
    status = g_test_run();

    if (status == 0)
        clawt_test_remove_tree(test_root);

    g_clear_pointer(&test_root, g_free);

    return status;
}
