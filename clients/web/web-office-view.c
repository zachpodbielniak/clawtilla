/*
 * web-office-view.c - The drawn office on the web Office page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * In an OFFICE=1 build the page draws the office on a canvas from
 * office.layout and the world pack's own files.  In any other build it
 * says how to get it; the roster under it carries the same information.
 */

#include "web-pages.h"

#ifndef CLAWT_HAVE_OFFICE

void
clawt_web_office_view_add(ClawtWebApp *app, HtmxElement *parent)
{
    (void)app;
    clawt_web_add(parent, clawt_web_text(
        "The office itself is drawn in an OFFICE=1 build (`make OFFICE=1`). "
        "The roster below is the same information.", "lede"));
}

void
clawt_web_office_view_register(HtmxRouter *router, ClawtWebApp *app)
{
    (void)router;
    (void)app;
}

#endif /* !CLAWT_HAVE_OFFICE */
