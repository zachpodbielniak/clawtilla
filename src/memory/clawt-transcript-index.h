/*
 * clawt-transcript-index.h - Searching every conversation the fleet had
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

#include <glib-object.h>

#include "chat/clawt-message.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TRANSCRIPT_HIT (clawt_transcript_hit_get_type())

/**
 * ClawtTranscriptHit:
 * @id: the message's id, so a caller can go and find it
 * @room_id: the room it was said in
 * @sender_id: who said it
 * @sender_name: (nullable): their display name at the time
 * @body: what was said, in full
 * @timestamp: unix seconds, which is what #ClawtMessage carries
 *
 * One message a search matched.
 *
 * A plain record with public fields, like #ClawtMemory: it is data, and
 * everything on it is read by whoever renders the result.
 *
 * @room_id and @sender_id are here because a hit has to be *filtered*
 * before it is shown.  A recall that returned the body without saying
 * where it came from could not be permission-checked by anything
 * downstream, and permission is the whole difficulty with searching
 * somebody else's conversations.
 */
typedef struct {
    gchar  *id;
    gchar  *room_id;
    gchar  *sender_id;
    gchar  *sender_name;
    gchar  *body;
    gint64  timestamp;
} ClawtTranscriptHit;

GType clawt_transcript_hit_get_type(void) G_GNUC_CONST;

/**
 * clawt_transcript_hit_copy:
 * @self: a #ClawtTranscriptHit
 *
 * Returns: (transfer full): a deep copy
 */
ClawtTranscriptHit *clawt_transcript_hit_copy(ClawtTranscriptHit *self);

/**
 * clawt_transcript_hit_free:
 * @self: (transfer full): a #ClawtTranscriptHit
 *
 * Frees it.
 */
void clawt_transcript_hit_free(ClawtTranscriptHit *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTranscriptHit, clawt_transcript_hit_free)

#define CLAWT_TYPE_TRANSCRIPT_INDEX (clawt_transcript_index_get_type())

G_DECLARE_FINAL_TYPE(ClawtTranscriptIndex, clawt_transcript_index, CLAWT,
                     TRANSCRIPT_INDEX, GObject)

/**
 * clawt_transcript_index_new:
 * @path: the database file
 * @error: (out) (optional): return location for a #GError
 *
 * Opens, and creates, the fleet's searchable transcript.
 *
 * One database for the whole fleet rather than one per agent, unlike
 * #ClawtMemoryStore: a conversation belongs to a *room*, and the same
 * room holds several agents.  So isolation cannot be a property of the
 * file here, and every read takes the rooms the caller may see -- see
 * clawt_transcript_index_search().
 *
 * Returns: (transfer full) (nullable): the index, or %NULL on failure
 */
ClawtTranscriptIndex *clawt_transcript_index_new(const gchar  *path,
                                                 GError      **error);

/**
 * clawt_transcript_index_add:
 * @self: a #ClawtTranscriptIndex
 * @room_id: the room the message landed in
 * @message: (transfer none): the message
 * @error: (out) (optional): return location for a #GError
 *
 * Records one message.
 *
 * @room_id is passed rather than read off @message because the router
 * resolves an agent id into the direct room between two agents, and the
 * message still names the agent.  Indexing what the message says would
 * file half the fleet's conversations under a room that does not exist.
 *
 * Writing the same message twice replaces the first, so re-indexing a
 * transcript at start is idempotent rather than a way to make every
 * search return everything three times.
 *
 * Returns: %TRUE if it was recorded
 */
gboolean clawt_transcript_index_add(ClawtTranscriptIndex  *self,
                                    const gchar           *room_id,
                                    ClawtMessage          *message,
                                    GError               **error);

/**
 * clawt_transcript_index_search:
 * @self: a #ClawtTranscriptIndex
 * @query: what to look for
 * @rooms: (nullable) (array zero-terminated=1): the rooms the caller may
 *   read, or %NULL for every room
 * @sender: (nullable): narrow to one agent's own messages
 * @since: only messages at or after this unix second, 0 for all
 * @limit: how many at most, 0 for a sensible default
 * @error: (out) (optional): return location for a #GError
 *
 * Full-text search across the fleet's conversations, newest first.
 *
 * @rooms is the permission check and it is not optional in the way a
 * nullable argument usually is: %NULL means *every room*, which is what
 * an operator's own client asks for and what an agent must never be
 * given.  Callers acting for an agent pass the rooms that agent is a
 * member of, so a room it is not in cannot appear in a result however
 * the query was spelled.
 *
 * @query is quoted as an FTS5 phrase literal.  An FTS5 query is syntax,
 * not a search string: a stray quote, a bare `NOT` or an unbalanced
 * paren is a parse error, and a failed search reports no matches -- which
 * is indistinguishable from a fleet that never said the word.
 *
 * Returns: (transfer full) (element-type ClawtTranscriptHit): the matches
 */
GPtrArray *clawt_transcript_index_search(ClawtTranscriptIndex  *self,
                                         const gchar           *query,
                                         const gchar * const   *rooms,
                                         const gchar           *sender,
                                         gint64                 since,
                                         guint                  limit,
                                         GError               **error);

/**
 * clawt_transcript_index_count:
 * @self: a #ClawtTranscriptIndex
 *
 * Returns: how many messages are indexed
 */
guint clawt_transcript_index_count(ClawtTranscriptIndex *self);

/**
 * clawt_transcript_index_has_full_text:
 * @self: a #ClawtTranscriptIndex
 *
 * Whether search is FTS5-ranked or a substring fallback.
 *
 * Returns: %TRUE when FTS5 is in use
 */
gboolean clawt_transcript_index_has_full_text(ClawtTranscriptIndex *self);

G_END_DECLS
