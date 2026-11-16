/*
 * test-desktop-relay.c - The policy in front of the guest's MCP server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * gnome-desktop-mcp offers every tool it has to whoever connects; it has
 * never heard of computer.desktop.allow_input.  The relay is the only
 * place that grant is enforced for a guest desktop, so a hole here is a
 * silent widening of it -- an observe-only agent quietly able to type.
 */

#include <clawtilla.h>

#include <string.h>

#include <json-glib/json-glib.h>

#include "clawt-test-util.h"

/* An observe-only agent: it may look, and it may not touch. */
static GStrv
observing_only(void)
{
    const gchar *tools[] = { "screenshot", "list_windows", NULL };

    return g_strdupv((GStrv)tools);
}

static JsonObject *
parse(const gchar *text)
{
    JsonParser *parser = json_parser_new();
    JsonObject *object;

    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));

    object = json_object_ref(
        json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser);

    return object;
}

static void
test_a_permitted_call_goes_through(void)
{
    g_auto(GStrv) permitted = observing_only();
    g_autofree gchar *refusal = NULL;
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"screenshot\",\"arguments\":{}}}";

    g_assert_true(clawt_desktop_relay_filter_outbound(line, permitted,
                                                      &refusal));
    g_assert_null(refusal);
}

/*
 * The whole reason this file exists.  Forwarding it would run it.
 */
static void
test_a_refused_call_is_not_forwarded(void)
{
    g_auto(GStrv) permitted = observing_only();
    g_autofree gchar *refusal = NULL;
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"send_key\",\"arguments\":{\"key\":\"a\"}}}";

    g_assert_false(clawt_desktop_relay_filter_outbound(line, permitted,
                                                       &refusal));
    g_assert_nonnull(refusal);

    {
        g_autoptr(JsonObject) reply = parse(refusal);
        JsonObject *error;

        /*
         * The id has to come back, and come back as the same JSON type.
         * A client matches replies to requests by it, and one that never
         * arrives is a client waiting for ever rather than one told no.
         */
        g_assert_true(json_object_has_member(reply, "id"));
        g_assert_cmpint(json_object_get_int_member(reply, "id"), ==, 7);

        g_assert_true(json_object_has_member(reply, "error"));
        error = json_object_get_object_member(reply, "error");

        /* And it says which grant would have allowed it. */
        g_assert_nonnull(strstr(
            json_object_get_string_member(error, "message"), "send_key"));
        g_assert_nonnull(strstr(
            json_object_get_string_member(error, "message"), "allow_input"));
    }
}

/*
 * A notification expects no answer, so there is nothing to refuse it
 * with.  Inventing a reply with no id to match would be a protocol error
 * of our own.
 */
static void
test_a_refused_notification_is_dropped_silently(void)
{
    g_auto(GStrv) permitted = observing_only();
    g_autofree gchar *refusal = NULL;
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"send_key\"}}";

    g_assert_false(clawt_desktop_relay_filter_outbound(line, permitted,
                                                       &refusal));
    g_assert_null(refusal);
}

/*
 * Everything that is not a tool call is somebody else's protocol.
 */
static void
test_other_methods_pass_untouched(void)
{
    g_auto(GStrv) permitted = observing_only();
    const gchar *lines[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
        NULL
    };
    gsize i;

    for (i = 0; lines[i] != NULL; i++)
        g_assert_true(clawt_desktop_relay_filter_outbound(lines[i], permitted,
                                                          NULL));
}

/*
 * A message neither end has told us about is far more likely to be a
 * newer MCP than an attack, and swallowing it surfaces as the client
 * hanging on a reply that never comes.
 */
static void
test_unparseable_input_is_passed_on(void)
{
    g_auto(GStrv) permitted = observing_only();

    g_assert_true(clawt_desktop_relay_filter_outbound("not json at all",
                                                      permitted, NULL));
}

/*
 * A batch is not an unknown dialect, and this test used to say it was.
 *
 * It asserted that "[1, 2, 3]" is passed through, under a comment about
 * forward compatibility -- which is true of a shape that is not JSON and
 * false of a JSON array, because an array is where JSON-RPC puts
 * requests.  The filter looked for a top-level object, did not find one,
 * and let the line go to the child verbatim; so wrapping a refused
 * tools/call in two characters was the whole exploit, against the only
 * place allow_input is enforced for a guest desktop and the only place a
 * connector's tools: grant is enforced at all.
 *
 * The intention now is that a batch is refused whatever is in it, since
 * neither user of this relay needs batching and a filter that has to be
 * right about nested shapes will be wrong about one of them.
 */
static void
test_a_batch_is_refused(void)
{
    g_auto(GStrv) permitted = observing_only();
    g_autofree gchar *refusal = NULL;
    g_autofree gchar *harmless = NULL;
    const gchar *batched_click =
        "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"click\",\"arguments\":{\"x\":1}}}]";

    g_assert_false(clawt_desktop_relay_filter_outbound(batched_click,
                                                       permitted, &refusal));

    /*
     * With an answer, so the agent is told rather than left waiting.  A
     * batch has no single id to reply to, so JSON-RPC's null-id form is
     * what it gets.
     */
    g_assert_nonnull(refusal);
    g_assert_nonnull(strstr(refusal, "batched"));

    /*
     * Even a batch holding nothing objectionable: the point is that the
     * filter cannot vouch for what is inside one.
     */
    g_assert_false(clawt_desktop_relay_filter_outbound(
        "[{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}]",
        permitted, &harmless));

    g_assert_false(clawt_desktop_relay_filter_outbound("[1, 2, 3]",
                                                       permitted, NULL));
}

/*
 * An empty permission list refuses everything, rather than permitting it.
 * This is the case a bug arrives at -- a daemon that answered with no
 * tools, a field that did not parse -- and it has to fail closed.
 */
static void
test_no_permitted_tools_refuses_everything(void)
{
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"screenshot\"}}";

    g_assert_false(clawt_desktop_relay_filter_outbound(line, NULL, NULL));
}

/*
 * A tool that is advertised and then refused is worse than one that is
 * absent: the agent plans around it, calls it, and has to work out from
 * an error that it never had it.
 */
static void
test_tools_list_loses_the_refused_ones(void)
{
    g_auto(GStrv) permitted = observing_only();
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
        "{\"name\":\"screenshot\"},"
        "{\"name\":\"send_key\"},"
        "{\"name\":\"list_windows\"},"
        "{\"name\":\"spawn\"}]}}";
    g_autofree gchar *filtered =
        clawt_desktop_relay_filter_inbound(line, permitted);
    g_autoptr(JsonObject) reply = parse(filtered);
    JsonArray *tools;

    tools = json_object_get_array_member(
        json_object_get_object_member(reply, "result"), "tools");

    g_assert_cmpuint(json_array_get_length(tools), ==, 2);
    g_assert_null(strstr(filtered, "send_key"));
    g_assert_null(strstr(filtered, "spawn"));
    g_assert_nonnull(strstr(filtered, "screenshot"));
    g_assert_nonnull(strstr(filtered, "list_windows"));
}

/*
 * A result with nothing to remove is passed on byte for byte.  Re-rendering
 * it would reorder members and reformat numbers for no reason, and this
 * sits in the middle of a protocol that is not ours.
 */
static void
test_an_untouched_result_is_not_rewritten(void)
{
    const gchar *tools[] = { "screenshot", NULL };
    g_auto(GStrv) permitted = g_strdupv((GStrv)tools);
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
        "{\"name\":\"screenshot\",\"description\":\"take one\"}]}}";
    g_autofree gchar *filtered =
        clawt_desktop_relay_filter_inbound(line, permitted);

    g_assert_cmpstr(filtered, ==, line);
}

static void
test_a_reply_that_is_not_a_tool_list_is_untouched(void)
{
    g_auto(GStrv) permitted = observing_only();
    const gchar *line =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"content\":"
        "[{\"type\":\"text\",\"text\":\"send_key\"}]}}";
    g_autofree gchar *filtered =
        clawt_desktop_relay_filter_inbound(line, permitted);

    /*
     * Named a refused tool in its *text*, which is not the same thing as
     * offering it.  A filter matching on the whole line rather than on
     * the tool entries would corrupt a screenshot's caption.
     */
    g_assert_cmpstr(filtered, ==, line);
}

/*
 * The argv is a pure function so it can be asserted on without a
 * hypervisor, an SSH connection or a GNOME session.
 */
static void
test_no_desktop_means_no_command(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);
    g_auto(GStrv) argv =
        clawt_vm_computer_build_desktop_argv(CLAWT_VM_COMPUTER(computer));

    /*
     * NULL rather than a command that dials nowhere: the relay reports
     * that the VM cannot be reached, instead of starting an ssh that
     * fails a few seconds later inside somebody else's protocol.
     */
    g_assert_null(argv);
}

static void
test_the_command_reaches_the_session_account(void)
{
    g_autoptr(ClawtComputer) computer =
        clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);
    g_autoptr(ClawtGuestDesktop) desktop = clawt_guest_desktop_new("clawt");
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *joined = NULL;

    clawt_vm_computer_set_ssh(CLAWT_VM_COMPUTER(computer), "root", NULL,
                              "127.0.0.1", 2222);
    clawt_vm_computer_set_desktop(CLAWT_VM_COMPUTER(computer), desktop);

    argv = clawt_vm_computer_build_desktop_argv(CLAWT_VM_COMPUTER(computer));
    g_assert_nonnull(argv);

    joined = g_strjoinv(" ", argv);

    /*
     * As the session account, not as the login commands run as.  The MCP
     * server needs the session bus of whoever is logged in at the screen,
     * and GDM will not log root in -- so root is by definition not it.
     */
    g_assert_nonnull(strstr(joined, "clawt@127.0.0.1"));
    g_assert_null(strstr(joined, "root@"));

    /*
     * -T, because this carries JSON-RPC frames rather than a session for
     * a person: a pty would translate line endings and act on control
     * characters in the middle of a message.
     */
    g_assert_nonnull(strstr(joined, " -T "));

    /* One bare word, so nothing has to survive three layers of quoting. */
    g_assert_cmpstr(argv[g_strv_length(argv) - 1], ==,
                    CLAWT_GUEST_DESKTOP_LAUNCHER);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/desktop-relay/outbound/permitted",
                    test_a_permitted_call_goes_through);
    g_test_add_func("/desktop-relay/outbound/refused",
                    test_a_refused_call_is_not_forwarded);
    g_test_add_func("/desktop-relay/outbound/refused-notification",
                    test_a_refused_notification_is_dropped_silently);
    g_test_add_func("/desktop-relay/outbound/other-methods",
                    test_other_methods_pass_untouched);
    g_test_add_func("/desktop-relay/outbound/unparseable",
                    test_unparseable_input_is_passed_on);
    g_test_add_func("/desktop-relay/outbound/batch", test_a_batch_is_refused);
    g_test_add_func("/desktop-relay/outbound/fails-closed",
                    test_no_permitted_tools_refuses_everything);
    g_test_add_func("/desktop-relay/inbound/tools-list-filtered",
                    test_tools_list_loses_the_refused_ones);
    g_test_add_func("/desktop-relay/inbound/untouched-is-verbatim",
                    test_an_untouched_result_is_not_rewritten);
    g_test_add_func("/desktop-relay/inbound/other-results",
                    test_a_reply_that_is_not_a_tool_list_is_untouched);
    g_test_add_func("/desktop-relay/argv/none-without-a-desktop",
                    test_no_desktop_means_no_command);
    g_test_add_func("/desktop-relay/argv/session-account",
                    test_the_command_reaches_the_session_account);

    return g_test_run();
}
