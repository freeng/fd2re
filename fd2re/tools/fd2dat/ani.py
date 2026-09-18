# -*- coding: utf-8 -*-
"""ANI.DAT 流式动画块（fd2dat）

块 = 173B 头（i16 帧数@165，其余未解 — 原样保留）+ 逐帧
[u16 数据长][u16 操作数][4B 保留] + 操作码流（duel.c ani_play 0x20421 /
ani_decode_ops 0x36FF4）：
  0..3 操作 768B palette work（fill / full / 字节RLE / sparse entries）
  4..9 操作 64000B VRAM（fill / full / 字节RLE / sparse px / sparse run /
       sparse literal）；palette work 不写 DAC。

现代格式 = 每帧解码全屏 PNG（观察）+ frames.json（记录/操作码原始字节，
pack 无损重建）。"""
from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import List, Optional, Tuple

VRAM = 64000
PAL = 768
HEADER = 173


def parse(block: bytes) -> dict:
    n = struct.unpack_from("<h", block, 165)[0]
    pos = HEADER
    frames = []
    for _ in range(n):
        size, ops = struct.unpack_from("<HH", block, pos)
        reserved = block[pos + 4:pos + 8]
        payload = block[pos + 8:pos + 8 + size]
        frames.append({"size": size, "ops": ops,
                       "reserved_hex": reserved.hex(),
                       "payload": payload})
        pos += 8 + size
    return {"count": n, "header": block[:HEADER], "frames": frames,
            "consumed": pos}


def _consume_rle(d: bytes, p: int, total: int) -> int:
    """byte-RLE（op2/op6，duel.c case 2/6）：读标签直到累计 total 像素。"""
    o = 0
    while o < total:
        tag = d[p]
        p += 1
        if tag & 0xC0 == 0xC0:
            p += 1
            o += tag & 0x3F
        else:
            o += 1
    return p


def ops_of(frame: dict, buf: Optional[bytearray] = None) -> List[Tuple[int, bytes]]:
    """payload -> [(opcode, 单个操作负载)]（操作码数 = 头部 ops 字段）。
    buf = 持久帧缓冲（原版逐帧复用 64000B malloc，数据短于操作流时读到
    上一帧残留字节）；不传则只读 payload。"""
    d = bytes(buf) if buf is not None else frame["payload"]
    out = []
    p = 0
    for _ in range(frame["ops"]):
        op = d[p]
        p += 1
        start = p
        if op in (0, 4):
            p += 1
        elif op == 1:
            p += PAL
        elif op == 5:
            p += VRAM
        elif op == 2:
            p = _consume_rle(d, p, PAL)
        elif op == 6:
            p = _consume_rle(d, p, VRAM)
        elif op == 3:
            records = d[p]
            p += 1
            for _r in range(records):
                sz = d[p + 1]
                p += 2 + sz
        elif op in (7, 8, 9):
            records = d[p] | (d[p + 1] << 8)
            p += 2
            for _r in range(records):
                p += 2
                if op == 7:
                    p += 1
                else:
                    sz = d[p]
                    p += 2 + sz
        out.append((op, d[start:p]))
    return out


def render(frames: List[dict], palette_default: List[Tuple[int, int, int]]):
    """重放操作序列，返回 [(vram bytes, palette 768B, palette_touched)]。
    VRAM/palette/帧缓冲均为常驻缓冲（原版 ani_play 逐帧复用 malloc，
    短拷贝/越界读落在上一帧残留字节上；越界写按 C 语义出界，此处钳制）。"""
    vram = bytearray(VRAM)
    pal = bytearray(PAL)
    frame_buf = bytearray(VRAM)
    pal_touched = False
    out = []
    for fr in frames:
        pay_full = fr["payload"]
        end = min(len(pay_full), VRAM)
        frame_buf[:end] = pay_full[:end]
        try:
            frame_ops = ops_of(fr, frame_buf)
        except IndexError:
            frame_ops = []            # 操作流越过 64000B：余下放弃
        for op, pay in frame_ops:
          try:
            if op == 0:
                pal[:] = pay[:1] * PAL
                pal_touched = True
            elif op == 1:
                pal[:] = pay[:PAL]
                pal_touched = True
            elif op == 2:
                p = o = 0
                while o != PAL:
                    tag = pay[p]
                    p += 1
                    if tag & 0xC0 == 0xC0:
                        run = tag & 0x3F
                        pal[o:o + run] = pay[p:p + 1] * run
                        p += 1
                        o += run
                    else:
                        pal[o] = tag
                        o += 1
                pal_touched = True
            elif op == 3:
                p = 0
                records = pay[p]
                p += 1
                for _r in range(records):
                    entry, sz = pay[p], pay[p + 1]
                    pal[3 * entry:3 * entry + sz] = pay[p + 2:p + 2 + sz]
                    p += 2 + sz
                pal_touched = True
            elif op == 4:
                vram[:] = pay[:1] * VRAM
            elif op == 5:
                vram[:len(pay)] = pay      # 短拷贝：尾部保留旧帧内容
            elif op == 6:
                p = o = 0
                while o != VRAM and p < len(pay):
                    tag = pay[p]
                    p += 1
                    if tag & 0xC0 == 0xC0:
                        run = tag & 0x3F
                        vram[o:o + run] = pay[p:p + 1] * run
                        p += 1
                        o += run
                    else:
                        vram[o] = tag
                        o += 1
            elif op == 7:
                p = 2
                records = pay[0] | (pay[1] << 8)
                for _ in range(records):
                    off = pay[p] | (pay[p + 1] << 8)
                    if off < VRAM:
                        vram[off] = pay[p + 2]
                    p += 3
            elif op == 8:
                p = 2
                records = pay[0] | (pay[1] << 8)
                for _ in range(records):
                    off = pay[p] | (pay[p + 1] << 8)
                    sz = pay[p + 2]
                    sz = min(sz, VRAM - off) if off < VRAM else 0
                    if sz:
                        vram[off:off + sz] = pay[p + 3:p + 4] * sz
                    p += 4
            elif op == 9:
                p = 2
                records = pay[0] | (pay[1] << 8)
                for _ in range(records):
                    off = pay[p] | (pay[p + 1] << 8)
                    sz = pay[p + 2]
                    sz = min(sz, VRAM - off) if off < VRAM else 0
                    if sz:
                        vram[off:off + sz] = pay[p + 4:p + 4 + sz]
                    p += 4 + sz
          except IndexError:
            pass    # 帧内数据按原样消费；回包走 payload_hex 原始字节
        out.append((bytes(vram), bytes(pal), pal_touched))
    return out


def export_block(block: bytes, outdir: Path, name: str,
                 palette_hint: List[Tuple[int, int, int]]) -> dict:
    from PIL import Image
    parsed = parse(block)
    frames_json = [{"size": fr["size"], "ops": fr["ops"],
                    "reserved_hex": fr["reserved_hex"],
                    "payload_hex": fr["payload"].hex()}
                   for fr in parsed["frames"]]
    doc = {"schema": "fd2dat.ani/1", "header_hex": parsed["header"].hex(),
           "frame_count": parsed["count"], "frames": frames_json}
    (outdir / f"{name}.frames.json").write_text(
        json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    rendered = render(parsed["frames"], palette_hint)
    paths = []
    for i, (vram, pal6, touched) in enumerate(rendered):
        img = Image.new("RGB", (320, 200))
        px = img.load()
        use = pal6 if touched else bytes(
            c for rgb in palette_hint for c in rgb)
        for k in range(VRAM):
            idx = vram[k]
            px[k % 320, k // 320] = ((use[3 * idx] * 255) // 63,
                                     (use[3 * idx + 1] * 255) // 63,
                                     (use[3 * idx + 2] * 255) // 63)
        p = outdir / f"{name}.f{i:04d}.png"
        img.save(str(p))
        paths.append(p.name)
    return {"frames": parsed["count"], "png": paths,
            "consumed_matches": parsed["consumed"] == len(block)}


def pack_block(doc: dict) -> bytes:
    out = bytearray(bytes.fromhex(doc["header_hex"]))
    for fr in doc["frames"]:
        out += struct.pack("<HH", fr["size"], fr["ops"])
        out += bytes.fromhex(fr["reserved_hex"])
        out += bytes.fromhex(fr["payload_hex"])
    return bytes(out)
