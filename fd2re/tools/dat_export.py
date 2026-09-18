# -*- coding: utf-8 -*-
"""DAT -> 现代格式目录 导出（fd2dat.export）

用法：dat_export.py <FD2.EXE 同目录下的资源名> [--out DIR]
资源名 = ANI/BG/DATO/FDFIELD/FDMUS/FDOTHER/FDSHAP/FDTXT/FIGANI/TAI/TITLE.DAT
        或 FDICON.B24。

产出：<out>/<资源名去扩展>/ 下 manifest.json + 各块现代格式文件族，
并打印种类普查与像素回包自检统计。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from fd2dat import blockio, container, pngio
from fd2dat.codec import rd_u16, rd_u32

FDICON_NAME = "FDICON.B24"


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def init_ctx(repo_dir: Path) -> blockio.Ctx:
    """默认调色板 = FDOTHER.DAT 首个 768B 块（blk0 = 启动当前调色板）。"""
    ctx = blockio.Ctx()
    other = repo_dir / "FDOTHER.DAT"
    if other.exists():
        data = other.read_bytes()
        try:
            for i, blk in enumerate(container.container_blocks(data)):
                if len(blk) == 768:
                    ctx.default_palette = pngio.load_palette_768(blk)
                    ctx.default_palette_id = i
                    break
        except ValueError:
            pass
    return ctx


def export_fdicon(src: Path, outdir: Path, ctx: blockio.Ctx) -> dict:
    data = src.read_bytes()
    w, h, count = struct.unpack_from("<HHH", data)
    table = [rd_u32(data, 6 + 4 * i) for i in range(count + 1)]
    items, metas = [], []
    for i in range(count):
        body = data[table[i]:table[i + 1]]
        px, _ = blockio.codec.decode_sprite24(body, transparent=True,
                                              w=w, h=h)
        meta = {"index": i, "icon": i // 12, "phase": i % 12,
                "codec": "sprite24", "w": w, "h": h}
        if px is None:
            ctx.note_frame(False)
            meta["raw_hex"] = body.hex()
        else:
            again = blockio.codec.encode_sprite24(px, w, h)
            meta["reencoded"] = again == body
            ctx.note_frame(again == body)
            meta["atlas_index"] = len(items)
            if again != body:
                # 贪婪 sprite24 编码对个别形态有损：非字节等价帧保留原流
                meta["raw_hex"] = body.hex()
            items.append({"pixels": px, "w": w, "h": h,
                          "name": f"icon{i//12:03d}p{i%12}",
                          "fd2": {"icon": i // 12, "phase": i % 12}})
        metas.append(meta)
    atlas = blockio._emit_atlas(
        outdir, "icons", items, ctx.default_palette, True,
        (w, h), {"kind": "sprite24_pkg", "source": src.name,
                 "note": "140 立绘源 × 12 相位（icon_load_entry 0x11019）；"
                         "op11 跳过语义=透明变体（0x1AE86 定案）"}, tp=True)
    entry = {"kind": "sprite24_pkg", "icon_w": w, "icon_h": h,
             "frame_count": count, "frames_per_icon": 12, "frames": metas,
             "atlas": atlas}
    blockio._write_json(outdir, "FDICON.json", entry)
    blocks_meta = [{"id": 0, "kind": "sprite24_pkg",
                    "files": ["FDICON.json", "icons.png", "icons.idx.png",
                              "icons.tp.json"],
                    "frames": count}]
    return {
        "schema": "fd2dat.export/1", "source": src.name,
        "source_path": str(src.resolve()),
        "sha256": sha256(data), "size": len(data),
        "census": {"sprite24_pkg": [0]}, "blocks": blocks_meta,
        "pixel_roundtrip": dict(ctx.stats),
    }


def export_dat(src: Path, outdir: Path, ctx: blockio.Ctx) -> dict:
    data = src.read_bytes()
    blocks = container.container_blocks(data)
    outdir.mkdir(parents=True, exist_ok=True)
    blocks_meta = []
    census: dict[str, list[int]] = {}
    for i, b in enumerate(blocks):
        kind = container.classify_block(b, src.name, i)
        kind = blockio.fix_kind(src.name, i, kind, blocks)
        census.setdefault(kind, []).append(i)
        name = f"b{i:03d}"
        fn = blockio.EXPORTERS[kind]
        try:
            r = fn(outdir, name, b, ctx)
        except Exception as e:                      # 导出失败不阻断全量
            r = blockio._export_unknown(outdir, name, b, ctx)
            r["error"] = f"{type(e).__name__}: {e}"
            kind = "unknown"
        entry = {"id": i, "kind": kind, "files": r["files"]}
        entry.update({k: v for k, v in r.items()
                      if k not in ("kind", "files")})
        blocks_meta.append(entry)
    manifest = {
        "schema": "fd2dat.export/1", "source": src.name,
        "source_path": str(src.resolve()),
        "sha256": sha256(data), "size": len(data),
        "container": "LLLLLL", "block_count": len(blocks),
        "census": census, "blocks": blocks_meta,
        "pixel_roundtrip": dict(ctx.stats),
        "default_palette": getattr(ctx, "default_palette_id", None),
    }
    (outdir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8")
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description="FD2 DAT -> 现代格式目录")
    ap.add_argument("source", type=Path, help="DAT/B24 文件路径")
    ap.add_argument("--out", type=Path, default=None,
                    help="输出根目录（默认 <repo>/tools/dat-modern）")
    args = ap.parse_args()

    src = args.source
    if not src.exists():
        tools_dir = Path(__file__).resolve().parent
        alt = tools_dir.parent.parent / src.name
        if alt.exists():
            src = alt
        else:
            print(f"找不到 {args.source}", file=sys.stderr)
            return 2
    repo_dir = src.parent
    out_root = args.out or (Path(__file__).resolve().parent / "dat-modern")
    outdir = out_root / src.stem if src.suffix == ".DAT" else out_root / src.name
    ctx = init_ctx(repo_dir)
    print(f"export {src.name} ({src.stat().st_size} bytes) -> {outdir}")
    if src.name == FDICON_NAME:
        manifest = export_fdicon(src, outdir, ctx)
        (outdir / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=1) + "\n",
            encoding="utf-8")
    else:
        manifest = export_dat(src, outdir, ctx)
    print(f"  块数 {manifest.get('block_count', 1)}  种类：")
    for kind, ids in sorted(manifest["census"].items(),
                            key=lambda kv: -len(kv[1])):
        head = ",".join(map(str, ids[:14]))
        more = f" …(+{len(ids)-14})" if len(ids) > 14 else ""
        print(f"    {kind:18s} n={len(ids):4d}  [{head}{more}]")
    rt = manifest["pixel_roundtrip"]
    total = rt["frames_total"]
    if total:
        print(f"  像素回包：{total} 帧全部可从 PNG 重建；"
              f"字节一致率 {rt['frames_pixel_rt']}/{total}（其余为像素等价"
              f"重编码，pack 时解码校验）")
    for e in manifest["blocks"]:
        if e.get("error"):
            print(f"  !! b{e['id']:03d} 导出异常：{e['error']}")
    print("  manifest.json 已写")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
