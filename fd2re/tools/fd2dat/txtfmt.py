# -*- coding: utf-8 -*-
"""FDTXT.DAT 文本块 <-> JSON + 可读 .txt（fd2dat）

块 = u16 串偏移表（条数 = 首偏移/2，starts[0] 即首块数据偏移 146 — 无计数
字段）+ u16 token 流（-1 终止）。token 语义（text.c render_tokens 0x161F9
全分支，architecture.md §13.43/§13.34）：
  -1 串尾  -2 换行  -3 翻页  -4/-5 嵌套串（u16 id，-4 用 g_disp_num_a、
  -5 用 g_disp_num_c 的数字寄存器再递归）  -6 数字寄存器
  -17/-19 开顶窗+说话人（u16 参数）  -18/-20 开底窗+说话人
  >=0  字形 id（FDOTHER blk4 16×16 字库，10=空格）
"""
from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import List

ARG_TOKENS = (-17, -18, -19, -20, -4, -5)

TOKEN_NAMES = {
    -1: "END", -2: "NL", -3: "PG", -6: "NUM",
    -4: "NEST_A", -5: "NEST_C",
    -17: "TOP_ROSTER", -18: "BOT_ROSTER",
    -19: "TOP_ENT", -20: "BOT_ENT",
}


def parse_strings(block: bytes) -> List[List[dict]]:
    first = struct.unpack_from("<H", block)[0]
    count = first // 2
    offs = [struct.unpack_from("<H", block, 2 * i)[0] for i in range(count)]
    out: List[List[dict]] = []
    for off in offs:
        toks: List[dict] = []
        i = off
        while i + 2 <= len(block):
            t = struct.unpack_from("<h", block, i)[0]
            i += 2
            if t == -1:
                break
            if t in ARG_TOKENS:
                arg = struct.unpack_from("<H", block, i)[0]
                i += 2
                toks.append({"t": t, "arg": arg})
            else:
                toks.append({"t": t})
        out.append(toks)
    return out


def token_to_text(tok: dict) -> str:
    t = tok["t"]
    if t >= 0:
        if t == 10:
            return " "
        if t < 10:
            return chr(ord("0") + t)     # 数字字形 0..9（fdtxt_token_scan 定案）
        return f"<g{t}>"
    name = TOKEN_NAMES.get(t, f"<{t}>")
    if "arg" in tok:
        return f"[{name}({tok['arg']})]"
    return f"[{name}]"


def export_block(block: bytes, json_path: Path, txt_path: Path) -> dict:
    strings = parse_strings(block)
    doc = {"schema": "fd2dat.fdtxt/1", "string_count": len(strings),
           "strings": [{"id": i, "tokens": s} for i, s in enumerate(strings)]}
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n",
                         encoding="utf-8")
    lines = []
    for i, s in enumerate(strings):
        lines.append(f"str {i}: " + "".join(token_to_text(t) for t in s))
    txt_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"strings": len(strings)}


def pack_block(doc: dict) -> bytes:
    strings = doc["strings"]
    n = len(strings)
    body = bytearray()
    offs = []
    for s in strings:
        offs.append(len(body))
        for tok in s["tokens"]:
            body += struct.pack("<h", tok["t"])
            if tok["t"] in ARG_TOKENS:
                body += struct.pack("<H", tok["arg"])
        body += struct.pack("<h", -1)
    # 偏移表补齐到 first 偏移（原表条数 = 首偏移/2；fd2re text.c 按串 id
    # 直接索引，导出时条数即串数）
    out = bytearray()
    table_len = 2 * n
    for o in offs:
        out += struct.pack("<H", table_len + o)
    out += body
    return bytes(out)
