/*
 * clawt-agent.c - One agent in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "agent/clawt-agent.h"

enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_ERROR,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ClawtAgent {
    GObject parent_instance;

    ClawtAgentConfig  *config;
    ClawtMailbox      *mailbox;
    ClawtAgentRuntime *runtime;
    ClawtComputer     *computer;
    ClawtLink         *link;

    ClawtAgentState state;
    ClawtAgentCaps  caps;
    gchar          *status_detail;
};

G_DEFINE_FINAL_TYPE(ClawtAgent, clawt_agent, G_TYPE_OBJECT)

static void
set_state(ClawtAgent *self, ClawtAgentState state, const gchar *detail)
{
    if (self->state == state &&
        g_strcmp0(self->status_detail, detail) == 0)
        return;

    self->state = state;

    g_free(self->status_detail);
    self->status_detail = g_strdup(detail);

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state, detail);
}

/*
 * Works out what this agent can actually do, from what it has rather than
 * what its config asked for.
 *
 * This exists so no interface offers a control the agent cannot honour, and
 * so the agent is never told it has a tool that is not really there -- an
 * agent that believes it has a computer it cannot reach burns whole turns
 * hunting for it.
 */
static void
recompute_caps(ClawtAgent *self)
{
    ClawtAgentCaps caps = CLAWT_AGENT_CAPS_NONE;

    if (self->runtime != NULL)
        caps |= clawt_agent_runtime_get_caps(self->runtime);

    if (self->computer != NULL) {
        ClawtComputerType type =
            (ClawtComputerType)clawt_agent_config_get_enum(self->config,
                                                           "computer.type");

        if (type != CLAWT_COMPUTER_NONE)
            caps |= CLAWT_AGENT_CAPS_COMPUTER;

        if (type == CLAWT_COMPUTER_HOST)
            caps |= CLAWT_AGENT_CAPS_HOST_CONTROL;
    }

    {
        g_autoptr(GPtrArray) mounts =
            clawt_agent_config_get_mounts(self->config);

        if (mounts != NULL && mounts->len > 0)
            caps |= CLAWT_AGENT_CAPS_MOUNTS;
    }

    if (clawt_agent_config_get_boolean(self->config,
                                       "computer.desktop.enabled")) {
        caps |= CLAWT_AGENT_CAPS_DESKTOP;

        /*
         * Input is a separate capability from seeing the screen.  An agent
         * that can take screenshots but not click is a genuinely useful
         * amount of access and a much smaller grant, so the two are not
         * conflated.
         */
        if (clawt_agent_config_get_boolean(self->config,
                                           "computer.desktop.allow_input"))
            caps |= CLAWT_AGENT_CAPS_DESKTOP_INPUT;
    }

    if (clawt_agent_config_get_string(self->config, "model.effort") != NULL)
        caps |= CLAWT_AGENT_CAPS_EFFORT_LEVELS;

    self->caps = caps;
}

ClawtAgent *
clawt_agent_new(ClawtAgentConfig *config, ClawtMailbox *mailbox)
{
    ClawtAgent *self;

    g_return_val_if_fail(config != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_AGENT, NULL);
    self->config = clawt_agent_config_ref(config);

    if (mailbox != NULL)
        self->mailbox = g_object_ref(mailbox);

    if (clawt_agent_config_is_shadow(config)) {
        self->state = CLAWT_AGENT_STATE_SHADOW;
        self->status_detail =
            g_strdup(clawt_agent_config_get_shadow_reason(config));
    } else {
        self->state = CLAWT_AGENT_STATE_STOPPED;
    }

    recompute_caps(self);

    return self;
}

const gchar *
clawt_agent_get_id(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return clawt_agent_config_get_id(self->config);
}

const gchar *
clawt_agent_get_name(ClawtAgent *self)
{
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    name = clawt_agent_config_get_string(self->config, "name");

    /* An agent with no display name is called by its id rather than nothing. */
    return (name != NULL) ? name : clawt_agent_get_id(self);
}

const gchar *
clawt_agent_get_description(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return clawt_agent_config_get_string(self->config, "description");
}

ClawtAgentState
clawt_agent_get_state(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_AGENT_STATE_ERROR);

    return self->state;
}

ClawtAgentCaps
clawt_agent_get_caps(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), CLAWT_AGENT_CAPS_NONE);

    return self->caps;
}

ClawtAgentConfig *
clawt_agent_get_config(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->config;
}

ClawtMailbox *
clawt_agent_get_mailbox(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->mailbox;
}

gboolean
clawt_agent_is_chief_of_staff(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    return clawt_agent_config_get_boolean(self->config, "chief_of_staff");
}

const gchar *
clawt_agent_get_status_detail(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->status_detail;
}

static void
on_runtime_exited(ClawtAgentRuntime *runtime,
                  gboolean           clean,
                  const gchar       *detail,
                  gpointer           user_data)
{
    ClawtAgent *self = user_data;

    /*
     * The link goes with the process.  Leaving it attached would let
     * delivery keep succeeding into a socket nobody is reading.
     */
    g_clear_object(&self->link);

    if (self->state == CLAWT_AGENT_STATE_STOPPING || clean)
        set_state(self, CLAWT_AGENT_STATE_STOPPED, detail);
    else
        set_state(self, CLAWT_AGENT_STATE_ERROR, detail);
}

void
clawt_agent_set_runtime(ClawtAgent *self, ClawtAgentRuntime *runtime)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (self->runtime != NULL)
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_exited),
                                             self);

    g_clear_object(&self->runtime);

    if (runtime != NULL) {
        self->runtime = g_object_ref(runtime);
        g_signal_connect(runtime, "exited", G_CALLBACK(on_runtime_exited),
                         self);
    }

    recompute_caps(self);
}

ClawtAgentRuntime *
clawt_agent_get_runtime(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->runtime;
}

void
clawt_agent_set_computer(ClawtAgent *self, ClawtComputer *computer)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->computer);

    if (computer != NULL)
        self->computer = g_object_ref(computer);

    recompute_caps(self);
}

ClawtComputer *
clawt_agent_get_computer(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->computer;
}

void
clawt_agent_set_link(ClawtAgent *self, ClawtLink *link_)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    g_clear_object(&self->link);

    if (link_ != NULL) {
        self->link = g_object_ref(link_);

        if (self->state == CLAWT_AGENT_STATE_STARTING ||
            self->state == CLAWT_AGENT_STATE_DEGRADED)
            set_state(self, CLAWT_AGENT_STATE_RUNNING, NULL);

        return;
    }

    /*
     * A running agent that loses its link is degraded, not stopped: the
     * process is still there, but nothing can be delivered to it, and
     * reporting it as running would make messages appear to be going
     * somewhere.
     */
    if (self->state == CLAWT_AGENT_STATE_RUNNING)
        set_state(self, CLAWT_AGENT_STATE_DEGRADED,
                  "the agent's process is up but it is not connected");
}

ClawtLink *
clawt_agent_get_link(ClawtAgent *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), NULL);

    return self->link;
}

gboolean
clawt_agent_start(ClawtAgent *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_AGENT(self), FALSE);

    if (self->state == CLAWT_AGENT_STATE_SHADOW) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' cannot run: %s",
                    clawt_agent_get_id(self),
                    self->status_detail != NULL ? self->status_detail
                                                : "its configuration was not "
                                                  "understood");
        return FALSE;
    }

    if (!clawt_agent_config_get_boolean(self->config, "enabled")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' is disabled", clawt_agent_get_id(self));
        return FALSE;
    }

    if (self->state == CLAWT_AGENT_STATE_RUNNING ||
        self->state == CLAWT_AGENT_STATE_STARTING)
        return TRUE;

    if (self->runtime == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "agent '%s' has no runtime attached",
                    clawt_agent_get_id(self));
        return FALSE;
    }

    /*
     * Starting, not running.  The process being up is not the same as being
     * reachable: it becomes running when its link arrives.
     */
    set_state(self, CLAWT_AGENT_STATE_STARTING, NULL);

    if (!clawt_agent_runtime_start(self->runtime, error)) {
        set_state(self, CLAWT_AGENT_STATE_ERROR,
                  (error != NULL && *error != NULL) ? (*error)->message
                                                    : "it would not start");
        return FALSE;
    }

    return TRUE;
}

void
clawt_agent_stop(ClawtAgent *self)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    if (self->state == CLAWT_AGENT_STATE_STOPPED ||
        self->state == CLAWT_AGENT_STATE_SHADOW)
        return;

    set_state(self, CLAWT_AGENT_STATE_STOPPING, NULL);

    if (self->link != NULL) {
        clawt_link_close(self->link, "the agent is being stopped");
        g_clear_object(&self->link);
    }

    if (self->runtime != NULL)
        clawt_agent_runtime_stop(self->runtime);
    else
        set_state(self, CLAWT_AGENT_STATE_STOPPED, NULL);

    /*
     * The mailbox is deliberately untouched.  Messages queued for a stopped
     * agent wait for it to come back -- that is the entire point of having
     * one.
     */
}

void
clawt_agent_mark_shadow(ClawtAgent *self, const gchar *reason)
{
    g_return_if_fail(CLAWT_IS_AGENT(self));

    set_state(self, CLAWT_AGENT_STATE_SHADOW, reason);
}

static void
clawt_agent_dispose(GObject *object)
{
    ClawtAgent *self = CLAWT_AGENT(object);

    if (self->runtime != NULL)
        g_signal_handlers_disconnect_by_func(self->runtime,
                                             G_CALLBACK(on_runtime_exited),
                                             self);

    g_clear_object(&self->runtime);
    g_clear_object(&self->computer);
    g_clear_object(&self->link);
    g_clear_object(&self->mailbox);
    g_clear_pointer(&self->config, clawt_agent_config_unref);

    G_OBJECT_CLASS(clawt_agent_parent_class)->dispose(object);
}

static void
clawt_agent_finalize(GObject *object)
{
    ClawtAgent *self = CLAWT_AGENT(object);

    g_clear_pointer(&self->status_detail, g_free);

    G_OBJECT_CLASS(clawt_agent_parent_class)->finalize(object);
}

static void
clawt_agent_class_init(ClawtAgentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_agent_dispose;
    object_class->finalize = clawt_agent_finalize;

    /**
     * ClawtAgent::state-changed:
     * @self: the agent
     * @state: the new state
     * @detail: (nullable): what happened
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed", CLAWT_TYPE_AGENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 2,
                     G_TYPE_INT, G_TYPE_STRING);

    /**
     * ClawtAgent::error:
     * @self: the agent
     * @message: what went wrong
     */
    signals[SIGNAL_ERROR] =
        g_signal_new("error", CLAWT_TYPE_AGENT, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
clawt_agent_init(ClawtAgent *self)
{
    self->state = CLAWT_AGENT_STATE_STOPPED;
    self->caps = CLAWT_AGENT_CAPS_NONE;
}
