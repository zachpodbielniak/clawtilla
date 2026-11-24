/*
 * clawt-media.h - Paid media services (Meshy, PixelLab): the pure half
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_MEDIA_H
#define CLAWT_MEDIA_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_MEDIA_MAX_IMAGE_BYTES:
 *
 * The most of one picture, sheet or archive a media result may be,
 * enforced while it is read.
 */
#define CLAWT_MEDIA_MAX_IMAGE_BYTES (48 * 1024 * 1024)

/**
 * CLAWT_MEDIA_MAX_MODEL_BYTES:
 *
 * The most of one 3D model a media result may be (GLB, FBX, OBJ, USDZ),
 * enforced while it is read.  A textured 8K model is large; a model
 * larger than this is not one an office or an agent's next step needs.
 */
#define CLAWT_MEDIA_MAX_MODEL_BYTES (200 * 1024 * 1024)

/**
 * CLAWT_MEDIA_PIXELLAB_TYPICAL:
 *
 * What a PixelLab call usually costs, said when it is asked about:
 * PixelLab prices a call by the GPU time it took and publishes no table,
 * so there is no number to give before it runs.
 */
#define CLAWT_MEDIA_PIXELLAB_TYPICAL "most calls cost $0.01 to $0.08"

/**
 * clawt_media_service_for_tool:
 * @tool: a tool name
 *
 * Returns: (transfer none) (nullable): `meshy` or `pixellab` for one of
 *   their clawtilla_* tools, else %NULL
 */
const gchar *
clawt_media_service_for_tool(
	const gchar *tool
);

/**
 * clawt_media_service_label:
 * @service: `meshy` or `pixellab`
 *
 * Returns: (transfer none): the service's name as people write it
 */
const gchar *
clawt_media_service_label(
	const gchar *service
);

/**
 * clawt_media_request_new:
 * @service: `meshy` or `pixellab`
 * @arguments: a generate tool's arguments: `operation`, `prompt`,
 *   `model` and `params` (an object passed to the service untouched)
 * @error: (out) (optional): where a refusal is written
 *
 * The request a generate call makes, without its images (the daemon
 * reads those from the agent's own directories and adds them with
 * clawt_media_request_add_image()).  The operation is required and
 * becomes the request's `operation` parameter, which both clients read.
 *
 * Returns: (transfer full) (nullable): the request
 */
AiMediaRequest *
clawt_media_request_new(
	const gchar  *service,
	JsonObject   *arguments,
	GError      **error
);

/**
 * clawt_media_request_add_image:
 * @service: `meshy` or `pixellab`
 * @request: the request
 * @param: the service's parameter the picture is for (`image_url`,
 *   `init_image`, `reference_image`...)
 * @path: a file the caller has already checked the agent may read
 * @error: (out) (optional): where a refusal is written
 *
 * Adds a picture in the spelling the service takes: a data URI string
 * for Meshy, an image object for PixelLab.
 *
 * Returns: %TRUE when added
 */
gboolean
clawt_media_request_add_image(
	const gchar     *service,
	AiMediaRequest  *request,
	const gchar     *param,
	const gchar     *path,
	GError         **error
);

/**
 * clawt_media_cost_line:
 * @service: `meshy` or `pixellab`
 * @arguments: (nullable): a generate tool's arguments
 *
 * The cost sentence an approval card carries: Meshy's from its credit
 * table (ai_meshy_estimate_credits()), PixelLab's as what is known --
 * that the service prices it when it is done.
 *
 * Returns: (transfer full): the sentence
 */
gchar *
clawt_media_cost_line(
	const gchar *service,
	JsonObject  *arguments
);

/**
 * clawt_media_file_name:
 * @name: a named result (`glb`, `south:0`, `texture:base_color`)
 * @url: (nullable): where it is, for the extension
 *
 * A safe file name for a result: `:` and `/` become `-`, and the
 * extension comes from the URL's path, or from the name for a model
 * format, or `.png` for a `data:` image.  Never a path: the caller joins
 * it to the job's directory.
 *
 * Returns: (transfer full): the file name
 */
gchar *
clawt_media_file_name(
	const gchar *name,
	const gchar *url
);

/**
 * clawt_media_max_bytes_for:
 * @file_name: a result's file name from clawt_media_file_name()
 *
 * Returns: %CLAWT_MEDIA_MAX_MODEL_BYTES for a model format, else
 *   %CLAWT_MEDIA_MAX_IMAGE_BYTES
 */
gsize
clawt_media_max_bytes_for(
	const gchar *file_name
);

/**
 * clawt_media_provenance:
 * @service: `meshy` or `pixellab`
 * @operation: the operation
 * @prompt: (nullable): the prompt
 * @job_id: the service's job id
 * @cost: (nullable): what it cost, as said (`12 credits`, `$0.04`)
 * @files: (element-type utf8): the files written
 *
 * The `provenance.yaml` written beside a job's results: where they came
 * from, and that their use is governed by the service's terms.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
clawt_media_provenance(
	const gchar *service,
	const gchar *operation,
	const gchar *prompt,
	const gchar *job_id,
	const gchar *cost,
	GPtrArray   *files
);

/**
 * clawt_media_spend_path:
 * @state_dir: an agent's state directory
 *
 * Where an agent's paid media calls are recorded, one JSON line each:
 * what it cost in micro-dollars when the service said, and in credits
 * when it priced it that way.
 *
 * Returns: (transfer full): `<state_dir>/media-spend.ndjson`
 */
gchar *
clawt_media_spend_path(
	const gchar *state_dir
);

/**
 * clawt_media_spend_record:
 * @path: from clawt_media_spend_path()
 * @service: `meshy` or `pixellab`
 * @operation: the operation
 * @job_id: the service's job id
 * @micro_usd: what it cost in micro-dollars, or -1 when unknown
 * @credits: what it cost in the service's credits, or -1
 * @error: (out) (optional): where a failure is written
 *
 * Appends one call.  0600, like everything else that says what an agent
 * spent.
 *
 * Returns: %TRUE when written
 */
gboolean
clawt_media_spend_record(
	const gchar  *path,
	const gchar  *service,
	const gchar  *operation,
	const gchar  *job_id,
	gint64        micro_usd,
	gint64        credits,
	GError      **error
);

/**
 * clawt_media_spend_since:
 * @path: from clawt_media_spend_path()
 * @since: unix seconds; calls before are not counted
 * @credit_micros: what one Meshy credit costs in micro-dollars, or 0
 *   when unknown
 * @micro_usd: (out): the total in micro-dollars, credits converted where
 *   a price is known
 * @unpriced_credits: (out) (optional): credits that could not be priced
 * @calls: (out) (optional): how many calls
 *
 * Sums an agent's paid media calls.  A missing file is zero.  A line
 * that does not parse is skipped, never a reason to report nothing.
 */
void
clawt_media_spend_since(
	const gchar *path,
	gint64       since,
	gint64       credit_micros,
	gint64      *micro_usd,
	gint64      *unpriced_credits,
	gint64      *calls
);

G_END_DECLS

#endif /* CLAWT_MEDIA_H */
