/*
 * clawt-office-frames.c - The host side of libregnum's frame protocol
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Written from libregnum's docs/modules/remote/frame-protocol.org and
 * checked against src/remote/lrg-frame-wire.c and lrg-frame-receiver.c,
 * which are not linked: libclawt must never pull libregnum in, and the
 * GTK client that uses this must never link it either.
 *
 * One difference from the reference reader is deliberate.  It reads in
 * 64 KiB chunks until the socket is empty, so its buffer is bounded by
 * nothing but how fast the sender writes; this one never reads past the
 * end of the message it is assembling, skips an unknown type's body
 * without keeping it, and refuses an oversized header before reading the
 * body it announces.  A limit applied after the read is not a limit.
 */

/* F_GET_SEALS and the seal bits, which gnu89 alone does not declare */
#define _GNU_SOURCE

#include "clawt-office-frames.h"
#include "clawt-error.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* how much one dispatch reads before giving the loop back */
#define DISPATCH_BUDGET_BYTES ((gsize)64 * 1024 * 1024)
#define DISPATCH_BUDGET_MESSAGES (256)

/* an unknown body is discarded through this, never kept */
#define SKIP_CHUNK (65536)

/* descriptors received and not yet paired with a frame */
#define MAX_QUEUED_FDS (8)
#define MAX_FDS_PER_READ (8)

/* input that the socket has not taken: past this the peer is not reading */
#define MAX_PENDING_OUTPUT ((gsize)1024 * 1024)

struct _ClawtOfficeFrames {
    GObject     parent_instance;

    gint        fd;
    gboolean    own_fd;

    /* the message being assembled: never more than one */
    GByteArray *buf;
    gsize       need;               /* the whole size of the message in buf */
    guint32     skip_remaining;     /* body bytes of an unknown type still to discard */
    GQueue     *fds;                /* received descriptors, as fd + 1 */
    gboolean    broken;

    /* the hello */
    gboolean    have_hello;
    guint32     capabilities;
    guint       hello_width;
    guint       hello_height;

    /* the newest frame not yet taken */
    GBytes     *frame;
    guint       frame_width;
    guint       frame_height;
    guint       frame_stride;
    guint64     frame_number;
    guint64     received;

    /* input not yet written */
    GByteArray *pending;
};

G_DEFINE_FINAL_TYPE(ClawtOfficeFrames, clawt_office_frames, G_TYPE_OBJECT)

/* ── Little-endian, whatever the host ─────────────────────────────── */

static void
put_u16(guint8 *p, guint16 v)
{
    p[0] = (guint8)(v & 0xFF);
    p[1] = (guint8)(v >> 8);
}

static void
put_u32(guint8 *p, guint32 v)
{
    p[0] = (guint8)(v & 0xFF);
    p[1] = (guint8)((v >> 8) & 0xFF);
    p[2] = (guint8)((v >> 16) & 0xFF);
    p[3] = (guint8)(v >> 24);
}

static void
put_f32(guint8 *p, gfloat v)
{
    guint32 bits;

    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

static guint16
get_u16(const guint8 *p)
{
    return (guint16)(p[0] | (p[1] << 8));
}

static guint32
get_u32(const guint8 *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) |
           ((guint32)p[3] << 24);
}

static guint64
get_u64(const guint8 *p)
{
    return (guint64)get_u32(p) | ((guint64)get_u32(p + 4) << 32);
}

/* ── Lifetime ─────────────────────────────────────────────────────── */

static void
close_queued_fds(ClawtOfficeFrames *self)
{
    while (self->fds != NULL && !g_queue_is_empty(self->fds))
        close(GPOINTER_TO_INT(g_queue_pop_head(self->fds)) - 1);
}

static void
clawt_office_frames_finalize(GObject *object)
{
    ClawtOfficeFrames *self = CLAWT_OFFICE_FRAMES(object);

    close_queued_fds(self);
    g_clear_pointer(&self->fds, g_queue_free);
    g_clear_pointer(&self->buf, g_byte_array_unref);
    g_clear_pointer(&self->pending, g_byte_array_unref);
    g_clear_pointer(&self->frame, g_bytes_unref);
    if (self->own_fd && self->fd >= 0)
        close(self->fd);

    G_OBJECT_CLASS(clawt_office_frames_parent_class)->finalize(object);
}

static void
clawt_office_frames_class_init(ClawtOfficeFramesClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_office_frames_finalize;
}

static void
clawt_office_frames_init(ClawtOfficeFrames *self)
{
    self->fd = -1;
    self->buf = g_byte_array_new();
    self->fds = g_queue_new();
    self->pending = g_byte_array_new();
}

ClawtOfficeFrames *
clawt_office_frames_new(gint fd, gboolean take_fd)
{
    ClawtOfficeFrames *self;

    g_return_val_if_fail(fd >= 0, NULL);

    self = g_object_new(CLAWT_TYPE_OFFICE_FRAMES, NULL);
    self->fd = fd;
    self->own_fd = take_fd;
    return self;
}

gint
clawt_office_frames_get_fd(ClawtOfficeFrames *self)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), -1);
    return self->fd;
}

/* ── Errors ───────────────────────────────────────────────────────── */

static gboolean
protocol_error(ClawtOfficeFrames *self, GError **error, const gchar *format, ...)
    G_GNUC_PRINTF(3, 4);

/* Marks the stream broken: nothing after a bad message can be trusted. */
static gboolean
protocol_error(ClawtOfficeFrames *self, GError **error, const gchar *format, ...)
{
    va_list args;
    g_autofree gchar *message = NULL;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    self->broken = TRUE;
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL, message);
    return FALSE;
}

static gboolean
errno_error(ClawtOfficeFrames *self, GError **error, gint err, const gchar *what)
{
    self->broken = TRUE;
    if (err == EPIPE || err == ECONNRESET)
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                    "%s: the office renderer closed the connection", what);
    else
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", what,
                    g_strerror(err));
    return FALSE;
}

/* ── Reading ──────────────────────────────────────────────────────── */

typedef enum {
    READ_GOT,       /* some bytes arrived */
    READ_AGAIN,     /* nothing now: the socket is empty */
    READ_FAILED     /* error set */
} ReadResult;

/*
 * One recvmsg() of at most @want bytes into @into, collecting any
 * descriptors that came with them.  @want is always the remainder of
 * the current message (or of a skipped body), so the buffer can never
 * hold more than one message.
 */
static ReadResult
read_some(ClawtOfficeFrames *self, guint8 *into, gsize want, gsize *got,
          GError **error)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        gchar          buf[CMSG_SPACE(sizeof(gint) * MAX_FDS_PER_READ)];
    } control;
    struct cmsghdr *cmsg;
    gssize n;

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = into;
    iov.iov_len = want;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    do
        n = recvmsg(self->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return READ_AGAIN;
        errno_error(self, error, errno, "recvmsg");
        return READ_FAILED;
    }

    /* descriptors first: they are ours to close whatever happens next */
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        gsize count;
        gsize i;

        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(gint);
        for (i = 0; i < count; i++) {
            gint fd;

            memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(gint), sizeof(gint));
            g_queue_push_tail(self->fds, GINT_TO_POINTER(fd + 1));
        }
    }
    if (msg.msg_flags & MSG_CTRUNC) {
        protocol_error(self, error, "too many descriptors in one read: frames can no "
                                    "longer be paired with them");
        return READ_FAILED;
    }
    /* a sender passing descriptors with no frames to own them is hoarding ours */
    if (g_queue_get_length(self->fds) > MAX_QUEUED_FDS) {
        protocol_error(self, error, "%u descriptors arrived with no frame to own them",
                       g_queue_get_length(self->fds));
        return READ_FAILED;
    }

    if (n == 0) {
        self->broken = TRUE;
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the office renderer closed the connection");
        return READ_FAILED;
    }
    *got = (gsize)n;
    return READ_GOT;
}

/* a memfd frame's mapping, released with the last reference to its pixels */
typedef struct {
    gpointer base;
    gsize    size;
} Mapping;

static void
unmap_pixels(gpointer data)
{
    Mapping *mapping = data;

    munmap(mapping->base, mapping->size);
    g_free(mapping);
}

/*
 * The frame's fixed part, checked as soon as it is here -- before an
 * inline frame's pixels are read, so a frame that does not add up costs
 * 64 bytes and not the body it announced.
 */
static gboolean
check_frame_head(ClawtOfficeFrames *self, const guint8 *b, guint32 body_size,
                 GError **error)
{
    guint32 width = get_u32(b);
    guint32 height = get_u32(b + 4);
    guint32 stride = get_u32(b + 8);
    guint32 format = get_u32(b + 12);
    guint32 flags = get_u32(b + 32);
    guint64 pixel_bytes = get_u32(b + 36);

    if (format != CLAWT_OFFICE_FRAMES_FORMAT_RGBA8 || width == 0 || height == 0 ||
        width > CLAWT_OFFICE_FRAMES_MAX_DIMENSION ||
        height > CLAWT_OFFICE_FRAMES_MAX_DIMENSION ||
        (guint64)stride < (guint64)width * 4u ||
        pixel_bytes != (guint64)stride * height)
        return protocol_error(self, error,
                              "a %ux%u frame (stride %u, %" G_GUINT64_FORMAT
                              " bytes, format %u) does not add up",
                              width, height, stride, pixel_bytes, format);

    if (!(flags & CLAWT_OFFICE_FRAMES_FLAG_PIXELS_IN_FD) &&
        (guint64)body_size < CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE + pixel_bytes)
        return protocol_error(self, error, "an inline frame of %u bytes is shorter "
                              "than its %" G_GUINT64_FORMAT " bytes of pixels",
                              body_size, pixel_bytes);
    return TRUE;
}

/* A whole frame message: keep its pixels as the newest frame. */
static gboolean
handle_frame(ClawtOfficeFrames *self, const guint8 *b, GError **error)
{
    guint32 width = get_u32(b);
    guint32 height = get_u32(b + 4);
    guint32 stride = get_u32(b + 8);
    guint32 flags = get_u32(b + 32);
    gsize pixel_bytes = get_u32(b + 36);
    GBytes *pixels;

    if (flags & CLAWT_OFFICE_FRAMES_FLAG_PIXELS_IN_FD) {
        struct stat st;
        gint fd;
        gint seals;
        gpointer map;

        /* descriptors pair with frames in arrival order */
        if (g_queue_is_empty(self->fds))
            return protocol_error(self, error,
                                  "a frame in a memfd arrived without its descriptor");
        fd = GPOINTER_TO_INT(g_queue_pop_head(self->fds)) - 1;

        /*
         * The mapping outlives this call -- it is the texture's memory --
         * so the memfd must be sealed against shrinking and writing: a
         * file shrunk under a live mapping is a SIGBUS in the renderer
         * of the window, not in the application that shrank it.
         */
        seals = fcntl(fd, F_GET_SEALS);
        if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_WRITE)) !=
                         (F_SEAL_SHRINK | F_SEAL_WRITE)) {
            close(fd);
            return protocol_error(self, error, "a frame's memfd is not sealed against "
                                               "shrinking and writing");
        }
        if (fstat(fd, &st) < 0 || (guint64)st.st_size < (guint64)pixel_bytes) {
            close(fd);
            return protocol_error(self, error,
                                  "a frame's memfd is smaller than its pixels");
        }
        map = mmap(NULL, pixel_bytes, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (map == MAP_FAILED)
            return errno_error(self, error, errno, "mmap");
        {
            Mapping *mapping = g_new0(Mapping, 1);

            mapping->base = map;
            mapping->size = pixel_bytes;
            pixels = g_bytes_new_with_free_func(map, pixel_bytes, unmap_pixels, mapping);
        }
    } else {
        pixels = g_bytes_new(b + CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE, pixel_bytes);
    }

    g_clear_pointer(&self->frame, g_bytes_unref);
    self->frame = pixels;
    self->frame_width = width;
    self->frame_height = height;
    self->frame_stride = stride;
    self->frame_number = get_u64(b + 16);
    self->received++;
    return TRUE;
}

/* A whole message is in buf: act on it and start the next one. */
static gboolean
handle_message(ClawtOfficeFrames *self, GError **error)
{
    const guint8 *body = self->buf->data + CLAWT_OFFICE_FRAMES_HEADER_SIZE;
    guint16 type = get_u16(self->buf->data + 6);
    guint32 body_size = get_u32(self->buf->data + 8);
    gboolean ok = TRUE;

    if (type == CLAWT_OFFICE_FRAMES_MSG_HELLO && body_size >= 16) {
        self->have_hello = TRUE;
        self->capabilities = get_u32(body);
        self->hello_width = get_u32(body + 4);
        self->hello_height = get_u32(body + 8);
    } else if (type == CLAWT_OFFICE_FRAMES_MSG_FRAME) {
        ok = handle_frame(self, body, error);
    }
    g_byte_array_set_size(self->buf, 0);
    self->need = 0;
    return ok;
}

/*
 * The header is here: check it, and decide what the rest of the message
 * is.  A known type is assembled (self->need is its whole size); any
 * other has its body skipped without being kept.
 */
static gboolean
check_header(ClawtOfficeFrames *self, GError **error)
{
    const guint8 *p = self->buf->data;
    guint32 magic = get_u32(p);
    guint16 version = get_u16(p + 4);
    guint16 type = get_u16(p + 6);
    guint32 body_size = get_u32(p + 8);

    if (magic != CLAWT_OFFICE_FRAMES_MAGIC)
        return protocol_error(self, error, "bad magic 0x%08x: not the frame protocol",
                              magic);
    if (version != CLAWT_OFFICE_FRAMES_VERSION)
        return protocol_error(self, error, "frame protocol version %u; this side "
                              "speaks %u", version, CLAWT_OFFICE_FRAMES_VERSION);
    if (body_size > CLAWT_OFFICE_FRAMES_MAX_BODY)
        return protocol_error(self, error, "a %u byte message is larger than the "
                              "frame protocol allows", body_size);
    if (type == CLAWT_OFFICE_FRAMES_MSG_FRAME &&
        body_size < CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE)
        return protocol_error(self, error, "a frame message of %u bytes is too short",
                              body_size);

    if (type != CLAWT_OFFICE_FRAMES_MSG_HELLO && type != CLAWT_OFFICE_FRAMES_MSG_FRAME) {
        /* a later version's message, or one for the other direction */
        self->skip_remaining = body_size;
        self->need = 0;
        g_byte_array_set_size(self->buf, 0);
        return TRUE;
    }
    self->need = CLAWT_OFFICE_FRAMES_HEADER_SIZE + (gsize)body_size;
    return TRUE;
}

/* Reads at most @want more bytes of the current message onto buf. */
static ReadResult
read_into_buf(ClawtOfficeFrames *self, gsize want, gsize *got, GError **error)
{
    guint old = self->buf->len;
    ReadResult r;

    g_byte_array_set_size(self->buf, (guint)(old + want));
    r = read_some(self, self->buf->data + old, want, got, error);
    g_byte_array_set_size(self->buf, (guint)(old + (r == READ_GOT ? *got : 0)));
    return r;
}

gboolean
clawt_office_frames_dispatch(ClawtOfficeFrames *self, GError **error)
{
    gsize budget = DISPATCH_BUDGET_BYTES;
    guint messages = 0;

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (self->broken) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                            "the connection to the office renderer is broken");
        return FALSE;
    }
    if (!clawt_office_frames_flush(self, error))
        return FALSE;

    while (budget > 0 && messages < DISPATCH_BUDGET_MESSAGES) {
        gsize got = 0;
        ReadResult r;

        if (self->skip_remaining > 0) {
            /* an unknown body goes through a scratch buffer and is never kept */
            guint8 scratch[SKIP_CHUNK];

            r = read_some(self, scratch, MIN((gsize)self->skip_remaining,
                                             sizeof(scratch)), &got, error);
            if (r == READ_GOT) {
                self->skip_remaining -= (guint32)got;
                if (self->skip_remaining == 0)
                    messages++;
            }
        } else if (self->buf->len < CLAWT_OFFICE_FRAMES_HEADER_SIZE) {
            /* the header, and nothing past it */
            r = read_into_buf(self, CLAWT_OFFICE_FRAMES_HEADER_SIZE - self->buf->len,
                              &got, error);
            if (r == READ_GOT && self->buf->len == CLAWT_OFFICE_FRAMES_HEADER_SIZE) {
                if (!check_header(self, error))
                    return FALSE;
                if (self->need == 0 && self->skip_remaining == 0)
                    messages++;     /* an unknown type with an empty body */
            }
        } else if (get_u16(self->buf->data + 6) == CLAWT_OFFICE_FRAMES_MSG_FRAME &&
                   self->buf->len < CLAWT_OFFICE_FRAMES_HEADER_SIZE +
                                    CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE) {
            /* a frame's fixed part, checked before its pixels are read */
            r = read_into_buf(self, CLAWT_OFFICE_FRAMES_HEADER_SIZE +
                                    CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE - self->buf->len,
                              &got, error);
            if (r == READ_GOT && self->buf->len == CLAWT_OFFICE_FRAMES_HEADER_SIZE +
                                                   CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE &&
                !check_frame_head(self, self->buf->data + CLAWT_OFFICE_FRAMES_HEADER_SIZE,
                                  (guint32)(self->need - CLAWT_OFFICE_FRAMES_HEADER_SIZE),
                                  error))
                return FALSE;
        } else if (self->buf->len >= self->need) {
            if (!handle_message(self, error))
                return FALSE;
            messages++;
            continue;
        } else {
            /* the rest of the body, and not a byte of the next message */
            r = read_into_buf(self, self->need - self->buf->len, &got, error);
        }

        if (r == READ_FAILED)
            return FALSE;
        if (r == READ_AGAIN)
            return TRUE;
        budget -= MIN(budget, got);
    }
    return TRUE;
}

gboolean
clawt_office_frames_get_hello(ClawtOfficeFrames *self, guint32 *capabilities,
                              guint *width, guint *height)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);

    if (!self->have_hello)
        return FALSE;
    if (capabilities != NULL)
        *capabilities = self->capabilities;
    if (width != NULL)
        *width = self->hello_width;
    if (height != NULL)
        *height = self->hello_height;
    return TRUE;
}

GBytes *
clawt_office_frames_take_frame(ClawtOfficeFrames *self, guint *width, guint *height,
                               guint *stride, guint64 *frame_number)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), NULL);

    if (self->frame == NULL)
        return NULL;
    if (width != NULL)
        *width = self->frame_width;
    if (height != NULL)
        *height = self->frame_height;
    if (stride != NULL)
        *stride = self->frame_stride;
    if (frame_number != NULL)
        *frame_number = self->frame_number;
    return g_steal_pointer(&self->frame);
}

guint64
clawt_office_frames_get_frames_received(ClawtOfficeFrames *self)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), 0);
    return self->received;
}

/* ── Writing ──────────────────────────────────────────────────────── */

gboolean
clawt_office_frames_has_pending_output(ClawtOfficeFrames *self)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    return self->pending->len > 0;
}

gboolean
clawt_office_frames_flush(ClawtOfficeFrames *self, GError **error)
{
    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);

    while (self->pending->len > 0) {
        gssize n = send(self->fd, self->pending->data, self->pending->len,
                        MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return TRUE;
            return errno_error(self, error, errno, "send");
        }
        g_byte_array_remove_range(self->pending, 0, (guint)n);
    }
    return TRUE;
}

/*
 * Input is small and is never dropped: written now when the socket takes
 * it, queued behind anything already queued otherwise, so the order the
 * application sees is the order things happened.
 */
static gboolean
send_message(ClawtOfficeFrames *self, ClawtOfficeFramesMessage type,
             const guint8 *body, guint32 body_size, GError **error)
{
    guint8 message[CLAWT_OFFICE_FRAMES_HEADER_SIZE + 16];
    gsize total = CLAWT_OFFICE_FRAMES_HEADER_SIZE + body_size;
    gsize written = 0;

    g_assert(body_size <= 16);
    put_u32(message, CLAWT_OFFICE_FRAMES_MAGIC);
    put_u16(message + 4, CLAWT_OFFICE_FRAMES_VERSION);
    put_u16(message + 6, (guint16)type);
    put_u32(message + 8, body_size);
    put_u32(message + 12, 0);
    memcpy(message + CLAWT_OFFICE_FRAMES_HEADER_SIZE, body, body_size);

    if (self->broken) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the connection to the office renderer is broken");
        return FALSE;
    }
    if (!clawt_office_frames_flush(self, error))
        return FALSE;

    while (self->pending->len == 0 && written < total) {
        gssize n = send(self->fd, message + written, total - written,
                        MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return errno_error(self, error, errno, "send");
        }
        written += (gsize)n;
    }
    if (written < total) {
        if (self->pending->len + (total - written) > MAX_PENDING_OUTPUT) {
            self->broken = TRUE;
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                        "the office renderer has not read its last %u bytes of input",
                        self->pending->len);
            return FALSE;
        }
        g_byte_array_append(self->pending, message + written, (guint)(total - written));
    }
    return TRUE;
}

gboolean
clawt_office_frames_send_pointer_move(ClawtOfficeFrames *self, gfloat x, gfloat y,
                                      GError **error)
{
    guint8 body[8];

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    put_f32(body, x);
    put_f32(body + 4, y);
    return send_message(self, CLAWT_OFFICE_FRAMES_MSG_POINTER_MOVE, body, sizeof(body),
                        error);
}

gboolean
clawt_office_frames_send_button(ClawtOfficeFrames *self, guint button, gboolean pressed,
                                gfloat x, gfloat y, GError **error)
{
    guint8 body[16];

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    put_u32(body, button);
    put_u32(body + 4, pressed ? 1u : 0u);
    put_f32(body + 8, x);
    put_f32(body + 12, y);
    return send_message(self, CLAWT_OFFICE_FRAMES_MSG_POINTER_BUTTON, body,
                        sizeof(body), error);
}

gboolean
clawt_office_frames_send_scroll(ClawtOfficeFrames *self, gfloat dx, gfloat dy,
                                GError **error)
{
    guint8 body[8];

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    put_f32(body, dx);
    put_f32(body + 4, dy);
    return send_message(self, CLAWT_OFFICE_FRAMES_MSG_SCROLL, body, sizeof(body), error);
}

gboolean
clawt_office_frames_send_key(ClawtOfficeFrames *self, guint key, gboolean pressed,
                             guint modifiers, gunichar codepoint, GError **error)
{
    guint8 body[16];

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);
    put_u32(body, key);
    put_u32(body + 4, pressed ? 1u : 0u);
    put_u32(body + 8, modifiers);
    put_u32(body + 12, (guint32)codepoint);
    return send_message(self, CLAWT_OFFICE_FRAMES_MSG_KEY, body, sizeof(body), error);
}

gboolean
clawt_office_frames_send_resize(ClawtOfficeFrames *self, guint width, guint height,
                                GError **error)
{
    guint8 body[8];

    g_return_val_if_fail(CLAWT_IS_OFFICE_FRAMES(self), FALSE);

    if (width == 0 || height == 0 || width > CLAWT_OFFICE_FRAMES_MAX_DIMENSION ||
        height > CLAWT_OFFICE_FRAMES_MAX_DIMENSION) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%ux%u is not a size the office renderer draws at", width, height);
        return FALSE;
    }
    put_u32(body, width);
    put_u32(body + 4, height);
    return send_message(self, CLAWT_OFFICE_FRAMES_MSG_RESIZE, body, sizeof(body), error);
}

/* ── Finding and starting the renderer ────────────────────────────── */

#define RENDERER_NAME "clawtilla-office"

gchar *
clawt_office_renderer_resolve(GError **error)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    g_autofree gchar *beside = NULL;
    g_autofree gchar *installed = g_build_filename(CLAWT_DATA_DIR, "..", "..", "bin",
                                                   RENDERER_NAME, NULL);
    gchar *on_path;

    /* beside the running binary: a checkout's build/<type>/ */
    if (exe != NULL) {
        g_autofree gchar *dir = g_path_get_dirname(exe);

        beside = g_build_filename(dir, RENDERER_NAME, NULL);
        if (g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE))
            return g_steal_pointer(&beside);
    }
    /* the install location: <prefix>/bin, from <prefix>/share/clawtilla */
    if (g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE)) {
        g_autofree gchar *canonical = g_canonicalize_filename(installed, NULL);

        return g_steal_pointer(&canonical);
    }
    on_path = g_find_program_in_path(RENDERER_NAME);
    if (on_path != NULL)
        return on_path;

    {
        g_autofree gchar *canonical = g_canonicalize_filename(installed, NULL);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s was not found: looked beside this program (%s), at the "
                    "install location (%s) and on PATH.  Build it with `make OFFICE=1 "
                    "LRG_BACKEND=1`", RENDERER_NAME,
                    beside != NULL ? beside : "unknown: no /proc/self/exe", canonical);
    }
    return NULL;
}

gchar **
clawt_office_renderer_embed_argv(const gchar *binary, gint fd,
                                 ClawtConnection *connection, gboolean saved,
                                 guint width, guint height, GError **error)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);

    g_return_val_if_fail(binary != NULL, NULL);
    g_return_val_if_fail(fd >= 0, NULL);

    g_ptr_array_add(argv, g_strdup(binary));
    g_ptr_array_add(argv, g_strdup_printf("--embed=%d", fd));

    if (connection != NULL && clawt_connection_is_local(connection)) {
        const gchar *socket_path = clawt_connection_get_socket_path(connection);

        /* nothing for the default socket: the renderer finds it the same way */
        if (socket_path != NULL && *socket_path != '\0') {
            g_ptr_array_add(argv, g_strdup("--socket"));
            g_ptr_array_add(argv, g_strdup(socket_path));
        }
    } else if (connection != NULL) {
        /* a remote daemon only by profile name: a token in an argv is public */
        if (!saved) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                        "the office can follow a remote daemon only through a saved "
                        "connection, and \"%s\" is not one: save it under Connections",
                        clawt_connection_get_name(connection));
            return NULL;
        }
        g_ptr_array_add(argv, g_strdup("--profile"));
        g_ptr_array_add(argv, g_strdup(clawt_connection_get_name(connection)));
    }

    g_ptr_array_add(argv, g_strdup("--width"));
    g_ptr_array_add(argv, g_strdup_printf("%u", MAX(width, 1u)));
    g_ptr_array_add(argv, g_strdup("--height"));
    g_ptr_array_add(argv, g_strdup_printf("%u", MAX(height, 1u)));
    g_ptr_array_add(argv, NULL);
    return (gchar **)g_ptr_array_free(g_steal_pointer(&argv), FALSE);
}
