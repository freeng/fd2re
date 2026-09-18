# -*- coding: utf-8 -*-
"""XMIDI (FDMUS.DAT 块) <-> 标准 MIDI + 事件 JSON（fd2dat）

EVNT 语法（architecture.md §7.1 静态定案，15 曲全量解析自洽）：
- delta = 连续 <0x80 字节相加（0 省略），首个 >=0x80 字节即 status；
- note-on = status+note+vel+VLQ(duration)（AIL 32 槽 note 队列自动 note-off），
  曲目无显式 0x8n；
- VLQ = 标准 BE 7bit 组（AIL 0x424B0）；
- 时基恒 1 tick = 1/120 s（泵 120Hz），FF 51 不进事件速率。

现代格式 = .mid（division=60 + tempo 500000μs 恰为 120 tick/s，可直接进
DAW / libADLMIDI）+ .events.json（保留原 delta/duration 字节与 FF 51 原值，
pack 无损重建 XMIDI；字段可编辑，缺省字节由语义字段重新生成）。
"""
from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SMF_DIVISION = 60
SMF_TEMPO_US = 500000          # 60 PPQN × 0.5s/quarter = 120 tick/s


def _read_vlq(d: bytes, p: int) -> Tuple[int, int]:
    v = 0
    while True:
        b = d[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, p


def _write_vlq(v: int) -> bytes:
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def parse_xmidi(block: bytes) -> dict:
    """解析 FORM XDIRINFO / CAT XMID{FORM XMID{TIMB,RBRN?,EVNT}}。"""
    def iff(off: int) -> Tuple[str, int, int]:
        tag = block[off:off + 4]
        ln = struct.unpack_from(">I", block, off + 4)[0]
        return tag.decode("ascii", "replace"), off + 8, ln

    tag, off, _ln = iff(0)
    if tag != "FORM":
        raise ValueError("XMIDI: no FORM")
    ftype = block[off:off + 8]
    off += 8
    if ftype != b"XDIRINFO":
        raise ValueError(f"XMIDI: bad form type {ftype!r}")
    # XDIRINFO 载荷 = u32 BE 长度 + 数据（实测长度 2，数据 "01 00"）
    info_len = struct.unpack_from(">I", block, off)[0]
    info = block[off + 4:off + 4 + info_len]
    off += 4 + info_len
    # CAT XMID
    tag, off, _ln = iff(off)
    if tag != "CAT " or block[off:off + 4] != b"XMID":
        raise ValueError("XMIDI: no CAT XMID")
    off += 4
    tag, off, _ln = iff(off)
    if tag != "FORM" or block[off:off + 4] != b"XMID":
        raise ValueError("XMIDI: no FORM XMID")
    off += 4
    timb: Optional[bytes] = None
    rbrn: Optional[bytes] = None
    evnt: Optional[bytes] = None
    while off + 8 <= len(block):
        tag, off, ln = iff(off)
        body = block[off:off + ln]
        if tag == "TIMB":
            timb = body
        elif tag == "RBRN":
            rbrn = body
        elif tag == "EVNT":
            evnt = body
        off += ln + (ln & 1)
    if evnt is None:
        raise ValueError("XMIDI: no EVNT chunk")
    return {"info": info, "timb": timb, "rbrn": rbrn, "evnt": evnt}


def decode_evnt(evnt: bytes) -> List[dict]:
    """EVNT -> 事件列表（含原始 delta / duration 字节，供无损回包）。"""
    evs: List[dict] = []
    p = 0
    n = len(evnt)
    while p < n:
        delta = 0
        start = p
        while p < n and evnt[p] < 0x80:
            delta += evnt[p]
            p += 1
        if p >= n:
            break                    # 截断流：已解析事件有效
        status = evnt[p]
        p += 1
        ev = {"dh": evnt[start:p - 1].hex(), "t": delta, "st": status}
        if status == 0xFF:
            mtype = evnt[p]
            p += 1
            ln, p = _read_vlq(evnt, p)
            ev["mt"] = mtype
            ev["data"] = evnt[p:p + ln].hex()
            p += ln
        elif status < 0x80:
            raise ValueError(f"EVNT: stray status {status:#x} @ {p}")
        elif (status & 0xF0) == 0x90:
            note, vel = evnt[p], evnt[p + 1]
            p += 2
            ds, p2 = _read_vlq(evnt, p)
            ev.update({"d1": note, "d2": vel, "dur": ds,
                       "vh": evnt[p:p2].hex()})
            p = p2
        elif (status & 0xF0) in (0x80, 0xA0, 0xB0, 0xE0):
            ev.update({"d1": evnt[p], "d2": evnt[p + 1]})
            p += 2
        elif (status & 0xF0) in (0xC0, 0xD0):
            ev["d1"] = evnt[p]
            p += 1
        elif status in (0xF0, 0xF7):
            ln, p = _read_vlq(evnt, p)
            ev["data"] = evnt[p:p + ln].hex()
            p += ln
        else:
            raise ValueError(f"EVNT: bad status {status:#x} @ {p}")
        evs.append(ev)
        if status == 0xFF and ev.get("mt") == 0x2F:
            break                    # EOT：尾部可能有填充字节
    return evs


def build_evnt(evs: List[dict]) -> bytes:
    out = bytearray()
    for ev in evs:
        if "dh" in ev:
            out += bytes.fromhex(ev["dh"])
        else:
            t = max(0, int(ev.get("t", 0)))
            # 原语法：delta = <0x80 字节之和；>127 拆多字节
            while t > 127:
                out.append(127)
                t -= 127
            out.append(t)
        st = ev["st"]
        out.append(st)
        if st == 0xFF:
            out.append(ev["mt"])
            data = bytes.fromhex(ev["data"])
            out += _write_vlq(len(data))
            out += data
        elif (st & 0xF0) == 0x90:
            out += bytes([ev["d1"], ev["d2"]])
            if "vh" in ev:
                out += bytes.fromhex(ev["vh"])
            else:
                out += _write_vlq(int(ev.get("dur", 0)))
        elif (st & 0xF0) in (0x80, 0xA0, 0xB0, 0xE0):
            out += bytes([ev["d1"], ev["d2"]])
        elif (st & 0xF0) in (0xC0, 0xD0):
            out.append(ev["d1"])
        elif st in (0xF0, 0xF7):
            data = bytes.fromhex(ev["data"])
            out += _write_vlq(len(data))
            out += data
    return bytes(out)


def _chunk(tag: bytes, body: bytes) -> bytes:
    """AIL 写法：odd 体补 pad，长度字段含 pad（FDMUS 实测）。"""
    ln = len(body) + (len(body) & 1)
    return tag + struct.pack(">I", ln) + body + (b"\x00" if len(body) & 1 else b"")

def build_xmidi(doc: dict) -> bytes:
    """events JSON 文档（含 info/timb/rbrn 原始 hex）-> XMIDI 字节。"""
    evnt = build_evnt(doc["events"])
    out = bytearray()
    info = bytes.fromhex(doc["info_hex"])
    out += _chunk(b"FORM", b"XDIRINFO" + struct.pack(">I", len(info)) + info)
    body = _chunk(b"FORM", b"XMID" + _chunk(b"TIMB", bytes.fromhex(doc["timb_hex"]))
                  + (b"" if doc.get("rbrn_hex") is None
                     else _chunk(b"RBRN", bytes.fromhex(doc["rbrn_hex"])))
                  + _chunk(b"EVNT", evnt))
    out += _chunk(b"CAT ", b"XMID" + body)
    return bytes(out)


def export_music(block: bytes, json_path: Path, mid_path: Path) -> dict:
    parsed = parse_xmidi(block)
    evs = decode_evnt(parsed["evnt"])
    timb_pairs = []
    if parsed["timb"]:
        t = parsed["timb"]
        for i in range(0, len(t) - 1, 2):
            if t[i] == 0xFF and t[i + 1] == 0xFF:
                break
            timb_pairs.append({"num": t[i], "page": t[i + 1]})
    # 原始 FF 51（不进 SMF，恒 120tick/s 语义）
    orig_tempo = next((int(e["data"], 16) for e in evs
                       if e.get("st") == 0xFF and e.get("mt") == 0x51), None)
    doc = {
        "schema": "fd2dat.xmidi/1",
        "info_hex": parsed["info"].hex(),
        "timb_hex": (parsed["timb"] or b"").hex(),
        "rbrn_hex": parsed["rbrn"].hex() if parsed["rbrn"] else None,
        "timb": timb_pairs,
        "tempo_meta_original": orig_tempo,
        "tick_rate_per_sec": 120,
        "events": evs,
    }
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    write_smf(evs, mid_path)
    return {"events": len(evs), "timb": len(timb_pairs)}


def write_smf(evs: List[dict], path: Path) -> None:
    """事件 -> format-1 SMF。track0 = 120tick/s tempo；track1 = 演奏事件。"""
    # 绝对时间展开（dh 累计），note-on 按 vh/dur 排 note-off
    t = 0
    abs_ev: List[Tuple[int, int, bytes]] = []   # (tick, order, bytes)
    note_offs: List[Tuple[int, int, bytes]] = []
    order = 0
    for ev in evs:
        t += ev.get("t", 0)
        st = ev["st"]
        if st == 0xFF:
            if ev["mt"] == 0x51:      # 原 FF51 不进 SMF（见模块注释）
                continue
            body = bytes([ev["mt"]]) + _write_vlq(len(bytes.fromhex(ev["data"]))) \
                + bytes.fromhex(ev["data"])
            abs_ev.append((t, order, b"\xff" + body))
        elif (st & 0xF0) == 0x90:
            abs_ev.append((t, order, bytes([st, ev["d1"], ev["d2"]])))
            dur = ev.get("dur", 0)
            note_offs.append((t + dur, order,
                              bytes([0x80 | (st & 0x0F), ev["d1"], 0])))
        elif (st & 0xF0) in (0x80, 0xA0, 0xB0, 0xE0):
            abs_ev.append((t, order, bytes([st, ev["d1"], ev["d2"]])))
        elif (st & 0xF0) in (0xC0, 0xD0):
            abs_ev.append((t, order, bytes([st, ev["d1"]])))
        elif st in (0xF0, 0xF7):
            data = bytes.fromhex(ev["data"])
            abs_ev.append((t, order, bytes([st]) + _write_vlq(len(data)) + data))
        order += 1
    all_ev = sorted(abs_ev + note_offs, key=lambda e: (e[0], e[1]))

    def track(events: List[Tuple[int, int, bytes]]) -> bytes:
        out = bytearray()
        last = 0
        for tick, _o, data in events:
            out += _write_vlq(tick - last)
            out += data
            last = tick
        out += _write_vlq(0) + b"\xff\x2f\x00"
        return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)

    tempo_track = track([(0, 0, b"\xff\x51\x03" + struct.pack(">I", SMF_TEMPO_US)[1:])])
    body = struct.pack(">HHH", 1, 2, SMF_DIVISION) + tempo_track + track(all_ev)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"MThd" + struct.pack(">I", 6) + body)


def load_music_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))
