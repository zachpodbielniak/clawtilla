/*
 * test-office-frames.c - The host side of libregnum's frame protocol
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The application side is faked here, over a socketpair, byte by byte
 * from the specification (libregnum's frame-protocol.org) -- not with
 * libregnum's own sender, which libclawt must not link.  Every read is
 * non-blocking, so nothing here can wait; alarm() is the watchdog in
 * case that ever stops being true.
 */

/* memfd_create() and the seals, which gnu89 alone does not declare */
#define _GNU_SOURCE

#include "clawtilla.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAGIC 0x4D52464Cu

typedef struct {
    gint app;                   /* the fake renderer's end */
    ClawtOfficeFrames *frames;  /* the host's end, owned by the receiver */
} Pair;

static void
pair_open(Pair *pair)
{
    gint sv[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), ==, 0);
    pair->app = sv[0];
    pair->frames = clawt_office_frames_new(sv[1], TRUE);
}

static void
pair_close(Pair *pair)
{
    g_clear_object(&pair->frames);
    if (pair->app >= 0)
        close(pair->app);
    pair->app = -1;
}

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
put_u64(guint8 *p, guint64 v)
{
    put_u32(p, (guint32)(v & 0xFFFFFFFFu));
    put_u32(p + 4, (guint32)(v >> 32));
}

static GByteArray *
message(guint16 type, const guint8 *body, guint32 body_size)
{
    GByteArray *out = g_byte_array_sized_new(16 + body_size);
    guint8 header[16];

    put_u32(header, MAGIC);
    put_u16(header + 4, 1);
    put_u16(header + 6, type);
    put_u32(header + 8, body_size);
    put_u32(header + 12, 0);
    g_byte_array_append(out, header, sizeof(header));
    if (body_size > 0)
        g_byte_array_append(out, body, body_size);
    return out;
}

static void
write_all(gint fd, const guint8 *data, gsize length)
{
    while (length > 0) {
        gssize n = write(fd, data, length);

        g_assert_cmpint(n, >, 0);
        data += n;
        length -= (gsize)n;
    }
}

static void
send_hello(gint fd, guint32 caps, guint32 w, guint32 h)
{
    guint8 body[16] = { 0 };
    g_autoptr(GByteArray) msg = NULL;

    put_u32(body, caps);
    put_u32(body + 4, w);
    put_u32(body + 8, h);
    msg = message(1, body, sizeof(body));
    write_all(fd, msg->data, msg->len);
}

/* The 48-byte fixed part of a frame. */
static void
frame_head(guint8 *b, guint32 w, guint32 h, guint32 stride, guint64 number,
           guint32 flags)
{
    memset(b, 0, 48);
    put_u32(b, w);
    put_u32(b + 4, h);
    put_u32(b + 8, stride);
    put_u32(b + 12, 1);
    put_u64(b + 16, number);
    put_u64(b + 24, 123456);
    put_u32(b + 32, flags);
    put_u32(b + 36, stride * h);
}

/* An inline frame whose every pixel byte is its index, mod 251. */
static GByteArray *
inline_frame(guint32 w, guint32 h, guint32 stride, guint64 number)
{
    g_autofree guint8 *body = g_malloc0(48 + (gsize)stride * h);
    gsize i;

    frame_head(body, w, h, stride, number, 0);
    for (i = 0; i < (gsize)stride * h; i++)
        body[48 + i] = (guint8)(i % 251);
    return message(2, body, 48 + stride * h);
}

static void
assert_pattern(GBytes *pixels, gsize expected_size)
{
    gsize size = 0;
    const guint8 *data = g_bytes_get_data(pixels, &size);
    gsize i;

    g_assert_cmpuint(size, ==, expected_size);
    for (i = 0; i < size; i++)
        g_assert_cmpuint(data[i], ==, (guint)(i % 251));
}

/* The hello is remembered; nothing is a frame yet. */
static void
test_hello(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    guint32 caps = 0;
    guint w = 0;
    guint h = 0;

    pair_open(&pair);
    g_assert_false(clawt_office_frames_get_hello(pair.frames, NULL, NULL, NULL));
    send_hello(pair.app, 1, 640, 360);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_office_frames_get_hello(pair.frames, &caps, &w, &h));
    g_assert_cmpuint(caps, ==, 1);
    g_assert_cmpuint(w, ==, 640);
    g_assert_cmpuint(h, ==, 360);
    g_assert_null(clawt_office_frames_take_frame(pair.frames, NULL, NULL, NULL, NULL));
    pair_close(&pair);
}

/* An inline frame with a stride wider than its rows, after a hello. */
static void
test_inline_frame(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) frame = inline_frame(5, 3, 24, 7);
    g_autoptr(GBytes) pixels = NULL;
    guint w = 0;
    guint h = 0;
    guint stride = 0;
    guint64 number = 0;

    pair_open(&pair);
    send_hello(pair.app, 0, 5, 3);
    write_all(pair.app, frame->data, frame->len);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_no_error(error);

    pixels = clawt_office_frames_take_frame(pair.frames, &w, &h, &stride, &number);
    g_assert_nonnull(pixels);
    g_assert_cmpuint(w, ==, 5);
    g_assert_cmpuint(h, ==, 3);
    g_assert_cmpuint(stride, ==, 24);
    g_assert_cmpuint(number, ==, 7);
    assert_pattern(pixels, 24 * 3);
    g_assert_cmpuint(clawt_office_frames_get_frames_received(pair.frames), ==, 1);

    /* taken once: the next take has nothing new */
    g_assert_null(clawt_office_frames_take_frame(pair.frames, NULL, NULL, NULL, NULL));
    pair_close(&pair);
}

/* Two frames in one dispatch: only the newest is kept. */
static void
test_newest_frame_wins(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) first = inline_frame(2, 2, 8, 1);
    g_autoptr(GByteArray) second = inline_frame(4, 1, 16, 2);
    g_autoptr(GBytes) pixels = NULL;
    guint w = 0;
    guint64 number = 0;

    pair_open(&pair);
    write_all(pair.app, first->data, first->len);
    write_all(pair.app, second->data, second->len);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    pixels = clawt_office_frames_take_frame(pair.frames, &w, NULL, NULL, &number);
    g_assert_nonnull(pixels);
    g_assert_cmpuint(w, ==, 4);
    g_assert_cmpuint(number, ==, 2);
    g_assert_cmpuint(clawt_office_frames_get_frames_received(pair.frames), ==, 2);
    pair_close(&pair);
}

/* A memfd made the way libregnum's sender makes one. */
static gint
sealed_memfd(gsize size, guint seals)
{
    gint fd = memfd_create("test-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    guint8 *map;
    gsize i;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, (off_t)size), ==, 0);
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    g_assert_true(map != MAP_FAILED);
    for (i = 0; i < size; i++)
        map[i] = (guint8)(i % 251);
    munmap(map, size);
    if (seals != 0)
        g_assert_cmpint(fcntl(fd, F_ADD_SEALS, seals), ==, 0);
    return fd;
}

/* One message with a descriptor on its first byte. */
static void
send_with_fd(gint sock, const guint8 *data, gsize length, gint fd)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        gchar          buf[CMSG_SPACE(sizeof(gint))];
    } control;
    struct cmsghdr *cmsg;
    gssize n;

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = (gpointer)data;
    iov.iov_len = length;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(gint));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(gint));
    n = sendmsg(sock, &msg, 0);
    g_assert_cmpint(n, ==, (gssize)length);
}

static void
send_memfd_frame(gint sock, guint32 w, guint32 h, guint32 stride, guint seals)
{
    guint8 body[48];
    g_autoptr(GByteArray) msg = NULL;
    gint fd = sealed_memfd((gsize)stride * h, seals);

    frame_head(body, w, h, stride, 42, 1);
    msg = message(2, body, sizeof(body));
    send_with_fd(sock, msg->data, msg->len, fd);
    close(fd);
}

/* A memfd frame: the pixels are the mapping, and the receiver owns the fd. */
static void
test_memfd_frame(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) pixels = NULL;
    guint w = 0;
    guint stride = 0;
    guint64 number = 0;

    pair_open(&pair);
    send_hello(pair.app, 1, 16, 8);
    send_memfd_frame(pair.app, 16, 8, 64, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE |
                                          F_SEAL_SEAL);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_no_error(error);
    pixels = clawt_office_frames_take_frame(pair.frames, &w, NULL, &stride, &number);
    g_assert_nonnull(pixels);
    g_assert_cmpuint(w, ==, 16);
    g_assert_cmpuint(stride, ==, 64);
    g_assert_cmpuint(number, ==, 42);
    assert_pattern(pixels, 64 * 8);

    /* the pixels outlive the receiver: the mapping goes with the bytes */
    pair_close(&pair);
    assert_pattern(pixels, 64 * 8);
}

/* An unsealed memfd could shrink under the texture; it is refused. */
static void
test_memfd_unsealed(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;

    pair_open(&pair);
    send_memfd_frame(pair.app, 4, 4, 16, 0);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

/* A memfd frame with no descriptor is a protocol error. */
static void
test_memfd_without_fd(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    guint8 body[48];
    g_autoptr(GByteArray) msg = NULL;

    pair_open(&pair);
    frame_head(body, 4, 4, 16, 0, 1);
    msg = message(2, body, sizeof(body));
    write_all(pair.app, msg->data, msg->len);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

/* A frame dribbled in a few bytes at a time arrives whole, once. */
static void
test_split_frame(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) frame = inline_frame(3, 2, 12, 9);
    gsize cuts[] = { 1, 7, 16, 17, 40, 64, 70 };
    gsize done = 0;
    gsize i;
    g_autoptr(GBytes) pixels = NULL;

    pair_open(&pair);
    for (i = 0; i < G_N_ELEMENTS(cuts); i++) {
        write_all(pair.app, frame->data + done, cuts[i] - done);
        done = cuts[i];
        g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
        g_assert_no_error(error);
        g_assert_null(clawt_office_frames_take_frame(pair.frames, NULL, NULL, NULL,
                                                     NULL));
    }
    write_all(pair.app, frame->data + done, frame->len - done);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    pixels = clawt_office_frames_take_frame(pair.frames, NULL, NULL, NULL, NULL);
    g_assert_nonnull(pixels);
    assert_pattern(pixels, 12 * 2);
    pair_close(&pair);
}

static void
test_bad_magic(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) msg = message(1, (const guint8 *)"0123456789abcdef", 16);

    pair_open(&pair);
    msg->data[0] = 'X';
    write_all(pair.app, msg->data, msg->len);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);

    /* broken for good: a stream cannot be resynchronised */
    g_clear_error(&error);
    send_hello(pair.app, 0, 1, 1);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

static void
test_bad_version(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) msg = message(1, (const guint8 *)"0123456789abcdef", 16);

    pair_open(&pair);
    put_u16(msg->data + 4, 2);
    write_all(pair.app, msg->data, msg->len);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

/* Refused on the header alone: the body it announces is never sent, and
 * the receiver must not be waiting for it. */
static void
test_oversized_body(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) msg = message(2, NULL, 0);

    pair_open(&pair);
    put_u32(msg->data + 8, 256u * 1024u * 1024u + 1u);
    write_all(pair.app, msg->data, msg->len);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

/* A frame that does not add up is refused at its fixed part, before the
 * pixels it claims are read. */
static void
test_inconsistent_frame(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    guint8 body[48];
    g_autoptr(GByteArray) msg = NULL;

    pair_open(&pair);
    frame_head(body, 8, 8, 16, 0, 0);    /* stride 16 < 8 x 4 */
    msg = message(2, body, sizeof(body));
    put_u32(msg->data + 8, 48 + 16 * 8);
    write_all(pair.app, msg->data, msg->len);
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
    pair_close(&pair);
}

/* Unknown types are skipped whole, with and without a body. */
static void
test_unknown_type_skipped(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    g_autofree guint8 *junk = g_malloc0(100000);
    g_autoptr(GByteArray) unknown = message(99, junk, 100000);
    g_autoptr(GByteArray) empty = message(77, NULL, 0);
    g_autoptr(GByteArray) frame = inline_frame(2, 2, 8, 5);
    g_autoptr(GBytes) pixels = NULL;
    guint64 number = 0;

    pair_open(&pair);
    write_all(pair.app, empty->data, empty->len);
    send_hello(pair.app, 0, 2, 2);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));

    /* bigger than the socket buffer: written as the receiver drains it */
    {
        gsize done = 0;
        guint rounds = 0;

        g_assert_cmpint(fcntl(pair.app, F_SETFL, O_NONBLOCK), ==, 0);
        while (done < unknown->len) {
            gssize n = write(pair.app, unknown->data + done, unknown->len - done);

            if (n > 0)
                done += (gsize)n;
            g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
            g_assert_cmpuint(++rounds, <, 100000);
        }
    }
    write_all(pair.app, frame->data, frame->len);
    g_assert_true(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_office_frames_get_hello(pair.frames, NULL, NULL, NULL));
    pixels = clawt_office_frames_take_frame(pair.frames, NULL, NULL, NULL, &number);
    g_assert_nonnull(pixels);
    g_assert_cmpuint(number, ==, 5);
    pair_close(&pair);
}

/* The application going away is an error, not a quiet stop. */
static void
test_closed(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;

    pair_open(&pair);
    close(pair.app);
    pair.app = -1;
    g_assert_false(clawt_office_frames_dispatch(pair.frames, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED);
    pair_close(&pair);
}

static void
read_exact(gint fd, guint8 *out, gsize length)
{
    gsize done = 0;

    while (done < length) {
        gssize n = recv(fd, out + done, length - done, MSG_DONTWAIT);

        g_assert_cmpint(n, >, 0);
        done += (gsize)n;
    }
}

/* Every input message, byte for byte as the specification lays it out. */
static void
test_input_bytes(void)
{
    Pair pair;
    g_autoptr(GError) error = NULL;
    guint8 got[32];
    static const guint8 move[] = {
        'L', 'F', 'R', 'M', 1, 0, 16, 0, 8, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0xC0, 0x3F,     /* 1.5f */
        0x00, 0x00, 0x20, 0xC1      /* -10.0f */
    };
    static const guint8 button[] = {
        'L', 'F', 'R', 'M', 1, 0, 17, 0, 16, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0,                 /* right */
        1, 0, 0, 0,                 /* pressed */
        0x00, 0x00, 0x80, 0x3F,     /* 1.0f */
        0x00, 0x00, 0x00, 0x40      /* 2.0f */
    };
    static const guint8 scroll[] = {
        'L', 'F', 'R', 'M', 1, 0, 18, 0, 8, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0x00, 0x00,     /* 0.0f */
        0x00, 0x00, 0x80, 0xBF      /* -1.0f */
    };
    static const guint8 key[] = {
        'L', 'F', 'R', 'M', 1, 0, 19, 0, 16, 0, 0, 0, 0, 0, 0, 0,
        0x41, 0, 0, 0,              /* KEY_A, 65 */
        0, 0, 0, 0,                 /* released */
        3, 0, 0, 0,                 /* shift | control */
        0xE9, 0, 0, 0               /* U+00E9 */
    };
    static const guint8 resize[] = {
        'L', 'F', 'R', 'M', 1, 0, 20, 0, 8, 0, 0, 0, 0, 0, 0, 0,
        0x80, 0x07, 0, 0,           /* 1920 */
        0x38, 0x04, 0, 0            /* 1080 */
    };

    pair_open(&pair);

    g_assert_true(clawt_office_frames_send_pointer_move(pair.frames, 1.5f, -10.0f, &error));
    read_exact(pair.app, got, sizeof(move));
    g_assert_cmpmem(got, sizeof(move), move, sizeof(move));

    g_assert_true(clawt_office_frames_send_button(pair.frames, 1, TRUE, 1.0f, 2.0f, &error));
    read_exact(pair.app, got, sizeof(button));
    g_assert_cmpmem(got, sizeof(button), button, sizeof(button));

    g_assert_true(clawt_office_frames_send_scroll(pair.frames, 0.0f, -1.0f, &error));
    read_exact(pair.app, got, sizeof(scroll));
    g_assert_cmpmem(got, sizeof(scroll), scroll, sizeof(scroll));

    g_assert_true(clawt_office_frames_send_key(pair.frames, 65, FALSE, 3, 0xE9, &error));
    read_exact(pair.app, got, sizeof(key));
    g_assert_cmpmem(got, sizeof(key), key, sizeof(key));

    /*
     * What the GTK Office page forwards for a movement key and a
     * right-drag: W down and W up (raylib 87, from clawt_office_key_from_gdk),
     * then button 1.  A widget test would need a display; these are the
     * bytes that call produces.  The button's identity is the u32 already
     * covered above, and again here so a right-drag cannot be sent as left.
     */
    {
        static const guint8 w_down[] = {
            'L', 'F', 'R', 'M', 1, 0, 19, 0, 16, 0, 0, 0, 0, 0, 0, 0,
            87, 0, 0, 0,                /* KEY_W */
            1, 0, 0, 0,                 /* pressed */
            0, 0, 0, 0,
            0, 0, 0, 0
        };
        static const guint8 w_up[] = {
            'L', 'F', 'R', 'M', 1, 0, 19, 0, 16, 0, 0, 0, 0, 0, 0, 0,
            87, 0, 0, 0,
            0, 0, 0, 0,                 /* released */
            0, 0, 0, 0,
            0, 0, 0, 0
        };

        g_assert_cmpuint(clawt_office_key_from_gdk(0x0077), ==, 87);
        g_assert_cmpuint(clawt_office_key_from_gdk(0xff0d), ==, 0);
        g_assert_true(clawt_office_frames_send_key(pair.frames, 87, TRUE, 0, 0, &error));
        read_exact(pair.app, got, sizeof(w_down));
        g_assert_cmpmem(got, sizeof(w_down), w_down, sizeof(w_down));
        g_assert_true(clawt_office_frames_send_key(pair.frames, 87, FALSE, 0, 0, &error));
        read_exact(pair.app, got, sizeof(w_up));
        g_assert_cmpmem(got, sizeof(w_up), w_up, sizeof(w_up));
        g_assert_true(clawt_office_frames_send_button(pair.frames, 1, TRUE, 4.0f, 5.0f, &error));
        g_assert_true(clawt_office_frames_send_pointer_move(pair.frames, 12.0f, 8.0f, &error));
        /* button identity 1, then a move: the drag.  Consume both. */
        read_exact(pair.app, got, sizeof(button));
        g_assert_cmpuint(got[16], ==, 1);
        read_exact(pair.app, got, sizeof(move));
        g_assert_cmpuint(got[6], ==, 16);
    }

    g_assert_true(clawt_office_frames_send_resize(pair.frames, 1920, 1080, &error));
    read_exact(pair.app, got, sizeof(resize));
    g_assert_cmpmem(got, sizeof(resize), resize, sizeof(resize));
    g_assert_no_error(error);
    g_assert_false(clawt_office_frames_has_pending_output(pair.frames));

    /* a size the application would ignore is refused, not sent */
    g_assert_false(clawt_office_frames_send_resize(pair.frames, 0, 10, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_cmpint(recv(pair.app, got, sizeof(got), MSG_DONTWAIT), <, 0);
    pair_close(&pair);
}

/* The command line: the socket for a local daemon, a profile for a
 * remote one, and never a token. */
static void
test_embed_argv(void)
{
    g_autoptr(GError) error = NULL;
    ClawtConnection *local = clawt_connection_new_local("Local", "/run/x/daemon.sock");
    ClawtConnection *plain = clawt_connection_new_local("Local", NULL);
    ClawtConnection *remote = clawt_connection_new_remote("workstation", "100.64.0.9",
                                                          7413, "sekrit-token");
    g_auto(GStrv) a = NULL;
    g_auto(GStrv) b = NULL;
    g_auto(GStrv) c = NULL;
    g_auto(GStrv) d = NULL;
    g_autofree gchar *joined = NULL;

    a = clawt_office_renderer_embed_argv("/bin/clawtilla-office", 3, local, FALSE,
                                         800, 450, &error);
    g_assert_no_error(error);
    joined = g_strjoinv(" ", a);
    g_assert_cmpstr(joined, ==, "/bin/clawtilla-office --embed=3 --socket "
                                "/run/x/daemon.sock --width 800 --height 450");
    g_clear_pointer(&joined, g_free);

    b = clawt_office_renderer_embed_argv("o", 4, plain, FALSE, 0, 0, &error);
    joined = g_strjoinv(" ", b);
    g_assert_cmpstr(joined, ==, "o --embed=4 --width 1 --height 1");
    g_clear_pointer(&joined, g_free);

    c = clawt_office_renderer_embed_argv("o", 5, remote, TRUE, 10, 20, &error);
    joined = g_strjoinv(" ", c);
    g_assert_cmpstr(joined, ==, "o --embed=5 --profile workstation --width 10 --height 20");
    g_assert_null(strstr(joined, "sekrit"));
    g_clear_pointer(&joined, g_free);

    /* typed and not saved: a token cannot ride an argv, so no command at all */
    d = clawt_office_renderer_embed_argv("o", 5, remote, FALSE, 10, 20, &error);
    g_assert_null(d);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    clawt_connection_free(local);
    clawt_connection_free(plain);
    clawt_connection_free(remote);
}

/* Every word of a standalone command line, joined; for asserting on. */
static gchar *
window_line(ClawtConnection *connection, gboolean saved, ClawtOfficeViewMode mode,
            gint token_fd, GError **error)
{
    g_auto(GStrv) argv = clawt_office_renderer_window_argv("o", connection, saved, mode,
                                                           token_fd, error);

    return argv != NULL ? g_strjoinv(" ", argv) : NULL;
}

/*
 * The standalone window's command line: never --embed, always --2d or
 * --3d, the client's own connection named outright, and never a token --
 * a saved remote by profile, a typed one by --host and a token fd.
 */
static void
test_window_argv(void)
{
    g_autoptr(GError) error = NULL;
    ClawtConnection *local = clawt_connection_new_local("Local", "/run/x/daemon.sock");
    ClawtConnection *plain = clawt_connection_new_local("Local", NULL);
    ClawtConnection *remote = clawt_connection_new_remote("workstation", "100.64.0.9",
                                                          7413, "sekrit-token");
    ClawtConnection *bare = clawt_connection_new_remote("100.64.0.9", "100.64.0.9",
                                                        7413, NULL);
    ClawtConnection *tls = clawt_connection_new_remote("vault", "vault.example", 443,
                                                       "sekrit-token");
    g_autofree gchar *default_socket = clawt_client_default_socket_path();
    g_autofree gchar *want = NULL;
    gchar *line;

    clawt_connection_set_tls(tls, TRUE, FALSE);

    /* local: the socket, and the mode always spelled out */
    line = window_line(local, FALSE, CLAWT_OFFICE_VIEW_2D, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(line, ==, "o --2d --socket /run/x/daemon.sock");
    g_free(line);

    /* the default socket too: unsaid, the renderer would follow another client */
    line = window_line(plain, FALSE, CLAWT_OFFICE_VIEW_3D, -1, &error);
    g_assert_no_error(error);
    want = g_strdup_printf("o --3d --socket %s", default_socket);
    g_assert_cmpstr(line, ==, want);
    g_free(line);

    /* saved: the profile, which keeps the token; the fd is not used */
    line = window_line(remote, TRUE, CLAWT_OFFICE_VIEW_3D, 3, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(line, ==, "o --3d --profile workstation");
    g_free(line);

    /* typed: --host, and the token from an fd */
    line = window_line(remote, FALSE, CLAWT_OFFICE_VIEW_2D, 3, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(line, ==,
                    "o --2d --host 100.64.0.9 --daemon-port 7413 --token-file /dev/fd/3");
    g_assert_null(strstr(line, "sekrit"));
    g_assert_null(strstr(line, "--token "));
    g_free(line);

    /* typed with a token and nowhere to read it from: no command at all */
    line = window_line(remote, FALSE, CLAWT_OFFICE_VIEW_2D, -1, &error);
    g_assert_null(line);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_null(strstr(error->message, "sekrit"));
    g_clear_error(&error);

    /* typed without a token: --host alone */
    line = window_line(bare, FALSE, CLAWT_OFFICE_VIEW_3D, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(line, ==, "o --3d --host 100.64.0.9 --daemon-port 7413");
    g_free(line);

    /* TLS lives only in a saved profile */
    line = window_line(tls, FALSE, CLAWT_OFFICE_VIEW_2D, 3, &error);
    g_assert_null(line);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);

    /* nothing to hand over: the renderer chooses */
    line = window_line(NULL, FALSE, CLAWT_OFFICE_VIEW_2D, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(line, ==, "o --2d");
    g_free(line);

    clawt_connection_free(local);
    clawt_connection_free(plain);
    clawt_connection_free(remote);
    clawt_connection_free(bare);
    clawt_connection_free(tls);
}

/* What the launch says it handed over: where, how the token went, never the token. */
static void
test_window_describe(void)
{
    ClawtConnection *local = clawt_connection_new_local("Local", "/run/x/daemon.sock");
    ClawtConnection *remote = clawt_connection_new_remote("workstation", "100.64.0.9",
                                                          7413, "sekrit-token");
    g_autofree gchar *a = clawt_office_renderer_describe_connection(local, FALSE);
    g_autofree gchar *b = clawt_office_renderer_describe_connection(remote, TRUE);
    g_autofree gchar *c = clawt_office_renderer_describe_connection(remote, FALSE);

    g_assert_cmpstr(a, ==, "the local daemon (/run/x/daemon.sock)");
    g_assert_nonnull(strstr(b, "saved connection"));
    g_assert_nonnull(strstr(b, "workstation"));
    g_assert_nonnull(strstr(c, "100.64.0.9:7413"));
    g_assert_nonnull(strstr(c, "through a pipe"));
    g_assert_null(strstr(b, "sekrit"));
    g_assert_null(strstr(c, "sekrit"));

    clawt_connection_free(local);
    clawt_connection_free(remote);
}

/*
 * The whole launch, against a stand-in renderer on PATH that writes
 * down its argv and whatever its --token-file held: the token arrives,
 * through the pipe, and is nowhere in the command line.
 */
static void
test_open_window_token_pipe(void)
{
    static const gchar fake[] =
        "#!/bin/sh\n"
        "out=\"$CLAWT_TEST_OFFICE_OUT\"\n"
        ": > \"$out/argv.part\"\n"
        "prev=\n"
        "for a in \"$@\"; do\n"
        "    printf '%s\\n' \"$a\" >> \"$out/argv.part\"\n"
        "    if [ \"$prev\" = --token-file ]; then cat \"$a\" > \"$out/token\"; fi\n"
        "    prev=\"$a\"\n"
        "done\n"
        "mv \"$out/argv.part\" \"$out/argv\"\n";
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-office-open-XXXXXX", &error);
    g_autofree gchar *script = NULL;
    g_autofree gchar *found = NULL;
    g_autofree gchar *old_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *argv_path = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *argv_text = NULL;
    g_autofree gchar *token_text = NULL;
    g_autofree gchar *handed_over = NULL;
    g_autofree gchar *want = NULL;
    g_autoptr(GSubprocess) child = NULL;
    ClawtConnection *remote = clawt_connection_new_remote("100.64.0.9", "100.64.0.9",
                                                          7413, "sekrit-token");

    g_assert_no_error(error);
    script = g_build_filename(dir, "clawtilla-office", NULL);
    g_assert_true(g_file_set_contents(script, fake, -1, &error));
    g_assert_cmpint(g_chmod(script, 0700), ==, 0);
    /* in front, not alone: the stand-in needs cat and mv from the rest */
    {
        g_autofree gchar *path = g_strdup_printf("%s:%s", dir,
                                                 old_path != NULL ? old_path : "/usr/bin:/bin");

        g_setenv("PATH", path, TRUE);
    }
    g_setenv("CLAWT_TEST_OFFICE_OUT", dir, TRUE);

    /* a real renderer beside the tests or installed would be found first */
    found = clawt_office_renderer_resolve(NULL);
    if (g_strcmp0(found, script) != 0) {
        g_setenv("PATH", old_path != NULL ? old_path : "", TRUE);
        clawt_connection_free(remote);
        g_test_skip("a clawtilla-office is installed beside the tests or in the prefix");
        return;
    }

    child = clawt_office_renderer_open_window(remote, FALSE, CLAWT_OFFICE_VIEW_3D,
                                              &handed_over, &error);
    g_setenv("PATH", old_path != NULL ? old_path : "", TRUE);
    g_assert_no_error(error);
    g_assert_nonnull(child);
    g_assert_nonnull(strstr(handed_over, "100.64.0.9:7413"));
    g_assert_null(strstr(handed_over, "sekrit"));

    /* the test may wait; the clients never do */
    g_assert_true(g_subprocess_wait_check(child, NULL, &error));
    g_assert_no_error(error);

    argv_path = g_build_filename(dir, "argv", NULL);
    token_path = g_build_filename(dir, "token", NULL);
    g_assert_true(g_file_get_contents(argv_path, &argv_text, NULL, &error));
    g_assert_true(g_file_get_contents(token_path, &token_text, NULL, &error));
    /* "$@": everything after the program, which resolve() already checked */
    want = g_strdup_printf("--3d\n--host\n100.64.0.9\n--daemon-port\n7413\n"
                           "--token-file\n/dev/fd/%d\n", CLAWT_OFFICE_RENDERER_TOKEN_FD);
    g_assert_cmpstr(argv_text, ==, want);
    g_assert_null(strstr(argv_text, "sekrit"));
    g_assert_cmpstr(token_text, ==, "sekrit-token");

    g_unlink(argv_path);
    g_unlink(token_path);
    g_unlink(script);
    g_rmdir(dir);
    clawt_connection_free(remote);
}

/* Missing, the error names the three places and how to build it. */
static void
test_resolve_missing(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *old_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *found = NULL;

    g_setenv("PATH", "/nonexistent-clawt-office-test", TRUE);
    found = clawt_office_renderer_resolve(&error);
    if (old_path != NULL)
        g_setenv("PATH", old_path, TRUE);
    if (found != NULL) {
        g_test_skip("a clawtilla-office is installed beside the tests or in the prefix");
        return;
    }
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "beside this program"));
    g_assert_nonnull(strstr(error->message, "install location"));
    g_assert_nonnull(strstr(error->message, "PATH"));
    g_assert_nonnull(strstr(error->message, "make OFFICE=1 LRG_BACKEND=1"));
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* nothing here blocks; if that ever changes, fail rather than hang */
    alarm(60);

    g_test_add_func("/office-frames/hello", test_hello);
    g_test_add_func("/office-frames/inline-frame", test_inline_frame);
    g_test_add_func("/office-frames/newest-frame-wins", test_newest_frame_wins);
    g_test_add_func("/office-frames/memfd-frame", test_memfd_frame);
    g_test_add_func("/office-frames/memfd-unsealed", test_memfd_unsealed);
    g_test_add_func("/office-frames/memfd-without-fd", test_memfd_without_fd);
    g_test_add_func("/office-frames/split-frame", test_split_frame);
    g_test_add_func("/office-frames/bad-magic", test_bad_magic);
    g_test_add_func("/office-frames/bad-version", test_bad_version);
    g_test_add_func("/office-frames/oversized-body", test_oversized_body);
    g_test_add_func("/office-frames/inconsistent-frame", test_inconsistent_frame);
    g_test_add_func("/office-frames/unknown-type-skipped", test_unknown_type_skipped);
    g_test_add_func("/office-frames/closed", test_closed);
    g_test_add_func("/office-frames/input-bytes", test_input_bytes);
    g_test_add_func("/office-frames/embed-argv", test_embed_argv);
    g_test_add_func("/office-frames/window-argv", test_window_argv);
    g_test_add_func("/office-frames/window-describe", test_window_describe);
    g_test_add_func("/office-frames/open-window-token-pipe", test_open_window_token_pipe);
    g_test_add_func("/office-frames/resolve-missing", test_resolve_missing);

    return g_test_run();
}
