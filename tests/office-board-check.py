#!/usr/bin/env python3
# office-board-check.py - Check a usage board screenshot against its report
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of clawtilla.
#
# Called by tests/test-office-board-shots.sh with the frames and reports
# clawtilla-office wrote (--screenshot, --usage-report).  Standard library
# only: PNGs are decoded here with zlib, so the test needs nothing the
# build does not.
#
#   office-board-check.py board ON.png OFF.png ON.json DETAIL
#       the board stayed in its zone: every item inside it, every text no
#       wider than its column, the expected level of detail, and the two
#       frames -- board on, board off -- differ nowhere outside the zone
#   office-board-check.py panel REPORT.json
#       the panel is inside the view, every text fits its column, and
#       every window is listed or counted in "+N more"
#   office-board-check.py inview REPORT.json MODE
#       the view was drawn in MODE (2d or 3d), the board was drawn at
#       some level of detail, and its zone -- in 3D the rectangle the
#       board's corners cover -- and the board itself are inside the view

import json
import struct
import sys
import zlib
from typing import Any

SLACK: float = 1.0          # a pixel, for rounding between float and int rects


class Image(object):
    """An RGBA or RGB 8-bit PNG, decoded."""

    def __init__(self, path: str):
        self.path: str = path
        self.width: int = 0
        self.height: int = 0
        self.channels: int = 0
        self.rows: list[bytes] = []
        self._load()

    def _load(self) -> None:
        data: bytes = open(self.path, "rb").read()
        if data[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"{self.path}: not a PNG")
        pos: int = 8
        idat: bytearray = bytearray()
        depth: int = 0
        colour: int = 0
        while pos < len(data):
            length: int = struct.unpack(">I", data[pos:pos + 4])[0]
            kind: bytes = data[pos + 4:pos + 8]
            body: bytes = data[pos + 8:pos + 8 + length]
            pos += 12 + length
            if kind == b"IHDR":
                self.width, self.height, depth, colour = struct.unpack(">IIBB", body[:10])
            elif kind == b"IDAT":
                idat += body
            elif kind == b"IEND":
                break
        if depth != 8 or colour not in (2, 6):
            raise ValueError(f"{self.path}: only 8-bit RGB/RGBA is read here")
        self.channels = 4 if colour == 6 else 3
        raw: bytes = zlib.decompress(bytes(idat))
        stride: int = self.width * self.channels
        previous: bytearray = bytearray(stride)
        offset: int = 0
        for _ in range(self.height):
            kind_byte: int = raw[offset]
            line: bytearray = bytearray(raw[offset + 1:offset + 1 + stride])
            offset += 1 + stride
            self._unfilter(kind_byte, line, previous)
            self.rows.append(bytes(line))
            previous = line

    def _unfilter(self, kind: int, line: bytearray, prev: bytearray) -> None:
        bpp: int = self.channels
        i: int
        if kind == 0:
            return
        for i in range(len(line)):
            left: int = line[i - bpp] if i >= bpp else 0
            up: int = prev[i]
            corner: int = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p: int = left + up - corner
                pa: int = abs(p - left)
                pb: int = abs(p - up)
                pc: int = abs(p - corner)
                best: int = left if pa <= pb and pa <= pc else (up if pb <= pc else corner)
                line[i] = (line[i] + best) & 0xFF
            else:
                raise ValueError(f"{self.path}: unknown PNG filter {kind}")


def fail(message: str) -> None:
    print(f"office-board-check: {message}", file=sys.stderr)
    sys.exit(1)


def inside(rect: list[float], box: list[float]) -> bool:
    return (rect[0] >= box[0] - SLACK and rect[1] >= box[1] - SLACK and
            rect[0] + rect[2] <= box[0] + box[2] + SLACK and
            rect[1] + rect[3] <= box[1] + box[3] + SLACK)


def check_texts(layout: dict[str, Any], what: str) -> None:
    item: dict[str, Any]
    for item in layout["items"]:
        if item["kind"] == "text" and item["rect"][2] > item["room"] + SLACK:
            fail(f"{what}: \"{item['text']}\" is {item['rect'][2]} wide in a "
                 f"{item['room']} column")


def check_board(on_png: str, off_png: str, report_path: str, detail: str) -> None:
    report: dict[str, Any] = json.load(open(report_path))
    board: dict[str, Any] | None = report.get("board")
    zone: list[float] | None = report.get("zone")
    on: Image = Image(on_png)
    off: Image = Image(off_png)
    item: dict[str, Any]
    lo_x: int = on.width
    lo_y: int = on.height
    hi_x: int = -1
    hi_y: int = -1
    y: int
    x: int

    if board is None or zone is None:
        fail(f"{report_path}: no board was drawn")
    if board["detail"] != detail:
        fail(f"{report_path}: the board shows {board['detail']}, not {detail}")
    if not inside(board["rect"], zone):
        fail(f"{report_path}: the board {board['rect']} leaves its zone {zone}")
    for item in board["items"]:
        if not inside(item["rect"], zone):
            fail(f"{report_path}: {item['kind']} {item['rect']} leaves the zone {zone}")
    check_texts(board, report_path)
    if detail == "gauges" and board["hidden"] + sum(
            1 for i in board["items"] if i["kind"] == "badge") != report["logins"]:
        fail(f"{report_path}: logins were dropped without a \"+N\"")

    # the board changed the frame only inside its zone
    if (on.width, on.height) != (off.width, off.height):
        fail(f"{on_png} and {off_png} differ in size")
    for y in range(on.height):
        if on.rows[y] == off.rows[y]:
            continue
        for x in range(on.width):
            a: int = x * on.channels
            if on.rows[y][a:a + 3] != off.rows[y][a:a + 3]:
                lo_x = min(lo_x, x)
                hi_x = max(hi_x, x)
                lo_y = min(lo_y, y)
                hi_y = max(hi_y, y)
    if hi_x < 0:
        fail(f"{on_png}: the board drew nothing")
    if not inside([lo_x, lo_y, hi_x - lo_x + 1, hi_y - lo_y + 1], zone):
        fail(f"{on_png}: pixels changed at [{lo_x},{lo_y}]-[{hi_x},{hi_y}], outside the "
             f"zone {zone}")


def check_panel(report_path: str) -> None:
    report: dict[str, Any] = json.load(open(report_path))
    panel: dict[str, Any] | None = report.get("panel")
    view: list[float] = [0.0, 0.0, float(report["view"][0]), float(report["view"][1])]
    item: dict[str, Any]
    labels: int = 0

    if panel is None:
        fail(f"{report_path}: no panel was drawn")
    if not inside(panel["rect"], view):
        fail(f"{report_path}: the panel {panel['rect']} leaves the view {view}")
    for item in panel["items"]:
        if not inside(item["rect"], panel["rect"]):
            fail(f"{report_path}: {item['kind']} {item['rect']} leaves the panel")
    check_texts(panel, report_path)
    # one label per window shown; the rest are counted in "+N more"
    labels = sum(1 for i in panel["items"] if i.get("ink") == "label")
    if labels + panel["hidden"] < report["windows"]:
        fail(f"{report_path}: {labels} windows listed and {panel['hidden']} counted, of "
             f"{report['windows']}")
    if panel["hidden"] > 0 and not any(i.get("text") == f"+{panel['hidden']} more"
                                       for i in panel["items"]):
        fail(f"{report_path}: {panel['hidden']} rows hidden with no \"+N more\"")


def check_inview(report_path: str, mode: str) -> None:
    report: dict[str, Any] = json.load(open(report_path))
    board: dict[str, Any] | None = report.get("board")
    zone: list[float] | None = report.get("zone")
    view: list[float] = [0.0, 0.0, float(report["view"][0]), float(report["view"][1])]

    if report.get("mode") != mode:
        fail(f"{report_path}: drawn in {report.get('mode')}, not {mode}")
    if zone is None:
        fail(f"{report_path}: the board's zone is not on screen")
    if not inside(zone, view):
        fail(f"{report_path}: the board's zone {zone} leaves the view {view}")
    if board is None or board["detail"] == "none":
        fail(f"{report_path}: no board was drawn")
    if not inside(board["rect"], view):
        fail(f"{report_path}: the board {board['rect']} leaves the view {view}")


def main() -> None:
    if len(sys.argv) == 6 and sys.argv[1] == "board":
        check_board(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif len(sys.argv) == 3 and sys.argv[1] == "panel":
        check_panel(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "inview":
        check_inview(sys.argv[2], sys.argv[3])
    else:
        fail("usage: office-board-check.py board ON.png OFF.png ON.json DETAIL | "
             "panel REPORT.json | inview REPORT.json MODE")


if __name__ == "__main__":
    main()
