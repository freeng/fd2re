# -*- coding: utf-8 -*-
"""FD2 资源容器与块种类分类（fd2dat）

容器定案（docs/architecture.md §5/§6.6/§13.28/§13.33/§13.47/§13.77）：
- LLLLLL DAT：6B 魔数 + u32 starts[]（含尾哨兵=文件长），块 = starts[i]..[i+1]，
  目录自 6 起、块体紧排（starts[0] = 6+4*条目数）。
- FDICON.B24：[u16 24][u16 24][u16 条目数] + u32 绝对偏移表@6 + sprite24 帧
  流紧排（140 图标 × 12 帧，text.c icon_load_entry 0x11019）。
- 嵌套 LLLLLL 音效包（FDOTHER blk31/77/78）：6B 魔数 + u32 偏移表@6，
  条目 = 8bit 无符号 PCM @11025Hz（audio.c sfx_trigger）。
- 嵌套 LLLLLL 图像包（FDOTHER blk7 ≡ TITLE.DAT，字节相同已验证）：子块 =
  单帧 4-op RLE 图。
- ANI.DAT：LLLLLL 容器，块 = 173B 动画头（i16 帧数@165）+ 逐帧
  [u16 数据长][u16 操作数][4B 保留] + 操作码流（duel.c ani_play 0x20421）。

分类器为纯结构探测（无消费者假设），个别需要消费方知识的块走 OVERRIDES
（附证据出处）。全部规则与 fd2re/src/*.c 的逆向定案一一对应。
"""
from __future__ import annotations

import struct
from typing import Dict, List, Optional, Tuple

from . import codec

LLL = b"LLLLLL"


def parse_starts(data: bytes) -> List[int]:
    """目录 = 自 6 起的 u32 单调序列，末项 = 文件长（哨兵）。"""
    if len(data) < 10 or data[:6] != LLL:
        raise ValueError("not a LLLLLL container")
    starts: List[int] = []
    cur = 6
    while cur + 4 <= len(data):
        v = struct.unpack_from("<I", data, cur)[0]
        if not starts:
            if v <= 6 or v > len(data):
                break
        elif v < starts[-1] or v > len(data):
            break
        starts.append(v)
        cur += 4
    if len(starts) < 2 or starts[-1] != len(data):
        raise ValueError("bad LLLLLL directory")
    return starts


def container_blocks(data: bytes) -> List[bytes]:
    starts = parse_starts(data)
    return [data[starts[i]:starts[i + 1]] for i in range(len(starts) - 1)]


def build_container(blocks: List[bytes]) -> bytes:
    starts = [6 + 4 * (len(blocks) + 1)]
    for b in blocks:
        starts.append(starts[-1] + len(b))
    out = bytearray(LLL)
    for s in starts:
        out += struct.pack("<I", s)
    for b in blocks:
        out += b
    return bytes(out)


# ------------------------------------------------------------------ 分类

def _valid_lmi1(b: bytes) -> Optional[int]:
    if len(b) < 12 or b[:4] != b"LMI1":
        return None
    n = struct.unpack_from("<H", b, 4)[0]
    if n < 1 or 6 + 4 * (n + 1) > len(b):
        return None
    offs = [struct.unpack_from("<I", b, 6 + 4 * i)[0] for i in range(n + 1)]
    if offs[0] != 6 + 4 * (n + 1) or offs[-1] != len(b):
        return None
    if any(offs[i] >= offs[i + 1] for i in range(n)):
        return None
    return n


def _valid_wh(b: bytes) -> dict:
    """帧表动画包（FIGANI 3i 姿态/动画、FDOTHER WH 族；blit_frame_flat 族
    消费——表@+8 与头无关；fx_scene_play 读 tpose[0]、ending_anim_part2 读
    ftab[2]）：
      头 [u8 count][u8 flag][u16 played][u32 param]
      + u32 off[count] @8（off[0]=8+4*count，单调）
      + 帧 [x2][y2][A2][b1][mode1][0 1][w2][h2][4-op rows]（流@13）。
    经典 WH 族 = flag=0 / played=count / param=0 的特例。"""
    if len(b) < 14:
        return None
    n = b[0]
    if n < 1 or 8 + 4 * n > len(b):
        return None
    offs = [struct.unpack_from("<I", b, 8 + 4 * i)[0] for i in range(n)]
    if offs[0] != 8 + 4 * n:
        return None
    if any(offs[i] >= offs[i + 1] for i in range(n - 1)):
        return None
    if offs[-1] > len(b):
        return None
    for i in range(n):
        end = offs[i + 1] if i + 1 < n else len(b)
        f = b[offs[i]:end]
        if len(f) < 15 or f[8] != 0:
            return None
        w = f[9] | (f[10] << 8)
        h = f[11] | (f[12] << 8)
        if not (1 <= w <= 640 and 1 <= h <= 400):
            return None
        px, _ = codec.decode_rle4_rows(f[13:], w, h)
        if px is None:
            return None
    return {"count": n, "flag": b[1],
            "played": struct.unpack_from("<H", b, 2)[0],
            "param": struct.unpack_from("<I", b, 4)[0]}


def _valid_dato(b: bytes) -> bool:
    """DATO 块：u32 目录 ×4 @0（dir[0]=16）+ 4 帧 80×80 字节 RLE
    （architecture.md §13.33：137 块 × 4 帧 80×80，dir[0]=16）。"""
    if len(b) < 20:
        return False
    offs = [struct.unpack_from("<I", b, 4 * i)[0] for i in range(4)]
    if offs[0] != 16 or any(offs[i] >= offs[i + 1] for i in range(3)):
        return False
    if offs[3] > len(b):
        return False
    for i in range(4):
        end = offs[i + 1] if i < 3 else len(b)
        w, h = struct.unpack_from("<HH", b, offs[i])
        if (w, h) != (80, 80):
            return False
        px, _ = codec.decode_byte_rle(b[offs[i] + 4:end], w, h)
        if px is None:
            return False
    return True


def _valid_tiles(b: bytes) -> bool:
    """FDSHAP tile 块（§6.7）：u16 w+h+cnt + u32 绝对偏移表@6。"""
    if len(b) < 10:
        return False
    w, h, n = struct.unpack_from("<HHH", b)
    if not (1 <= w <= 64 and 1 <= h <= 64 and 1 <= n <= 4096):
        return False
    if 6 + 4 * n > len(b):
        return False
    offs = [struct.unpack_from("<I", b, 6 + 4 * i)[0] for i in range(n)]
    if offs[0] != 6 + 4 * n:
        return False
    for i in range(n):
        end = offs[i + 1] if i < n - 1 else len(b)
        px, _ = codec.decode_rle4_linear(b[offs[i]:end], w, h)
        if px is None:
            return False
    return True


def _valid_field_map(b: bytes) -> bool:
    if len(b) < 8:
        return False
    w, h = struct.unpack_from("<hh", b)
    return 1 <= w <= 256 and 1 <= h <= 256 and len(b) == 4 + 4 * w * h


def _valid_battle_ctx(b: bytes) -> bool:
    """FDFIELD 伴块（§6.9）：L = 131 + 26F（126B 头 + 26B×F + 尾 5B），
    F = ctx[2]（state0 为 ctx[2]+1，挂号）。"""
    if len(b) < 131 or len(b) % 26 != 1:
        return False
    f = (len(b) - 131) // 26
    e = b[2]
    return f == e or f == e + 1


def _valid_deploy(b: bytes) -> bool:
    """布阵层（§6.9）：u16 头 = P+E；长 = 2 + 6×(P+E)。"""
    if len(b) < 8 or len(b) % 6 != 2:
        return False
    head = struct.unpack_from("<H", b)[0]
    return head > 0 and len(b) == 2 + 6 * head


def _valid_text(b: bytes) -> bool:
    """FDTXT 块（§13.43 附带定案）：u16 串偏移表 + u16 token 流（-1 终止）。"""
    if len(b) < 4:
        return False
    first = struct.unpack_from("<H", b)[0]
    if first < 2 or first % 2 or first > len(b):
        return False
    prev = -1
    k = 0
    while k + 2 <= first:
        v = struct.unpack_from("<H", b, k)[0]
        if v < prev:            # 偏移表须非降
            return False
        prev = v
        k += 2
    # 首串必须 -1 终止
    i = first
    steps = 0
    while i + 2 <= len(b) and steps < 0x20000:
        t = struct.unpack_from("<h", b, i)[0]
        i += 2
        steps += 1
        if t == -1:
            return True
        if t in (-17, -18, -19, -20, -4, -5):
            i += 2
    return False


def _valid_image_raw(b: bytes) -> bool:
    """[u16 w][u16 h] + 裸像素恰满（FDOTHER blk15/55 = 320×200 RAW 图）。"""
    if len(b) < 8:
        return False
    w, h = struct.unpack_from("<HH", b)
    return 1 <= w <= 640 and 1 <= h <= 400 and len(b) == 4 + w * h


def _valid_sprite_pkg(b: bytes) -> bool:
    """[u16 w][u16 h][u16 cnt] + u32 off[]（cnt 或 cnt+1 项）+ sprite24 帧
    （FDOTHER blk1 标记 / blk96；FDICON.B24 同族特例走专用导出）。"""
    if len(b) < 14:
        return False
    w, h, cnt = struct.unpack_from("<HHH", b)
    if not (1 <= w <= 64 and 1 <= h <= 64 and 1 <= cnt <= 4096):
        return False
    entries_a = 6 + 4 * cnt          # 无哨兵
    entries_b = 6 + 4 * (cnt + 1)    # 带哨兵
    for table_end in (entries_a, entries_b):
        if table_end + 4 <= len(b) or (table_end == len(b) and cnt > 1):
            pass
    for table_end in (entries_a, entries_b):
        if table_end > len(b):
            continue
        n = (table_end - 6) // 4
        offs = [struct.unpack_from("<I", b, 6 + 4 * i)[0] for i in range(n)]
        if offs[0] != table_end:
            continue
        if any(offs[i] >= offs[i + 1] for i in range(n - 1)):
            continue
        # 首帧必须按 sprite24 恰耗尽
        end = offs[1] if n > 1 else len(b)
        px, _ = codec.decode_sprite24(b[offs[0]:end], True, w, h)
        if px is not None:
            return True
    return False


def _valid_image(b: bytes) -> bool:
    """单帧图：[u16 w][u16 h] + 逐行 4-op RLE 严格耗尽（BG/TAI/TITLE）
    或字节 RLE 严格耗尽（FDOTHER blk10 覆盖层，stamp_frame_opaque
    0x4EBFF 不透明消费——流格式与 pkg_frame_blit 族同）。"""
    if len(b) < 6:
        return False
    w, h = struct.unpack_from("<HH", b)
    if not (1 <= w <= 640 and 1 <= h <= 400):
        return False
    px, _ = codec.decode_rle4_rows(b[4:], w, h)
    if px is not None:
        return True
    px, _ = codec.decode_byte_rle(b[4:], w, h)
    return px is not None


def _valid_anim(b: bytes) -> bool:
    """ANI.DAT 块（§13.4）：173B 头 + 逐帧 8B 记录+数据，恰好走到块尾。"""
    if len(b) < 173 + 8:
        return False
    n = struct.unpack_from("<h", b, 165)[0]
    if n < 1 or n > 5000:
        return False
    pos = 173
    for _ in range(n):
        if pos + 8 > len(b):
            return False
        size, _ops = struct.unpack_from("<HH", b, pos)
        pos += 8 + size
    return pos == len(b)


def _valid_raw_frame_table(b: bytes) -> bool:
    """FDOTHER blk2 型（battle.c blk2_frame：目录在块首 u32[]，78 个
    24×20 字面帧）：条目 = [u16 w][u16 h] + 裸像素，恰填目录间距。"""
    if len(b) < 12:
        return False
    first = struct.unpack_from("<I", b)[0]
    if first < 8 or first % 4 or first > len(b):
        return False
    n = first // 4
    offs = [struct.unpack_from("<I", b, 4 * i)[0] for i in range(n)]
    if any(offs[i] >= offs[i + 1] for i in range(n - 1)):
        return False
    for i in range(n):
        end = offs[i + 1] if i < n - 1 else len(b)
        w, h = struct.unpack_from("<HH", b, offs[i])
        if 4 + w * h != end - offs[i] or not (1 <= w <= 320 and 1 <= h <= 200):
            return False
    return True


def _classify_nested(b: bytes) -> str:
    """嵌套 LLLLLL（音效包 / 图像包）：按子块能否分类为单帧图区分。"""
    starts = parse_starts(b)
    subs = [b[starts[i]:starts[i + 1]] for i in range(len(starts) - 1)]
    if subs and all(_valid_image(s) for s in subs):
        return "image_pkg"
    return "sfx_pkg"


def classify_block(b: bytes, dat: str = "", block_id: int = -1) -> str:
    ov = OVERRIDES.get(dat, {}).get(block_id)
    if ov:
        return ov
    if len(b) == 768:
        return "palette"
    if len(b) > 16 and b[:4] == b"FORM" and b[8:12] in (b"XDIR", b"XMID"):
        return "music"
    if len(b) == 3:
        return "music_placeholder" if dat == "FDMUS.DAT" else "anim_empty"
    if _valid_lmi1(b) is not None:
        return "lmi1"
    if dat == "FDSHAP.DAT" and _valid_tiles(b):
        return "tiles"
    if _valid_sprite_pkg(b):
        return "sprite_pkg"
    if _valid_wh(b) is not None:
        return "wh_anim"
    if _valid_dato(b):
        return "dato"
    if _valid_anim(b):
        return "anim"
    if _valid_tiles(b):
        return "tiles"
    if _valid_field_map(b):
        return "field_map"
    if _valid_battle_ctx(b):
        return "battle_ctx"
    if _valid_deploy(b):
        return "deploy"
    if _valid_text(b):
        return "text"
    if _valid_raw_frame_table(b):
        return "raw_frames"
    if _valid_image(b):
        return "image"
    if _valid_image_raw(b):
        return "image_raw"
    if b[:6] == LLL:
        try:
            return _classify_nested(b)
        except ValueError:
            pass
    return "unknown"


# 需要消费方知识的块（结构探测无法唯一确定），附证据：
# - FDOTHER blk4 = 16×16 点阵字形库：text.c glyph_blit(0x4EDC2) 以
#   32B/字形直接消费 g_pkg_fdother_4（58368 = 1824×32 精确吻合）。
OVERRIDES: Dict[str, Dict[int, str]] = {
    "FDOTHER.DAT": {4: "font"},
}


def census(data: bytes, dat: str) -> Dict[str, List[int]]:
    kinds: Dict[str, List[int]] = {}
    for i, b in enumerate(container_blocks(data)):
        kinds.setdefault(classify_block(b, dat, i), []).append(i)
    return kinds
