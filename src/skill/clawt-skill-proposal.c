/*
 * clawt-skill-proposal.c - A skill an agent wrote, waiting for a person
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Each proposal is a directory under the store:
 *
 *   <directory>/<id>/proposal.yaml   who, what, why, and where it stands
 *   <directory>/<id>/SKILL.md        the text exactly as it was proposed
 *
 * The second file is the reviewed text.  For a new skill the draft also
 * lives in the library, disabled, because that is where teach's commit
 * puts it; the copy here is what approval compares the library's file
 * against, so a draft rewritten after it was shown to somebody is not
 * the one that gets enabled.
 */

#include "clawtilla.h"

#include "skill/clawt-skill-proposal.h"
#include "skill/clawt-skill-scan.h"
#include "chat/clawt-preamble.h"

#include <string.h>

#include <yaml-glib.h>

/* What the record beside each draft is called. */
#define RECORD_NAME "proposal.yaml"

/* The text that was proposed, kept beside the record. */
#define DRAFT_NAME "SKILL.md"

/* How long a settled proposal is remembered, in microseconds. */
#define SETTLED_KEEP_USEC ((gint64)7 * 24 * G_TIME_SPAN_HOUR)

/* The window the per-agent rate limit counts over, in microseconds. */
#define RATE_WINDOW_USEC ((gint64)24 * G_TIME_SPAN_HOUR)

/*
 * How many cells the diff's match table may have.  Past this the middle
 * of the two texts is shown as removed and re-added whole: still a true
 * diff, and a bounded amount of work whatever an agent sends.
 */
#define DIFF_MAX_CELLS ((gsize)1000000)

/* Lines of context either side of a change. */
#define DIFF_CONTEXT (3)

/* ── The record ──────────────────────────────────────────────────── */

struct _ClawtSkillProposal {
	gchar                   *id;
	gchar                   *agent;
	gchar                   *name;
	gchar                   *reason;
	gchar                   *base_digest;   /* the live skill when an update was proposed */
	gchar                   *draft_digest;  /* the draft as it was proposed */
	gboolean                 is_update;
	gint64                   created_at;    /* microseconds */
	gint64                   settled_at;    /* microseconds, 0 while pending */
	ClawtSkillProposalState  state;
};

G_DEFINE_BOXED_TYPE(ClawtSkillProposal, clawt_skill_proposal,
                    clawt_skill_proposal_copy, clawt_skill_proposal_free)

static ClawtSkillProposal *
proposal_new(void)
{
	return g_new0(ClawtSkillProposal, 1);
}

ClawtSkillProposal *
clawt_skill_proposal_copy(
	ClawtSkillProposal *self
){
	ClawtSkillProposal *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = proposal_new();
	copy->id = g_strdup(self->id);
	copy->agent = g_strdup(self->agent);
	copy->name = g_strdup(self->name);
	copy->reason = g_strdup(self->reason);
	copy->base_digest = g_strdup(self->base_digest);
	copy->draft_digest = g_strdup(self->draft_digest);
	copy->is_update = self->is_update;
	copy->created_at = self->created_at;
	copy->settled_at = self->settled_at;
	copy->state = self->state;

	return copy;
}

void
clawt_skill_proposal_free(
	ClawtSkillProposal *self
){
	if (self == NULL)
		return;

	g_free(self->id);
	g_free(self->agent);
	g_free(self->name);
	g_free(self->reason);
	g_free(self->base_digest);
	g_free(self->draft_digest);
	g_free(self);
}

const gchar *
clawt_skill_proposal_get_id(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->id;
}

const gchar *
clawt_skill_proposal_get_agent(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->agent;
}

const gchar *
clawt_skill_proposal_get_name(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->name;
}

gboolean
clawt_skill_proposal_is_update(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, FALSE);

	return self->is_update;
}

const gchar *
clawt_skill_proposal_get_reason(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->reason;
}

gint64
clawt_skill_proposal_get_created_at(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->created_at;
}

gint64
clawt_skill_proposal_get_settled_at(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->settled_at;
}

ClawtSkillProposalState
clawt_skill_proposal_get_state(
	ClawtSkillProposal *self
){
	g_return_val_if_fail(self != NULL, CLAWT_SKILL_PROPOSAL_PENDING);

	return self->state;
}

/*
 * No default: a state added later is a -Wswitch warning here rather than
 * a record written with a nickname the reader does not know.
 */
const gchar *
clawt_skill_proposal_state_to_string(
	ClawtSkillProposalState state
){
	switch (state) {
	case CLAWT_SKILL_PROPOSAL_PENDING:
		return "pending";
	case CLAWT_SKILL_PROPOSAL_APPROVED:
		return "approved";
	case CLAWT_SKILL_PROPOSAL_EDITED:
		return "edited";
	case CLAWT_SKILL_PROPOSAL_REJECTED:
		return "rejected";
	}

	return "pending";
}

static gboolean
state_from_string(
	const gchar             *text,
	ClawtSkillProposalState *out
){
	static const ClawtSkillProposalState states[] = {
		CLAWT_SKILL_PROPOSAL_PENDING,
		CLAWT_SKILL_PROPOSAL_APPROVED,
		CLAWT_SKILL_PROPOSAL_EDITED,
		CLAWT_SKILL_PROPOSAL_REJECTED
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(states); i++) {
		if (g_strcmp0(text, clawt_skill_proposal_state_to_string(states[i])) == 0) {
			*out = states[i];
			return TRUE;
		}
	}

	return FALSE;
}

gboolean
clawt_skill_proposal_id_is_valid(
	const gchar *id
){
	const gchar *p;
	gsize length;
	gsize i;

	if (id == NULL || !g_str_has_prefix(id, CLAWT_SKILL_PROPOSAL_ID_PREFIX))
		return FALSE;

	/*
	 * Exactly the shape g_uuid_string_random() produces: 36 characters
	 * of lowercase hex with hyphens at four fixed places.  Anything
	 * looser is a string a client chose being joined onto a directory.
	 */
	p = id + strlen(CLAWT_SKILL_PROPOSAL_ID_PREFIX);
	length = strlen(p);

	if (length != 36)
		return FALSE;

	for (i = 0; i < length; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (p[i] != '-')
				return FALSE;
			continue;
		}

		if (!g_ascii_isxdigit(p[i]) || g_ascii_isupper(p[i]))
			return FALSE;
	}

	return TRUE;
}

/* ── Showing untrusted text ──────────────────────────────────────── */

gchar *
clawt_skill_proposal_fence(
	const gchar *text,
	const gchar *info
){
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *fence = NULL;
	const gchar *p;
	guint run;
	guint longest;
	gsize length;

	escaped = clawt_preamble_escape(text != NULL ? text : "");

	/* The longest run of backticks anywhere, so no line can close the fence. */
	run = 0;
	longest = 0;
	for (p = escaped; *p != '\0'; p++) {
		if (*p == '`') {
			run++;
			if (run > longest)
				longest = run;
		} else {
			run = 0;
		}
	}

	fence = g_strnfill(MAX((gsize)3, (gsize)longest + 1), '`');
	length = strlen(escaped);

	return g_strdup_printf("%s%s\n%s%s%s",
	                       fence, info != NULL ? info : "",
	                       escaped,
	                       (length > 0 && escaped[length - 1] != '\n') ? "\n" : "",
	                       fence);
}

/* ── Diff ────────────────────────────────────────────────────────── */

/*
 * Lines of @text, without their newlines.  A final newline ends the last
 * line rather than starting an empty one, so "a\n" is one line, as it is
 * to every diff tool a reader has used.
 */
static GPtrArray *
split_lines(
	const gchar *text
){
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	const gchar *start;
	const gchar *p;

	if (text == NULL)
		return lines;

	start = text;
	for (p = text; *p != '\0'; p++) {
		if (*p == '\n') {
			g_ptr_array_add(lines, g_strndup(start, (gsize)(p - start)));
			start = p + 1;
		}
	}

	if (*start != '\0')
		g_ptr_array_add(lines, g_strdup(start));

	return lines;
}

/* One step of the edit script: ' ' kept, '-' removed, '+' added. */
typedef struct {
	gchar  op;
	guint  old_index;   /* the line in @before, for ' ' and '-' */
	guint  new_index;   /* the line in @after, for ' ' and '+' */
} DiffStep;

static void
diff_add_step(
	GArray *steps,
	gchar   op,
	guint   old_index,
	guint   new_index
){
	DiffStep step;

	step.op = op;
	step.old_index = old_index;
	step.new_index = new_index;
	g_array_append_val(steps, step);
}

/*
 * The edit script between two line arrays.
 *
 * The common head and tail are matched first, because almost every
 * update touches a few lines in the middle of a file and the table below
 * then covers only those.  The middle is a longest-common-subsequence
 * table filled from the end, walked forwards; one too big for the budget
 * is removed and re-added whole.
 */
static GArray *
diff_steps(
	GPtrArray *a,
	GPtrArray *b
){
	GArray *steps = g_array_new(FALSE, FALSE, sizeof(DiffStep));
	guint n = a->len;
	guint m = b->len;
	guint head = 0;
	guint tail = 0;
	guint mid_n;
	guint mid_m;
	guint i;
	guint j;

	while (head < n && head < m &&
	       g_strcmp0(g_ptr_array_index(a, head), g_ptr_array_index(b, head)) == 0)
		head++;

	while (tail < n - head && tail < m - head &&
	       g_strcmp0(g_ptr_array_index(a, n - 1 - tail),
	                 g_ptr_array_index(b, m - 1 - tail)) == 0)
		tail++;

	for (i = 0; i < head; i++)
		diff_add_step(steps, ' ', i, i);

	mid_n = n - head - tail;
	mid_m = m - head - tail;

	if ((gsize)(mid_n + 1) * (gsize)(mid_m + 1) <= DIFF_MAX_CELLS) {
		g_autofree guint32 *table = NULL;
		gsize width = (gsize)mid_m + 1;

		table = g_new0(guint32, ((gsize)mid_n + 1) * width);

		/* table[i][j]: the longest match between a[head+i..] and b[head+j..] */
		for (i = mid_n; i > 0; i--) {
			for (j = mid_m; j > 0; j--) {
				gsize cell = (gsize)(i - 1) * width + (j - 1);

				if (g_strcmp0(g_ptr_array_index(a, head + i - 1),
				              g_ptr_array_index(b, head + j - 1)) == 0)
					table[cell] = table[(gsize)i * width + j] + 1;
				else
					table[cell] = MAX(table[(gsize)i * width + (j - 1)],
					                  table[(gsize)(i - 1) * width + j]);
			}
		}

		i = 0;
		j = 0;
		while (i < mid_n && j < mid_m) {
			if (g_strcmp0(g_ptr_array_index(a, head + i),
			              g_ptr_array_index(b, head + j)) == 0) {
				diff_add_step(steps, ' ', head + i, head + j);
				i++;
				j++;
			} else if (table[(gsize)(i + 1) * width + j] >=
			           table[(gsize)i * width + (j + 1)]) {
				diff_add_step(steps, '-', head + i, head + j);
				i++;
			} else {
				diff_add_step(steps, '+', head + i, head + j);
				j++;
			}
		}

		for (; i < mid_n; i++)
			diff_add_step(steps, '-', head + i, head + j);
		for (; j < mid_m; j++)
			diff_add_step(steps, '+', head + i, head + j);
	} else {
		for (i = 0; i < mid_n; i++)
			diff_add_step(steps, '-', head + i, head);
		for (j = 0; j < mid_m; j++)
			diff_add_step(steps, '+', head + mid_n, head + j);
	}

	for (i = 0; i < tail; i++)
		diff_add_step(steps, ' ', head + mid_n + i, head + mid_m + i);

	return steps;
}

/* "start,count" for a hunk header, in unified diff's own convention. */
static void
append_range(
	GString *out,
	guint    first,
	guint    count
){
	/* An empty side names the line it would follow, which is `first`. */
	if (count == 0)
		g_string_append_printf(out, "%u,0", first);
	else if (count == 1)
		g_string_append_printf(out, "%u", first + 1);
	else
		g_string_append_printf(out, "%u,%u", first + 1, count);
}

gchar *
clawt_skill_proposal_diff_text(
	const gchar *before,
	const gchar *after,
	const gchar *before_label,
	const gchar *after_label
){
	g_autoptr(GPtrArray) a = split_lines(before);
	g_autoptr(GPtrArray) b = split_lines(after);
	g_autoptr(GArray) steps = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	guint k;
	gboolean any = FALSE;

	steps = diff_steps(a, b);

	for (k = 0; k < steps->len; k++) {
		if (g_array_index(steps, DiffStep, k).op != ' ') {
			any = TRUE;
			break;
		}
	}

	if (!any)
		return g_strdup("");

	g_string_append_printf(out, "--- %s\n+++ %s\n",
	                       before_label != NULL ? before_label : "before",
	                       after_label != NULL ? after_label : "after");

	/*
	 * Hunks: a run of changes with DIFF_CONTEXT unchanged lines either
	 * side, two runs merged when the gap between them is no more than
	 * twice that -- what `diff -u` does, so it reads the way one does.
	 */
	k = 0;
	while (k < steps->len) {
		guint start;
		guint end;
		guint last_change;
		guint old_first = 0;
		guint new_first = 0;
		guint old_count = 0;
		guint new_count = 0;
		guint s;
		gboolean have_first = FALSE;

		while (k < steps->len && g_array_index(steps, DiffStep, k).op == ' ')
			k++;
		if (k >= steps->len)
			break;

		start = (k > DIFF_CONTEXT) ? k - DIFF_CONTEXT : 0;
		last_change = k;
		end = k;

		while (end < steps->len) {
			if (g_array_index(steps, DiffStep, end).op != ' ') {
				last_change = end;
			} else if (end - last_change > 2 * DIFF_CONTEXT) {
				break;
			}
			end++;
		}

		end = MIN(last_change + DIFF_CONTEXT + 1, steps->len);

		for (s = start; s < end; s++) {
			DiffStep *step = &g_array_index(steps, DiffStep, s);

			if (!have_first) {
				old_first = step->old_index;
				new_first = step->new_index;
				have_first = TRUE;
			}
			if (step->op != '+')
				old_count++;
			if (step->op != '-')
				new_count++;
		}

		g_string_append(out, "@@ -");
		append_range(out, old_first, old_count);
		g_string_append(out, " +");
		append_range(out, new_first, new_count);
		g_string_append(out, " @@\n");

		for (s = start; s < end; s++) {
			DiffStep *step = &g_array_index(steps, DiffStep, s);
			const gchar *line = (step->op == '+')
				? g_ptr_array_index(b, step->new_index)
				: g_ptr_array_index(a, step->old_index);

			g_string_append_c(out, step->op);
			g_string_append(out, line);
			g_string_append_c(out, '\n');
		}

		k = end;
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── The store ───────────────────────────────────────────────────── */

struct _ClawtSkillProposals {
	GObject     parent_instance;

	gchar      *directory;
	GHashTable *proposals;   /* id -> ClawtSkillProposal* */
};

G_DEFINE_FINAL_TYPE(ClawtSkillProposals, clawt_skill_proposals, G_TYPE_OBJECT)

/* Where one proposal lives, or NULL for an id that is not one. */
static gchar *
proposal_directory(
	ClawtSkillProposals *self,
	const gchar         *id
){
	if (!clawt_skill_proposal_id_is_valid(id))
		return NULL;

	return g_build_filename(self->directory, id, NULL);
}

static gchar *
draft_path(
	ClawtSkillProposals *self,
	const gchar         *id
){
	g_autofree gchar *directory = proposal_directory(self, id);

	if (directory == NULL)
		return NULL;

	return g_build_filename(directory, DRAFT_NAME, NULL);
}

/* The SKILL.md a skill of @name has in the library's own directory. */
static gchar *
library_file(
	ClawtSkillLibrary *library,
	const gchar       *name
){
	g_autofree gchar *directory = NULL;

	if (library == NULL || clawt_skill_library_get_directory(library) == NULL)
		return NULL;

	directory = clawt_skill_directory_for(clawt_skill_library_get_directory(library), name);

	if (directory == NULL)
		return NULL;

	return g_build_filename(directory, "SKILL.md", NULL);
}

/* The digest of a file's contents, or NULL when it cannot be read. */
static gchar *
file_digest(
	const gchar *path
){
	g_autofree gchar *text = NULL;

	if (path == NULL || !g_file_get_contents(path, &text, NULL, NULL))
		return NULL;

	return clawt_skill_digest(text);
}

static void
set_string_member(
	YamlMapping *fields,
	const gchar *key,
	const gchar *value
){
	g_autoptr(YamlNode) node = NULL;

	if (value == NULL)
		return;

	node = yaml_node_new_string(value);
	yaml_mapping_set_member(fields, key, node);
}

static void
set_int_member(
	YamlMapping *fields,
	const gchar *key,
	gint64       value
){
	g_autoptr(YamlNode) node = yaml_node_new_int(value);

	yaml_mapping_set_member(fields, key, node);
}

/* Writes a proposal's record, atomically, beside its draft. */
static gboolean
save_record(
	ClawtSkillProposals  *self,
	ClawtSkillProposal   *proposal,
	GError              **error
){
	g_autofree gchar *directory = proposal_directory(self, proposal->id);
	g_autofree gchar *path = NULL;
	g_autoptr(YamlNode) root = yaml_node_new_mapping(NULL);
	g_autoptr(YamlGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	YamlMapping *fields = yaml_node_get_mapping(root);

	if (directory == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a proposal id", proposal->id);
		return FALSE;
	}

	if (!clawt_ensure_dir(directory, 0700, error))
		return FALSE;

	set_string_member(fields, "agent", proposal->agent);
	set_string_member(fields, "name", proposal->name);
	set_string_member(fields, "kind", proposal->is_update ? "update" : "create");
	set_string_member(fields, "state",
	                  clawt_skill_proposal_state_to_string(proposal->state));
	set_string_member(fields, "reason", proposal->reason);
	set_string_member(fields, "base_sha256", proposal->base_digest);
	set_string_member(fields, "draft_sha256", proposal->draft_digest);
	set_int_member(fields, "created_at", proposal->created_at);
	set_int_member(fields, "settled_at", proposal->settled_at);

	generator = yaml_generator_new();
	yaml_generator_set_root(generator, root);
	text = yaml_generator_to_data(generator, NULL, error);

	if (text == NULL)
		return FALSE;

	path = g_build_filename(directory, RECORD_NAME, NULL);

	return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

static const gchar *
record_string(
	YamlMapping *fields,
	const gchar *key
){
	YamlNode *node = yaml_mapping_get_member(fields, key);

	return node != NULL ? yaml_node_get_string(node) : NULL;
}

static gint64
record_int(
	YamlMapping *fields,
	const gchar *key
){
	YamlNode *node = yaml_mapping_get_member(fields, key);

	return node != NULL ? yaml_node_get_int(node) : 0;
}

/* One record from disk, or NULL with the reason logged. */
static ClawtSkillProposal *
load_record(
	ClawtSkillProposals *self,
	const gchar         *id
){
	g_autofree gchar *directory = proposal_directory(self, id);
	g_autofree gchar *path = NULL;
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtSkillProposal) proposal = NULL;
	YamlNode *root;
	YamlMapping *fields;
	const gchar *name;

	if (directory == NULL)
		return NULL;

	path = g_build_filename(directory, RECORD_NAME, NULL);
	parser = yaml_parser_new();

	if (!yaml_parser_load_from_file(parser, path, &error)) {
		g_warning("skill proposals: %s could not be read: %s", path, error->message);
		return NULL;
	}

	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
		return NULL;

	fields = yaml_node_get_mapping(root);
	name = record_string(fields, "name");

	/* The name goes back through the gate: the file is on disk, not trusted. */
	if (!clawt_skill_name_is_valid(name)) {
		g_warning("skill proposals: %s names no valid skill; ignored", path);
		return NULL;
	}

	proposal = proposal_new();
	proposal->id = g_strdup(id);
	proposal->agent = g_strdup(record_string(fields, "agent"));
	proposal->name = g_strdup(name);
	proposal->reason = g_strdup(record_string(fields, "reason"));
	proposal->base_digest = g_strdup(record_string(fields, "base_sha256"));
	proposal->draft_digest = g_strdup(record_string(fields, "draft_sha256"));
	proposal->is_update = g_strcmp0(record_string(fields, "kind"), "update") == 0;
	proposal->created_at = record_int(fields, "created_at");
	proposal->settled_at = record_int(fields, "settled_at");

	/*
	 * A state this build does not know is read as rejected, never as
	 * pending or approved: either of those would put a draft back in
	 * front of somebody, or treat it as reviewed, on the strength of a
	 * word nothing here understands.
	 */
	if (!state_from_string(record_string(fields, "state"), &proposal->state)) {
		g_warning("skill proposals: %s has an unknown state; treated as rejected", path);
		proposal->state = CLAWT_SKILL_PROPOSAL_REJECTED;
	}

	return g_steal_pointer(&proposal);
}

/*
 * Every record on disk, forgetting settled ones older than a week.
 *
 * Kept for a week rather than dropped at once because the rate limit
 * counts rejected proposals too, and a person may want to see what was
 * decided yesterday.
 */
static void
load_all(
	ClawtSkillProposals *self
){
	g_autoptr(GDir) dir = NULL;
	const gchar *entry;
	gint64 now = g_get_real_time();

	dir = g_dir_open(self->directory, 0, NULL);
	if (dir == NULL)
		return;

	while ((entry = g_dir_read_name(dir)) != NULL) {
		ClawtSkillProposal *proposal;

		if (!clawt_skill_proposal_id_is_valid(entry))
			continue;

		proposal = load_record(self, entry);
		if (proposal == NULL)
			continue;

		if (proposal->state != CLAWT_SKILL_PROPOSAL_PENDING &&
		    proposal->settled_at > 0 &&
		    now - proposal->settled_at > SETTLED_KEEP_USEC) {
			g_autofree gchar *directory = proposal_directory(self, entry);
			g_autoptr(GError) error = NULL;

			if (!clawt_remove_tree(directory, self->directory, &error))
				g_warning("skill proposals: could not forget %s: %s",
				          entry, error->message);
			clawt_skill_proposal_free(proposal);
			continue;
		}

		g_hash_table_replace(self->proposals, g_strdup(entry), proposal);
	}
}

ClawtSkillProposals *
clawt_skill_proposals_new(
	const gchar *directory
){
	ClawtSkillProposals *self;

	g_return_val_if_fail(directory != NULL, NULL);

	self = g_object_new(CLAWT_TYPE_SKILL_PROPOSALS, NULL);
	self->directory = g_strdup(directory);
	load_all(self);

	return self;
}

const gchar *
clawt_skill_proposals_get_directory(
	ClawtSkillProposals *self
){
	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	return self->directory;
}

ClawtSkillProposal *
clawt_skill_proposals_get(
	ClawtSkillProposals *self,
	const gchar         *id
){
	ClawtSkillProposal *proposal;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	if (id == NULL)
		return NULL;

	proposal = g_hash_table_lookup(self->proposals, id);

	return proposal != NULL ? clawt_skill_proposal_copy(proposal) : NULL;
}

static gint
by_created_at(
	gconstpointer a,
	gconstpointer b
){
	const ClawtSkillProposal *x = *(ClawtSkillProposal * const *)a;
	const ClawtSkillProposal *y = *(ClawtSkillProposal * const *)b;

	if (x->created_at != y->created_at)
		return x->created_at < y->created_at ? -1 : 1;

	return g_strcmp0(x->id, y->id);
}

GPtrArray *
clawt_skill_proposals_list(
	ClawtSkillProposals *self,
	gboolean             pending_only
){
	GPtrArray *out;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_skill_proposal_free);

	g_hash_table_iter_init(&iter, self->proposals);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtSkillProposal *proposal = value;

		if (pending_only && proposal->state != CLAWT_SKILL_PROPOSAL_PENDING)
			continue;

		g_ptr_array_add(out, clawt_skill_proposal_copy(proposal));
	}

	g_ptr_array_sort(out, by_created_at);

	return out;
}

guint
clawt_skill_proposals_count_recent(
	ClawtSkillProposals *self,
	const gchar         *agent,
	gint64               now
){
	GHashTableIter iter;
	gpointer value;
	guint count = 0;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), 0);

	g_hash_table_iter_init(&iter, self->proposals);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtSkillProposal *proposal = value;

		if (g_strcmp0(proposal->agent, agent) == 0 &&
		    now - proposal->created_at < RATE_WINDOW_USEC)
			count++;
	}

	return count;
}

/* The oldest proposal @agent made inside the window, for "try again in". */
static gint64
oldest_recent(
	ClawtSkillProposals *self,
	const gchar         *agent,
	gint64               now
){
	GHashTableIter iter;
	gpointer value;
	gint64 oldest = 0;

	g_hash_table_iter_init(&iter, self->proposals);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtSkillProposal *proposal = value;

		if (g_strcmp0(proposal->agent, agent) != 0 ||
		    now - proposal->created_at >= RATE_WINDOW_USEC)
			continue;

		if (oldest == 0 || proposal->created_at < oldest)
			oldest = proposal->created_at;
	}

	return oldest;
}

/* The pending proposal for @name, if there is one. */
static ClawtSkillProposal *
pending_for_name(
	ClawtSkillProposals *self,
	const gchar         *name
){
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->proposals);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtSkillProposal *proposal = value;

		if (proposal->state == CLAWT_SKILL_PROPOSAL_PENDING &&
		    g_strcmp0(proposal->name, name) == 0)
			return proposal;
	}

	return NULL;
}

/*
 * Text an agent sent, checked before anything is built from it: present,
 * UTF-8, and under its bound.  Bounded in bytes, which is what is stored
 * and what a reviewer's screen has to hold.
 */
static gboolean
check_text(
	const gchar  *what,
	const gchar  *text,
	gsize         max,
	gboolean      required,
	GError      **error
){
	if (text == NULL || *text == '\0') {
		if (!required)
			return TRUE;

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "a proposal needs %s", what);
		return FALSE;
	}

	if (!g_utf8_validate(text, -1, NULL)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the %s is not UTF-8 text", what);
		return FALSE;
	}

	if (max > 0 && strlen(text) > max) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the %s is %" G_GSIZE_FORMAT " bytes; the most a proposal may "
		            "carry is %" G_GSIZE_FORMAT,
		            what, strlen(text), max);
		return FALSE;
	}

	return TRUE;
}

ClawtSkillProposal *
clawt_skill_proposals_propose(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *agent,
	const gchar          *name,
	const gchar          *description,
	const gchar          *body,
	gboolean              is_update,
	const gchar          *reason,
	guint                 max_per_day,
	GError              **error
){
	g_autofree gchar *decoded = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *rendered = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(ClawtSkill) draft = NULL;
	g_autoptr(ClawtSkill) parsed = NULL;
	g_autoptr(ClawtSkillProposal) proposal = NULL;
	g_autoptr(GError) cleanup_error = NULL;
	ClawtSkillProposal *pending;
	ClawtSkill *current;
	gint64 now = g_get_real_time();
	guint recent;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	/* ── Whether proposing is possible at all ── */

	if (library == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "skills are turned off for this fleet, so there is "
		                    "no library to propose into");
		return NULL;
	}

	if (max_per_day == 0) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "proposing skills is turned off for this fleet "
		                    "(skills.max_proposals_per_day is 0)");
		return NULL;
	}

	if (agent == NULL || *agent == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a proposal needs the agent that made it");
		return NULL;
	}

	/* ── The text, before anything is built from it ── */

	decoded = clawt_skill_name_from_wire(name != NULL ? name : "", error);
	if (decoded == NULL)
		return NULL;

	if (!check_text("description", description, CLAWT_SKILL_MAX_DESCRIPTION, TRUE, error) ||
	    !check_text("body", body, CLAWT_SKILL_PROPOSAL_MAX_BODY, TRUE, error) ||
	    !check_text("reason", reason, CLAWT_SKILL_PROPOSAL_MAX_REASON, is_update, error))
		return NULL;

	/* ── The rate limit, which counts rejected proposals too ── */

	recent = clawt_skill_proposals_count_recent(self, agent, now);
	if (recent >= max_per_day) {
		gint64 free_at = oldest_recent(self, agent, now) + RATE_WINDOW_USEC;
		gint64 minutes = MAX((gint64)1, (free_at - now) / G_TIME_SPAN_MINUTE);

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "%s has made %u skill proposals in the last 24 hours, which is "
		            "this fleet's limit (skills.max_proposals_per_day); the next "
		            "one can be made in %" G_GINT64_FORMAT " minutes. Fold this "
		            "into a proposal that is still pending only if it is the same "
		            "skill; otherwise keep it for later.",
		            agent, recent, minutes);
		return NULL;
	}

	pending = pending_for_name(self, decoded);
	if (pending != NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
		            "'%s' already has a proposal waiting for review (%s, from %s); "
		            "one proposal per skill at a time, so the reviewer is never "
		            "choosing between two drafts",
		            decoded, pending->id, pending->agent);
		return NULL;
	}

	/* ── The draft, validated by the same parse a read gets ── */

	current = clawt_skill_library_lookup(library, decoded);

	proposal = proposal_new();
	uuid = g_uuid_string_random();
	proposal->id = g_strconcat(CLAWT_SKILL_PROPOSAL_ID_PREFIX, uuid, NULL);
	proposal->agent = g_strdup(agent);
	proposal->name = g_strdup(decoded);
	proposal->reason = (reason != NULL && *reason != '\0') ? g_strdup(reason) : NULL;
	proposal->is_update = is_update;
	proposal->created_at = now;
	proposal->state = CLAWT_SKILL_PROPOSAL_PENDING;

	if (is_update) {
		g_autofree gchar *live_path = NULL;
		g_autofree gchar *live = NULL;
		g_autofree gchar *own = NULL;

		if (current == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "there is no skill called '%s' to update; leave out "
			            "`skill` to propose a new one", decoded);
			return NULL;
		}

		own = clawt_skill_directory_for(clawt_skill_library_get_directory(library), decoded);
		if (g_strcmp0(clawt_skill_get_directory(current), own) != 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
			            "'%s' is not in the fleet's own skills directory (it is "
			            "read from somewhere clawtilla does not write), so an "
			            "update to it could never be applied", decoded);
			return NULL;
		}

		/* The live skill's own front matter is kept: only the two texts change. */
		draft = clawt_skill_copy(current);
		clawt_skill_set_description(draft, description);
		clawt_skill_set_body(draft, body);

		live_path = library_file(library, decoded);
		if (live_path == NULL || !g_file_get_contents(live_path, &live, NULL, NULL)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "the skill '%s' could not be read", decoded);
			return NULL;
		}

		proposal->base_digest = clawt_skill_digest(live);
		rendered = clawt_skill_render(draft);

		if (g_strcmp0(live, rendered) == 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "the skill '%s' already says exactly this", decoded);
			return NULL;
		}
	} else {
		if (current != NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
			            "a skill called '%s' already exists; to change it, "
			            "propose an update by naming it in `skill`", decoded);
			return NULL;
		}

		draft = clawt_skill_new(decoded);
		clawt_skill_set_description(draft, description);
		clawt_skill_set_body(draft, body);
		rendered = clawt_skill_render(draft);
	}

	parsed = clawt_skill_parse(rendered, -1, decoded, error);
	if (parsed == NULL)
		return NULL;

	/* ── Written: the snapshot first, then the draft, then the record ── */

	directory = proposal_directory(self, proposal->id);
	if (!clawt_ensure_dir(self->directory, 0700, error) ||
	    !clawt_ensure_dir(directory, 0700, error))
		return NULL;

	path = g_build_filename(directory, DRAFT_NAME, NULL);
	if (!clawt_write_file_atomic(path, rendered, -1, 0600, FALSE, error)) {
		clawt_remove_tree(directory, self->directory, NULL);
		return NULL;
	}

	proposal->draft_digest = clawt_skill_digest(rendered);

	if (!is_update) {
		g_autofree gchar *origin = g_strconcat("propose:", proposal->id, NULL);
		g_autofree gchar *written = NULL;
		g_autofree gchar *written_path = NULL;

		/*
		 * teach's own commit, so a proposed skill gets exactly what a
		 * taught one does: the name rules, the description bound, the
		 * traversal gate, the scan, and `enabled: false`.
		 */
		if (clawt_skill_library_create_taught(library, decoded, description, body,
		                                      origin, error) == NULL) {
			clawt_remove_tree(directory, self->directory, NULL);
			return NULL;
		}

		/*
		 * The digest approval compares against is the library's file,
		 * read back, rather than the render above: should the two ever
		 * differ by a byte, every approval would be refused as stale.
		 */
		written_path = library_file(library, decoded);
		written = file_digest(written_path);
		if (written != NULL) {
			g_free(proposal->draft_digest);
			proposal->draft_digest = g_steal_pointer(&written);
		}
	}

	if (!save_record(self, proposal, error)) {
		if (!is_update &&
		    !clawt_skill_library_remove(library, decoded, &cleanup_error))
			g_warning("skill proposals: could not remove the draft of '%s': %s",
			          decoded, cleanup_error->message);
		clawt_remove_tree(directory, self->directory, NULL);
		return NULL;
	}

	g_hash_table_replace(self->proposals, g_strdup(proposal->id),
	                     clawt_skill_proposal_copy(proposal));

	return g_steal_pointer(&proposal);
}

gchar *
clawt_skill_proposals_get_draft(
	ClawtSkillProposals *self,
	const gchar         *id
){
	g_autofree gchar *path = NULL;
	gchar *text = NULL;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	path = draft_path(self, id);
	if (path == NULL || !g_file_get_contents(path, &text, NULL, NULL))
		return NULL;

	return text;
}

gchar *
clawt_skill_proposals_get_diff(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
){
	ClawtSkillProposal *proposal;
	g_autofree gchar *draft = NULL;
	g_autofree gchar *live_path = NULL;
	g_autofree gchar *live = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	proposal = (id != NULL) ? g_hash_table_lookup(self->proposals, id) : NULL;
	if (proposal == NULL || !proposal->is_update)
		return NULL;

	draft = clawt_skill_proposals_get_draft(self, id);
	live_path = library_file(library, proposal->name);

	if (draft == NULL || live_path == NULL ||
	    !g_file_get_contents(live_path, &live, NULL, NULL))
		return NULL;

	before = g_strdup_printf("%s/SKILL.md (now)", proposal->name);
	after = g_strdup_printf("%s/SKILL.md (proposed)", proposal->name);

	return clawt_skill_proposal_diff_text(live, draft, before, after);
}

gboolean
clawt_skill_proposals_is_stale(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
){
	ClawtSkillProposal *proposal;
	g_autofree gchar *digest = NULL;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), FALSE);

	proposal = (id != NULL) ? g_hash_table_lookup(self->proposals, id) : NULL;
	if (proposal == NULL || proposal->state != CLAWT_SKILL_PROPOSAL_PENDING)
		return FALSE;

	{
		g_autofree gchar *path = library_file(library, proposal->name);

		digest = file_digest(path);
	}

	if (proposal->is_update)
		return g_strcmp0(digest, proposal->base_digest) != 0;

	return g_strcmp0(digest, proposal->draft_digest) != 0;
}

gchar *
clawt_skill_proposals_describe(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
){
	ClawtSkillProposal *proposal;
	g_autoptr(GString) out = NULL;
	g_autofree gchar *draft = NULL;
	g_autofree gchar *shown = NULL;
	g_autoptr(ClawtSkill) parsed = NULL;
	GPtrArray *warnings;
	guint i;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	proposal = (id != NULL) ? g_hash_table_lookup(self->proposals, id) : NULL;
	if (proposal == NULL)
		return NULL;

	draft = clawt_skill_proposals_get_draft(self, id);
	out = g_string_new(NULL);

	/* The headline: an agent id and a validated name, nothing freer. */
	if (proposal->is_update)
		g_string_append_printf(out, "%s proposes a change to the skill '%s'.\n",
		                       proposal->agent, proposal->name);
	else
		g_string_append_printf(out, "%s proposes a new skill, '%s'.\n",
		                       proposal->agent, proposal->name);

	if (proposal->reason != NULL) {
		g_autofree gchar *reason = clawt_skill_proposal_fence(proposal->reason, "text");

		g_string_append_printf(out, "\nWhy, in %s's own words:\n%s\n",
		                       proposal->agent, reason);
	}

	if (proposal->is_update) {
		g_autofree gchar *diff = clawt_skill_proposals_get_diff(self, library, id);

		shown = clawt_skill_proposal_fence(diff != NULL ? diff : "", "diff");
		g_string_append_printf(out, "\nThe change, against the skill as it is now:\n%s\n",
		                       shown);
	} else {
		shown = clawt_skill_proposal_fence(draft != NULL ? draft : "", "markdown");
		g_string_append_printf(out, "\nThe proposed SKILL.md:\n%s\n", shown);
	}

	/*
	 * What the scan found, in the daemon's words.  The scan is the same
	 * one an imported skill gets; it warns, and the person decides.
	 */
	parsed = (draft != NULL) ? clawt_skill_parse(draft, -1, proposal->name, NULL) : NULL;
	warnings = (parsed != NULL) ? clawt_skill_get_warnings(parsed) : NULL;

	if (warnings != NULL && warnings->len > 0) {
		g_string_append(out, "\nThe scan noticed:\n");
		for (i = 0; i < warnings->len; i++)
			g_string_append_printf(out, "- %s\n",
			                       (const gchar *)g_ptr_array_index(warnings, i));
	}

	if (clawt_skill_proposals_is_stale(self, library, id))
		g_string_append(out,
			"\nThe file on disk has changed since this was proposed, so "
			"approving it is refused; edit shows what is there now, or "
			"reject it.\n");

	if (proposal->is_update)
		g_string_append(out,
			"\nNothing changes until you answer. approve writes this over the "
			"live skill; edit lets you change the text first; reject discards "
			"it and leaves the skill as it is.");
	else
		g_string_append(out,
			"\nIt is in the library now, disabled: it reaches no prompt until "
			"you answer. approve enables it; edit lets you change the text "
			"first; reject removes the draft.");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Settling ────────────────────────────────────────────────────── */

/* The pending proposal @id, or NULL with an error saying why not. */
static ClawtSkillProposal *
pending_or_error(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	gboolean              need_library,
	GError              **error
){
	ClawtSkillProposal *proposal;

	proposal = (id != NULL) ? g_hash_table_lookup(self->proposals, id) : NULL;

	if (proposal == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "no skill proposal '%s'", id != NULL ? id : "");
		return NULL;
	}

	if (proposal->state != CLAWT_SKILL_PROPOSAL_PENDING) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "the proposal for '%s' was already %s",
		            proposal->name, clawt_skill_proposal_state_to_string(proposal->state));
		return NULL;
	}

	if (need_library && library == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "skills are turned off for this fleet; there is no "
		                    "library to apply the proposal to");
		return NULL;
	}

	return proposal;
}

/* Records the outcome, and hands the caller a copy of the settled record. */
static ClawtSkillProposal *
settle(
	ClawtSkillProposals      *self,
	ClawtSkillProposal       *proposal,
	ClawtSkillProposalState   state
){
	g_autoptr(GError) error = NULL;

	proposal->state = state;
	proposal->settled_at = g_get_real_time();

	/*
	 * The library has already changed by now, so a record that will not
	 * save is a warning rather than a failure: refusing here would report
	 * the change as not made when it was.
	 */
	if (!save_record(self, proposal, &error))
		g_warning("skill proposals: %s settled as %s, and the record could not "
		          "be saved: %s", proposal->id,
		          clawt_skill_proposal_state_to_string(state), error->message);

	return clawt_skill_proposal_copy(proposal);
}

/* Refused when the reviewed bytes are no longer the bytes on disk. */
static gboolean
check_not_stale(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	ClawtSkillProposal   *proposal,
	gboolean              draft_matters,
	GError              **error
){
	g_autofree gchar *path = library_file(library, proposal->name);
	g_autofree gchar *digest = file_digest(path);

	(void)self;

	if (proposal->is_update) {
		if (digest == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "the skill '%s' is gone from the library; reject this "
			            "proposal", proposal->name);
			return FALSE;
		}

		if (g_strcmp0(digest, proposal->base_digest) != 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
			            "the skill '%s' has changed since this update was "
			            "proposed, so it would overwrite text nobody reviewed it "
			            "against; reject it and ask for a fresh proposal",
			            proposal->name);
			return FALSE;
		}

		return TRUE;
	}

	if (digest == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "the draft of '%s' is gone from the library; reject this "
		            "proposal", proposal->name);
		return FALSE;
	}

	if (draft_matters && g_strcmp0(digest, proposal->draft_digest) != 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "the draft of '%s' was rewritten after it was proposed, so "
		            "what would be enabled is not what was reviewed; edit it to "
		            "see what is there now, or reject it", proposal->name);
		return FALSE;
	}

	return TRUE;
}

/*
 * The library's view of a skill, rescanned when it does not have it: the
 * library learns about files through a debounced monitor, and a lookup
 * between a write and the rescan would read as the skill being gone.
 */
static ClawtSkill *
lookup_fresh(
	ClawtSkillLibrary *library,
	const gchar       *name
){
	ClawtSkill *skill = clawt_skill_library_lookup(library, name);

	if (skill == NULL) {
		clawt_skill_library_scan(library);
		skill = clawt_skill_library_lookup(library, name);
	}

	return skill;
}

ClawtSkillProposal *
clawt_skill_proposals_approve(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	GError              **error
){
	ClawtSkillProposal *proposal;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	proposal = pending_or_error(self, library, id, TRUE, error);
	if (proposal == NULL)
		return NULL;

	if (!check_not_stale(self, library, proposal, TRUE, error))
		return NULL;

	if (proposal->is_update) {
		g_autofree gchar *draft = clawt_skill_proposals_get_draft(self, id);
		g_autoptr(ClawtSkill) parsed = NULL;

		if (draft == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "the staged text of this proposal is missing; reject it");
			return NULL;
		}

		/* The snapshot is parsed again: it is a file on disk, not a promise. */
		parsed = clawt_skill_parse(draft, -1, proposal->name, error);
		if (parsed == NULL)
			return NULL;

		if (lookup_fresh(library, proposal->name) == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "no skill called '%s'", proposal->name);
			return NULL;
		}

		if (clawt_skill_library_replace(library, proposal->name,
		                                clawt_skill_get_description(parsed),
		                                clawt_skill_get_body(parsed), error) == NULL)
			return NULL;
	} else {
		if (lookup_fresh(library, proposal->name) == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
			            "the draft of '%s' is not in the library; reject this "
			            "proposal", proposal->name);
			return NULL;
		}

		if (!clawt_skill_library_set_enabled(library, proposal->name, TRUE, error))
			return NULL;
	}

	return settle(self, proposal, CLAWT_SKILL_PROPOSAL_APPROVED);
}

ClawtSkillProposal *
clawt_skill_proposals_edit(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	const gchar          *description,
	const gchar          *body,
	GError              **error
){
	ClawtSkillProposal *proposal;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	proposal = pending_or_error(self, library, id, TRUE, error);
	if (proposal == NULL)
		return NULL;

	if (!check_text("description", description, CLAWT_SKILL_MAX_DESCRIPTION, TRUE, error) ||
	    !check_text("body", body, CLAWT_SKILL_PROPOSAL_MAX_BODY, TRUE, error))
		return NULL;

	/*
	 * The person is writing the text, so a draft rewritten since it was
	 * proposed does not matter -- what they wrote replaces it.  The live
	 * skill under an update still does: they edited against it.
	 */
	if (!check_not_stale(self, library, proposal, FALSE, error))
		return NULL;

	if (lookup_fresh(library, proposal->name) == NULL) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "no skill called '%s'", proposal->name);
		return NULL;
	}

	if (clawt_skill_library_replace(library, proposal->name, description, body,
	                                error) == NULL)
		return NULL;

	if (!proposal->is_update &&
	    !clawt_skill_library_set_enabled(library, proposal->name, TRUE, error))
		return NULL;

	return settle(self, proposal, CLAWT_SKILL_PROPOSAL_EDITED);
}

ClawtSkillProposal *
clawt_skill_proposals_reject(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	GError              **error
){
	ClawtSkillProposal *proposal;

	g_return_val_if_fail(CLAWT_IS_SKILL_PROPOSALS(self), NULL);

	proposal = pending_or_error(self, library, id, FALSE, error);
	if (proposal == NULL)
		return NULL;

	/*
	 * A new skill's draft goes -- unless a person enabled it from the
	 * Skills page meanwhile, which is them keeping it, or it is no
	 * longer the library's own file.  An update was only ever staged
	 * here, so there is nothing in the library to undo.
	 */
	if (!proposal->is_update && library != NULL) {
		ClawtSkill *skill = lookup_fresh(library, proposal->name);
		g_autofree gchar *own = clawt_skill_directory_for(
			clawt_skill_library_get_directory(library), proposal->name);

		if (skill != NULL && !clawt_skill_get_enabled(skill) &&
		    g_strcmp0(clawt_skill_get_directory(skill), own) == 0 &&
		    !clawt_skill_library_remove(library, proposal->name, error))
			return NULL;
	}

	return settle(self, proposal, CLAWT_SKILL_PROPOSAL_REJECTED);
}

/* ── GObject ─────────────────────────────────────────────────────── */

static void
clawt_skill_proposals_finalize(
	GObject *object
){
	ClawtSkillProposals *self = CLAWT_SKILL_PROPOSALS(object);

	g_clear_pointer(&self->proposals, g_hash_table_unref);
	g_clear_pointer(&self->directory, g_free);

	G_OBJECT_CLASS(clawt_skill_proposals_parent_class)->finalize(object);
}

static void
clawt_skill_proposals_class_init(
	ClawtSkillProposalsClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_skill_proposals_finalize;
}

static void
clawt_skill_proposals_init(
	ClawtSkillProposals *self
){
	self->proposals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        (GDestroyNotify)clawt_skill_proposal_free);
}
