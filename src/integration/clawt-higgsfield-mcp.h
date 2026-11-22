/*
 * clawt-higgsfield-mcp.h - Higgsfield generation through a connector's sign-in
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <ai-glib.h>
#include "integration/clawt-mcp-http-client.h"

G_BEGIN_DECLS

/**
 * CLAWT_HIGGSFIELD_MCP_DEFAULT_MODEL:
 *
 * The image model this road asks for when `office.asset_model` names
 * none.  A model id from Higgsfield's MCP catalogue, which is not the
 * REST API's endpoint path (`higgsfield-ai/soul/v2/standard` there).
 */
#define CLAWT_HIGGSFIELD_MCP_DEFAULT_MODEL "nano_banana_2"

/**
 * CLAWT_HIGGSFIELD_MCP_MAX_RESULT_BYTES:
 *
 * The largest result image the generator will download.
 */
#define CLAWT_HIGGSFIELD_MCP_MAX_RESULT_BYTES (48 * 1024 * 1024)

#define CLAWT_TYPE_HIGGSFIELD_MCP (clawt_higgsfield_mcp_get_type())

G_DECLARE_FINAL_TYPE(ClawtHiggsfieldMcp, clawt_higgsfield_mcp, CLAWT, HIGGSFIELD_MCP, GObject)

/**
 * clawt_higgsfield_mcp_new:
 * @client: (transfer none): a client for Higgsfield's MCP server, holding
 *   the connector's sign-in
 *
 * An #AiMediaGenerator that makes images with Higgsfield's own MCP tools
 * -- `estimate_image_cost`, `generate_image`, `job_status` -- so what it
 * spends is the signed-in account's *app credits*, the same ones the
 * `hf` CLI and the connector spend.  #AiHiggsfieldClient is the other
 * road: the REST API with a key pair, billed to a separate API balance.
 *
 * Its estimate is in credits, not dollars: estimate_finish() reports
 * %AI_MEDIA_COST_UNKNOWN micro-USD, and
 * clawt_higgsfield_mcp_get_estimate_credits() says what it cost.
 *
 * Returns: (transfer full): a new generator
 */
ClawtHiggsfieldMcp *clawt_higgsfield_mcp_new(ClawtMcpHttpClient *client);

/**
 * clawt_higgsfield_mcp_get_connector:
 * @self: a #ClawtHiggsfieldMcp
 *
 * Returns: (transfer none): the connector it signs in through
 */
const gchar *clawt_higgsfield_mcp_get_connector(ClawtHiggsfieldMcp *self);

/**
 * clawt_higgsfield_mcp_set_connector:
 * @self: a #ClawtHiggsfieldMcp
 * @name: the connector's name, for messages and provenance
 */
void clawt_higgsfield_mcp_set_connector(ClawtHiggsfieldMcp *self, const gchar *name);

/**
 * clawt_higgsfield_mcp_get_estimate_credits:
 * @self: a #ClawtHiggsfieldMcp
 *
 * Returns: the credits the last estimate quoted, or a negative number
 *   when none has been made or the server named none
 */
gdouble clawt_higgsfield_mcp_get_estimate_credits(ClawtHiggsfieldMcp *self);

/**
 * clawt_higgsfield_mcp_upload_async:
 * @self: a #ClawtHiggsfieldMcp
 * @data: the image
 * @filename: its name, whose extension tells Higgsfield its type
 * @mime: its type, e.g. "image/png"
 * @cancellable: (nullable): a #GCancellable
 * @callback: called with the media id
 * @user_data: for @callback
 *
 * Uploads a reference image: `media_upload` for a presigned URL, a PUT
 * of the bytes there (no credential: the URL is the permission), then
 * `media_confirm`.  Set the id it gives with
 * ai_media_request_set_image_url() and the generation starts from it.
 */
void clawt_higgsfield_mcp_upload_async(ClawtHiggsfieldMcp  *self,
                                       GBytes              *data,
                                       const gchar         *filename,
                                       const gchar         *mime,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);

/**
 * clawt_higgsfield_mcp_upload_finish:
 * @self: a #ClawtHiggsfieldMcp
 * @result: the result passed to the callback
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the media id
 */
gchar *clawt_higgsfield_mcp_upload_finish(ClawtHiggsfieldMcp  *self,
                                          GAsyncResult        *result,
                                          GError             **error);

/**
 * clawt_higgsfield_mcp_params_for:
 * @request: a media request
 *
 * The `params` object Higgsfield's image tools take for @request: its
 * model, prompt, a square aspect, and its image as a reference medium.
 * Pure, so what is sent is tested as data.
 *
 * Returns: (transfer full): the params
 */
JsonObject *clawt_higgsfield_mcp_params_for(AiMediaRequest *request);

G_END_DECLS
