# -*- coding: utf-8 -*-
"""PNG 调色板/图集 I/O（fd2dat）

每个图集导出两份 PNG：
- `<name>.png`      观看用：调色板应用后的 RGBA（FD2 调色板 6bit VGA → 8bit）。
- `<name>.idx.png`  无损索引：R=G=B=色号、A=流级透明位（pack 的真值来源）。

TP JSON（TexturePacker hash 格式）：帧定位（WH 族帧头 x/y / mode）映射到
spriteSourceSize/sourceSize，fd2 专有元数据放 meta.fd2 与帧内自定义键。
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image

from . import codec

Pixels = List[Tuple[int, int]]


def scale6(v: int) -> int:
    return (v * 255) // 63


def load_palette_768(b: bytes) -> List[Tuple[int, int, int]]:
    """768B VGA 6bit 调色板 → [(r,g,b)]×256（保持 6bit 原值）。"""
    assert len(b) == 768
    return [(b[3 * i], b[3 * i + 1], b[3 * i + 2]) for i in range(256)]


def pack_atlas(sizes: Sequence[Tuple[int, int]],
               max_width: int = 1024) -> Tuple[int, int, List[Tuple[int, int]]]:
    """货架式装箱：按高降序逐行摆放。返回 (W, H, 位置列表)。"""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0]))
    x = y = 0
    row_h = 0
    max_x = 0
    pos: List[Optional[Tuple[int, int]]] = [None] * len(sizes)
    for i in order:
        w, h = sizes[i]
        if x + w > max_width and x > 0:
            y += row_h
            x = 0
            row_h = 0
        pos[i] = (x, y)
        x += w
        row_h = max(row_h, h)
        max_x = max(max_x, x)
    return max_x, y + row_h, pos


def write_atlas_pngs(png_path: Path, frames: Sequence[Pixels],
                     sizes: Sequence[Tuple[int, int]],
                     positions: Sequence[Tuple[int, int]],
                     atlas_size: Tuple[int, int],
                     palette: Optional[Sequence[Tuple[int, int, int]]],
                     transparent_render_zero: bool) -> None:
    """索引 PNG（真值）+ 调色板观看 PNG。transparent_render_zero：观看图
    是否把色号 0 渲染为透明（byte-RLE/RAW 族 0=不写的消费语义）。"""
    w, h = atlas_size
    idx = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    view = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pi = idx.load()
    pv = view.load()
    for f, (fw, fh) in enumerate(sizes):
        ax, ay = positions[f]
        px = frames[f]
        for yy in range(fh):
            for xx in range(fw):
                index, alpha = px[yy * fw + xx]
                pi[ax + xx, ay + yy] = (index, index, index, alpha)
                if alpha:
                    if index == 0 and transparent_render_zero:
                        rgba = (0, 0, 0, 0)
                    elif palette:
                        rgba = (scale6(palette[index][0]),
                                scale6(palette[index][1]),
                                scale6(palette[index][2]), 255)
                    else:
                        rgba = (index, index, index, 255)
                else:
                    rgba = (0, 0, 0, 0)
                pv[ax + xx, ay + yy] = rgba
    png_path.parent.mkdir(parents=True, exist_ok=True)
    idx.save(str(png_path) + ".idx.png")
    view.save(str(png_path))


def read_idx_png(path: Path) -> Tuple[List[Pixels], List[Tuple[int, int]], Tuple[int, int]]:
    """读索引 PNG，返回 (帧像素列表, 尺寸列表, 图集尺寸)。非透明像素的
    index 取 R 通道；alpha=0 -> (index_from_R, 0)（保留 skip 位置的原色号）。"""
    im = Image.open(str(path) + ".idx.png").convert("RGBA")
    w, h = im.size
    data = im.load()
    return data, (w, h), im.size  # 由调用方按 boxes 裁剪


def crop_pixels(im_data, box: Tuple[int, int, int, int]) -> Pixels:
    """im_data = PIL Image（已 convert RGBA）。numpy 批量裁剪（纯 Python
    逐像素循环对 2000+ 帧图集慢 2 个量级）；无 numpy 环境回退逐像素。"""
    x0, y0, w, h = box
    try:
        import numpy as np
        arr = np.asarray(im_data)
        sub = arr[y0:y0 + h, x0:x0 + w]
        idx = sub[:, :, 0].astype(np.uint16)
        alpha = (sub[:, :, 3] != 0).astype(np.uint8) * 255
        return list(zip(idx.ravel().tolist(), alpha.ravel().tolist()))
    except ImportError:
        px = im_data.load()
        out: Pixels = []
        for yy in range(h):
            for xx in range(w):
                r, _g, _b, a = px[x0 + xx, y0 + yy]
                out.append((r, 255 if a else 0))
        return out


def write_tp_json(json_path: Path, image_name: str, frames: List[dict],
                  atlas_size: Tuple[int, int], fd2_meta: dict) -> None:
    """TexturePacker hash 格式；fd2 元数据进 meta.fd2 / 帧 ["fd2"]。"""
    doc = {
        "frames": {},
        "meta": {
            "app": "fd2dat",
            "version": "1.0",
            "image": image_name,
            "format": "RGBA8888",
            "size": {"w": atlas_size[0], "h": atlas_size[1]},
            "scale": "1",
            "smartupdate": False,
            "fd2": fd2_meta,
        },
    }
    for f in frames:
        doc["frames"][f["name"]] = {
            "frame": {"x": f["x"], "y": f["y"], "w": f["w"], "h": f["h"]},
            "rotated": False,
            "trimmed": False,
            "spriteSourceSize": {"x": f.get("src_x", 0), "y": f.get("src_y", 0),
                                 "w": f["w"], "h": f["h"]},
            "sourceSize": {"w": f.get("canvas_w", f["w"]),
                           "h": f.get("canvas_h", f["h"])},
            "duration": f.get("duration", 100.0),
            **({"fd2": f["fd2"]} if f.get("fd2") else {}),
        }
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n",
                         encoding="utf-8")
