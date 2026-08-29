/*
 * clawt-trigger-event.c - What every forge's delivery is flattened into
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "trigger/clawt-trigger-event.h"

#include <string.h>

struct _ClawtTriggerEvent {
    ClawtTriggerProvider  provider;

    gchar                *name;
    gchar                *delivery_id;
    gchar                *repo;
    gchar                *ref;
    gchar                *branch;
    gchar                *actor;
    gchar                *title;
    gchar                *url;
    gchar                *number;
    gchar                *payload;
};

ClawtTriggerEvent *
clawt_trigger_event_new(ClawtTriggerProvider  provider,
                        const gchar          *name,
                        const gchar          *delivery_id)
{
    ClawtTriggerEvent *self = g_new0(ClawtTriggerEvent, 1);

    self->provider = provider;
    self->name = g_strdup(name);
    self->delivery_id = g_strdup(delivery_id);

    return self;
}

ClawtTriggerEvent *
clawt_trigger_event_copy(ClawtTriggerEvent *self)
{
    ClawtTriggerEvent *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = clawt_trigger_event_new(self->provider, self->name,
                                   self->delivery_id);

    copy->repo = g_strdup(self->repo);
    copy->ref = g_strdup(self->ref);
    copy->branch = g_strdup(self->branch);
    copy->actor = g_strdup(self->actor);
    copy->title = g_strdup(self->title);
    copy->url = g_strdup(self->url);
    copy->number = g_strdup(self->number);
    copy->payload = g_strdup(self->payload);

    return copy;
}

void
clawt_trigger_event_free(ClawtTriggerEvent *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->delivery_id);
    g_free(self->repo);
    g_free(self->ref);
    g_free(self->branch);
    g_free(self->actor);
    g_free(self->title);
    g_free(self->url);
    g_free(self->number);
    g_free(self->payload);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtTriggerEvent, clawt_trigger_event,
                    clawt_trigger_event_copy, clawt_trigger_event_free)

ClawtTriggerProvider
clawt_trigger_event_get_provider(ClawtTriggerEvent *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_TRIGGER_PROVIDER_GENERIC);

    return self->provider;
}

#define GETTER(field_)                                                  \
    const gchar *                                                       \
    clawt_trigger_event_get_##field_(ClawtTriggerEvent *self)           \
    {                                                                   \
        g_return_val_if_fail(self != NULL, NULL);                       \
                                                                        \
        return self->field_;                                            \
    }

GETTER(name)
GETTER(delivery_id)
GETTER(repo)
GETTER(ref)
GETTER(branch)
GETTER(actor)
GETTER(title)
GETTER(url)
GETTER(number)
GETTER(payload)

#undef GETTER

#define SETTER(field_)                                                  \
    void                                                                \
    clawt_trigger_event_set_##field_(ClawtTriggerEvent *self,           \
                                     const gchar       *value)          \
    {                                                                   \
        g_return_if_fail(self != NULL);                                 \
                                                                        \
        g_free(self->field_);                                           \
        self->field_ = g_strdup(value);                                 \
    }

SETTER(repo)
SETTER(actor)
SETTER(title)
SETTER(url)
SETTER(number)
SETTER(payload)

#undef SETTER

void
clawt_trigger_event_set_payload_bytes(ClawtTriggerEvent *self,
                                      const guchar      *body,
                                      gsize              body_length)
{
    g_return_if_fail(self != NULL);

    g_free(self->payload);

    if (body == NULL || body_length == 0) {
        self->payload = NULL;
        return;
    }

    /*
     * g_strndup() stops at a NUL, which is what makes this the one place
     * an embedded NUL truncates the payload.  Doing it here rather than
     * leaving it to whichever reader called strlen() first means the
     * store, the prompt and the client all hold the same text.
     */
    self->payload = g_strndup((const gchar *)body, body_length);
}

void
clawt_trigger_event_set_identity(ClawtTriggerEvent *self,
                                 const gchar       *name,
                                 const gchar       *delivery_id)
{
    g_return_if_fail(self != NULL);

    if (name != NULL) {
        g_free(self->name);
        self->name = g_strdup(name);
    }

    if (delivery_id != NULL) {
        g_free(self->delivery_id);
        self->delivery_id = g_strdup(delivery_id);
    }
}

void
clawt_trigger_event_set_ref(ClawtTriggerEvent *self, const gchar *ref)
{
    g_return_if_fail(self != NULL);

    g_free(self->ref);
    g_free(self->branch);

    self->ref = g_strdup(ref);

    /*
     * The branch is derived here rather than by whoever filters on it.
     * `branch: master` is what a person writes and `refs/heads/master`
     * is what every forge sends, so a comparison against the raw ref is
     * a filter that matches nothing and says nothing about why.
     *
     * A tag keeps no branch at all -- `refs/tags/v1` is not a branch,
     * and calling it one would let a branch filter match a tag push.
     */
    if (ref != NULL && g_str_has_prefix(ref, "refs/heads/"))
        self->branch = g_strdup(ref + strlen("refs/heads/"));
    else if (ref != NULL && strchr(ref, '/') == NULL)
        self->branch = g_strdup(ref);
    else
        self->branch = NULL;
}

/*
 * Every placeholder a template may use, in one table.
 *
 * The docs, the client hints and the expander all read this, because a
 * documented placeholder the expander does not know is a template that
 * quietly keeps its braces, and a placeholder the expander knows that
 * nothing documents is one nobody uses.
 */
static const gchar *placeholders[] = {
    "event", "repo", "ref", "actor", "title", "url", "number"
};

const gchar *
clawt_trigger_event_placeholder(ClawtTriggerEvent *self, const gchar *name)
{
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(name != NULL, NULL);

    if (g_strcmp0(name, "event") == 0)
        return self->name;
    if (g_strcmp0(name, "repo") == 0)
        return self->repo;
    if (g_strcmp0(name, "ref") == 0)
        return self->ref;
    if (g_strcmp0(name, "actor") == 0)
        return self->actor;
    if (g_strcmp0(name, "title") == 0)
        return self->title;
    if (g_strcmp0(name, "url") == 0)
        return self->url;
    if (g_strcmp0(name, "number") == 0)
        return self->number;

    return NULL;
}

guint
clawt_trigger_event_placeholder_count(void)
{
    return G_N_ELEMENTS(placeholders);
}

const gchar *
clawt_trigger_event_placeholder_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(placeholders), "event");

    return placeholders[n];
}
