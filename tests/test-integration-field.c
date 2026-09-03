/*
 * test-integration-field.c - The form, and that it still describes the config
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The field table exists because both clients were rendering
 * configuration keys straight at people -- `imap_host`, `smtp_port`,
 * `access_token` -- and because the GTK notify editor drew every
 * backend's fields at once, so choosing "Desktop notification" asked for
 * a Matrix homeserver.
 *
 * A table of labels is exactly the thing this codebase has watched drift
 * every time somebody has written one: three key tables, two
 * colour-scheme lists, three integration key lists.  So the interesting
 * tests here are not that the labels read nicely.  They are the ones
 * that fail when the table and the schema stop describing the same
 * software.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * Every key in the table is one the daemon will actually accept.
 *
 * clawt_daemon_apply_integration_fields() walks the schema and applies
 * any payload member whose name is an `integrations.*` leaf.  A field
 * naming anything else is a control somebody fills in, a save that
 * reports success, and a value read from nowhere -- which is the exact
 * shape of half the bugs in this tree's own notes.
 */
static void
test_every_field_is_a_real_config_key(void)
{
    static const gchar *const types[] = {
        "matrix", "email", "webhook", "mcp", "connector", "notify"
    };
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize total = 0;
    gsize t;

    schema = clawt_config_schema_get(&n_entries);
    g_assert_cmpuint(n_entries, >, 0);

    for (t = 0; t < G_N_ELEMENTS(types); t++) {
        const ClawtIntegrationField *list;
        gsize n = 0;
        gsize f;

        list = clawt_integration_fields(types[t], &n);
        total += n;

        for (f = 0; f < n; f++) {
            g_autofree gchar *key =
                g_strdup_printf("integrations.%s", list[f].key);
            gboolean found = FALSE;
            gsize e;

            for (e = 0; e < n_entries; e++) {
                if (g_strcmp0(schema[e].key, key) == 0)
                    found = TRUE;
            }

            if (!found)
                g_error("%s field '%s' is not a config key "
                        "(no schema entry '%s')",
                        types[t], list[f].label, key);
        }
    }

    /*
     * And there were fields to check.  A lookup that returned nothing
     * for every type would pass the walk above without looking at
     * anything -- the shape this suite's floor script exists for, one
     * table down.
     */
    g_assert_cmpuint(total, >, 25);
}

/*
 * Every key an integration type *requires* has something to type it in.
 *
 * ClawtIntegrationInfo.required_keys is what the loader refuses an
 * enabled integration for.  A required key with no field is an
 * integration that cannot be completed from either client -- you would
 * add it, save it, and be told it is invalid with nowhere to fix it.
 */
static void
test_every_required_key_has_a_field(void)
{
    const ClawtIntegrationInfo *types;
    gsize n_types = 0;
    gsize t;

    types = clawt_integration_list(&n_types);

    for (t = 0; t < n_types; t++) {
        gsize k;

        if (types[t].required_keys == NULL)
            continue;

        for (k = 0; types[t].required_keys[k] != NULL; k++) {
            const ClawtIntegrationField *list;
            gsize n = 0;
            gsize f;
            gboolean found = FALSE;

            list = clawt_integration_fields(types[t].id, &n);

            for (f = 0; f < n; f++) {
                if (g_strcmp0(list[f].key, types[t].required_keys[k]) == 0)
                    found = TRUE;
            }

            if (!found)
                g_error("%s requires '%s' and no field asks for it",
                        types[t].id, types[t].required_keys[k]);
        }
    }
}

/*
 * And every credential key is described as a secret.
 *
 * A credential rendered as an ordinary text field is an invitation to
 * paste the credential itself, which is the one thing that must never
 * reach clawtilla.yaml.  The type is what makes a client draw the
 * reference syntax instead, so the two tables have to agree about which
 * keys those are.
 */
static void
test_every_credential_key_is_a_secret_field(void)
{
    const ClawtIntegrationInfo *types;
    gsize n_types = 0;
    gsize t;

    types = clawt_integration_list(&n_types);

    for (t = 0; t < n_types; t++) {
        gsize k;

        if (types[t].credential_keys == NULL)
            continue;

        for (k = 0; types[t].credential_keys[k] != NULL; k++) {
            const ClawtIntegrationField *list;
            gsize n = 0;
            gsize f;
            gboolean found = FALSE;

            list = clawt_integration_fields(types[t].id, &n);

            for (f = 0; f < n; f++) {
                if (g_strcmp0(list[f].key, types[t].credential_keys[k]) != 0)
                    continue;

                found = TRUE;

                if (list[f].kind != CLAWT_FIELD_SECRET)
                    g_error("%s's '%s' holds a credential and is drawn as "
                            "an ordinary field", types[t].id,
                            types[t].credential_keys[k]);
            }

            if (!found)
                g_error("%s takes a credential in '%s' and no field asks "
                        "for it", types[t].id, types[t].credential_keys[k]);
        }
    }
}

/*
 * A secret field's hint says the value is a reference.
 *
 * The daemon never writes a secret's value into the config -- it takes
 * secret_key/secret_backend/secret_locator and nothing else -- so a
 * field that does not say so is one somebody pastes a live token into
 * and watches be silently discarded.
 */
static void
test_a_secret_field_explains_itself(void)
{
    const gchar *const types[] = { "matrix", "email", "notify" };
    gsize t;
    guint secrets = 0;

    for (t = 0; t < G_N_ELEMENTS(types); t++) {
        const ClawtIntegrationField *list;
        gsize n = 0;
        gsize f;

        list = clawt_integration_fields(types[t], &n);

        for (f = 0; f < n; f++) {
            if (list[f].kind != CLAWT_FIELD_SECRET)
                continue;

            secrets++;

            g_assert_nonnull(list[f].hint);
            g_assert_nonnull(strstr(list[f].hint, "env:"));
            g_assert_nonnull(strstr(list[f].hint, "file:"));
        }
    }

    /* And there were some, so the walk above proved something. */
    g_assert_cmpuint(secrets, >=, 3);
}

/*
 * A field's type run is contiguous.
 *
 * clawt_integration_fields() returns a pointer into the table and a
 * count, so a field filed under the wrong heading would silently
 * truncate its type's form -- the count would stop at the interruption
 * and the fields after it would never be drawn.
 */
static void
test_each_type_is_one_run(void)
{
    const gchar *const types[] = {
        "matrix", "email", "webhook", "mcp", "connector", "notify"
    };
    gsize t;

    for (t = 0; t < G_N_ELEMENTS(types); t++) {
        const ClawtIntegrationField *list;
        gsize n = 0;
        gsize f;

        list = clawt_integration_fields(types[t], &n);

        g_assert_cmpuint(n, >, 0);

        for (f = 0; f < n; f++)
            g_assert_cmpstr(list[f].type, ==, types[t]);
    }
}

/*
 * A field that depends on a choice is shown for that choice and no other.
 *
 * This is the whole "inputs which may or may not relate to the
 * integration you are adding" complaint: the old form asked a desktop
 * notification for a Matrix homeserver, a room, an ntfy URL, a token and
 * a command, all at once.
 */
static void
test_a_conditional_field_applies_to_its_choice(void)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    guint desktop_fields = 0;
    guint matrix_fields = 0;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        if (clawt_integration_field_applies(&list[i], "desktop"))
            desktop_fields++;

        if (clawt_integration_field_applies(&list[i], "matrix"))
            matrix_fields++;
    }

    /*
     * A desktop notification needs nothing of its own, so it sees only
     * the fields every backend shares -- and strictly fewer than the
     * Matrix one, which adds a homeserver and a room.
     */
    g_assert_cmpuint(desktop_fields, <, matrix_fields);
    g_assert_cmpuint(desktop_fields, >, 0);

    /* Named, so the counts above cannot drift into meaning nothing. */
    for (i = 0; i < n; i++) {
        if (g_strcmp0(list[i].key, "room") != 0)
            continue;

        g_assert_true(clawt_integration_field_applies(&list[i], "matrix"));
        g_assert_false(clawt_integration_field_applies(&list[i], "desktop"));
        g_assert_false(clawt_integration_field_applies(&list[i], "ntfy"));

        /*
         * And an unresolved value hides it rather than showing it.  The
         * caller is required to pass the *effective* backend, so a NULL
         * here means the caller did not resolve one -- showing every
         * conditional field would be the old bug returning.
         */
        g_assert_false(clawt_integration_field_applies(&list[i], NULL));
    }
}

/*
 * One key can be two fields when it means two things.
 *
 * notify's `url` is an ntfy topic and a Gotify server.  Calling both
 * "URL" with one hint is a form that is accurate and useless, so they
 * are separate entries -- and exactly one of them may apply at a time,
 * or a client would draw two inputs for one key and save whichever it
 * read last.
 */
static void
test_one_key_can_be_two_fields(void)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    guint urls = 0;
    guint applying = 0;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        if (g_strcmp0(list[i].key, "url") != 0)
            continue;

        urls++;

        if (clawt_integration_field_applies(&list[i], "ntfy"))
            applying++;
    }

    g_assert_cmpuint(urls, ==, 2);
    g_assert_cmpuint(applying, ==, 1);
}

/*
 * The sentence shown before a type is chosen.
 *
 * It lists only the *unconditionally* required fields: saying a notifier
 * needs a room would be wrong for four of its five backends, and being
 * confidently wrong about what something needs is worse than saying
 * nothing.
 */
static void
test_the_needs_summary_skips_conditional_fields(void)
{
    g_autofree gchar *matrix = clawt_integration_needs_summary("matrix");
    g_autofree gchar *notify = clawt_integration_needs_summary("notify");
    g_autofree gchar *local = clawt_integration_needs_summary("local");

    g_assert_nonnull(matrix);
    g_assert_nonnull(strstr(matrix, "Homeserver"));
    g_assert_nonnull(strstr(matrix, "User ID"));
    g_assert_nonnull(strstr(matrix, "Access token"));

    /*
     * notify's only unconditional requirement is the backend; the room,
     * the topic URL and the command each belong to one of them.
     */
    g_assert_nonnull(notify);
    g_assert_null(strstr(notify, "Room"));
    g_assert_null(strstr(notify, "Topic URL"));
    g_assert_null(strstr(notify, "Command"));

    /* A type that needs nothing says nothing rather than "Needs .". */
    g_assert_null(local);
}

/*
 * Every type the catalogue offers has a name a person would recognise.
 *
 * Walked from clawt_integration_list() rather than from a list here, so
 * a type added later fails this instead of quietly appearing in both
 * clients' pickers as its own lowercase id.
 */
static void
test_every_type_has_a_label(void)
{
    const ClawtIntegrationInfo *types;
    gsize n_types = 0;
    gsize t;

    types = clawt_integration_list(&n_types);
    g_assert_cmpuint(n_types, >, 0);

    for (t = 0; t < n_types; t++) {
        const gchar *label = clawt_integration_type_label(types[t].id);

        g_assert_nonnull(label);
        g_assert_cmpstr(label, !=, "");

        /*
         * Different from the id, which is what says somebody wrote one.
         * The fallback returns the id itself, so this is the assertion
         * that a type was not simply forgotten.
         */
        if (g_strcmp0(label, types[t].id) == 0)
            g_error("integration type '%s' has no human name", types[t].id);
    }
}

/*
 * The events a notifier offers are the ones the type actually has.
 *
 * This is a hand-written list of an option's values, which is the single
 * thing this codebase has watched drift every time -- three key tables,
 * two colour-scheme lists, three integration key lists.  The GTK editor
 * had one of exactly these before, with four entries, and it went stale
 * the moment a fifth event was added: `update` existed, the daemon
 * accepted it, and no client offered it.
 *
 * Walked from the flags type rather than compared against a copy here,
 * so a sixth fails this instead of quietly being unreachable.
 */
static void
test_the_notify_events_are_the_real_ones(void)
{
    g_autoptr(GFlagsClass) klass =
        g_type_class_ref(CLAWT_TYPE_NOTIFY_EVENTS);
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    gboolean checked = FALSE;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        guint offered = 0;
        guint v;

        if (list[i].kind != CLAWT_FIELD_FLAGS)
            continue;

        checked = TRUE;

        /* Every nick the type carries is offered... */
        for (v = 0; v < klass->n_values; v++) {
            const gchar *nick = klass->values[v].value_nick;
            gboolean found = FALSE;
            gsize c;

            for (c = 0; list[i].choices[c] != NULL; c++) {
                if (g_strcmp0(list[i].choices[c], nick) == 0)
                    found = TRUE;
            }

            if (!found)
                g_error("a notifier can be told about '%s' and no client "
                        "offers it", nick);
        }

        /* ...and nothing is offered that the type does not have. */
        for (offered = 0; list[i].choices[offered] != NULL; offered++) {
            guint value = 0;

            if (!clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS,
                                       list[i].choices[offered], &value))
                g_error("'%s' is offered as an event and is not one",
                        list[i].choices[offered]);
        }

        g_assert_cmpuint(offered, ==, klass->n_values);

        /* And each has something to call it. */
        for (offered = 0; list[i].choices[offered] != NULL; offered++)
            g_assert_nonnull(list[i].choice_labels[offered]);
    }

    g_assert_true(checked);
}

/*
 * And a notifier's backends are the ones the enum has.
 *
 * Same reasoning as the events above, and the same hazard: this list is
 * what a person picks from, so a backend the daemon grew and the picker
 * did not is one nobody can select -- silently, because a combo with
 * five entries looks exactly like a combo with six.
 */
static void
test_the_notify_backends_are_the_real_ones(void)
{
    g_autoptr(GEnumClass) klass =
        g_type_class_ref(CLAWT_TYPE_NOTIFY_BACKEND);
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    gboolean checked = FALSE;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        guint offered;
        guint v;

        if (g_strcmp0(list[i].key, "backend") != 0)
            continue;

        checked = TRUE;

        for (v = 0; v < klass->n_values; v++) {
            const gchar *nick = klass->values[v].value_nick;
            gboolean found = FALSE;
            gsize c;

            for (c = 0; list[i].choices[c] != NULL; c++) {
                if (g_strcmp0(list[i].choices[c], nick) == 0)
                    found = TRUE;
            }

            if (!found)
                g_error("a notifier can reach you by '%s' and no client "
                        "offers it", nick);
        }

        for (offered = 0; list[i].choices[offered] != NULL; offered++) {
            gint value = 0;

            if (!clawt_enum_from_nick(CLAWT_TYPE_NOTIFY_BACKEND,
                                      list[i].choices[offered], &value))
                g_error("'%s' is offered as a backend and is not one",
                        list[i].choices[offered]);

            g_assert_nonnull(list[i].choice_labels[offered]);
        }

        g_assert_cmpuint(offered, ==, klass->n_values);
    }

    g_assert_true(checked);
}

/*
 * The default of a choice is its first value.
 *
 * A client has to resolve `when_key` the same way the daemon will, and
 * an instance that has never had a backend written is on the schema
 * default -- so "unset" has to become a real value before anything is
 * compared against it.
 */
static void
test_a_choice_has_a_default(void)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields("notify", &n);

    for (i = 0; i < n; i++) {
        if (g_strcmp0(list[i].key, "backend") == 0) {
            g_assert_cmpstr(clawt_integration_field_default(&list[i]), ==,
                            "desktop");
        }

        /* Only a choice has one; a text field would be inventing it. */
        if (list[i].kind == CLAWT_FIELD_TEXT)
            g_assert_null(clawt_integration_field_default(&list[i]));
    }
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/integration-field/every-field-is-a-real-config-key",
                    test_every_field_is_a_real_config_key);
    g_test_add_func("/integration-field/every-required-key-has-a-field",
                    test_every_required_key_has_a_field);
    g_test_add_func("/integration-field/every-credential-is-a-secret-field",
                    test_every_credential_key_is_a_secret_field);
    g_test_add_func("/integration-field/a-secret-field-explains-itself",
                    test_a_secret_field_explains_itself);
    g_test_add_func("/integration-field/each-type-is-one-run",
                    test_each_type_is_one_run);
    g_test_add_func("/integration-field/a-conditional-field-applies",
                    test_a_conditional_field_applies_to_its_choice);
    g_test_add_func("/integration-field/one-key-can-be-two-fields",
                    test_one_key_can_be_two_fields);
    g_test_add_func("/integration-field/needs-summary-skips-conditionals",
                    test_the_needs_summary_skips_conditional_fields);
    g_test_add_func("/integration-field/every-type-has-a-label",
                    test_every_type_has_a_label);
    g_test_add_func("/integration-field/notify-events-are-the-real-ones",
                    test_the_notify_events_are_the_real_ones);
    g_test_add_func("/integration-field/notify-backends-are-the-real-ones",
                    test_the_notify_backends_are_the_real_ones);
    g_test_add_func("/integration-field/a-choice-has-a-default",
                    test_a_choice_has_a_default);

    return g_test_run();
}
