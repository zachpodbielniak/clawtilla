/*
 * clawt-takeover.c - A person taking the screen back from the agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-takeover.h"

typedef struct {
    gchar  *holder;
    gint64  expires_at;   /* microseconds since the epoch, 0 = not held */
    gchar  *request;      /* why the agent wants hands, or NULL */
} Hold;

struct _ClawtTakeover {
    GObject     parent_instance;

    GHashTable *holds;    /* agent id -> Hold */
};

enum {
    SIGNAL_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtTakeover, clawt_takeover, G_TYPE_OBJECT)

static void
hold_free(gpointer data)
{
    Hold *hold = data;

    if (hold == NULL)
        return;

    g_free(hold->holder);
    g_free(hold->request);
    g_free(hold);
}

static Hold *
hold_for(ClawtTakeover *self, const gchar *agent_id, gboolean create)
{
    Hold *hold = g_hash_table_lookup(self->holds, agent_id);

    if (hold != NULL || !create)
        return hold;

    hold = g_new0(Hold, 1);
    g_hash_table_insert(self->holds, g_strdup(agent_id), hold);

    return hold;
}

/*
 * Applies the expiry, and says whether anybody still holds it.
 *
 * Every read goes through this rather than through a timer that clears
 * the entry.  A timer is a promise about *when* the main loop next runs
 * a source, and under load that is not soon -- so an agent would go on
 * being refused after its lease had lapsed, for a length of time nothing
 * could explain and nothing could reproduce.
 */
static gboolean
expire_if_lapsed(ClawtTakeover *self, const gchar *agent_id, Hold *hold)
{
    if (hold == NULL || hold->expires_at == 0)
        return FALSE;

    if (g_get_real_time() < hold->expires_at)
        return TRUE;

    g_message("agent %s: the screen hold has lapsed", agent_id);

    hold->expires_at = 0;
    g_clear_pointer(&hold->holder, g_free);

    return FALSE;
}

ClawtTakeover *
clawt_takeover_new(void)
{
    return g_object_new(CLAWT_TYPE_TAKEOVER, NULL);
}

gboolean
clawt_takeover_take(ClawtTakeover  *self,
                    const gchar    *agent_id,
                    const gchar    *holder,
                    gint64          lease_seconds,
                    GError        **error)
{
    Hold *hold;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    hold = hold_for(self, agent_id, TRUE);

    if (expire_if_lapsed(self, agent_id, hold) &&
        g_strcmp0(hold->holder, holder) != 0) {
        g_autofree gchar *detail = g_strdup_printf(
            "%s already has this screen; ask them to let go, or wait for "
            "the hold to lapse",
            (hold->holder != NULL) ? hold->holder : "somebody else");

        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            detail);
        return FALSE;
    }

    /*
     * A floor rather than trusting the caller.  A zero here -- which is
     * what an unset integer key reads as -- would be a lease that has
     * already expired, so taking the screen would appear to succeed and
     * change nothing at all.
     */
    if (lease_seconds <= 0)
        lease_seconds = 900;

    g_free(hold->holder);
    hold->holder = g_strdup((holder != NULL && *holder != '\0')
                            ? holder : "somebody");
    hold->expires_at = g_get_real_time() +
                       (lease_seconds * G_USEC_PER_SEC);

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0, agent_id);

    return TRUE;
}

gboolean
clawt_takeover_release(ClawtTakeover *self, const gchar *agent_id)
{
    Hold *hold;
    gboolean was_held;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    hold = hold_for(self, agent_id, FALSE);

    if (hold == NULL)
        return FALSE;

    was_held = expire_if_lapsed(self, agent_id, hold);

    hold->expires_at = 0;
    g_clear_pointer(&hold->holder, g_free);

    /*
     * The request goes with it.
     *
     * Somebody who took the screen because the agent asked for hands has
     * finished the moment they let go; a separate "done helping" step
     * would be one more thing to forget, and forgetting it leaves the
     * agent waiting for an event that already happened.
     */
    g_clear_pointer(&hold->request, g_free);

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0, agent_id);

    return was_held;
}

gboolean
clawt_takeover_is_held(ClawtTakeover *self, const gchar *agent_id)
{
    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), FALSE);

    if (agent_id == NULL)
        return FALSE;

    return expire_if_lapsed(self, agent_id,
                            hold_for(self, agent_id, FALSE));
}

const gchar *
clawt_takeover_get_holder(ClawtTakeover *self, const gchar *agent_id)
{
    Hold *hold;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), NULL);

    if (agent_id == NULL)
        return NULL;

    hold = hold_for(self, agent_id, FALSE);

    if (!expire_if_lapsed(self, agent_id, hold))
        return NULL;

    return hold->holder;
}

gint64
clawt_takeover_get_expires_at(ClawtTakeover *self, const gchar *agent_id)
{
    Hold *hold;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), 0);

    if (agent_id == NULL)
        return 0;

    hold = hold_for(self, agent_id, FALSE);

    if (!expire_if_lapsed(self, agent_id, hold))
        return 0;

    return hold->expires_at;
}

gboolean
clawt_takeover_request(ClawtTakeover *self,
                       const gchar   *agent_id,
                       const gchar   *reason)
{
    Hold *hold;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    hold = hold_for(self, agent_id, TRUE);

    g_free(hold->request);
    hold->request = g_strdup((reason != NULL && *reason != '\0')
                             ? reason
                             : "it needs somebody at the screen");

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0, agent_id);

    return TRUE;
}

const gchar *
clawt_takeover_get_request(ClawtTakeover *self, const gchar *agent_id)
{
    Hold *hold;

    g_return_val_if_fail(CLAWT_IS_TAKEOVER(self), NULL);

    if (agent_id == NULL)
        return NULL;

    hold = hold_for(self, agent_id, FALSE);

    return (hold != NULL) ? hold->request : NULL;
}

void
clawt_takeover_clear_agent(ClawtTakeover *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_TAKEOVER(self));

    if (agent_id == NULL)
        return;

    if (g_hash_table_remove(self->holds, agent_id))
        g_signal_emit(self, signals[SIGNAL_CHANGED], 0, agent_id);
}

const gchar *
clawt_takeover_refusal_text(void)
{
    /*
     * One string, defined once, and every refusal is this exact text.
     *
     * A message assembled per call site would drift, and the drift would
     * be in the half that matters: an agent reads this as instructions,
     * so a variant that dropped "do not retry" would produce an agent
     * that retries -- once a second, for a whole turn, against a screen
     * a person is using.
     */
    return "A person has taken this screen, so that action was refused "
           "and nothing happened -- no key was sent and no button was "
           "pressed. Do not retry it: while somebody is holding the "
           "screen every input is refused for the same reason, and a "
           "click that eventually lands would land on whatever is under "
           "it by then rather than on what you aimed at. To wait, stop "
           "acting on the screen and say in your reply that you are "
           "waiting for the screen to come back; you will be told when "
           "it does. If you need something done that you cannot do, say "
           "what it is with clawtilla_request_hands rather than trying "
           "again.";
}

static void
clawt_takeover_finalize(GObject *object)
{
    ClawtTakeover *self = CLAWT_TAKEOVER(object);

    g_clear_pointer(&self->holds, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_takeover_parent_class)->finalize(object);
}

static void
clawt_takeover_class_init(ClawtTakeoverClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_takeover_finalize;

    /**
     * ClawtTakeover::changed:
     * @self: the #ClawtTakeover
     * @agent_id: whose screen changed hands
     *
     * Somebody took the screen, let it go, or asked for it.
     */
    signals[SIGNAL_CHANGED] =
        g_signal_new("changed", CLAWT_TYPE_TAKEOVER, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_takeover_init(ClawtTakeover *self)
{
    self->holds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        hold_free);
}
