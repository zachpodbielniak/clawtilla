/*
 * clawt-device-store.c - Paired client devices and their tokens
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-device-store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <yaml-glib.h>

/*
 * How far last-seen may move before it is written down again.
 *
 * The sweep touches every connected device once a minute, and writing
 * the file each time would be a disk write a minute per connected laptop
 * for a value whose only reader counts in days.
 */
#define LAST_SEEN_WRITE_GRANULARITY_SECONDS (300)

/* How long the rate limit's window is. */
#define REDEEM_WINDOW_SECONDS (60)

struct _ClawtDevice {
    gchar  *id;
    gchar  *name;
    gint64  created;
    gint64  last_seen;
};

/*
 * What the store keeps per device: the public part, the hash, and the
 * last-seen value that is on disk -- the comparison that decides whether
 * a touch is worth a write.
 */
typedef struct {
    ClawtDevice device;
    gchar      *hash;
    gint64      last_seen_saved;
} Entry;

struct _ClawtDeviceStore {
    GObject parent_instance;

    gchar     *path;
    GPtrArray *entries;      /* Entry*, owned, oldest first */
    gint64     idle_seconds;

    /* The one outstanding pairing code, if any. */
    gchar  *code;
    gchar  *code_name;
    gint64  code_expires_at;
    guint   code_wrong;

    /* Failed redemptions in the current window. */
    gint64  window_start;
    guint   window_failures;
};

G_DEFINE_BOXED_TYPE(ClawtDevice, clawt_device, clawt_device_copy,
                    clawt_device_free)

G_DEFINE_FINAL_TYPE(ClawtDeviceStore, clawt_device_store, G_TYPE_OBJECT)

/* ── ClawtDevice ─────────────────────────────────────────────────── */

ClawtDevice *
clawt_device_copy(ClawtDevice *self)
{
    ClawtDevice *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtDevice, 1);
    copy->id = g_strdup(self->id);
    copy->name = g_strdup(self->name);
    copy->created = self->created;
    copy->last_seen = self->last_seen;

    return copy;
}

void
clawt_device_free(ClawtDevice *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->name);
    g_free(self);
}

const gchar *
clawt_device_get_id(ClawtDevice *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

const gchar *
clawt_device_get_name(ClawtDevice *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

gint64
clawt_device_get_created(ClawtDevice *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->created;
}

gint64
clawt_device_get_last_seen(ClawtDevice *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->last_seen;
}

/* ── Entries ─────────────────────────────────────────────────────── */

static void
entry_free(gpointer data)
{
    Entry *entry = data;

    g_free(entry->device.id);
    g_free(entry->device.name);

    /*
     * The hash is not the token, but it is the thing an offline guesser
     * would want; there is no reason to leave it lying in freed memory.
     */
    if (entry->hash != NULL)
        memset(entry->hash, 0, strlen(entry->hash));

    g_free(entry->hash);
    g_free(entry);
}

static Entry *
find_entry(ClawtDeviceStore *self, const gchar *id, guint *index)
{
    guint i;

    for (i = 0; id != NULL && i < self->entries->len; i++) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        if (g_strcmp0(entry->device.id, id) == 0) {
            if (index != NULL)
                *index = i;

            return entry;
        }
    }

    return NULL;
}

/* ── Randomness ──────────────────────────────────────────────────── */

/*
 * Straight from the kernel pool, as clawt_generate_token() does.
 *
 * GRand is a Mersenne Twister seeded from the clock and the pid: fine for
 * shuffling, and recoverable from a handful of outputs, which for a
 * pairing code would mean predicting the next one.
 */
static gboolean
read_urandom(guchar *buffer, gsize length, GError **error)
{
    FILE *pool;
    gsize got;

    pool = fopen("/dev/urandom", "rb");

    if (pool == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open /dev/urandom: %s", g_strerror(errno));
        return FALSE;
    }

    got = fread(buffer, 1, length, pool);
    fclose(pool);

    /*
     * Refused rather than padded: a code drawn from fewer random bytes
     * than intended looks exactly like one drawn from enough.
     */
    if (got != length) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "short read from /dev/urandom: wanted %u bytes, got %u",
                    (guint)length, (guint)got);
        return FALSE;
    }

    return TRUE;
}

/*
 * Six digits with no modulo bias.
 *
 * A 32-bit value modulo a million favours the low codes, slightly -- and
 * slightly is a bias a guesser can start from.  Values above the largest
 * whole multiple of a million are drawn again instead.
 */
static gchar *
draw_code(GError **error)
{
    const guint32 range = 1000000u;
    const guint32 limit = (G_MAXUINT32 / range) * range;
    guint attempts;

    for (attempts = 0; attempts < 64; attempts++) {
        guchar bytes[4];
        guint32 value;

        if (!read_urandom(bytes, sizeof bytes, error))
            return NULL;

        value = ((guint32)bytes[0] << 24) | ((guint32)bytes[1] << 16) |
                ((guint32)bytes[2] << 8) | (guint32)bytes[3];

        memset(bytes, 0, sizeof bytes);

        if (value >= limit)
            continue;

        return g_strdup_printf("%06u", (guint)(value % range));
    }

    /* 64 rejections in a row is a broken pool, not bad luck. */
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "/dev/urandom kept returning values outside the range");
    return NULL;
}

/* "d" and eight hex digits: short enough to type into `device revoke`. */
static gchar *
draw_id(ClawtDeviceStore *self, GError **error)
{
    guint attempts;

    for (attempts = 0; attempts < 16; attempts++) {
        guchar bytes[4];
        g_autofree gchar *id = NULL;

        if (!read_urandom(bytes, sizeof bytes, error))
            return NULL;

        id = g_strdup_printf("d%02x%02x%02x%02x", bytes[0], bytes[1],
                             bytes[2], bytes[3]);

        if (find_entry(self, id, NULL) == NULL)
            return g_steal_pointer(&id);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                "could not find an unused device id");
    return NULL;
}

/* ── Helpers every surface shares ────────────────────────────────── */

gchar *
clawt_device_token_hash(const gchar *token)
{
    g_return_val_if_fail(token != NULL, NULL);

    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
}

gchar *
clawt_device_code_normalize(const gchar *text)
{
    GString *digits;
    const gchar *p;

    if (text == NULL)
        return NULL;

    digits = g_string_new(NULL);

    for (p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == '-' || *p == '\t')
            continue;

        if (!g_ascii_isdigit(*p)) {
            g_string_free(digits, TRUE);
            return NULL;
        }

        g_string_append_c(digits, *p);
    }

    if (digits->len != CLAWT_DEVICE_CODE_DIGITS) {
        g_string_free(digits, TRUE);
        return NULL;
    }

    return g_string_free(digits, FALSE);
}

gchar *
clawt_device_code_display(const gchar *code)
{
    g_return_val_if_fail(code != NULL, NULL);

    if (strlen(code) != CLAWT_DEVICE_CODE_DIGITS)
        return g_strdup(code);

    return g_strdup_printf("%.3s %.3s", code, code + 3);
}

gchar *
clawt_device_countdown_text(gint64 remaining)
{
    if (remaining <= 0)
        return g_strdup("expired");

    return g_strdup_printf("%" G_GINT64_FORMAT ":%02d", remaining / 60,
                           (gint)(remaining % 60));
}

gchar *
clawt_device_seen_text(gint64 last_seen, gboolean connected, gint64 now)
{
    gint64 age;

    if (connected)
        return g_strdup("connected now");

    if (last_seen <= 0)
        return g_strdup("never seen");

    age = now - last_seen;

    /* A clock that moved backwards is "just now", not a negative age. */
    if (age < 60)
        return g_strdup("seen just now");

    if (age < 3600)
        return g_strdup_printf("seen %" G_GINT64_FORMAT " minute%s ago",
                               age / 60, (age / 60 == 1) ? "" : "s");

    if (age < 86400)
        return g_strdup_printf("seen %" G_GINT64_FORMAT " hour%s ago",
                               age / 3600, (age / 3600 == 1) ? "" : "s");

    return g_strdup_printf("seen %" G_GINT64_FORMAT " day%s ago",
                           age / 86400, (age / 86400 == 1) ? "" : "s");
}

/* ── Reading and writing the file ────────────────────────────────── */

static const gchar *
member_string(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node = yaml_mapping_get_member(mapping, key);

    if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
        return NULL;

    return yaml_node_get_string(node);
}

static gint64
member_int(YamlMapping *mapping, const gchar *key)
{
    const gchar *text = member_string(mapping, key);

    return (text != NULL) ? g_ascii_strtoll(text, NULL, 10) : 0;
}

/* Single-quoted, so nothing in a device name needs escaping but a quote. */
static void
append_quoted(GString *out, const gchar *key, const gchar *value)
{
    const gchar *p;

    g_string_append_printf(out, "    %s: '", key);

    for (p = (value != NULL) ? value : ""; *p != '\0'; p++) {
        /*
         * A newline in a name would end the scalar and start a key; a
         * name is one line, so it becomes a space.
         */
        if (*p == '\'')
            g_string_append(out, "''");
        else if (*p == '\n' || *p == '\r')
            g_string_append_c(out, ' ');
        else
            g_string_append_c(out, *p);
    }

    g_string_append(out, "'\n");
}

static gboolean
save(ClawtDeviceStore *self, GError **error)
{
    g_autoptr(GString) out = g_string_new(NULL);
    g_autofree gchar *dir = g_path_get_dirname(self->path);
    guint i;

    g_string_append(
        out,
        "# clawtilla paired devices -- written by the daemon.\n"
        "#\n"
        "# Each token is stored as its SHA-256, never as itself, so this\n"
        "# file does not let anybody connect. `clawtilla device list` and\n"
        "# `clawtilla device revoke ID` are the way to manage it.\n"
        "\n"
        "devices:\n");

    if (self->entries->len == 0)
        g_string_append(out, "  []\n");

    for (i = 0; i < self->entries->len; i++) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        g_string_append(out, "  -\n");
        append_quoted(out, "id", entry->device.id);
        append_quoted(out, "name", entry->device.name);
        append_quoted(out, "hash", entry->hash);
        g_string_append_printf(out, "    created: %" G_GINT64_FORMAT "\n",
                               entry->device.created);
        g_string_append_printf(out, "    last_seen: %" G_GINT64_FORMAT "\n",
                               entry->device.last_seen);
    }

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    if (!clawt_write_file_atomic(self->path, out->str, (gssize)out->len,
                                 0600, FALSE, error))
        return FALSE;

    for (i = 0; i < self->entries->len; i++) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        entry->last_seen_saved = entry->device.last_seen;
    }

    return TRUE;
}

/*
 * A write that fails after the in-memory change has been made.
 *
 * Revoking or pairing has already taken effect for this daemon's life;
 * the warning is what tells somebody it will not survive a restart.
 */
static void
save_or_warn(ClawtDeviceStore *self, const gchar *what)
{
    g_autoptr(GError) error = NULL;

    if (!save(self, &error))
        g_warning("devices: %s, but %s could not be written (%s); the "
                  "change will not survive a restart", what, self->path,
                  error->message);
}

gboolean
clawt_device_store_load(ClawtDeviceStore *self, GError **error)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autofree gchar *text = NULL;
    YamlNode *root;
    YamlNode *list;
    YamlSequence *sequence;
    gsize length;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), FALSE);

    g_ptr_array_set_size(self->entries, 0);

    if (!g_file_test(self->path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (!g_file_get_contents(self->path, &text, NULL, error)) {
        g_prefix_error(error, "%s: ", self->path);
        return FALSE;
    }

    if (*text == '\0')
        return TRUE;

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, text, -1, error)) {
        g_prefix_error(error, "%s: ", self->path);
        return FALSE;
    }

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return TRUE;

    list = yaml_mapping_get_member(yaml_node_get_mapping(root), "devices");

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return TRUE;

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        YamlMapping *mapping;
        const gchar *id;
        const gchar *hash;
        Entry *entry;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        mapping = yaml_node_get_mapping(element);
        id = member_string(mapping, "id");
        hash = member_string(mapping, "hash");

        /*
         * An entry with no hash could never authenticate, and one with no
         * id could never be revoked; skipping both is the honest reading.
         */
        if (id == NULL || *id == '\0' || hash == NULL || *hash == '\0')
            continue;

        entry = g_new0(Entry, 1);
        entry->device.id = g_strdup(id);
        entry->device.name = g_strdup(member_string(mapping, "name"));
        entry->device.created = member_int(mapping, "created");
        entry->device.last_seen = member_int(mapping, "last_seen");
        entry->last_seen_saved = entry->device.last_seen;
        entry->hash = g_ascii_strdown(hash, -1);

        g_ptr_array_add(self->entries, entry);
    }

    return TRUE;
}

/* ── The store ───────────────────────────────────────────────────── */

ClawtDeviceStore *
clawt_device_store_new(const gchar *path)
{
    ClawtDeviceStore *self;

    g_return_val_if_fail(path != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_DEVICE_STORE, NULL);
    self->path = clawt_expand_path(path);

    return self;
}

const gchar *
clawt_device_store_get_path(ClawtDeviceStore *self)
{
    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    return self->path;
}

void
clawt_device_store_set_idle_seconds(ClawtDeviceStore *self, gint64 seconds)
{
    g_return_if_fail(CLAWT_IS_DEVICE_STORE(self));

    self->idle_seconds = MAX(seconds, 0);
}

static gboolean
entry_is_idle(ClawtDeviceStore *self, Entry *entry, gint64 now)
{
    gint64 seen;

    if (self->idle_seconds <= 0)
        return FALSE;

    /* Never seen since pairing counts from the pairing. */
    seen = MAX(entry->device.last_seen, entry->device.created);

    return now - seen > self->idle_seconds;
}

static void
clear_code(ClawtDeviceStore *self)
{
    if (self->code != NULL)
        memset(self->code, 0, strlen(self->code));

    g_clear_pointer(&self->code, g_free);
    g_clear_pointer(&self->code_name, g_free);
    self->code_expires_at = 0;
    self->code_wrong = 0;
}

gchar *
clawt_device_store_begin_pairing(ClawtDeviceStore  *self,
                                 const gchar       *name,
                                 gint64             now,
                                 gint64            *expires_at,
                                 GError           **error)
{
    gchar *code;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    code = draw_code(error);

    if (code == NULL)
        return NULL;

    clear_code(self);

    self->code = g_strdup(code);
    self->code_name = (name != NULL && *name != '\0') ? g_strdup(name)
                                                      : NULL;
    self->code_expires_at = now + CLAWT_DEVICE_CODE_LIFETIME_SECONDS;

    if (expires_at != NULL)
        *expires_at = self->code_expires_at;

    return code;
}

gint64
clawt_device_store_pairing_expires_at(ClawtDeviceStore *self, gint64 now)
{
    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), 0);

    if (self->code == NULL)
        return 0;

    if (now >= self->code_expires_at) {
        clear_code(self);
        return 0;
    }

    return self->code_expires_at;
}

void
clawt_device_store_cancel_pairing(ClawtDeviceStore *self)
{
    g_return_if_fail(CLAWT_IS_DEVICE_STORE(self));

    clear_code(self);
}

/*
 * One failed redemption, counted against the window.
 *
 * Counted whatever the reason -- no code outstanding, an expired one, a
 * wrong one -- because to somebody guessing they are all the same thing:
 * a guess that told them nothing.
 */
static void
note_failure(ClawtDeviceStore *self, gint64 now)
{
    if (now - self->window_start >= REDEEM_WINDOW_SECONDS ||
        now < self->window_start) {
        self->window_start = now;
        self->window_failures = 0;
    }

    self->window_failures++;
}

gchar *
clawt_device_store_redeem(ClawtDeviceStore  *self,
                          const gchar       *code,
                          const gchar       *name,
                          gint64             now,
                          gchar            **device_id,
                          GError           **error)
{
    g_autofree gchar *typed = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *id = NULL;
    const gchar *chosen_name;
    gboolean matches;
    Entry *entry;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    /*
     * The rate limit first, before the code is looked at: a refusal that
     * still compared the code would still be an oracle, only a slower
     * one.
     */
    if (now - self->window_start < REDEEM_WINDOW_SECONDS &&
        now >= self->window_start &&
        self->window_failures >= CLAWT_DEVICE_REDEEM_FAILURES_PER_MINUTE) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
                    "too many pairing attempts; wait a minute and try "
                    "again");
        return NULL;
    }

    typed = clawt_device_code_normalize(code);

    if (self->code != NULL && now >= self->code_expires_at)
        clear_code(self);

    if (self->code == NULL) {
        note_failure(self, now);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "there is no pairing code waiting -- it expired or was "
                    "already used; run `clawtilla device pair` on the "
                    "daemon's machine for a new one");
        return NULL;
    }

    /*
     * Constant time, like the owner token.  The code is short-lived and
     * guess-limited, but a comparison that stops at the first wrong digit
     * would hand back the digits one at a time.  Something that is not
     * six digits still goes through the comparison, as a string that
     * cannot match.
     */
    matches = clawt_secure_equals(typed != NULL ? typed : "", self->code);

    if (!matches) {
        guint left;

        note_failure(self, now);
        self->code_wrong++;

        if (self->code_wrong >= CLAWT_DEVICE_CODE_MAX_GUESSES) {
            clear_code(self);
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                        "that pairing code is wrong, and that was the last "
                        "try: the code no longer works; ask for a new one");
            return NULL;
        }

        left = CLAWT_DEVICE_CODE_MAX_GUESSES - self->code_wrong;
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH,
                    "that pairing code is wrong (%u %s left before it "
                    "stops working)", left, (left == 1) ? "try" : "tries");
        return NULL;
    }

    token = clawt_generate_token(error);

    if (token == NULL)
        return NULL;

    id = draw_id(self, error);

    if (id == NULL)
        return NULL;

    chosen_name = (self->code_name != NULL) ? self->code_name
                : (name != NULL && *name != '\0') ? name
                : "unnamed device";

    entry = g_new0(Entry, 1);
    entry->device.id = g_strdup(id);
    entry->device.name = g_strdup(chosen_name);
    entry->device.created = now;
    entry->device.last_seen = now;
    entry->hash = clawt_device_token_hash(token);

    g_ptr_array_add(self->entries, entry);

    /* Single use: the code is gone whether or not the write succeeds. */
    clear_code(self);

    if (!save(self, error)) {
        g_ptr_array_remove(self->entries, entry);
        return NULL;
    }

    if (device_id != NULL)
        *device_id = g_steal_pointer(&id);

    return g_steal_pointer(&token);
}

gchar *
clawt_device_store_authenticate(ClawtDeviceStore *self,
                                const gchar      *token,
                                gint64            now)
{
    g_autofree gchar *hash = NULL;
    Entry *found = NULL;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    if (token == NULL || *token == '\0')
        return NULL;

    hash = clawt_device_token_hash(token);

    /*
     * Every entry, with no break: when the loop ends tells nobody which
     * entry matched, or whether one did.
     */
    for (i = 0; i < self->entries->len; i++) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        if (clawt_secure_equals(hash, entry->hash) && found == NULL)
            found = entry;
    }

    if (found == NULL)
        return NULL;

    if (entry_is_idle(self, found, now)) {
        g_message("devices: %s (%s) was idle past daemon.device_idle_days "
                  "and has been removed", found->device.id,
                  found->device.name);
        g_ptr_array_remove(self->entries, found);
        save_or_warn(self, "an idle device was removed");
        return NULL;
    }

    found->device.last_seen = now;

    /*
     * Written on every authentication rather than at the granularity:
     * connecting is rare, and this is the moment that most needs to
     * survive a restart.
     */
    save_or_warn(self, "a device connected");

    return g_strdup(found->device.id);
}

void
clawt_device_store_touch(ClawtDeviceStore *self, const gchar *id,
                         gint64 now)
{
    Entry *entry;

    g_return_if_fail(CLAWT_IS_DEVICE_STORE(self));

    entry = find_entry(self, id, NULL);

    if (entry == NULL || now <= entry->device.last_seen)
        return;

    entry->device.last_seen = now;

    if (now - entry->last_seen_saved >= LAST_SEEN_WRITE_GRANULARITY_SECONDS)
        save_or_warn(self, "a connected device was seen");
}

GPtrArray *
clawt_device_store_list(ClawtDeviceStore *self)
{
    GPtrArray *out;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_device_free);

    for (i = 0; i < self->entries->len; i++) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        g_ptr_array_add(out, clawt_device_copy(&entry->device));
    }

    return out;
}

ClawtDevice *
clawt_device_store_lookup(ClawtDeviceStore *self, const gchar *id)
{
    Entry *entry;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    entry = find_entry(self, id, NULL);

    return (entry != NULL) ? clawt_device_copy(&entry->device) : NULL;
}

gboolean
clawt_device_store_revoke(ClawtDeviceStore *self, const gchar *id,
                          GError **error)
{
    guint index = 0;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), FALSE);

    if (find_entry(self, id, &index) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no paired device '%s'", id != NULL ? id : "");
        return FALSE;
    }

    g_ptr_array_remove_index(self->entries, index);

    /*
     * In memory first, then on disk: a failed write still leaves the
     * token refused for as long as this daemon runs, and says so.
     */
    save_or_warn(self, "a device was revoked");

    return TRUE;
}

GPtrArray *
clawt_device_store_expire_idle(ClawtDeviceStore *self, gint64 now)
{
    GPtrArray *removed;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DEVICE_STORE(self), NULL);

    removed = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < self->entries->len;) {
        Entry *entry = g_ptr_array_index(self->entries, i);

        if (!entry_is_idle(self, entry, now)) {
            i++;
            continue;
        }

        g_message("devices: %s (%s) was idle past daemon.device_idle_days "
                  "and has been removed", entry->device.id,
                  entry->device.name);
        g_ptr_array_add(removed, g_strdup(entry->device.id));
        g_ptr_array_remove_index(self->entries, i);
    }

    if (removed->len > 0)
        save_or_warn(self, "idle devices were removed");

    /* And a code nobody used, so it is not held past its life. */
    if (self->code != NULL && now >= self->code_expires_at)
        clear_code(self);

    return removed;
}

static void
clawt_device_store_finalize(GObject *object)
{
    ClawtDeviceStore *self = CLAWT_DEVICE_STORE(object);

    clear_code(self);
    g_ptr_array_unref(self->entries);
    g_free(self->path);

    G_OBJECT_CLASS(clawt_device_store_parent_class)->finalize(object);
}

static void
clawt_device_store_class_init(ClawtDeviceStoreClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_device_store_finalize;
}

static void
clawt_device_store_init(ClawtDeviceStore *self)
{
    self->entries = g_ptr_array_new_with_free_func(entry_free);
}
