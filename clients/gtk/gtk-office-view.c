/*
 * gtk-office-view.c - The drawn office, embedded in the Office page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The office is drawn by clawtilla-office, on libregnum.  GTK never
 * links libregnum: in an OFFICE=1 build this runs clawtilla-office as a
 * child with --embed, receives its frames over a socketpair and shows
 * them, and sends input back.  In any other build it says how to get it.
 */

#include "clawt-window-private.h"

#ifndef CLAWT_HAVE_OFFICE

void
clawt_gtk_office_view_attach(ClawtWindow *self)
{
    GtkWidget *note = gtk_label_new(
        "The office itself is drawn by clawtilla-office, which this build does "
        "not have: build with `make OFFICE=1 LRG_BACKEND=1`.  The roster below "
        "is the same information.");

    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_box_append(GTK_BOX(self->office_view_slot), note);
}

void
clawt_gtk_office_view_detach(ClawtWindow *self)
{
    (void)self;
}

#endif /* !CLAWT_HAVE_OFFICE */
