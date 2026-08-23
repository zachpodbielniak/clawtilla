/*
 * clawt-vm-image.c - Cloud images, fetched once and kept
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-vm-image.h"

#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

/* Big enough that a fast mirror is not throttled by round trips. */
#define CHUNK_BYTES (256 * 1024)

/* What a download is called before it is complete. */
#define PARTIAL_SUFFIX ".part"

/* Where an image records the URL it came from. */
#define SOURCE_SUFFIX ".source"

/* ── The catalog ─────────────────────────────────────────────────── */

static const ClawtVmImageSource sources[] = {
    { "fedora-44", "Fedora 44 Cloud Base",
      "the default, and what clawtilla is tested against", "Fedora",
      "https://download.fedoraproject.org/pub/fedora/linux/releases/44/"
      "Cloud/x86_64/images/",
      "Fedora-Cloud-Base-Generic-44-*.x86_64.qcow2",
      CLAWT_GUEST_FLAVOUR_FEDORA },

    { "fedora-43", "Fedora 43 Cloud Base",
      "the previous release", "Fedora",
      "https://download.fedoraproject.org/pub/fedora/linux/releases/43/"
      "Cloud/x86_64/images/",
      "Fedora-Cloud-Base-Generic-43-*.x86_64.qcow2",
      CLAWT_GUEST_FLAVOUR_FEDORA },

    { "centos-stream-10", "CentOS Stream 10",
      "closest to RHEL", "Enterprise Linux",
      "https://cloud.centos.org/centos/10-stream/x86_64/images/",
      "CentOS-Stream-GenericCloud-10-*.x86_64.qcow2",
      CLAWT_GUEST_FLAVOUR_ENTERPRISE },

    { "debian-13", "Debian 13 (trixie)",
      "small, and apt is familiar", "Debian and Ubuntu",
      "https://cloud.debian.org/images/cloud/trixie/latest/"
      "debian-13-generic-amd64.qcow2", NULL,
      CLAWT_GUEST_FLAVOUR_DEBIAN },

    { "debian-12", "Debian 12 (bookworm)",
      "the previous stable", "Debian and Ubuntu",
      "https://cloud.debian.org/images/cloud/bookworm/latest/"
      "debian-12-generic-amd64.qcow2", NULL,
      CLAWT_GUEST_FLAVOUR_DEBIAN },

    { "ubuntu-24.04", "Ubuntu 24.04 LTS",
      NULL, "Debian and Ubuntu",
      "https://cloud-images.ubuntu.com/noble/current/"
      "noble-server-cloudimg-amd64.img", NULL,
      CLAWT_GUEST_FLAVOUR_UBUNTU },

    /*
     * Arch publishes two qcow2s and only one of them is any use here:
     * `basic` has no cloud-init, so a VM built from it boots and admits
     * nobody, which looks exactly like a VM that failed to boot.  The
     * `cloudimg` variant is the one with cloud-init preinstalled.
     */
    { "arch", "Arch Linux",
      "rolling, and the only image here that is rebuilt monthly",
      "Rolling",
      "https://geo.mirror.pkgbuild.com/images/latest/"
      "Arch-Linux-x86_64-cloudimg.qcow2", NULL,
      CLAWT_GUEST_FLAVOUR_ARCH }
};

const ClawtVmImageSource *
clawt_vm_image_catalog(gsize *n_sources)
{
    g_return_val_if_fail(n_sources != NULL, NULL);

    *n_sources = G_N_ELEMENTS(sources);

    return sources;
}

/*
 * Which family an image belongs to.
 *
 * The catalog first, because that is authoritative.  Then the name,
 * because an image somebody fetched keeps its distribution in the
 * filename and that is the only clue there is short of booting it --
 * `debian-13-generic-amd64.qcow2`, `noble-server-cloudimg-amd64.img`,
 * `CentOS-Stream-GenericCloud-10-...`.
 *
 * Matched against the whole string rather than the basename: a path may
 * be `~/images/ubuntu/disk.qcow2`, where the only mention of the
 * distribution is a directory.
 */
static const struct {
    const gchar      *marker;
    ClawtGuestFlavour flavour;
} flavour_markers[] = {
    { "fedora",   CLAWT_GUEST_FLAVOUR_FEDORA },
    { "centos",   CLAWT_GUEST_FLAVOUR_ENTERPRISE },
    { "rhel",     CLAWT_GUEST_FLAVOUR_ENTERPRISE },
    { "rocky",    CLAWT_GUEST_FLAVOUR_ENTERPRISE },
    { "alma",     CLAWT_GUEST_FLAVOUR_ENTERPRISE },
    { "debian",   CLAWT_GUEST_FLAVOUR_DEBIAN },
    { "ubuntu",   CLAWT_GUEST_FLAVOUR_UBUNTU },
    /*
     * Ubuntu's own cloud images are named after the release adjective
     * and never say "ubuntu": the file is `noble-server-cloudimg`.
     */
    { "noble",    CLAWT_GUEST_FLAVOUR_UBUNTU },
    { "jammy",    CLAWT_GUEST_FLAVOUR_UBUNTU },
    { "trixie",   CLAWT_GUEST_FLAVOUR_DEBIAN },
    { "bookworm", CLAWT_GUEST_FLAVOUR_DEBIAN },
    /*
     * Spelled out rather than matched on "arch", which is a substring of
     * words that turn up in perfectly ordinary paths -- `research`,
     * `archive`, `~/archived-vms/debian.qcow2`.  Any of those would place
     * the image as Arch and install pacman's package names into a Debian.
     */
    { "arch-linux", CLAWT_GUEST_FLAVOUR_ARCH },
    { "archlinux",  CLAWT_GUEST_FLAVOUR_ARCH }
};

ClawtGuestFlavour
clawt_vm_image_flavour(const gchar *image)
{
    const ClawtVmImageSource *entry;
    g_autofree gchar *folded = NULL;
    gsize i;

    if (image == NULL || *image == '\0')
        return CLAWT_GUEST_FLAVOUR_AUTO;

    entry = clawt_vm_image_catalog_lookup(image);

    if (entry != NULL)
        return entry->flavour;

    folded = g_ascii_strdown(image, -1);

    for (i = 0; i < G_N_ELEMENTS(flavour_markers); i++) {
        if (strstr(folded, flavour_markers[i].marker) != NULL)
            return flavour_markers[i].flavour;
    }

    return CLAWT_GUEST_FLAVOUR_AUTO;
}

const ClawtVmImageSource *
clawt_vm_image_catalog_lookup(const gchar *id)
{
    gsize i;

    if (id == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(sources); i++) {
        if (g_strcmp0(sources[i].id, id) == 0)
            return &sources[i];
    }

    return NULL;
}

/* ── One image ───────────────────────────────────────────────────── */

ClawtVmImage *
clawt_vm_image_copy(ClawtVmImage *self)
{
    ClawtVmImage *copy;

    if (self == NULL)
        return NULL;

    copy = g_new0(ClawtVmImage, 1);
    copy->name = g_strdup(self->name);
    copy->path = g_strdup(self->path);
    copy->url = g_strdup(self->url);
    copy->bytes = self->bytes;
    copy->total = self->total;
    copy->downloading = self->downloading;

    return copy;
}

void
clawt_vm_image_free(ClawtVmImage *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->path);
    g_free(self->url);
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtVmImage, clawt_vm_image,
                    clawt_vm_image_copy, clawt_vm_image_free)

/* ── The store ───────────────────────────────────────────────────── */

typedef struct {
    ClawtVmImageStore *store;      /* borrowed: the store owns this */
    gchar             *name;
    gchar             *url;
    gchar             *target;
    gchar             *partial;
    GCancellable      *cancellable;
    SoupMessage       *message;
    gchar             *pattern;
    GInputStream      *input;
    GOutputStream     *output;
    goffset            done;
    goffset            total;
    goffset            announced;
    guint8            *buffer;
} Download;

struct _ClawtVmImageStore {
    GObject parent_instance;

    gchar       *directory;
    SoupSession *session;
    GHashTable  *downloads;   /* name -> Download * */
};

enum {
    SIGNAL_PROGRESS,
    SIGNAL_FINISHED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(ClawtVmImageStore, clawt_vm_image_store, G_TYPE_OBJECT)

static void read_chunk(Download *download);
static void send_request(Download *download);
static gchar *newest_match(const gchar *body, gsize length,
                           const gchar *pattern);

/*
 * Checks a bare filename we are willing to create.
 *
 * The result is joined onto the store's directory, so anything that could
 * climb out of it, or collide with the bookkeeping files beside an image,
 * is refused.  Refused rather than trimmed to its last component: a name
 * somebody typed and a name silently replaced by part of it are different
 * things, and the second is the one that surprises them later.
 */
static gchar *
safe_name(const gchar *candidate)
{
    if (candidate == NULL || *candidate == '\0' || *candidate == '.' ||
        strlen(candidate) > 200 ||
        strchr(candidate, '/') != NULL ||
        g_str_has_suffix(candidate, PARTIAL_SUFFIX) ||
        g_str_has_suffix(candidate, SOURCE_SUFFIX))
        return NULL;

    return g_strdup(candidate);
}

/*
 * A URL's last path component, if it makes a usable name.
 *
 * Taking the basename *is* right here -- a URL is a path, and its final
 * component is the filename it offers.
 */
static gchar *
name_from_path(const gchar *path)
{
    g_autofree gchar *base = g_path_get_basename(path);

    return safe_name(base);
}

static gchar *
name_from_url(const gchar *url)
{
    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);

    if (uri == NULL)
        return NULL;

    return name_from_path(g_uri_get_path(uri));
}

static void
download_free(gpointer data)
{
    Download *download = data;

    if (download == NULL)
        return;

    g_clear_object(&download->cancellable);
    g_clear_object(&download->message);
    g_clear_object(&download->input);
    g_clear_object(&download->output);
    g_free(download->pattern);
    g_free(download->name);
    g_free(download->url);
    g_free(download->target);
    g_free(download->partial);
    g_free(download->buffer);
    g_free(download);
}

/*
 * Ends a download, one way or the other.
 *
 * A failed or cancelled download leaves nothing behind: a half-written
 * disk image that looks like a whole one is worse than no image, because
 * it fails later, inside a VM that will not boot.
 */
static void
finish_download(Download *download, const gchar *error_message)
{
    ClawtVmImageStore *store = download->store;
    g_autofree gchar *name = g_strdup(download->name);
    g_autofree gchar *target = g_strdup(download->target);

    if (download->output != NULL) {
        g_output_stream_close(download->output, NULL, NULL);
        g_clear_object(&download->output);
    }

    g_clear_object(&download->input);

    if (error_message == NULL) {
        g_autofree gchar *source_path =
            g_strconcat(download->target, SOURCE_SUFFIX, NULL);

        if (g_rename(download->partial, download->target) != 0) {
            error_message = "the download could not be moved into place";
        } else {
            /*
             * Recorded so a person looking at a directory of qcow2 files
             * later can tell which is which, and re-fetch one.
             */
            g_autofree gchar *contents =
                g_strconcat(download->url, "\n", NULL);

            g_file_set_contents(source_path, contents, -1, NULL);
        }
    }

    if (error_message != NULL)
        g_unlink(download->partial);

    g_hash_table_remove(store->downloads, download->name);

    g_signal_emit(store, signals[SIGNAL_FINISHED], 0, name,
                  error_message == NULL ? target : NULL, error_message);
}

/*
 * Emits progress rarely enough to be watchable.
 *
 * A 500 MB image in 256 KB chunks is two thousand callbacks, and each one
 * becomes an event delivered to every connected client.  Whole percents
 * caps that at a hundred, which is all a progress bar can show anyway.
 */
static void
announce_progress(Download *download)
{
    gboolean worth_saying;

    if (download->total > 0)
        worth_saying = (download->done * 100 / download->total) !=
                       (download->announced * 100 / download->total);
    else
        worth_saying = download->done - download->announced >= 8 * 1024 * 1024;

    if (!worth_saying)
        return;

    download->announced = download->done;

    g_signal_emit(download->store, signals[SIGNAL_PROGRESS], 0,
                  download->name, (gint64)download->done,
                  (gint64)download->total);
}

static void
on_chunk_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Download *download = user_data;
    g_autoptr(GError) error = NULL;
    gssize read;

    read = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

    if (read < 0) {
        finish_download(download,
                        g_cancellable_is_cancelled(download->cancellable)
                            ? "cancelled" : error->message);
        return;
    }

    if (read == 0) {
        /*
         * A server that closed early leaves a short file that qemu will
         * open and a guest will fail to boot from, so a length that does
         * not match what was promised is a failure here rather than a
         * puzzle later.
         */
        if (download->total > 0 && download->done < download->total) {
            finish_download(download,
                            "the download ended early; the image is "
                            "incomplete");
            return;
        }

        finish_download(download, NULL);
        return;
    }

    /*
     * Written synchronously: this is a local file and a 256 KB write does
     * not meaningfully block, whereas threading the write turns the whole
     * transfer into a state machine for no gain.
     */
    if (!g_output_stream_write_all(download->output, download->buffer,
                                   (gsize)read, NULL, NULL, &error)) {
        finish_download(download, error->message);
        return;
    }

    download->done += read;
    announce_progress(download);

    read_chunk(download);
}

static void
read_chunk(Download *download)
{
    g_input_stream_read_async(download->input, download->buffer, CHUNK_BYTES,
                              G_PRIORITY_DEFAULT, download->cancellable,
                              on_chunk_read, download);
}

static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Download *download = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = NULL;

    download->input = soup_session_send_finish(SOUP_SESSION(source), result,
                                               &error);

    if (download->input == NULL) {
        finish_download(download,
                        g_cancellable_is_cancelled(download->cancellable)
                            ? "cancelled" : error->message);
        return;
    }

    /*
     * A 404 arrives as a perfectly good stream of error page, so the
     * status has to be looked at: without this a stale catalog entry
     * produced a qcow2 file containing HTML.
     */
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(download->message))) {
        g_autofree gchar *detail =
            g_strdup_printf("the server answered %u %s",
                            soup_message_get_status(download->message),
                            soup_message_get_reason_phrase(download->message));

        finish_download(download, detail);
        return;
    }

    download->total = soup_message_headers_get_content_length(
        soup_message_get_response_headers(download->message));

    file = g_file_new_for_path(download->partial);
    download->output = G_OUTPUT_STREAM(
        g_file_replace(file, NULL, FALSE, G_FILE_CREATE_PRIVATE, NULL,
                       &error));

    if (download->output == NULL) {
        finish_download(download, error->message);
        return;
    }

    g_signal_emit(download->store, signals[SIGNAL_PROGRESS], 0,
                  download->name, (gint64)0, (gint64)download->total);

    read_chunk(download);
}

static void
send_request(Download *download)
{
    g_clear_object(&download->message);
    download->message = soup_message_new("GET", download->url);

    if (download->message == NULL) {
        finish_download(download, "that is not a URL this can fetch");
        return;
    }

    /*
     * The message is kept on the download rather than beside the session:
     * soup_session_send_finish() hands back only the stream, and the
     * status and length are needed after it, while several downloads may
     * be in flight on the one session.
     */
    soup_session_send_async(download->store->session, download->message,
                            G_PRIORITY_DEFAULT, download->cancellable,
                            on_response, download);
}

/* ── Resolving a catalog entry to a file ─────────────────────────── */

/*
 * Picks the newest file matching @pattern out of a directory listing.
 *
 * The listing is HTML from whichever mirror answered, so this looks for
 * the pattern's literal prefix and takes what follows up to the next
 * delimiter, rather than pretending to parse the page.
 */
gchar *
clawt_vm_image_pick_newest(const gchar *listing, const gchar *pattern)
{
    g_return_val_if_fail(listing != NULL, NULL);
    g_return_val_if_fail(pattern != NULL, NULL);

    return newest_match(listing, strlen(listing), pattern);
}

static gchar *
newest_match(const gchar *body, gsize length, const gchar *pattern)
{
    g_autofree gchar *prefix = NULL;
    const gchar *star = strchr(pattern, '*');
    g_autofree gchar *best = NULL;
    const gchar *cursor;
    gsize prefix_length;

    if (star == NULL)
        return g_strdup(pattern);

    prefix = g_strndup(pattern, (gsize)(star - pattern));
    prefix_length = strlen(prefix);

    if (prefix_length == 0)
        return NULL;

    for (cursor = body; cursor != NULL && cursor < body + length; ) {
        const gchar *hit = g_strstr_len(cursor, body + length - cursor,
                                        prefix);
        const gchar *end;
        g_autofree gchar *candidate = NULL;

        if (hit == NULL)
            break;

        end = hit;
        while (end < body + length && *end != '"' && *end != '<' &&
               *end != '\'' && *end != ' ' && *end != '\n' && *end != '\r')
            end++;

        candidate = g_strndup(hit, (gsize)(end - hit));
        cursor = hit + prefix_length;

        if (!g_pattern_match_simple(pattern, candidate))
            continue;

        if (best == NULL || g_strcmp0(candidate, best) > 0) {
            g_free(best);
            best = g_steal_pointer(&candidate);
        }
    }

    return g_steal_pointer(&best);
}

static void
on_listing(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Download *download = user_data;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *filename = NULL;
    g_autofree gchar *resolved = NULL;
    gsize length = 0;
    const gchar *data;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             &error);

    if (body == NULL) {
        finish_download(download,
                        g_cancellable_is_cancelled(download->cancellable)
                            ? "cancelled" : error->message);
        return;
    }

    /*
     * A refused listing arrives as a perfectly good page of refusal, in
     * which nothing matches -- so without this the failure was reported
     * as "the catalog entry has gone stale", which sent you looking at
     * the wrong thing entirely.
     */
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(download->message))) {
        g_autofree gchar *detail =
            g_strdup_printf("could not list %s: the server answered %u %s",
                            download->url,
                            soup_message_get_status(download->message),
                            soup_message_get_reason_phrase(download->message));

        finish_download(download, detail);
        return;
    }

    data = g_bytes_get_data(body, &length);
    filename = newest_match(data, length, download->pattern);

    if (filename == NULL) {
        finish_download(download,
                        "no image matching that release is published there "
                        "any more; the catalog entry has gone stale");
        return;
    }

    resolved = g_strconcat(download->url, filename, NULL);
    g_free(download->url);
    download->url = g_steal_pointer(&resolved);

    send_request(download);
}

static void
resolve_and_send(Download *download)
{
    g_clear_object(&download->message);
    download->message = soup_message_new("GET", download->url);

    if (download->message == NULL) {
        finish_download(download, "that is not a URL this can fetch");
        return;
    }

    soup_session_send_and_read_async(download->store->session,
                                     download->message, G_PRIORITY_DEFAULT,
                                     download->cancellable, on_listing,
                                     download);
}

/* ── Public API ──────────────────────────────────────────────────── */

ClawtVmImageStore *
clawt_vm_image_store_new(const gchar *directory)
{
    ClawtVmImageStore *self = g_object_new(CLAWT_TYPE_VM_IMAGE_STORE, NULL);

    self->directory = clawt_expand_path(directory);

    return self;
}

GPtrArray *
clawt_vm_image_store_list(ClawtVmImageStore *self)
{
    GPtrArray *images;
    g_autoptr(GDir) dir = NULL;
    GHashTableIter iter;
    gpointer value;
    const gchar *entry;

    g_return_val_if_fail(CLAWT_IS_VM_IMAGE_STORE(self), NULL);

    images = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_vm_image_free);

    dir = g_dir_open(self->directory, 0, NULL);

    while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autofree gchar *source_path = NULL;
        ClawtVmImage *image;
        GStatBuf info;

        if (g_str_has_suffix(entry, PARTIAL_SUFFIX) ||
            g_str_has_suffix(entry, SOURCE_SUFFIX))
            continue;

        path = g_build_filename(self->directory, entry, NULL);

        if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;

        image = g_new0(ClawtVmImage, 1);
        image->name = g_strdup(entry);
        image->bytes = (gint64)info.st_size;
        image->total = image->bytes;

        source_path = g_strconcat(path, SOURCE_SUFFIX, NULL);

        if (g_file_get_contents(source_path, &image->url, NULL, NULL))
            g_strstrip(image->url);

        image->path = g_steal_pointer(&path);

        g_ptr_array_add(images, image);
    }

    /*
     * The ones still arriving are listed too, so a client shows a row with
     * a bar rather than nothing at all until it completes.
     */
    g_hash_table_iter_init(&iter, self->downloads);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Download *download = value;
        ClawtVmImage *image = g_new0(ClawtVmImage, 1);

        image->name = g_strdup(download->name);
        image->path = g_strdup(download->target);
        image->url = g_strdup(download->url);
        image->bytes = (gint64)download->done;
        image->total = (gint64)download->total;
        image->downloading = TRUE;

        g_ptr_array_add(images, image);
    }

    return images;
}

gchar *
clawt_vm_image_store_path(ClawtVmImageStore *self, const gchar *name)
{
    g_autofree gchar *safe = NULL;
    g_autofree gchar *path = NULL;

    g_return_val_if_fail(CLAWT_IS_VM_IMAGE_STORE(self), NULL);

    safe = safe_name(name);

    if (safe == NULL)
        return NULL;

    path = g_build_filename(self->directory, safe, NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return NULL;

    return g_steal_pointer(&path);
}

gchar *
clawt_vm_image_store_start(ClawtVmImageStore  *self,
                           const gchar        *url,
                           const gchar        *name,
                           GError            **error)
{
    const ClawtVmImageSource *source = NULL;
    g_autofree gchar *chosen = NULL;
    g_autofree gchar *target = NULL;
    const gchar *fetch_url;
    Download *download;

    g_return_val_if_fail(CLAWT_IS_VM_IMAGE_STORE(self), NULL);
    g_return_val_if_fail(url != NULL, NULL);

    /* A catalog id is accepted anywhere a URL is, so a client can offer
     * the list without knowing where any of it lives. */
    source = clawt_vm_image_catalog_lookup(url);
    fetch_url = source != NULL ? source->url : url;

    if (!g_str_has_prefix(fetch_url, "http://") &&
        !g_str_has_prefix(fetch_url, "https://")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "only http and https URLs can be fetched, and '%s' is "
                    "neither -- a local image needs no downloading: point "
                    "computer.vm.image straight at it", url);
        return NULL;
    }

    if (name != NULL)
        chosen = safe_name(name);
    else if (source == NULL || source->pattern == NULL)
        chosen = name_from_url(fetch_url);
    else
        chosen = g_strdup(source->id);

    if (chosen == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "there is no usable filename in '%s'; give one with a "
                    "name of your own", url);
        return NULL;
    }

    if (g_hash_table_contains(self->downloads, chosen)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is already downloading", chosen);
        return NULL;
    }

    if (!clawt_ensure_dir(self->directory, 0700, error))
        return NULL;

    target = g_build_filename(self->directory, chosen, NULL);

    if (g_file_test(target, G_FILE_TEST_EXISTS)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is already here; remove it first to fetch it "
                    "again", chosen);
        return NULL;
    }

    download = g_new0(Download, 1);
    download->store = self;
    download->name = g_strdup(chosen);
    download->url = g_strdup(fetch_url);
    download->target = g_steal_pointer(&target);
    download->partial = g_strconcat(download->target, PARTIAL_SUFFIX, NULL);
    download->cancellable = g_cancellable_new();
    download->buffer = g_malloc(CHUNK_BYTES);

    g_hash_table_insert(self->downloads, g_strdup(chosen), download);

    if (source != NULL && source->pattern != NULL) {
        download->pattern = g_strdup(source->pattern);
        resolve_and_send(download);
    } else {
        send_request(download);
    }

    return g_steal_pointer(&chosen);
}

gboolean
clawt_vm_image_store_cancel(ClawtVmImageStore *self, const gchar *name)
{
    Download *download;

    g_return_val_if_fail(CLAWT_IS_VM_IMAGE_STORE(self), FALSE);

    download = g_hash_table_lookup(self->downloads, name);

    if (download == NULL)
        return FALSE;

    g_cancellable_cancel(download->cancellable);

    return TRUE;
}

gboolean
clawt_vm_image_store_remove(ClawtVmImageStore  *self,
                            const gchar        *name,
                            GError            **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *source_path = NULL;

    g_return_val_if_fail(CLAWT_IS_VM_IMAGE_STORE(self), FALSE);

    if (clawt_vm_image_store_cancel(self, name))
        return TRUE;

    path = clawt_vm_image_store_path(self, name);

    if (path == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no image called '%s'", name);
        return FALSE;
    }

    if (g_unlink(path) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not remove %s: %s", path, g_strerror(errno));
        return FALSE;
    }

    source_path = g_strconcat(path, SOURCE_SUFFIX, NULL);
    g_unlink(source_path);

    return TRUE;
}

static void
clawt_vm_image_store_dispose(GObject *object)
{
    ClawtVmImageStore *self = CLAWT_VM_IMAGE_STORE(object);
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, self->downloads);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_cancellable_cancel(((Download *)value)->cancellable);

    g_hash_table_remove_all(self->downloads);
    g_clear_object(&self->session);

    G_OBJECT_CLASS(clawt_vm_image_store_parent_class)->dispose(object);
}

static void
clawt_vm_image_store_finalize(GObject *object)
{
    ClawtVmImageStore *self = CLAWT_VM_IMAGE_STORE(object);

    g_clear_pointer(&self->downloads, g_hash_table_unref);
    g_clear_pointer(&self->directory, g_free);

    G_OBJECT_CLASS(clawt_vm_image_store_parent_class)->finalize(object);
}

static void
clawt_vm_image_store_class_init(ClawtVmImageStoreClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_vm_image_store_dispose;
    object_class->finalize = clawt_vm_image_store_finalize;

    /**
     * ClawtVmImageStore::progress:
     * @self: the store
     * @name: the image
     * @done: bytes received
     * @total: bytes expected, or 0 when the server did not say
     */
    signals[SIGNAL_PROGRESS] =
        g_signal_new("progress", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                     G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT64);

    /**
     * ClawtVmImageStore::finished:
     * @self: the store
     * @name: the image
     * @path: (nullable): where it landed, or %NULL if it did not
     * @error: (nullable): why not, or %NULL on success
     */
    signals[SIGNAL_FINISHED] =
        g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void
clawt_vm_image_store_init(ClawtVmImageStore *self)
{
    /*
     * Named, because several mirrors refuse a request that does not say
     * who it is: cloud.centos.org answers 403 to libsoup's default of no
     * User-Agent at all, and the 403 page then looks like a directory
     * with no images in it.
     */
    self->session = soup_session_new_with_options(
        "user-agent",
        "clawtilla/" G_STRINGIFY(CLAWT_VERSION_MAJOR) "."
        G_STRINGIFY(CLAWT_VERSION_MINOR) "."
        G_STRINGIFY(CLAWT_VERSION_MICRO), NULL);
    self->downloads = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, download_free);

    /*
     * A mirror that has stopped sending must not hold a download open for
     * ever; the transfer itself is unbounded, only the wait for the next
     * byte is capped.
     */
    soup_session_set_timeout(self->session, 60);
    soup_session_set_idle_timeout(self->session, 60);
}
