# -*- coding: utf-8 -*-
"""块级 现代格式 导出/回包（fd2dat）

每个 (DAT, 块) 按其种类导出为现代格式文件族；pack 按导出产生的
manifest+JSON 无损重建原始字节。像素类帧在导出时做「像素→RLE 重编码」
自检：与原始流逐字节一致的帧标记 reencoded=true（纯像素回包），不一致
的帧自动落 raw 十六进制兜底（仍无损，但不是纯像素重建）。

现代格式选型（种类 → 方案）：
  palette768    → .palette.json + .pal (GIMP/JASC)
  image         → PNG 图集 + JSON（BG/TAI/TITLE/FDOTHER 单帧图）
  image_pkg     → 子块 PNG 图集族 + JSON（FDOTHER blk7 ≡ TITLE.DAT）
  sfx_pkg       → .wav (8bit unsigned PCM 11025Hz) + JSON
  lmi1          → TexturePacker JSON 图集 + PNG（帧格式逐帧探测：
                  rle8 / rle4 / raw / lut256）
  wh_anim       → TexturePacker JSON 图集 + PNG（帧头 x/y →
                  spriteSourceSize，mode 元数据保留）
  dato          → TexturePacker JSON 图集 + PNG（4 帧口型，byte-RLE 不透明）
  tiles/attrs   → tileset PNG + JSON / 属性数组 JSON（FDSHAP 偶/奇对）
  raw_frames    → 图集 + JSON（FDOTHER blk2 径向菜单 24×20 字面帧）
  font          → 点阵 PNG 网格 + JSON（FDOTHER blk4，32B/字形 1bpp）
  field_map     → JSON（cells 4B 原始 + tile/flags 便捷字段）
  battle_ctx    → JSON（§6.9 布局：头/宝箱/26B 记录/尾，raw_hex 兜底）
  deploy        → JSON（u16 三元组条目）
  text          → .strings.json + .txt（FDTXT token 结构化）
  music         → .mid (SMF) + .events.json（XMIDI 无损）
  anim          → 逐帧 PNG + .frames.json（ANI.DAT 操作码无损）
"""
from __future__ import annotations

import json
import struct
import wave
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from PIL import Image

from . import ani, codec, container, midfmt, pngio, txtfmt
from .codec import rd_i16, rd_u16, rd_u32

CANVAS_W, CANVAS_H = 320, 200
SFX_RATE = 11025


class Ctx:
    """导出/回包共享上下文：默认调色板 + 统计。"""

    def __init__(self):
        self.default_palette: List[Tuple[int, int, int]] = [(0, 0, 0)] * 256
        self.stats = {"frames_total": 0, "frames_pixel_rt": 0,
                      "frames_raw_fallback": 0}

    def note_frame(self, reencoded: bool):
        self.stats["frames_total"] += 1
        self.stats["frames_pixel_rt" if reencoded else
                   "frames_raw_fallback"] += 1


# ---------------------------------------------------------------- helpers

def _selftest(codec_name: str, pixels, w, h, orig: bytes,
              strategies: Tuple[str, ...] = ("front63", "balanced")) -> Tuple[Optional[bytes], Optional[str], Optional[str]]:
    """像素重编码并与原始流对拍；返回 (bytes, codec, strategy)。"""
    cands = [codec_name] if codec_name in ("rle4", "rle4_linear", "sprite24") \
        else [codec_name]
    for c in cands:
        if c in ("rle4", "rle4_linear", "sprite24"):
            data = codec.reencode(c, pixels, w, h)
            if data == orig:
                return data, c, None
        else:  # rle8：两种长游程切分策略
            for st in strategies:
                data = codec.reencode(c, pixels, w, h, st)
                if data == orig:
                    return data, c, st
    return None, None, None


def _emit_atlas(outdir: Path, name: str, items: List[dict],
                palette, render_transparent_zero: bool,
                canvas: Tuple[int, int], fd2_meta: dict,
                tp: bool) -> dict:
    """items: [{pixels,w,h,src_x,src_y,duration,fd2}] → 图集 PNG(+idx) + JSON。
    返回 atlas 描述（写进块 JSON，pack 用 boxes 直接裁剪）。"""
    sizes = [(it["w"], it["h"]) for it in items]
    aw, ah, pos = pngio.pack_atlas(sizes)
    pngio.write_atlas_pngs(outdir / f"{name}.png",
                           [it["pixels"] for it in items], sizes, pos,
                           (aw, ah), palette, render_transparent_zero)
    boxes = [[pos[i][0], pos[i][1], sizes[i][0], sizes[i][1]]
             for i in range(len(items))]
    if tp:
        frames = []
        for i, it in enumerate(items):
            frames.append({
                "name": it.get("name", f"f{i}"),
                "x": pos[i][0], "y": pos[i][1], "w": it["w"], "h": it["h"],
                "src_x": it.get("src_x", 0), "src_y": it.get("src_y", 0),
                "canvas_w": canvas[0], "canvas_h": canvas[1],
                "duration": it.get("duration", 100.0),
                "fd2": it.get("fd2"),
            })
        pngio.write_tp_json(outdir / f"{name}.tp.json", f"{name}.png",
                            frames, (aw, ah), fd2_meta)
    return {"image": f"{name}.png", "boxes": boxes, "size": [aw, ah]}


def _crop_atlas(outdir: Path, atlas: dict) -> List[Tuple[codec.Pixels, int, int]]:
    im = _atlas_img(outdir, atlas)
    out = []
    for x, y, w, h in atlas["boxes"]:
        out.append((pngio.crop_pixels(im, (x, y, w, h)), w, h))
    return out


def _write_json(outdir: Path, name: str, doc: dict) -> None:
    (outdir / name).write_text(json.dumps(doc, ensure_ascii=False, indent=1)
                               + "\n", encoding="utf-8")


def _read_json(outdir: Path, name: str) -> dict:
    return json.loads((outdir / name).read_text(encoding="utf-8"))


# ---------------------------------------------------------------- kinds

def export_palette(outdir, name, b, ctx):
    pal = pngio.load_palette_768(b)
    _write_json(outdir, f"{name}.palette.json", {
        "schema": "fd2dat.palette/1", "vga6": True,
        "palette": [list(c) for c in pal]})
    lines = ["JASC-PAL", "0100", "256"]
    lines += [f"{pngio.scale6(r)} {pngio.scale6(g)} {pngio.scale6(b2)}"
              for r, g, b2 in pal]
    (outdir / f"{name}.pal").write_text("\r\n".join(lines) + "\r\n",
                                        encoding="utf-8")
    return {"kind": "palette", "files": [f"{name}.palette.json", f"{name}.pal"]}


def pack_palette(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.palette.json")
    out = bytearray()
    for c in doc["palette"]:
        out += bytes(c)
    return bytes(out)


def export_image(outdir, name, b, ctx, tp=False):
    w, h = struct.unpack_from("<HH", b)
    stream = b[4:]
    px, _ = codec.decode_rle4_rows(stream, w, h)
    if px is not None:
        codec_name, cand = "rle4_rows", "rle4"
    else:
        # 字节 RLE 变体（FDOTHER blk10/blk15 等：分类器 _valid_image
        # 已识别，导出器曾只试 rle4_rows -> 落 unknown 与 manifest 失配）
        px, _ = codec.decode_byte_rle(stream, w, h)
        codec_name, cand = "rle8", "rle8"
    if px is None:
        return _export_unknown(outdir, name, b, ctx)
    data, c, st = _selftest(cand, px, w, h, stream)
    ctx.note_frame(data == stream)
    atlas = _emit_atlas(outdir, name, [{
        "pixels": px, "w": w, "h": h, "name": "f0"}],
        ctx.default_palette, False, (w, h),
        {"kind": "image", "codec": codec_name}, tp)
    entry = {"kind": "image", "w": w, "h": h, "atlas": atlas,
             "codec": codec_name, "byte_equal": data == stream}
    if data != stream:
        # 编码器无法字节复现原流（贪婪/DP 与原版切分差异）：保留原流，
        # pack 原样回包——与其他 kind 的 raw_hex 兜底一致
        entry["raw_hex"] = stream.hex()
    elif cand == "rle8":
        entry["strategy"] = st
    _write_json(outdir, f"{name}.json", entry)
    return {"kind": "image", "files": [f"{name}.png", f"{name}.json"]}


def pack_image(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    if "raw_hex" in doc:
        stream = bytes.fromhex(doc["raw_hex"])
    elif doc.get("codec") == "rle8":
        (px, w, h), = _crop_atlas(outdir, doc["atlas"])
        stream = codec.encode_byte_rle(px, doc.get("strategy", "front63"))
    else:
        (px, w, h), = _crop_atlas(outdir, doc["atlas"])
        stream = codec.encode_rle4_rows(px, w, h)
    return struct.pack("<HH", doc["w"], doc["h"]) + stream


def export_image_raw(outdir, name, b, ctx):
    w, h = struct.unpack_from("<HH", b)
    px, _ = codec.decode_raw(b[4:], w, h, False)
    if px is None:
        return _export_unknown(outdir, name, b, ctx)
    ctx.note_frame(True)
    atlas = _emit_atlas(outdir, name, [{
        "pixels": px, "w": w, "h": h, "name": "f0"}],
        ctx.default_palette, False, (w, h),
        {"kind": "image_raw", "codec": "raw"}, tp=False)
    _write_json(outdir, f"{name}.json", {
        "kind": "image_raw", "w": w, "h": h, "atlas": atlas,
        "codec": "raw"})
    return {"kind": "image_raw", "files": [f"{name}.json", f"{name}.png",
                                           f"{name}.idx.png"]}


def pack_image_raw(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    (px, w, h), = _crop_atlas(outdir, doc["atlas"])
    return struct.pack("<HH", doc["w"], doc["h"]) + bytes(v for v, _a in px)


def export_sprite_pkg(outdir, name, b, ctx):
    w, h, cnt = struct.unpack_from("<HHH", b)
    # 表项数由首偏移反推（cnt 项无哨兵 / cnt+1 项带哨兵）
    first = struct.unpack_from("<I", b, 6)[0]
    n = (first - 6) // 4
    offs = [struct.unpack_from("<I", b, 6 + 4 * i)[0] for i in range(n)]
    items, metas = [], []
    for i in range(cnt):
        end = offs[i + 1] if i + 1 < n else len(b)
        body = b[offs[i]:end]
        # 两变体按【重编码 byte_equal】判别：op11 跳过（透明版 0x1AE86）/
        # op11 写 0x49（不透明版 sub_4E22A 原型，FDOTHER blk96 实测）
        meta = {"index": i, "codec": "sprite24", "w": w, "h": h}
        px_t, _ = codec.decode_sprite24(body, True, w, h)
        px_o, _ = codec.decode_sprite24(body, False, w, h)
        chosen = None
        for variant, px in (("transparent", px_t), ("opaque49", px_o)):
            if px is None:
                continue
            again = codec.encode_sprite24(px, w, h)
            if again == body:
                chosen = (variant, px, True)
                break
        if chosen is None and px_t is not None:
            chosen = ("transparent", px_t, False)
        if chosen is None and px_o is not None:
            chosen = ("opaque49", px_o, False)
        if chosen is None:
            ctx.note_frame(False)
            meta["raw_hex"] = body.hex()
            metas.append(meta)
            continue
        variant, px, beq = chosen
        meta["variant"] = variant
        meta["byte_equal"] = beq
        meta["atlas_index"] = len(items)
        if not beq:
            # 贪婪 sprite24 编码器对个别形态有损：非字节等价帧保留原流
            #（pack 走原字节；图集像素仍来自原流解码，供观察/预览）
            meta["raw_hex"] = body.hex()
        ctx.note_frame(beq)
        items.append({"pixels": px, "w": w, "h": h,
                      "name": f"f{i}", "fd2": {"variant": variant}})
        metas.append(meta)
    entry = {"kind": "sprite_pkg", "icon_w": w, "icon_h": h,
             "frame_count": cnt, "table_entries": n, "frames": metas}
    if items:
        entry["atlas"] = _emit_atlas(outdir, name, items,
                                     ctx.default_palette, True, (w, h),
                                     {"kind": "sprite_pkg", "block": name},
                                     tp=True)
    _write_json(outdir, f"{name}.json", entry)
    files = [f"{name}.json"]
    if items:
        files += [f"{name}.png", f"{name}.idx.png", f"{name}.tp.json"]
    return {"kind": "sprite_pkg", "files": files, "frames": cnt}


def pack_sprite_pkg_generic(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    w, h = doc["icon_w"], doc["icon_h"]
    bodies = []
    for m in doc["frames"]:
        if "raw_hex" in m:
            bodies.append(bytes.fromhex(m["raw_hex"]))
            continue
        px, _w, _h = crops[m["atlas_index"]]
        bodies.append(codec.encode_sprite24(px, w, h))
    n = doc["table_entries"]
    out = bytearray(struct.pack("<HHH", w, h, doc["frame_count"]))
    pos = 6 + 4 * n
    offs = []
    for bd in bodies:
        offs.append(pos)
        pos += len(bd)
    while len(offs) < n:            # 含哨兵的块目录多 1 项 = 总长
        offs.append(pos)
    for o in offs:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def export_image_pkg(outdir, name, b, ctx):
    starts = container.parse_starts(b)
    files = []
    subs = []
    n = len(starts) - 1
    for i in range(n):
        sub = b[starts[i]:starts[i + 1]]
        sname = f"{name}.s{i}"
        r = export_image(outdir, sname, sub, ctx)
        files += r["files"]
        subs.append(sname)
    _write_json(outdir, f"{name}.json", {"kind": "image_pkg", "subs": subs})
    files.append(f"{name}.json")
    return {"kind": "image_pkg", "files": files}


def pack_image_pkg(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    return container.build_container(
        [pack_image(outdir, s, None) for s in doc["subs"]])


def export_sfx_pkg(outdir, name, b, ctx):
    starts = container.parse_starts(b)
    files = []
    n = len(starts) - 1
    lens = []
    for i in range(n):
        pcm = b[starts[i]:starts[i + 1]]
        lens.append(len(pcm))
        wp = outdir / f"{name}.e{i}.wav"
        wp.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(wp), "wb") as wv:
            wv.setnchannels(1)
            wv.setsampwidth(1)          # 8bit unsigned（AIL DIG 定案）
            wv.setframerate(SFX_RATE)
            wv.writeframes(pcm)
        files.append(wp.name)
    _write_json(outdir, f"{name}.json", {
        "kind": "sfx_pkg", "rate": SFX_RATE, "bits": 8, "channels": 1,
        "entries": n, "lengths": lens})
    files.append(f"{name}.json")
    return {"kind": "sfx_pkg", "files": files, "entries": n}


def pack_sfx_pkg(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    blocks = []
    for i in range(doc["entries"]):
        with wave.open(str(outdir / f"{name}.e{i}.wav"), "rb") as wv:
            assert (wv.getframerate(), wv.getsampwidth(), wv.getnchannels()) == \
                (SFX_RATE, 1, 1), f"{name}.e{i}: WAV 参数被改动"
            blocks.append(wv.readframes(wv.getnframes()))
    return container.build_container(blocks)


def export_lmi1(outdir, name, b, ctx):
    n = struct.unpack_from("<H", b, 4)[0]
    offs = [rd_u32(b, 6 + 4 * i) for i in range(n + 1)]
    items, metas = [], []
    lut_frames = 0
    for i in range(n):
        body = b[offs[i]:offs[i + 1]]
        meta = {"index": i}
        if len(body) == 256:
            # LUT 重映射表（FDOTHER blk3，tile4_stamp_lut 消费）
            meta["codec"] = "lut256"
            meta["table"] = list(body)
            lut_frames += 1
            metas.append(meta)
            continue
        w, h = rd_i16(body, 0), rd_i16(body, 2)
        stream = body[4:]
        meta["w"], meta["h"] = w, h
        # LMI1 帧体多格式混合（§6.5/§6.6）：逐帧选【重编码 byte_equal】的
        # 视图，保证图集像素 = 该 codec 的真实解码；无 byte_equal 视图的
        # 帧落 raw 兜底（字节保留、不进图集——解码视图歧义不猜测）。
        chosen = None
        px8, _c8 = codec.decode_byte_rle(stream, w, h)
        if px8 is not None:
            data, _c, st = _selftest("rle8", px8, w, h, stream)
            if data == stream:
                chosen = ("rle8", px8, st, True)
        if chosen is None:
            px4, _c4 = codec.decode_rle4_rows(stream, w, h)
            if px4 is not None:
                data, _c, _s = _selftest("rle4", px4, w, h, stream)
                if data == stream:
                    chosen = ("rle4", px4, None, True)
        if chosen is None and len(stream) == w * h:
            pxr, _cr = codec.decode_raw(stream, w, h, True)
            chosen = ("raw_t", pxr, None, True)
        if chosen is None:
            ctx.note_frame(False)
            meta["codec"] = "raw_fallback"
            meta["raw_hex"] = body.hex()
            metas.append(meta)
            continue
        cname, px, st, beq = chosen
        meta["codec"] = cname
        meta["byte_equal"] = beq
        if cname == "rle8":
            meta["strategy"] = "front63"
        ctx.note_frame(beq)
        meta["atlas_index"] = len(items)
        items.append({"pixels": px, "w": w, "h": h,
                      "name": f"f{i}", "fd2": {"codec": cname}})
        metas.append(meta)
    entry = {"kind": "lmi1", "frame_count": n, "frames": metas}
    if items:
        atlas = _emit_atlas(outdir, name, items, ctx.default_palette,
                            True, (CANVAS_W, CANVAS_H),
                            {"kind": "lmi1", "block": name}, tp=True)
        entry["atlas"] = atlas
    _write_json(outdir, f"{name}.json", entry)
    files = [f"{name}.json"]
    if items:
        files += [f"{name}.png", f"{name}.idx.png", f"{name}.tp.json"]
    return {"kind": "lmi1", "files": files, "frames": n,
            "lut_frames": lut_frames}


def pack_lmi1(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    bodies: List[bytes] = [None] * doc["frame_count"]  # type: ignore
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    for m in doc["frames"]:
        i = m["index"]
        if m["codec"] == "lut256":
            bodies[i] = bytes(m["table"])
        elif "raw_hex" in m:
            bodies[i] = bytes.fromhex(m["raw_hex"])
        else:
            px, w, h = crops[m["atlas_index"]]
            if m["codec"] == "rle8":
                stream = codec.encode_byte_rle(px, m.get("strategy", "front63"))
            elif m["codec"] == "rle4":
                stream = codec.encode_rle4_rows(px, w, h)
            else:
                stream = bytes(v for v, _a in px)
            bodies[i] = struct.pack("<hh", m["w"], m["h"]) + stream
    out = bytearray(b"LMI1" + struct.pack("<H", doc["frame_count"]))
    pos = 6 + 4 * (doc["frame_count"] + 1)
    offs = []
    for bd in bodies:
        offs.append(pos)
        pos += len(bd)
    offs.append(pos)
    for o in offs:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def export_wh_anim(outdir, name, b, ctx):
    # 帧表动画包：头 [u8 count][1..8 原样保留（flag/played/param，语义
    # 挂号——fx_scene_play 读 tpose[0]=count、ending 读 ftab[2]）] +
    # u32 off[count] @8 + WH 帧（流@13，byte4..8 原样进 meta）。
    n = b[0]
    head_hex = b[1:8].hex()
    offs = [rd_u32(b, 8 + 4 * i) for i in range(n)]
    items, metas = [], []
    for i in range(n):
        end = offs[i + 1] if i + 1 < n else len(b)
        f = b[offs[i]:end]
        x, y = rd_i16(f, 0), rd_i16(f, 2)
        tail_hex = f[4:9].hex()        # byte4..8 原样（A2/b/mode/0，挂号：
                                       # unit_anim_play 注释 fr[5]=音效
                                       # id、fr[6]=延时 tick）
        mode = f[7]
        w, h = rd_u16(f, 9), rd_u16(f, 11)
        stream = f[13:]
        px, _ = codec.decode_rle4_rows(stream, w, h)
        meta = {"index": i, "x": x, "y": y, "tail_hex": tail_hex,
                "mode": mode, "w": w, "h": h}
        if px is None:
            ctx.note_frame(False)
            meta["codec"] = "raw_fallback"
            meta["raw_hex"] = f.hex()
        else:
            data, _c, _s = _selftest("rle4", px, w, h, stream)
            meta["byte_equal"] = data == stream
            ctx.note_frame(data == stream)
            meta["codec"] = "rle4"
            meta["atlas_index"] = len(items)
            items.append({"pixels": px, "w": w, "h": h, "src_x": x,
                          "src_y": y, "name": f"f{i}",
                          "fd2": {"mode": mode, "x": x, "y": y}})
        metas.append(meta)
    entry = {"kind": "wh_anim", "frame_count": n, "head_hex": head_hex,
             "frames": metas, "canvas": [CANVAS_W, CANVAS_H]}
    if items:
        entry["atlas"] = _emit_atlas(
            outdir, name, items, ctx.default_palette, False,
            (CANVAS_W, CANVAS_H),
            {"kind": "wh_anim", "block": name}, tp=True)
    _write_json(outdir, f"{name}.json", entry)
    files = [f"{name}.json"]
    if items:
        files += [f"{name}.png", f"{name}.idx.png", f"{name}.tp.json"]
    return {"kind": "wh_anim", "files": files, "frames": n}


def pack_wh_anim(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    bodies = []
    for m in doc["frames"]:
        if "raw_hex" in m:
            bodies.append(bytes.fromhex(m["raw_hex"]))
            continue
        px, w, h = crops[m["atlas_index"]]
        stream = codec.encode_rle4_rows(px, w, h)
        # 帧头 13B：x2 y2 + byte4..8 原样（tail_hex）+ w2 h2
        head = struct.pack("<hh", m["x"], m["y"]) \
            + bytes.fromhex(m.get("tail_hex", "00" * 5)) \
            + struct.pack("<HH", m["w"], m["h"])
        bodies.append(head + stream)
    # 块头 8B：[u8 count][u8 flag][u16 played][u32 param]（head_hex 原样）
    out = bytearray(bytes([doc["frame_count"]])
                    + bytes.fromhex(doc.get("head_hex", "00" * 7)))
    pos = 8 + 4 * doc["frame_count"]
    offs = []
    for bd in bodies:
        offs.append(pos)
        pos += len(bd)
    for o in offs:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def export_anim_empty(outdir, name, b, ctx):
    _write_json(outdir, f"{name}.json", {"kind": "anim_empty",
                                         "hex": b.hex()})
    return {"kind": "anim_empty", "files": [f"{name}.json"]}


def pack_anim_empty(outdir, name, entry):
    return bytes.fromhex(_read_json(outdir, f"{name}.json")["hex"])


def export_dato(outdir, name, b, ctx):
    offs = [rd_u32(b, 4 * i) for i in range(4)]
    items, metas = [], []
    for i in range(4):
        end = offs[i + 1] if i < 3 else len(b)
        w, h = struct.unpack_from("<HH", b, offs[i])
        stream = b[offs[i] + 4:end]
        px, _ = codec.decode_byte_rle(stream, w, h)
        meta = {"index": i, "w": w, "h": h}
        if px is None:
            ctx.note_frame(False)
            meta["codec"] = "raw_fallback"
            meta["raw_hex"] = b[offs[i]:end].hex()
        else:
            data, _c, _st = _selftest("rle8", px, w, h, stream)
            meta["codec"] = "rle8"
            meta["byte_equal"] = data == stream
            meta["strategy"] = "front63"
            meta["atlas_index"] = len(items)
            ctx.note_frame(data == stream)
            items.append({"pixels": px, "w": w, "h": h, "name": f"mouth{i}",
                          "fd2": {"mouth_phase": i}})
        metas.append(meta)
    entry = {"kind": "dato", "frames": metas,
             "note": "byte-RLE 不透明（0x4EBFF），帧序 = 口型动画 0..3"}
    if items:
        entry["atlas"] = _emit_atlas(outdir, name, items,
                                     ctx.default_palette, False,
                                     (80, 80), {"kind": "dato", "block": name},
                                     tp=True)
    _write_json(outdir, f"{name}.json", entry)
    files = [f"{name}.json"]
    if items:
        files += [f"{name}.png", f"{name}.idx.png", f"{name}.tp.json"]
    return {"kind": "dato", "files": files, "frames": 4}


def pack_dato(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    bodies = []
    for m in doc["frames"]:
        if "raw_hex" in m:
            bodies.append(bytes.fromhex(m["raw_hex"]))
            continue
        px, w, h = crops[m["atlas_index"]]
        stream = codec.encode_byte_rle(px, m.get("strategy", "front63"))
        bodies.append(struct.pack("<HH", m["w"], m["h"]) + stream)
    out = bytearray()
    pos = 16
    offs = []
    for bd in bodies:
        offs.append(pos)
        pos += len(bd)
    for o in offs:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def export_tiles(outdir, name, b, ctx):
    w, h, n = struct.unpack_from("<HHH", b)
    offs = [rd_u32(b, 6 + 4 * i) for i in range(n)]
    cols = 16
    rows = (n + cols - 1) // cols
    idx = Image.new("RGBA", (cols * w, rows * h), (0, 0, 0, 0))
    pix = idx.load()
    metas = []
    for i in range(n):
        end = offs[i + 1] if i < n - 1 else len(b)
        stream = b[offs[i]:end]
        px, _ = codec.decode_rle4_linear(stream, w, h)
        meta = {"index": i}
        if px is None:
            ctx.note_frame(False)
            meta["codec"] = "raw_fallback"
            meta["raw_hex"] = stream.hex()
        else:
            data, _c, _s = _selftest("rle4_linear", px, w, h, stream)
            meta["codec"] = "rle4_linear"
            meta["byte_equal"] = data == stream
            ctx.note_frame(data == stream)
            tx, ty = (i % cols) * w, (i // cols) * h
            for yy in range(h):
                for xx in range(w):
                    v, a = px[yy * w + xx]
                    pix[tx + xx, ty + yy] = (v, v, v, a)
        metas.append(meta)
    idx.save(str(outdir / f"{name}.idx.png"))
    # 观看版（调色板）
    view = Image.new("RGBA", idx.size, (0, 0, 0, 0))
    vp = view.load()
    for yy in range(idx.size[1]):
        for xx in range(idx.size[0]):
            v, _g, _b, a = pix[xx, yy]
            if a:
                c = ctx.default_palette[v]
                vp[xx, yy] = (pngio.scale6(c[0]), pngio.scale6(c[1]),
                              pngio.scale6(c[2]), 255)
    view.save(str(outdir / f"{name}.png"))
    entry = {"kind": "tiles", "tile_w": w, "tile_h": h, "tile_count": n,
             "cols": cols, "tiles": metas,
             "atlas": {"image": f"{name}.idx.png"}}
    _write_json(outdir, f"{name}.json", entry)
    return {"kind": "tiles", "files": [f"{name}.json", f"{name}.png",
                                       f"{name}.idx.png"], "tiles": n}


def pack_tiles(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    w, h, n = doc["tile_w"], doc["tile_h"], doc["tile_count"]
    im = Image.open(str(outdir / doc["atlas"]["image"])).convert("RGBA")
    pix = im.load()
    cols = doc["cols"]
    streams = []
    for i in range(n):
        m = doc["tiles"][i]
        if "raw_hex" in m:
            streams.append(bytes.fromhex(m["raw_hex"]))
            continue
        tx, ty = (i % cols) * w, (i // cols) * h
        px = []
        for yy in range(h):
            for xx in range(w):
                r, _g, _b, a = pix[tx + xx, ty + yy]
                px.append((r, 255 if a else 0))
        streams.append(codec.encode_rle4_linear(px, w, h))
    out = bytearray(struct.pack("<HHH", w, h, n))
    pos = 6 + 4 * n
    offs = []
    for s in streams:
        offs.append(pos)
        pos += len(s)
    for o in offs:
        out += struct.pack("<I", o)
    for s in streams:
        out += s
    return bytes(out)


def export_attrs(outdir, name, b, ctx):
    n = len(b) // 4
    pairs = [list(struct.unpack_from("<HH", b, 4 * i)) for i in range(n)]
    _write_json(outdir, f"{name}.json", {
        "kind": "attrs", "count": n,
        "note": "4B/tile 地形属性数组（g_shape_map，tile_lookup 消费）",
        "pairs": pairs})
    return {"kind": "attrs", "files": [f"{name}.json"], "count": n}


def pack_attrs(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    out = bytearray()
    for p in doc["pairs"]:
        out += struct.pack("<HH", *p)
    return bytes(out)


def export_raw_frames(outdir, name, b, ctx):
    first = rd_u32(b, 0)
    n = first // 4
    offs = [rd_u32(b, 4 * i) for i in range(n)]
    items, metas = [], []
    for i in range(n):
        end = offs[i + 1] if i < n - 1 else len(b)
        w, h = struct.unpack_from("<HH", b, offs[i])
        stream = b[offs[i] + 4:end]
        px, _ = codec.decode_raw(stream, w, h, False)
        meta = {"index": i, "w": w, "h": h, "codec": "raw_literal"}
        ctx.note_frame(px is not None)
        if px is not None:
            meta["atlas_index"] = len(items)
            items.append({"pixels": px, "w": w, "h": h, "name": f"f{i}"})
        metas.append(meta)
    entry = {"kind": "raw_frames", "frame_count": n, "frames": metas,
             "note": "目录在块首 u32[]，字面帧（battle.c blk2_frame 径向图标）"}
    if items:
        entry["atlas"] = _emit_atlas(outdir, name, items,
                                     ctx.default_palette, False,
                                     (CANVAS_W, CANVAS_H),
                                     {"kind": "raw_frames"}, tp=True)
    _write_json(outdir, f"{name}.json", entry)
    files = [f"{name}.json"]
    if items:
        files += [f"{name}.png", f"{name}.idx.png", f"{name}.tp.json"]
    return {"kind": "raw_frames", "files": files, "frames": n}


def pack_raw_frames(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    bodies = []
    for m in doc["frames"]:
        if "atlas_index" in m:
            px, w, h = crops[m["atlas_index"]]
            bodies.append(struct.pack("<HH", m["w"], m["h"])
                          + bytes(v for v, _a in px))
        else:
            bodies.append(struct.pack("<HH", m["w"], m["h"]))
    out = bytearray()
    pos = 4 * doc["frame_count"]
    offs = []
    for bd in bodies:
        offs.append(pos)
        pos += len(bd)
    for o in offs:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def export_font(outdir, name, b, ctx):
    total = len(b) // 32
    cols = 48
    rows = (total + cols - 1) // cols
    idx = Image.new("L", (cols * 16, rows * 16), 0)
    pix = idx.load()
    for g in range(total):
        gx, gy = (g % cols) * 16, (g // cols) * 16
        for r in range(16):
            # 0x4ED7A：行 u16 经字节交换后 MSB 先行 = 内存序 B0=左半 8px、
            # B1=右半 8px。此前直接测 rd_u16 高位 -> 每行左右半互换，全
            # 字库字形花屏（预览文字异常根因，2026-09-09）
            for half in range(2):
                v = b[32 * g + 2 * r + half]
                for k in range(8):
                    if v & (0x80 >> k):
                        pix[gx + half * 8 + k, gy + r] = 255
    idx.save(str(outdir / f"{name}.idx.png"))
    view = Image.new("RGBA", idx.size, (0, 0, 0, 255))
    vp = view.load()
    for yy in range(idx.size[1]):
        for xx in range(idx.size[0]):
            if pix[xx, yy]:
                vp[xx, yy] = (255, 255, 255, 255)
    view.save(str(outdir / f"{name}.png"))
    _write_json(outdir, f"{name}.json", {
        "kind": "font", "glyph_count": total, "glyph_w": 16, "glyph_h": 16,
        "bits": "row u16 MSB-first, 32B/glyph（glyph_blit 0x4EDC2）",
        "cols": cols, "glyph10": "space"})
    return {"kind": "font", "files": [f"{name}.json", f"{name}.png",
                                      f"{name}.idx.png"], "glyphs": total}


def pack_font(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    im = Image.open(str(outdir / f"{name}.idx.png")).convert("L")
    pix = im.load()
    cols = doc["cols"]
    out = bytearray()
    for g in range(doc["glyph_count"]):
        gx, gy = (g % cols) * 16, (g // cols) * 16
        for r in range(16):
            # 内存字节序回包（同 export_font：B0=左半、B1=右半）
            for half in range(2):
                v = 0
                for k in range(8):
                    if pix[gx + half * 8 + k, gy + r]:
                        v |= 0x80 >> k
                out.append(v)
    return bytes(out)


def export_field_map(outdir, name, b, ctx):
    w, h = struct.unpack_from("<hh", b)
    cells = []
    tiles = []
    flags = []
    for i in range(w * h):
        c0, c1, c2, c3 = b[4 + 4 * i:8 + 4 * i]
        cells.append([c0, c1, c2, c3])
        tiles.append(c0 | ((c1 & 3) << 8))
        flags.append(c2 & 0x1F)
    _write_json(outdir, f"{name}.json", {
        "kind": "field_map", "w": w, "h": h,
        "note": "cell = 块基址+4+4i；tile=cell[0]|(cell[1]&3)<<8，"
                "flags=cell[2]&0x1F，cell[3]=洪泛候选（运行时改写）",
        "cells": cells, "tiles": tiles, "flags": flags})
    return {"kind": "field_map", "files": [f"{name}.json"],
            "size": f"{w}x{h}"}


def pack_field_map(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    out = bytearray(struct.pack("<hh", doc["w"], doc["h"]))
    for c in doc["cells"]:
        out += bytes(c)
    return bytes(out)


def export_battle_ctx(outdir, name, b, ctx):
    f = (len(b) - 131) // 26
    head = b[0:3]
    mid = b[3:83]
    treasures = b[83:125]
    pad = b[125]
    records = []
    pos = 126
    for i in range(f):
        rec = b[pos:pos + 26]
        pos += 26
        records.append({
            "raw_hex": rec.hex(),
            "group": rec[0],
            "field5": rec[5],
            "icon": rec[6],
            "level": rec[9],
            "items": list(rec[10:18]),
            "spells": list(rec[18:22]),
        })
    tail = b[pos:pos + 5]
    _write_json(outdir, f"{name}.json", {
        "kind": "battle_ctx", "shape": head[0], "player_slots": head[1],
        "enemy_count": head[2], "record_count": f,
        "mid_80b_hex": mid.hex(),
        "note": "mid 80B = 16×3B 回合事件 + 16×2B 地块事件（§13.15 勘误）；"
                "treasures[t] = 3B（类型/参数），索引=tile id",
        "treasures": [{"type": treasures[3 * i],
                       "param": rd_u16(treasures, 3 * i + 1)}
                      for i in range(14)],
        "pad": pad, "records": records, "tail_hex": tail.hex()})
    return {"kind": "battle_ctx", "files": [f"{name}.json"], "records": f}


def pack_battle_ctx(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    out = bytearray(bytes([doc["shape"], doc["player_slots"],
                           doc["enemy_count"]]))
    out += bytes.fromhex(doc["mid_80b_hex"])
    for t in doc["treasures"]:
        out += bytes([t["type"]]) + struct.pack("<H", t["param"])
    out.append(doc["pad"])
    for r in doc["records"]:
        out += bytes.fromhex(r["raw_hex"])
    out += bytes.fromhex(doc["tail_hex"])
    return bytes(out)


def export_deploy(outdir, name, b, ctx):
    head = rd_u16(b, 0)
    n = (len(b) - 2) // 6
    entries = [list(struct.unpack_from("<HHH", b, 2 + 6 * i))
               for i in range(n)]
    _write_json(outdir, f"{name}.json", {
        "kind": "deploy", "head": head, "entry_count": n,
        "note": "u16 头 = P+E；敌方条（group,x,icon）在前、玩家条（x,y,?）"
                "在后（§6.9）",
        "entries": entries})
    return {"kind": "deploy", "files": [f"{name}.json"], "entries": n}


def pack_deploy(outdir, name, entry):
    doc = _read_json(outdir, f"{name}.json")
    out = bytearray(struct.pack("<H", doc["head"]))
    for e in doc["entries"]:
        out += struct.pack("<HHH", *e)
    return bytes(out)


def export_text(outdir, name, b, ctx):
    r = txtfmt.export_block(b, outdir / f"{name}.strings.json",
                            outdir / f"{name}.txt")
    return {"kind": "text", "files": [f"{name}.strings.json", f"{name}.txt"],
            "strings": r["strings"]}


def pack_text(outdir, name, entry):
    return txtfmt.pack_block(_read_json(outdir, f"{name}.strings.json"))


def export_music_block(outdir, name, b, ctx):
    r = midfmt.export_music(b, outdir / f"{name}.events.json",
                            outdir / f"{name}.mid")
    return {"kind": "music", "files": [f"{name}.events.json", f"{name}.mid"],
            "events": r["events"], "timb": r["timb"]}


def pack_music_block(outdir, name, entry):
    return midfmt.build_xmidi(_read_json(outdir, f"{name}.events.json"))


def export_music_placeholder(outdir, name, b, ctx):
    _write_json(outdir, f"{name}.json", {
        "kind": "music_placeholder",
        "note": "3 字节占位轨（music_play assign 拒绝即静默，§7.1）",
        "hex": b.hex()})
    return {"kind": "music_placeholder", "files": [f"{name}.json"]}


def pack_music_placeholder(outdir, name, entry):
    return bytes.fromhex(_read_json(outdir, f"{name}.json")["hex"])


def export_anim_block(outdir, name, b, ctx):
    r = ani.export_block(b, outdir, name, ctx.default_palette)
    return {"kind": "anim", "files": [f"{name}.frames.json"] + r["png"],
            "frames": r["frames"],
            "consumed_matches": r["consumed_matches"]}


def pack_anim_block(outdir, name, entry):
    return ani.pack_block(_read_json(outdir, f"{name}.frames.json"))


def export_sprite24_pkg(outdir, name, b, ctx):
    raise NotImplementedError("FDICON.B24 走 dat_export.export_fdicon")


def pack_sprite24_pkg(outdir, name, entry):
    doc = _read_json(outdir, "FDICON.json")
    crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
    w, h = doc["icon_w"], doc["icon_h"]
    count = doc["frame_count"]
    bodies = []
    for m in doc["frames"]:
        if "raw_hex" in m:
            bodies.append(bytes.fromhex(m["raw_hex"]))
            continue
        px, _w, _h = crops[m["atlas_index"]]
        bodies.append(codec.encode_sprite24(px, w, h))
    out = bytearray(struct.pack("<HHH", w, h, count))
    # 注：帧级 variant 已由编码器从空洞位记号还原（opaque49 的 op11 记
    # (0x49,0)，透明版其余空洞 idx 也记 0，见 codec.encode_sprite24）
    pos = 6 + 4 * (count + 1)
    table = []
    for bd in bodies:
        table.append(pos)
        pos += len(bd)
    table.append(pos)
    for o in table:
        out += struct.pack("<I", o)
    for bd in bodies:
        out += bd
    return bytes(out)


def _export_unknown(outdir, name, b, ctx):
    (outdir / f"{name}.unknown.bin").write_bytes(b)
    _write_json(outdir, f"{name}.json", {
        "kind": "unknown", "size": len(b),
        "note": "结构未识别，保留原字节；pack 原样回包"})
    return {"kind": "unknown", "files": [f"{name}.json",
                                         f"{name}.unknown.bin"]}


def pack_unknown(outdir, name, entry):
    return (outdir / f"{name}.unknown.bin").read_bytes()


# -------------------------------------------------------------- 调度

EXPORTERS = {
    "palette": export_palette, "image": export_image,
    "image_pkg": export_image_pkg, "sfx_pkg": export_sfx_pkg,
    "lmi1": export_lmi1, "wh_anim": export_wh_anim,
    "anim_empty": export_anim_empty, "dato": export_dato,
    "tiles": export_tiles, "attrs": export_attrs,
    "raw_frames": export_raw_frames, "font": export_font,
    "image_raw": export_image_raw, "sprite_pkg": export_sprite_pkg,
    "field_map": export_field_map, "battle_ctx": export_battle_ctx,
    "deploy": export_deploy, "text": export_text,
    "music": export_music_block, "music_placeholder": export_music_placeholder,
    "anim": export_anim_block, "unknown": _export_unknown,
}

PACKERS = {
    "palette": pack_palette, "image": pack_image,
    "image_pkg": pack_image_pkg, "sfx_pkg": pack_sfx_pkg,
    "lmi1": pack_lmi1, "wh_anim": pack_wh_anim,
    "anim_empty": pack_anim_empty, "dato": pack_dato,
    "tiles": pack_tiles, "attrs": pack_attrs,
    "raw_frames": pack_raw_frames, "font": pack_font,
    "image_raw": pack_image_raw, "sprite_pkg": pack_sprite_pkg_generic,
    "field_map": pack_field_map, "battle_ctx": pack_battle_ctx,
    "deploy": pack_deploy, "text": pack_text,
    "music": pack_music_block,
    "music_placeholder": pack_music_placeholder,
    "anim": pack_anim_block, "unknown": pack_unknown,
    "sprite24_pkg": pack_sprite24_pkg,
}


def fix_kind(dat: str, block_id: int, kind: str, blocks: List[bytes]) -> str:
    """上下文修正：FDSHAP 奇块在偶块 tile 数吻合时 = 属性数组。"""
    if (dat == "FDSHAP.DAT" and kind == "unknown" and block_id % 2 == 1
            and block_id > 0 and len(blocks[block_id]) % 4 == 0
            and container._valid_tiles(blocks[block_id - 1])):
        w, _h, n = struct.unpack_from("<HHH", blocks[block_id - 1])
        if len(blocks[block_id]) >= 4 * n:
            # 属性表可大于 tile 数（tile_lookup 边界守卫允许；blk1=300×4 vs n=288）
            return "attrs"
    return kind


# ------------------------------------------------------- pack 像素校验

_PNG_CACHE: list = []          # [(abs_path, loaded PIL image)]，LRU 8


def _atlas_img(outdir: Path, atlas: dict):
    path = (outdir / atlas["image"]).resolve()
    # pack/校验一律取索引真值图（R=色号、A=流级透明）；观看图 R 是
    # 调色板色——曾因此把调色板色当索引重编码（自洽校验掩盖）。
    idx = path.with_name(path.name + ".idx.png")
    if idx.exists():
        path = idx
    for p, im in _PNG_CACHE:
        if p == path:
            return im
    im = Image.open(str(path)).convert("RGBA")
    _PNG_CACHE.append((path, im))
    if len(_PNG_CACHE) > 8:
        _PNG_CACHE.pop(0)
    return im


def _png_px(outdir: Path, atlas: dict, i: int) -> Pixels:
    im = _atlas_img(outdir, atlas)
    x, y, w, h = atlas["boxes"][i]
    return pngio.crop_pixels(im, (x, y, w, h))


def _verify_px_arrays(outdir: Path, atlas: dict, i: int):
    """校验专用：真值图 crop 的 (索引 u8 数组, 不透明 bool 数组)，
    免 tuple 列表往返（wh_anim 千帧级块的对拍热点）。"""
    import numpy as np
    im = _atlas_img(outdir, atlas)
    x, y, w, h = atlas["boxes"][i]
    sub = np.asarray(im)[y:y + h, x:x + w]
    return sub[:, :, 0].ravel(), (sub[:, :, 3] != 0).ravel()


def _ab_key_eq(gi, ga, wi, wa) -> bool:
    """decode_*_ab 孪生 (索引, alpha) vs 真值数组对拍，语义同 _px_key：
    两侧 alpha 键须相等、不透明位索引须相等，空洞位索引不参与。"""
    import numpy as np
    g = np.frombuffer(bytes(gi), np.uint8)
    ga_ = np.frombuffer(bytes(ga), np.uint8) != 0
    return bool(np.array_equal(ga_, wa)
                and np.array_equal(np.where(ga_, g, 0),
                                   np.where(wa, wi, 0)))


def _px_key(px: Pixels):
    """渲染相关像素键：透明位索引不参与（alpha=0 不写目标面，
    空洞索引仅是解码器实现细节）。numpy 快路径（64000px 帧 genexp
    版每调用 ~50ms，BG 56 块 ×2 键即可观）。"""
    if not px:
        return (b"", b"")
    try:
        import numpy as np
    except ImportError:
        return (bytes(v if a else 0 for v, a in px),
                bytes(255 if a else 0 for _v, a in px))
    arr = np.array(px, dtype=np.uint16)
    al = arr[:, 1] != 0
    return (np.where(al, arr[:, 0], 0).astype(np.uint8).tobytes(),
            np.where(al, 255, 0).astype(np.uint8).tobytes())


def verify_block(kind: str, name: str, rebuilt: bytes, outdir: Path) -> dict:
    """pack 后校验：重建块重新解码的像素 == 导出 PNG 记录的像素。
    返回 {frames, ok, byte_equal}；结构类块 byte_equal 恒真。"""
    if kind == "image":
        doc = _read_json(outdir, f"{name}.json")
        w, h = doc["w"], doc["h"]
        if doc.get("codec") == "rle8":
            dec, _ = codec.decode_byte_rle(rebuilt[4:], w, h)
            if dec is None:
                return {"frames": 1, "ok": 0, "byte_equal": 0}
            want = _px_key(_png_px(outdir, doc["atlas"], 0))
            ok = int(_px_key(dec) == want)
        else:
            gi, ga, _ = codec.decode_rle4_rows_ab(rebuilt[4:], w, h)
            wi, wa = _verify_px_arrays(outdir, doc["atlas"], 0)
            ok = int(gi is not None and _ab_key_eq(gi, ga, wi, wa))
        return {"frames": 1, "ok": ok,
                "byte_equal": int(doc.get("byte_equal", False))}
    if kind == "wh_anim":
        doc = _read_json(outdir, f"{name}.json")
        ok = total = 0
        for m in doc["frames"]:
            if "raw_hex" in m:
                continue
            total += 1
            body = _wh_frame_bytes(rebuilt, doc, m)
            gi, ga, _ = codec.decode_rle4_rows_ab(body[13:], m["w"], m["h"])
            if gi is None:
                continue
            wi, wa = _verify_px_arrays(outdir, doc["atlas"],
                                       m["atlas_index"])
            ok += int(_ab_key_eq(gi, ga, wi, wa))
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    if kind == "dato":
        doc = _read_json(outdir, f"{name}.json")
        ok = total = 0
        for m in doc["frames"]:
            if "raw_hex" in m:
                continue
            total += 1
            body = _dato_frame_bytes(rebuilt, doc, m)
            dec, _ = codec.decode_byte_rle(body[4:], m["w"], m["h"])
            if dec is None:
                continue
            want = _px_key(_png_px(outdir, doc["atlas"], m["atlas_index"]))
            ok += int(_px_key(dec) == want)
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    if kind == "lmi1":
        doc = _read_json(outdir, f"{name}.json")
        ok = total = 0
        for m in doc["frames"]:
            if m.get("codec") == "lut256" or "raw_hex" in m:
                continue
            total += 1
            body = _lmi1_frame_bytes(rebuilt, doc, m["index"])
            w, h = m["w"], m["h"]
            stream = body[4:]
            dec = None
            if m["codec"] == "rle8":
                dec, _ = codec.decode_byte_rle(stream, w, h)
            elif m["codec"] == "rle4":
                dec, _ = codec.decode_rle4_rows(stream, w, h)
            elif m["codec"] == "raw_t":
                dec, _ = codec.decode_raw(stream, w, h, True)
            if dec is None:
                continue
            want = _px_key(_png_px(outdir, doc["atlas"], m["atlas_index"]))
            ok += int(_px_key(dec) == want)
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    if kind == "tiles":
        doc = _read_json(outdir, f"{name}.json")
        im = Image.open(str(outdir / doc["atlas"]["image"])).convert("RGBA")
        pix = im.load()
        w, h, n, cols = (doc["tile_w"], doc["tile_h"], doc["tile_count"],
                         doc["cols"])
        ok = total = 0
        offs = _tile_offsets(rebuilt, n)
        for i in range(n):
            m = doc["tiles"][i]
            if "raw_hex" in m:
                continue
            total += 1
            end = offs[i + 1] if i < n - 1 else len(rebuilt)
            dec, _ = codec.decode_rle4_linear(rebuilt[offs[i]:end], w, h)
            if dec is None:
                continue
            tx, ty = (i % cols) * w, (i // cols) * h
            want = []
            for yy in range(h):
                for xx in range(w):
                    r, _g, _b, a = pix[tx + xx, ty + yy]
                    want.append((r, 255 if a else 0))
            ok += int(_px_key(dec) == _px_key(want))
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    if kind == "sprite24_pkg":
        doc = _read_json(outdir, "FDICON.json")
        crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
        ok = total = 0
        for m in doc["frames"]:
            if "raw_hex" in m:
                continue
            total += 1
            dec, _ = codec.decode_sprite24(
                _sprite24_body(rebuilt, doc, m["index"]),
                True, doc["icon_w"], doc["icon_h"])
            if dec is None:
                continue
            px, _w, _h = crops[m["atlas_index"]]
            ok += int(_px_key(dec) == _px_key(px))
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    if kind == "image_raw":
        doc = _read_json(outdir, f"{name}.json")
        (px, w, h), = _crop_atlas(outdir, doc["atlas"])
        got = rebuilt[4:4 + w * h]
        ok = int(got == bytes(v for v, _a in px))
        return {"frames": 1, "ok": ok, "byte_equal": ok}
    if kind == "sprite_pkg":
        doc = _read_json(outdir, f"{name}.json")
        crops = _crop_atlas(outdir, doc["atlas"]) if "atlas" in doc else []
        ok = total = 0
        n = doc["table_entries"]
        offs = [rd_u32(rebuilt, 6 + 4 * i) for i in range(n)]
        for m in doc["frames"]:
            if "raw_hex" in m:
                continue
            total += 1
            i = m["index"]
            end = offs[i + 1] if i + 1 < n else len(rebuilt)
            dec, _ = codec.decode_sprite24(rebuilt[offs[i]:end],
                                           True, doc["icon_w"], doc["icon_h"])
            if dec is None:
                continue
            px, _w, _h = crops[m["atlas_index"]]
            ok += int(_px_key(dec) == _px_key(px))
        return {"frames": total, "ok": ok,
                "byte_equal": _manifest_byte_equal(doc)}
    return {"frames": 0, "ok": 0, "byte_equal": 1}


def _manifest_byte_equal(doc: dict) -> int:
    metas = doc.get("frames", doc.get("tiles", []))
    if not metas:
        return 1
    flagged = [m for m in metas if "byte_equal" in m]
    return int(bool(flagged) and all(m["byte_equal"] for m in flagged))


def _sprite24_body(rebuilt: bytes, doc: dict, index: int) -> bytes:
    count = doc["frame_count"]
    offs = _dir_entries(rebuilt, 6, count + 1)
    return rebuilt[offs[index]:offs[index + 1]]


def _dir_entries(buf: bytes, pos: int, n: int):
    return [rd_u32(buf, pos + 4 * i) for i in range(n)]


def _wh_frame_bytes(rebuilt: bytes, doc: dict, m: dict) -> bytes:
    n = doc["frame_count"]
    offs = _dir_entries(rebuilt, 8, n)
    i = m["index"]
    end = offs[i + 1] if i + 1 < n else len(rebuilt)
    return rebuilt[offs[i]:end]


def _dato_frame_bytes(rebuilt: bytes, doc: dict, m: dict) -> bytes:
    offs = _dir_entries(rebuilt, 0, 4)
    i = m["index"]
    end = offs[i + 1] if i < 3 else len(rebuilt)
    return rebuilt[offs[i]:end]


def _lmi1_frame_bytes(rebuilt: bytes, doc: dict, index: int) -> bytes:
    n = doc["frame_count"]
    offs = _dir_entries(rebuilt, 6, n + 1)
    return rebuilt[offs[index]:offs[index + 1]]


def _tile_offsets(rebuilt: bytes, n: int):
    offs = _dir_entries(rebuilt, 6, n)
    return offs + [len(rebuilt)]
