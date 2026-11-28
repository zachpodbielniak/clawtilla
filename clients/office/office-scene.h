/*
 * office-scene.h - The drawn office: a world pack and who is in it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One scene, drawn by clawtilla-office (in a window, offscreen for a
 * screenshot, or streamed to the GTK client) and by the lrg client's
 * Office page in its own process.  It knows nothing of the daemon: the
 * feed (office-feed.h) hands it a pack directory, the world's JSON, the
 * director's layout and each agent's colour, and it draws them.  Every
 * pose it draws came from office.layout; it invents nothing, except in
 * simulate mode, which says so on screen.
 */

#ifndef CLAWT_OFFICE_SCENE_H
#define CLAWT_OFFICE_SCENE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <clawtilla.h>

G_BEGIN_DECLS

/**
 * ClawtOfficeSceneMode:
 * @CLAWT_OFFICE_SCENE_2D: the Tiled map, from above
 * @CLAWT_OFFICE_SCENE_3D: the libregnum level, from an orbiting god's-eye camera
 *
 * How the office is drawn.
 */
typedef enum {
    CLAWT_OFFICE_SCENE_2D,
    CLAWT_OFFICE_SCENE_3D
} ClawtOfficeSceneMode;

typedef struct _ClawtOfficeScene ClawtOfficeScene;

/**
 * clawt_office_scene_new:
 *
 * Returns: (transfer full): an empty scene, drawing a "loading" line
 */
ClawtOfficeScene *clawt_office_scene_new(void);

/**
 * clawt_office_scene_free:
 * @self: (nullable): a scene
 */
void clawt_office_scene_free(ClawtOfficeScene *self);

/**
 * clawt_office_scene_load_world:
 * @self: a scene
 * @pack_dir: a local directory holding the pack's files
 * @world: the world, as `office.world` answers it
 * @error: return location for a #GError
 *
 * Loads the pack's 2D map (and 3D level when it has one).  Needs a GL
 * context: textures are loaded here.
 *
 * Returns: %TRUE when at least one view loaded
 */
gboolean clawt_office_scene_load_world(ClawtOfficeScene  *self,
                                       const gchar       *pack_dir,
                                       JsonObject        *world,
                                       GError           **error);

/**
 * CLAWT_OFFICE_ADAPTER_SYMBOL:
 *
 * What a world-adapter module exports (docs/office-adapters.org):
 * `LrgWorldAdapter *clawt_office_adapter_new (const gchar *pack_dir,
 * GError **error)`.
 */
#define CLAWT_OFFICE_ADAPTER_SYMBOL "clawt_office_adapter_new"

/**
 * clawt_office_scene_set_adapter_dirs:
 * @self: a scene
 * @dirs: (nullable) (array zero-terminated=1): where adapter modules are
 *   looked for, before the default places
 */
void clawt_office_scene_set_adapter_dirs(ClawtOfficeScene *self, const gchar *const *dirs);

/**
 * clawt_office_scene_get_adapter_name:
 * @self: a scene
 *
 * Returns: (transfer none) (nullable): the world adapter drawing the
 *   office, or %NULL when the scene draws the pack itself
 */
const gchar *clawt_office_scene_get_adapter_name(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_layout:
 * @self: a scene
 * @layout: `office.layout`'s answer: `placements` and `participants`
 *
 * Moves everybody towards where the director put them.  A participant
 * not in @layout leaves; a new one walks in from the entrance.
 */
void clawt_office_scene_set_layout(ClawtOfficeScene *self, JsonObject *layout);

/**
 * clawt_office_scene_set_color:
 * @self: a scene
 * @id: a participant's id
 * @color: (nullable): its colour, `#rgb` or `#rrggbb`; %NULL for a derived one
 */
void clawt_office_scene_set_color(ClawtOfficeScene *self, const gchar *id,
                                  const gchar *color);

/**
 * clawt_office_scene_set_look:
 * @self: a scene
 * @id: a participant's id
 * @name: (nullable): the name the letter is taken from
 * @face: (nullable): the resolved face name, or %NULL for the generated one
 * @has_avatar: %TRUE when a profile picture is stored
 *
 * Which face @id wears, from the same rule as the web office
 * (clawt_office_face_resolve()).  A picture is drawn once
 * clawt_office_scene_set_portrait() has handed over its pixels; until
 * then the colour blob stands in.  A character sheet still wins.
 */
void clawt_office_scene_set_look(ClawtOfficeScene *self, const gchar *id,
                                 const gchar *name, const gchar *face,
                                 gboolean has_avatar);

/**
 * clawt_office_scene_set_portrait:
 * @self: a scene
 * @id: a participant's id
 * @rgba: (nullable) (array): square straight RGBA, or %NULL to drop the picture
 * @size: the square's side; ignored when @rgba is %NULL
 * @etag: (nullable): the bytes' tag; the same tag is not copied again
 *
 * The picture @id wears.  Copied: @rgba need only last for the call.
 * Uploaded at the next clawt_office_scene_prepare(), never from inside a
 * host's render texture.  Drawn with a bilinear filter, at the size the
 * head already is, so a pixel-art world keeps it legible at Fit.
 */
void clawt_office_scene_set_portrait(ClawtOfficeScene *self, const gchar *id,
                                     const guint8 *rgba, gint size,
                                     const gchar *etag);

/**
 * clawt_office_scene_face_kind:
 * @self: a scene
 * @id: a participant's id
 *
 * What @id is drawn as, from the look, the portrait and any character
 * sheet.  Needs no GL context: it is the decision, not the texture.
 *
 * Returns: the kind clawt_office_face_resolve() would answer
 */
ClawtOfficeFaceKind clawt_office_scene_face_kind(ClawtOfficeScene *self,
                                                 const gchar *id);

/**
 * clawt_office_scene_set_character:
 * @self: a scene
 * @id: an agent's id
 * @dir: a local directory holding its files at the paths @character names
 * @character: its layout, as `office.characters` gives it
 *
 * Draws @id with its own character instead of the default body: its
 * sheet as a sprite in 2D; in 3D its mesh when it has one, otherwise the
 * sheet's frame on a billboard facing the camera.  The texture is
 * loaded at the next clawt_office_scene_prepare().
 */
void clawt_office_scene_set_character(ClawtOfficeScene *self, const gchar *id,
                                      const gchar *dir, JsonObject *character);

/**
 * clawt_office_scene_set_usage:
 * @self: a scene
 * @usage: (nullable): `usage.providers`' answer, or %NULL for none
 *
 * What each provider login has left, drawn on the wall the pack tags
 * `usage_board` and in the details panel -- the same words, meters and
 * tones as the Fleet page, from the same library calls
 * (clawt_office_board_new()).  A pack with no such wall draws no board;
 * the corner panel needs none.
 */
void clawt_office_scene_set_usage(ClawtOfficeScene *self, JsonObject *usage);

/**
 * clawt_office_scene_set_usage_placement:
 * @self: a scene
 * @placement: on the wall, docked in the corner, or off
 *
 * Where provider usage is drawn.  On the wall
 * (%CLAWT_OFFICE_USAGE_WORLD) the board is fitted to the `usage_board`
 * zone and hovering or clicking it opens the details panel beside it;
 * in the corner the same panel is docked at the top right and always
 * shown; off, nothing is.  A change closes a pinned panel.
 */
void clawt_office_scene_set_usage_placement(ClawtOfficeScene          *self,
                                            ClawtOfficeUsagePlacement  placement);

/**
 * clawt_office_scene_get_usage_placement:
 * @self: a scene
 *
 * Returns: where provider usage is drawn
 */
ClawtOfficeUsagePlacement clawt_office_scene_get_usage_placement(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_usage_open:
 * @self: a scene
 * @open: %TRUE to pin the details panel open beside the board
 *
 * What a click on the board does; for a screenshot of the panel.
 */
void clawt_office_scene_set_usage_open(ClawtOfficeScene *self, gboolean open);

/**
 * clawt_office_scene_get_usage_open:
 * @self: a scene
 *
 * Returns: whether the details panel is shown: pinned, hovered, or docked
 */
gboolean clawt_office_scene_get_usage_open(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_clock:
 * @self: a scene
 * @now: Unix seconds to word the board at ("read 2m ago", "resets in
 *   2h"), or 0 for the real clock
 *
 * A fixed clock, for drawing a usage fixture the same way every time.
 */
void clawt_office_scene_set_clock(ClawtOfficeScene *self, gint64 now);

/**
 * clawt_office_scene_click:
 * @self: a scene
 * @x: the pointer, in target pixels
 * @y: likewise
 *
 * A click, for the usage board before anybody under it: on the board it
 * pins the details panel open (or closes it); on the panel it does
 * nothing more; away from a pinned panel it closes it.  A host opens the
 * participant under the pointer only when this returns %FALSE.
 *
 * Returns: whether the board took the click
 */
gboolean clawt_office_scene_click(ClawtOfficeScene *self, gfloat x, gfloat y);

/**
 * clawt_office_scene_dismiss:
 * @self: a scene
 *
 * Escape: closes the details panel beside the board, if it is open.  A
 * docked panel is not closed this way; the placement turns it off.
 *
 * Returns: whether a panel was closed (the key was used)
 */
gboolean clawt_office_scene_dismiss(ClawtOfficeScene *self);

/**
 * clawt_office_scene_describe_usage:
 * @self: a scene
 *
 * What the last frame drew of provider usage: the view's size, the
 * placement, the `usage_board` zone on screen (2D), and the board's and
 * panel's layouts -- their level of detail, rect, how many did not fit,
 * and every item with its rect (and a text's size and column).  For
 * clawtilla-office's `--usage-report` and the screenshot test.
 *
 * Returns: (transfer full): a new object
 */
JsonObject *clawt_office_scene_describe_usage(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_mode:
 * @self: a scene
 * @mode: 2D or 3D
 *
 * A 3D request on a pack without a level stays 2D.
 */
void clawt_office_scene_set_mode(ClawtOfficeScene *self, ClawtOfficeSceneMode mode);

/**
 * clawt_office_scene_get_mode:
 * @self: a scene
 *
 * Returns: the mode actually drawn
 */
ClawtOfficeSceneMode clawt_office_scene_get_mode(ClawtOfficeScene *self);

/**
 * clawt_office_scene_has_3d:
 * @self: a scene
 *
 * Returns: whether the loaded world has a 3D level to draw
 */
gboolean clawt_office_scene_has_3d(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_zoom:
 * @self: a scene
 * @zoom: Fit, Pixel-perfect, 1x or 2x
 *
 * How far the 2D map is zoomed (clawt_office_view_map_zoom()): Fit is the
 * largest scale at which the whole map fits, fractional included.
 * Pixel-perfect is that scale floored to a whole number.  In 3D, Fit and
 * Pixel-perfect set the camera's distance to frame the office, and 1x
 * and 2x come closer; the wheel then changes it.  Either way a drag's
 * panning is forgotten.
 */
void clawt_office_scene_set_zoom(ClawtOfficeScene *self, ClawtOfficeZoom zoom);

/**
 * clawt_office_scene_get_zoom:
 * @self: a scene
 *
 * Returns: the zoom last set
 */
ClawtOfficeZoom clawt_office_scene_get_zoom(ClawtOfficeScene *self);

/**
 * clawt_office_scene_set_ui_scale:
 * @self: a scene
 * @ui_scale: device pixels per logical pixel (1 to
 *   %CLAWT_OFFICE_VIEW_MAX_UI_SCALE)
 *
 * The host's device scale.  Text, labels and markers are drawn this much
 * larger, so a frame drawn at a 4K screen's device size reads like one
 * at 1080p; 1x and 2x zoom are logical pixels per map pixel.
 */
void clawt_office_scene_set_ui_scale(ClawtOfficeScene *self, gint ui_scale);

/**
 * clawt_office_scene_pan:
 * @self: a scene
 * @dx: how far the pointer dragged, in target pixels
 * @dy: likewise
 *
 * Drags a 2D map that is larger than the view, within its edges.  Does
 * nothing in 3D (a drag turns the camera there) or at a zoom where the
 * whole map is in view.
 */
void clawt_office_scene_pan(ClawtOfficeScene *self, gfloat dx, gfloat dy);

/**
 * clawt_office_scene_set_status:
 * @self: a scene
 * @text: (nullable): a line drawn over the office ("The office is off",
 *   "Simulation"), or %NULL for none
 */
void clawt_office_scene_set_status(ClawtOfficeScene *self, const gchar *text);

/**
 * clawt_office_scene_prepare:
 * @self: a scene
 *
 * Draws the map once into a texture of its own.  Called by
 * clawt_office_scene_update(); a host that updates from inside its own
 * render texture (raylib's texture modes do not nest) calls this first,
 * outside it, after loading a world.
 */
void clawt_office_scene_prepare(ClawtOfficeScene *self);

/**
 * clawt_office_scene_update:
 * @self: a scene
 * @delta: seconds since the last update
 *
 * Walks, fades and turns.  Returns %TRUE while anything is still moving,
 * so a host that idles may stop drawing when it is %FALSE.
 *
 * Returns: whether the scene is animating
 */
gboolean clawt_office_scene_update(ClawtOfficeScene *self, gfloat delta);

/**
 * clawt_office_scene_draw:
 * @self: a scene
 * @width: the target's width in pixels
 * @height: the target's height in pixels
 *
 * Draws into whatever is current (the screen between BeginDrawing() and
 * EndDrawing(), or a render texture's mode), filling @width x @height.
 */
void clawt_office_scene_draw(ClawtOfficeScene *self, gint width, gint height);

/**
 * clawt_office_scene_pointer:
 * @self: a scene
 * @x: the pointer, in target pixels
 * @y: the pointer, in target pixels
 *
 * Hovers whoever is under the pointer: their full name is shown.
 *
 * Returns: (transfer none) (nullable): the id of the participant under
 *   the pointer
 */
const gchar *clawt_office_scene_pointer(ClawtOfficeScene *self, gfloat x, gfloat y);

/**
 * clawt_office_scene_orbit:
 * @self: a scene
 * @dx: how far to turn the 3D camera, in radians
 * @zoom: a zoom step: positive in, negative out, 0 for none
 */
void clawt_office_scene_orbit(ClawtOfficeScene *self, gfloat dx, gfloat zoom);

/**
 * clawt_office_scene_set_focus:
 * @self: a scene
 * @id: (nullable): a participant to follow, or %NULL for the whole office
 *
 * The 3D camera centres on @id and comes in close, following it as it
 * walks.  In 2D a map zoomed past the view is centred on @id; at Fit the
 * whole map shows anyway.  Its name label is always drawn.  %NULL or ""
 * is nobody.
 */
void clawt_office_scene_set_focus(ClawtOfficeScene *self, const gchar *id);

/**
 * clawt_office_scene_units_ratio:
 * @mesh_height: the height of a skinned model's mesh, as loaded
 * @skeleton_height: the height its bind-pose skeleton spans
 *
 * By what the mesh must be multiplied to share its skeleton's units.
 * raylib applies a skinned mesh node's world transform to the vertices,
 * which glTF says a skinned mesh ignores; Meshy's rigs put the whole
 * model under an `Armature` scaled 0.01, so the mesh loads a hundred
 * times smaller than the bones that move it, and the first walk frame
 * drew the character hundreds of metres tall.  A unit mistake is a power
 * of ten, so that is what is answered; heights within a factor of five
 * of each other are taken to agree (a skeleton stops short of the hair).
 *
 * Returns: 1, or the power of ten that brings the mesh to the skeleton
 */
gfloat clawt_office_scene_units_ratio(gfloat mesh_height, gfloat skeleton_height);

/**
 * clawt_office_scene_n_participants:
 * @self: a scene
 *
 * Returns: how many are drawn
 */
guint clawt_office_scene_n_participants(ClawtOfficeScene *self);

/**
 * clawt_office_simulate_layout:
 * @world: the world, as `office.world` answers it
 * @layout: (transfer none): a real layout; its participants are used
 * @rng: a random source
 *
 * A plausible layout for the real participants -- calls, a meeting,
 * somebody waiting, somebody broken -- seated on the world's own
 * anchors.  For simulate mode only: it is drawn with a "Simulation"
 * status line and never sent anywhere.
 *
 * Returns: (transfer full): a new layout object
 */
JsonObject *clawt_office_simulate_layout(JsonObject *world, JsonObject *layout, GRand *rng);

G_END_DECLS

#endif /* CLAWT_OFFICE_SCENE_H */
