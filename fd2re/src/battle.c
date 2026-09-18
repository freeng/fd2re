/* battle.c — 战场子系统（TRPG 部分）
 *
 * 2026-09-04：战斗主循环定位为 anim_pump@0x117E7（main 内层 do-while 每轮调用，
 * 每次处理一键）；0x22BBE 只是共享尾声。回合/指令/伤害公式结构已验证，
 * 见 reverse/battle/digest.md。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "battle.h"
#include "duel.h"
#include "magic.h"
#include "event.h"
#include "audio.h"
#include "video.h"
#include "timer.h"
#include "entities.h"
#include "script.h"
#include "input.h"
#include "text.h"
#include "save.h"
#include "resource.h"
#include "host.h"

/* ---- 战斗运行态（IDA dseg02；宿主侧保留同一数据形状） ----------- */
static uint8_t s_battle_ctx[2211];
uint8_t *g_battle_ctx = s_battle_ctx;
static int s_battle_ctx_len;                  /* 已装载块长（dat_load 后记录） */
static uint8_t *s_deploy_map;                 /* FDFIELD[3k+2] 布阵层（原版全局，用后 free） */
uint8_t g_spawn_direct;                      /* 0x53AFA：1=增援直接落位 / 0=最近可走格搜索 */

/* ---- g_battle_ctx 布局（FDFIELD[3k+1]，2026-09-05 定案 §6.9，反汇编+资产双验证）
 * 块长 = 126 + 26×F + 5（14 战场全验证；F=记录数=ctx[2]，state0 实测 F=E+1 挂号）：
 *   [0]        shape 变体（FDSHAP 块对 2v / 2v+1）
 *   [1]        玩家槽数 P（=g_ent_count 初值；4→16 递增）
 *   [2]        敌人条数 E（布阵层条目数；布阵块长 2+6×(P+E) 14/14 验证）
 *   [3..50]    16x3B 回合事件槽 (回合,事件脚本id,相位)——battle_turn_event_scan
 *   [51..82]   16x2B 地块事件槽 (event_id,触发模式)——tile_event_check
 *              （2026-09-05 全解，见 §13.10/§13.12）
 *   [83..124]  宝箱表 3B×14，索引=tile id t（[0]类型 0=物品/1=金钱/其他=事件id，
 *              [1..2]u16 参数；treasure_interact@0x190AC 与 AI 行为 5 共用）
 *   [125]      填充
 *   [126..126+26F) standing 的交错记录区。不能按独立 26B 结构体解释：
 *              sprites_reload_slot@0x10BE4 的分组键 = ctx[26i+152]；
 *              standing_sprite_build@0x10D79 的 payload = ctx+26i+131，
 *              后者会读取 26B，跨越相邻条目的边界。
 *   [尾 5B]    挂号 ---- */
#define CTX_OFF_SHAPE          0
#define CTX_OFF_PLAYER_SLOTS   1
#define CTX_OFF_ENEMY_COUNT    2
#define CTX_OFF_TREASURE      83
#define CTX_TREASURE_COUNT    14
#define CTX_OFF_SPRITE_REC   126
#define CTX_SPRITE_STRIDE     26
uint8_t g_spawner_flags[32];

int32_t g_cur_unit_idx;
uint32_t g_cursor_x, g_cursor_y;
int32_t g_cursor_view_x, g_cursor_view_y;
uint32_t g_action_saved_x, g_action_saved_y;
uint32_t g_scroll_x, g_scroll_y;
int32_t g_view_w, g_view_h;
int32_t g_turn_count;
int32_t g_gold;
int32_t g_exp_gained;
int32_t g_unit_acted;
int32_t g_menu_choice;
int32_t g_ai_score_target;
int32_t g_ai_target_x, g_ai_target_y;
int32_t g_ai_target_item_slot;
int32_t g_ai_score_move;
int32_t g_ai_move_x, g_ai_move_y;
int32_t g_ai_move_item_slot;
int32_t g_ai_target_unit;
int32_t g_ai_score_attack;
int32_t g_ai_attack_x, g_ai_attack_y;               /* 0x53C43/0x53C47 */
int32_t g_cursor_anim_frame;                        /* 0x53C1F */
uint8_t g_attack_miss_flag;                         /* 0x53C6B */
static int s_atk_tiebreak;                          /* sub_14237 平局 diff 记忆（v45） */
static int s_attack_slot_a[2];                      /* 0x53A30/34 槽A=arg_4(目标)立绘位 */
static int s_attack_slot_b[2];                      /* 0x53A38/3C 槽B=arg_0(攻击者)位（-1=无） */

/* Full resource loading can replace these tables; zero initialization keeps
 * the verified formulas linkable until those loaders are reconstructed. */
uint8_t g_class_crit_base[32];
/* 0x51A12/0x51A2A 是二进制内静态 i32[6] 表（非运行时加载，2026-09-07
 * get_bytes 定案；dword[attr] 索引，attr = tile_lookup info[5]，值 0..5）。 */
static const int32_t s_tile_atk_pct[6] = {5, 0, -5, -5, -5, 0};
static const int32_t s_tile_def_pct[6] = {0, 0, 10, 10, -5, 0};
const int32_t *g_tile_atk_pct = s_tile_atk_pct;
const int32_t *g_tile_def_pct = s_tile_def_pct;
uint8_t g_state_duel_bg[FD2_STATE_COUNT];
uint8_t g_ending_duel_bg;                     /* 0x54133（ending_sequence_play 写） */
void *g_death_fx_pkg;
void *g_pkg_fdother_64;                    /* 0x53B0F（定义见 fd2.h 注释） */
void (*g_state_post_action[FD2_STATE_COUNT])(int unit);

/* ---- 主输入泵（0x117E7 结构已验证） -------------------------------- */

/* 玩家战斗输入循环的循环不变量（0x51A83，全写点取证 2026-09-08）：
 * anim_pump 等键期间 g_text_busy 必须为 1（正常选框模式）——选框层
 * sub_122DC@0x122DC 的 switch 无 case 0，模式 0 时选择器完全不画。
 * 原版从不在此处显式恢复，而是靠两类副作用维持：
 *   ① battle_field_preset@0x205DA 中段写 1、enemy_phase_check@0x13565
 *      的 0→battle_turn_end→1 括号——结构性恢复；
 *   ② 对白遗留——战斗开场 exit 序列的末句以活跃说话人开窗收尾，
 *      dialog_open_anim@0x165AC 聚焦支 0x165DE 写 1、关窗（0x15F84
 *      族）不写——数据性依赖：末句若为旁白/说话人不在场（fd2re
 *      实体表时序须与原版分毫不差才成立），1 就不会出现。
 * fd2re 把这条不变量提升为输入泵边界的显式恢复：战场在场（field/
 * 实体表俱在）且模式为 0 时回 1。中途模态流（移动/施法/2..6 指示
 * 范数）都在单次 anim_pump 调用内同步完成、结束后自带归 1，本守卫
 * 只拦"以 0 状态跨调用交回玩家输入"这一类断裂交接。 */
static void battle_input_phase_restore(void)
{
    if (g_text_busy == 0 && g_field_map && g_ent_table)
        g_text_busy = 1;
}

int anim_pump(void)
{
    battle_input_phase_restore();
    int key = input_wait_key();

    if (key == SCAN_ESC || key == 44 || key == 76) {   /* Esc / ',' / 'L' */
        /* 环扫下一可动单位：(flags&0x85)==0 && type==2 */
        int found = 0, idx = g_cur_unit_idx;
        for (int i = 0; i < g_ent_count && !found; i++, idx = (idx + 1) % g_ent_count) {
            uint8_t *rec = g_ent_table[idx];
            if ((rec[5] & 0x85) == 0 && rec[6] == 2) {
                camera_focus_ent(idx);            /* 0x12D7B */
                g_cur_unit_idx = (idx + 1) % g_ent_count;
                found = 1;
            }
        }
        kbd_flush();
        return 0;
    }
    if (key == 57 || key == SCAN_ENTER) {              /* Space / Enter */
        /* 0x11892：byte_51A42 非零则递减（全库无写点，原版死变量照录；
         * 0x118AA 为 EB（jz 被补丁成 jmp），sub_33FAF 重试环死码不译）。 */
        static uint8_t s_byte_51a42;
        if (s_byte_51a42)
            s_byte_51a42--;
        int u = ent_at_cursor();                  /* 0x12C0D */
        if (u != -1) {
            uint8_t *rec = g_ent_table[u];
            g_exp_gained = 0;   /* 0x118EE：选中即清残留经验——满级/满经验
                                 * 击杀者的未领经验不得泄给下一个选中单位
                                 *（原版早退路径 0x1317D 不清，靠此处收口）*/
            if (rec[7] != 121 && rec[31] != 10) {
                if (rec[6] == 2 && (signed char)rec[5] >= 0 && !rec[38]) {
                    sfx_play(g_pkg_fdother_31, 7, 1);  /* 0x11930 选单位音 */
                    while (!battle_unit_turn(u))
                        ;
                }
                else
                    sub_17AED(u);                 /* 单位状态页 */
                anim_tick_update(0);
                if (g_exp_gained > 99)
                    g_exp_gained = 99;
                exp_levelup_check(u);
                g_state_post_action[g_state](u);  /* 0x51B19 */
                enemy_phase_check();              /* 0x13565 */
                event_script_dispatch(u);         /* 0x11994-0x119A6，见 event.c */
                kbd_flush();
                return 0;
            }
            return 0;
        }
        int r;
        do { r = battle_system_menu(); } while (!r);   /* 0x16F55 */
        return r == 1 ? 0 : r;
    }
    if (key == 34)                                    /* '"'：无操作 */
        return 0;
    switch (key) {
    case 59: case 0x49:                               /* ';' / PgUp */
        battle_map_overview();
        return 0;
    case 60: case 0x47: {                             /* '<' / Home：状态页 */
        int u = ent_at_cursor();
        if (u != -1 && g_ent_table[u][7] != 121 && g_ent_table[u][31] != 10)
            sub_17AED(u);
        return 0;
    }
    case 0x48: sfx_play(g_pkg_fdother_31, 0, 1); cursor_up();    return 0;
    case 0x50: sfx_play(g_pkg_fdother_31, 0, 1); cursor_down();  return 0;
    case 0x4B: sfx_play(g_pkg_fdother_31, 0, 1); cursor_left();  return 0;
    case 0x4D: sfx_play(g_pkg_fdother_31, 0, 1); cursor_right(); return 0;
    default: return 0;
    }
}

/* ---- 伤害公式（0x2F7B6 完整还原） ---------------------------------- */
int battle_strike_formula(int att, int def, int res[6])
{
    uint8_t *a = g_ent_table[att], *d = g_ent_table[def];
    int atk = *(uint16_t *)(a + 0x48);
    int dfn = *(uint16_t *)(d + 0x4A);
    int hit = *(uint16_t *)(a + 0x4C);
    int eva = *(uint16_t *)(d + 0x4E);
    int hp  = *(uint16_t *)(d + 0x40);
    int maxhp = *(uint16_t *)(d + 0x42);

    res[0] = 1; res[1] = res[2] = res[3] = res[4] = res[5] = 0;

    /* 地形百分比修正（tile_lookup 取所在格；原版 0x1ED97/0x2F8B4 均以
     * dword 表[info[5]] 索引——此前误用 info[1]，2026-09-07 修正） */
    uint8_t tile_a[8], tile_d[8];
    if (!sub_1F183(att)) {
        tile_lookup(a[0], a[1], tile_a);
        atk += atk * g_tile_atk_pct[tile_a[5]] / 100;
    }
    if (!sub_1F183(def)) {
        tile_lookup(d[0], d[1], tile_d);
        dfn += dfn * g_tile_def_pct[tile_d[5]] / 100;
    }

    int crit = g_class_crit_base[a[32] - 1];
    /* 武器记录 = weapon_table_entry(inventory_get_item(att,
     * equipped_slot_find(att, 0)))：w[9]=附魔类型 w[10]=附魔参数
     * w[11]/[12]=射程 min/max（表 @0x602AD 已嵌 fd2re tables.c，
     * 2026-09-04 接入真数据）。 */
    int wtype = 0, wparam = 0;
    int wslot = equipped_slot_find(att, 0);
    if (wslot >= 0) {
        const uint8_t *w = weapon_table_entry(inventory_get_item(att, wslot));
        if (w) {
            wtype = w[9];
            wparam = w[10];
        }
    }
    switch (wtype) {
    case 4: crit += wparam; break;
    case 2:                                            /* 附加状态 */
        if ((int)(fd2_rand() % 100) < wparam) {
            d[0x25] = fd2_rand() % 4 + 2;
            res[2] = 1;
        }
        break;
    case 3: res[4] = 1; break;
    }

    int dmg = 0;
    if ((int)(fd2_rand() % 100) < hit - eva) {        /* 命中 */
        res[0] = 0;
        if ((int)(fd2_rand() % 100) < crit) { dfn /= 2; res[1] = 1; }  /* 暴击 */
        dmg = 9 * (atk - dfn) / 10;
        if (dmg < 0) dmg = 0;
        if (dmg / 9) dmg += fd2_rand() % (dmg / 9);   /* 方差 ±dmg/9 */
        hp -= dmg;
        if (hp < 0) hp = 0;
        *(uint16_t *)(d + 0x40) = (uint16_t)hp;
    }

    /* 经验：玩家攻击进阶敌人(icon>=0x44)。
     * 原版（0x2F7B6 与 0x1ECC7 双拷贝一致）：打死(剩余HP==0)→base 全额；
     * 未死(含未命中 dmg=0)→dmg×base/MaxHP 按比例（未命中=0）。
     * 2026-09-05 对照 0x1ECC7 修正：此前误用 dmg!=0（未命中错发全额）。 */
    if (a[6] == 2 && d[7] >= 0x44) {
        int lv = a[33];
        if ((a[32] > 8 && a[32] < 0x19) || a[8] == 28) lv += 30;
        /* 敌基表：enemy_base_table_entry@0x4E84F（10B/条，[9]=经验基数） */
        int base = enemy_base_table_entry(d[7] - 68)[9] * d[33] / lv;
        g_exp_gained = hp ? dmg * base / maxhp : base;
    }
    res[5] = dmg;
    return dmg;
}

/* 战斗交互层原语（定义在本文件后部 / duel.c，§13.25/§13.38） */
static uint8_t *field_cell(int x, int y);
static void     field_flood_seed(int cls, int x, int y, int budget);
static int      field_path_build(int cls, int x0, int y0, int budget,
                                 uint8_t *path, int cap, int dest_x,
                                 int dest_y, int flag, int *wp_x, int *wp_y);
static int  menu_first_enabled(const int flags[4]);
static void radial_menu_open(const int def[4], const int flags[4]);
static void radial_menu_close(const int def[4], const int flags[4]);
static int  radial_menu_run(const int def[4], const int flags[4]);
static int  terrain_immune_test(int unit);        /* 0x1F183（定义后部） */
static const uint8_t *class_table_entry(int cls); /* 0x4E8A5（定义后部） */
static int  ai_attack_now(int unit);              /* 0x13FD4（定义后部） */

/* ---- 玩家单位回合（0x18890 / 0x18D8C 结构） ------------------------ */
/* ==== 移动域链（0x145CD/0x146D1/0x14625/0x146A7/0x4E4F6/0x13488 +
 * 0x18890 主流程，2026-09-08 §13.38） ================================ */

/* 0x146A7：敌邻格 cell[2] |= 0x80（洪泛进入后预算清 0 = 必停）。 */
static void cell_mark_zoc(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_field_w || y >= g_field_h)
        return;
    field_cell(x, y)[2] |= 0x80u;
}

/* 0x14625：敌格 cell[2] |= 0x40（禁入）+ 四邻 0x80（ZoC 必停）。 */
static void enemy_cell_mark(int x, int y)
{
    if (x > 0) cell_mark_zoc(x - 1, y);
    if (y > 0) cell_mark_zoc(x, y - 1);
    if (x < g_field_w - 1) cell_mark_zoc(x + 1, y);
    if (y < g_field_h - 1) cell_mark_zoc(x, y + 1);
    field_cell(x, y)[2] |= 0x40u;
}

/* 0x145CD(enemy_filter=mode)：对立阵营 ZoC 标记（洪泛前调用；
 * field_reset_candidates 的 cell[2]&=0x1F 会清掉本标记）。
 * 2026-09-08 按 0x145D2 反汇编带 mode 重建：mode==0（敌方行动）→ 标
 * 非敌方（ent[6]!=0）格；mode==1（我方/NPC 行动）→ 标敌方格。
 * 旧无参版仅覆盖玩家移动语义（=mode 1）。 */
static void enemies_zoc_mark(int mode)
{
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (rec[5] & 1u)
            continue;
        if (mode == 0 ? rec[6] == 0 : rec[6] != 0)
            continue;
        if (rec[0] >= g_field_w || rec[1] >= g_field_h)
            continue;
        enemy_cell_mark(rec[0], rec[1]);
    }
}

/* 0x146D1(unit, mode)：同阵营单位所占格 marker=255（不可停留，可途经）。
 * mode==0 → 标敌方格（敌方行动时己方互挡）；mode==1 → 标非敌方格。 */
static void allies_cells_block(int unit, int mode)
{
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (i == unit || (rec[5] & 1u))
            continue;
        if (mode == 0 ? rec[6] != 0 : rec[6] == 0)
            continue;                 /* 挡己方阵营格：mode0=敌方、mode1=非敌方 */
        if (rec[0] >= g_field_w || rec[1] >= g_field_h)
            continue;
        field_cell(rec[0], rec[1])[3] = 0xFF;
    }
}

/* 0x4E4F6 field_path_build：自含 DFS 路径构建（忠实复刻版，实现在本
 * 文件后部 field_flood_seed 之后；2026-09-08 撤销"洪泛 marker 回溯"近似
 * ——该近似取代价最小路径且平局裁决与原版不同，是移动路径形状不一致
 * 的根因，详见实现处注释）。 */

/* 0x13488 walk_path_anim：路径步序列播放，方向码直派四向原语——
 * 0=ent_walk_down_one_tile、1=左、2=上、其余=右（rec[3]=方向码）。
 * 2026-09-07：此前以 anim_tick_update+cursor_scroll_to 结构级近似，
 * 现四原语全解落地（每帧 ±4px 平滑镜头滚动 + 实体分数位移），近似
 * 与 §13.38 的像素滚动挂号一并关闭。 */
static void walk_path_anim(int unit, const uint8_t *path, int len)
{
    for (int i = 0; i < len; i++) {
        switch (path[i]) {
        case 0:  ent_walk_down_one_tile(unit); break;
        case 1:  ent_walk_left_one_tile(unit); break;
        case 2:  ent_walk_up_one_tile(unit); break;
        default: ent_walk_right_one_tile(unit); break;
        }
    }
}

/* 0x18890 battle_unit_turn 全量重写（§13.38）：
 * 移动域 = rec[59] 洪泛（敌格 bit6 禁入/敌邻 bit7 必停 ZoC + 我方格
 * 不可停留）→ battle_move_select(4) → reset+二次 ZoC+洪泛重建 → 路径
 * 回溯 → 走路动画 →
 * ents_face_reset → 行动菜单（移动后 rec[7]∉{18,19,34} 禁魔法）。
 * Esc 撤销链：移动选中 Esc=回合结束(返 1)；菜单 Esc 且未行动=撤销
 * 位移回起点重试(返 0)。path==255 不可达=回合结束；path==0 原地=
 * 菜单 after_move=0。 */
int battle_unit_turn(int unit)
{
    uint8_t path[64];
    int flags[4] = { 0, 0, 0, 0 };                /* unk_53F12 全 0 */

    if (!g_ent_table || unit < 0 || unit >= g_ent_count) return 1;
    uint8_t *rec = g_ent_table[unit];
    g_unit_acted = 0;
    g_pending_event_id = 255;
    int move = rec[59];
    int cx0 = (int)g_cursor_x, cy0 = (int)g_cursor_y;
    /* 0x188E1：cls=rec[32]；terrain_immune→19；rec[7]==28→16 */
    int cls = rec[32];
    if (terrain_immune_test(unit))
        cls = 19;
    else if (rec[7] == 28)
        cls = 16;

    if (move <= 0 || move > (int)sizeof(path)) {
        /* 移动力 0：直接行动菜单（after_move=0） */
        int r;
        do {
            r = battle_act_menu(unit, flags, 0);
        } while (!r);
        if (r != -1) {
            tile_event_check(rec[0], rec[1], 1);
            return 1;
        }
        return g_unit_acted;
    }
    /* 0x18928：ZoC 标记（洪泛前）→ 洪泛 → 我方格排除 */
    field_reset_candidates();
    enemies_zoc_mark(1);                          /* 玩家单位：敌方 ZoC */
    field_flood_seed(cls, cx0, cy0, move);
    allies_cells_block(unit, 1);

    move_range_wait_pump(unit);                   /* 0x18973 等首键+单位面板 */
    int sel = battle_move_select(4, 0, NULL);
    field_reset_candidates();
    if (sel == -1) {                              /* 移动取消：回合结束 */
        cursor_scroll_to(cx0, cy0);
        return 1;
    }
    enemies_zoc_mark(1);                           /* 0x189C5 二次标记 */
    /* 0x189F8 原版此处 reset+ZoC 后直接自含 DFS（十参），无二次洪泛；
     * 2026-09-08 起为忠实复刻 DFS（步数最短/等长后到覆盖/右左下上序）。
     * 不含我方格排除：路径可途经我方格（仅落点受排除，落点已在
     * battle_move_select mode4 过滤）。 */
    int plen = field_path_build(cls, cx0, cy0, move, path, sizeof(path),
                                (int)g_cursor_x, (int)g_cursor_y, 0,
                                NULL, NULL);
    field_reset_candidates();
    g_text_busy = 0;
    cursor_scroll_to(cx0, cy0);
    g_text_busy = 1;
    if (plen == 255)
        return 1;                                 /* 不可达（防御路径） */

    if (plen > 0) {
        walk_path_anim(unit, path, plen);         /* 0x18A52 走路 */
        ents_face_reset();
        if (rec[7] != 18 && rec[7] != 19 && rec[7] != 34)
            flags[1] = 1;                         /* 移动后禁魔法 */
    }
    int r;
    do {
        r = battle_act_menu(unit, flags, plen > 0 ? 1 : 0);
    } while (!r);
    if (r != -1) {
        tile_event_check(rec[0], rec[1], 1);
        return r;
    }
    if (!g_unit_acted) {
        if (plen > 0) {                           /* 撤销位移回起点 */
            rec[0] = (uint8_t)cx0;
            rec[1] = (uint8_t)cy0;
            cursor_scroll_to(cx0, cy0);
        }
        return 0;                                 /* 重试回合 */
    }
    sub_13512(unit);                              /* 0x18ADC 已行动 */
    tile_event_check(rec[0], rec[1], 1);
    return g_unit_acted;
}

/* ---- 0x18D8C 行动菜单（2026-09-08 全量重写，§13.25） ------------------
 * 径向 2x2 菜单 def={0,1,2,3}（blk2 帧 3*def+2*flags +选中 0/1 闪）。
 * flags 禁用：[0]=无装备武器/射程内无敌人，[1]=无法术或 ent[39] 状态
 * （沉默），[2]=包空。choice 0=攻击 1=魔法 2=道具 3=调查/待机。 */
int battle_act_menu(int unit, int flags[4], int after_move)
{
    static const int def[4] = { 0, 1, 2, 3 };        /* unk_51ED5 */
    uint8_t targets[100];
    uint8_t probe[32];
    int wmin = 0, wmax = 0, r;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count) return -1;
    uint8_t *rec = g_ent_table[unit];
    flags[0] = 0;
    g_exp_gained = 0;
    int slot = equipped_slot_find(unit, 0);
    if (slot == -1) {
        flags[0] = 1;                                /* 无装备武器 */
    } else {
        const uint8_t *wd = weapon_table_entry(sub_1B722(unit, slot));
        wmin = wd[11];
        wmax = wd[12];
        if (shape_targets_collect(NULL, g_cursor_x, g_cursor_y,
                                  wmax, wmin, 0) == 0)
            flags[0] = 1;                            /* 射程内无敌人 */
        field_reset_candidates();
    }
    (void)menu_first_enabled(flags);                 /* 0x18E53 一次初选 */
    radial_menu_open(def, flags);
    if (sub_1B8A6(unit) == 0)
        flags[2] = 1;                                /* 包空禁道具 */
    if (spells_collect(unit, probe) == 0)
        flags[1] = 1;                                /* 无法术禁魔法 */
    if (rec[39])
        flags[1] = 1;                                /* ent[39] 状态禁魔法 */
    g_menu_choice = menu_first_enabled(flags);       /* 0x18ED6 二次初选 */
    do {
        r = radial_menu_run(def, flags);             /* 0x177FC */
    } while (!r);
    radial_menu_close(def, flags);
    anim_tick_update(0);
    if (r == -1)
        return -1;

    if (g_menu_choice == 0) {                        /* 攻击 0x18F34 */
        int cx0 = (int)g_cursor_x, cy0 = (int)g_cursor_y;
        int n = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      wmax, wmin, 0);
        int sel = battle_move_select(0, n, targets);
        field_reset_candidates();
        if (sel == -1) {
            cursor_scroll_to(cx0, cy0);
            return 0;                                /* 取消 → 外层重开 */
        }
        int tgt = ent_at_cursor();
        battle_face_target(unit, tgt);               /* 0x18FBC 出手朝向 */
        duel_scene_load(unit, tgt);
        ents_face_reset();
        uint8_t drops[15][3];
        int nd = battle_collect_drops(drops);
        battle_death_anim();
        battle_show_results(unit, nd, (const uint8_t *)drops);
        sub_13512(unit);                             /* 已行动标记 */
        kbd_flush();
        return 1;
    }
    if (g_menu_choice == 1) {                        /* 魔法 0x1CFF0 */
        do {
            r = spell_cast_flow(unit);
        } while (!r);
        if (r == -1)
            return 0;
        sub_13512(unit);
        int base = rec[33];
        if (rec[32] > 8)
            base += 30;
        g_exp_gained /= base;                        /* 0x19029 经验折算 */
        return 1;
    }
    if (g_menu_choice == 2) {                        /* 道具 0x1BBDC */
        do {
            r = battle_item_target_flow(unit);
        } while (!r);
        if (r == -1)
            return 0;
        g_exp_gained = 0;
        return 1;
    }
    /* choice 3：调查/待机。0x19077：after_move==0（未移动）→ 先
     * ai_attack_now（再生 tick：sfx + 闪白 + HP+Max/5）再调查。 */
    if (!after_move)
        ai_attack_now(unit);
    treasure_interact(unit);
    sub_13512(unit);
    return 1;
}

/* ==== 回合开始/转场原语族（2026-09-08 逐指令定案） ==================== */

/* 0x1DA16 的 24x24 4-op 流有界贴图（宿主防越界；原版平面内存直写）：
 * silhouette=0 普通透明写（0x4E22A 语义）；=1 纯色剪影（0x4E127 语义：
 * 0x4E127 开头 mov ah,al 取 pitch 低字节为色，仍读流确定形状但所有像素同色）。 */
static void sprite24_stamp_bounded(const uint8_t *src, uint8_t *surf,
                                   size_t surf_size, long off, int pitch,
                                   int silhouette, uint8_t color)
{
    for (int row = 0; row < 24; row++) {
        int col = 0;
        while (col < 24) {
            uint8_t op = *src++;
            int n = (op & 0x3F) + 1;
            switch (op >> 6) {
            case 0: {                            /* RLE 实心 */
                uint8_t v = silhouette ? color : *src;
                src++;                           /* 剪影模式仍需推进指针 */
                for (int k = 0; k < n; k++, col++) {
                    long at = off + (long)row * pitch + col;
                    if (at >= 0 && (size_t)at < surf_size)
                        surf[at] = v;
                }
                break;
            }
            case 1: {                            /* 隔点写（奇数列） */
                uint8_t v = silhouette ? color : *src;
                src++;                           /* 剪影模式仍需推进指针 */
                /* 0x4E22A 语义：写 col+1/col+3/col+5... 共 n 对（消耗 2n 列） */
                for (int k = 0; k < n; k++, col += 2) {
                    long at = off + (long)row * pitch + col + 1;
                    if (at >= 0 && (size_t)at < surf_size)
                        surf[at] = v;
                }
                break;
            }
            case 2:                              /* 字面（剪影=整段同色） */
                for (int k = 0; k < n; k++, col++) {
                    long at = off + (long)row * pitch + col;
                    if (at >= 0 && (size_t)at < surf_size)
                        surf[at] = silhouette ? color : src[k];
                }
                src += n;
                break;
            default:                             /* 透明跳过 */
                col += n;
                break;
            }
        }
    }
}

/* 0x1DA16 sub_1DA16(dst, pitch, unit, mode, color)：把单位当前立绘帧
 * 重新盖到 dst 合成面。视口裁剪（x∈[scroll-1, scroll+view_w]、
 * y∈[scroll-1, scroll+view_h+1]）；像素位 = tile 原点上移 6 行
 * （-6*pitch）+ 24*pitch*(y-sy) + 24*(x-sx) + 朝向行走偏移
 * rec[4]×(下/上=±4 行、左/右=∓4px)；帧 = 立绘表 [slot][朝向×3+相位]
 * （行走中取实体相位钟，否则光标相位钟，相位 3 折叠为 1）。
 * mode 0 = 普通透明绘制；mode 2 = 纯色剪影；mode 1/其他不绘制。
 * battle_turn_end 回复双 pass、ai_attack_now 回复闪烁、attack_anim_play
 * 命中闪白三处共用。2026-09-08 修复剪影色：原版 sub_4E127(0x4E127)
 * `mov ah,al`（eax=第 3 参 pitch）——填充色 = pitch 低字节（456→0xC8
 * 淡蓝白、320→0x40 黄绿），调用方压栈的 color(0xFD) 是死参数从未被读
 * （0x1DB4E push 序列 + `add esp,10h` 四参可证）；fd2re 此前照抄 0xFD
 * 纯白，休息闪光颜色错误。 */
static void unit_sprite_stamp(uint8_t *dst, int pitch, int unit, int mode,
                              uint8_t color)
{
    const uint8_t *stream;
    size_t len;
    uint8_t *rec;
    uint8_t *surf;
    size_t surf_size;
    long off;
    int x, y, dir, phase;

    (void)color;                    /* 原版死参数（见函数头注释） */
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    rec = g_ent_table[unit];
    x = rec[0];
    y = rec[1];
    if (x < (int)g_scroll_x - 1 || x > g_view_w + (int)g_scroll_x
        || y < (int)g_scroll_y - 1 || y > g_view_h + (int)g_scroll_y + 1)
        return;
    phase = rec[4] ? field_entity_phase() : field_cursor_phase();
    if (phase == 3)
        phase = 1;
    if (icon_frame_get(rec[2], 3 * rec[3] + phase, &stream, &len) != 0)
        return;

    /* 面缓冲判定：pitch 456 = 战场影带；320 = VRAM（attack_anim_play
     * 直写屏幕路径）。原版无越界防护（DOS 平面内存），宿主按面尺寸钳。 */
    if (pitch == 456) {
        surf = battle_shadow_layer();
        surf_size = (size_t)456 * 336;
    } else {
        surf = vram_base();
        surf_size = FD2_VRAM_SIZE;
    }
    off = (long)(dst - surf) - 6L * pitch
        + 24L * pitch * (y - (int)g_scroll_y)
        + 24L * (x - (int)g_scroll_x);
    dir = rec[3];
    if (dir == 0)
        off += 4L * pitch * rec[4];
    else if (dir == 1)
        off -= 4L * rec[4];
    else if (dir == 2)
        off -= 4L * pitch * rec[4];
    else
        off += 4L * rec[4];

    if (mode == 0)
        sprite24_stamp_bounded(stream, surf, surf_size, off, pitch, 0, 0);
    else if (mode == 2) /* 剪影色 = pitch 低字节（原版 sub_4E127 语义） */
        sprite24_stamp_bounded(stream, surf, surf_size, off, pitch, 1,
                               (uint8_t)pitch);
}

/* 0x4EB59：312x192 视口区（+1284 基）自 src 向 dst 的 zoom 倍像素
 * 复制变焦——横/纵每 zoom 个目的像素取同一源点，源点每 zoom 列/行
 * 步进 zoom（横.dst 行距 312+8=320；纵.src 行起点每 zoom 行 +320*zoom）。 */
static void screen_zoom_copy(int zoom, uint8_t *dst, const uint8_t *src)
{
    const uint8_t *srow = src + 1284;
    uint8_t *d = dst + 1284;
    int rowrep = zoom;

    for (int row = 0; row < 192; row++) {
        const uint8_t *s = srow;
        int colrep = zoom;
        for (int col = 0; col < 312; col++) {
            *d++ = *s;
            if (--colrep == 0) {
                colrep = zoom;
                s += zoom;
            }
        }
        d += 8;
        if (--rowrep == 0) {
            rowrep = zoom;
            srow += 320 * zoom;
        }
    }
}

/* 0x1F42D curtain_step(step, frame)：上幕帘帧 frame @band(85-step, 82)、
 * 下幕帘帧 81 @band(165+step, 82)（水平对开，step 0=合拢 100=展开）→
 * blit_rows 呈现 → wait(1) → pkg_frame_free 双恢复。 */
static void curtain_step(int step, int frame)
{
    uint8_t *band = battle_shadow_layer() + 32904;
    void *w_top = pkg_frame_blit(g_death_fx_pkg, band, 456,
                                 85 - step, 82, frame);
    void *w_bot = pkg_frame_blit(g_death_fx_pkg, band, 456,
                                 165 + step, 82, 81);

    blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
    wait_bios_ticks(1);
    pkg_frame_free(band, 456, w_top);
    pkg_frame_free(band, 456, w_bot);
}

/* 0x1F1CC curtain_close(frame)：幕帘合拢（step 100→75→50→25→0，再 1、0
 * 收紧）→ 影带=当前屏快照 → 16 级变焦放大（zoom 1..16）逐级叠帧
 * frame@work(89,86) + 帧81@(169,86) + palette(16..255, j) 亮起并整屏
 * 呈现（每级 wait(1)）→ 收尾 kbd_flush。 */
static void curtain_close(int frame)
{
    uint8_t *work = (uint8_t *)malloc(FD2_VRAM_SIZE);
    uint8_t *shadow = battle_shadow_layer();
    int zoom = 1;

    if (!work)
        return;
    memcpy(work, vram_base(), FD2_VRAM_SIZE);
    for (int i = 4; i >= 0; --i)
        curtain_step(25 * i, frame);
    curtain_step(1, frame);
    curtain_step(0, frame);

    memcpy(shadow, work, FD2_VRAM_SIZE);
    memset(work, 0, FD2_VRAM_SIZE);
    for (int j = 0; j < 16; j++) {
        void *w;
        screen_zoom_copy(zoom, work, shadow);
        w = pkg_frame_blit(g_death_fx_pkg, work, 320, 89, 86, frame);
        free(w);
        w = pkg_frame_blit(g_death_fx_pkg, work, 320, 169, 86, 81);
        free(w);
        palette_apply_range(16, 255, j);
        memcpy(vram_base(), work, FD2_VRAM_SIZE);
        wait_bios_ticks(1);
        zoom++;
    }
    free(work);
    kbd_flush();
}

/* 0x1F30A curtain_open(frame)：17 级变焦收缩（zoom 17..1）+ palette
 * (16..255, i) 渐暗（每级叠帧同 curtain_close → 整屏呈现）→ 影带重渲
 * 染地形+实体（field_tile_render 13x8 + ents_render_all）→ 幕帘展开
 * （step 0,25,50,75,100）+ 调光复位（sub_1F42D 序列自带呈现）。 */
static void curtain_open(int frame)
{
    uint8_t *work = (uint8_t *)malloc(FD2_VRAM_SIZE);
    uint8_t *shadow = battle_shadow_layer();
    int zoom = 17;

    if (!work)
        return;
    memset(work, 0, FD2_VRAM_SIZE);
    for (int i = 16; i >= 0; --i) {
        void *w;
        screen_zoom_copy(zoom, work, shadow);
        w = pkg_frame_blit(g_death_fx_pkg, work, 320, 89, 86, frame);
        free(w);
        w = pkg_frame_blit(g_death_fx_pkg, work, 320, 169, 86, 81);
        free(w);
        palette_apply_range(16, 255, i);
        memcpy(vram_base(), work, FD2_VRAM_SIZE);
        wait_bios_ticks(1);
        zoom--;
    }
    free(work);
    field_render_viewport_ext(shadow + 32904, 456, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
    field_render_entities(shadow);
    for (int j = 0; j <= 4; ++j)
        curtain_step(25 * j, frame);
}

/* 0x1A866 phase_status_tick(phase)：相位开始状态结算（两循环均按
 * rec[6]==phase 且 !(flags&1) 过滤）——
 *   循环1 中毒（rec[0x25]!=0）：dmg=Max/10（g_disp_num_b 同值供文本
 *   变量），HP=max(HP-dmg,0)；镜头聚焦 + 对话框（icon=rec[7]）文本
 *   487 + kbd_flush + wait_key_ticks(10) + 收框；
 *   battle_death_anim（毒发致死演出）+ g_state_post_action[g_state](0)；
 *   循环2 状态计时 +34..+39（6 槽）：非零先减，减到 0 → 对话框文本
 *   481+k + 同款收尾 + ent_recompute_derived。
 * battle_turn_end 于友军相(1)/敌方相(0)/回合收尾(2=玩家侧)各调一次。 */
static void phase_status_tick(int phase)
{
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (!rec[0x25] || rec[6] != phase || (rec[5] & 1u))
            continue;
        int hp = *(uint16_t *)(rec + 64);
        int mx = *(uint16_t *)(rec + 66);
        int dmg = mx / 10;
        g_disp_num_b = dmg;                      /* 0x53AE1 文本变量 */
        hp -= dmg;
        if (hp < 0)
            hp = 0;
        *(uint16_t *)(rec + 64) = (uint16_t)hp;
        g_text_busy = 0;
        camera_focus_ent(i);
        g_text_busy = 1;
        dialog_backdrop_load(rec[7]);
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W,
                        vram_base() + 40739, 487, g_pkg_fdtxt0);
        kbd_flush();
        wait_key_ticks(10);
        dialog_backdrop_restore();
    }
    battle_death_anim();
    g_state_post_action[g_state](0);
    for (int j = 0; j < g_ent_count; j++) {
        uint8_t *rec = g_ent_table[j];
        for (int k = 0; k < 6; k++) {
            if (rec[6] != phase || (rec[5] & 1u))
                continue;
            if (!rec[34 + k])
                continue;
            if (--rec[34 + k] != 0)
                continue;
            g_text_busy = 0;
            camera_focus_ent(j);
            g_text_busy = 1;
            dialog_backdrop_load(rec[7]);
            text_render_box(1, 19, 74, 205, FD2_SCREEN_W,
                            vram_base() + 40739, 481 + k, g_pkg_fdtxt0);
            kbd_flush();
            wait_key_ticks(10);
            dialog_backdrop_restore();
            ent_recompute_derived(j);
        }
    }
}

/* 0x1A7BD/0x1A7F1：AI 相（友军/敌方）前后装卸 FDOTHER blk64
 * （dword_53B0F；53AF9 存档标志门控）。blk64 = attack_anim_play 的
 * 近战音效包（sfx_play 第一参），敌方相 battle FX 场景共用。 */
static void ai_phase_fx_load(void)
{
    if (!g_save_flag_53af9)
        return;
    g_pkg_fdother_64 = NULL;
    g_pkg_fdother_64 = dat_load_block("FDOTHER.DAT", NULL, 64);
}

static void ai_phase_fx_free(void)
{
    if (!g_save_flag_53af9)
        return;
    free(g_pkg_fdother_64);
}

/* ==== 回合结束/新回合开始（0x1A30B 全量重写，2026-09-08 逐指令定案）====
 * 链路（压栈/跳转逐段核对；旧实现只有单遍回复+相位骨架）：
 *   ① wait(1)
 *   ② 回复 pass A：合格单位（type==2 且 !(flags&0x81) 且 +0x25/+0x26==0
 *      且 HP!=Max）逐个 sub_1DA16(band,456,u,2,0xFD) 白剪影重绘 →
 *      blit_rows 呈现；任一命中 → sfx(fdother_31,#4) → wait(0)
 *   ③ 回复 pass B：同过滤 HP=min(HP+Max/5,Max) + sub_1DA16 mode0 复原
 *      重绘 + sub_13512(u)（+5 bit7 暂置已行动）→ blit → wait(0)
 *   ④ anim_tick → scan(1) → sub_1A866(1)（NPC 中毒/状态计时）→
 *      pending 出 → sub_1A7BD（53AF9 时装 FDOTHER blk64）→ 友军 NPC 相
 *      → sub_1A7F1 → pending 出
 *   ⑤ 音乐切换：bgm[s]!=alt[s] → music_play(-1,0)；幕帘关(帧82) →
 *      delay20 → 幕帘开(帧82) → battle_load_map（清 bit7=敌方可动）
 *      → anim_tick → scan(0) → sub_1A866(0) → pending 出 →
 *      music_play(alt[s]) → sub_1A7BD → 敌方相 → sub_1A7F1 → pending 出
 *   ⑥ g_turn_count++；bgm!=alt → music_play(-1,0)；幕帘关(帧80) →
 *      delay150 → 幕帘开(帧80) → text_busy=0 → battle_load_map（第二次
 *      = 新玩家回合）→ anim_tick → music_play(bgm[s])
 *   ⑦ 回合横幅两段（0x1A620..0x1A76A，同 save_game_load 0x104A1 尾）：
 *      段1 帧 83..91 直贴 VRAM(120,84)（esi>6 盖 3 位回合数色 42
 *      @(171,91)）delay70/帧 + pkg_frame_free 恢复（esi==8 多歇 500ms）；
 *      段2 帧 91 入带 (116,k²+84)（k∈{2,3,4,9}）数字 @+167+456*(k²+90)
 *      → blit → wait(1) → free 恢复
 *   ⑧ anim_tick → delay200 → cur_unit=0 → scan(2) → sub_1A866(2)（玩家
 *      侧中毒/状态计时=回合开始结算）→ text_busy=1 → camera_focus(0)
 *      → kbd_flush
 * 注：scan 相位值与相的对应按实际流程序定案——scan(1) 在友军相前、
 * scan(0) 在敌方相前、scan(2) 收尾（旧注释"0=友军前/1=敌方前"反了）。 */
void battle_turn_end(void)
{
    int regen_any;
    uint8_t *band;

    wait_bios_ticks(1);                          /* 0x1A328 */
    band = battle_shadow_layer() + 32904;

    /* ② pass A：剪影(pitch456→色 0xC8 淡蓝白)标记全部将回复单位
     * （0x1A332..0x1A3E4） */
    regen_any = 0;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (rec[6] == 2 && (rec[5] & 0x81) == 0
            && !rec[0x25] && !rec[0x26]
            && *(uint16_t *)(rec + 0x40) != *(uint16_t *)(rec + 0x42)) {
            unit_sprite_stamp(band, 456, i, 2, 0xFD);   /* 0xFD=死参数照抄 */
            regen_any = 1;
        }
    }
    blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
    if (regen_any)
        sfx_play(g_pkg_fdother_31, 4, 1);        /* 0x1A3D5 */
    wait_bios_ticks(0);

    /* ③ pass B：HP += Max/5（clamp）+ 普通重绘 + 暂置已行动（0x1A3F2..） */
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (rec[6] == 2 && (rec[5] & 0x81) == 0
            && !rec[0x25] && !rec[0x26]
            && *(uint16_t *)(rec + 0x40) != *(uint16_t *)(rec + 0x42)) {
            int hp = *(uint16_t *)(rec + 0x40);
            int mx = *(uint16_t *)(rec + 0x42);
            hp += mx / 5;
            if (hp > mx) hp = mx;
            *(uint16_t *)(rec + 0x40) = (uint16_t)hp;
            unit_sprite_stamp(band, 456, i, 0, 0);
            sub_13512(i);                        /* +5 bit7（随后被清） */
        }
    }
    blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
    wait_bios_ticks(0);

    /* ④ 友军 NPC 相（scan(1)+状态结算(1)+blk64 装卸夹持） */
    anim_tick_update(0);
    battle_turn_event_scan(1);
    phase_status_tick(1);
    if (g_state_pending)
        return;
    ai_phase_fx_load();                          /* 0x1A7BD */
    battle_phase_ally_npc();
    ai_phase_fx_free();                          /* 0x1A7F1 */
    if (g_state_pending)
        return;

    /* ⑤ 幕帘转场 + 音乐切敌方相 + 敌方相 */
    if (g_state_bgm[g_state] != g_state_bgm_alt[g_state])
        music_play(-1, 0);
    curtain_close(0x52);                         /* 0x1F1CC 帧 82 变体 */
    delay_ms(20);
    curtain_open(0x52);                          /* 0x1F30A */
    battle_load_map();                           /* 清 bit7 → 敌方可动 */
    anim_tick_update(0);
    battle_turn_event_scan(0);
    phase_status_tick(0);                        /* 敌方中毒/状态计时 */
    if (g_state_pending)
        return;
    music_play(g_state_bgm_alt[g_state], 0);
    ai_phase_fx_load();
    battle_phase_enemy();
    ai_phase_fx_free();
    if (g_state_pending)
        return;

    /* ⑥ 新回合：回合数++、幕帘转场（帧 80 变体）、二次 load_map、
     * 音乐回玩家相（bgm 表） */
    g_turn_count++;
    if (g_state_bgm[g_state] != g_state_bgm_alt[g_state])
        music_play(-1, 0);
    curtain_close(0x50);
    delay_ms(150);
    curtain_open(0x50);
    g_text_busy = 0;
    battle_load_map();                           /* 新玩家回合 */
    anim_tick_update(0);
    music_play(g_state_bgm[g_state], 0);

    /* ⑦ 回合横幅段 1：帧 83..91 直贴 VRAM（0x1A620..0x1A69C）：
     * delay70 → (i==8 再 delay500) → free 恢复（末帧多停 500ms 后才擦）。 */
    for (int j = 0; j < 9; j++) {
        void *work = pkg_frame_blit(g_death_fx_pkg, vram_base(), 320,
                                    120, 84, 83 + j);
        if (j > 6)
            number_stamp_digits(vram_base() + 29291, 320,
                                g_turn_count, 42, 3);
        delay_ms(70);
        if (j == 8)
            delay_ms(500);
        pkg_frame_free(vram_base(), 320, work);
    }
    /* 段 2：帧 91 平方缓降入带 k²+84（k: 2,3,4,5→9 → 88/93/100/165） */
    for (int k = 2; k < 6; k++) {
        if (k == 5)
            k = 9;
        int ky = k * k;
        void *work = pkg_frame_blit(g_death_fx_pkg, band, 456,
                                    116, ky + 84, 91);
        number_stamp_digits(band + 167 + 456L * (ky + 90), 456,
                            g_turn_count, 42, 3);
        blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
        wait_bios_ticks(1);
        pkg_frame_free(band, 456, work);
    }

    /* ⑧ 收尾：回合开始状态结算（玩家侧）+ 镜头复位 */
    anim_tick_update(0);
    delay_ms(200);
    g_cur_unit_idx = 0;
    battle_turn_event_scan(2);
    phase_status_tick(2);
    g_text_busy = 1;
    camera_focus_ent(0);
    kbd_flush();
}

void enemy_phase_check(void)
{
    int all_done = 1;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if ((rec[5] & 0x81) == 0 && rec[6] == 2 && !rec[38])
            all_done = 0;
    }
    if (all_done) {
        g_transition_busy = 0;
        g_text_busy = 0;
        battle_turn_end();
        g_text_busy = 1;
        g_transition_busy = 1;
    }
}

void battle_phase_ally_npc(void)
{
    g_text_busy = 0;
    for (int i = 0; i < g_ent_count; i++) {
        g_text_busy = 0;
        kbd_flush();
        g_pending_event_id = 255;
        uint8_t *rec = g_ent_table[i];
        if (rec[6] == 1 && (rec[5] & 0x81) == 0 && !rec[38])
            ai_unit_act(i, 1);
        event_script_dispatch(i);
        g_state_post_action[g_state](i);
        if (g_state_pending)
            break;
    }
}
/* ==== AI 决策层全量重建（2026-09-08，逐指令反汇编定案；撤销旧
 * "最近敌步进"启发式近似）================================================
 * 评分三方案：ai_attack_scan(0x14237)=普攻 / ai_score_targets(0x1598A)=法术 /
 * ai_pick_move(0x1567E)=道具·武器 —— ai_decide_attack(0x14EF0) 三分择优
 * （全<6 待机）→ plan_a(0x15055 道具·贯穿) / plan_b(0x15311 法术) /
 * plan_c(0x1548E 普攻贴邻)。执行：ai_unit_act(0x13A9F) 11 行为分发 +
 * ai_move_to(0x14B78) 移动 + ai_attack_now(0x13FD4 再生 tick)。 */

/* 0x14B16 field_candidates_collect：全图扫洪泛候选字节（cell[3] != 255，
 * 带符号读）→ (x,y) 列表。 */
static int field_candidates_collect(uint8_t *out, int cap_pairs)
{
    int n = 0;
    if (!g_field_map)
        return 0;
    for (int y = 0; y < g_field_h; y++) {
        for (int x = 0; x < g_field_w; x++) {
            if ((int8_t)field_cell(x, y)[3] != -1) {
                if (out && n < cap_pairs) {
                    out[2 * n] = (uint8_t)x;
                    out[2 * n + 1] = (uint8_t)y;
                }
                n++;
            }
        }
    }
    return n;
}

/* 0x149F8 ray_targets_collect(out, from_x, from_y, to_x, to_y, len, ally)：
 * 直线贯穿收集。方向由 from→to 定单轴；游标自 to 起沿该方向步进 len 格
 * （借 g_cursor_x/y 作游标，保存/恢复）；阵营过滤：ally==0 收 ent[+6]!=0、
 * ally!=0 收 ent[+6]==0（与 shape_targets_collect 数值方向相反，按原版
 * 表达式照录）。返回收集数。 */
int ray_targets_collect(uint8_t *out, int from_x, int from_y,
                        int to_x, int to_y, int len, int ally)
{
    int n = 0, sx = 0, sy = 0;

    if (to_x == from_x)
        sy = (to_y <= from_y) ? 1 : -1;
    else
        sx = (to_x <= from_x) ? 1 : -1;
    unsigned keep_x = g_cursor_x, keep_y = g_cursor_y;
    g_cursor_x = (uint32_t)to_x;
    g_cursor_y = (uint32_t)to_y;
    for (int i = 0; i < len; i++) {
        g_cursor_x += (uint32_t)sx;
        g_cursor_y += (uint32_t)sy;
        if ((int)g_cursor_x < g_field_w && (int)g_cursor_x >= 0
            && (int)g_cursor_y < g_field_h && (int)g_cursor_y >= 0) {
            int ent = ent_at_cursor();
            if (ent != -1) {
                int side = g_ent_table[ent][6];
                if ((ally == 0 && side != 0) || (ally != 0 && side == 0))
                    out[n++] = (uint8_t)ent;
            }
        }
    }
    g_cursor_x = keep_x;
    g_cursor_y = keep_y;
    return n;
}

/* 0x1F04A battle_face_target(att, def)：主轴朝向（dx<=dy 纵向 0/2，
 * 否则横向 3/1）。duel_scene_load 内亦双向调用（同式）。 */
void battle_face_target(int attacker, int defender)
{
    if (!g_ent_table || attacker < 0 || defender < 0
        || attacker >= g_ent_count || defender >= g_ent_count)
        return;
    uint8_t *a = g_ent_table[attacker];
    const uint8_t *d = g_ent_table[defender];
    int dx = abs((int)a[0] - (int)d[0]);
    int dy = abs((int)a[1] - (int)d[1]);
    if (dx <= dy)
        a[3] = a[1] <= d[1] ? 0 : 2;
    else
        a[3] = a[0] <= d[0] ? 3 : 1;
}

/* 0x1DEBE counter-attack test（sub_14237 评分用）：目标 unit 于格 (x,y)
 * ——已行动 → -1；曼哈顿 ≠1 → -1；无低半区装备武器或 min 射程[11]>1
 * （非近战）→ -1；否则 1（可反击）。 */
static int ai_counter_test(int unit, int x, int y)
{
    uint8_t *rec = g_ent_table[unit];
    if (rec[38])
        return -1;
    if (abs((int)rec[0] - x) + abs((int)rec[1] - y) != 1)
        return -1;
    int slot = equipped_slot_find(unit, 0);
    if (slot == -1)
        return -1;
    const uint8_t *w = weapon_table_entry(inventory_get_item(unit, slot));
    if (w && (unsigned)w[11] > 1)
        return -1;
    return 1;
}

/* 0x15DA2 ai_score_field(field, n, targets, weight)：目标 ent[field]==0
 * 每个加 weight。法术 17-19（field=spell+17=34/35/36 吸取标记）与
 * 26/27（field=37/38 状态）委托。 */
static int ai_score_field(int field, int n, const uint8_t *targets, int weight)
{
    int score = 0;
    for (int i = 0; i < n; i++)
        if (g_ent_table[targets[i]][field] == 0)
            score += weight;
    return score;
}

/* 0x15B77 ai_score_effect(spell, n, targets)（按效果表索引分类）：
 *  <13 伤害系（10-12 跳过地形免疫目标）：目标 HP>=威力→8、能秒(HP<威力)
 *      →24；ent[+8]==0 无名杂兵 ×1.5（fpu 1.5 截断）；
 *  13-16 吸取系：HP>2/3Max→8、1/3..2/3→3、<1/3→0；ent[+52]&1 → ×2；
 *  17-19 → ai_score_field(spell+17, 3)；
 *  20/21 → 目标 ent[+37]/[+38]!=0（中毒/麻痹）各 +6；
 *  22 → 目标 ent[+39]==0（未沉默）且 spells_collect>0（会施法）+6；
 *  26/27 → ai_score_field(37/38, 4)。 */
static int ai_score_effect(int spell, int n, const uint8_t *targets)
{
    const uint8_t *ep = effect_param_entry(spell);
    int power = ep ? (int16_t)(ep[0] | ((uint16_t)ep[1] << 8)) : 0;
    int score = 0;

    if (spell < 13) {
        for (int i = 0; i < n; i++) {
            if (spell >= 10 && sub_1F183(targets[i]))
                continue;                        /* 10-12 对地形免疫者无效 */
            uint8_t *rec = g_ent_table[targets[i]];
            int s = *(uint16_t *)(rec + 64) >= power ? 8 : 24;
            if (rec[8] == 0)
                s = (int)((double)s * 1.5);
            score += s;
        }
        return score;
    }
    if (spell < 17) {
        for (int i = 0; i < n; i++) {
            uint8_t *rec = g_ent_table[targets[i]];
            int hp = *(uint16_t *)(rec + 64);
            int mx = *(uint16_t *)(rec + 66);
            int s = mx / 3 <= hp ? (mx / 2 <= hp ? 0 : 3) : 8;
            if (rec[52] & 1u)
                s *= 2;
            score += s;
        }
        return score;
    }
    if (spell < 20)
        return ai_score_field(spell + 17, n, targets, 3);
    switch (spell) {
    case 20:
        for (int i = 0; i < n; i++)
            if (g_ent_table[targets[i]][37])
                score += 6;
        break;
    case 21:
        for (int i = 0; i < n; i++)
            if (g_ent_table[targets[i]][38])
                score += 6;
        break;
    case 22:
        for (int i = 0; i < n; i++) {
            uint8_t *rec = g_ent_table[targets[i]];
            uint8_t known[12];
            if (!rec[39] && spells_collect(targets[i], known) > 0)
                score += 6;
        }
        break;
    case 26: return ai_score_field(37, n, targets, 4);
    case 27: return ai_score_field(38, n, targets, 4);
    default: break;
    }
    return score;
}

/* 0x1598A ai_score_targets(unit, mode)：法术方案评分。spells_collect 展开
 * 位图（rec[+39]!=0 沉默 → 整体跳过）；逐法术：ep[5] MP 消耗 > 自身
 * MP(+68) 跳过；ep[3] 射程洪泛候选圈；每候选格 ep[4] 形状收目标
 * （阵营 = mode ? ep[6] : ep[6]==0 取反）；ai_score_effect 打分 > 当前或
 * 平局且威力大 → 更新 g_ai_score_target/x/y/item_slot（item_slot 存
 * 法术 id，plan_b 的 exec 表键即它）。 */
int ai_score_targets(int unit, int mode)
{
    g_ai_score_target = 0;
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    uint8_t *rec = g_ent_table[unit];
    uint8_t spells[12];
    uint8_t *cands = (uint8_t *)malloc(2048);
    uint8_t targets[32];
    int best_power = 0;

    int n_spells = 0;
    if (!rec[39])
        n_spells = spells_collect(unit, spells);
    if (cands && n_spells > 0) {
        int mp = *(uint16_t *)(rec + 68);
        for (int i = 0; i < n_spells; i++) {
            const uint8_t *ep = effect_param_entry(spells[i]);
            if (!ep || (unsigned)ep[5] > (unsigned)mp)
                continue;
            field_flood_seed(0, rec[0], rec[1], ep[3]);
            int n_cand = field_candidates_collect(cands, 1024);
            field_reset_candidates();
            for (int j = 0; j < n_cand; j++) {
                int cx = cands[2 * j], cy = cands[2 * j + 1];
                int ally = mode ? ep[6] : (ep[6] == 0);
                int n_t = shape_targets_collect(targets, cx, cy,
                                                ep[4], 0, ally);
                field_reset_candidates();
                if (n_t <= 0)
                    continue;
                int s = ai_score_effect(spells[i], n_t, targets);
                int power = (int16_t)(ep[0] | ((uint16_t)ep[1] << 8));
                if (s > g_ai_score_target
                    || (s == g_ai_score_target && power > best_power)) {
                    g_ai_score_target = s;
                    g_ai_target_x = cx;
                    g_ai_target_y = cy;
                    g_ai_target_item_slot = spells[i];
                    best_power = power;
                }
            }
        }
    }
    free(cands);
    return 0;
}

/* 0x15880 ai_score_item(item, n, targets)（武器表 [13] 效果类型分类）：
 * 5/13 群体伤害：HP<=Max/3→8、Max/3..Max/2→3、>Max/2→0（残血收割）；
 *   ent[+52] bit7 → ×3；
 * 20/21/24 单体：威力 = 20/21 查效果参数表（weapon[14..15]=效果 id）、
 *   24 直接 weapon[14..15]；目标 HP<=威力 →18（能秒）否则 8。 */
static int ai_score_item(int item, int n, const uint8_t *targets)
{
    const uint8_t *w = weapon_table_entry(item);
    if (!w || n <= 0)
        return 0;
    int power = (int16_t)(w[14] | ((uint16_t)w[15] << 8));
    int type = w[13];
    int score = 0;

    if (type == 5 || type == 13) {
        for (int i = 0; i < n; i++) {
            uint8_t *rec = g_ent_table[targets[i]];
            int hp = *(uint16_t *)(rec + 64);
            int mx = *(uint16_t *)(rec + 66);
            int s = hp > mx / 3 ? (hp > mx / 2 ? 0 : 3) : 8;
            if ((int8_t)rec[52] < 0)
                s *= 3;
            score += s;
        }
    } else if (type == 20 || type == 21 || type == 24) {
        if (type != 24) {
            const uint8_t *ep = effect_param_entry(power);
            power = ep ? (int16_t)(ep[0] | ((uint16_t)ep[1] << 8)) : 0;
        }
        for (int i = 0; i < n; i++) {
            uint8_t *rec = g_ent_table[targets[i]];
            score += power >= *(uint16_t *)(rec + 64) ? 18 : 8;
        }
    }
    return score;
}

/* 0x1567E ai_pick_move(unit, mode)：道具/武器攻击方案评分。遍历 8 道具槽
 * （inventory_used_count>0 才做）；[13]!=0 才有效；[16] 攻击距离（>0xF →
 * 257 特殊）；射程圈 = shape_targets_collect(out=NULL, 自身, shape=range_lo,
 * radius=range_hi（257→1/1 即四邻）) 洪泛标记 + field_candidates_collect；
 * 每候选格：[16]<=0xF → shape_targets_collect([18] 形状)；否则
 * ray_targets_collect(aim=候选格, origin=自身, len=[16]-16)；ai_score_item
 * 打分更高 → 更新 g_ai_score_move/x/y/item_slot（存槽号）。 */
int ai_pick_move(int unit, int mode)
{
    g_ai_score_move = 0;
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    uint8_t *rec = g_ent_table[unit];
    uint8_t *cands = (uint8_t *)malloc(2048);
    uint8_t targets[32];
    int used = inventory_used_count(unit);

    if (cands && used > 0) {
        for (int slot = 0; slot < used && slot < 8; slot++) {
            int item = inventory_get_item(unit, slot);
            const uint8_t *w = weapon_table_entry(item);
            if (!w || !w[13])
                continue;
            int range = w[16] > 0xF ? 257 : w[16];
            /* 0x1575A：射程圈洪泛（out=NULL 仅标记；257 → dl=1/dh=1） */
            shape_targets_collect(NULL, rec[0], rec[1],
                                  range & 0xFF, range >> 8, 0);
            int n_cand = field_candidates_collect(cands, 1024);
            field_reset_candidates();
            for (int j = 0; j < n_cand; j++) {
                int cx = cands[2 * j], cy = cands[2 * j + 1];
                int n_t;
                if (w[16] <= 0xF) {
                    int ally = mode ? w[17] : (w[17] == 0);
                    n_t = shape_targets_collect(targets, cx, cy,
                                                w[18], 0, ally);
                } else {
                    n_t = ray_targets_collect(targets, cx, cy,
                                              rec[0], rec[1],
                                              w[16] - 16, 0);
                }
                field_reset_candidates();
                if (n_t <= 0)
                    continue;
                int s = ai_score_item(item, n_t, targets);
                if (s > g_ai_score_move) {
                    g_ai_score_move = s;
                    g_ai_move_x = cx;
                    g_ai_move_y = cy;
                    g_ai_move_item_slot = slot;
                }
            }
        }
    }
    free(cands);
    return 0;
}

/* 0x14237 sub_14237 ai_attack_scan(unit, mode)：普攻方案评分。装备武器
 * 射程 [11]/[12]；自身位置 rec[59] 移动力洪泛（ZoC + 我方格排除）→
 * 候选格 = 可达站位；每站位（自身 terrain_immune → 按候选格地形修正
 * 攻/防 —— 与 strike 公式极性相反，照录 0x143D5 jz）以 [12]/[11] 收
 * 目标（ally = mode?0:1）；每目标：diff = 己方ATK-敌方DEF（地形修正后，
 * 目标免疫亦同极性）——diff<=2 → 0 分否则 8；diff>目标 HP → ×2 且 18
 * 分（能秒）；可反击（ai_counter_test==1）→ diff += 己方DEF-敌方ATK；
 * 无名杂兵(rec[8]==0) → diff×1.5；分高或平局 diff 大者胜 → 更新
 * g_ai_score_attack / 攻击站位 g_ai_attack_x/y / g_ai_target_unit。 */
static void ai_attack_scan(int unit, int mode)
{
    g_ai_score_attack = 0;
    s_atk_tiebreak = 0;
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    uint8_t *rec = g_ent_table[unit];
    int atk = *(uint16_t *)(rec + 72);
    int dfn = *(uint16_t *)(rec + 74);
    int wslot = equipped_slot_find(unit, 0);
    if (wslot == -1)
        return;
    const uint8_t *w = weapon_table_entry(inventory_get_item(unit, wslot));
    if (!w)
        return;
    int rmin = w[11], rmax = w[12];
    uint8_t *cands = (uint8_t *)malloc(2048);
    uint8_t *targets = (uint8_t *)malloc(100);
    /* 0x14299：cls = terrain_immune ? 19 : rec[32]（无 rec-28 特例） */
    int scan_cls = terrain_immune_test(unit) ? 19 : rec[32];

    field_reset_candidates();
    enemies_zoc_mark(mode);
    field_flood_seed(scan_cls, rec[0], rec[1], rec[59]);
    allies_cells_block(unit, mode);
    int n_cand = field_candidates_collect(cands, 1024);
    field_reset_candidates();
    for (int i = 0; i < n_cand; i++) {
        int cx = cands[2 * i], cy = cands[2 * i + 1];
        int my_atk = atk, my_dfn = dfn;
        if (terrain_immune_test(unit)) {
            uint8_t info[8];
            tile_lookup(cx, cy, info);
            my_atk += atk * g_tile_atk_pct[info[5]] / 100;
            my_dfn += dfn * g_tile_def_pct[info[5]] / 100;
        }
        int ally = mode ? 0 : 1;                /* 0x1431B: !mode → 非敌 */
        int n_t = shape_targets_collect(targets, cx, cy, rmax, rmin, ally);
        field_reset_candidates();
        for (int j = 0; j < n_t; j++) {
            uint8_t *trec = g_ent_table[targets[j]];
            int t_atk = *(uint16_t *)(trec + 72);
            int t_dfn = *(uint16_t *)(trec + 74);
            if (sub_1F183(targets[j])) {
                uint8_t info[8];
                tile_lookup(trec[0], trec[1], info);
                t_atk += t_atk * g_tile_atk_pct[info[5]] / 100;
                t_dfn += t_dfn * g_tile_def_pct[info[5]] / 100;
            }
            int diff = my_atk - t_dfn;
            int score = diff <= 2 ? 0 : 8;
            if (diff > *(uint16_t *)(trec + 64)) {
                diff *= 2;
                score = 18;
            }
            if (ai_counter_test(targets[j], cx, cy) == 1)
                diff += my_dfn - t_atk;
            if (trec[8] == 0)
                diff = 3 * diff / 2;
            if (score > g_ai_score_attack
                || (score == g_ai_score_attack && diff > s_atk_tiebreak)) {
                s_atk_tiebreak = diff;
                g_ai_attack_x = cx;
                g_ai_attack_y = cy;
                g_ai_target_unit = targets[j];
                g_ai_score_attack = score;
            }
        }
    }
    free(targets);
    free(cands);
}

void battle_phase_enemy(void)
{
    /* IDA 0x1D8BA: first score every eligible enemy, allowing an immediate
     * action only for a high-confidence score; second pass executes all
     * eligible enemies.  Event dispatch and state hooks run even when an AI
     * action was skipped, matching the original loop boundaries. */
    for (int i = 0; i < g_ent_count; i++) {
        g_text_busy = 0;
        kbd_flush();
        g_pending_event_id = 255;
        uint8_t *rec = g_ent_table[i];
        int type = rec[6];
        if (type == 0 && (rec[5] & 0x81u) == 0 && !rec[38]) {
            ai_score_targets(i, type);
            ai_pick_move(i, type);
            if (g_ai_score_target >= 6 || g_ai_score_move >= 6)
                ai_unit_act(i, 0);
        }
        event_script_dispatch(i);
        g_state_post_action[g_state](i);
        if (g_state_pending)
            return;
    }

    for (int j = 0; j < g_ent_count; j++) {
        kbd_flush();
        g_pending_event_id = 255;
        uint8_t *rec = g_ent_table[j];
        int type = rec[6];
        if (type == 0 && (rec[5] & 0x81u) == 0 && !rec[38])
            ai_unit_act(j, type);
        event_script_dispatch(j);
        g_state_post_action[g_state](j);
        if (g_state_pending)
            break;
    }
}
/* ==== 近战执行链（0x1F0DC/0x1E739/0x1E7F6/0x1E611/0x1EB05/0x1E98C/
 * 0x1E856，2026-09-08 逐指令定案） ==================================== */

/* 0x1F0DC melee_adjacent_test(att, def)：贴邻判定——目标已行动(+38)→-1；
 * 曼哈顿≠1 →-1；无低半区装备武器 →-1；min 射程[11]≠1（非近战）→-1；
 * 其余返回该射程值（贴邻=1 即可反击）。 */
/* 0x1F0DC melee_adjacent_test(att, def)：贴邻且守方装备最小射程 1 的
 * 武器 → 1（可反击）；否则 -1。duel_scene_load 反击门（duel.c）共用。 */
int melee_adjacent_test(int att, int def)
{
    uint8_t *a, *d;
    int slot, r;
    const uint8_t *w;

    if (!g_ent_table || att < 0 || def < 0
        || att >= g_ent_count || def >= g_ent_count)
        return -1;
    a = g_ent_table[att];
    d = g_ent_table[def];
    if (d[38])
        return -1;
    if (abs((int)a[0] - (int)d[0]) + abs((int)a[1] - (int)d[1]) != 1)
        return -1;
    slot = equipped_slot_find(def, 0);
    if (slot == -1)
        return -1;
    w = weapon_table_entry(inventory_get_item(def, slot));
    if (!w)
        return -1;
    r = w[11];
    return r == 1 ? r : -1;
}

/* 0x1E739 hp_bar_stamp(dst, pitch, width)：HP/指示条分段打印
 * （g_death_fx_pkg RAW 帧：23 头/24 身/25 尾、29 空格/30 空尾；
 * 空/未满部分以 29 铺至 69 格 + 30 收尾）。 */
static void hp_bar_stamp(uint8_t *dst, int pitch, int width)
{
    int i = 0;

    if (!g_death_fx_pkg || !dst)
        return;
    if (width <= 0) {
        for (i = 0; i < 69; i++)
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 29),
                             dst + i, pitch);
        stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 30),
                         dst + i, pitch);
        return;
    }
    stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 23), dst, pitch);
    for (i = 1; i < width; i++)
        stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 24),
                         dst + i, pitch);
    stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 25),
                     dst + i, pitch);
    if (width < 70) {
        while (i < 69) {
            stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 29),
                             dst + i, pitch);
            i++;
        }
        stamp_raw_opaque(package_frame_ptr(g_death_fx_pkg, 30),
                         dst + i, pitch);
    }
}

/* 0x1E7F6 hp_bar_draw(dst, pitch, unit, coords)：宽 = 69×HP/MaxHP+1 像素，
 * 位置 = coords[0] + pitch*(coords[1]+6) + 7；HP==0 不画。 */
static void hp_bar_draw(uint8_t *dst, int pitch, int unit, const int *coords)
{
    uint8_t *rec;
    int hp, mx, width;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count || !dst || !coords)
        return;
    rec = g_ent_table[unit];
    hp = *(uint16_t *)(rec + 64);
    if (!hp)
        return;
    mx = *(uint16_t *)(rec + 66);
    if (mx <= 0)
        return;
    width = 69 * hp / mx + 1;
    hp_bar_stamp(dst + coords[0] + pitch * (coords[1] + 6) + 7, pitch, width);
}

/* 攻击动画帧表（§7.5：g_attack_anim_classes@0x61955 = 21 项指针 →
 * 6 张帧表 @0x619A9..0x619FD；表 = [u8 帧数][帧号,音效]×N，0xFF=无音效；
 * 类别 0x15..0x1B 远程/法器不查本表——原版直接越界读，宿主侧跳过）。 */
static const uint8_t s_anim_t0[] = { 6, 0, 0xFF, 1, 0, 2, 0xFF, 3, 0xFF, 4, 0xFF, 5, 0xFF };
static const uint8_t s_anim_t3[] = { 6, 6, 3, 7, 3, 8, 3, 9, 0xFF, 10, 0xFF, 11, 0xFF };
static const uint8_t s_anim_t4[] = { 6, 12, 0xFF, 13, 1, 14, 0xFF, 15, 0xFF, 16, 0xFF, 17, 0xFF };
static const uint8_t s_anim_t5[] = { 8, 18, 2, 19, 2, 20, 2, 21, 0xFF, 22, 0xFF, 23, 0xFF, 24, 0xFF, 25, 0xFF };
static const uint8_t s_anim_t6[] = { 8, 31, 5, 32, 5, 33, 5, 34, 0xFF, 35, 0xFF, 36, 0xFF, 37, 0xFF, 38, 0xFF };
static const uint8_t s_anim_t7[] = { 5, 26, 0xFF, 27, 3, 28, 0xFF, 29, 0xFF, 30, 0xFF };
static const uint8_t *const s_attack_anim_classes[21] = {
    s_anim_t0, s_anim_t0, s_anim_t0, s_anim_t3, s_anim_t4, s_anim_t5,
    s_anim_t6, s_anim_t7, s_anim_t6, s_anim_t0, s_anim_t3, s_anim_t5,
    s_anim_t0, s_anim_t6, s_anim_t7, s_anim_t6, s_anim_t3, s_anim_t3,
    s_anim_t0, s_anim_t6, s_anim_t6,
};

/* 0x1E98C attack_anim_play(att, unit)（2026-09-08 双参+逐指令重写）：
 * 武器/帧表取自 att（arg_0），动画贴位与闪白立绘取自 unit（arg_4）
 * ——attack_execute 传 (att, def)：攻击特效帧贴守方格。逐帧序：
 * 音效（包 = FDOTHER blk64/g_pkg_fdother_64：MISS=4、HIT=表 id、
 * 0xFF=无）→ HIT 且 i==0/1 时 unit 白剪影(0xFD)/复原（sub_1DA16 直写
 * VRAM，pitch 320）→ blk6 帧 blit → delay80 → pkg_frame_free
 * 恢复。类别 0x15..0x1B 不查表（原版越界读，宿主防越界跳过）。 */
static void attack_anim_play(int att, int unit)
{
    uint8_t *rec;
    const uint8_t *tbl, *w;
    int slot, x, y;

    if (!g_ent_table || att < 0 || att >= g_ent_count
        || unit < 0 || unit >= g_ent_count)
        return;
    rec = g_ent_table[unit];
    slot = equipped_slot_find(att, 0);
    if (slot == -1)
        return;
    w = weapon_table_entry(inventory_get_item(att, slot));
    if (!w || (unsigned)w[0] >= 21)
        return;
    tbl = s_attack_anim_classes[w[0]];
    x = 24 * ((int)rec[0] - (int)g_scroll_x) + 4;
    y = 24 * ((int)rec[1] - (int)g_scroll_y);
    for (int i = 0; i < tbl[0]; i++) {
        int frame = tbl[1 + 2 * i];
        int sfx = tbl[2 + 2 * i];
        if (sfx != 0xFF && g_pkg_fdother_64)
            sfx_play(g_pkg_fdother_64, g_attack_miss_flag ? 4 : sfx, 1);
        if (!g_attack_miss_flag) {
            /* 剪影 pitch 320 → 色 0x40 黄绿（0x1EA59 原版 push 0xFD 为
             * 死参数；直写 VRAM 无复原 blit，靠下一帧盖回） */
            if (i == 0)
                unit_sprite_stamp(vram_base() + 1284, 320, unit, 2, 0xFD);
            else if (i == 1)
                unit_sprite_stamp(vram_base() + 1284, 320, unit, 0, 0);
        }
        if (g_pkg_fdother_6) {
            void *work = pkg_frame_blit(g_pkg_fdother_6, vram_base(), 320,
                                        x, y, frame);
            delay_ms(80);
            pkg_frame_free(vram_base(), 320, work);
        }
    }
}

/* 0x1EC2A：攻击立绘屏幕坐标（含朝向偏移与边界回折）。 */
static void attack_sprite_pos(int unit, int coords[2])
{
    uint8_t *rec = g_ent_table[unit];

    coords[0] = 24 * ((int)rec[0] - (int)g_scroll_x) + 4;
    coords[1] = 24 * ((int)rec[1] - (int)g_scroll_y);
    if (rec[3] < 2) {
        coords[1] -= 18;
        if (coords[1] < 0)
            coords[1] += 5;
        if (coords[0] + 108 > 319)
            coords[0] -= 86;
        else
            coords[0] += 28;
    } else {
        if (coords[1] + 37 <= 199)
            coords[1] += 22;
        else
            coords[1] += 5;
        if (coords[0] - 86 < 0)
            coords[0] += 28;
        else
            coords[0] -= 88;
    }
}

/* 0x1EB05 attack_sprites_load(att, def) → 槽位坐标对（0x53A30..3C，
 * 2026-09-08 槽位语义定案）：槽 A = def（arg_4/sub_1EC2A 第二实参）
 * 立绘位；槽 B = att（arg_0）位，仅贴邻（melee_adjacent_test==1）时
 * 有效、否则 -1。10 帧 blk6 帧 39-48 双位对撞动画：每帧槽 A blit →
 * 槽 B blit → delay25 → i<9 双 pkg_frame_free 恢复；
 * 末帧（i==9）保留在屏，工作区普通 free（原版 0x1EC05 free 不恢复）。 */
static int attack_sprites_load(int att, int def)
{
    int a_xy[2], b_xy[2] = { -1, -1 };
    int adjacent = melee_adjacent_test(att, def);

    if (!g_ent_table || def < 0 || def >= g_ent_count)
        return 0;
    attack_sprite_pos(def, a_xy);
    if (adjacent == 1 && att >= 0 && att < g_ent_count)
        attack_sprite_pos(att, b_xy);
    s_attack_slot_a[0] = a_xy[0]; s_attack_slot_a[1] = a_xy[1];
    s_attack_slot_b[0] = b_xy[0]; s_attack_slot_b[1] = b_xy[1];
    for (int i = 0; i < 10; i++) {
        void *wa = NULL, *wb = NULL;

        if (g_pkg_fdother_6) {
            wa = pkg_frame_blit(g_pkg_fdother_6, vram_base(), 320,
                                a_xy[0], a_xy[1], i + 39);
            if (b_xy[0] != -1)
                wb = pkg_frame_blit(g_pkg_fdother_6, vram_base(), 320,
                                    b_xy[0], b_xy[1], i + 39);
        }
        delay_ms(25);
        if (i < 9) {
            if (wa)
                pkg_frame_free(vram_base(), 320, wa);
            if (wb)
                pkg_frame_free(vram_base(), 320, wb);
        } else {
            free(wa);                            /* 末帧保留（原版泄漏位） */
            free(wb);
        }
    }
    memcpy(s_attack_slot_a, a_xy, sizeof(a_xy));
    memcpy(s_attack_slot_b, b_xy, sizeof(b_xy));
    return 1;
}

/* 0x1E856 attack_execute(att, def, pos)：普攻结算 + HP 条动画。
 * 攻击次数 = 1；武器附魔[9]==3 → 2；rand%100<3 → 2（3% 二连击）。
 * 每次：battle_strike_formula（=0x1ECC7 普攻区拷贝，res[0]=1 即
 * MISS → g_attack_miss_flag）→ attack_anim_play → HP 条 70×旧/Max 逐格
 * 降至新值（delay8/格）。返回是否有效结算。 */
static int attack_execute(int att, int def, const int *pos)
{
    uint8_t *drec;
    int res[6], strikes = 1, valid = 0;

    if (!g_ent_table || att < 0 || def < 0
        || att >= g_ent_count || def >= g_ent_count)
        return 0;
    drec = g_ent_table[def];
    {
        int slot = equipped_slot_find(att, 0);
        const uint8_t *w = slot >= 0
            ? weapon_table_entry(inventory_get_item(att, slot)) : NULL;
        if (w && w[9] == 3)
            strikes = 2;
    }
    if ((int)(fd2_rand() % 100) < 3)
        strikes = 2;

    for (int s = 0; s < strikes; s++) {
        int hp0 = *(uint16_t *)(drec + 64);
        int mx = *(uint16_t *)(drec + 66);
        int cells = mx > 0 ? 70 * hp0 / mx : 0;
        battle_strike_formula(att, def, res);    /* 返回值原版即弃用，结果写 res[] */
        g_attack_miss_flag = (uint8_t)res[0];  /* 1=未命中 */
        attack_anim_play(att, def);              /* 0x1E928：特效贴守方格 */
        valid |= res[0] == 0;
        int hp1 = *(uint16_t *)(drec + 64);
        int target = mx > 0 ? 70 * hp1 / mx : 0;
        while (cells >= target) {
            if (pos)
                hp_bar_stamp(vram_base() + 320 * (pos[1] + 6) + pos[0] + 7,
                             320, cells);
            delay_ms(8);
            cells--;
        }
    }
    return valid;
}

/* 0x1E611 melee_panel_render(coords, att, def)：反击前双方面板 =
 * 战场 13×8 合成 + blk6 帧 48 双立绘（槽A-4 恒画、槽B-4 仅
 * coords[2]≠−1；原版此处不调 pkg_frame_free——帧留在带上，工作区
 * 泄漏位宿主以 discard 收）+ 攻击指示条 55（槽A 右下）+ HP 条
 * （0x1E6A7 守方 def→槽A / 0x1E6FC 攻方 att→槽B）→ 整带呈现。
 * plan_c 传 (coords, unit, 目标)：目标条在槽A（=目标立绘位）。 */
static void melee_panel_render(const int *coords, int att, int def)
{
    uint8_t *band = battle_shadow_layer();

    if (!band)
        return;
    field_render_viewport_ext(band + 32904, 456, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
    field_render_entities(band);
    if (g_pkg_fdother_6) {
        void *work = pkg_frame_blit(g_pkg_fdother_6, band + 32904, 456,
                                    coords[0] - 4, coords[1] - 4, 48);
        pkg_frame_discard(work);
        if (coords[2] != -1) {
            work = pkg_frame_blit(g_pkg_fdother_6, band + 32904, 456,
                                  coords[2] - 4, coords[3] - 4, 48);
            pkg_frame_discard(work);
        }
    }
    hp_bar_stamp(band + 32904 + coords[0] + 456L * (coords[1] + 2) + 3,
                 456, 55);
    hp_bar_draw(band + 31076, 456, def, coords);
    if (coords[2] != -1)
        hp_bar_draw(band + 31076, 456, att, coords + 2);
    battle_shadow_blit();
}

/* ==== AI 移动/再生/让步原语 ========================================== */

/* 0x14B78 ai_move_to(dest_x, dest_y, unit, mode)：向目标点走近。
 * ①ZoC + 直达路径（真实移动力）→ 不可达时 ②28 预算全程路径 + 真实
 * 洪泛标记 → 沿路径最后可停留格为新目标（aim）→ ③真实移动力洪泛 +
 * 我方格排除 → 候选格取距 aim 最近（曼哈顿，平局 |dx-dy| 小者）→
 * 路径回溯 + walk_path_anim。返回步数（0=未移动）。
 * 洪泛 class 门（0x14BA6）：cls=rec[32]；terrain_immune→19；
 * rec[8]==28 → 1（覆盖免疫，与玩家侧 0x188F9 的 rec[7]==28→16 不同）。 */
/* 0x14B78 ai_move_to(dest_x, dest_y, unit, mode)（2026-09-08 按原始反汇编
 * 重排调用序，撤销旧版两处偏差：首次构建前多了 reset+洪泛（原版仅
 * enemies_zoc_mark，场已由前序调用清干净）；路径以洪泛回溯近似替代
 * 原版自含 DFS——路径形状与原版不同）：
 * ① 0x14C0E enemies_zoc_mark(mode) → field_path_build(cls, x, y,
 *    rec[59], flag=0)——自含 DFS，不构建洪泛场；
 * ② 不可达(255)时 0x14C53 reset + field_path_build(…, 28, flag=1) 取
 *    28 预算参考路径，再 reset+ZoC+洪泛(真实预算) 沿该路径取最后
 *    marker!=-1 的格为 aim；
 * ③ 0x14D4D reset+ZoC+洪泛(真实预算)+allies_cells_block+候选收集，
 *    以 aim 为基准取曼哈顿最近候选（平局 |dx-dy| 小者，再平局行主序
 *    先扫到者；无候选时目标即 aim——v46/v48 初值=aim 照录）；
 * ④ 0x14E61 reset+ZoC → field_path_build(…, flag=0, 目标=③) → reset
 *    → 走路动画。返回路径步数（0=未动，eax 经 0x11011 公共尾返回）。 */
static int ai_move_to(int dest_x, int dest_y, int unit, int mode)
{
    uint8_t path[256], path28[256], *cands;
    int aim_x, aim_y, move, plen;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    uint8_t *rec = g_ent_table[unit];
    move = rec[59];
    int cls = rec[32];
    if (terrain_immune_test(unit))
        cls = 19;
    if (rec[8] == 28)
        cls = 1;
    aim_x = dest_x;
    aim_y = dest_y;
    cands = (uint8_t *)malloc(2048);
    if (!cands)
        return 0;

    /* ① 首建（0x14C0E-0x14C47）：仅 ZoC 标记，自含 DFS */
    enemies_zoc_mark(mode);
    plen = field_path_build(cls, rec[0], rec[1], move, path, sizeof(path),
                            dest_x, dest_y, 0, NULL, NULL);
    if (plen == 255) {
        /* ② 28 预算参考路径 + 真实洪泛取最后可停留格（0x14C53-0x14D43） */
        field_reset_candidates();
        int p28 = field_path_build(cls, rec[0], rec[1], 28, path28,
                                   sizeof(path28), dest_x, dest_y, 1,
                                   NULL, NULL);
        if (p28 != 255) {
            field_reset_candidates();
            enemies_zoc_mark(mode);
            field_flood_seed(cls, rec[0], rec[1], move);
            int px = rec[0], py = rec[1];
            for (int i = 0; i < p28; i++) {
                switch (path28[i]) {
                case 0:  ++py; break;
                case 1:  --px; break;
                case 2:  --py; break;
                default: ++px; break;
                }
                if (px >= 0 && py >= 0 && px < g_field_w && py < g_field_h
                    && (int8_t)field_cell(px, py)[3] != -1) {
                    aim_x = px;
                    aim_y = py;
                }
            }
        }
    }
    /* ③ 候选格择近（0x14D4D-0x14E5B） */
    field_reset_candidates();
    enemies_zoc_mark(mode);
    field_flood_seed(cls, rec[0], rec[1], move);
    allies_cells_block(unit, mode);
    int n_cand = field_candidates_collect(cands, 1024);
    int best = -1, best_d = 0x7FFFFFFF, best_diag = 0x7FFFFFFF;
    for (int i = 0; i < n_cand; i++) {
        int cx = cands[2 * i], cy = cands[2 * i + 1];
        int dx = abs(cx - aim_x), dy = abs(cy - aim_y);
        int d = dx + dy, diag = abs(dx - dy);
        if (d < best_d || (d == best_d && diag < best_diag)) {
            best = i;
            best_d = d;
            best_diag = diag;
        }
    }
    int tx = aim_x, ty = aim_y;                /* v46/v48：无更近候选时=aim */
    if (best >= 0) {
        tx = cands[2 * best];
        ty = cands[2 * best + 1];
    }
    /* ④ 终建 + 走路（0x14E61-0x14EC4） */
    field_reset_candidates();
    enemies_zoc_mark(mode);
    plen = field_path_build(cls, rec[0], rec[1], move, path, sizeof(path),
                            tx, ty, 0, NULL, NULL);
    field_reset_candidates();
    if (plen > 0)
        walk_path_anim(unit, path, plen);
    free(cands);
    return plen;
}

/* 0x13FD4 ai_attack_now(unit)：再生 tick（非攻击）——满血/中毒(+0x25)/
 * 已行动(+0x26) 返回 0；否则 text_busy=0 + 聚焦 + wait(1) +
 * sfx(fdother_31,#4) + 白剪影重绘→blit→wait(1) + 普通重绘→blit→
 * wait(1)（sub_1DA16 双 pass，0x14045/0x1409E——旧调光近似已撤）+
 * HP += Max/5 clamp + text_busy=1，返回 1。 */
static int ai_attack_now(int unit)
{
    uint8_t *rec, *band;
    int hp, mx;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    rec = g_ent_table[unit];
    hp = *(uint16_t *)(rec + 64);
    mx = *(uint16_t *)(rec + 66);
    if (hp == mx || rec[37] || rec[38])
        return 0;
    band = battle_shadow_layer() + 32904;
    g_text_busy = 0;
    camera_focus_ent(unit);
    wait_bios_ticks(1);
    sfx_play(g_pkg_fdother_31, 4, 1);
    unit_sprite_stamp(band, 456, unit, 2, 0xFD);     /* 剪影色 0xC8；0xFD 死参数 */
    blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
    wait_bios_ticks(1);
    unit_sprite_stamp(band, 456, unit, 0, 0);        /* 复原重绘 */
    blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
    wait_bios_ticks(1);
    hp += mx / 5;
    if (hp > mx)
        hp = mx;
    *(uint16_t *)(rec + 64) = (uint16_t)hp;
    g_text_busy = 1;
    return 1;
}

/* 0x14121 ai_advance_foe(unit, mode)（2026-09-08 flag==2 DFS 全量忠实复刻，
 * 撤销"洪泛取到达代价最小对立单位"近似——多敌格在 28 预算内时它与
 * 原版 DFS 序选出的目标格不同，是"相同局面敌人走向不同"的根因之一）：
 * enemies_zoc_mark(mode)（无 reset——0x22BBE 公共尾/前序扫描均已清场）
 * 后 field_path_build(cls, x, y, 28, flag=2)：DFS 松弛 0x4E6DE 分支跳过
 * 0x40/0x80 检查（可穿越敌格、不受 ZoC 停），写 marker 后调
 * field_path_emit_foe_cell(0x4E703)——格 [flags]&0x40 则坐标写入 out 且
 * byte_60078=1；每次松弛到 0x40 格都覆盖且 DFS 无提前终止，故航点 =
 * 右/左/下/上 DFS 序全程最后一个被松弛的对立单位格。
 * 返回 255（28 预算内无可达敌格，0x141BF reset 后返 0）或航点=当前位
 * （0x141EC 守卫，直接返 0——text_busy 未动）→ 0；否则 text_busy=0 +
 * camera_focus + ai_move_to(航点)，text_busy=1，返回 moved（0x1421D
 * test eax）。 */
static int ai_retreat_corner(int unit, int mode)
{
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    uint8_t *rec = g_ent_table[unit];
    int cls = rec[32];
    if (terrain_immune_test(unit))
        cls = 19;
    if (rec[8] == 28)
        cls = 1;
    enemies_zoc_mark(mode);                    /* 0x14189：原版无 reset */
    int wx = rec[0], wy = rec[1];              /* 航点兜底=当前位（原版 out
                                                * 指向调用方栈未初始化；取
                                                * 确定值使病态局落入下方
                                                * ==当前位守卫=不移动） */
    int r = field_path_build(cls, rec[0], rec[1], 28, NULL, 0, 0, 0, 2,
                             &wx, &wy);
    if (r == 255) {
        field_reset_candidates();              /* 0x141BF */
        return 0;
    }
    field_reset_candidates();                  /* 0x141D4 */
    if (wx == (int)rec[0] && wy == (int)rec[1])
        return 0;                              /* 0x141EC 航点=当前位守卫 */
    g_text_busy = 0;                           /* 0x141F5 */
    camera_focus_ent(unit);
    int mv = ai_move_to(wx, wy, unit, mode);
    g_text_busy = 1;                           /* 0x14226 */
    return mv != 0;                            /* 0x1421D */
}

/* 0x13E9C sub_13E9C(unit, mode)：接近最近对立阵营单位（曼哈顿，原版
 * 不排除阵亡单位——照录）；无目标或已在目标位 → 0；移动成功 → 1。 */
static int ai_approach_nearest(int unit, int mode)
{
    int best = -1, best_d = 0x7FFFFFFF;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    uint8_t *rec = g_ent_table[unit];
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *t = g_ent_table[i];
        if (mode == 0 ? t[6] == 0 : t[6] != 0)
            continue;
        int d = abs((int)rec[0] - (int)t[0]) + abs((int)rec[1] - (int)t[1]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    uint8_t *t = g_ent_table[best];
    if (t[0] == rec[0] && t[1] == rec[1])
        return 0;
    g_text_busy = 0;
    camera_focus_ent(unit);
    int r = ai_move_to(t[0], t[1], unit, mode);
    int moved = r != 0;
    g_text_busy = 1;
    return moved;
}

/* 0x15DF3 sub_15DF3(idx, &x, &y)：扫描刷新点地块（属性 **byte0（info[4]）**
 * &0x60==0x20 且 **槽号 info[2..3]==idx**，0x15E4B/0x15E55 反汇编定案——
 * 行为 5 的 spawner 参数即槽号，同 g_spawner_flags/ctx 宝箱表索引；旧实现
 * 误测 info[5] 且比 tile 形状号）→ 坐标。找到返回 0，否则 1（退回普通链）。 */
static int ai_spawn_point_find(int idx, int *ox, int *oy)
{
    uint8_t info[8];

    if (!g_field_map)
        return 1;
    for (int y = 0; y < g_field_h; y++) {
        for (int x = 0; x < g_field_w; x++) {
            tile_lookup(x, y, info);
            if ((info[4] & 0x60) != 0x20)
                continue;
            if ((info[2] | ((int)info[3] << 8)) == idx) {
                *ox = x;
                *oy = y;
                return 0;
            }
        }
    }
    return 1;
}

static int ai_find_char_id(int char_id)
{
    if (!g_ent_table)
        return -1;
    for (int i = 0; i < g_ent_count; i++)
        if (!(g_ent_table[i][5] & 1u) && g_ent_table[i][8] == (uint8_t)char_id)
            return i;
    return -1;
}

/* ==== AI 三方案执行（0x15055/0x15311/0x1548E） ======================= */

/* 0x15055 plan_a：道具/武器攻击执行（配 ai_pick_move 评分）。g_ai_move_
 * item_slot 取武器；[17] 阵营（mode 取反）；[16]<=0xF 形状收集目标、
 * >0xF ray 贯穿收集（aim=g_ai_move_x/y、origin=自身、len=[16]-16）；
 * delay200 → 贯穿分支：朝向 + anim(1) + wait + unit_anim_play +
 * cast_beam_rise + delay200 + 64 级调光渐暗 → 终点 = 光标 + (aim-
 * 光标)×(range-16) clamp → 光标滚动 + 8 帧光标动画 + 聚焦首目标；
 * 非贯穿分支：光标滚动。统一 magic_effect_dispatch 结算 →
 * ents_face_reset → exp=0。（cast_beam_rise 原版实参 (unit,80,-4)：
 * 施法者屏位圆心 + 半径 80 每帧 -4 收缩光晕，2026-09-08 半径语义
 * 定案后取实体精确中心。） */
static void ai_attack_plan_a(int unit, int mode)
{
    uint8_t *rec;
    uint8_t targets[32];
    const uint8_t *w;
    int ally, range, n;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    rec = g_ent_table[unit];
    g_menu_choice = inventory_get_item(unit, g_ai_move_item_slot);
    w = weapon_table_entry(g_menu_choice);
    if (!w)
        return;
    ally = mode ? w[17] : (w[17] == 0);
    camera_focus_ent(unit);
    range = w[16];
    if (range <= 0xF)
        n = shape_targets_collect(targets, g_ai_move_x, g_ai_move_y,
                                  w[18], 0, ally);
    else
        n = ray_targets_collect(targets, g_ai_move_x, g_ai_move_y,
                                rec[0], rec[1], range - 16, 0);
    field_reset_candidates();
    delay_ms(200);
    g_text_busy = w[18] + 2;
    if (range >= 0x10) {
        if (n > 0)
            battle_face_target(unit, targets[0]);
        anim_tick_update(1);
        wait_bios_ticks(1);
        wait_bios_ticks(2);
        unit_anim_play(unit, 0);
        cast_beam_rise(24 * ((int)rec[0] - (int)g_scroll_x) + 12,
                       24 * ((int)rec[1] - (int)g_scroll_y) + 18, 80, -4);
        delay_ms(200);
        for (int i = 64; i >= 0; --i) {
            palette_add_range(0, 255, i);
            delay_ms(4);
        }
        palette_apply_range(0, 255, 0);
        g_text_busy = 6;
        range -= 16;
        int ex = (g_ai_move_x - (int)g_cursor_x) * range + (int)g_cursor_x;
        if (ex >= g_field_w)
            ex = g_field_w - 1;
        else if (ex < 0)
            ex = 0;
        g_ai_move_x = ex;
        int ey = (g_ai_move_y - (int)g_cursor_y) * range + (int)g_cursor_y;
        if (ey >= g_field_h)
            ey = g_field_h - 1;
        else if (ey < 0)
            ey = 0;
        g_ai_move_y = ey;
        g_cursor_anim_frame = 0;
        cursor_scroll_to(g_ai_move_x, g_ai_move_y);
        g_text_busy = 0;
        for (int j = 1; j < 9; ++j) {
            g_cursor_anim_frame = j;
            anim_tick_update(1);
            wait_bios_ticks(1);
        }
        field_reset_candidates();
        wait_bios_ticks(2);
        if (n > 0)
            camera_focus_ent(targets[0]);
    } else {
        cursor_scroll_to(g_ai_move_x, g_ai_move_y);
    }
    magic_effect_dispatch(unit, unit, g_ai_move_item_slot, n, targets);
    ents_face_reset();
    g_exp_gained = 0;
}

/* 0x15311 plan_b：法术执行（配 ai_score_targets 评分）。score<6 → 直接
 * 返回；ep[6] 阵营（mode 取反）+ ep[4] 形状收目标；delay200 →
 * text_busy=ep[4]+2 + 光标滚动 → spell>=10 或 53AF9 → exec 表
 * （fx_magic_sfx_load/spell_execute/sfx_stop 包夹），否则 fx_scene_play →
 * collect_drops + death_anim + show_results(target) + exp=0。 */
static void ai_attack_plan_b(int unit, int mode)
{
    uint8_t targets[32];
    const uint8_t *ep;
    int ally, n;

    ep = effect_param_entry(g_ai_target_item_slot);
    if (!ep)
        return;
    if (g_ai_score_target < 6)
        return;
    ally = mode ? ep[6] : (ep[6] == 0);
    camera_focus_ent(unit);
    n = shape_targets_collect(targets, g_ai_target_x, g_ai_target_y,
                              ep[4], 0, ally);
    field_reset_candidates();
    delay_ms(200);
    g_text_busy = ep[4] + 2;
    cursor_scroll_to(g_ai_target_x, g_ai_target_y);
    g_text_busy = 0;
    anim_tick_update(0);
    if (g_ai_target_item_slot >= 10 || g_save_flag_53af9) {
        fx_magic_sfx_load();
        spell_execute(unit, g_ai_target_item_slot, n, targets);
        fx_magic_sfx_stop();
    } else {
        fx_scene_play(unit, g_ai_target_item_slot, n, targets);
    }
    uint8_t drops[15][3];
    int nd = battle_collect_drops(drops);
    anim_tick_update(0);
    battle_death_anim();
    battle_show_results(g_ai_target_unit, nd, (const uint8_t *)drops);
    anim_tick_update(0);
    g_exp_gained = 0;
    g_text_busy = 0;
}

/* 0x1548E plan_c：普攻执行（配 ai_attack_scan 评分）。聚焦 + ai_move_to
 * （攻击站位 g_ai_attack_x/y）→ 聚焦目标 + unit 朝目标（0x154E6）→
 * 53AF9==0 → duel_scene_load 直通（0x154FE jz 定案——旧注"≠0 决斗"
 * 极性反，已正）；53AF9≠0 → 贴邻序列：melee_adjacent_test==1 时
 * **目标**朝向 unit（0x15536，反击架势）→ attack_sprites_load
 * (unit,target)（0x1EB13/0x1EB39 定案：槽A=目标位/槽B=unit位，返回
 * &0x53A30）+ 双 HP 条（目标→槽A、unit→槽B）→ ents_face_reset →
 * attack_execute(unit,target,槽A)（返回是否有效）→ 仍贴邻且有效 →
 * 双向朝向（0x155D5 unit朝目标 + 0x155E4 目标朝unit）+
 * melee_panel_render(coords,unit,目标)（0x155F3 压栈序定案；内部
 * 守方条槽A）+ attack_execute(目标,unit,槽B) 反击 → ents_face_reset
 * + collect_drops + death_anim + show_results(目标) +
 * exp_levelup_check(目标)。（原版 show_results/exp_levelup 首实参 =
 * g_ai_target_unit——反击经验归属玩家目标，照录。） */
static void ai_attack_plan_c(int unit, int mode)
{
    g_text_busy = 0;
    camera_focus_ent(unit);
    ai_move_to(g_ai_attack_x, g_ai_attack_y, unit, mode);
    g_text_busy = 1;
    if (g_ai_target_unit < 0 || g_ai_target_unit >= g_ent_count)
        return;
    camera_focus_ent(g_ai_target_unit);
    battle_face_target(unit, g_ai_target_unit);

    if (!g_save_flag_53af9) {
        g_text_busy = 0;
        anim_tick_update(1);
        wait_bios_ticks(1);
        wait_bios_ticks(2);
        g_text_busy = 1;
        duel_scene_load(unit, g_ai_target_unit);
    } else {
        g_text_busy = 0;
        anim_tick_update(1);
        g_text_busy = 1;
        /* 0x15536-0x1553D：push ebx 后 push 目标 → 目标朝向 unit
         * （反击架势，非 unit 朝目标——旧注极性反，已正）。 */
        if (melee_adjacent_test(unit, g_ai_target_unit) == 1)
            battle_face_target(g_ai_target_unit, unit);
        attack_sprites_load(unit, g_ai_target_unit);
        /* 0x15556/0x15588：槽 A=目标立绘位（0x1EB05 arg_4=def 定案）——
         * 目标血条在槽 A、unit 血条在槽 B（贴邻才有第二格）。 */
        hp_bar_draw(vram_base(), 320, g_ai_target_unit, s_attack_slot_a);
        if (melee_adjacent_test(unit, g_ai_target_unit) == 1)
            hp_bar_draw(vram_base(), 320, unit, s_attack_slot_b);
        ents_face_reset();
        int valid = attack_execute(unit, g_ai_target_unit, s_attack_slot_a);
        int adjacent = melee_adjacent_test(unit, g_ai_target_unit) == 1;
        if (valid && adjacent) {
            battle_face_target(unit, g_ai_target_unit);
            battle_face_target(g_ai_target_unit, unit);
            int coords[4] = { s_attack_slot_a[0], s_attack_slot_a[1],
                              s_attack_slot_b[0], s_attack_slot_b[1] };
            melee_panel_render(coords, unit, g_ai_target_unit);
            attack_execute(g_ai_target_unit, unit, s_attack_slot_b);
        }
    }
    ents_face_reset();
    uint8_t drops[15][3];
    int nd = battle_collect_drops(drops);
    anim_tick_update(0);
    battle_death_anim();
    battle_show_results(g_ai_target_unit, nd, (const uint8_t *)drops);
    anim_tick_update(0);
    exp_levelup_check(g_ai_target_unit);
}

/* 0x14EF0 ai_decide_attack(unit, mode)：三方案评分（ai_attack_scan +
 * ai_score_targets + ai_pick_move）→ 全 <6 待机（返回 0）；tie-break：
 * attack 最大 → plan_c；attack==target>move：slot>=11 且非特殊 → plan_b、
 * slot<11 且威力 >= 自身ATK(+0x48)-目标DEF(+0x4A) → plan_b、否则 plan_c；
 * attack==move>target：特殊 → plan_c，否则 plan_a；target 最大 → plan_b；
 * move 最大 → plan_a。返回值：全 <6 → 0；**执行任一 plan（含 plan_a
 * 道具）→ 恒 1**——0x1503E 尾统一 mov eax,1（0x1504B）覆盖 plan 自身
 * 返回（plan_a 函数 0x15307 恒 0，但不透传），0x22BBE 纯 pop/ret 不改
 * eax；消费点 0x13AFF test→jnz 公共尾，即道具用毕回合终止，不再
 * 让步/接近/再生（2026-09-13 勘误"返回值=plan 返回值"旧注，§13.83）。 */
static int ai_decide_attack(int unit, int mode)
{
    uint8_t *rec;
    int power_gap, acted = 0;

    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return 0;
    rec = g_ent_table[unit];
    ai_attack_scan(unit, mode);
    ai_score_targets(unit, mode);
    ai_pick_move(unit, mode);
    /* 原版无守卫（power_gap 读 attack 扫描的 g_ai_target_unit 旧值）；
     * 宿主仅钳制越界防崩，语义保持。 */
    if (g_ai_target_unit < 0)
        g_ai_target_unit = 0;
    if (g_ai_target_unit >= g_ent_count)
        g_ai_target_unit = g_ent_count - 1;
    int special = rec[52] & 0x40;
    power_gap = *(uint16_t *)(rec + 72)
              - *(uint16_t *)(g_ent_table[g_ai_target_unit] + 74);

    if (g_ai_score_attack < 6 && g_ai_score_target < 6
        && g_ai_score_move < 6)
        return 0;
    if (g_ai_score_attack > g_ai_score_target
        && g_ai_score_attack > g_ai_score_move) {
        ai_attack_plan_c(unit, mode);           /* attack 最大 → 返 1 */
        acted = 1;
    } else if (g_ai_score_attack == g_ai_score_target
               && g_ai_score_attack > g_ai_score_move) {
        const uint8_t *ep = effect_param_entry(g_ai_target_item_slot);
        if (g_ai_target_item_slot >= 11) {
            if (!special) {
                ai_attack_plan_b(unit, mode);   /* 高阶法术直取 → 返 1 */
                acted = 1;
            } else {
                ai_attack_plan_c(unit, mode);
                acted = 1;
            }
        } else if (ep
                   && (int16_t)(ep[0] | ((uint16_t)ep[1] << 8)) >= power_gap) {
            ai_attack_plan_b(unit, mode);       /* 威力 >= ATK-DEF 差 */
            acted = 1;
        } else {
            ai_attack_plan_c(unit, mode);
            acted = 1;
        }
    } else if (g_ai_score_attack == g_ai_score_move
               && g_ai_score_attack > g_ai_score_target) {
        if (special) {
            ai_attack_plan_c(unit, mode);
            acted = 1;
        } else {
            ai_attack_plan_a(unit, mode);   /* 0x1500B→0x1503E：恒返 1 */
            acted = 1;
        }
    } else if (g_ai_score_target > g_ai_score_attack
               && g_ai_score_target >= g_ai_score_move) {
        ai_attack_plan_b(unit, mode);
        acted = 1;
    } else if (g_ai_score_move > g_ai_score_attack
               && g_ai_score_move > g_ai_score_target) {
        ai_attack_plan_a(unit, mode);       /* 0x15039 直落 0x1503E：恒返 1 */
        acted = 1;
    }
    g_text_busy = 0;
    return acted;
}

/* 0x13A9F ai_unit_act(unit, mode)：行为号 = rec[34h]&0xF 分发（反汇编
 * 2026-09-08 全解；旧"最近敌步进"近似撤销；偏移均 hex 记法——行为区
 * +34h/+35h/+36h = 十进制 52/53/54，掉落区 +31h..33h = 49..51；
 * 2026-09-12 修正曾把行为区错当十进制 34..36=状态计时器区，
 * ctx 生成单位行为号恒读 0，stage1 村民（行为 4 逃向 (19,6) 等固定点）
 * 误走攻击链）：
 *  0：decide → sub_14121 让步 → sub_13E9C 接近 → attack_now 再生
 *  1：decide → sub_14121 → attack_now
 *  2：decide → attack_scan 重评（>=6 结束）→ attack_now
 *  3：decide → find_speaker(rec[35h]) 命中 → 聚焦+move_to(目标位)+
 *     attack_now；无 → 0 链
 *  4：聚焦 + move_to(rec[35h], rec[36h]) → attack_now
 *  5：decide → 聚焦 → spawner_flags[rec[61]] 已置或刷新点缺失 → 0 链；
 *     否则 move_to(刷新点)+attack_now，到达 → ctx[83+3idx] spawn
 *     （type<2 改写 rec[31h]/[32h]，type==0 加道具）+ flags=1 +
 *     sfx_b(blk31,12) + map_spawn_count_refresh + rec[34h]=7
 *  6/default：仅公共尾
 *  7：聚焦 + move_to(rec[35h],rec[36h]) → attack_now；到达 → ent_mark_flag1
 *  8：直返 epilogue（0x13D9A jz 0x1317D），**跳过公共尾**
 *  9：find_speaker(rec[35h]) 命中 → 聚焦+move_to+attack_now；无 → 0 链
 * 10：decide → 聚焦 + move_to(rec[35h],rec[36h])
 * 11：score_targets>=6 → plan_b（**无 break，0x13E21 直落 attack_scan**
 *     ——法术+普攻可同回合连发，原版机器码行为照录）；attack>=6 →
 *     plan_c；否则 sub_14121 → attack_now
 * 公共尾：tile_event_check(所在格) → sub_13512（已行动标记）→
 * ents_face_reset → anim_tick_update(0)。 */
void ai_unit_act(int unit, int mode)
{
    if (!g_ent_table || unit < 0 || unit >= g_ent_count)
        return;
    uint8_t *rec = g_ent_table[unit];
    if (rec[5] & 5u)
        return;
    int action = rec[52] & 0x0F;
    int x_arg = rec[53], y_arg = rec[54];
    int spawner = rec[61];

    switch (action) {
    case 0:
        if (ai_decide_attack(unit, mode)) break;
        if (ai_retreat_corner(unit, mode)) break;
        if (ai_approach_nearest(unit, mode)) break;
        ai_attack_now(unit);
        break;
    case 1:
        if (ai_decide_attack(unit, mode)) break;
        if (ai_retreat_corner(unit, mode)) break;
        ai_attack_now(unit);
        break;
    case 2:
        if (ai_decide_attack(unit, mode)) break;
        ai_attack_scan(unit, mode);            /* 分数副作用（返回恒 0） */
        ai_attack_now(unit);                   /* 0x13C06：eax==0 → 必经 */
        break;
    case 3: {
        if (ai_decide_attack(unit, mode)) break;
        int target = ai_find_char_id(x_arg);
        if (target < 0) {
            if (ai_retreat_corner(unit, mode)) break;
            if (ai_approach_nearest(unit, mode)) break;
            ai_attack_now(unit);
            break;
        }
        camera_focus_ent(unit);
        if (!ai_move_to(g_ent_table[target][0], g_ent_table[target][1],
                        unit, mode))
            ai_attack_now(unit);
        g_text_busy = 0;
        break;
    }
    case 4:
        g_text_busy = 0;
        camera_focus_ent(unit);
        if (!ai_move_to(x_arg, y_arg, unit, mode))
            ai_attack_now(unit);
        break;
    case 5:
        if (ai_decide_attack(unit, mode)) break;
        g_text_busy = 0;
        camera_focus_ent(unit);
        {
            int sx = 0, sy = 0;
            if (!g_spawner_flags[spawner]
                && !ai_spawn_point_find(spawner, &sx, &sy)) {
                if (!ai_move_to(sx, sy, unit, mode))
                    ai_attack_now(unit);
                if (rec[0] == sx && rec[1] == sy && g_battle_ctx) {
                    const uint8_t *spawn = g_battle_ctx + 83 + 3 * spawner;
                    int type = spawn[0];
                    int param = spawn[1] | (spawn[2] << 8);
                    if (type < 2) {
                        rec[49] = (uint8_t)type;        /* +31h 掉落/生成参数区 */
                        *(uint16_t *)(rec + 50) = (uint16_t)param;
                        if (type == 0)
                            ent_inventory_add(unit, param);
                    }
                    g_spawner_flags[spawner] = 1;
                    sfx_play_b(g_pkg_fdother_31, 12, 1);
                    map_spawn_count_refresh();
                    rec[52] = 7;                         /* 0x13D20 行为字节=7 */
                }
            } else {
                /* 已生成或刷新点缺失 → 退回行为 0 链 */
                if (ai_retreat_corner(unit, mode)) { g_text_busy = 1; break; }
                g_text_busy = 1;
                if (ai_approach_nearest(unit, mode)) break;
                ai_attack_now(unit);
                break;
            }
        }
        break;
    case 7:
        g_text_busy = 0;
        camera_focus_ent(unit);
        if (!ai_move_to(x_arg, y_arg, unit, mode))
            ai_attack_now(unit);
        if (rec[0] == x_arg && rec[1] == y_arg)
            ent_mark_flag1(unit);
        break;
    case 8:
        /* 0x13D97 jz loc_1317D：行为 8 直返 epilogue，跳过共享收尾
         * （tile_event_check/sub_13512/face_reset/anim_tick）。 */
        return;
    case 9: {
        int target = ai_find_char_id(x_arg);
        if (target < 0) {
            if (ai_decide_attack(unit, mode)) break;
            if (ai_retreat_corner(unit, mode)) break;
            if (ai_approach_nearest(unit, mode)) break;
            ai_attack_now(unit);
            break;
        }
        camera_focus_ent(unit);
        if (!ai_move_to(g_ent_table[target][0], g_ent_table[target][1],
                        unit, mode))
            ai_attack_now(unit);
        g_text_busy = 0;
        break;
    }
    case 10:
        if (ai_decide_attack(unit, mode)) break;
        g_text_busy = 0;
        camera_focus_ent(unit);
        ai_move_to(x_arg, y_arg, unit, mode);
        break;
    case 11:
        ai_score_targets(unit, mode);
        if (g_ai_score_target >= 6)
            ai_attack_plan_b(unit, mode);      /* 无 break：0x13E21 直落 */
        ai_attack_scan(unit, mode);
        if (g_ai_score_attack >= 6) {
            ai_attack_plan_c(unit, mode);
            break;
        }
        if (ai_retreat_corner(unit, mode)) break;
        ai_attack_now(unit);
        break;
    default:
        break;
    }
    tile_event_check(rec[0], rec[1], 1);
    sub_13512(unit);
    ents_face_reset();
    anim_tick_update(0);
}


/* 0x1E529 stat_growth_roll（2026-09-08 全量含显示）：roll = lo +
 * fd2_rand()%(hi-lo)；g_disp_num_b=roll；**非零**才显示——行==3 先
 * dialog_scroll_text 上滚并回行 2，kbd_flush + 文本 text_id
 * @vram+38175+6080*行（打字机）+ stat += roll + 行++；返回行号。 */
static int growth_roll(uint8_t *rec, int stat_off, int profile_off,
                       int text_id, int line)
{
    static const uint8_t fallback[11] = { 6, 8, 4, 6, 2, 3, 8, 12, 0, 0, 0xFF };
    const uint8_t *profile = growth_table_entry(rec[7]);
    if (!profile)
        profile = fallback;
    int lo = profile[profile_off], hi = profile[profile_off + 1];
    if (hi < lo) hi = lo;
    int gain = lo;
    if (hi > lo)
        gain += (int)(fd2_rand() % (unsigned)(hi - lo));
    g_disp_num_b = gain;
    if (gain) {
        if (line == 3) {
            line = 2;
            dialog_scroll_text();
        }
        kbd_flush();
        text_render_box(1, 19, 74, 205, 320,
                        vram_base() + 38175 + 6080 * line,
                        text_id, g_pkg_fdtxt0);
        *(uint16_t *)(rec + stat_off) = (uint16_t)(
            *(uint16_t *)(rec + stat_off) + gain);
        line++;
    }
    return line;
}

/* 0x1E292 exp_levelup_check 全量（2026-09-08 收获对话落地）：
 * 门槛：g_exp_gained≠0、未移除、未达入口上限（职业30/31→99 级，
 * 其余→40 级；不过门槛时原版不清 exp，照录）。
 * 开窗：g_disp_num_b=g_exp_gained → kbd_flush →
 * dialog_backdrop_load(rec[7] 单位肖像) → 文本 488（获得经验）@38175
 * → dato_frame_stamp(0)。
 * 每满 100exp 一轮：kbd_flush → 等级++ → 文本 489（升级）@44255 →
 * 5×growth_roll（+55/+57/+62/+66/+70，文本 490..494 逐行叠放）→
 * 习得魔法（growth[10]≠255 → 6 对 (等级,魔法)：等级匹配 →
 * spell_learn_add + g_disp_num_a=魔法+441 + 文本 587@6080*行）→
 * ent_recompute_derived → exp-=100；等级到 30（职业30/31 到 99，
 * 0x1E4F6 cmp 1Eh 定案）→ exp 清零出环。
 * 收尾：wait_key_ticks(11)（0x1E5C0）→ dialog_backdrop_restore →
 * rec[60]=余数（字节截断）→ g_exp_gained=0。 */
void exp_levelup_check(int unit)
{
    if (unit < 0 || unit >= g_ent_count || g_exp_gained == 0)
        return;
    uint8_t *rec = g_ent_table[unit];
    int level = rec[33];
    int entry_cap = (rec[7] == 30 || rec[7] == 31) ? 99 : 40;
    if ((rec[5] & 1u) || level >= entry_cap)
        return;

    const uint8_t *gr = growth_table_entry(rec[7]);
    int learn_set = gr ? gr[10] : 255;
    int exp = rec[60] + g_exp_gained;
    int line = 2;

    g_disp_num_b = g_exp_gained;
    kbd_flush();
    dialog_backdrop_load(rec[7]);
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38175,
                    488, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    while (exp >= 100) {
        kbd_flush();
        rec[33] = (uint8_t)++level;
        text_render_box(1, 19, 74, 205, 320, vram_base() + 44255,
                        489, g_pkg_fdtxt0);
        line = growth_roll(rec, 55, 0, 490, line);
        line = growth_roll(rec, 57, 2, 491, line);
        line = growth_roll(rec, 62, 4, 492, line);
        line = growth_roll(rec, 66, 6, 493, line);
        line = growth_roll(rec, 70, 8, 494, line);
        if (learn_set != 255) {
            const uint8_t *pairs = spell_learn_entry(learn_set);
            for (int i = 0; pairs && i < 6; i++) {
                if (level == pairs[2 * i]) {
                    int spell = pairs[2 * i + 1];
                    g_disp_num_a = spell + 441;
                    spell_learn_add(unit, spell);
                    text_render_box(1, 19, 74, 205, 320,
                                    vram_base() + 38175 + 6080 * line,
                                    587, g_pkg_fdtxt0);
                }
            }
        }
        ent_recompute_derived(unit);
        exp -= 100;
        if (((rec[7] == 30 || rec[7] == 31) && level == 99) || level == 30)
            exp = 0;
    }
    wait_key_ticks(11);
    dialog_backdrop_restore();
    rec[60] = (uint8_t)exp;
    g_exp_gained = 0;
}
void battle_award_exp_list(const uint8_t *units, int count, int field_off)
{
    /* IDA 0x22AF6: a list entry receives exp only when its selected
     * participation field is set.  Both callers use +37 or +38. */
    if (!units || field_off < 0 || field_off >= FD2_ENT_REC_SIZE)
        return;
    for (int i = 0; i < count; i++) {
        int unit = units[i];
        if (unit < 0 || unit >= g_ent_count)
            continue;
        uint8_t *rec = g_ent_table[unit];
        int level = rec[33];
        if (rec[32] > 8 && rec[32] < 25)
            level += 30;
        if (rec[field_off]) {
            rec[field_off] = 0;
            g_exp_gained += 4 * level;
        }
    }
    anim_tick_update(0);
}

/* ---- 结算展示（0x1AA1D） ------------------------------------------- */
void battle_show_results(int unit, int count, const uint8_t records[])
{
    uint8_t *ent = g_ent_table[unit];

    if (count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        kbd_flush();
        int type = records[3 * i];
        int val = records[3 * i + 1] | (records[3 * i + 2] << 8);
        switch (type) {
        case 0:                                            /* 物品拾取 */
            if (ent[6] != 2)
                return;
            g_disp_num_a = val + 181;
            dialog_backdrop_load(ent[7]);
            /* 0x1ACB2/0x1AA98：结算框文本基 vram+40739（0xA9F23） */
            text_render_box(1, 19, 74, 205, 320, vram_base() + 40739,
                            432, g_pkg_fdtxt0);
            if (ent_inventory_add(unit, val) == -1) {
                /* 0x1AA56 包满全流程：433 → 是/否确认 → 换物或 434 */
                dato_frame_stamp(0);
                wait_key_anim(0);
                dialog_backdrop_restore();
                delay_ms(100);
                dialog_backdrop_load(ent[7]);
                text_render_box(1, 19, 74, 205, 320, vram_base() + 40739,
                                433, g_pkg_fdtxt0);
                dato_frame_stamp(0);
                int yes = confirm_yes_no();
                confirm_close_anim();
                if (yes == 1 && g_menu_choice == 0) {
                    dialog_backdrop_restore();
                    if (inventory_pick_slot(unit, 0)) {
                        /* 0x1AAF9 get 返回值原版未消费，照录 */
                        inventory_get_item(unit, g_menu_choice);
                        inventory_remove_item(unit, g_menu_choice);
                        ent_inventory_add(unit, val);
                        continue;               /* 0x1AB1A：直接下一条 */
                    }
                    delay_ms(100);
                    dialog_backdrop_load(ent[7]);
                    text_render_box(1, 19, 74, 205, 320, vram_base() + 40739,
                                    434, g_pkg_fdtxt0);
                } else {
                    /* 0x1AB89：否/放弃 → 434 画在下一行（+6080=0xAB6E3） */
                    text_render_box(1, 19, 74, 205, 320, vram_base() + 46819,
                                    434, g_pkg_fdtxt0);
                }
                delay_ms(200);
            } else {
                dato_frame_stamp(0);
                wait_key_anim(0);
            }
            dialog_backdrop_restore();
            break;
        case 1:                                            /* 金钱 */
            if (ent[6] != 2)
                return;
            dialog_backdrop_load(ent[7]);
            g_disp_num_b = val;
            text_render_box(1, 19, 74, 205, 320, vram_base() + 40739,
                            435, g_pkg_fdtxt0);
            dato_frame_stamp(0);
            wait_key_anim(0);
            dialog_backdrop_restore();
            g_gold += g_disp_num_b;
            break;
        case 2:                                            /* 延时 200ms 后事件脚本 */
            delay_ms(200);
            g_event_script_table[val](unit);
            break;
        case 3:                                            /* 事件对白包文本 */
            text_render_box(3, 19, 74, 205, 320, vram_base(),
                            val, g_pkg_text_evt);
            break;
        default:
            break;
        }
    }
}

int battle_collect_drops(uint8_t out[][3])
{
    int n = 0;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (!(rec[5] & 1) && rec[49] != 255 && *(uint16_t *)(rec + 0x40) == 0)
            memcpy(out[n++], rec + 49, 3);
    }
    return n;
}

void battle_death_anim(void)
{
    /* 0x1DB65 全量（2026-09-08 演出落地，撤销"仅状态效果"占位）：
     * 收集视口内 HP==0 未移除单位的特效落点——带内偏移 = 30168 +
     * 24*(x-1-sx) + 456*24*(y-1-sy)（= 实体格左上一格，48x48 档位；
     * 原版 0x1DC1A/0x1DC23 算式：32904-2736=30168）。
     * 相位 1 自旋 13 帧（0x1DC67..0x1DCA0）：每帧 field 视口重渲 +
     * 全实体渲染——死亡者 rec[3]=j%4 方向轮转＝"转圈"（原版
     * ent_render_one 另携 j/4 相位参，fd2re 以全局实体相位近似，
     * 挂号）→ 上屏 → 1 tick。
     * 标记（0x1DD25）：HP==0 → rec[5]=1，无视口过滤。
     * 相位 2 消失 12 帧（0x1DD5C..0x1DEA4）：净场（视口 + 存活实体，
     * 死亡者已移除不渲）渲入临时 0x25680 带；sfx(blk31,3)；帧
     * 68..73 直接叠在自旋末帧上、帧 74..79 每帧先整带回拷净场再盖
     * g_death_fx_pkg（FDOTHER blk5）对应帧；每帧上屏 + 1 tick。
     * 收尾 anim_tick_update(0)。 */
    enum { DEATH_SHADOW_BYTES = 456 * 336 };  /* entities.c s_field_shadow 同尺寸（0x25680） */
    uint8_t *band = battle_shadow_layer();
    ptrdiff_t pos[30];
    int nvis = 0;

    for (int i = 0; i < g_ent_count && nvis < 30; i++) {
        uint8_t *rec = g_ent_table[i];
        int x = rec[0], y = rec[1];
        if (!(rec[5] & 1u) && *(uint16_t *)(rec + 64) == 0
            && x >= (int)g_scroll_x - 1 && x <= (int)g_view_w + (int)g_scroll_x
            && y >= (int)g_scroll_y - 1
            && y <= (int)g_view_h + (int)g_scroll_y + 1)
            pos[nvis++] = (ptrdiff_t)30168
                + 24 * (x - 1 - (int)g_scroll_x)
                + 456 * 24 * (y - 1 - (int)g_scroll_y);
    }

    if (nvis > 0) {
        for (int j = 0; j < 13; j++) {
            field_render_viewport_ext(band + 32904, 456, 13, 8,
                                      (int)g_scroll_x, (int)g_scroll_y);
            for (int k = 0; k < g_ent_count; k++) {
                uint8_t *rec = g_ent_table[k];
                if (!(rec[5] & 1u) && *(uint16_t *)(rec + 64) == 0)
                    rec[3] = (uint8_t)(j % 4);
            }
            field_render_entities(band);
            battle_shadow_blit();
            wait_bios_ticks(1);
        }
    }

    for (int m = 0; m < g_ent_count; m++) {
        if (*(uint16_t *)(g_ent_table[m] + 64) == 0)
            g_ent_table[m][5] = 1;
    }

    if (nvis > 0) {
        uint8_t *clean = (uint8_t *)malloc(DEATH_SHADOW_BYTES);

        if (clean) {
            field_render_viewport_ext(clean + 32904, 456, 13, 8,
                                      (int)g_scroll_x, (int)g_scroll_y);
            field_render_entities(clean);
            sfx_play(g_pkg_fdother_31, 3, 1);
            /* 消失特效帧 68..79 是字节 RLE（literal/run，0x4EC66 族，
             * 帧长精确耗尽验证）——须走 rle_blit_transparent，此前误用
             * 4-op 的 package_blit_frame 解码＝花屏。 */
            for (int n = 0; n < 6; n++) {
                for (int i = 0; i < nvis; i++)
                    rle_blit_transparent(
                        package_frame_ptr(g_death_fx_pkg, 68 + n),
                        band + pos[i], 456);
                battle_shadow_blit();
                wait_bios_ticks(1);
            }
            for (int jj = 6; jj < 12; jj++) {
                memcpy(band, clean, DEATH_SHADOW_BYTES);
                for (int i = 0; i < nvis; i++)
                    rle_blit_transparent(
                        package_frame_ptr(g_death_fx_pkg, 68 + jj),
                        band + pos[i], 456);
                battle_shadow_blit();
                wait_bios_ticks(1);
            }
            free(clean);
        }
        anim_tick_update(0);
    }
}
/* 0x2EBE1 duel_exchange / 0x2E2B0 duel_scene_load 的实现已随决斗演出
 * 原语族移驻 duel.c（2026-09-08 全量落地，battle.h 声明保留）。 */

void battle_map_overview(void)
{
    /* IDA 0x2000A: overview is modal, animates the cursor marker, and exits
     * only after a keyboard event.  The host renderer has no separate 4x
     * overview surface, so keep the modal/animation contract on the current
     * frame. */
    kbd_flush();
    while (!kbd_key_avail()) {
        anim_tick_update(0);
        wait_bios_ticks(1);
    }
    kbd_flush();
}
/* ---- 战斗系统菜单 0x16F55 全量定案（2026-09-07 反汇编） --------------
 * 径向 2x2 图标菜单（围绕光标）：图标 = FDOTHER blk2 字面帧 24x20，
 * 目录在块首 u32[]；索引 = 3*def[item] + 2*selected（def@0x51E9F =
 * {7,5,6,4} → 常态帧 21/15/18/12，选中 +2 高亮变体）。键：Up=0
 * Left=1 Right=2 Down=3（menu_run 0x177FC），Enter/Space=确认(1)，
 * Esc=取消(-1)。开(0x1741C)=重画战场+sfx(blk31,8)+图标 4 步生长，
 * 关(0x176B4)=反向 4 步收拢。逐项像素偏移（内环 ±2280/±6 族）为
 * 2026-09-08 §13.56 径向偏移族逐字节定案（本实现已对齐）。
 * choice 0=save_menu_run（0x19DF7 已全解，返回值 1/0/-1 直达本函数
 * 返回——2026-09-08 定案）；1=自动执行（确认 417/取消 412→418→逐我方
 * 单位 AI 行动→battle_turn_end）；2=options_menu_run→**返回 0（外层
 * do-while 重开菜单）**；3=结束回合（确认 419/取消 412→420→transition
 * 三明治 + battle_turn_end，**无 pending——非"回标题"，fd2re 旧实现
 * 误设 pending=1 已撤**）。Esc→1。anim_pump 0x118C1 消费：0=重开本
 * 菜单（jz 回调）/ 1=xor ebx,eax 归零继续战斗 / -1=退出游戏。
 * save 子菜单 2026-09-08 全解重写（见 save_menu_run）；options 子菜单
 * 0x1728C 布局级最小实现（文本/开关行为已对齐，逐帧布局挂号）。 */

static const uint8_t *blk2_frame(int idx)
{
    const uint8_t *pkg = (const uint8_t *)g_pkg_fdother_2;
    uint32_t off;

    if (!pkg || idx < 0 || idx >= 78)
        return NULL;
    off = (uint32_t)pkg[4 * idx] | ((uint32_t)pkg[4 * idx + 1] << 8)
        | ((uint32_t)pkg[4 * idx + 2] << 16) | ((uint32_t)pkg[4 * idx + 3] << 24);
    return pkg + off;
}

/* 0x51E9F 系统菜单图标 def；0x51EF5 存/读/退子菜单 def（get_bytes 定案）。
 * sysmenu_stamp_icon/draw/run 分叉实现 2026-09-08 并轨撤销——原版
 * 0x16F97/0x16FA9/0x16FC1 系统菜单与存读退子菜单直调 radial 族
 * （0x1741C/0x177FC/0x176B4），见下方 radial_* 严格版。 */
static const int s_radial_def_sys[4] = { 7, 5, 6, 4 };
static const int s_radial_def_save[4] = { 12, 13, 14, 15 };

/* 0x19953 confirm_yes_no（text.c 全解重建版接入，2026-09-06）：提示
 * 文本先画（外层系统菜单/存读退子菜单确认均@vram+40739=0xA9F23，
 * 2026-09-08 字节级复核；此前记录的"外层@+19939"有误——0xA4DC3 在
 * 原版二进制中不存在），
 * 再盖闭口肖像帧；是/否双按钮 + 闪烁 + 店主口型由本体呈现。
 * 注意：fd2re 战斗系统菜单未开场景窗——confirm 的口型/backup 走
 * scratch 代偿路径，肖像位沿用当前 53C67（原版此处有 blk 窗）。 */
static int sysmenu_confirm(int prompt_id, unsigned vram_off)
{
    int yn;

    text_render_box(1, 19, 74, 205, 320, vram_base() + vram_off,
                    prompt_id, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    yn = confirm_yes_no();
    confirm_close_anim();
    return yn;
}

static void sysmenu_result_text(int str_id)
{
    text_render_box(1, 19, 74, 205, 320, vram_base() + 46819,
                    str_id, g_pkg_fdtxt0);
    delay_ms(200);
    dialog_backdrop_restore();                          /* dialog_backdrop_restore */
}

/* 0x19DF7 save_menu_run 全解（2026-09-08 机器码级）：存/读/退径向子菜单，
 * 经 battle_system_menu 共享尾链返回——正常=1（0x16FD8 mov eax,1）、
 * 子菜单 Esc=0（0x19EF3 xor eax,eax；anim_pump 0x118CC 收 0 重开外层
 * 菜单）、退出=-1（0x1A301；anim_pump 收 -1 → main 退出游戏）。
 * 探测（0x19E25..0x19EAC，位移定案 [esp+8]/[esp+4]）：fopen FD2.SAV rb
 * 成功→整读 22987B 后即弃（无校验）；失败→state[2]=1 禁"读档"。
 * 任一实体 (ent[5]&1)==0 且 bit7 置位→state[1]=1 禁"存档"。
 * def@0x51EF5=图标{12,13,14,15}，state 基表@0x53F22 全零。
 * 0=继续：battlefield_intro_anim（0x1B1E7 12 步鸟瞰，2026-09-08 已严格重建）→返 1。
 * 1=存档：确认 410→是→save_game_write→文本 411；否/Esc→412。
 * 2=读档：确认 413→是→414@46819+delay200+restore+music_play(-1,0)+
 * save_game_load()（无参）+kbd_flush→返 1；否→412。
 * 3=退出：确认 415→是→416@46819+music_play(-1,1)+delay200+restore→返-1；
 * 否→外层 0x1716F 共享尾（412+delay+restore+返 1）。
 * 确认框统一 dialog(75)+文本@vram+40739+dato_frame_stamp(0)+confirm+
 * sub_197E5 收拢动画（sysmenu_confirm 已接 text.c 全解版，2026-09-08
 * 撤销"restore 代偿"占位）。 */
/* ===================== 0x1B1E7 战场重介绍（战况板） =====================
 * save 菜单"继续"分支：12 步四段拼合滑入 → 空闲动画等键（战场每
 * tick 重渲染 + 面板常驻）→ 12 步收拢 → 恢复 + free。
 * 面板合成 0x1B41D：blk5 帧 133..136 四块 @(109,19)/(75,37)/(75,155)/
 * (129,172) + 章节数(g_state+1,2位)@(143,24) 回合数(3位)@(188,24)
 * 金币(8位)@(140,176) + 三方存活数 0x1B5F1（faction 0/2/1 →
 * (120,159)/(182,159)/(228,159)，2 位）+ 章节名文本 0x255+2*state@
 * (80,61)（state==0x10 且 roster 无 18 号 → id-2）与 id+1@(80,116)。
 * 四段揭示（源恒取面板就位态，目标屏坐标逐步就位；影带作 64000B
 * 暂存、320 网格，段后整块 memcpy VRAM）：
 *  seg1 0x1AF99/0x1AF1E：170x117 段 @(75,37)——滑入 i≤4 xoff=
 *  75+50*(4-i)（宽 min(170,320-xoff)），i>4 全展；滑出 i<5 右移
 *  收出（esi=75-50*(4-i)，负值时源右移/宽减），i≥5 全展；
 *  seg2 0x1B019：102x11 @(109,y0)，i∈[3,7] y0=19-6*(4-(i-3))（负值
 *  顶出截行），i<3 不画，i>7 y0=19；seg3 0x1B0AD：170x16 @(75,155)，
 *  i∈[5,9] y=155+9*(9-i)（y+16>200 零行）；seg4 0x1B14B：63x15
 *  @(129,172)，i∈[8,12] y=172+4*(12-i)。 */
static int battlefield_alive_count(int faction)
{
    int n = 0;

    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];

        if (rec[6] != (uint8_t)faction || rec[7] == 0x79 || rec[31] == 10)
            continue;
        if (!ent_flag1_test(i))
            n++;
    }
    return n;
}

static int bf_roster_has_unit(int id)
{
    for (int i = 0; i < g_roster_count; i++)
        if (g_roster_table[i][8] == (uint8_t)id)
            return 1;
    return 0;
}

/* 0x1B41D */
static void battlefield_panel_compose(uint8_t *dst, int pitch)
{
    package_blit_frame(g_death_fx_pkg, dst + 19 * pitch + 109, pitch, 133);
    package_blit_frame(g_death_fx_pkg, dst + 37 * pitch + 75, pitch, 134);
    package_blit_frame(g_death_fx_pkg, dst + 155 * pitch + 75, pitch, 135);
    package_blit_frame(g_death_fx_pkg, dst + 172 * pitch + 129, pitch, 136);
    number_stamp_digits(dst + 24 * pitch + 143, pitch, g_state + 1, 42, 2);
    number_stamp_digits(dst + 24 * pitch + 188, pitch, g_turn_count, 42, 3);
    number_stamp_digits(dst + 176 * pitch + 140, pitch, g_gold, 31, 8);
    number_stamp_digits(dst + 159 * pitch + 120, pitch,
                        battlefield_alive_count(0), 42, 2);
    number_stamp_digits(dst + 159 * pitch + 182, pitch,
                        battlefield_alive_count(2), 42, 2);
    number_stamp_digits(dst + 159 * pitch + 228, pitch,
                        battlefield_alive_count(1), 42, 2);
    {
        int str_id = 0x255 + 2 * g_state;

        if (g_state == 0x10 && !bf_roster_has_unit(0x12))
            str_id -= 2;
        text_render_box(0, 19, 0, 205, pitch, dst + 61 * pitch + 80,
                        str_id, g_pkg_fdtxt0);
        text_render_box(0, 19, 0, 205, pitch, dst + 116 * pitch + 80,
                        str_id + 1, g_pkg_fdtxt0);
    }
}

/* 段行拷贝族：dst = 影带暂存（320 网格屏坐标），src = 面板就位态。 */
static void bf_band_rows(uint8_t *dst, int dst_x, int src_x, int width,
                         int rows, int y, int src_y, const uint8_t *panel)
{
    for (int i = 0; i < rows; i++)
        memcpy(dst + 320 * (y + i) + dst_x,
               panel + 320 * (src_y + i) + src_x, (size_t)width);
}

/* 0x1AF99 */
static void bf_seg1_slide_in(int step, const uint8_t *panel)
{
    int xoff = 75, width = 170;

    if (step <= 4) {
        xoff = 75 + 50 * (4 - step);
        if (xoff + 170 > 320)
            width = 320 - xoff;
    }
    bf_band_rows(battle_shadow_layer(), xoff, 75, width, 117, 37, 37, panel);
}

/* 0x1AF1E */
static void bf_seg1_slide_out(int step, const uint8_t *panel)
{
    int dst_x = 75, src_x = 75, width = 170;

    if (step < 5) {
        int left = 75 - 50 * (4 - step);

        if (left < 0) {
            width += left;
            src_x = 75 - left;
            dst_x = -left;
        } else {
            dst_x = left;
        }
    }
    bf_band_rows(battle_shadow_layer(), dst_x, src_x, width, 117, 37, 37,
                 panel);
}

/* 0x1B019 */
static void bf_seg2_reveal(int step, const uint8_t *panel)
{
    int y0, rows = 11, src_y = 19;

    if (step < 3)
        return;
    if (step > 7)
        step = 7;
    y0 = 19 - 6 * (4 - (step - 3));
    if (y0 < 0) {
        src_y = 19 - y0;
        rows += y0;
        y0 = 0;
    }
    bf_band_rows(battle_shadow_layer(), 109, 109, 102, rows, y0, src_y,
                 panel);
}

/* 0x1B0AD */
static void bf_seg3_reveal(int step, const uint8_t *panel)
{
    int y, rows = 16;

    if (step < 5 || step > 9)
        step = 9;
    y = 155 + 9 * (9 - step);
    if (y + 16 > 200)
        rows = 0;
    bf_band_rows(battle_shadow_layer(), 75, 75, 170, rows, y, 155, panel);
}

/* 0x1B14B */
static void bf_seg4_reveal(int step, const uint8_t *panel)
{
    int y, rows = 15;

    if (step < 8)
        return;
    if (step > 12)
        step = 12;
    y = 172 + 4 * (12 - step);
    if (y + 15 > 200)
        rows = 0;
    bf_band_rows(battle_shadow_layer(), 129, 129, 63, rows, y, 172, panel);
}

/* 0x1B1E7 battlefield_intro_anim */
void battlefield_intro_anim(void)
{
    uint8_t *work = malloc(FD2_VRAM_SIZE);
    uint8_t *panel = malloc(FD2_VRAM_SIZE);
    uint8_t *band = battle_shadow_layer();
    uint32_t last;

    if (!work || !panel) {
        free(work);
        free(panel);
        return;
    }
    memset(work, 0, FD2_VRAM_SIZE);
    /* 0x1B227：初始战场帧（地形+实体入带）→ 视口 312x192 → work。 */
    field_backdrop_render(band, (int)g_scroll_x, (int)g_scroll_y);
    field_render_viewport_ext(band + 32904, 456, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
    field_render_entities(band);
    blit_rows(work + 1284, 320, band + 32904, 456, 312, 192);
    battlefield_panel_compose(panel, 320);          /* 0x1B288 */
    for (int i = 0; i < 12; i++) {                  /* 0x1B290：滑入 */
        memcpy(band, work, FD2_VRAM_SIZE);
        bf_seg1_slide_in(i, panel);
        bf_seg2_reveal(i, panel);
        bf_seg3_reveal(i, panel);
        bf_seg4_reveal(i, panel);
        memcpy(vram_base(), band, FD2_VRAM_SIZE);
    }
    last = bios_tick();
    for (;;) {                                      /* 0x1B2EE：等键 */
        int key = kbd_key_avail();

        if (bios_tick() != last) {
            field_phase_tick();            /* 0x1B302 anim_phase_tick */
            idle_pump();
            field_backdrop_render(band, (int)g_scroll_x, (int)g_scroll_y);
            field_render_viewport_ext(band + 32904, 456, 13, 8,
                                      (int)g_scroll_x, (int)g_scroll_y);
            field_render_entities(band);
            /* 0x1B349：面板合成入带（456 网格），视口连框呈现。 */
            battlefield_panel_compose(band + 31076, 456);
            blit_rows(vram_base() + 1284, 320, band + 32904, 456, 312, 192);
            last = bios_tick();
        }
        if (key)
            break;
    }
    kbd_flush();
    for (int i = 11; i >= 0; i--) {                 /* 0x1B38E：收拢 */
        memcpy(band, work, FD2_VRAM_SIZE);
        bf_seg1_slide_out(i, panel);
        bf_seg2_reveal(i, panel);
        bf_seg3_reveal(i, panel);
        bf_seg4_reveal(i, panel);
        memcpy(vram_base(), band, FD2_VRAM_SIZE);
    }
    memcpy(vram_base(), work, FD2_VRAM_SIZE);
    free(work);
    free(panel);
    kbd_flush();
}

static int save_menu_run(void)
{
    int state[4] = { 0, 0, 0, 0 };
    int r;

    {
        FILE *fp = fopen(save_file_path(), "rb");
        if (fp)
            fclose(fp);
        else
            state[2] = 1;
    }
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t flags = g_ent_table[i][5];
        if (!(flags & 1) && (flags & 0x80)) {
            state[1] = 1;
            break;
        }
    }

    /* 0x19E62..0x19EB8：径向子菜单原语族（0x1741C/0x177FC/0x176B4
     * 与系统菜单/行动菜单共用——sysmenu_* 分叉 2026-09-08 并轨）。 */
    radial_menu_open(s_radial_def_save, state);
    do {
        r = radial_menu_run(s_radial_def_save, state);
    } while (r == 0);
    radial_menu_close(s_radial_def_save, state);
    anim_tick_update(0);

    if (r == -1)
        return 0;
    if (g_menu_choice == 0) {
        /* 0x19F03 → sub_1B1E7：战场重介绍（战况板四段拼合 + 空闲
         * 动画等键 + 收拢）——2026-09-08 严格重建，模态等待占位撤销。 */
        kbd_flush();
        battlefield_intro_anim();
        return 1;
    }
    if (g_menu_choice == 1) {
        dialog_backdrop_load(75);
        r = sysmenu_confirm(410, 40739);
        if (r == 1 && g_menu_choice == 0) {
            save_game_write();
            sysmenu_result_text(411);
        } else {
            sysmenu_result_text(412);
        }
        kbd_flush();
        return 1;
    }
    if (g_menu_choice == 2) {
        dialog_backdrop_load(75);
        r = sysmenu_confirm(413, 40739);
        if (r == 1 && g_menu_choice == 0) {
            sysmenu_result_text(414);           /* 0x1A22B 文本 414@+46819 */
            music_play(-1, 0);                  /* 0x1A249 */
            save_game_load();                   /* 0x1A251 无参调用 */
        } else {
            sysmenu_result_text(412);
        }
        kbd_flush();
        return 1;
    }
    /* 3=退出（0x1A25B）：确认 415→是→416+music(-1,1)+delay+restore+返-1。 */
    dialog_backdrop_load(75);
    r = sysmenu_confirm(415, 40739);
    if (r == 1 && g_menu_choice == 0) {
        text_render_box(1, 19, 74, 205, 320, vram_base() + 46819,
                        416, g_pkg_fdtxt0);
        music_play(-1, 1);
        delay_ms(200);
        dialog_backdrop_restore();
        return -1;
    }
    sysmenu_result_text(412);                   /* 0x1716F 共享取消尾 */
    return 1;
}

/* 0x1728C options_menu_run 全量重写（2026-09-08 逐指令定案）：与系统
 * 菜单同族径向 2x2 图标菜单（radial open/menu_run/close），18..25 为
 * blk2 图标帧基（帧 = 3*def+2*flags+blink → 54..77），**非 fdtxt 文本
 * id**——旧"dialog(75)+四行文本@+38175"布局级实现系误读，已撤。
 * def 每轮按开关现值重算（0x172C4..0x17318，def@0x51EAF 同基 18..24）：
 *   [0]=0x12+(music_gate==0) [1]=0x14+(sfx_gate==0)
 *   [2]=0x16+(53AF9!=0)      [3]=0x18+(51AAB==0)
 * state@0x53F02 全零（无禁用项）。g_menu_choice 仅入循环前清 0
 * （0x172BA），切换后重开保持所选项。menu_run 非 0 返回后 close：
 *   Esc(r=-1)→共享尾 loc_16FDD 返回（外层 battle_system_menu 收 0
 *   重开系统菜单）；Enter/Space(r=1) 按 choice 分发：
 *   0=音乐翻转 + AIL_set_sequence_volume(seq, 新值?0x7F:0, 1000)
 *     （0x1737A off→0 / 0x17393 on→0x7F，ramp 1000ms）；
 *   2=53AF9^1（0x173A5）；3=51AAB^1（0x173BA）；其余(1)=音效翻转
 *   （0x173C6）——均回 0x172C4 重算 def 重开菜单。 */
static void options_menu_run(void)
{
    const int flags[4] = { 0, 0, 0, 0 };              /* @0x53F02 全零 */
    int def[4];
    int r;

    g_menu_choice = 0;
    for (;;) {
        def[0] = 0x12 + (g_music_gate == 0);
        def[1] = 0x14 + (g_sfx_gate == 0);
        def[2] = 0x16 + (g_save_flag_53af9 != 0);
        def[3] = 0x18 + (g_save_flag_51aab == 0);
        radial_menu_open(def, flags);
        do {
            r = radial_menu_run(def, flags);          /* 0x177FC */
        } while (r == 0);
        radial_menu_close(def, flags);
        if (r == -1)
            return;                                   /* 0x17355 → loc_16FDD */
        if (g_menu_choice == 0) {
            g_music_gate = (g_music_gate == 0);       /* 0x17364 翻转 */
            music_seq_set_volume(g_music_gate ? 0x7F : 0, 1000);
        } else if (g_menu_choice == 2) {
            g_save_flag_53af9 ^= 1;                   /* 0x173A5 */
        } else if (g_menu_choice == 3) {
            g_save_flag_51aab ^= 1;                   /* 0x173BA */
        } else {
            g_sfx_gate = (g_sfx_gate == 0);           /* 0x173C6（choice 1） */
        }
    }
}

int battle_system_menu(void)
{
    static const int state_zero[4] = { 0, 0, 0, 0 };  /* @0x53EF2 全零 */
    int r;

    g_menu_choice = 0;
    /* 0x16F97..0x16FCB：radial_menu_open/menu_run/radial_menu_close
     * 原版直调（sysmenu_* 分叉 2026-09-08 并轨撤销）。 */
    radial_menu_open(s_radial_def_sys, state_zero);
    do {
        r = radial_menu_run(s_radial_def_sys, state_zero);
    } while (r == 0);
    radial_menu_close(s_radial_def_sys, state_zero);
    anim_tick_update(0);                  /* 0x16FC9 */

    if (r == -1)
        return 1;                         /* Esc：继续战斗（0x16FD8 eax=1） */
    if (g_menu_choice == 0) {
        /* 0x16FED：子菜单返回值直达（正常 1 / Esc 0=重开 / 退出 -1）。 */
        return save_menu_run();
    }
    if (g_menu_choice == 1) {
        /* 自动执行：确认 → 418 → 逐我方单位（flags&0x85==0 且 type==2）
         * 卷镜 + ai_move_to(x,y,i,1)（0x17113 直调，2026-09-08 接真身；
         * 旧以 ai_unit_act(i,1) 行为等价近似已撤）→ 事件分发 →
         * face_reset → battle_turn_end。 */
        dialog_backdrop_load(g_ent_table ? g_ent_table[0][7] : 75);
        r = sysmenu_confirm(417, 40739);       /* 0x17025 push 0xA9F23=VRAM+40739 */
        if (r == 1 && g_menu_choice == 0) {
            sysmenu_result_text(418);
            g_text_busy = 0;
            g_transition_busy = 0;
            for (int i = 0; i < g_ent_count; i++) {
                uint8_t *rec = g_ent_table[i];
                if (rec[5] & 0x85)
                    continue;
                if (rec[6] != 2)
                    continue;
                cursor_scroll_to(rec[0], rec[1]);
                g_pending_event_id = 255;
                ai_move_to(rec[0], rec[1], i, 1);       /* 0x17113 */
                if (g_pending_event_id != 255)
                    event_script_dispatch(i);
                ents_face_reset();
            }
            anim_tick_update(0);
            battle_turn_end();
            g_text_busy = 1;
            g_transition_busy = 1;
            return 1;
        }
        sysmenu_result_text(412);
        return 1;
    }
    if (g_menu_choice == 2) {
        options_menu_run();
        return 0;                         /* 0x17285：外层 do-while 重开菜单 */
    }
    /* choice 3 = 结束回合（0x171D0：无 pending——回标题在 save 子菜单
     * 的"退出"项，此前 fd2re 在此误设 pending=1）。 */
    dialog_backdrop_load(75);
    r = sysmenu_confirm(419, 40739);           /* 0x171EC push 0xA9F23=VRAM+40739 */
    if (r == 1 && g_menu_choice == 0) {
        sysmenu_result_text(420);
        g_transition_busy = 0;
        battle_turn_end();
        g_transition_busy = 1;
        return 1;
    }
    sysmenu_result_text(412);
    return 1;
}

/* ==== 战斗交互层原语（0x14818/0x115B6/0x173E7/0x1741C/0x176B4/
 * 0x177FC/0x17898/0x179D5/0x1BBDC 群，2026-09-08 §13.25） ============ */

/* 0x51CF9/0x51CFD：传送目的格。 */
int32_t g_warp_dest_x, g_warp_dest_y;

/* 0x1F183 terrain_immune_test：rec[7]==28（小体型）恒否；rec[32]==19
 * （职业=飞兵族）或 rec[31]∈{4,5}（系统系）→ 免疫地形限制。 */
static int terrain_immune_test(int unit)
{
    uint8_t *rec = g_ent_table[unit];
    if (rec[7] == 28)
        return 0;
    if (rec[32] == 19)
        return 1;
    return rec[31] == 4 || rec[31] == 5;
}

/* g_class_table @0x61646（20B/条，0x4E8A5 class_table_entry）：
 * 条目按"地块代价字节"（g_shape_map[4*tile+1]）索引，值 20=可进入
 * （mode-6 传送落点判据），1/2=进入代价，其余=不可。29 条（0..28）。 */
static const uint8_t s_class_table[29][20] = {
    {0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x02,0x03,0x03,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x02,0x03,0x03,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x01,0x01,0x01,0x01,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x01,0x01,0x01,0x01,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x01,0x14,0x01,0x02,0x02,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x14,0x14,0x14,0x14,0x01,0x14,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01}
};

static const uint8_t *class_table_entry(int cls)
{
    if (cls < 0 || cls > 28)
        cls = 0;
    return s_class_table[cls];
}

/* ---- 可达域洪泛（0x4E390 seed / 0x4E42C recur / 0x4E4BE relax）-------
 * 字段图单元 {tile u16(10b), flags, marker}：marker 兼作洪泛剩余预算，
 * 255/0xFF 视作 -1=未访问（带符号比较，原版 0x4E4E1 cmp/jle 定案：
 * new > marker 才写——洪泛保留"最大剩余预算"，非最短路）。
 * 每步代价 = class 行[shape_map[4*tile+1]]（0x4E4D2：shape[+1] 是地形
 * 码，须经 class_table_entry(cls) 的 20B 职业地形代价行换算；越界码
 * 按原版连表平铺读，超表尾视为不可进入）。cell[2] bit6=固定物不进入
 * （0x4E4E8，在改进判定之后）；bit7=必停（0x4E4EE：以 rem-cost 判定
 * 改进后写入值清 0，不再扩散）。2026-09-07 修正：此前 shape[+1] 直作
 * 代价、且以 nv>=marker 剪枝——未访问格 0xFF 即 (int8_t)-1，恒被剪，
 * 洪泛只标起点一格 = 可移动域恒 0。原版四向递归，此处显式栈等价。 */
static uint8_t *field_cell(int x, int y)
{
    return (uint8_t *)g_field_map + 4 + 4 * (size_t)(x + g_field_w * y);
}

static void field_flood_seed(int cls, int x, int y, int budget)
{
    enum { FLOOD_STACK_MAX = 8192 };
    static int16_t sx[FLOOD_STACK_MAX], sy[FLOOD_STACK_MAX];
    const uint8_t *tbl = class_table_entry(cls);

    if (!g_field_map || x < 0 || y < 0 || x >= g_field_w || y >= g_field_h
        || budget < 0 || budget > 127)
        return;
    field_cell(x, y)[3] = (uint8_t)budget;
    int sp = 0;
    sx[sp] = (int16_t)x; sy[sp] = (int16_t)y; sp++;
    while (sp > 0) {
        sp--;
        int cx = sx[sp], cy = sy[sp];
        int rem = field_cell(cx, cy)[3];
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || ny < 0 || nx >= g_field_w || ny >= g_field_h)
                continue;
            uint8_t *nc = field_cell(nx, ny);
            uint8_t cost = 0xFFu;
            if (g_shape_map) {
                unsigned tile = (unsigned)nc[0]
                              | ((unsigned)(nc[1] & 3u) << 8);
                if ((size_t)tile * 4u + 2u <= (size_t)g_shape_map_size) {
                    unsigned code =
                        ((const uint8_t *)g_shape_map)[4u * tile + 1];
                    size_t off = (size_t)(tbl - &s_class_table[0][0]) + code;
                    cost = off < sizeof(s_class_table)
                         ? ((const uint8_t *)s_class_table)[off] : 0xFFu;
                }
            }
            if (rem < cost)
                continue;                      /* 0x4E4D8 rem<代价剪枝 */
            int nv = rem - cost;
            if (nv <= (int8_t)nc[3])
                continue;       /* 0x4E4E1 带符号：严格更大才写（-1=未访问） */
            if (nc[2] & 0x40u)
                continue;                      /* 0x4E4E8 固定物 */
            if (nc[2] & 0x80u)
                nv = 0;                        /* 0x4E4EE 必停：写 0 */
            nc[3] = (uint8_t)nv;
            if (sp < FLOOD_STACK_MAX) {
                sx[sp] = (int16_t)nx; sy[sp] = (int16_t)ny; sp++;
            }
        }
    }
}

/* ---- 路径 DFS（0x4E4F6 build / 0x4E5CC dfs / 0x4E680 relax /
 * 0x4E751 emit / 0x4E703 foe_cell / 0x4E71F 段数统计，2026-09-08 全量
 * 忠实复刻）-----------------------------------------------------------
 * 撤销旧"洪泛 marker 严格增大回溯"近似——它与原版有三处不等价，是
 * "相同局面下移动路径形状/走向与原版不同"的直接根因：
 *  ① 选路判据：原版 emit(0x4E76D) 比较的是 DFS 深度（步数）——预算内
 *     步数最少者胜；回溯法给的是代价最小路径，两者在地形代价不均时
 *     选不同路线；
 *  ② 平局裁决：原版 `cmp ah,byte_60078 / ja`＝深度<=best 即拷贝，等长
 *     路径后找到者覆盖先找到者；结合 DFS 枚举序右(3)/左(1)/下(0)/上(2)
 *     （0x4E5CC 四段递归书写序）＝取该字典序下最后一条等长最短路径；
 *     回溯法的平局是"从终点看下/左/上/右第一个严格更大邻居"，两套
 *     规则选出不同路径；
 *  ③ 自含性：原版 field_path_build 不依赖外部洪泛场（起点写预算后
 *     直接探索，DFS 松弛自写 marker），调用点（ai_move_to 首建
 *     0x14C3A / 终建 0x14EA4、battle_unit_turn 0x189F8、ai_advance_foe
 *     0x141B0）前均只有 enemies_zoc_mark（场已由前序 reset 清干净）。
 * 工作区对应（0x60060-0x60079）：out=0x60073、dest=0x60071、
 * flag=0x6017A、深度=0x60077、best=0x60078、帧栈=0x60079（8B/帧，
 * +3=方向码）。cell[1] 高 6 位为 DFS 暂存的方向段数×4（低 2 位是 tile
 * 索引位 8-9；field_reset_candidates 的 cell[1]&=0x03 清除暂存）。
 * flag 语义（0x4E680 relax 分支序照录）：
 *  0=常规：等预算剪枝；0x40 禁入剪枝（不写 marker）；0x80 必停写 0；
 *     松弛成功即 emit（终点判定+拷贝）。
 *  1=28 预算参考路径：等预算且当前路径段数×4（0x4E71F，无符号）>
 *     存储值时允许重访（0x4E6B4/0x4E6C1 jbe 反向）。
 *  2=敌格航点搜索：写 marker 后提前返回（0x4E6DE）——跳过 0x40/0x80
 *     检查＝可穿越敌格、不受 ZoC 停；0x40 格经 foe_cell(0x4E703) 把
 *     坐标写入 *wp 且 best=1，每次松弛都覆盖且 DFS 不提前终止＝
 *     航点为 DFS 全程最后一个被松弛的对立单位格。 */
typedef struct {
    uint8_t *out;              /* 方向序列输出（flag 0/1）＝0x60073 */
    int out_cap;
    int dest_x, dest_y;        /* 0x60071 */
    int flag;                  /* 0x6017A */
    int depth;                 /* 0x60077：当前帧栈深＝路径步数 */
    int best_len;              /* 0x60078：255=未达 */
    int wp_x, wp_y;            /* flag==2 航点（0x60073 复用） */
    const uint8_t *tbl;        /* 0x6006A：职业地形代价行 */
    uint8_t dirs[256];         /* 0x60079 帧栈方向码（8B/帧 取 +3） */
} path_dfs_t;

/* 0x4E71F sub_4E71F：当前路径方向段数×4（首段恒计；uint8 截断照录）。 */
static uint8_t path_stack_segments4(const path_dfs_t *ctx)
{
    int count = 0, last = -1;

    for (int i = 0; i < ctx->depth; i++) {
        if (ctx->dirs[i] != last) {
            count++;
            last = ctx->dirs[i];
        }
    }
    return (uint8_t)(4 * count);
}

/* 0x4E751 field_path_emit：终点且深度<=best（无符号，0x4E76D ja 反向）
 * → 更新 best 并按帧序拷贝方向码（等长后到覆盖；深度 0 / 无缓冲 /
 * 超容量不拷贝——超容量为原版 8B 帧直写的护栏，正常预算<=127 不可达）。 */
static void path_emit(path_dfs_t *ctx, int x, int y)
{
    if (x != ctx->dest_x || y != ctx->dest_y)
        return;
    if ((unsigned)ctx->depth > (unsigned)ctx->best_len)
        return;
    ctx->best_len = ctx->depth;                /* 0x4E76F */
    if (ctx->depth == 0 || !ctx->out || ctx->depth > ctx->out_cap)
        return;                                /* 0x4E777 or ah,ah */
    for (int i = 0; i < ctx->depth; i++)
        ctx->out[i] = ctx->dirs[i];            /* 0x4E787 stosb 循环 */
}

/* 每步地形代价（0x4E69A 与洪泛 0x4E4D8 同式）：tile 10 位 →
 * shape_map[4*tile+1] 地形码 → class 行代价；越界按不可进入。 */
static int field_step_cost(const uint8_t *tbl, const uint8_t *cell)
{
    unsigned tile, code;
    size_t off;

    if (!g_shape_map)
        return 0xFF;
    tile = (unsigned)cell[0] | ((unsigned)(cell[1] & 3u) << 8);
    if ((size_t)tile * 4u + 2u > (size_t)g_shape_map_size)
        return 0xFF;
    code = ((const uint8_t *)g_shape_map)[4u * tile + 1];
    off = (size_t)(tbl - &s_class_table[0][0]) + code;
    return off < sizeof(s_class_table)
         ? (int)((const uint8_t *)s_class_table)[off] : 0xFF;
}

/* 0x4E680 field_path_step_relax：邻格松弛。返回 1=递归（clc），
 * 0=剪枝（stc）；*rem_out=递归用剩余预算。 */
static int path_step_relax(path_dfs_t *ctx, int rem, int nx, int ny,
                           int *rem_out)
{
    uint8_t *nc = field_cell(nx, ny);
    int cost = field_step_cost(ctx->tbl, nc);

    if ((unsigned)rem < (unsigned)cost)
        return 0;                              /* 0x4E69D jb：预算不足 */
    int nv = rem - cost;
    if (nv < (int8_t)nc[3])
        return 0;                              /* 0x4E6A7 jl：带符号劣化 */
    if (nv == (int8_t)nc[3]) {
        if (ctx->flag != 1)
            return 0;                          /* 0x4E6B2：等预算仅 flag=1 */
        uint8_t seg4 = path_stack_segments4(ctx);
        if ((unsigned)seg4 <= (unsigned)(nc[1] & 0xFCu))
            return 0;                          /* 0x4E6C1 jbe：段数未增 */
        nc[1] = (uint8_t)(seg4 | (nc[1] & 0x3u));  /* 0x4E6C3→4E6CA */
    } else {
        uint8_t seg4 = path_stack_segments4(ctx);
        nc[1] = (uint8_t)(seg4 | (nc[1] & 0x3u));  /* 0x4E6C5→4E6CA */
    }
    if (ctx->flag == 2) {                      /* 0x4E6D5 */
        nc[3] = (uint8_t)nv;                   /* 0x4E6DE：跳过 0x40/0x80 */
        if (nc[2] & 0x40u) {                   /* 0x4E703 foe_cell 覆盖式 */
            ctx->wp_x = nx;
            ctx->wp_y = ny;
            ctx->best_len = 1;
        }
        *rem_out = nv;
        return 1;
    }
    if (nc[2] & 0x40u)
        return 0;                              /* 0x4E6EC 固定物剪枝 */
    int r2 = nv;
    if (nc[2] & 0x80u)
        r2 = 0;                                /* 0x4E6F6 必停：剩余清 0 */
    nc[3] = (uint8_t)r2;
    path_emit(ctx, nx, ny);                    /* 0x4E6FA */
    *rem_out = r2;
    return 1;
}

/* 0x4E5CC field_path_dfs：四向递归，枚举序右(3)/左(1)/下(0)/上(2)
 * （0x4E5E1/0x4E60E/0x4E637/0x4E659 逐向边界判定同序）。 */
static void path_dfs(path_dfs_t *ctx, int x, int y, int rem)
{
    static const int bdx[4] = { 1, -1, 0, 0 };
    static const int bdy[4] = { 0, 0, 1, -1 };
    static const uint8_t bdir[4] = { 3, 1, 0, 2 };
    int d, b;

    if (ctx->depth >= (int)sizeof(ctx->dirs))
        return;                 /* 帧栈护栏（原版 0x60079 直写无界） */
    d = ctx->depth++;
    for (b = 0; b < 4; b++) {
        int nx = x + bdx[b], ny = y + bdy[b];
        if (nx < 0 || ny < 0 || nx >= g_field_w || ny >= g_field_h)
            continue;
        int nrem = 0;
        ctx->dirs[d] = bdir[b];                /* 帧+3 方向码（emit/段数共用） */
        if (path_step_relax(ctx, rem, nx, ny, &nrem))
            path_dfs(ctx, nx, ny, nrem);
    }
    ctx->depth--;
}

/* 0x4E4F6 field_path_build：起点格写预算（0x4E5A8）→ 起点即终点 emit
 * （0x4E5B8）→ DFS（0x4E5BD）。返回 best_len：正=步数，0=原地，
 * 255=不可达（flag=2：1=找到敌格航点，255=预算内无）。不自建场——
 * 调用方须保证 marker 状态与原版调用点一致（本文件全部调用点入参前
 * 均已 reset/enemies_zoc_mark）。 */
static int field_path_build(int cls, int x0, int y0, int budget,
                            uint8_t *path, int cap,
                            int dest_x, int dest_y, int flag,
                            int *wp_x, int *wp_y)
{
    path_dfs_t ctx;

    if (!g_field_map || x0 < 0 || y0 < 0 || x0 >= g_field_w || y0 >= g_field_h
        || budget < 0 || budget > 127)
        return 255;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = path;
    ctx.out_cap = cap;
    ctx.dest_x = dest_x;
    ctx.dest_y = dest_y;
    ctx.flag = flag;
    ctx.best_len = 255;                        /* 0x4E5B1 byte_60078=-1 */
    ctx.tbl = class_table_entry(cls);
    ctx.wp_x = x0;
    ctx.wp_y = y0;     /* 航点兜底=起点：原版 out 指向调用方栈（未初始化
                        * 残留）；此处取确定值，使"找到=假象"的病态局
                        * （起点即(0,0) 且无 0x40 格）落入航点=当前位守卫 */
    field_cell(x0, y0)[3] = (uint8_t)budget;   /* 0x4E5A8 */
    ctx.depth = 0;                             /* 0x4E5AA byte_60077=0 */
    path_emit(&ctx, x0, y0);                   /* 0x4E5B8 */
    path_dfs(&ctx, x0, y0, budget);
    if (wp_x)
        *wp_x = ctx.wp_x;
    if (wp_y)
        *wp_y = ctx.wp_y;
    return ctx.best_len;
}

/* 0x14818：作用形状选格 + 目标收集（详见 battle.h）。 */
int shape_targets_collect(uint8_t *out, int x, int y, int shape,
                          int radius, int ally_mode)
{
    int n = 0;

    if (!g_field_map)
        return 0;
    if (shape >= 16) {
        int arm = shape - 16;
        for (int i = 0; i < g_field_w; i++)
            if (abs(i - x) <= arm)
                field_cell(i, y)[3] = 0;         /* 十字：行 */
        for (int j = 0; j < g_field_h; j++)
            if (abs(j - y) <= arm)
                field_cell(x, j)[3] = 0;         /* 十字：列 */
    } else {
        field_flood_seed(0, x, y, shape);
        if (radius > 0) {
            for (int j = 0; j < g_field_h; j++)
                for (int i = 0; i < g_field_w; i++)
                    if (abs(i - x) + abs(j - y) < radius)
                        field_cell(i, j)[3] = 0xFF;  /* 最小射程排除圈 */
        }
    }
    for (int m = 0; m < g_ent_count; m++) {
        uint8_t *rec = g_ent_table[m];
        int ally = rec[6];
        if (rec[5] & 1u)
            continue;                            /* 阵亡 */
        if (rec[0] >= g_field_w || rec[1] >= g_field_h)
            continue;
        if (field_cell(rec[0], rec[1])[3] == 0xFF)
            continue;                            /* 非候选格 */
        int ok = ally_mode == 0 ? ally == 0
               : ally_mode == 1 ? ally != 0
               : ally_mode == 2 ? ally == 1 : ally == 2;
        if (!ok)
            continue;
        if (out)
            out[n] = (uint8_t)m;
        n++;
    }
    return n;
}

/* 0x14742：曼哈顿 <radius 内按阵营计单位数（out 可 NULL）。
 * battle_move_select 的确认半径判据。 */
static int ent_targets_by_distance(uint8_t *out, int x, int y,
                                   int radius, int ally_mode)
{
    int n = 0;

    if (!g_ent_table)
        return 0;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        int ally = rec[6];
        if (rec[5] & 1u)
            continue;
        if ((int)abs((int)rec[0] - x) + (int)abs((int)rec[1] - y) >= radius)
            continue;
        int ok = ally_mode == 0 ? ally == 0
               : ally_mode == 1 ? ally != 0
               : ally_mode == 2 ? ally == 1 : ally == 2;
        if (!ok)
            continue;
        if (out)
            out[n] = (uint8_t)i;
        n++;
    }
    return n;
}

/* 0x115B6：光标目标/落点选择（详见 battle.h）。 */
int battle_move_select(int mode, int n_targets, const uint8_t *targets)
{
    int radius = g_text_busy > 1 ? g_text_busy - 1 : g_text_busy;
    int cyc = 0, focus = -1;

    if (!g_ent_table)
        return -1;
    if (n_targets > 0 && targets) {
        focus = targets[0];
        if (mode != 6 && focus < g_ent_count
            && g_ent_table[focus][7] != 121)
            camera_focus_ent(focus);
    }
    for (;;) {
        int key = input_wait_key();              /* 0x12DAC 等价 */
        if (key == SCAN_ESC)
            return -1;
        if (key == 57 || key == SCAN_ENTER) {
            int cx = (int)g_cursor_x, cy = (int)g_cursor_y;
            if (mode == 6) {                     /* 传送落点 */
                int blocked = 0;
                for (int i = 0; i < g_ent_count && !blocked; i++) {
                    if (i == focus)
                        continue;
                    uint8_t *rec = g_ent_table[i];
                    if (rec[0] == cx && rec[1] == cy && !ent_flag1_test(i))
                        blocked = 1;
                }
                if (!blocked && focus >= 0 && focus < g_ent_count) {
                    uint8_t *rec = g_ent_table[focus];
                    int cls = rec[32];
                    if (rec[7] == 28)
                        cls = 1;
                    if (terrain_immune_test(focus))
                        cls = 19;
                    uint8_t info[8];
                    tile_lookup(cx, cy, info);
                    int idx = info[5] < 20 ? info[5] : 19;
                    if (class_table_entry(cls)[idx] == 20)
                        return focus;
                }
            } else if (mode != 5) {
                if (cx >= g_field_w || cy >= g_field_h)
                    continue;
                if (field_cell(cx, cy)[3] != 0xFF) {
                    if (mode == 4)
                        return 0;                /* 候选格确认 */
                    if (ent_targets_by_distance(NULL, cx, cy, radius, mode) > 0)
                        return ent_at_cursor();
                }
            } else {
                return 0;                        /* 任意格确认 */
            }
        } else if ((key == 44 || key == 76) && n_targets > 0 && targets) {
            cyc = (cyc + 1) % n_targets;        /* 0x1177C: ++v7==v6 环绕 */
            focus = targets[cyc];
            if (focus < g_ent_count && g_ent_table[focus][7] != 121)
                camera_focus_ent(focus);
        } else if (key == SCAN_UP) {
            cursor_up();
            sfx_play(g_pkg_fdother_31, 0, 1);
        } else if (key == SCAN_DOWN) {
            cursor_down();
            sfx_play(g_pkg_fdother_31, 0, 1);
        } else if (key == SCAN_LEFT) {
            cursor_left();
            sfx_play(g_pkg_fdother_31, 0, 1);
        } else if (key == SCAN_RIGHT) {
            cursor_right();
            sfx_play(g_pkg_fdother_31, 0, 1);
        }
    }
}

/* ---- 径向 2x2 菜单原语（行动/道具菜单；系统菜单仍走 sysmenu_* 老路径，
 * 本组为其参数化泛化版） ---------------------------------------------- */

/* 0x173E7：g_menu_choice = 首个未禁用项。 */
static int menu_first_enabled(const int flags[4])
{
    for (int i = 0; i < 4; i++) {
        g_menu_choice = i;
        if (!flags[i])
            break;
    }
    return g_menu_choice;
}

static uint8_t s_radial_snap[456u * 336u];

/* ---- 径向偏移族 2026-09-08 逐字节定案（0x17587/0x179D5/0x177CB）：
 * 偏移为 456 带内字节值（1 垂直 px = 456B；±2280 = 上/下 5px，
 * ±6 = 左/右 6px）。常驻位（icons_redraw 0x17A0B 常量）：
 * 项0 上 (0,-20)=−9120、项1 左 (−24,+2)=888、项2 右 (+24,+2)=936、
 * 项3 下 (0,+24)=10944。open（0x1746C）四项自 (0,+2)=912 起每步
 * {−2280,−6,+6,+2280}（生长序列上 (0,−3/−8/−13/−18)、左/右 ±6/步
 * ——止于 (0,−18)/(0,+22)，常驻位差 2px 由 wait 首帧接管，原版即此）；
 * close（0x17715）自常驻位反向 4 步回 (0,0)/(0,+2)/(0,+2)/(0,+4)。
 * 帧索引 = 3*def + 2*flags（生长/收拢期无选中位）；选中项 += blink
 * （icons_redraw 0x17A7D，blink 0/1 每 ≥3 tick 翻转 0x178D7）。 */
static int s_menu_blink;                 /* 0x53A8D */

static void radial_stamp_at(uint8_t *band, const int def[4],
                            const int flags[4], int item, int byte_off,
                            int blink_sel)
{
    const uint8_t *fr = blk2_frame(3 * def[item] + 2 * flags[item]
                                   + blink_sel);
    size_t at;

    if (!fr)
        return;
    at = 32904u + 24u * (unsigned)g_cursor_view_x
         + 10944u * (unsigned)g_cursor_view_y + (size_t)byte_off;
    stamp_raw_transparent(fr, band + at, 456);   /* sub_4ED34 */
}

/* 0x179D5 radial_menu_icons_redraw：常驻位 4 图标（选中 +blink）+
 * 光标单位立绘（ent_render_one = field_render_entity_offset）。 */
static void radial_menu_icons_redraw(const int def[4], const int flags[4])
{
    static const int rest[4] = { -9120, 888, 936, 10944 };
    uint8_t *band = battle_shadow_layer();

    for (int i = 0; i < 4; i++)
        radial_stamp_at(band, def, flags, i, rest[i],
                        i == g_menu_choice ? s_menu_blink : 0);
    {
        int u = ent_at_cursor();

        if (u != -1)
            field_render_entity_offset(band, u, 0);
    }
}

/* 0x1741C：sfx(8) → 相位钟 → 战场重渲染（backdrop+viewport+entities+
 * tile_info）→ 快照（原版 sub_175A9 3x3 格版，fd2re 以整带等价）→
 * 4 步生长（每步：恢复快照[原版 sub_17643 3x3] → 4 图标[无 blink] →
 * 光标单位 → blit；无延时）。初选 0x173E7 不在本体——调用方
 * （battle_act_menu 0x18E53/0x18ED6）显式调用，choice 由进入前值
 * 或方向键驱动。 */
static void radial_menu_open(const int def[4], const int flags[4])
{
    static const int step[4] = { -2280, -6, 6, 2280 };
    uint8_t *band = battle_shadow_layer();
    int off[4] = { 912, 912, 912, 912 };

    sfx_play(g_pkg_fdother_31, 8, 1);
    field_phase_tick();
    field_backdrop_render(band, (int)g_scroll_x, (int)g_scroll_y);
    field_render_viewport_ext(band + 32904, 456, 13, 8,
                              (int)g_scroll_x, (int)g_scroll_y);
    field_render_entities(band);
    tile_info_panel_draw(band + 32904, 456);
    memcpy(s_radial_snap, band, sizeof(s_radial_snap));
    for (int s = 0; s < 4; s++) {
        int u = ent_at_cursor();

        for (int i = 0; i < 4; i++)
            off[i] += step[i];
        memcpy(band, s_radial_snap, sizeof(s_radial_snap));
        for (int i = 0; i < 4; i++)
            radial_stamp_at(band, def, flags, i, off[i], 0);
        if (u != -1)
            field_render_entity_offset(band, u, 0);
        battle_shadow_blit();
    }
    kbd_flush();
}

/* 0x17898 radial_menu_wait：有键归一化返回（input_wait_key 尾等价
 * 0x17982）；无键：idle_pump → tick 差 >3 或 <0 翻转 blink →
 * 相位钟 + 战场全帧重渲染（含 tile_info）+ icons_redraw + blit。 */
static int radial_menu_wait(const int def[4], const int flags[4])
{
    static uint32_t last_tick;           /* 共享 g_last_bios_tick 族 */
    uint8_t *band = battle_shadow_layer();

    for (;;) {
        if (kbd_key_avail())
            return input_wait_key();
        idle_pump();
        {
            int delta = (int)(bios_tick() - last_tick);

            if (delta > 3 || delta < 0) {
                s_menu_blink ^= 1;       /* 0x178D7：++ 后 ==2 清 0 */
                last_tick = bios_tick();
            }
        }
        field_phase_tick();
        field_backdrop_render(band, (int)g_scroll_x, (int)g_scroll_y);
        field_render_viewport_ext(band + 32904, 456, 13, 8,
                                  (int)g_scroll_x, (int)g_scroll_y);
        field_render_entities(band);
        tile_info_panel_draw(band + 32904, 456);
        radial_menu_icons_redraw(def, flags);
        battle_shadow_blit();
    }
}

/* 0x177FC radial_menu_run：Esc=-1；Enter/Space=1；方向键 flags==0
 * 置 choice（禁用不动 choice）；一律返 0 由外层重开 wait。 */
static int radial_menu_run(const int def[4], const int flags[4])
{
    for (;;) {
        int key = radial_menu_wait(def, flags);

        if (key == SCAN_ESC)
            return -1;
        if (key == 57 || key == SCAN_ENTER)
            return 1;
        if (key == SCAN_UP && !flags[0])
            g_menu_choice = 0;
        else if (key == SCAN_DOWN && !flags[3])
            g_menu_choice = 3;
        else if (key == SCAN_LEFT && !flags[1])
            g_menu_choice = 1;
        else if (key == SCAN_RIGHT && !flags[2])
            g_menu_choice = 2;
        return 0;
    }
}

/* 0x176B4：sfx(8) → 自常驻位反向 4 步收拢（每步：调偏移 → 恢复
 * 快照 → 4 图标[无 blink] → 光标单位 → blit）→ 末次恢复 + kbd_flush。 */
static void radial_menu_close(const int def[4], const int flags[4])
{
    uint8_t *band = battle_shadow_layer();
    int off[4] = { -9120, 888, 936, 10944 };

    sfx_play(g_pkg_fdother_31, 8, 1);
    for (int s = 0; s < 4; s++) {
        int u = ent_at_cursor();

        off[0] += 2280;
        off[1] += 6;
        off[2] -= 6;
        off[3] -= 2280;
        memcpy(band, s_radial_snap, sizeof(s_radial_snap));
        for (int i = 0; i < 4; i++)
            radial_stamp_at(band, def, flags, i, off[i], 0);
        if (u != -1)
            field_render_entity_offset(band, u, 0);
        battle_shadow_blit();
    }
    memcpy(band, s_radial_snap, sizeof(s_radial_snap));
    battle_shadow_blit();
    kbd_flush();
}

/* 0x1BBDC：战斗道具四项菜单（使用/给予/状态/丢弃）。 */
int battle_item_target_flow(int unit)
{
    /* unk_51F05 原始字节 = {8,9,10,11}（get_bytes 定案）→ blk2 帧
     * 24..35 四组三态图标；旧误读 {16,17,16,17} 落帧 48..53 = 无关
     * 资源，菜单渲染成垃圾（2026-09-08 第四轮实测修正）。 */
    static const int def[4] = { 8, 9, 10, 11 };
    uint8_t targets[100];
    int flags[4] = { 0, 0, 0, 0 };                   /* unk_53F32 全 0 */
    int cx0 = (int)g_cursor_x, cy0 = (int)g_cursor_y;
    int r, n, sel, count;

    if (sub_1B8A6(unit) == 0)
        return -1;                               /* 包空（防御路径） */
    n = shape_targets_collect(NULL, g_cursor_x, g_cursor_y, 1, 1, 3);
    field_reset_candidates();
    if (n == 0)
        flags[1] = 1;                            /* 相邻无我方 → 禁给予 */
    (void)menu_first_enabled(flags);             /* 0x1BC65 初选 */
    radial_menu_open(def, flags);
    do {
        r = radial_menu_run(def, flags);
    } while (!r);
    radial_menu_close(def, flags);
    anim_tick_update(0);
    if (r == -1)
        return -1;

    if (g_menu_choice == 0) {                    /* 使用 */
        for (;;) {
            if (!inventory_pick_slot(unit, 1))
                return 0;
            int slot = g_menu_choice;
            const uint8_t *wd = weapon_table_entry(sub_1B722(unit, slot));
            int warp = (wd[13] == 23);
            g_text_busy = wd[18] + 2;            /* 确认半径 = w[18]+1 */
            n = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                      wd[16], warp, wd[21]);
            sel = battle_move_select(wd[21], n, targets);
            field_reset_candidates();
            g_text_busy = 1;
            count = shape_targets_collect(targets, g_cursor_x, g_cursor_y,
                                          wd[18], 0, wd[21]);
            field_reset_candidates();
            if (warp) {                          /* 传送类道具 w[13]==23 */
                uint8_t *rec = g_ent_table[unit];
                if (rec[8] != 24 || (int)*(uint16_t *)(rec + 70) < 20)
                    sel = -1;                    /* 0x1BDDC 资格判定 */
                if (sel != -1) {
                    uint8_t focus1 = (uint8_t)(targets[0] < g_ent_count
                                               ? targets[0] : unit);
                    sel = battle_move_select(6, 1, &focus1);
                }
                if (sel != -1) {
                    g_warp_dest_x = (int32_t)g_cursor_x;
                    g_warp_dest_y = (int32_t)g_cursor_y;
                    g_text_busy = 0;
                    camera_focus_ent(unit);
                    g_text_busy = 1;
                }
            }
            if (sel != -1) {
                magic_effect_dispatch(sel, unit, slot, count, targets);
                sub_13512(unit);
                return 1;
            }
            g_text_busy = 0;
            camera_focus_ent(unit);
            g_text_busy = 1;                     /* 取消 → 重选道具 */
        }
    }
    if (g_menu_choice == 1) {                    /* 给予 */
        if (!inventory_pick_slot(unit, 0)) {
            ent_recompute_derived(unit);
            return 0;
        }
        int slot = g_menu_choice;
        uint8_t *buf = (uint8_t *)malloc(100);
        n = buf ? shape_targets_collect(buf, g_cursor_x, g_cursor_y, 1, 1, 3)
                : 0;
        int sel2 = battle_move_select(3, n, buf);
        field_reset_candidates();
        int dst = ent_at_cursor();
        cursor_scroll_to(cx0, cy0);
        free(buf);
        if (sel2 == -1 || dst < 0) {
            ent_recompute_derived(unit);
            return 0;
        }
        int item = sub_1B722(unit, slot);
        if (ent_inventory_add(dst, item) == -1) {
            /* 对方满包：选其弃置槽交换（0x1BF53） */
            if (!inventory_pick_slot(dst, 0)) {
                ent_recompute_derived(unit);
                return 0;
            }
            int dropped = sub_1B722(dst, g_menu_choice);
            sub_1B8E7(dst, g_menu_choice);
            ent_inventory_add(dst, item);
            sub_1B8E7(unit, slot);
            ent_inventory_add(unit, dropped);
        } else {
            sub_1B8E7(unit, slot);
        }
        g_unit_acted = 1;
        ent_recompute_derived(unit);
        return 0;
    }
    if (g_menu_choice == 2) {                    /* 状态页 0x1BFFE */
        item_equip_menu(unit);
        return 0;
    }
    /* choice 3：丢弃 */
    if (inventory_pick_slot(unit, 0))
        sub_1B8E7(unit, g_menu_choice);
    ent_recompute_derived(unit);
    return 0;
}

int32_t g_disp_num_a;                    /* 0x53AD9 */
int32_t g_disp_num_c;                    /* 0x53ADD：token -5 嵌套文本 id（0x1609D 读） */
int32_t g_disp_num_b;                    /* 0x53AE1 */

/* 原版无计数门（直接 g_ent_table+unit*80）；场景菜单把 g_ent_table 切到
 * roster 而 g_ent_count 属战斗表保持 0，装备菜单链路的宿主护栏须按
 * 当前生效表取界（2026-09-12 武器店装备 no-op 根因）。 */
static int active_ent_count(void)
{
    return g_ent_table == g_roster_table ? (int)g_roster_count
                                         : (int)g_ent_count;
}

int sub_1B8A6(int unit)
{
    if (!g_ent_table || unit < 0 || unit >= active_ent_count())
        return 0;
    int used = 0;
    for (int i = 0; i < 8; i++)
        if (*(int8_t *)(g_ent_table[unit] + 10 + 2 * i) >= 0)
            used++;
    return used;
}

int sub_1B722(int unit, int index)
{
    if (!g_ent_table || unit < 0 || unit >= active_ent_count()
        || index < 0 || index >= 8)
        return -1;
    return g_ent_table[unit][11 + 2 * index];
}

void sub_1B8E7(int unit, int choice)
{
    if (!g_ent_table || unit < 0 || unit >= active_ent_count())
        return;
    uint8_t *ent = g_ent_table[unit];
    if (choice < 0 || choice > 7)
        return;
    memmove(ent + 10 + 2 * choice,
            ent + 12 + 2 * choice,
            (size_t)(2 * (7 - choice)));
    ent[24] = 0x80;
}
int sub_2AEDB(int unit, int item_id)
{
    /* IDA 0x2AEDB: despite the old shop_open label, this helper only scans
     * the unit's occupied inventory slots and returns the matching index. */
    int used = sub_1B8A6(unit);
    for (int i = 0; i < used; i++)
        if (sub_1B722(unit, i) == item_id)
            return i;
    return -1;
}
int save_game_load(void)
{
    uint8_t record[FD2_SAVE_SIZE];
    int count;

    if (!save_record_load(record)) {
        /* 0x100BD..0x10105：校验和不符先弹损坏档对话框（DATO blk75 背景
         * + 文本 436 @VRAM+40739 + dato_frame_stamp(0) + wait_key_anim(0)
         * + 0x196CB 恢复；frame_stamp 未重构，按据点分支同款省略）。
         * 原版弹完对话框后照常载入该记录（垃圾 state 后续 undefined）；
         * fd2re 保持回标题护栏（r==2 入口本身以校验和通过为前提，此路
         * 仅磁盘态在菜单与载入之间被改动的边缘）。 */
        dialog_backdrop_load(75);
        text_render_box(1, 19, 74, 205, FD2_SCREEN_W,
                        vram_base() + 40739,
                        436, g_pkg_fdtxt0);
        wait_key_anim(0);
        dialog_backdrop_restore();
        return -1;
    }
    /* 0x1010A：进入装载本体前统一压黑（菜单尾已黑，此处为幂等原样）。 */
    palette_fade_black();

    memcpy(s_battle_ctx, record + FD2_SAVE_CTX_OFF, sizeof(s_battle_ctx));
    /* 档区存的是整块 2211B 缓冲镜像（save_game_write 全量转录），读档后
     * 缓冲即已装载内容；len 必须同步恢复，否则 battle_ctx_standing_
     * payload/group 全部越界判负（2026-09-08：读 stage0 战斗档推进到
     * 增援 evt00 时 sprites_reload_slot(3) 零生成，script 7 引用实体
     * 12 撞 count=12 守卫退出——len 仅在 battle_ctx_load 设置，读档
     * 路径漏设，静态初值 0）。 */
    s_battle_ctx_len = (int)sizeof(s_battle_ctx);
    if (!g_roster_table)
        g_roster_table = malloc(FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    if (!g_roster_table)
        return -1;
    memcpy(g_roster_table, record + FD2_SAVE_ROSTER_OFF,
           FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);

    /* 0x1013E: the real title-menu resume path reinstalls the base VGA
     * palette after fading the title/menu palette to black. */
    g_palette_ptr = dat_load_block("FDOTHER.DAT", g_palette_ptr, 0);

    count = record[FD2_SAVE_COUNT_OFF];
    if (count > 96)
        return -1;
    free(g_ent_table);
    /* 原版实体表恒为整幅 malloc(7680)=96*80，读档只回填前 count 条；
     * 按存量 count 精确分配会让读档后的增援追加（battle_spawn_standing
     * 写 g_ent_table[g_ent_count] 后自增）越界写堆——与
     * battle_stage_load 的全量分配对齐。 */
    g_ent_table = malloc(FD2_ENT_MAX * FD2_ENT_REC_SIZE);
    if (!g_ent_table)
        return -1;
    g_ent_count = count;
    if (count)
        memcpy(g_ent_table, record + FD2_SAVE_ENT_OFF,
               (size_t)count * FD2_ENT_REC_SIZE);
    memcpy(g_spawner_flags, record + FD2_SAVE_SPAWN_OFF,
           sizeof(g_spawner_flags));

    g_turn_count = record[FD2_SAVE_AUX_OFF];
    g_state = record[FD2_SAVE_STATE_OFF];
    if (field_load_for_state(g_state, s_battle_ctx[0]) != 0)
        return -1;
    /* 0x101D7：读档后按恢复态重建战场背景层。 */
    battle_backdrop_load();
    g_scroll_x = record[FD2_SAVE_AUX_OFF + 3];
    g_scroll_y = record[FD2_SAVE_AUX_OFF + 4];
    g_cursor_x = record[FD2_SAVE_AUX_OFF + 5];
    g_cursor_y = record[FD2_SAVE_AUX_OFF + 6];
    g_action_saved_x = g_cursor_x;
    g_action_saved_y = g_cursor_y;
    g_cursor_view_x = record[FD2_SAVE_AUX_OFF + 7];
    g_cursor_view_y = record[FD2_SAVE_AUX_OFF + 8];
    g_roster_count = record[FD2_SAVE_AUX_OFF + 9];
    g_gold = (int32_t)((uint32_t)record[FD2_SAVE_AUX_OFF + 10]
             | ((uint32_t)record[FD2_SAVE_AUX_OFF + 11] << 8)
             | ((uint32_t)record[FD2_SAVE_AUX_OFF + 12] << 16)
             | ((uint32_t)record[FD2_SAVE_AUX_OFF + 13] << 24));
    g_save_flag_53af9 = record[FD2_SAVE_FLAG_53AF9_OFF];
    g_save_flag_51aab = record[FD2_SAVE_FLAG_51AAB_OFF];
    g_music_gate = record[FD2_SAVE_MUSIC_GATE_OFF];
    g_sfx_gate = record[FD2_SAVE_SFX_GATE_OFF];

    /* 0x101F6..0x103A1：事件文本包 + 图标缓存重建 + FD2.TMP 持久化。
     * fd2re 此前缺整段——载档战场无单位立绘的直接根因（2026-09-07
     * 同状态比对差异数 4356px 定位；原版 free 旧缓存→FDICON 逐实体
     * icon_load_entry(ent[7])→ent[2]=槽号→TMP 落盘）。原版另加载
     * FDFIELD[3s+2] 到 g_deploy_map 并在尾部 free（无消费者，照录省略）。 */
    g_pkg_text_evt = dat_load_block("FDTXT.DAT", g_pkg_text_evt, g_state + 1);
    g_pkg_text_evt_size = g_last_block_size;
    {
        FILE *fp = fopen("FDICON.B24", "rb");
        free(g_standing_sprites);
        g_standing_sprites = NULL;      /* icon_load_entry 惰性重分配 */
        icon_cache_reset();
        if (fp) {
            for (int i = 0; i < g_ent_count; i++)
                g_ent_table[i][2] =
                    (uint8_t)icon_load_entry(g_ent_table[i][7], fp);
            fclose(fp);
        }
        fp = fopen("FD2.TMP", "wb");
        if (fp) {
            if (g_standing_sprites)
                fwrite(g_standing_sprites, 1, 0x32A00, fp);
            fclose(fp);
        }
    }

    /* 0x10483..0x10616 收尾：text_busy=0 → 增援点刷新 → 合成一帧 →
     * 渐显 → 回合横幅。 */
    g_text_busy = 0;
    map_spawn_count_refresh();
    anim_tick_update(1);
    fade_in();
    for (int j = 0; j < 9; j++) {
        /* 0x104BE-0x104B0：帧 83+j @ VRAM(120,84)（pkg_frame_blit 存底，
         * 循环尾 free 恢复）；j>6 盖 3 位回合数（色 42）@ (171,91)。 */
        void *work = pkg_frame_blit(g_death_fx_pkg, vram_base(), 320,
                                    120, 84, 83 + j);
        if (j > 6)
            number_stamp_digits(vram_base() + 29291, 320,
                                g_turn_count, 42, 3);
        delay_ms(70);
        if (j == 8)
            delay_ms(500);
        pkg_frame_free(vram_base(), 320, work);
    }
    {
        /* 第二段 0x10529-0x105CF：k∈{2,3,4,9}（5→9），帧 91 @ 影带
         * (116, k²+84)（blit 存底）→ 数字入带 @ +167+456*(k²+90) →
         * 整带呈现 → wait 1 tick → free 恢复影带。 */
        uint8_t *band = battle_shadow_layer() + 32904;
        for (int k = 2; k < 6; k++) {
            void *work;
            if (k == 5)
                k = 9;
            work = pkg_frame_blit(g_death_fx_pkg, band, 456,
                                  116, k * k + 84, 91);
            number_stamp_digits(band + 167 + 456 * (k * k + 90), 456,
                                g_turn_count, 42, 3);
            blit_rows(vram_base() + 1284, 320, band, 456, 312, 192);
            wait_bios_ticks(1);
            pkg_frame_free(band, 456, work);
        }
    }
    anim_tick_update(0);
    delay_ms(200);
    g_cur_unit_idx = 0;
    g_text_busy = 1;
    kbd_flush();
    return 0;
}
void battle_sync_to_roster(void)
{
    /* IDA 0x11506: match battle entities to roster records by char-id (+8),
     * copy the 80-byte record, clear transient AI bytes, preserve enlistment,
     * and restore HP/derived fields exactly as the original offsets show. */
    if (!g_ent_table || !g_roster_table)
        return;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *ent = g_ent_table[i];
        for (int j = 0; j < g_roster_count; j++) {
            uint8_t *roster = g_roster_table[j];
            if (ent[8] != roster[8])
                continue;
            if (roster[8] == 0 && (ent[5] & 1u))
                continue;
            ent_recompute_derived(i);
            memmove(roster, ent, FD2_ENT_REC_SIZE);
            memset(roster + 34, 0, 6);
            roster[5] &= 1u;
            if (roster[5] != 1u)
                *(uint16_t *)(roster + 64) = *(uint16_t *)(roster + 66);
            *(uint16_t *)(roster + 68) = *(uint16_t *)(roster + 70);
        }
    }
}

/* ---- 原有 ---------------------------------------------------------- */

void battle_field_init(const uint8_t *list_x, const uint8_t *list_y,
                       const uint8_t *list_dir, int first, int last,
                       int leader_idx, int lx, int ly, int ldir,
                       int cursor_x, int cursor_y)
{
    /* 全解（2026-09-07 对齐 0x233C6；2026-09-13 真 ABI 定案，11 栈参）：
     * fade → battle_load_map → ent[first..last] 写
     *   rec[0]=list_x[i], rec[1]=list_y[i], rec[3]=dir；
     * dir 字段 >=4 为逐单位朝向表指针，<4 为全队常量朝向
     * （0=下 1=左 2=上 3=右）；leader != 0 → ent[leader]=(lx,ly,ldir)；
     * g_text_busy=0；卷动/光标=(cx,cy)；**view=0**；
     * 尾 anim_tick_update(1) + fade_in + delay(200)（0x23489-0x234B1）。 */
    palette_fade_black();
    battle_load_map();
    for (int i = first; i <= last; i++) {
        uint8_t *rec = g_ent_table[i];
        rec[0] = list_x ? list_x[i] : 0;
        rec[1] = list_y ? list_y[i] : 0;
        rec[3] = (uintptr_t)list_dir >= 4 ? list_dir[i]
                                          : (uint8_t)(uintptr_t)list_dir;
    }
    if (leader_idx != 0) {
        uint8_t *rec = g_ent_table[leader_idx];
        rec[0] = (uint8_t)lx;
        rec[1] = (uint8_t)ly;
        rec[3] = (uint8_t)ldir;
    }
    g_text_busy = 0;
    g_scroll_x = cursor_x;
    g_scroll_y = cursor_y;
    g_cursor_x = cursor_x;
    g_cursor_y = cursor_y;
    g_cursor_view_x = 0;                    /* 0x23489/0x23493 */
    g_cursor_view_y = 0;
    anim_tick_update(1);                    /* 0x2349F */
    fade_in();                              /* 0x234A7 */
    delay_ms(200);                          /* 0x234B1 */
}

void battle_load_map(void)
{
    /* 原版：清全部实体 flags bit7（回合已行动位）→ 新回合 */
    for (int i = 0; i < g_ent_count; i++)
        g_ent_table[i][5] &= (uint8_t)~0x80;
}

/* ---- ctx 装载与访问（2026-09-05） ------------------------------------ */
int battle_ctx_load(int state)
{
    /* FDFIELD[3k+1] → s_battle_ctx（原版 dat_load 到堆；fd2re 静态 2211B） */
    void *p = dat_load_block("FDFIELD.DAT", NULL, 3 * state + 1);
    if (!p)
        return -1;
    s_battle_ctx_len = g_last_block_size;
    if (s_battle_ctx_len > (int)sizeof(s_battle_ctx))
        s_battle_ctx_len = (int)sizeof(s_battle_ctx);
    memcpy(s_battle_ctx, p, (size_t)s_battle_ctx_len);
    free(p);
    return 0;
}

const uint8_t *battle_ctx_sprite_record(int idx)
{
    if (idx < 0 || idx >= battle_ctx_sprite_record_count())
        return NULL;
    return s_battle_ctx + CTX_OFF_SPRITE_REC + CTX_SPRITE_STRIDE * idx;
}

/* 0x10D79: standing_sprite_build 的实际数据视图不是第 i 条记录起点，
 * 而是 ctx + 26*i + 131。完整 payload 末尾落在下条记录的前五字节。 */
static const uint8_t *battle_ctx_standing_payload(int idx)
{
    size_t off;

    if (idx < 0)
        return NULL;
    off = 131u + CTX_SPRITE_STRIDE * (size_t)idx;
    if (off + CTX_SPRITE_STRIDE > (size_t)s_battle_ctx_len)
        return NULL;
    return s_battle_ctx + off;
}

/* 0x10BE4: 分组键位于 payload 之后 21 字节，即下条记录的首字节。 */
static int battle_ctx_standing_group(int idx, uint8_t *key)
{
    size_t off;

    if (!key || idx < 0)
        return -1;
    off = 152u + CTX_SPRITE_STRIDE * (size_t)idx;
    if (off >= (size_t)s_battle_ctx_len)
        return -1;
    *key = s_battle_ctx[off];
    return 0;
}

int battle_ctx_sprite_record_count(void)
{
    /* F = (块长 − 126 − 5 尾) / 26；14 战场与 ctx[2] 一致（state0 +1 挂号） */
    if (s_battle_ctx_len < CTX_OFF_SPRITE_REC + CTX_SPRITE_STRIDE + 5)
        return 0;
    return (s_battle_ctx_len - CTX_OFF_SPRITE_REC - 5) / CTX_SPRITE_STRIDE;
}

void battle_ctx_turn_event_arm(int slot, uint8_t turn)
{
    /* 原版 evt30 的两条裸 mov：ctx[3]=turn+1 / ctx[6]=turn+2（0x34E05/
     * 0x34E1B），即回合事件槽 slot0/slot1 的 turn 字段 ctx[3*slot+3]。
     * battle_turn_event_scan 每相位读该字段派发——资产 turn=255 为禁用。 */
    if (slot < 0 || slot >= 16 || 3 * slot + 3 >= (int)sizeof(s_battle_ctx))
        return;
    s_battle_ctx[3 * slot + 3] = turn;
}

/* 0x10C50 standing_sprite_build 全解（2026-09-05）：26B 记录 → 追加战场实体。
 * 属性两分支（[6]icon 判定）：
 *   <0x44 模板表：HP=tpl[3..4]+(lv-1)×growth[6]；MP=tpl[5..6]+(lv-1)×growth[8]；
 *     ATK/DEF/+62 = lv×growth[0]/[2]/[4] + tpl[18..19]/[20..21]/[22..23]；
 *     +31/+32/+59 = tpl[0]/[1]/[7]
 *   ≥0x44 敌基表：HP=lv×base[2..3]u16；MP=lv×base[4]；ATK=lv×base[5]；
 *     DEF=lv×base[6]；+62=lv×base[7]；+31/+32/+59 = base[0]/[1]/[8]
 * 落位：x=布阵条目[0..1]，y=条目[2..3]。道具槽 [10..17]=8 个 id（FF 空）：
 * 槽 0 恒装备(0x40)，槽 1 =（[10]==255 ? 空 : 装备），槽 2..7 常规/空。 */
static void battle_spawn_standing(int rec_idx, const uint8_t *dep)
{
    const uint8_t *payload = battle_ctx_standing_payload(rec_idx);
    uint8_t *ent;
    int icon, lv, hp, mp;

    if (!payload || !g_ent_table || g_ent_count >= FD2_ENT_MAX || !g_roster_table)
        return;
    icon = payload[1];
    lv = payload[4];
    ent = g_ent_table[g_ent_count];
    memset(ent, 0, FD2_ENT_REC_SIZE);

    if (icon < 0x44) {
        const uint8_t *t = enemy_template_entry(icon);
        const uint8_t *g = growth_table_entry(icon);
        hp = (t[3] | (t[4] << 8)) + (lv - 1) * g[6];
        mp = (t[5] | (t[6] << 8)) + (lv - 1) * g[8];
        ent[31] = t[0];
        ent[32] = t[1];
        *(uint16_t *)(ent + 55) = (uint16_t)(lv * g[0] + (t[18] | (t[19] << 8)));
        *(uint16_t *)(ent + 57) = (uint16_t)(lv * g[2] + (t[20] | (t[21] << 8)));
        *(uint16_t *)(ent + 62) = (uint16_t)(lv * g[4] + (t[22] | (t[23] << 8)));
        ent[59] = t[7];
    } else {
        const uint8_t *b = enemy_base_table_entry(icon - 68);
        hp = (b[2] | (b[3] << 8)) * lv;
        mp = b[4] * lv;
        ent[31] = b[0];
        ent[32] = b[1];
        *(uint16_t *)(ent + 55) = (uint16_t)(b[5] * lv);
        *(uint16_t *)(ent + 57) = (uint16_t)(b[6] * lv);
        *(uint16_t *)(ent + 62) = (uint16_t)(b[7] * lv);
        ent[59] = b[8];
    }

    /* 落位（0x10cb0/0x10cc0 分支）：g_spawn_direct(0x53AFA)=1 → 布阵层坐标
     * 直接落位（事件增援）；否则以布阵坐标为目标的全图最近格搜索——
     * 0x10CFC 直读运行时 cell[2] bit6（0x10CA5/0x10CAF 双侧 enemies_zoc_mark
     * 置的占位标记），best 初值 255、d<=best 平局后扫描者胜；扫描毕
     * 0x10D64 field_reset_candidates 清标记。布阵敌条 = x@+0、y@+2、变体@+4
     * （u16×3；原版字面读块内 +2/+4 两字节，与玩家项 pl[0]/pl[2] 同构）。 */
    {
        int tx = dep ? dep[2 + 6 * rec_idx] : 0;
        int ty = dep ? dep[2 + 6 * rec_idx + 2] : 0;

        enemies_zoc_mark(0);                   /* 0x10CA5 非敌方占位 */
        enemies_zoc_mark(1);                   /* 0x10CAF 敌方占位 */
        if (g_spawn_direct) {
            ent[0] = (uint8_t)tx;
            ent[1] = (uint8_t)ty;
        } else {
            int best = 255, bx = tx, by = ty;
            for (int y = 0; y < g_field_h; y++) {
                for (int x = 0; x < g_field_w; x++) {
                    int d;
                    if (field_cell(x, y)[2] & 0x40u)
                        continue;              /* 0x10CFC 被占/禁入格 */
                    d = abs(x - tx) + abs(y - ty);
                    if (d <= best) {
                        best = d;
                        bx = x;
                        by = y;
                    }
                }
            }
            ent[0] = (uint8_t)bx;
            ent[1] = (uint8_t)by;
        }
        field_reset_candidates();              /* 0x10D64 */
    }

    {
        FILE *fp = fopen("FDICON.B24", "rb");
        if (fp) {
            ent[2] = (uint8_t)icon_load_entry(icon, fp);
            fclose(fp);
        }
    }
    ent[3] = 0;
    ent[4] = 0;
    ent[5] = 0;
    ent[6] = payload[0];
    ent[7] = (uint8_t)icon;
    ent[8] = (uint8_t)icon;
    ent[9] = 0;
    ent[10] = 64;                                   /* 槽 0 恒装备 */
    ent[11] = payload[5];
    if (payload[5] == 255) {
        ent[12] = 0x80;
    } else {
        ent[12] = 64;
        ent[13] = payload[6];
    }
    for (int k = 0; k < 6; k++) {
        ent[14 + 2 * k] = payload[7 + k] == 255 ? 0x80 : 0;
        ent[15 + 2 * k] = payload[7 + k];
    }
    memset(ent + 34, 0, 6);
    memmove(ent + 26, payload + 13, 4);
    ent[30] = 0;
    ent[33] = (uint8_t)lv;
    ent[49] = payload[22];
    *(uint16_t *)(ent + 50) = (uint16_t)(payload[23] | (payload[24] << 8));
    ent[52] = payload[17];
    ent[53] = payload[18];
    ent[54] = payload[19];
    ent[61] = payload[2];
    ent[60] = payload[0] == 2 ? 0 : 0xFF;
    *(uint16_t *)(ent + 64) = (uint16_t)hp;
    *(uint16_t *)(ent + 66) = (uint16_t)hp;
    *(uint16_t *)(ent + 68) = (uint16_t)mp;
    *(uint16_t *)(ent + 70) = (uint16_t)mp;
    ent_recompute_derived(g_ent_count);
    g_ent_count++;
}

/* 0x10B4E sprites_reload_slot(key)：重载布阵层，遍历 i<ctx[2] 找
 * ctx[26*i+152]==key
 * 的条目逐个 standing 生成（key=分组键；battle_stage_load 收尾用 0 = 初始组，
 * 其余组由事件脚本触发 = 增援）。 */
void sprites_reload_slot(int key)
{
    uint8_t *dep = dat_load_block("FDFIELD.DAT", NULL, 3 * g_state + 2);
    int count = s_battle_ctx[CTX_OFF_ENEMY_COUNT];

    if (dep) {
        for (int i = 0; i < count && g_ent_count < FD2_ENT_MAX; i++) {
            uint8_t group;
            if (!battle_ctx_standing_payload(i)
                || battle_ctx_standing_group(i, &group) != 0)
                break;
            if (group == (uint8_t)key)
                battle_spawn_standing(i, dep);
        }
        free(dep);
    }

    /* 原版收尾原样持久化 0x32A00B 的 standing-sprite cache；FD2.TMP
     * 不是实体表，决斗演出后会按此格式重读。 */
    {
        FILE *tmp = fopen("FD2.TMP", "wb");
        if (tmp) {
            if (g_standing_sprites)
                fwrite(g_standing_sprites, 1, 0x32A00, tmp);
            fclose(tmp);
        }
    }
}

/* 0x1088D battle_stage_load 全解（2026-09-05）：FDTXT[state+1] 事件文本、
 * FDFIELD[3k+2] 布阵层、[3k+1] ctx、[3k] 地图、FDSHAP 2v/2v+1 影像对；
 * 玩家填充 P 槽（roster 80B memmove + 布阵覆盖 x/y + 重算派生）；
 * state>=13 的 i==6 槽在 roster 候选 char-id!=2 时强制空；收尾
 * sprites_reload_slot(0) 生成分组 0 的初始敌人。
 * 布阵层：u16 头=P+E；敌条目 6B（[0..1]u16 x [2..3]u16 y [4..5]u16 icon）；
 * 玩家条目 6B（[0]x [2]y——battle_stage_load 直证）。 */
int battle_stage_load(int state)
{
    FILE *fp;
    int P, E, roster_idx;
    const uint8_t *pl;

    /* The prologue uses resource-only scenes 31 and 32.  They do not have
     * enter/exit table entries, but their FDFIELD/FDTXT blocks are present. */
    if (state < 0 || state >= FD2_STAGE_COUNT)
        return -1;
    g_pkg_text_evt = dat_load_block("FDTXT.DAT", g_pkg_text_evt, state + 1);
    g_pkg_text_evt_size = g_last_block_size;
    if (battle_ctx_load(state) != 0)
        return -1;
    if (field_load_for_state(state, s_battle_ctx[CTX_OFF_SHAPE]) != 0)
        return -1;
    /* 0x108A6：战场动画背景层随关卡装载（读 g_state，战斗态才有块）。 */
    battle_backdrop_load();

    P = s_battle_ctx[CTX_OFF_PLAYER_SLOTS];
    E = s_battle_ctx[CTX_OFF_ENEMY_COUNT];
    if (P < 0 || P > FD2_ENT_MAX)
        return -1;

    s_deploy_map = dat_load_block("FDFIELD.DAT", s_deploy_map, 3 * state + 2);
    free(g_ent_table);
    g_ent_table = malloc(FD2_ENT_MAX * FD2_ENT_REC_SIZE);
    if (!g_ent_table)
        return -1;
    g_ent_count = P;

    fp = fopen("FDICON.B24", "rb");
    pl = s_deploy_map ? s_deploy_map + 2 + 6 * E : NULL;
    roster_idx = 0;
    for (int i = 0; i < P; i++) {
        uint8_t *rec = g_ent_table[i];
        uint8_t *ros = (g_roster_table && roster_idx < g_roster_count)
                       ? g_roster_table[roster_idx] : NULL;
        if (ros && (state >= 13 || i != 6 || ros[8] == 2)) {
            memmove(rec, ros, FD2_ENT_REC_SIZE);
            if (pl) {
                rec[0] = pl[0];
                rec[1] = pl[2];
                pl += 6;
            }
            if (fp)
                rec[2] = (uint8_t)icon_load_entry(rec[7], fp);
            rec[3] = 0;
            rec[4] = 0;
            rec[6] = 2;
            rec[49] = 0xFF;
            memset(rec + 34, 0, 6);
            ent_recompute_derived(i);
            roster_idx++;
        } else {
            memset(rec, 0, FD2_ENT_REC_SIZE);
            rec[5] = 1;                     /* 空位标记 */
        }
    }
    if (fp)
        fclose(fp);
    free(s_deploy_map);
    s_deploy_map = NULL;
    sprites_reload_slot(0);
    return 0;
}

/* 0x190AC treasure_interact 全解（2026-09-06 窗格链重建）：光标 tile
 * 属性 &0x60：0x20=村庄组（421/422/426/427），否则宝箱组（428/429/430/
 * 431）；未拾取（g_spawner_flags[t]==0）时：kbd_flush →
 * dialog_backdrop_load(ent[actor][7] 肖像) → 提示 421/428@vram+40739 →
 * dato_frame_stamp(0) → confirm_yes_no + confirm_close_anim。非"是"→
 * delay100 → 412@+46819（同窗第二行）→ delay200 → restore。
 * "是"→ sfx_play_b(FDOTHER#31,12,1) + delay300 → ctx[83+3t] 分发：
 * 类型0 物品：g_disp_num_a=item+181 → 422/429@+46819 →
 * ent_inventory_add 成功→标志置位+stamp+wait(0)+restore+refresh→返；
 * 满包→stamp+wait(0)+restore+delay100 → 重开窗 423@+40739+stamp+
 * confirm 对：非是→424@+46819→delay200+restore；是→restore →
 * inventory_pick_slot(actor,0)：选中→get 旧物/remove/add 新物+ctx 参数
 * 改写为旧物（宝箱重装填、标志不置位）→delay100+重开窗 425@+40739
 * （g_disp_num_a=新物+181、g_disp_num_c=旧物+181→token -5）+stamp+
 * wait(0)+restore；取消→delay100+重开窗 424@+40739→delay200+restore。
 * 类型1 金钱：参数非 0→g_disp_num_b=参数+426/430@+46819，空参数→
 * 427/431@+46819 → stamp+wait(0)+restore → g_gold+=g_disp_num_b（原版
 * 空箱加残值，照抄）→ 标志置位+refresh。
 * 其他：delay200+restore → 事件脚本分发（标志不置位）。 */
int treasure_interact(int actor)
{
    uint8_t info[8];
    uint8_t *ent;
    int attr, village, slot;
    const uint8_t *entry;
    int item, yn;

    tile_lookup(g_cursor_x, g_cursor_y, info);
    /* 0x190EB/0x1912A：门与村庄位都在 shape 属性 **byte0（info[4]）** 的
     * 0x60 位域（宝箱/据点标记；tile_event_check 的门即此域==0——两类
     * 互斥）。0x190F6/0x191CB：开启标志 g_spawner_flags 与宝箱表 ctx+83
     * 均按 **info[2] 槽号**（cell[2]&0x1F）索引，非 tile 形状号。 */
    attr = info[4];
    if (!(attr & 0x60))
        return 0;
    village = (attr & 0x20) != 0;
    slot = info[2];
    if (slot < 0 || slot >= (int)sizeof(g_spawner_flags) || g_spawner_flags[slot])
        return 0;

    ent = g_ent_table[actor];
    kbd_flush();
    dialog_backdrop_load(ent[7]);          /* 0x1911D：行动者肖像窗 */
    text_render_box(1, 19, 74, 205, 320, vram_base() + 40739u,
                    village ? 421 : 428, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    yn = confirm_yes_no();
    confirm_close_anim();
    if (yn != 1 || g_menu_choice != 0) {
        delay_ms(100);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 46819u,
                        412, g_pkg_fdtxt0);   /* 0x1954F：同窗第二行 */
        delay_ms(200);
        dialog_backdrop_restore();
        return 0;
    }
    sfx_play_b(g_pkg_fdother_31, 12, 1);   /* 0x191A9 */
    delay_ms(300);

    entry = g_battle_ctx + CTX_OFF_TREASURE + 3 * slot;
    item = entry[1] | (entry[2] << 8);
    if (entry[0] == 0) {
        g_disp_num_a = item + 181;
        text_render_box(1, 19, 74, 205, 320, vram_base() + 46819u,
                        village ? 422 : 429, g_pkg_fdtxt0);
        if (ent_inventory_add(actor, item) != -1) {
            g_spawner_flags[slot] = 1;
            dato_frame_stamp(0);
            wait_key_anim(0);
            dialog_backdrop_restore();
            map_spawn_count_refresh();
            return 1;
        }
        /* 满包：弃置确认 → 换装（0x19274..0x1940B） */
        dato_frame_stamp(0);
        wait_key_anim(0);
        dialog_backdrop_restore();
        delay_ms(100);
        dialog_backdrop_load(ent[7]);
        text_render_box(1, 19, 74, 205, 320, vram_base() + 40739u,
                        423, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        yn = confirm_yes_no();
        confirm_close_anim();
        if (yn == 1 && g_menu_choice == 0) {
            dialog_backdrop_restore();
            if (inventory_pick_slot(actor, 0)) {
                int old = inventory_get_item(actor, g_menu_choice);
                inventory_remove_item(actor, g_menu_choice);
                ent_inventory_add(actor, item);
                g_battle_ctx[CTX_OFF_TREASURE + 3 * slot + 1] = (uint8_t)old;
                g_battle_ctx[CTX_OFF_TREASURE + 3 * slot + 2] =
                    (uint8_t)((unsigned)old >> 8);
                delay_ms(100);
                dialog_backdrop_load(ent[7]);
                g_disp_num_c = old + 181;  /* 0x1937B：token -5 旧物名 */
                text_render_box(1, 19, 74, 205, 320, vram_base() + 40739u,
                                425, g_pkg_fdtxt0);
                dato_frame_stamp(0);
                wait_key_anim(0);
                dialog_backdrop_restore();               /* 0x19564 */
                return 1;
            }
            delay_ms(100);
            dialog_backdrop_load(ent[7]);
            text_render_box(1, 19, 74, 205, 320, vram_base() + 40739u,
                            424, g_pkg_fdtxt0);  /* 0x193F9：选槽取消 */
        } else {
            text_render_box(1, 19, 74, 205, 320, vram_base() + 46819u,
                            424, g_pkg_fdtxt0);  /* 0x19427：弃置同窗二行 */
        }
        delay_ms(200);
        dialog_backdrop_restore();
        return 0;
    }
    if (entry[0] == 1) {
        if (item) {
            g_disp_num_b = item;
            text_render_box(1, 19, 74, 205, 320, vram_base() + 46819u,
                            village ? 426 : 430, g_pkg_fdtxt0);
        } else {
            text_render_box(1, 19, 74, 205, 320, vram_base() + 46819u,
                            village ? 427 : 431, g_pkg_fdtxt0);
        }
        dato_frame_stamp(0);
        wait_key_anim(0);
        dialog_backdrop_restore();
        g_gold += g_disp_num_b;            /* 原版空箱亦加残值（0x194E8） */
        g_spawner_flags[slot] = 1;
        map_spawn_count_refresh();
        return 1;
    }
    /* 事件：先收窗再分发；标志由脚本自理（0x19501..0x19511） */
    delay_ms(200);
    dialog_backdrop_restore();
    g_pending_event_id = (uint8_t)item;
    event_script_dispatch(actor);
    return 1;
}

/* ---- 0x2AF28 编队选人全家（2026-09-13 逐指令重建，撤销"等键桩"
 * 近似——32 人档 roster>16 首次走进击路径即黑屏假死，根因即桩无渲染。
 * 网格槽位 = 名册 1..count-1（槽 0 主角固定，sub_2B777 选中项紧凑到
 * 槽 1 起——战场按名册前排取出击队伍）；每槽 28px 宽 10 列、行高 30，
 * 未入选暗显（0x4E1A6）、入选下沉 3 行透明亮显（0x4E22A）。 ---- */

/* 0x2B749：已选计数（扫描网格域 count-1 项）。 */
static int roster_sel_count(const uint8_t *sel)
{
    int n = 0;

    for (int i = 0; i < g_roster_count - 1; i++)
        if (sel[i])
            n++;
    return n;
}

/* 0x2B777：选中项紧凑重排——槽 0 不动，选中槽（1..）按序前移，
 * 未选中殿后（2560B 整表快照换序）。 */
static void roster_sel_compact(const uint8_t *sel)
{
    uint8_t *tmp = malloc(FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    int out = 1;

    if (!tmp)
        return;
    memcpy(tmp, g_roster_table, FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    for (int i = 0; i < g_roster_count - 1; i++)
        if (sel[i]) {
            memcpy(g_roster_table[out], tmp + (size_t)(i + 1) * 80, 80);
            out++;
        }
    for (int i = 0; i < g_roster_count - 1; i++)
        if (!sel[i]) {
            memcpy(g_roster_table[out], tmp + (size_t)(i + 1) * 80, 80);
            out++;
        }
    free(tmp);
}

/* 网格贴图行裁剪（宿主等价修正）：原版第 4 行（y=190）立绘/光标
 *（y=194）24 行写出 64000 备份缓冲尾（名册满 32 人时 i=30 必触发）。
 * DOS 下静默踩堆无感，宿主 CRT 为堆损坏（0xc0000374，2026-09-13
 * 实测）。等价：按可见行数截断解码（RLE 行序解码，截断即裁剪）。 */
static void roster_stamp_clipped(const uint8_t *spr, uint8_t *dst,
                                 int row, int transparent)
{
    int vis = 200 - row;

    if (vis <= 0)
        return;
    if (transparent)
        sprite24_stamp_transparent_clip(spr, dst, 320, vis);
    else
        sprite24_stamp_dark_clip(spr, dst, 320, vis);
}

/* 0x2B4FB 网格重绘：backup=面板基快照 → 容量 @+11261（基 31）/剩余
 * @+23421（基 42，capacity-count）→ 光标相位钟推进 → 光标单位状态面板
 * unit_stats_stamp(cursor+1) → 光标箭头（fdother_1 帧 0 透明贴
 * @+320*(30*(c/10)+104)+28*(c%10)+23）→ 全网格立绘（图标目录 48*(i+1)
 * + 4*相位；入选 +960 透明版 / 未入选 0x4E1A6 暗色版）。 */
static void roster_grid_redraw(const uint8_t *grid, int capacity,
                               const uint8_t *sel, int cursor)
{
    int phase;

    memcpy(g_screen_backup, grid, FD2_VRAM_SIZE);
    number_stamp_digits(g_screen_backup + 11261, 320, capacity, 31, 2);
    number_stamp_digits(g_screen_backup + 23421, 320,
                        capacity - roster_sel_count(sel), 42, 2);
    field_phase_tick();
    phase = field_cursor_phase();
    if (phase == 3)
        phase = 1;
    unit_stats_stamp(cursor + 1, g_screen_backup);
    if (g_standing_sprites && g_pkg_fdother_1) {
        int row = 30 * (cursor / 10) + 104;
        const uint8_t *arrow = package_frame_ptr(g_pkg_fdother_1, 0);
        uint8_t *dst = g_screen_backup + 320L * row + 28 * (cursor % 10) + 23;

        if (row + 24 > 200)
            roster_stamp_clipped(arrow, dst, row, 1);
        else
            sprite24_stamp_transparent(arrow, dst, 320);
    }
    for (int i = 0; i < g_roster_count - 1; i++) {
        int row = 30 * (i / 10) + 100 + (sel[i] ? 3 : 0);
        uint8_t *pos = g_screen_backup + 320L * row + 28 * (i % 10) + 23;

        if (g_standing_sprites) {
            const uint8_t *dir = g_standing_sprites + 48u * (i + 1);
            uint32_t off = (uint32_t)dir[4 * phase]
                | ((uint32_t)dir[4 * phase + 1] << 8)
                | ((uint32_t)dir[4 * phase + 2] << 16)
                | ((uint32_t)dir[4 * phase + 3] << 24);
            const uint8_t *spr = g_standing_sprites + off;

            if (row + 24 > 200)
                roster_stamp_clipped(spr, pos, row, sel[i]);
            else if (sel[i])
                sprite24_stamp_transparent(spr, pos, 320);
            else
                sprite24_stamp_dark(spr, pos, 320);
        }
    }
}

/* 0x2B67F 等键：无键期间每 BIOS tick 变化重绘网格并整屏提交；
 * 键归一化 E0/52→Enter、53→Esc，另把空格（AL=0x20，扫描码 57）
 * 映作 Enter——0x2B721 实证（2026-09-13 实测补）。 */
static int roster_wait_key(const uint8_t *grid, int capacity,
                           const uint8_t *sel, int cursor)
{
    static uint32_t last_tick;
    static int inited;
    int key;

    while (!kbd_key_avail()) {
        uint32_t now = bios_tick();

        if (!inited || now != last_tick) {
            last_tick = now;
            inited = 1;
            roster_grid_redraw(grid, capacity, sel, cursor);
            memcpy(vram_base(), g_screen_backup, FD2_VRAM_SIZE);
        }
        host_idle_pump();   /* 宿主事件泵（原版裸轮询） */
    }
    key = input_read_key_blocking();
    if (key == 57)                       /* 空格 → Enter（0x2B721） */
        key = SCAN_ENTER;
    return key;
}

/* 0x2B439 强制成员检查：槽 1..bound（=capacity）内须有 name id；
 * 缺失 → 文本 657 提示（backdrop 75 + g_disp_num_a=id+1 +
 * g_field_map 哨兵等键）。返回 1=在队 0=缺失。 */
static int roster_forced_check(int bound, int id)
{
    int found = 0;

    for (int i = 0; i < bound; i++)
        if (g_ent_table[i + 1][8] == (uint8_t)id) {
            found = 1;
            break;
        }
    if (!found) {
        dialog_backdrop_load(75);
        g_disp_num_a = id + 1;
        text_render_box(1, 19, 74, 205, 320, vram_base() + 38175u,
                        657, g_pkg_fdtxt0);
        dato_frame_stamp(0);
        g_field_map = (void *)1;         /* 0x2B4CF 哨兵（不触发重绘） */
        wait_key_anim(0);
        g_field_map = NULL;
        menu_buffers_teardown();
    }
    return found;
}

/* 0x2B843 强制先导向位：name id 所在槽（1..count-1）移到槽 1，
 * 其余顺延；随后整表重载 FDICON 缓存。 */
static void roster_lead_to_front(int id)
{
    uint8_t *tmp;
    int slot = 0, out = 2;

    for (int i = 1; i < g_roster_count; i++)
        if (g_ent_table[i][8] == (uint8_t)id)
            slot = i;
    tmp = malloc(FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    if (!tmp)
        return;
    memcpy(tmp, g_roster_table, FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    memcpy(g_roster_table[1], tmp + (size_t)slot * 80, 80);
    for (int j = 1; j < g_roster_count; j++) {
        if (j == slot)
            continue;
        memcpy(g_roster_table[out], tmp + (size_t)j * 80, 80);
        out++;
    }
    free(tmp);
    free(g_standing_sprites);
    g_standing_sprites = NULL;
    icon_cache_reset();
    {
        FILE *icons = fopen("FDICON.B24", "rb");

        if (icons) {
            for (int k = 0; k < g_roster_count; k++) {
                icon_load_entry(g_roster_table[k][7], icons);
                (void)icon_directory_alias(k, g_roster_table[k][7]);
            }
            fclose(icons);
        }
    }
}

/* 0x2AF28 编队选人整页（全解 2026-09-13）：4×64000 缓冲 → blk5 面板
 * 三贴（帧 20 @grid+2332 / 帧 21 @grid+30085 / 帧 137 RLE @grid+2245）
 * → 初绘 → 12 帧滑入（i=11..0，i==11/5 音效 5）→ 键循环：Enter 反选
 * （音效 7），选满 capacity 即紧凑重排并确认；←→ 循环移位（75/77 音效
 * 0，wrap count-2/count-1→0）；↑↓ 跨 10 槽（72/80）；Esc 弃选 →
 * 12 帧滑出（j=0..11，j==0/7 音效 6）→ 快照还原 → 四缓冲释放。
 * 确认后：重载 FDICON → 按进度强制成员检查（657 缺员提示，失败返 0）
 * → 强制先导向位（sub_2B843）→ 文本 658 二次确认（是→1 否/Esc→0）。 */
int roster_select_menu(void)
{
    uint8_t sel[FD2_ROSTER_MAX] = {0};
    uint8_t *grid;
    int capacity = g_state > 26 ? 19 : 15;
    int cursor = 0, result = 0, yn;

    if (!g_roster_table || g_roster_count <= 1)
        return 0;
    g_menu_work_buf = malloc(FD2_VRAM_SIZE);
    g_menu_snap_buf = malloc(FD2_VRAM_SIZE);
    grid = malloc(FD2_VRAM_SIZE);
    g_screen_backup = malloc(FD2_VRAM_SIZE);
    if (!g_menu_work_buf || !g_menu_snap_buf || !grid || !g_screen_backup) {
        free(g_menu_work_buf); free(g_menu_snap_buf);
        free(grid); free(g_screen_backup);
        g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
        return 0;                        /* 宿主 OOM 护栏（原版无） */
    }
    memcpy(g_menu_snap_buf, vram_base(), FD2_VRAM_SIZE);
    memcpy(grid, g_menu_snap_buf, FD2_VRAM_SIZE);
    /* 0x2AFDA..0x2B039：面板帧偏移取 pkg 偏移表 +0x56/+0x5A（=帧
     * 20/21），第三贴为 package_blit_frame 帧 137。 */
    rle_blit_opaque(package_frame_ptr(g_death_fx_pkg, 20), grid + 2332, 320);
    rle_blit_opaque(package_frame_ptr(g_death_fx_pkg, 21), grid + 30085, 320);
    package_blit_frame(g_death_fx_pkg, grid + 2245, 320, 137);
    roster_grid_redraw(grid, capacity, sel, cursor);   /* 仅合成 backup */
    for (int i = 11; i >= 0; i--) {      /* 0x2B04F 滑入 */
        if (i == 11 || i == 5)
            sfx_play(g_pkg_fdother_31, 5, 1);
        menu_slide_frame(i);
    }
    kbd_flush();
    for (;;) {
        int key = roster_wait_key(grid, capacity, sel, cursor);

        if (key == SCAN_ESC)             /* 0x2B0B3 v5=-1（重绘后再退环） */
            result = -1;
        if (key == SCAN_ENTER) {         /* 0x2B0BB */
            sfx_play_b(g_pkg_fdother_31, 7, 1);
            sel[cursor] ^= 1u;
            if (roster_sel_count(sel) == capacity) {
                result = 1;
                roster_sel_compact(sel); /* 0x2B0EF 选中紧凑 */
            }
            /* 0x2B0D3 v9=77：反选后光标顺移一格（含选满确认拍，
             * 原版 Enter 支落穿 77 处理——2026-09-13 复核补）。 */
            key = SCAN_RIGHT;
        }
        if (key == SCAN_LEFT) {          /* 0x2B0FA 75 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            if (--cursor == -1)
                cursor = g_roster_count - 2;
        }
        if (key == SCAN_RIGHT) {         /* 0x2B120 77 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            cursor++;
            if (cursor == g_roster_count - 1)
                cursor = 0;
        }
        if (key == SCAN_UP && cursor > 9) {        /* 0x2B149 72 */
            sfx_play(g_pkg_fdother_31, 0, 1);
            cursor -= 10;
        }
        if (key == SCAN_DOWN && cursor < g_roster_count - 11) {
            sfx_play(g_pkg_fdother_31, 0, 1);     /* 0x2B16F 80 */
            cursor += 10;
        }
        roster_grid_redraw(grid, capacity, sel, cursor);
        memcpy(vram_base(), g_screen_backup, FD2_VRAM_SIZE);
        if (result)
            break;                       /* while(!v5) */
    }
    for (int j = 0; j <= 11; j++) {      /* 0x2B1B9 滑出 */
        if (j == 0 || j == 7)
            sfx_play(g_pkg_fdother_31, 6, 1);
        menu_slide_frame(j);
    }
    memcpy(vram_base(), g_menu_snap_buf, FD2_VRAM_SIZE);
    free(g_menu_work_buf);
    free(g_menu_snap_buf);
    free(grid);
    free(g_screen_backup);
    g_menu_work_buf = g_menu_snap_buf = g_screen_backup = NULL;
    if (result != 1)
        return 0;                        /* Esc → 0（原版 LABEL_56） */
    /* 0x2B258：确认后整表重载 FDICON（选中重排后目录随槽位走）。 */
    free(g_standing_sprites);
    g_standing_sprites = NULL;
    icon_cache_reset();
    {
        FILE *icons = fopen("FDICON.B24", "rb");

        if (icons) {
            for (int i = 0; i < g_roster_count; i++) {
                icon_load_entry(g_roster_table[i][7], icons);
                (void)icon_directory_alias(i, g_roster_table[i][7]);
            }
            fclose(icons);
        }
    }
    /* 0x2B2BB..0x2B34D：按进度强制成员（缺员 657 提示后判负返 0）。
     * st16 仅在 unit 18 已入名册时要求随队。 */
    {
        int ok = 1;

        if (g_state == 16 && bf_roster_has_unit(18))
            ok = roster_forced_check(capacity, 18);
        else if (g_state == 17 || g_state == 19 || g_state > 25)
            ok = roster_forced_check(capacity, 9);
        else
            switch (g_state) {
            case 18: ok = roster_forced_check(capacity, 16); break;
            case 20: ok = roster_forced_check(capacity, 21); break;
            case 21:
            case 22: ok = roster_forced_check(capacity, 24); break;
            case 25:
                ok = roster_forced_check(capacity, 9)
                     && roster_forced_check(capacity, 29);
                break;
            default:
                break;                   /* 0x2B32F：无强制项，edi 保持 1 */
            }
        if (!ok)
            return 0;
    }
    /* 0x2B358..0x2B3AF：强制先导向位（move-to-slot-1 + 重载图标）。 */
    if (g_state == 17 || g_state == 19 || g_state > 25)
        roster_lead_to_front(9);
    else
        switch (g_state) {
        case 20: roster_lead_to_front(21); break;
        case 21:
        case 22: roster_lead_to_front(24); break;
        case 25:
            roster_lead_to_front(9);
            roster_lead_to_front(29);
            break;
        default:
            break;
        }
    /* 0x2B3B9：文本 658 出击二次确认。 */
    dialog_backdrop_load(75);
    text_render_box(1, 19, 74, 205, 320, vram_base() + 38175u,
                    658, g_pkg_fdtxt0);
    dato_frame_stamp(0);
    g_field_map = (void *)1;             /* 0x2B3F5 哨兵 */
    kbd_flush();
    yn = confirm_yes_no();
    g_field_map = NULL;                  /* 0x2B40B */
    confirm_close_anim();
    menu_buffers_teardown();
    if (yn == -1 || g_menu_choice)
        return 0;                        /* 0x2B42B 否/Esc → 0 留城 */
    return 1;
}

void battle_end_check(void)
{
    g_state_pending = 2;
    for (int i = 0; i < g_ent_count; i++) {
        uint8_t *rec = g_ent_table[i];
        if (rec[6] == 0 && !(rec[5] & 1))
            g_state_pending = 0;          /* 敌人仍在：战斗继续 */
    }
    if (g_ent_count > 0 && g_ent_table[0][5] & 1)
        g_state_pending = 1;              /* 先淡出 */
}
