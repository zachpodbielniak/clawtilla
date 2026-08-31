/*
 * test-enums.c - Enumeration registration and nickname round-trips
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Nicknames are the spelling used in config files and on the wire, so a
 * missing or misspelled one is a compatibility break rather than a cosmetic
 * problem.  These tests exist to make that break loud.
 */

#include <clawtilla.h>

/**
 * test_enum_round_trip:
 *
 * Every value of every registered enum survives value -> nick -> value.
 * A value with no registered nickname would silently format as NULL and
 * then fail to parse back, which is exactly the failure mode a config file
 * makes hardest to diagnose.
 */
static void
test_enum_round_trip(void)
{
    GType types[] = {
        CLAWT_TYPE_AGENT_STATE, CLAWT_TYPE_RUNTIME_TYPE,
        CLAWT_TYPE_RESTART_POLICY, CLAWT_TYPE_COMPUTER_TYPE,
        CLAWT_TYPE_COMPUTER_STATE, CLAWT_TYPE_VM_BACKEND,
        CLAWT_TYPE_CONFINE_MODE, CLAWT_TYPE_DESKTOP_BACKEND,
        CLAWT_TYPE_MOUNT_TYPE, CLAWT_TYPE_MOUNT_MODE, CLAWT_TYPE_RELABEL,
        CLAWT_TYPE_MAILBOX_STATE, CLAWT_TYPE_PRIORITY,
        CLAWT_TYPE_OVERFLOW_POLICY, CLAWT_TYPE_TASK_STATE,
        CLAWT_TYPE_SECRET_BACKEND, CLAWT_TYPE_LOG_LEVEL
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(types); i++) {
        g_autoptr(GEnumClass) klass = g_type_class_ref(types[i]);
        guint v;

        g_assert_nonnull(klass);
        g_assert_cmpuint(klass->n_values, >, 0);

        for (v = 0; v < klass->n_values; v++) {
            const gchar *nick;
            gint parsed = -1;

            nick = clawt_enum_to_nick(types[i], klass->values[v].value);
            g_assert_nonnull(nick);
            g_assert_cmpstr(nick, !=, "");

            g_assert_true(clawt_enum_from_nick(types[i], nick, &parsed));
            g_assert_cmpint(parsed, ==, klass->values[v].value);
        }
    }
}

/**
 * test_enum_from_nick_rejects_unknown:
 *
 * An unrecognised nickname must fail rather than fall through to zero.
 * Falling through to zero is how a typo in a config file turns into a
 * silently wrong default -- `confine: bwarp` quietly meaning "none" would
 * be a security bug, not a cosmetic one.
 */
static void
test_enum_from_nick_rejects_unknown(void)
{
    gint value = 12345;

    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "bwarp", &value));
    g_assert_cmpint(value, ==, 12345);

    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "", &value));
    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, NULL, &value));

    /* A near-miss is still a miss.  This is the case that matters: if
     * "bwarp" parsed as zero, a typo would silently drop a sandbox. */
    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "bwrap ", &value));
    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "b", &value));
    g_assert_cmpint(value, ==, 12345);
}

/**
 * test_enum_nick_is_case_insensitive:
 *
 * Nicknames are typed by hand into YAML, so any case is accepted -- and we
 * do the comparison ourselves rather than relying on GLib, whose own
 * g_enum_get_value_by_nick() case policy has changed between versions.
 * Leaning on it would mean a config file that parses on one machine and is
 * rejected on another.
 */
static void
test_enum_nick_is_case_insensitive(void)
{
    gint value = -1;

    g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "BWRAP", &value));
    g_assert_cmpint(value, ==, CLAWT_CONFINE_BWRAP);

    g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE, "BwRaP", &value));
    g_assert_cmpint(value, ==, CLAWT_CONFINE_BWRAP);

    /* Whatever case came in, what we emit is canonical. */
    g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_CONFINE_MODE, value), ==, "bwrap");
}

/**
 * test_enum_accepts_c_identifier:
 *
 * A value pasted out of a header or a log line should still parse.
 */
static void
test_enum_accepts_c_identifier(void)
{
    gint value = -1;

    g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_CONFINE_MODE,
                                       "CLAWT_CONFINE_BWRAP", &value));
    g_assert_cmpint(value, ==, CLAWT_CONFINE_BWRAP);
}

/**
 * test_flags_to_string:
 *
 * Capability sets format as a pipe-separated nickname list, and an empty
 * set formats as "none" rather than an empty string.
 */
static void
test_flags_to_string(void)
{
    g_autofree gchar *empty = NULL;
    g_autofree gchar *one = NULL;
    g_autofree gchar *several = NULL;

    empty = clawt_flags_to_string(CLAWT_TYPE_AGENT_CAPS, CLAWT_AGENT_CAPS_NONE);
    g_assert_cmpstr(empty, ==, "none");

    one = clawt_flags_to_string(CLAWT_TYPE_AGENT_CAPS, CLAWT_AGENT_CAPS_COMPUTER);
    g_assert_cmpstr(one, ==, "computer");

    several = clawt_flags_to_string(CLAWT_TYPE_AGENT_CAPS,
                                    CLAWT_AGENT_CAPS_COMPUTER |
                                    CLAWT_AGENT_CAPS_HOST_CONTROL |
                                    CLAWT_AGENT_CAPS_PEER_COMMS);
    g_assert_nonnull(strstr(several, "computer"));
    g_assert_nonnull(strstr(several, "host-control"));
    g_assert_nonnull(strstr(several, "peer-comms"));
}

/**
 * test_no_capability_is_permanently_false:
 *
 * `images` and `attachments` were registered capabilities that
 * recompute_caps() never once set, so every control bound to either was
 * insensitive for the whole life of the fleet and no agent could ever be
 * told it had them.  Neither was derivable from anything this build
 * knows: nothing records which models take image input, and every agent
 * can already send its operator a file.
 *
 * Asserted through the flags type rather than by looking for the C
 * symbol, because the type is what a caps string on the wire is built
 * from -- a nickname still registered is still a capability a client can
 * be told about.
 */
static void
test_no_capability_is_permanently_false(void)
{
    guint value = 0;

    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_AGENT_CAPS, "images",
                                         &value));
    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_AGENT_CAPS, "attachments",
                                         &value));

    /* And the ones that are derived are still there. */
    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_AGENT_CAPS, "mounts",
                                        &value));
    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_AGENT_CAPS, "streaming",
                                        &value));
}

/**
 * test_nick_list_names_every_value:
 *
 * The refusals that say what somebody was allowed to write are built
 * from this, because the version that spelled the list out by hand had
 * read "none, host, container and vm" ever since distrobox was added.
 */
static void
test_nick_list_names_every_value(void)
{
    g_autofree gchar *types = clawt_enum_nick_list(CLAWT_TYPE_COMPUTER_TYPE);
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_COMPUTER_TYPE);
    guint i;

    for (i = 0; i < klass->n_values; i++)
        g_assert_nonnull(strstr(types, klass->values[i].value_nick));

    /* Prose, not a machine list: it ends up inside a sentence. */
    g_assert_nonnull(strstr(types, " and "));
}

/**
 * test_relabel_walk_matches_the_registered_type:
 *
 * The offering list and the parser are two tables, and they have to be
 * the same one.
 *
 * Both clients now build their SELinux-relabel control by walking
 * clawt_relabel_count() / clawt_relabel_nth_nick(), and the daemon
 * parses what comes back with clawt_enum_from_nick() against the
 * registered GEnum.  A nick in the walk that the GType does not know
 * would be a value a client offers, somebody picks, and the daemon
 * refuses as invalid -- and a value in the GType that the walk omits is
 * a setting the YAML has and no client can reach, which is the state
 * this walk was added to end.
 */
static void
test_relabel_walk_matches_the_registered_type(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_RELABEL);
    guint i;

    g_assert_cmpuint(clawt_relabel_count(), ==, klass->n_values);

    for (i = 0; i < clawt_relabel_count(); i++) {
        const gchar *nick = clawt_relabel_nth_nick(i);
        gint parsed = -1;

        g_assert_nonnull(nick);
        g_assert_nonnull(clawt_relabel_nth_label(i));

        /* The nick the clients offer parses back to the value it names. */
        g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_RELABEL, nick,
                                           &parsed));
        g_assert_cmpint(parsed, ==, (gint)clawt_relabel_nth(i));
        g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_RELABEL,
                                           clawt_relabel_nth(i)), ==, nick);
    }
}

/**
 * test_agent_state_shadow_is_registered:
 *
 * SHADOW carries the forward-compatibility behaviour, so it being absent
 * from the registered values would mean an unparseable agent silently
 * reporting as STOPPED.
 */
static void
test_agent_state_shadow_is_registered(void)
{
    g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                       CLAWT_AGENT_STATE_SHADOW), ==, "shadow");
}

/**
 * test_error_codes_have_names:
 *
 * Every error code maps to a distinct non-empty string.  Two codes sharing
 * a name would make wire-level error handling ambiguous.
 */
static void
test_error_codes_have_names(void)
{
    g_autoptr(GHashTable) seen = NULL;
    gint code;

    seen = g_hash_table_new(g_str_hash, g_str_equal);

    for (code = CLAWT_ERROR_FAILED; code <= CLAWT_ERROR_AI; code++) {
        const gchar *name = clawt_error_code_to_string((ClawtError)code);

        g_assert_nonnull(name);
        g_assert_cmpstr(name, !=, "");
        g_assert_false(g_hash_table_contains(seen, name));
        g_hash_table_add(seen, (gpointer)name);
    }

    g_assert_cmpstr(clawt_error_code_to_string(CLAWT_ERROR_CONFINEMENT),
                    ==, "confinement");
}

/**
 * test_error_quark_is_stable:
 */
static void
test_error_quark_is_stable(void)
{
    g_assert_cmpuint(clawt_error_quark(), ==, clawt_error_quark());
    g_assert_cmpuint(clawt_error_quark(), !=, 0);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/enums/round-trip", test_enum_round_trip);
    g_test_add_func("/enums/rejects-unknown", test_enum_from_nick_rejects_unknown);
    g_test_add_func("/enums/case-insensitive", test_enum_nick_is_case_insensitive);
    g_test_add_func("/enums/accepts-c-identifier", test_enum_accepts_c_identifier);
    g_test_add_func("/enums/flags-to-string", test_flags_to_string);
    g_test_add_func("/enums/no-permanently-false-capability",
                    test_no_capability_is_permanently_false);
    g_test_add_func("/enums/nick-list-names-every-value",
                    test_nick_list_names_every_value);
    g_test_add_func("/enums/relabel-walk-matches-the-type",
                    test_relabel_walk_matches_the_registered_type);
    g_test_add_func("/enums/shadow-registered", test_agent_state_shadow_is_registered);
    g_test_add_func("/error/codes-have-names", test_error_codes_have_names);
    g_test_add_func("/error/quark-stable", test_error_quark_is_stable);

    return g_test_run();
}
