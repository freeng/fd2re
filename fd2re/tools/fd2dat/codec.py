# -*- coding: utf-8 -*-
"""FD2 DAT 像素编解码器（fd2dat）

解码器逐条对齐 fd2re/src/video.c / resource.c 已逆向定案的原语：
- byte RLE   (0x4EC66 rle_pixel_next / 0x15F0E pkg_frame_blit 透明、0x4EBFF
              stamp_frame_opaque 不透明)：<=0xC0 字面色号；0xC1..0xFF = 后跟
              色号重复 (b-0xC0) 次。流恒展开为 w*h 像素（0 也是像素），透明
              与否是消费方渲染语义，不是流语义。
- 4-op RLE   (0x4E98D rle_decode_frame / 0x4E22A tile / 0x4E29C sprite24)：
              控制字节 bit7..6 选操作、低 6 位 = count-1：00 实心 / 01 隔点
              / 10 字面 / 11 跳过（sprite24 不透明变体为写 0x49）。跳过是
              流级透明（像素不在流中）。
- RAW        (0x4ED34 透明 / 0x4ED0B 不透明)：[u16 w][u16 h] + 逐像素字节。

无损回包策略：解码时把「流级透明」记为 alpha=0（4-op 跳过 / sprite24 跳过，
索引记跳过变体的填充色 0x49 以便复现 op11），其余像素 alpha=255；回包从
alpha+index 贪婪重编码，导出时逐帧与原始流对拍，不一致的帧落 raw 十六进制
兜底（datexport 自动完成），保证 pack 字节精确。
"""
from __future__ import annotations

import struct
from typing import List, Optional, Tuple

# 像素 = (index, alpha)；alpha 0 = 流级透明（不写目标面）。
Pixels = List[Tuple[int, int]]

SPRITE24_FILL = 0x49   # sprite24 不透明变体 op11 的固定填充色（video.c）


def rd_u16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def rd_i16(b: bytes, off: int) -> int:
    v = b[off] | (b[off + 1] << 8)
    return v - 0x10000 if v & 0x8000 else v


def rd_u32(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)


# ---------------------------------------------------------------- byte RLE

def decode_byte_rle(stream: bytes, w: int, h: int) -> Tuple[Optional[Pixels], int]:
    """字节 RLE 全展开（resource.c rle_pixel_next）。返回 (pixels, consumed)；
    pixels 全 alpha=255（透明与否是渲染语义）。不恰好耗尽/不足 -> (None, 0)。"""
    total = w * h
    out: Pixels = []
    i = 0
    n = len(stream)
    while len(out) < total:
        if i >= n:
            return None, 0
        b = stream[i]
        i += 1
        if b <= 0xC0:
            out.append((b, 255))
        else:
            if i >= n:
                return None, 0
            c = stream[i]
            i += 1
            out.extend([(c, 255)] * (b - 0xC0))
    if len(out) != total:
        return None, 0
    return out, i


def encode_byte_rle(pixels: Pixels, strategy: str = "front63") -> bytes:
    """贪婪字节 RLE。strategy 控制长游程 (>63) 的切分方式，
    导出对拍时在两种常见切分间选择并记录到 JSON。"""
    out = bytearray()
    i, total = 0, len(pixels)
    while i < total:
        c = pixels[i][0]
        run = 0
        while i + run < total and pixels[i + run][0] == c:
            run += 1
        i += run
        if run == 1:
            if c <= 0xC0:
                out.append(c)
            else:
                out.append(0xC1)   # >0xC0 的色号不能作字面字节，转义重复 1 次
                out.append(c)
            continue
        if strategy == "balanced" and run > 63:
            # 均匀切分：避免 63+1 型碎尾
            k = (run + 62) // 63
            base = run // k
            rem = run - base * k
            chunks = [base + 1] * rem + [base] * (k - rem)
        else:
            chunks = []
            r = run
            while r > 0:
                t = min(r, 63)
                chunks.append(t)
                r -= t
        for t in chunks:
            out.append(0xC0 + t)
            out.append(c)
    return bytes(out)


# ------------------------------------------------------------------ 4-op

def decode_rle4_linear(stream: bytes, w: int, h: int,
                       transparent_skip: bool = True,
                       skip_fill: int = 0) -> Tuple[Optional[Pixels], int]:
    """线性 4-op（FDSHAP tile：跨行连续、无行复位；resource.c tile4_decode）。
    op11：transparent_skip=True -> alpha=0（索引记 skip_fill 供复现），
    否则写 skip_fill 不透明。"""
    total = w * h
    out: Pixels = [(0, 255)] * total
    i, px = 0, 0
    n = len(stream)
    while px < total:
        if i >= n:
            return None, 0
        c = stream[i]
        i += 1
        cnt = (c & 0x3F) + 1
        kind = (c >> 6) & 3
        if kind == 0:
            if i >= n:
                return None, 0
            v = stream[i]
            i += 1
            for _ in range(cnt):
                if px >= total:
                    return None, 0
                out[px] = (v, 255)
                px += 1
        elif kind == 1:
            if i >= n:
                return None, 0
            v = stream[i]
            i += 1
            for _ in range(cnt):
                if px + 1 >= total:
                    return None, 0
                out[px] = (skip_fill, 0)      # 隔点位恒不写（tile4 仅写 px+1）
                out[px + 1] = (v, 255)
                px += 2
        elif kind == 2:
            for _ in range(cnt):
                if px >= total or i >= n:
                    return None, 0
                out[px] = (stream[i], 255)
                i += 1
                px += 1
        else:
            for _ in range(cnt):
                if px >= total:
                    return None, 0
                out[px] = (skip_fill, 0 if transparent_skip else 255)
                px += 1
    return out, i


def decode_rle4_rows(stream: bytes, w: int, h: int) -> Tuple[Optional[Pixels], int]:
    """逐行 4-op（video.c rle_decode_frame flag=-1）。C 语义：字面 op 越行时
    目标钳制、但源仍按原始 count 前进（钳制帧无法从像素重建，导出侧标
    clamped 落 raw 兜底）；隔点 op 写【偶数位】(k=0,2,4..)；跳过 op 越行
    即视为非规则帧（C 会把像素写进行首，流无此形态）。"""
    out: Pixels = []
    i, n = 0, len(stream)
    for _row in range(h):
        col = 0
        while col < w:
            if i >= n:
                return None, 0
            c = stream[i]
            i += 1
            cnt = (c & 0x3F) + 1
            kind = (c >> 6) & 3
            if kind == 3:
                if col + cnt > w:
                    return None, 0
                out.extend([(0, 0)] * cnt)
                col += cnt
            elif kind == 2:
                k = min(cnt, w - col)
                if col + cnt > w:
                    return None, 0        # 钳制形态：交 raw 兜底，不猜
                for t in range(cnt):
                    if i >= n:
                        return None, 0
                    out.append((stream[i], 255))
                    i += 1
                col += cnt
            elif kind == 0:
                if i >= n:
                    return None, 0
                v = stream[i]
                i += 1
                if col + cnt > w:
                    return None, 0
                out.extend([(v, 255)] * cnt)
                col += cnt
            else:
                if i >= n:
                    return None, 0
                v = stream[i]
                i += 1
                if col + cnt * 2 > w:
                    return None, 0
                for t in range(cnt * 2):
                    out.append((v, 255) if t % 2 == 0 else (v, 0))
                col += cnt * 2
    return out, i


def decode_rle4_rows_ab(stream: bytes, w: int, h: int):
    """decode_rle4_rows 的字节孪生：产出 (索引 bytearray, alpha bytearray,
    consumed)，拼接全为 C 速度——pack 校验大帧量块（BG/FIGANI 千帧级）
    免 tuple 列表往返。op 语义与合法性判定与 decode_rle4_rows 完全一致；
    不合法流 -> (None, None, 0)。"""
    idx = bytearray()
    al = bytearray()
    i, n = 0, len(stream)
    for _row in range(h):
        col = 0
        while col < w:
            if i >= n:
                return None, None, 0
            c = stream[i]
            i += 1
            cnt = (c & 0x3F) + 1
            kind = (c >> 6) & 3
            if kind == 3:
                if col + cnt > w:
                    return None, None, 0
                idx += b"\x00" * cnt
                al += b"\x00" * cnt
                col += cnt
            elif kind == 2:
                if col + cnt > w or i + cnt > n:
                    return None, None, 0        # 钳制形态：交 raw 兜底，不猜
                idx += stream[i:i + cnt]
                al += b"\xff" * cnt
                i += cnt
                col += cnt
            elif kind == 0:
                if i >= n:
                    return None, None, 0
                v = stream[i]
                i += 1
                if col + cnt > w:
                    return None, None, 0
                idx += bytes([v]) * cnt
                al += b"\xff" * cnt
                col += cnt
            else:
                if i >= n:
                    return None, None, 0
                v = stream[i]
                i += 1
                if col + cnt * 2 > w:
                    return None, None, 0
                idx += bytes([v]) * (cnt * 2)
                al += b"\xff\x00" * cnt
                col += cnt * 2
    if len(idx) != w * h:
        return None, None, 0
    return idx, al, i


def encode_rle4_rows(pixels: Pixels, w: int, h: int) -> bytes:
    """逐行 4-op 重编码 —— 行内贪婪（线性时间：跳过/隔点/实心/字面，
    块长 <=64、不跨行）。原 DP 版对每像素回枚举 64 切分 ×3 操作，平坦
    底图（BG 满屏大色块）全像素命中最坏 O(64w) -> 数分钟；pack 校验是
    像素级对拍，贪婪输出同样像素精确（平坦行与 DP/原版编码器同构字节）。
    字面段内遇 >=3 同色游程断开交还实心分支，体积与 DP 同量级。"""
    out = bytearray()

    for row in range(h):
        base = row * w
        i = 0
        while i < w:
            idx, alpha = pixels[base + i]
            if alpha == 0:
                run = 0
                while i + run < w and pixels[base + i + run][1] == 0:
                    run += 1
                for t in range(0, run, 64):
                    k = min(64, run - t)
                    out.append(0xC0 | (k - 1))
                i += run
                continue
            # 隔点：(v,255),(v,0) 交替对（解码 op01 写偶位）；单对与字面
            # +跳过同字节，按 DP 偏好归字面，>=2 对才走隔点
            if i + 3 < w and pixels[base + i + 1] == (idx, 0):
                pairs = 1
                while (i + 2 * pairs + 1 < w
                       and pixels[base + i + 2 * pairs] == (idx, 255)
                       and pixels[base + i + 2 * pairs + 1] == (idx, 0)):
                    pairs += 1
                if pairs >= 2:
                    for t in range(0, pairs, 64):
                        k = min(64, pairs - t)
                        out.append(0x40 | (k - 1))
                        out.append(idx)
                    i += 2 * pairs
                    continue
            run = 0
            while i + run < w and pixels[base + i + run] == (idx, 255):
                run += 1
            if run >= 2:
                for t in range(0, run, 64):
                    k = min(64, run - t)
                    out.append(k - 1)
                    out.append(idx)
                i += run
                continue
            # 字面：连续不透明段；段内 >=3 同色游程处断开（实心 2 字节
            # 覆盖 r>=3 优于字面 r 字节）
            run = 0
            while i + run < w and pixels[base + i + run][1] == 255:
                run += 1
            cut = run
            j = 0
            while j < cut:
                v2 = pixels[base + i + j][0]
                s2 = 1
                while (j + s2 < cut
                       and pixels[base + i + j + s2] == (v2, 255)):
                    s2 += 1
                if s2 >= 3:
                    cut = j
                    break
                j += s2 if s2 > 1 else 1
            for t in range(0, cut, 64):
                k = min(64, cut - t)
                out.append(0x80 | (k - 1))
                out.extend(pixels[base + i + t + j2][0] for j2 in range(k))
            i += cut
    return bytes(out)


def encode_rle4_linear(pixels: Pixels, w: int, h: int,
                       skip_fill: int = 0) -> bytes:
    """线性 4-op 贪婪编码（tile4 逆；无行复位）。"""
    out = bytearray()
    total = w * h
    px_i = 0
    while px_i < total:
        idx, alpha = pixels[px_i]
        if alpha == 0:
            run = 0
            while (px_i + run < total and pixels[px_i + run][1] == 0
                   and pixels[px_i + run][0] == skip_fill):
                run += 1
            if run == 0:
                run = 1
            for t in range(0, run, 64):
                k = min(64, run - t)
                out.append(0xC0 | (k - 1))
            px_i += run
            continue
        if px_i + 1 < total and pixels[px_i + 1] == (idx, 0):
            # 隔点 (已写, 空洞) 对——空洞索引须精确吻合
            v = idx
            pairs = 0
            while (px_i + 2 * pairs + 1 < total
                   and pixels[px_i + 2 * pairs] == (v, 255)
                   and pixels[px_i + 2 * pairs + 1] == (v, 0)):
                pairs += 1
            if pairs >= 1:
                for t in range(0, pairs, 64):
                    k = min(64, pairs - t)
                    out.append(0x40 | (k - 1))
                    out.append(v)
                px_i += 2 * pairs
                continue
        run = 0
        while px_i + run < total and pixels[px_i + run] == (idx, 255):
            run += 1
        if run >= 2:
            for t in range(0, run, 64):
                k = min(64, run - t)
                out.append(k - 1)
                out.append(idx)
            px_i += run
            continue
        run = 0
        while px_i + run < total and pixels[px_i + run][1] == 255:
            run += 1
        for t in range(0, run, 64):
            k = min(64, run - t)
            out.append(0x80 | (k - 1))
            out.extend(pixels[px_i + t + j][0] for j in range(k))
        px_i += run
    return bytes(out)


# ------------------------------------------------------------- sprite24

def decode_sprite24(stream: bytes, transparent: bool,
                    w: int = 24, h: int = 24) -> Tuple[Optional[Pixels], int]:
    """sprite24（video.c sprite24_stamp[_transparent]）。op11：透明变体跳过
    （记 idx=0x49,alpha=0 供复现），不透明变体写 0x49（同样记 (0x49,0)，
    variant 字段决定渲染语义）。op01 写奇数位。"""
    total = w * h
    out: Pixels = []
    i = 0
    n = len(stream)
    for _row in range(h):
        col = 0
        while col < w:
            if i >= n:
                return None, 0
            c = stream[i]
            i += 1
            cnt = (c & 0x3F) + 1
            kind = (c >> 6) & 3
            if kind == 0:
                if i >= n:
                    return None, 0
                v = stream[i]
                i += 1
                out.extend([(v, 255)] * cnt)
                col += cnt
            elif kind == 1:
                if i >= n:
                    return None, 0
                v = stream[i]
                i += 1
                for _k in range(cnt):
                    out.append((v, 0))
                    out.append((v, 255))
                col += 2 * cnt
            elif kind == 2:
                for _k in range(cnt):
                    if i >= n:
                        return None, 0
                    out.append((stream[i], 255))
                    i += 1
                col += cnt
            else:
                alpha = 0 if transparent else 255
                out.extend([(SPRITE24_FILL, alpha)] * cnt)
                col += cnt
            if col > w:
                return None, 0
    if len(out) != total:
        return None, 0
    return out, i


def encode_sprite24(pixels: Pixels, w: int = 24, h: int = 24) -> bytes:
    """sprite24 贪婪编码：op11 记 alpha=0 -> 还原跳过；隔点/实心/字面同 4-op。"""
    out = bytearray()
    for row in range(h):
        base = row * w
        col = 0
        while col < w:
            idx, alpha = pixels[base + col]
            if alpha == 0 and idx == SPRITE24_FILL:
                run = 0
                while (col + run < w
                       and pixels[base + col + run] == (SPRITE24_FILL, 0)):
                    run += 1
                for t in range(0, run, 64):
                    k = min(64, run - t)
                    out.append(0xC0 | (k - 1))
                col += run
                continue
            # 隔点：sprite24 写奇数位（空洞在前）——扫描先遇空洞位
            if (col + 1 < w and alpha == 0 and idx != SPRITE24_FILL
                    and pixels[base + col + 1] == (idx, 255)):
                v = idx
                pairs = 0
                while (col + 2 * pairs + 1 < w
                       and pixels[base + col + 2 * pairs + 1] == (v, 255)
                       and pixels[base + col + 2 * pairs][1] == 0
                       and pixels[base + col + 2 * pairs][0] == v):
                    pairs += 1
                if pairs >= 1:
                    for t in range(0, pairs, 64):
                        k = min(64, pairs - t)
                        out.append(0x40 | (k - 1))
                        out.append(v)
                    col += 2 * pairs
                    continue
            if (col + 1 < w and alpha == 255
                    and pixels[base + col + 1] == (idx, 0)):
                # 隔点（从已写位起，防御性）
                v = idx
                pairs = 0
                while (col + 2 * pairs + 1 < w
                       and pixels[base + col + 2 * pairs] == (v, 255)
                       and pixels[base + col + 2 * pairs + 1] == (v, 0)):
                    pairs += 1
                if pairs >= 1:
                    for t in range(0, pairs, 64):
                        k = min(64, pairs - t)
                        out.append(0x40 | (k - 1))
                        out.append(v)
                    col += 2 * pairs
                    continue
            run = 0
            while (col + run < w
                   and pixels[base + col + run] == (idx, 255)):
                run += 1
            if run >= 2:
                for t in range(0, run, 64):
                    k = min(64, run - t)
                    out.append(k - 1)
                    out.append(idx)
                col += run
                continue
            run = 0
            while col + run < w and pixels[base + col + run][1] == 255:
                run += 1
            for t in range(0, run, 64):
                k = min(64, run - t)
                out.append(0x80 | (k - 1))
                out.extend(pixels[base + col + t + j][0] for j in range(k))
            col += run
    return bytes(out)


# ------------------------------------------------------------------- RAW

def decode_raw(stream: bytes, w: int, h: int, transparent_zero: bool,
               off: int = 0) -> Tuple[Optional[Pixels], int]:
    """RAW 帧（0x4ED34 透明 / 0x4ED0B 不透明）：w*h 逐像素字节。
    transparent_zero=True -> 0 = 流级透明（渲染跳过）记 alpha=0；
    False -> 全 alpha=255。"""
    total = w * h
    if off + total > len(stream):
        return None, 0
    out: Pixels = []
    for k in range(total):
        v = stream[off + k]
        out.append((v, 0 if (transparent_zero and v == 0) else 255))
    return out, off + total


# ------------------------------------------------------- 视图探测/重编码

def try_decode_views(stream: bytes, w: int, h: int,
                     linear: bool = False):
    """对一段帧体尝试各编解码视图，返回 [(name, pixels, consumed)]。
    byte-RLE/RAW 恒全展开（consumed 区分），4-op 区分行/线性。"""
    views = []
    p, c = decode_byte_rle(stream, w, h)
    if p is not None:
        views.append(("rle8", p, c))
    p, c = (decode_rle4_linear if linear else decode_rle4_rows)(stream, w, h)
    if p is not None:
        views.append(("rle4_linear" if linear else "rle4", p, c))
    if len(stream) >= w * h:
        p, c = decode_raw(stream, w, h, True)
        if p is not None:
            views.append(("raw_t", p, w * h))
        p, c = decode_raw(stream, w, h, False)
        if p is not None:
            views.append(("raw_o", p, w * h))
    return views


def reencode(codec: str, pixels: Pixels, w: int, h: int,
             strategy: str = "front63") -> bytes:
    if codec == "rle8":
        return encode_byte_rle(pixels, strategy)
    if codec == "rle4":
        return encode_rle4_rows(pixels, w, h)
    if codec == "rle4_linear":
        return encode_rle4_linear(pixels, w, h)
    if codec == "sprite24":
        return encode_sprite24(pixels, w, h)
    raise ValueError(f"codec {codec} 不可从像素重编码")
