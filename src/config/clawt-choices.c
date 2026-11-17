/*
 * clawt-choices.c - The fixed lists a setting's drop-down offers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Each list is the one place its values are spelled.  The values are
 * what the daemon reads -- clawt_cron's weekday parser, the task
 * manager's reviewer resolver, the CLIs' effort flags -- and the labels
 * are what a person reads; tests/test-choices.c holds the two together.
 */

#include "clawtilla.h"

typedef struct {
    const gchar *value;
    const gchar *label;
} Choice;

/* ── model.effort ────────────────────────────────────────────────── */

static const Choice efforts[] = {
    { "",       "The model's default" },
    { "low",    "Low" },
    { "medium", "Medium" },
    { "high",   "High" },
    { "xhigh",  "Extra high" },
    { "max",    "Maximum" }
};

guint
clawt_effort_count(void)
{
    return G_N_ELEMENTS(efforts);
}

const gchar *
clawt_effort_nth(guint n)
{
    return n < G_N_ELEMENTS(efforts) ? efforts[n].value : NULL;
}

const gchar *
clawt_effort_nth_label(guint n)
{
    return n < G_N_ELEMENTS(efforts) ? efforts[n].label : NULL;
}

/* ── routines.weekday ────────────────────────────────────────────── */

/* Sunday first, as cron and clawt_cron's parser count. */
static const Choice weekdays[] = {
    { "sunday",    "Sunday" },
    { "monday",    "Monday" },
    { "tuesday",   "Tuesday" },
    { "wednesday", "Wednesday" },
    { "thursday",  "Thursday" },
    { "friday",    "Friday" },
    { "saturday",  "Saturday" }
};

guint
clawt_weekday_count(void)
{
    return G_N_ELEMENTS(weekdays);
}

const gchar *
clawt_weekday_nth(guint n)
{
    return n < G_N_ELEMENTS(weekdays) ? weekdays[n].value : NULL;
}

const gchar *
clawt_weekday_nth_label(guint n)
{
    return n < G_N_ELEMENTS(weekdays) ? weekdays[n].label : NULL;
}

/* ── routines.schedule ───────────────────────────────────────────── */

/*
 * The labels, by nick.  The values are walked from the enumeration, so a
 * schedule added there is offered at once -- with its nick capitalised
 * until a label is written here.
 */
static const Choice schedule_labels[] = {
    { "manual",   "Only when started by hand" },
    { "hourly",   "Every hour" },
    { "daily",    "Every day" },
    { "weekdays", "Monday to Friday" },
    { "weekly",   "Once a week" },
    { "custom",   "A cron expression" }
};

static GEnumClass *
schedule_class(void)
{
    static GEnumClass *klass = NULL;

    if (g_once_init_enter(&klass))
        g_once_init_leave(&klass, g_type_class_ref(CLAWT_TYPE_SCHEDULE));
    return klass;
}

guint
clawt_schedule_count(void)
{
    return schedule_class()->n_values;
}

const gchar *
clawt_schedule_nth(guint n)
{
    GEnumClass *klass = schedule_class();

    return n < klass->n_values ? klass->values[n].value_nick : NULL;
}

const gchar *
clawt_schedule_nth_label(guint n)
{
    const gchar *nick = clawt_schedule_nth(n);
    gsize i;

    if (nick == NULL)
        return NULL;
    for (i = 0; i < G_N_ELEMENTS(schedule_labels); i++)
        if (g_strcmp0(schedule_labels[i].value, nick) == 0)
            return schedule_labels[i].label;
    return nick;
}

/* ── review_by ───────────────────────────────────────────────────── */

/*
 * The words clawt_task_resolve_reviewer() understands; any agent id is
 * the other half.  `user` is accepted there as `operator` and is not
 * offered twice.
 */
static const Choice reviewers[] = {
    { "",          NULL },
    { "none",      "Nobody" },
    { "operator",  "You (the operator)" },
    { "delegator", "Whoever delegated the task" },
    { "lead",      "The team's lead" }
};

guint
clawt_review_by_count(void)
{
    return G_N_ELEMENTS(reviewers);
}

const gchar *
clawt_review_by_nth(guint n)
{
    return n < G_N_ELEMENTS(reviewers) ? reviewers[n].value : NULL;
}

const gchar *
clawt_review_by_nth_label(guint n, gboolean project)
{
    if (n >= G_N_ELEMENTS(reviewers))
        return NULL;
    if (n == 0)
        return project ? "Not set -- each agent's own reviewer"
                       : "Not set -- no review";
    return reviewers[n].label;
}

gchar *
clawt_review_by_agent_label(const gchar *agent_id)
{
    g_return_val_if_fail(agent_id != NULL, NULL);

    return g_strdup_printf("The agent %s", agent_id);
}

/* ── Screen sizes ────────────────────────────────────────────────── */

/* The sizes worth offering, narrowest first.  Any other WxH still works. */
static const gchar *const resolutions[] = {
    "1024x768", "1280x720", "1280x800", "1280x1024", "1366x768",
    "1440x900", "1600x900", "1680x1050", "1920x1080", "1920x1200",
    "2560x1440", "3840x2160"
};

guint
clawt_resolution_count(void)
{
    return G_N_ELEMENTS(resolutions);
}

const gchar *
clawt_resolution_nth(guint n)
{
    return n < G_N_ELEMENTS(resolutions) ? resolutions[n] : NULL;
}
