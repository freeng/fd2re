# -*- coding: utf-8 -*-
"""dat_preview — 仅从 dat_export 产出的现代格式目录「模拟工程使用」（可观察）

不读任何原版 DAT：地图渲染读 FDFIELD JSON + FDSHAP tileset PNG，动画读
TexturePacker JSON + PNG，文本读 FDTXT JSON + FDOTHER 字形 PNG，音频读
.mid/.wav。产物为 PNG/GIF/控制台报告，用于确认现代格式目录携带了工程
消费所需的全部信息（fd2re 的使用路径 = architecture.md §5/§6.8/§13）。

子命令：
  map     <FDFIELD目录> <FDSHAP目录> <state> [--palette N] [--fdother DIR]
  anim    <目录> <块号> [--palette N] [--fdother DIR]   # WH 族帧包 → GIF
  portrait<目录> <块号> [--fdother DIR]                 # DATO 口型 → GIF
  icons   <FDICON目录> <图标号>                          # 立绘 12 相位 → GIF
  title   <FDOTHER目录> [--fdother DIR]                  # blk74+76/blk101 合成
  ani     <ANI目录> <动画号> [--delay MS]                # ANI 帧序 → GIF
  music   <FDMUS目录> <轨号>                             # .mid 报告
  sfx     <FDOTHER目录> <块号>                           # wav 时长报告
  text    <FDTXT目录> <块号> <串号...> --fdother DIR     # 字形渲染 → PNG
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import wave
from pathlib import Path

from PIL import Image

CANVAS = (320, 200)


def die(msg: str):
    print(msg, file=sys.stderr)
    raise SystemExit(2)


def jload(p: Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


def bname(i: int) -> str:
    return f"b{i:03d}"


def load_palette(fdother: Path, n: int):
    doc = jload(fdother / f"{bname(n)}.palette.json")
    pal = doc["palette"]
    return [(r * 255 // 63, g * 255 // 63, b * 255 // 63) for r, g, b in pal]


def palette_image(idx_img: Image.Image, palette, canvas=None, pos=(0, 0)):
    """把索引图按调色板上色（透明保留）。"""
    if canvas:
        out = Image.new("RGBA", canvas, (0, 0, 0, 0))
    else:
        out = Image.new("RGBA", idx_img.size, (0, 0, 0, 0))
    px = idx_img.load()
    op = out.load()
    for y in range(idx_img.size[1]):
        for x in range(idx_img.size[0]):
            r, _g, _b, a = px[x, y]
            if a:
                op[pos[0] + x, pos[1] + y] = (*palette[r], 255)
    return out


def crop_box(img: Image.Image, box):
    x, y, w, h = box
    return img.crop((x, y, x + w, y + h))


def atlas_truth_path(dirp: Path, atlas: dict) -> Path:
    """图集索引真值图（R=色号、A=流级透明）。两种导出约定：_emit_atlas
    族 image 指向观看图（真值 = 追加 .idx.png）；tiles 族 image 直接指
    向 .idx.png。拿观看图 R 通道再上色 = 调色板套调色板（花屏），与
    pack 的 idx 真值修复同源。"""
    p = dirp / atlas["image"]
    if p.name.endswith(".idx.png"):
        return p
    return p.with_name(p.name + ".idx.png")


def frames_of(dirp: Path, name: str):
    """读块 JSON + 索引真值图集，返回 [(帧索引图, meta)]。"""
    doc = jload(dirp / f"{name}.json")
    atlas = doc.get("atlas")
    if not atlas:
        return doc, []
    img = Image.open(str(atlas_truth_path(dirp, atlas)))
    frames = []
    for m in doc["frames"]:
        if "atlas_index" not in m:
            continue
        frames.append((crop_box(img, atlas["boxes"][m["atlas_index"]]), m))
    return doc, frames


def save_gif(paths_frames, out: Path, duration=120, canvas=None, loop=0):
    imgs = []
    for idx_img, palette in paths_frames:
        if canvas:
            base = Image.new("RGBA", canvas, (0, 0, 0, 0))
            base.alpha_composite(palette)
        else:
            base = palette
        imgs.append(base.convert("RGBA").quantize(colors=256))
    imgs[0].save(str(out), save_all=True, append_images=imgs[1:],
                 duration=duration, loop=loop, disposal=2)
    print(f"  -> {out}  ({len(imgs)} 帧)")


# ------------------------------------------------------------------ map

def cmd_map(a):
    state = a.state
    fdir, sdir = Path(a.field_dir), Path(a.shap_dir)
    fmap = jload(fdir / f"{bname(3*state)}.json")
    fctx = jload(fdir / f"{bname(3*state+1)}.json")
    fdep = jload(fdir / f"{bname(3*state+2)}.json")
    shape = fctx.get("shape", 0)
    tiles_doc = jload(sdir / f"{bname(2*shape)}.json")
    ts = Image.open(str(atlas_truth_path(sdir, tiles_doc["atlas"])))
    palette = load_palette(Path(a.fdother), a.palette) if a.fdother else None
    if palette is None:
        die("需要 --fdother <FDOTHER导出目录> 提供调色板")
    w, h = fmap["w"], fmap["h"]
    out = Image.new("RGBA", (w * 24, h * 24), (0, 0, 0, 255))
    cols = tiles_doc["cols"]
    tw, th = tiles_doc["tile_w"], tiles_doc["tile_h"]
    for y in range(h):
        for x in range(w):
            t = fmap["tiles"][y * w + x]
            tx, ty = (t % cols) * tw, (t // cols) * th
            tile = ts.crop((tx, ty, tx + tw, ty + th)).convert("RGBA")
            out.alpha_composite(palette_image(tile, palette), (x * 24, y * 24))
    dest = Path(a.out)
    dest.parent.mkdir(parents=True, exist_ok=True)
    out.convert("RGB").save(str(dest))
    print(f"state {state}: 地图 {w}x{h} 格，shape 变体 {shape} "
          f"(FDSHAP 块 {2*shape}/{2*shape+1})，palette blk{a.palette}")
    print(f"  玩家槽位 P={fctx['player_slots']}  敌条数 E={fctx['enemy_count']}  "
          f"站立记录 F={fctx['record_count']}")
    tr = [t for t in fctx["treasures"] if t["type"]]
    print(f"  宝箱 {len(tr)} 个："
          + (", ".join(f"tile{3*i}->{('金' if t['type']==1 else ('物' if t['type']==0 else '事件'))}:{t['param']}"
                       for i, t in enumerate(fctx["treasures"]) if t["type"]) or "无"))
    print(f"  布阵层条目 {fdep['entry_count']}（头 u16 = P+E）")
    print(f"  -> {dest}")


# ----------------------------------------------------------------- anim

def cmd_anim(a):
    dirp = Path(a.dir)
    name = bname(a.block)
    doc, frames = frames_of(dirp, name)
    if not frames:
        die(f"{name} 不是帧包（无图集）")
    palette = load_palette(Path(a.fdother), a.palette)
    composed = []
    for idx_img, m in frames:
        rgba = palette_image(idx_img.convert("RGBA"), palette)
        canvas = Image.new("RGBA", CANVAS, (0, 0, 0, 255))
        canvas.alpha_composite(rgba, (m.get("x", 0) % 320, m.get("y", 0) % 200))
        composed.append((idx_img, rgba, canvas, m))
    outdir = Path(a.out).parent
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    composed[0][2].convert("RGB").save(str(a.out))
    gif = Path(str(a.out).with_suffix("")) .with_suffix("") if False else Path(str(a.out)[:-4] + ".gif")
    save_gif([(c[0], c[2]) for c in composed], gif)
    print(f"  块 {name}: {len(composed)} 帧（WH 族 x/y 定位，mode 元数据见"
          f" {name}.tp.json），首帧 -> {a.out}")


def cmd_portrait(a):
    dirp = Path(a.dir)
    name = bname(a.block)
    doc, frames = frames_of(dirp, name)
    palette = load_palette(Path(a.fdother), a.palette)
    outdir = Path(a.out).parent
    outdir.mkdir(parents=True, exist_ok=True)
    pal_frames = []
    for idx_img, m in frames:
        rgba = palette_image(idx_img.convert("RGBA"), palette)
        base = Image.new("RGBA", idx_img.size, (0, 0, 0, 255))
        base.alpha_composite(rgba)
        pal_frames.append((idx_img, base))
    save_gif(pal_frames, Path(a.out), duration=a.delay)
    print(f"DATO {name}: {len(frames)} 帧口型（byte-RLE 不透明，§13.33）")


def cmd_icons(a):
    dirp = Path(a.dir)
    doc = jload(dirp / "FDICON.json")
    atlas = doc["atlas"]
    # icons 直接贴观看图（已按 FDOTHER blk0 调色板上色、无 --palette 入参；
    # 若走 idx 真值则需引入调色板依赖）
    img = Image.open(str(dirp / atlas["image"]))
    frames = []
    for m in doc["frames"]:
        if m.get("icon") == a.icon and "atlas_index" in m:
            frames.append((crop_box(img, atlas["boxes"][m["atlas_index"]]), m))
    if not frames:
        die(f"icon {a.icon} 不存在（0..{doc['frame_count']//12-1}）")
    rgba = [f[0].convert("RGBA") for f in frames]
    strip = Image.new("RGBA", (24 * len(rgba), 24), (0, 0, 0, 255))
    for i, im in enumerate(rgba):
        strip.alpha_composite(im, (i * 24, 0))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    strip.convert("RGB").save(str(a.out))
    gif = Path(str(a.out)[:-4] + ".gif")
    save_gif([(f[0], f[0].convert("RGBA")) for f in frames], gif, duration=150)
    print(f"icon {a.icon}: {len(frames)} 相位 -> {a.out} + {gif.name}")


def load_block_image(dirp: Path, name: str):
    """单帧图块（image/image_raw/lmi1 首帧）→ (索引图, w, h)。"""
    doc = jload(dirp / f"{name}.json")
    atlas = doc.get("atlas")
    if not atlas:
        return None, 0, 0
    img = Image.open(str(atlas_truth_path(dirp, atlas)))
    x, y, w, h = atlas["boxes"][0]
    return img.crop((x, y, x + w, y + h)), w, h


def cmd_title(a):
    fd = Path(a.dir)
    palette = load_palette(fd, a.palette)
    canvas = Image.new("RGB", CANVAS)
    for blk, note in ((74, "标题底帧"), (75, "菜单覆盖")):
        idx_img, w, h = load_block_image(fd, bname(blk))
        if idx_img is None:
            print(f"  （blk{blk} 无图集，跳过）")
            continue
        rgba = palette_image(idx_img.convert("RGBA"), palette)
        canvas.paste(rgba.convert("RGB"), (0, 0), rgba)
        print(f"  贴 blk{blk}（{note}，{w}x{h}）")
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    canvas.save(str(a.out))
    # 卷轴 blk69..73 纵向合成（startup 320x735）
    strip = Image.new("RGB", (320, 735))
    y = 0
    for blk in range(69, 74):
        idx_img, w, h = load_block_image(fd, bname(blk))
        if idx_img is None:
            break
        rgba = palette_image(idx_img.convert("RGBA"), palette)
        strip.paste(rgba.convert("RGB"), (0, y), rgba)
        y += h
    if y:
        strip = strip.crop((0, 0, 320, y))
        sp = Path(str(a.out)[:-4] + ".scroll.png")
        strip.save(str(sp))
        print(f"  卷轴 blk69..73 合成 {320}x{y} -> {sp}")
    print(f"  -> {a.out}（palette blk{a.palette}）")


def cmd_ani(a):
    dirp = Path(a.dir)
    name = bname(a.anim)
    doc = jload(dirp / f"{name}.frames.json")
    frames = sorted(dirp.glob(f"{name}.f*.png"))
    if not frames:
        die(f"{name}: 无 PNG 帧")
    imgs = [Image.open(str(p)).quantize(colors=256) for p in frames]
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    imgs[0].save(str(a.out), save_all=True, append_images=imgs[1:],
                 duration=a.delay, loop=0, disposal=2)
    print(f"ANI {name}: {len(imgs)} 帧 -> {a.out}（{a.delay}ms/帧）")


def cmd_music(a):
    dirp = Path(a.dir)
    name = bname(a.track)
    doc = jload(dirp / f"{name}.events.json")
    evs = doc["events"]
    total_t = 0
    t = 0
    notes = 0
    channels = set()
    programs = {}
    for ev in evs:
        t += ev.get("t", 0)
        total_t = max(total_t, t)
        st = ev["st"]
        if (st & 0xF0) == 0x90 and ev.get("d2", 0) > 0:
            notes += 1
            channels.add(st & 0xF)
            if "dur" in ev:
                total_t = max(total_t, t + ev["dur"])
        elif (st & 0xF0) == 0xC0:
            channels.add(st & 0xF)
            programs[st & 0xF] = ev["d1"]
    print(f"FDMUS {name}: {len(evs)} 事件，{notes} 个 note-on，"
          f"通道 {sorted(channels)}，program {programs}")
    print(f"  时长 ≈ {total_t/120:.1f}s（恒 120 tick/s，FF51={doc.get('tempo_meta_original')} 不改速率）")
    print(f"  TIMB 预装 {len(doc['timb'])} 项；"
          f"现代格式 {name}.mid 可直接进 DAW/libADLMIDI")


def cmd_sfx(a):
    dirp = Path(a.dir)
    name = bname(a.block)
    doc = jload(dirp / f"{name}.json")
    durs = [l / doc["rate"] for l in doc["lengths"]]
    print(f"{name}: {doc['entries']} 个 PCM 条目 "
          f"({doc['rate']}Hz 8bit unsigned 单声道)")
    for i, d in enumerate(durs):
        print(f"  e{i:02d}  {d*1000:7.1f} ms")


def cmd_text(a):
    tdir = Path(a.txt_dir)
    fd = Path(a.fdother)
    doc = jload(tdir / f"{bname(a.block)}.strings.json")
    font_doc = jload(fd / "b004.json")
    font = Image.open(str(fd / "b004.idx.png")).convert("L")
    cols = font_doc["cols"]
    palette = load_palette(fd, a.palette)
    fg = palette[a.fg]
    shadow = palette[a.shadow]
    strings = {s["id"]: s["tokens"] for s in doc["strings"]}
    canvas = Image.new("RGB", CANVAS, tuple(palette[a.bg]))
    px = canvas.load()

    def glyph(g, x, y):
        gx, gy = (g % cols) * 16, (g // cols) * 16
        for r in range(16):
            for c in range(16):
                if not font.getpixel((gx + c, gy + r)):
                    continue
                if x + c >= CANVAS[0] or y >= CANVAS[1]:
                    continue
                if y + r + 1 < CANVAS[1]:
                    px[x + c, y + r + 1] = shadow
                    if x + c > 0:
                        px[x + c - 1, y + r + 1] = shadow
                px[x + c, y + r] = fg

    for sid in a.ids:
        toks = strings.get(sid)
        if toks is None:
            print(f"  str {sid}: 不存在")
            continue
        x, y = 4, 4 + 22 * a.ids.index(sid)
        for tok in toks:
            t = tok["t"]
            if t == -1:
                break
            if t >= 0:
                if t != 10:
                    glyph(t, x, y)
                x += 16
            elif t == -2:
                x, y = 4, y + 19
            elif t == -3:
                x, y = 4, y + 19
            elif t == -6:
                glyph(0, x, y)
                x += 16
            elif t in (-17, -18, -19, -20):
                print(f"  str {sid}: 说话人窗 token {t}（参数 {tok.get('arg')}）"
                      f"→ DATO 块 {tok.get('arg')}，见 portrait")
            elif t in (-4, -5):
                print(f"  str {sid}: 嵌套数字串 {t}({tok.get('arg')})→ NUM")
        print(f"  str {sid}: 渲染 {len(toks)} token")
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    canvas.save(str(a.out))
    print(f"  -> {a.out}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("map")
    p.add_argument("field_dir"), p.add_argument("shap_dir")
    p.add_argument("state", type=int)
    p.add_argument("--fdother", required=False)
    p.add_argument("--palette", type=int, default=0)
    p.add_argument("--out", default="preview.map.png")

    p = sub.add_parser("anim")
    p.add_argument("dir"), p.add_argument("block", type=int)
    p.add_argument("--fdother", required=True)
    p.add_argument("--palette", type=int, default=0)
    p.add_argument("--out", default="preview.anim.png")

    p = sub.add_parser("portrait")
    p.add_argument("dir"), p.add_argument("block", type=int)
    p.add_argument("--fdother", required=True)
    p.add_argument("--palette", type=int, default=0)
    p.add_argument("--delay", type=int, default=250)
    p.add_argument("--out", default="preview.portrait.gif")

    p = sub.add_parser("icons")
    p.add_argument("dir"), p.add_argument("icon", type=int)
    p.add_argument("--out", default="preview.icon.png")

    p = sub.add_parser("title")
    p.add_argument("dir")
    p.add_argument("--palette", type=int, default=76)
    p.add_argument("--out", default="preview.title.png")

    p = sub.add_parser("ani")
    p.add_argument("dir"), p.add_argument("anim", type=int)
    p.add_argument("--delay", type=int, default=90)
    p.add_argument("--out", default="preview.ani.gif")

    p = sub.add_parser("music")
    p.add_argument("dir"), p.add_argument("track", type=int)

    p = sub.add_parser("sfx")
    p.add_argument("dir"), p.add_argument("block", type=int)

    p = sub.add_parser("text")
    p.add_argument("txt_dir"), p.add_argument("block", type=int)
    p.add_argument("ids", type=int, nargs="+")
    p.add_argument("--fdother", required=True)
    p.add_argument("--palette", type=int, default=0)
    p.add_argument("--fg", type=int, default=205)
    p.add_argument("--shadow", type=int, default=76)
    p.add_argument("--bg", type=int, default=74)
    p.add_argument("--out", default="preview.text.png")

    a = ap.parse_args()
    {"map": cmd_map, "anim": cmd_anim, "portrait": cmd_portrait,
     "icons": cmd_icons, "title": cmd_title, "ani": cmd_ani,
     "music": cmd_music, "sfx": cmd_sfx, "text": cmd_text}[a.cmd](a)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
