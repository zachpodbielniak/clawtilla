/*
 * clawt-appearance.h - How the client looks, on this machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These live in the *client's* config rather than in clawtilla.yaml, for
 * the same reason connection profiles do -- and the reason is sharper
 * here, because the client can switch daemons while it is running.  Fonts
 * that came from the daemon's config would change when you connected to
 * a workstation and change back when you went home, which is nobody's
 * idea of a preference.
 *
 * They are also per-machine by nature: the size that is right on a
 * laptop is not the size that is right on a 4K panel, and the same
 * clawtilla.yaml is often the same fleet seen from both.
 *
 * Every value has an "unset" that means *defer to the system* rather
 * than a hardcoded fallback -- an empty font name, a zero size,
 * CLAWT_THEME_SYSTEM.  A client that shipped its own idea of a font
 * would override the one the person chose for their desktop, on a
 * machine where they had never opened this dialog.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtTheme:
 * @CLAWT_THEME_SYSTEM: follow the desktop's light/dark preference
 * @CLAWT_THEME_LIGHT: always light
 * @CLAWT_THEME_DARK: always dark
 *
 * Which colour scheme the client asks libadwaita for.
 */
typedef enum {
    CLAWT_THEME_SYSTEM = 0,
    CLAWT_THEME_LIGHT,
    CLAWT_THEME_DARK
} ClawtTheme;

#define CLAWT_TYPE_APPEARANCE (clawt_appearance_get_type())

typedef struct _ClawtAppearance ClawtAppearance;

GType clawt_appearance_get_type(void) G_GNUC_CONST;

/**
 * clawt_appearance_new:
 *
 * Appearance that defers to the system in every respect.
 *
 * Returns: (transfer full): a new #ClawtAppearance
 */
ClawtAppearance *clawt_appearance_new(void);

ClawtAppearance *clawt_appearance_copy(ClawtAppearance *self);
void             clawt_appearance_free(ClawtAppearance *self);

ClawtTheme   clawt_appearance_get_theme(ClawtAppearance *self);
void         clawt_appearance_set_theme(ClawtAppearance *self,
                                        ClawtTheme       theme);

/**
 * clawt_appearance_get_font:
 * @self: a #ClawtAppearance
 *
 * Returns: (nullable): the interface font family, or %NULL for the
 *   system's
 */
const gchar *clawt_appearance_get_font(ClawtAppearance *self);
void         clawt_appearance_set_font(ClawtAppearance *self,
                                       const gchar     *family);

/**
 * clawt_appearance_get_font_size:
 * @self: a #ClawtAppearance
 *
 * Returns: the interface font size in points, or 0 for the system's
 */
gdouble clawt_appearance_get_font_size(ClawtAppearance *self);
void    clawt_appearance_set_font_size(ClawtAppearance *self, gdouble points);

/**
 * clawt_appearance_get_monospace_font:
 * @self: a #ClawtAppearance
 *
 * The font for code blocks, inline code and the exec console.
 *
 * Returns: (nullable): the family, or %NULL for the system's monospace
 */
const gchar *clawt_appearance_get_monospace_font(ClawtAppearance *self);
void         clawt_appearance_set_monospace_font(ClawtAppearance *self,
                                                 const gchar     *family);

gdouble clawt_appearance_get_monospace_size(ClawtAppearance *self);
void    clawt_appearance_set_monospace_size(ClawtAppearance *self,
                                            gdouble          points);

/**
 * clawt_appearance_to_css:
 * @self: a #ClawtAppearance
 *
 * The stylesheet that applies these fonts.
 *
 * Emitted rather than applied so it can be asserted on without a display
 * -- which is the only way to check that an unset value produces *no*
 * rule at all, and therefore that the desktop's own font still wins.
 *
 * Returns: (transfer full): CSS, empty when nothing is set
 */
gchar *clawt_appearance_to_css(ClawtAppearance *self);

/**
 * clawt_appearance_default_path:
 *
 * Returns: (transfer full): `$XDG_CONFIG_HOME/clawtilla/appearance.yaml`
 */
gchar *clawt_appearance_default_path(void);

/**
 * clawt_appearance_parse:
 * @text: the contents of an appearance file
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the settings, or %NULL on a parse
 *   error
 */
ClawtAppearance *clawt_appearance_parse(const gchar *text, GError **error);

/**
 * clawt_appearance_to_data:
 * @self: a #ClawtAppearance
 *
 * Returns: (transfer full): the file contents
 */
gchar *clawt_appearance_to_data(ClawtAppearance *self);

/**
 * clawt_appearance_load:
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * A missing file is the defaults rather than an error: never having
 * opened the dialog is the ordinary state.
 *
 * Returns: (transfer full) (nullable): the settings, or %NULL if the file
 *   exists and could not be read
 */
ClawtAppearance *clawt_appearance_load(const gchar *path, GError **error);

/**
 * clawt_appearance_save:
 * @self: a #ClawtAppearance
 * @path: (nullable): the file, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if written
 */
gboolean clawt_appearance_save(ClawtAppearance *self,
                               const gchar     *path,
                               GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtAppearance, clawt_appearance_free)

G_END_DECLS
