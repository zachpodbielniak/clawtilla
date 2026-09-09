/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "clawt-backup.h"
#include "clawt-error.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define ARCHIVE_LIMIT (256 * 1024 * 1024)
#define FILE_LIMIT (64 * 1024 * 1024)
#define ENTRY_LIMIT (100000)
#define MAGIC "CLAWT-BACKUP-1\n"

static gboolean safe_path(const gchar *path);

/* Only the generated per-agent config contains resolved credentials.
 * Project files named config.yaml inside workspaces are ordinary state. */
static gboolean
generated_config(const gchar *path)
{
	g_auto(GStrv) parts = g_strsplit(path, "/", -1);
	return g_strv_length(parts) == 3 && g_str_equal(parts[0], "agents") &&
		g_str_equal(parts[2], "config.yaml");
}

/* One error domain keeps filesystem and format failures usable by every client. */
static gboolean
fail(GError **error, const gchar *message)
{
	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s", message);
	return FALSE;
}

/* Configuration and arbitrary workspace files may contain inline credentials.
 * This policy excludes known credential locations; it is not a content scrubber. */
static gboolean
excluded(const gchar *name)
{
	return g_str_equal(name, ".") || g_str_equal(name, "..") ||
		g_str_equal(name, ".git") || g_str_equal(name, ".env") ||
		g_str_equal(name, ".mcp.json") || g_str_equal(name, ".ssh") ||
		g_str_equal(name, "secrets") ||
		g_str_equal(name, "credentials") || g_str_equal(name, "token") ||
		g_str_equal(name, "tcp-token") || g_str_equal(name, "daemon.lock") ||
		g_str_equal(name, "sessions");
}

/* Read with a hard cap before allocating, and never follow a final symlink. */
static GBytes *
read_file(gint parent, const gchar *name, gsize limit, GError **error)
{
	gint fd;
	struct stat st;
	gchar *data;
	gsize offset;
	ssize_t count;
	gchar extra;

	fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		fail(error, g_strerror(errno));
		return NULL;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
		(guint64)st.st_size > limit) {
		close(fd);
		fail(error, "Not a regular file, or backup size limit exceeded");
		return NULL;
	}
	data = g_malloc((gsize)st.st_size + 1);
	offset = 0;
	while (offset < (gsize)st.st_size) {
		count = read(fd, data + offset, (gsize)st.st_size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		offset += (gsize)count;
	}
	count = read(fd, &extra, 1);
	close(fd);
	if (offset != (gsize)st.st_size || count != 0) {
		g_free(data);
		fail(error, "File changed or failed while reading backup");
		return NULL;
	}
	return g_bytes_new_take(data, offset);
}

/* Descriptor-relative traversal prevents symlinks from escaping the state tree. */
static gboolean
collect(gint fd, const gchar *prefix, GVariantBuilder *builder,
	gsize *total, guint *entries, guint depth, GError **error)
{
	g_autofree gchar *proc = g_strdup_printf("/proc/self/fd/%d", fd);
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) io_error = NULL;
	const gchar *name;

	if (depth > 64)
		return fail(error, "Backup directory nesting exceeds 64 levels");
	dir = g_dir_open(proc, 0, &io_error);
	if (dir == NULL)
		return fail(error, io_error->message);
	while ((name = g_dir_read_name(dir)) != NULL) {
		struct stat st;
		g_autofree gchar *path = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *sum = NULL;
		gint child;

		if (excluded(name))
			continue;
		if (++*entries > ENTRY_LIMIT)
			return fail(error, "Backup exceeds entry limit");
		path = *prefix ? g_strconcat(prefix, "/", name, NULL) : g_strdup(name);
		if (generated_config(path))
			continue;
		if (!safe_path(path))
			return fail(error, "Backup filenames must be safe UTF-8 relative paths without control characters");
		if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			return fail(error, g_strerror(errno));
		if (S_ISLNK(st.st_mode))
			return fail(error, "Backup refuses symlinks; move them outside the state tree");
		if (S_ISDIR(st.st_mode)) {
			child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child < 0)
				return fail(error, g_strerror(errno));
			if (!collect(child, path, builder, total, entries, depth + 1, error)) {
				close(child);
				return FALSE;
			}
			close(child);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		bytes = read_file(fd, name, MIN((gsize)FILE_LIMIT, ARCHIVE_LIMIT - *total), error);
		if (bytes == NULL)
			return FALSE;
		*total += g_bytes_get_size(bytes) + strlen(path) + 128;
		if (*total > ARCHIVE_LIMIT)
			return fail(error, "Backup exceeds archive size or entry limit");
		sum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
		g_variant_builder_add(builder, "(ss@ay)", path, sum,
			g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), bytes, TRUE));
	}
	return TRUE;
}

gboolean
clawt_backup_create(const gchar *state_dir, const gchar *archive, GError **error)
{
	gint fd, lock_fd;
	struct stat lock_stat;
	GVariantBuilder builder;
	g_autoptr(GVariant) payload = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *sum = NULL;
	g_autofree gchar *header = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileOutputStream) out = NULL;
	g_autoptr(GError) io_error = NULL;
	gsize total = 0;
	guint entries = 0;
	gboolean ok;

	fd = open(state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return fail(error, g_strerror(errno));
	lock_fd = openat(fd, "daemon.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	if (lock_fd < 0 || fstat(lock_fd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
		flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		if (lock_fd >= 0)
			close(lock_fd);
		close(fd);
		return fail(error, "Cannot lock state directory; stop the daemon before backup");
	}
	g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ssay)"));
	ok = collect(fd, "", &builder, &total, &entries, 0, error);
	if (ok)
		payload = g_variant_ref_sink(g_variant_builder_end(&builder));
	else
		g_variant_builder_clear(&builder);
	close(fd);
	if (!ok) {
		close(lock_fd);
		return FALSE;
	}
	bytes = g_variant_get_data_as_bytes(payload);
	if (g_bytes_get_size(bytes) > ARCHIVE_LIMIT) {
		close(lock_fd);
		return fail(error, "Serialized archive exceeds 256 MiB");
	}
	sum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
	header = g_strdup_printf(MAGIC "%s\n", sum);
	file = g_file_new_for_path(archive);
	out = g_file_create(file, G_FILE_CREATE_PRIVATE, NULL, &io_error);
	ok = out != NULL &&
		g_output_stream_write_all(G_OUTPUT_STREAM(out), header, strlen(header), NULL, NULL, &io_error) &&
		g_output_stream_write_all(G_OUTPUT_STREAM(out), g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), NULL, NULL, &io_error) &&
		g_output_stream_close(G_OUTPUT_STREAM(out), NULL, &io_error);
	if (!ok && out != NULL)
		g_file_delete(file, NULL, NULL);
	close(lock_fd);
	if (!ok)
		return fail(error, io_error->message);
	return ok;
}

/* Reject traversal, hidden credentials and ambiguous names before any writes. */
static gboolean
safe_path(const gchar *path)
{
	g_auto(GStrv) parts = NULL;
	guint i;
	const guchar *p;
	if (strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL))
		return FALSE;
	/* Bound attacker-controlled input before allocating one entry per
	 * separator. The enclosing archive cap is far too large for a path. */
	parts = g_strsplit(path, "/", -1);
	if (generated_config(path))
		return FALSE;
	for (p = (const guchar *)path; *p; p++) {
		if (*p < 32 || *p == 127)
			return FALSE;
	}
	for (i = 0; parts[i] != NULL; i++) {
		if (!*parts[i] || excluded(parts[i]) || i > 64)
			return FALSE;
	}
	return TRUE;
}

/* Every restore component is opened without following symlinks, even if another
 * process of the same user interferes with the private destination. */
static gboolean
restore_file(gint root, const gchar *path, GVariant *data, GError **error)
{
	g_auto(GStrv) parts = g_strsplit(path, "/", -1);
	gint fd = dup(root);
	gint next;
	guint i;
	gsize size, offset = 0;
	const gchar *bytes = g_variant_get_fixed_array(data, &size, 1);
	ssize_t count;

	if (fd < 0)
		return fail(error, g_strerror(errno));
	for (i = 0; parts[i + 1] != NULL; i++) {
		if (mkdirat(fd, parts[i], 0700) != 0 && errno != EEXIST) {
			close(fd);
			return fail(error, g_strerror(errno));
		}
		next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		fd = next;
		if (fd < 0)
			return fail(error, g_strerror(errno));
	}
	next = openat(fd, parts[i], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	close(fd);
	if (next < 0)
		return fail(error, g_strerror(errno));
	while (offset < size) {
		count = write(next, bytes + offset, size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		offset += (gsize)count;
	}
	if (close(next) != 0 || offset != size)
		return fail(error, "Could not write restored file; destination is incomplete");
	return TRUE;
}

gchar *
clawt_backup_restore(const gchar *archive, const gchar *destination, GError **error)
{
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GVariant) payload = NULL;
	g_autoptr(GHashTable) paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GString) manifest = g_string_new(NULL);
	g_autofree gchar *sum = NULL;
	const gchar *raw;
	gsize size, i;
	gsize header_size = strlen(MAGIC) + 65;
	gint root = -1;

	bytes = read_file(AT_FDCWD, archive, ARCHIVE_LIMIT + header_size, error);
	if (bytes == NULL)
		return NULL;
	raw = g_bytes_get_data(bytes, &size);
	if (size < header_size || memcmp(raw, MAGIC, strlen(MAGIC)) != 0 || raw[header_size - 1] != '\n') {
		fail(error, "Invalid backup header");
		return NULL;
	}
	body = g_bytes_new_from_bytes(bytes, header_size, size - header_size);
	sum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
	if (memcmp(raw + strlen(MAGIC), sum, 64) != 0) {
		fail(error, "Archive SHA256 mismatch");
		return NULL;
	}
	payload = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("a(ssay)"), body, FALSE));
	if (!g_variant_is_normal_form(payload) || g_variant_n_children(payload) > ENTRY_LIMIT) {
		fail(error, "Invalid backup manifest");
		return NULL;
	}
	for (i = 0; i < g_variant_n_children(payload); i++) {
		const gchar *path, *expected;
		g_autoptr(GVariant) data = NULL;
		g_autoptr(GBytes) contents = NULL;
		g_autofree gchar *actual = NULL;
		g_variant_get_child(payload, i, "(&s&s@ay)", &path, &expected, &data);
		contents = g_variant_get_data_as_bytes(data);
		actual = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, contents);
		if (!safe_path(path) || g_hash_table_contains(paths, path) ||
			g_bytes_get_size(contents) > FILE_LIMIT || !g_str_equal(actual, expected)) {
			fail(error, "Unsafe, duplicate or corrupt backup member");
			return NULL;
		}
		g_hash_table_add(paths, g_strdup(path));
		g_string_append_printf(manifest, "%s\n", path);
	}
	/* Detect file/directory collisions during verification, not halfway through restore. */
	for (i = 0; i < g_variant_n_children(payload); i++) {
		const gchar *path;
		g_autofree gchar *parent = NULL;
		gchar *slash;
		g_variant_get_child(payload, i, "(&s&s@ay)", &path, NULL, NULL);
		parent = g_strdup(path);
		while ((slash = strrchr(parent, '/')) != NULL) {
			*slash = '\0';
			if (g_hash_table_contains(paths, parent)) {
				fail(error, "Backup member conflicts with a parent directory");
				return NULL;
			}
		}
	}
	if (destination == NULL)
		return g_string_free(g_steal_pointer(&manifest), FALSE);
	if (g_mkdir(destination, 0700) != 0) {
		fail(error, "Restore requires a new destination directory with an existing parent");
		return NULL;
	}
	root = open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0) {
		fail(error, g_strerror(errno));
		return NULL;
	}
	for (i = 0; i < g_variant_n_children(payload); i++) {
		const gchar *path;
		g_autoptr(GVariant) data = NULL;
		g_variant_get_child(payload, i, "(&s&s@ay)", &path, NULL, &data);
		if (!restore_file(root, path, data, error)) {
			close(root);
			return NULL;
		}
	}
	close(root);
	return g_string_free(g_steal_pointer(&manifest), FALSE);
}
