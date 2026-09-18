# -*- coding: utf-8 -*-
"""现代格式目录 -> DAT 回包（fd2dat.pack）

用法：dat_pack.py <导出目录> [--out FILE] [--verify [SRC]]

从 manifest.json 按块重建原始容器字节。--verify 与原文件做 SHA256 对拍
（默认在导出目录旁/仓库根找同名源文件）。字节级一致 = 现代格式目录是
原 DAT 的无损替代源（可编辑后回包给原版/fd2re 使用）。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

from fd2dat import blockio, container


_last_blocks = []


def pack_dir(outdir: Path) -> tuple[bytes, dict]:
    manifest = json.loads((outdir / "manifest.json").read_text(
        encoding="utf-8"))
    blocks = []
    for e in manifest["blocks"]:
        fn = blockio.PACKERS[e["kind"]]
        blocks.append(fn(outdir, f"b{e['id']:03d}", e))
    _last_blocks.clear()
    _last_blocks.extend(blocks)
    # FDICON.B24 是裸 sprite24 包（头 [w,h,cnt]+偏移表），不是 LLLLLL
    # 容器——套壳会把偏移表推后 14B，icon_load_entry 直读字节 6 全错位
    # （序章精灵丢失根因，2026-09-09）
    if manifest["source"].endswith(".B24"):
        return blocks[0], manifest
    return container.build_container(blocks), manifest


def verify_pixels(outdir: Path, manifest: dict, blocks: list) -> bool:
    """pack 后像素级校验：重建块重新解码 == 导出真值图（idx PNG）像素。
    每 5s 打一次心跳进度，长校验（FIGANI 2000+ 帧）不再静默。"""
    total = ok = 0
    all_pass = True
    n = len(manifest["blocks"])
    beat = time.perf_counter()
    for bi, (e, blk) in enumerate(zip(manifest["blocks"], blocks), 1):
        r = blockio.verify_block(e["kind"], f"b{e['id']:03d}", blk, outdir)
        total += r["frames"]
        ok += r["ok"]
        if r["frames"] and r["ok"] != r["frames"]:
            all_pass = False
            print(f"  !! b{e['id']:03d} ({e['kind']}) 像素校验失败 "
                  f"{r['ok']}/{r['frames']}", flush=True)
        if time.perf_counter() - beat >= 5:
            beat = time.perf_counter()
            print(f"  ... 校验中 {bi}/{n} 块（累计 {ok}/{total} 帧）",
                  flush=True)
    if total:
        print(f"  像素校验：{ok}/{total} 帧解码一致")
    return all_pass


def main() -> int:
    ap = argparse.ArgumentParser(description="现代格式目录 -> FD2 DAT")
    ap.add_argument("dir", type=Path, help="dat_export 产出的目录")
    ap.add_argument("--out", type=Path, default=None,
                    help="输出 DAT 路径（默认 <dir>.packed.<ext>）")
    ap.add_argument("--verify", type=Path, default=None, nargs="?",
                    const="AUTO", help="与源 DAT 做 SHA256 对拍")
    args = ap.parse_args()

    outdir = args.dir
    data, manifest = pack_dir(outdir)
    ext = ".B24" if manifest["source"].endswith(".B24") else ".DAT"
    out = args.out or (outdir.parent / outdir.name)
    out = out.parent / (out.name + ".packed" + ext)

    src = args.verify
    if src is not None and str(src) == "AUTO":
        cand = manifest.get("source_path")
        src = Path(cand) if cand and Path(cand).exists() else None
    if not verify_pixels(outdir, manifest, _last_blocks):
        return 1
    got = hashlib.sha256(data).hexdigest()
    out.write_bytes(data)
    print(f"pack {outdir} -> {out}  ({len(data)} bytes)")
    if src:
        want = hashlib.sha256(src.read_bytes()).hexdigest()
        dsz = len(data) - manifest["size"]
        if want == got:
            print(f"  VERIFY BYTE-EXACT：与源 {src.name} SHA256 完全一致")
        else:
            print(f"  VERIFY PIXEL-EQUIVALENT：与源 {src.name} 字节不同"
                  f"（RLE 等价重编码，体积 {dsz:+d}B），像素已逐帧校验")
    else:
        print("  VERIFY OK：回包完成（未提供源文件对拍）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
