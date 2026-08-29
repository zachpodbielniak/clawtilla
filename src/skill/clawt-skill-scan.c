/*
 * clawt-skill-scan.c - Reading a SKILL.md, and saying what is odd about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "skill/clawt-skill-scan.h"

#include <string.h>

/*
 * How long a run of base64 alphabet has to be before it is worth
 * mentioning.
 *
 * Below this it is a hash, an id or a word: `documentation` is thirteen
 * characters of the base64 alphabet and appears in a great many honest
 * skills.  Above it, nobody has typed it and nobody reviewing the file
 * can read it.
 */
#define BASE64_RUN_THRESHOLD (120)

/* ── Front matter ────────────────────────────────────────────────── */

/*
 * Unquote one scalar.
 *
 * A deliberately small subset of YAML: single and double quotes, and
 * the four escapes a description can plausibly contain.  Front matter
 * here is two keys and whatever else somebody left in place, and
 * pulling in a YAML parser for it would mean the file's meaning
 * depended on which parser -- ours or the harness's -- read it.
 */
static gchar *
unquote_scalar(const gchar *raw)
{
    g_autoptr(GString) out = NULL;
    gsize length;
    gchar quote;
    gsize i;

    if (raw == NULL)
        return NULL;

    length = strlen(raw);

    if (length < 2 || (raw[0] != '"' && raw[0] != '\''))
        return g_strdup(raw);

    quote = raw[0];

    if (raw[length - 1] != quote)
        return g_strdup(raw);

    out = g_string_new(NULL);

    for (i = 1; i + 1 < length; i++) {
        if (quote == '\'') {
            /* YAML's single-quote escape is a doubled quote. */
            if (raw[i] == '\'' && raw[i + 1] == '\'') {
                g_string_append_c(out, '\'');
                i++;
                continue;
            }

            g_string_append_c(out, raw[i]);
            continue;
        }

        if (raw[i] != '\\') {
            g_string_append_c(out, raw[i]);
            continue;
        }

        i++;

        if (i + 1 > length - 1)
            break;

        switch (raw[i]) {
        case 'n':
            g_string_append_c(out, '\n');
            break;

        case 't':
            g_string_append_c(out, '\t');
            break;

        default:
            g_string_append_c(out, raw[i]);
            break;
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Angle brackets are refused in front matter, and only in front matter.
 *
 * The body is markdown and belongs to whoever wrote it.  The two front
 * matter values are different: they are rendered into a client's page,
 * into an org file, and into the tool listing an agent reads -- three
 * escaping contexts that have to agree, which is exactly the shape of
 * mistake that ends up as script injection into whoever opened the
 * skills page.  Nothing legitimate needs a tag in a one-line
 * description, so this costs nobody anything.
 */
static gboolean
check_no_markup(const gchar *field, const gchar *value, GError **error)
{
    if (value == NULL)
        return TRUE;

    if (strchr(value, '<') == NULL && strchr(value, '>') == NULL)
        return TRUE;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "the skill's %s contains an angle bracket; front matter is "
                "rendered into pages and into the agent's own files, so it "
                "may not carry markup", field);

    return FALSE;
}

ClawtSkill *
clawt_skill_parse(const gchar  *text,
                  gssize        length,
                  const gchar  *expected_name,
                  GError      **error)
{
    g_autofree gchar *owned = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GPtrArray) keys = NULL;
    g_autoptr(GPtrArray) values = NULL;
    g_autoptr(GString) body = NULL;
    g_autoptr(ClawtSkill) skill = NULL;
    const gchar *name = NULL;
    const gchar *description = NULL;
    gboolean closed = FALSE;
    guint end_line = 0;
    guint i;

    if (text == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the skill file is empty");
        return NULL;
    }

    owned = (length < 0) ? g_strdup(text) : g_strndup(text, (gsize)length);

    /*
     * A UTF-8 check up front rather than a surprise later.  Every field
     * here is handed to json-glib, to Pango and to an org file, and each
     * of those reacts differently to invalid bytes -- one refuses, one
     * renders nothing at all, and one writes them through.
     */
    if (!g_utf8_validate(owned, -1, NULL)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the skill file is not valid UTF-8");
        return NULL;
    }

    lines = g_strsplit(owned, "\n", -1);

    /*
     * The very first line, with no leniency.
     *
     * A `---` further down a markdown file is a horizontal rule, and
     * every renderer in the world treats it as one.  Accepting front
     * matter anywhere would mean a document whose second section starts
     * with a rule silently acquired a `name` from whatever followed it.
     */
    if (lines[0] == NULL || g_strcmp0(g_strchomp(lines[0]), "---") != 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "SKILL.md must open with a '---' front-matter "
                            "block on its first line");
        return NULL;
    }

    keys = g_ptr_array_new_with_free_func(g_free);
    values = g_ptr_array_new_with_free_func(g_free);

    for (i = 1; lines[i] != NULL; i++) {
        g_autofree gchar *trimmed = g_strdup(lines[i]);
        gchar *colon;

        g_strchomp(trimmed);

        if (g_strcmp0(trimmed, "---") == 0 ||
            g_strcmp0(trimmed, "...") == 0) {
            closed = TRUE;
            end_line = i + 1;
            break;
        }

        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        colon = strchr(trimmed, ':');

        if (colon == NULL) {
            /*
             * A continuation line, a list item, or something this
             * parser has no opinion about.  Skipped rather than
             * refused: front matter belongs to whoever wrote the skill
             * and only two of its keys are ours.
             */
            continue;
        }

        *colon = '\0';

        {
            g_autofree gchar *raw = g_strdup(colon + 1);
            gchar *key = g_strdup(g_strstrip(trimmed));

            g_ptr_array_add(keys, key);
            g_ptr_array_add(values, unquote_scalar(g_strstrip(raw)));
        }
    }

    if (!closed) {
        /*
         * Named as unterminated rather than as "no name", which is what
         * the reader would otherwise be told: a file whose closing
         * `---` was lost has every key in it and none of them parsed,
         * and "this skill has no name" sends somebody to look at a line
         * that is perfectly correct.
         */
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the front matter is never closed; it needs a "
                            "'---' line of its own after the last key");
        return NULL;
    }

    for (i = 0; i < keys->len; i++) {
        const gchar *key = g_ptr_array_index(keys, i);

        if (g_strcmp0(key, "name") == 0)
            name = g_ptr_array_index(values, i);
        else if (g_strcmp0(key, "description") == 0)
            description = g_ptr_array_index(values, i);
    }

    if (name == NULL || *name == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the front matter has no 'name'");
        return NULL;
    }

    if (!clawt_skill_name_is_valid(name)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a skill name: use lowercase letters, digits "
                    "and single hyphens, at most %d characters",
                    name, CLAWT_SKILL_MAX_NAME);
        return NULL;
    }

    if (expected_name != NULL && g_strcmp0(name, expected_name) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the front matter says 'name: %s' but the directory is "
                    "'%s'; the directory is what a harness resolves, so the "
                    "two have to agree", name, expected_name);
        return NULL;
    }

    if (description == NULL || *description == '\0') {
        /*
         * Required, because it is what a model reads when deciding
         * whether this skill is relevant.  A skill with no description
         * is one that never gets used and never says why.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the skill '%s' has no 'description'; that line is the "
                    "only part an agent sees before opening it", name);
        return NULL;
    }

    if (g_utf8_strlen(description, -1) > CLAWT_SKILL_MAX_DESCRIPTION) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the description is %ld characters and the limit is %d; "
                    "it is in context on every turn for every agent this "
                    "skill is assigned to",
                    (long)g_utf8_strlen(description, -1),
                    CLAWT_SKILL_MAX_DESCRIPTION);
        return NULL;
    }

    if (!check_no_markup("name", name, error) ||
        !check_no_markup("description", description, error))
        return NULL;

    skill = clawt_skill_new(name);
    clawt_skill_set_description(skill, description);

    for (i = 0; i < keys->len; i++) {
        const gchar *key = g_ptr_array_index(keys, i);

        if (g_strcmp0(key, "name") == 0 ||
            g_strcmp0(key, "description") == 0)
            continue;

        /*
         * Preserved verbatim and acted on by nobody.  A harness-specific
         * key we do not understand is still that harness's, and dropping
         * it on the first rewrite would quietly break a skill for the
         * one CLI it was written against.
         */
        clawt_skill_set_meta(skill, key, g_ptr_array_index(values, i));
    }

    body = g_string_new(NULL);

    for (i = end_line; lines[i] != NULL; i++) {
        if (i > end_line)
            g_string_append_c(body, '\n');

        g_string_append(body, lines[i]);
    }

    {
        gchar *text_body = g_string_free(g_steal_pointer(&body), FALSE);

        g_strstrip(text_body);
        clawt_skill_set_body(skill, text_body);
        g_free(text_body);
    }

    clawt_skill_scan(skill);

    return g_steal_pointer(&skill);
}

/* ── The scan ────────────────────────────────────────────────────── */

static gboolean
is_base64_char(gchar c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' ||
           c == '-' || c == '_';
}

/*
 * A run of base64 alphabet long enough that nobody typed it.
 *
 * The run has to be unbroken: prose is full of the same characters and
 * is broken by a space every few of them, so the space is what
 * distinguishes a paragraph from a payload.
 */
static gboolean
has_long_base64_run(const gchar *text)
{
    gsize run = 0;
    const gchar *p;

    for (p = text; *p != '\0'; p++) {
        if (is_base64_char(*p)) {
            run++;

            if (run >= BASE64_RUN_THRESHOLD)
                return TRUE;

            continue;
        }

        run = 0;
    }

    return FALSE;
}

/*
 * A download whose output is handed straight to a shell.
 *
 * What makes this worth a sentence is not that it runs something: a
 * skill is allowed to tell an agent to run things, and most do.  It is
 * that *what* runs is chosen by a server at the moment it runs, so the
 * file a reviewer read does not contain the instruction the reviewer is
 * approving, and re-reading it tomorrow will not reveal the change.
 */
static gboolean
has_download_to_shell(const gchar *text)
{
    static const gchar *fetchers[] = { "curl", "wget", "fetch", NULL };
    static const gchar *shells[] = {
        "sh", "bash", "zsh", "dash", "ksh", "fish", "python", "python3",
        "perl", "ruby", "node", NULL
    };
    g_auto(GStrv) lines = NULL;
    guint i;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *pipe_at;
        gsize f;

        for (f = 0; fetchers[f] != NULL; f++) {
            if (strstr(lines[i], fetchers[f]) != NULL)
                break;
        }

        if (fetchers[f] == NULL)
            continue;

        for (pipe_at = strchr(lines[i], '|'); pipe_at != NULL;
             pipe_at = strchr(pipe_at + 1, '|')) {
            const gchar *word = pipe_at + 1;
            gsize s;

            /* `||` is a shell operator, not a pipe into a program. */
            if (*word == '|')
                continue;

            while (*word == ' ' || *word == '\t')
                word++;

            /* `sudo sh` and `sudo -E bash` are the same pipe. */
            if (g_str_has_prefix(word, "sudo ")) {
                word += strlen("sudo ");

                while (*word == ' ' || *word == '-')
                    word += (*word == '-') ? 2 : 1;
            }

            for (s = 0; shells[s] != NULL; s++) {
                gsize n = strlen(shells[s]);

                if (strncmp(word, shells[s], n) != 0)
                    continue;

                if (word[n] == '\0' || word[n] == ' ' || word[n] == '\t' ||
                    word[n] == '-' || word[n] == '`')
                    return TRUE;
            }
        }
    }

    return FALSE;
}

/*
 * Characters that are in the text and not on the screen.
 *
 * The sharpest of the three checks, and the reason the scan exists at
 * all rather than being left to "read the file".  A zero-width joiner
 * carries instructions the reviewer cannot see; a right-to-left
 * override reverses the *display* of a line while leaving what the
 * model reads untouched, so `rm -rf /` can be shown as something else
 * entirely.  Both are invisible by construction, so no amount of
 * careful reading substitutes for the check.
 */
static gboolean
has_invisible_unicode(const gchar *text, gunichar *found)
{
    const gchar *p;

    for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        switch (c) {
        case 0x200B:   /* ZERO WIDTH SPACE */
        case 0x200C:   /* ZERO WIDTH NON-JOINER */
        case 0x200D:   /* ZERO WIDTH JOINER */
        case 0x2060:   /* WORD JOINER */
        case 0xFEFF:   /* ZERO WIDTH NO-BREAK SPACE, mid-file */
        case 0x00AD:   /* SOFT HYPHEN */
        case 0x200E:   /* LEFT-TO-RIGHT MARK */
        case 0x200F:   /* RIGHT-TO-LEFT MARK */
        case 0x202A:   /* LEFT-TO-RIGHT EMBEDDING */
        case 0x202B:   /* RIGHT-TO-LEFT EMBEDDING */
        case 0x202C:   /* POP DIRECTIONAL FORMATTING */
        case 0x202D:   /* LEFT-TO-RIGHT OVERRIDE */
        case 0x202E:   /* RIGHT-TO-LEFT OVERRIDE */
        case 0x2066:   /* LEFT-TO-RIGHT ISOLATE */
        case 0x2067:   /* RIGHT-TO-LEFT ISOLATE */
        case 0x2068:   /* FIRST STRONG ISOLATE */
        case 0x2069:   /* POP DIRECTIONAL ISOLATE */
            if (found != NULL)
                *found = c;

            return TRUE;

        default:
            break;
        }

        /* The tag block, which is invisible and encodes ASCII. */
        if (c >= 0xE0000 && c <= 0xE007F) {
            if (found != NULL)
                *found = c;

            return TRUE;
        }
    }

    return FALSE;
}

GPtrArray *
clawt_skill_scan_text(const gchar *text)
{
    GPtrArray *out;
    gunichar invisible = 0;

    out = g_ptr_array_new_with_free_func(g_free);

    if (text == NULL || *text == '\0')
        return out;

    if (has_invisible_unicode(text, &invisible))
        g_ptr_array_add(out, g_strdup_printf(
            "Contains a character that is invisible on screen (U+%04X). "
            "The model reads it and you cannot see it, so read this skill "
            "with something that shows escapes before enabling it.",
            (guint)invisible));

    if (has_download_to_shell(text))
        g_ptr_array_add(out, g_strdup(
            "Pipes a download into a shell. What actually runs is decided "
            "by a server at the moment it runs, so the text you are "
            "reviewing is not the text that will execute."));

    if (has_long_base64_run(text))
        g_ptr_array_add(out, g_strdup_printf(
            "Contains an unbroken run of at least %d base64 characters. "
            "That may be a lookup table or it may be a payload, and "
            "nothing in the file distinguishes them -- decode it before "
            "enabling this.", BASE64_RUN_THRESHOLD));

    return out;
}

void
clawt_skill_scan(ClawtSkill *self)
{
    g_autofree gchar *rendered = NULL;
    g_autoptr(GPtrArray) warnings = NULL;
    guint i;

    g_return_if_fail(self != NULL);

    rendered = clawt_skill_render(self);
    warnings = clawt_skill_scan_text(rendered);

    for (i = 0; i < warnings->len; i++)
        clawt_skill_add_warning(self, g_ptr_array_index(warnings, i));
}

gchar *
clawt_skill_digest(const gchar *text)
{
    g_return_val_if_fail(text != NULL, NULL);

    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}
