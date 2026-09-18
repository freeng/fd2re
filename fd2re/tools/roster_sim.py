#!/usr/bin/env python3
"""roster_sim.py — 商店角色网格选取逻辑的离线模拟（纯静态验证工具，
不属于 fd2re 构建目标）。逐语句复刻 fd2re scene.c 的
roster_pick_unit 键处理 / roster_grid_draw 名字绘制（透明文字）/
shop_list_scroll_down/up 的精确几何，用符号像素检测名字区是否出现
"当前名字混入旧名字/旧颜色残留"（对应症状：部分字高亮）。
像素编码: (record<<4)|nibble，nibble: 1=fg201,2=fg205,A=影子。
"""

W = 320
VRAM = bytearray(b"\x77" * 64000)

SLOT_X, NAME_X, ROW_H = 132, 40, 26
MAXREC = 12
name_len = [4, 5, 3, 6, 4, 5, 3, 4, 6, 5, 4, 3]

g_roster_count = g_shop_list_scroll = g_menu_choice = 0


def cell_addr(slot, cell, dy):
    x = SLOT_X * (slot % 2) + NAME_X + 16 * cell
    y = ROW_H * (slot // 2) + 121 + dy
    return y * W + x


def draw_name(slot, rec, fg):
    nib = 1 if fg == 201 else 2
    for c in range(name_len[rec]):
        base = cell_addr(slot, c, 0)
        for dy in range(2, 14):
            off = base + dy * W
            for dx in range(2, 14):
                VRAM[off + dx] = (rec << 4) | nib
        # 影子行（第 15 行下溢一行）
        off = base + 14 * W
        for dx in range(2, 14):
            VRAM[off + dx] = (rec << 4) | 0xA


def scroll_down():
    for _ in range(3):
        for j in range(74):
            d = 36810 + W * j
            VRAM[d:d + 284] = VRAM[d + 1920:d + 1920 + 284]
        for k in range(6):
            d = 60490 + W * k
            VRAM[d:d + 284] = b"\x77" * 284
    for m in range(72):
        d = 36810 + W * m
        VRAM[d:d + 284] = VRAM[d + 2560:d + 2560 + 284]
    for n in range(8):
        d = 59850 + W * n
        VRAM[d:d + 284] = b"\x77" * 284


def scroll_up():
    for _ in range(3):
        for j in range(73, -1, -1):
            d = 38730 + W * j
            VRAM[d:d + 284] = VRAM[d - 1920:d - 1920 + 284]
        for k in range(6):
            d = 36810 + W * k
            VRAM[d:d + 284] = b"\x77" * 284
    for m in range(71, -1, -1):
        d = 39370 + W * m
        VRAM[d:d + 284] = VRAM[d - 2560:d - 2560 + 284]
    for n in range(8):
        d = 36810 + W * n
        VRAM[d:d + 284] = b"\x77" * 284


def rows_visible():
    if g_roster_count > 6:
        return 5 if g_shop_list_scroll + 6 > g_roster_count else 6
    return g_roster_count


def grid_draw(selected):
    for i in range(rows_visible()):
        rec = i + g_shop_list_scroll
        draw_name(i, rec, 201 if rec == selected else 205)


def audit():
    bad = 0
    detail = []
    for i in range(rows_visible()):
        want = i + g_shop_list_scroll
        for c in range(name_len[want]):
            for dy in range(2, 14):
                off = cell_addr(i, c, 0) + dy * W
                for dx in range(2, 14):
                    if VRAM[off + dx] >> 4 != want:
                        bad += 1
                        if len(detail) < 3:
                            detail.append((i, c, hex(VRAM[off + dx])))
    return bad, detail


def reset():
    global g_shop_list_scroll, g_menu_choice
    g_shop_list_scroll = 0
    g_menu_choice = 0
    for i in range(len(VRAM)):
        VRAM[i] = 0x77
    grid_draw(g_menu_choice)


def press(k):
    global g_menu_choice, g_shop_list_scroll
    if k == "r":
        if g_menu_choice == g_roster_count - 1:
            return
        g_menu_choice += 1
        if g_menu_choice - g_shop_list_scroll >= 6:
            g_shop_list_scroll += 2
            scroll_down()
    elif k == "l":
        if g_menu_choice == 0:
            return
        g_menu_choice -= 1
        if g_menu_choice < g_shop_list_scroll:
            g_shop_list_scroll -= 2
            scroll_up()
    elif k == "u":
        if g_menu_choice < 2:
            return
        g_menu_choice -= 2
        if g_menu_choice < g_shop_list_scroll:
            g_shop_list_scroll -= 2
            scroll_up()
    elif k == "d":
        if g_roster_count - 2 <= g_menu_choice:
            return
        g_menu_choice += 2
        if g_menu_choice - g_shop_list_scroll >= 6:
            g_shop_list_scroll += 2
            scroll_down()
    grid_draw(g_menu_choice)


def main():
    seqs = ["d", "dd", "ddd", "dddd", "r", "rr", "rd", "dr", "du", "ud",
            "ddddu", "dddu", "dud", "dudd", "d d d u", "rrddlu",
            "dduddu", "uddd", "dddddu", "dddddd"]
    total = 0
    for count in range(1, MAXREC + 1):
        global g_roster_count
        g_roster_count = count
        for s in seqs:
            reset()
            for ch in s:
                if ch != " ":
                    press(ch)
            bad, detail = audit()
            if bad:
                print(f"count={count} seq={s!r} scroll={g_shop_list_scroll} "
                      f"choice={g_menu_choice} bad={bad} ex={detail}")
                total += bad
    print("FAIL: %d stale pixels" % total if total else "OK: no stale pixels")
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main())
