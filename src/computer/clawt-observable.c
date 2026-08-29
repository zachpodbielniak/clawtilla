/*
 * clawt-observable.c - A computer that can show you what is on its screen
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-observable.h"

/* ── ClawtInputEvent ─────────────────────────────────────────────── */

G_DEFINE_BOXED_TYPE(ClawtInputEvent, clawt_input_event,
                    clawt_input_event_copy, clawt_input_event_free)

ClawtInputEvent *
clawt_input_event_new(ClawtInputKind kind)
{
    ClawtInputEvent *self = g_new0(ClawtInputEvent, 1);

    self->kind = kind;
    self->button = 1;

    return self;
}

ClawtInputEvent *
clawt_input_event_copy(ClawtInputEvent *self)
{
    ClawtInputEvent *copy;

    if (self == NULL)
        return NULL;

    copy = g_new0(ClawtInputEvent, 1);
    *copy = *self;
    copy->text = g_strdup(self->text);

    return copy;
}

void
clawt_input_event_free(ClawtInputEvent *self)
{
    if (self == NULL)
        return;

    g_free(self->text);
    g_free(self);
}

void
clawt_input_event_set_text(ClawtInputEvent *self, const gchar *text)
{
    g_return_if_fail(self != NULL);

    g_free(self->text);
    self->text = g_strdup(text);
}

gchar *
clawt_input_event_describe(ClawtInputEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    switch (self->kind) {
    case CLAWT_INPUT_KEY:
        return g_strdup_printf("key %s",
                               self->text != NULL ? self->text : "?");

    case CLAWT_INPUT_TEXT:
        /*
         * The length and not the text.  A takeover is a person driving
         * their own desktop; what they type into it is theirs, and a
         * trail that recorded it would be a keylogger written by
         * accident in the one place nobody thought to look for one.
         */
        return g_strdup_printf("typed %" G_GSIZE_FORMAT " characters",
                               self->text != NULL
                               ? g_utf8_strlen(self->text, -1) : 0);

    case CLAWT_INPUT_CLICK:
        return g_strdup_printf("click button %u at %d,%d",
                               self->button, self->x, self->y);

    case CLAWT_INPUT_MOVE:
        return g_strdup_printf("move to %d,%d", self->x, self->y);

    case CLAWT_INPUT_SCROLL:
        return g_strdup_printf("scroll %.0f,%.0f at %d,%d",
                               self->dx, self->dy, self->x, self->y);
    }

    return g_strdup("an unknown event");
}

/* ── The interface ───────────────────────────────────────────────── */

G_DEFINE_INTERFACE(ClawtObservable, clawt_observable, G_TYPE_OBJECT)

/*
 * The default for every vfunc is a refusal that names the type.
 *
 * Not %TRUE, and not silence.  A backend that half-implements this --
 * frames but no input, say -- is an ordinary thing to have, and the
 * half it does not implement has to be *reported* rather than answered
 * successfully: CALL_OR_TRUE on ClawtComputer once reported a VM as
 * removed while its domain and its disk stayed on disk, and this is the
 * same shape one layer up.
 */
static gboolean
default_start(ClawtObservable *self, guint fps, GError **error)
{
    (void)fps;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "%s cannot show a screen", G_OBJECT_TYPE_NAME(self));

    return FALSE;
}

static void
default_stop(ClawtObservable *self)
{
    (void)self;
}

static GBytes *
default_frame(ClawtObservable  *self,
              const gchar      *if_changed_from,
              gint64           *stamp_out,
              gchar           **hash_out,
              GError          **error)
{
    (void)if_changed_from;

    if (stamp_out != NULL)
        *stamp_out = 0;

    if (hash_out != NULL)
        *hash_out = NULL;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "%s has no screen to photograph", G_OBJECT_TYPE_NAME(self));

    return NULL;
}

static gboolean
default_can_input(ClawtObservable *self)
{
    (void)self;

    return FALSE;
}

static gboolean
default_send_input(ClawtObservable  *self,
                   ClawtInputEvent  *event,
                   GError          **error)
{
    (void)event;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "%s does not take keyboard or pointer events",
                G_OBJECT_TYPE_NAME(self));

    return FALSE;
}

static gchar *
default_viewer_uri(ClawtObservable *self)
{
    (void)self;

    return NULL;
}

static gboolean
default_geometry(ClawtObservable *self, guint *width, guint *height)
{
    (void)self;

    if (width != NULL)
        *width = 0;

    if (height != NULL)
        *height = 0;

    return FALSE;
}

static void
clawt_observable_default_init(ClawtObservableInterface *iface)
{
    iface->observe_start = default_start;
    iface->observe_stop = default_stop;
    iface->observe_frame = default_frame;
    iface->observe_can_input = default_can_input;
    iface->observe_send_input = default_send_input;
    iface->observe_viewer_uri = default_viewer_uri;
    iface->observe_geometry = default_geometry;
}

gboolean
clawt_observable_start(ClawtObservable *self, guint fps, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), FALSE);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_start(self, fps, error);
}

void
clawt_observable_stop(ClawtObservable *self)
{
    g_return_if_fail(CLAWT_IS_OBSERVABLE(self));

    CLAWT_OBSERVABLE_GET_IFACE(self)->observe_stop(self);
}

GBytes *
clawt_observable_frame(ClawtObservable  *self,
                       const gchar      *if_changed_from,
                       gint64           *stamp_out,
                       gchar           **hash_out,
                       GError          **error)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), NULL);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_frame(
        self, if_changed_from, stamp_out, hash_out, error);
}

gboolean
clawt_observable_can_input(ClawtObservable *self)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), FALSE);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_can_input(self);
}

gboolean
clawt_observable_send_input(ClawtObservable  *self,
                            ClawtInputEvent  *event,
                            GError          **error)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_send_input(self, event,
                                                                error);
}

gchar *
clawt_observable_viewer_uri(ClawtObservable *self)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), NULL);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_viewer_uri(self);
}

gboolean
clawt_observable_geometry(ClawtObservable *self, guint *width, guint *height)
{
    g_return_val_if_fail(CLAWT_IS_OBSERVABLE(self), FALSE);

    return CLAWT_OBSERVABLE_GET_IFACE(self)->observe_geometry(self, width,
                                                              height);
}

guint
clawt_observe_clamp_fps(gint64 fps)
{
    /*
     * Zero means the default, not "never".
     *
     * An integer key nobody set reads as zero here, and a client that
     * subscribed and then received no frame at all would have nothing
     * on screen and nothing to explain it -- which reads as the whole
     * feature being broken rather than as a rate of nought.
     */
    if (fps <= 0)
        return 1;

    if (fps > CLAWT_OBSERVE_MAX_FPS)
        return CLAWT_OBSERVE_MAX_FPS;

    return (guint)fps;
}

gboolean
clawt_frame_is_stale(gint64 stamp_us, gint64 now_us)
{
    /*
     * No frame is not a stale frame.  A client that treated the two the
     * same would draw "55 years ago" over an empty panel, which is both
     * wrong and the sort of wrong that sends somebody looking at clocks.
     */
    if (stamp_us <= 0)
        return FALSE;

    return (now_us - stamp_us) >
           ((gint64)CLAWT_FRAME_STALE_SECONDS * G_USEC_PER_SEC);
}
