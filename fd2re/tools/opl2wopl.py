#!/usr/bin/env python3
"""SAMPLE.OPL (AIL 3.x 运行时 FM 音色库) -> WOPL v3 (libADLMIDI)

格式定案依据（2026-09-08，全部静态取证）：
- 加载哪个库：SBPRO2.MDI 描述符含扩展名串 "OPL"（ADLIB/SBPRO1/SBLASTER 为
  "AD"），AIL 以 "SAMPLE"+扩展名打开运行时库；SAMPLE.AD 与 SAMPLE.OPL 字节
  相同。SAMPLE.BNK（VC 名字母库）不参与运行时。
- .OPL 结构：目录项 (u8 num, u8 page, u32 off) 至 FF FF；page 0=旋律 patch
  号，0x7F=打击乐 GM 音符号。timbre 记录 14 字节：
    u16 len(=14) + i8 notenum + mod{amvib20,ksl40,ardr60,slrr80,wfE0}
    + u8 feedconn(C0) + car{...同 mod}
- 字段映射以 libADLMIDI 自带 MonopolyDeluxe.ad/.wopl 原生对 (John Miles AIL
  银行) 逐字节对拍定案：mod->ops[1], car->ops[0], feedconn->fb_conn1,
  旋律 notenum->note_offset1, 打击 notenum->percussion_key_number,
  volume_model=WOPL_VM_AIL(7)。
"""
import struct
import sys

def parse_opl(path):
    data = open(path, "rb").read()
    a = 0
    while a * 6 + 6 <= len(data):
        num, page = data[a*6], data[a*6+1]
        off, = struct.unpack_from("<I", data, a*6+2)
        if num == 0xFF and page == 0xFF:
            break
        ln, = struct.unpack_from("<H", data, off)
        assert ln == 14, f"unexpected timbre len {ln} @ {off:#x}"
        rec = data[off:off+14]
        a += 1
        yield (num, page, {
            "notenum": struct.unpack_from("<b", rec, 2)[0],
            "mod": tuple(rec[3:8]),
            "feedconn": rec[8],
            "car": tuple(rec[9:14]),
        })

UNUSED_OP = (0x00, 0x3F, 0x00, 0xF0, 0x00)

def wopl_instrument(t, percussion):
    if t is None:
        return b"\0" * 32 + struct.pack(">hh", 0, 0) + bytes([0, 0, 0, 0x04]) \
               + bytes([0, 0]) \
               + b"".join(bytes(UNUSED_OP) for _ in range(4)) \
               + struct.pack(">HH", 0, 0)
    name = b"\0" * 32
    if percussion:
        head = name + struct.pack(">hh", 0, 0) + bytes([0, 0, t["notenum"] & 0xFF, 0x00])
    else:
        head = name + struct.pack(">hh", t["notenum"], 0) + bytes([0, 0, 0, 0x00])
    ops = bytes(t["car"]) + bytes(t["mod"]) + bytes(UNUSED_OP) * 2
    return head + bytes([t["feedconn"], 0]) + ops + struct.pack(">HH", 0, 0)

def convert(opl_path, wopl_path):
    mel = [None] * 128
    per = [None] * 128
    for num, page, t in parse_opl(opl_path):
        if page == 0x00:
            mel[num] = t
        elif page == 0x7F:
            per[num] = t
        else:
            raise ValueError(f"unknown page {page:#x} for num {num}")
    out = bytearray()
    out += b"WOPL3-BANK\0"
    out += struct.pack("<H", 3)          # version 3
    out += struct.pack(">HH", 1, 1)      # 1 melodic + 1 percussion bank
    out += bytes([0x00])                 # flags: no deep tremolo/vibrato
    out += bytes([0x07])                 # volume model = WOPL_VM_AIL
    out += b"FD2 SAMPLE.OPL melodic\0".ljust(32, b"\0")[:32] + bytes([0, 0])
    out += b"FD2 SAMPLE.OPL percussion\0".ljust(32, b"\0")[:32] + bytes([0, 0])
    for t in mel:
        out += wopl_instrument(t, False)
    for t in per:
        out += wopl_instrument(t, True)
    open(wopl_path, "wb").write(out)
    n_mel = sum(t is not None for t in mel)
    n_per = sum(t is not None for t in per)
    print(f"{opl_path} -> {wopl_path}: {len(out)} bytes, "
          f"melodic {n_mel}/128, percussion {n_per}/128")
    print("percussion notes:", [n for n, t in enumerate(per) if t])
    print("percussion (note, notenum):",
          [(n, t["notenum"]) for n, t in enumerate(per) if t][:40])

if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "SAMPLE.OPL"
    dst = sys.argv[2] if len(sys.argv) > 2 else "FD2SAMPLE.wopl"
    convert(src, dst)
