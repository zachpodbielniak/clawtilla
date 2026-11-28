#!/usr/bin/env bash
# test-office-avatar-shots.sh - Two profile pictures, drawn, in the native office
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# A throwaway daemon, two agents who are configured and not started, each
# with a flat PNG.  clawtilla-office --simulate draws them (a stopped
# agent is not in the roster; simulate seats the cast anyway) and writes
# a frame.  The pictures are colours the bodies are not, and the frame
# must contain a solid patch of each.
#
# Needs an OFFICE=1 build, Xvfb and python3.  Skipped, saying why,
# without any of them.  During the office gate those are present, so a
# skip there is a failure of the harness, not a pass.
#
# CLAWT_AVATAR_SHOTS_OUT=DIR keeps the frame.  The exchange copy is
# written when /mnt/clawtilla/exchange/leopard exists.

set -euo pipefail

BUILD="${CLAWT_BUILD_DIR:-build/release}"
HERE="$(CDPATH='' cd -- "$(dirname -- "${0}")" && pwd)"
TOP="$(dirname -- "${HERE}")"
OFFICE="${TOP}/${BUILD}/clawtilla-office"
DAEMON="${TOP}/${BUILD}/clawtillad"
CLI="${TOP}/${BUILD}/clawtilla"

skip () {
	echo "test-office-avatar-shots: skipped (${1})"
	exit 0
}

[[ -x "${OFFICE}" ]] || skip "no clawtilla-office in ${BUILD}: build with OFFICE=1"
[[ -x "${DAEMON}" ]] || skip "no clawtillad in ${BUILD}"
[[ -x "${CLI}" ]] || skip "no clawtilla in ${BUILD}"
command -v Xvfb >/dev/null 2>&1 || skip "Xvfb is not installed"
command -v python3 >/dev/null 2>&1 || skip "python3 is not installed"

RUN=$(mktemp -d "${TMPDIR:-/tmp}/clawt-avatar-shot-XXXXXX")
XVFB_PID=""
DAEMON_PID=""

cleanup () {
	if [[ -n "${DAEMON_PID}" ]]
	then
		kill "${DAEMON_PID}" 2>/dev/null || true
		wait "${DAEMON_PID}" 2>/dev/null || true
	fi
	if [[ -n "${XVFB_PID}" ]]
	then
		kill "${XVFB_PID}" 2>/dev/null || true
		wait "${XVFB_PID}" 2>/dev/null || true
	fi
	rm -rf "${RUN}"
}
trap cleanup EXIT

python3 - <<'PY' "${RUN}"
import struct, sys, zlib
out = sys.argv[1]
def png(rgb, path):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    raw = b"".join(b"\x00" + bytes(rgb) * 4 for _ in range(4))
    ihdr = struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)
png((255, 0, 17), out + "/red.png")
png((0, 255, 17), out + "/green.png")
PY

cat > "${RUN}/config.yaml" <<EOF
daemon:
  tailscale: false
  state_dir: "${RUN}/state"
  socket: "${RUN}/d.sock"
  automation_dir: "${RUN}/pods"
defaults:
  workspace_root: "${RUN}/agents"
  autostart: false
office:
  enabled: true
  world: modern-office
  world_dirs: ["${TOP}/data/office/worlds"]
agents:
  - id: red
    name: Red
    color: "#2244aa"
  - id: green
    name: Green
    color: "#ddcc22"
EOF

"${DAEMON}" -c "${RUN}/config.yaml" --foreground --no-bind > "${RUN}/daemon.log" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 100)
do
	[[ -S "${RUN}/d.sock" ]] && break
	sleep 0.1
done
[[ -S "${RUN}/d.sock" ]] || {
	echo "test-office-avatar-shots: FAIL daemon did not listen"
	tail -n 20 "${RUN}/daemon.log" || true
	exit 1
}

for _ in $(seq 50)
do
	"${CLI}" --socket "${RUN}/d.sock" office who > "${RUN}/who.log" 2>&1 && break
	sleep 0.1
done
"${CLI}" --socket "${RUN}/d.sock" office who > "${RUN}/who.log" 2>&1 \
	|| {
		echo "test-office-avatar-shots: FAIL office who"
		tail -n 20 "${RUN}/who.log" || true
		exit 1
	}

"${CLI}" --socket "${RUN}/d.sock" agent avatar set red "${RUN}/red.png"
"${CLI}" --socket "${RUN}/d.sock" agent avatar set green "${RUN}/green.png"

Xvfb -displayfd 3 -screen 0 4096x4096x24 -nolisten tcp 3> "${RUN}/display" \
	> "${RUN}/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 50)
do
	[[ -s "${RUN}/display" ]] && break
	sleep 0.1
done
[[ -s "${RUN}/display" ]] || {
	echo "test-office-avatar-shots: FAIL Xvfb did not start"
	tail -n 20 "${RUN}/xvfb.log" || true
	exit 1
}
DISPLAY=":$(head -n 1 "${RUN}/display")"
LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY LIBGL_ALWAYS_SOFTWARE

# Fit, not 2x: a 2x view is the middle of the map and the simulated
# seats (the booths) sit just outside it, so the pictures never land
# in the frame.  Fit keeps both of them in view wherever they are
# still walking from the door.  The window is large enough that a
# fitted head is tens of pixels across, so the flat portrait colour
# survives the smooth scale as a solid patch.
if ! "${OFFICE}" --socket "${RUN}/d.sock" --simulate --2d --zoom fit --usage off \
	--width 2200 --height 2000 --frames 450 \
	--screenshot "${RUN}/avatars.png" > "${RUN}/office.log" 2>&1
then
	echo "test-office-avatar-shots: FAIL clawtilla-office"
	sed 's/^/    /' "${RUN}/office.log"
	exit 1
fi

if [[ -n "${CLAWT_AVATAR_SHOTS_OUT:-}" ]]
then
	mkdir -p "${CLAWT_AVATAR_SHOTS_OUT}"
	cp "${RUN}/avatars.png" "${CLAWT_AVATAR_SHOTS_OUT}/avatars.png"
fi
if [[ -d /mnt/clawtilla/exchange/leopard ]]
then
	cp "${RUN}/avatars.png" /mnt/clawtilla/exchange/leopard/office-avatars.png
fi

if ! python3 - "${RUN}/avatars.png" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
data = open(path, "rb").read()
if data[:8] != b"\x89PNG\r\n\x1a\n":
    sys.exit("not a png")
pos = 8
width = height = depth = color = interlace = None
idat = b""
while pos + 8 <= len(data):
    length = struct.unpack(">I", data[pos:pos + 4])[0]
    tag = data[pos + 4:pos + 8]
    chunk = data[pos + 8:pos + 8 + length]
    if tag == b"IHDR":
        width, height, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", chunk[:13])
    elif tag == b"IDAT":
        idat += chunk
    elif tag == b"IEND":
        break
    pos += 12 + length
if interlace != 0 or depth != 8 or color not in (2, 6):
    sys.exit("png is depth %s color %s interlace %s; want 8-bit rgb or rgba" % (depth, color, interlace))
bpp = 4 if color == 6 else 3
stride = width * bpp
raw = zlib.decompress(idat)
need = (stride + 1) * height
if len(raw) < need:
    sys.exit("png rows are short")

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c

prev = bytearray(stride)
red = green = 0
at = 0
for y in range(height):
    filt = raw[at]
    at += 1
    row = bytearray(raw[at:at + stride])
    at += stride
    for x in range(stride):
        left = row[x - bpp] if x >= bpp else 0
        up = prev[x]
        ul = prev[x - bpp] if x >= bpp else 0
        if filt == 0:
            add = 0
        elif filt == 1:
            add = left
        elif filt == 2:
            add = up
        elif filt == 3:
            add = (left + up) // 2
        elif filt == 4:
            add = paeth(left, up, ul)
        else:
            sys.exit("png filter %d" % filt)
        row[x] = (row[x] + add) & 255
    prev = row
    for x in range(width):
        r, g, b = row[x * bpp], row[x * bpp + 1], row[x * bpp + 2]
        if r == 255 and g == 0 and b == 17:
            red += 1
        elif r == 0 and g == 255 and b == 17:
            green += 1
print("red %d green %d" % (red, green))
if red < 100 or green < 100:
    sys.exit("need >=100 of each portrait colour, got red %d green %d" % (red, green))
PY
then
	echo "test-office-avatar-shots: FAIL portrait colours"
	tail -n 40 "${RUN}/office.log" || true
	exit 1
fi

echo "test-office-avatar-shots: ok"
