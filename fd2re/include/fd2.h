/* fd2.h — FD2 重构公共定义
 *
 * 逆向来源：FD2.EXE（WATCOM 32-bit DOS extender）。
 * 本文件中的地址注释均为 IDA 静态地址（镜像基址 0x10000）；
 * 运行时线性地址 = IDA + 0x19C000。
 */
#ifndef FD2_H
#define FD2_H

#include <stdint.h>
#include <stddef.h>

/* ---- 常量（已验证） ------------------------------------------------ */
#define FD2_SCREEN_W      320           /* VGA mode 13h */
#define FD2_SCREEN_H      200
#define FD2_VRAM_SIZE     (FD2_SCREEN_W * FD2_SCREEN_H)  /* 0xFA00 */
#define FD2_STATE_COUNT   30            /* g_state_enter/exit 表项数 */
#define FD2_STAGE_COUNT   33            /* FDFIELD/FDTXT 场景资源：0..32；31/32 为序章内部场景 */
#define FD2_ENT_REC_SIZE  80            /* 实体/花名册记录（IDA: malloc(2560)=32*80） */
#define FD2_ROSTER_MAX    32
#define FD2_ENT_MAX       96            /* 战场实体表（原版 malloc(7680)=96*80） */
#define FD2_STATE_CFG_SZ  31            /* g_state_config 每项字节数（0x4E809） */
#define FD2_SAVE_SIZE     0x59CB        /* FD2.SAV 记录长度（F-005/0x25EBB） */
#define FD2_SAVE_PROGRESS_OFF 0x30C5    /* 进度字节（0xFF=无进度） */
#define FD2_DAT_MAGIC     "LLLLLL"      /* DAT 目录魔数 */

/* 扫描码重映射（input_wait_key, IDA 0x11AA8） */
#define SCAN_ESC          0x01
#define SCAN_ENTER        0x1C
#define SCAN_DEL          0x53
#define SCAN_KP_ENTER     0x52
#define SCAN_PREFIX_E0    0xE0
#define SCAN_UP           0x48
#define SCAN_DOWN         0x50
#define SCAN_LEFT         0x4B
#define SCAN_RIGHT        0x4D

/* ---- 全局状态（对应 dseg02 数据，地址注释 = IDA） ------------------ */
extern int32_t  g_state;             /* 0x53C03 runtime 0x1EFC03，0..29 */
extern int32_t  g_state_pending;     /* 0x53ECC: 1=淡出后切换 2=立即切换 */
extern uint8_t  g_transition_busy;   /* 0x51AAC: 切换期间清 0，恢复 1 */
extern int32_t  g_last_block_size;   /* 0x53BFF: dat_load_block 长度副作用 */

/* 资源包指针（main 启动加载；见 docs/architecture.md §5） */
extern void    *g_pkg_fdother_31;    /* 0x53EEC */
extern void    *g_anim_sfx_data;     /* 0x5414B 动画帧音效包 */
extern void    *g_fdtxt_ptr_table;   /* DATO.DAT 相位块（ending 角色卡/状态页，
                                      定义于 scene.c） */
/* 选人器/列表三缓冲（0x27D33/0x27F4A/0x27738 族 malloc 0xFA00×3；
 * 0x26996 menu_buffers_teardown 统一收尾：面板带下滑 + VRAM←快照 +
 * free——选人器/列表尾不 free，缓冲由调用方收。对话框侧缓冲在
 * text.c s_dialog（同一 0x26996 的另半边）。定义于 scene.c。 */
extern uint8_t *g_menu_work_buf;    /* 0x53C5B sub_1974C 步进暂存 */
extern uint8_t *g_menu_snap_buf;    /* 0x53C5F 开面板前整屏快照 */
extern uint8_t *g_screen_backup;    /* 0x53C63 面板/列表合成源 */
extern uint8_t *g_standing_sprites;         /* 0x53A61 立绘缓存（FD2.TMP 重读者） */
extern int32_t  g_roster_grid_col;          /* 0x53F72 roster 立绘列选择（==3→1） */
extern void    *g_pkg_fdother_1;     /* 0x53A4D */
extern void    *g_pkg_fdother_2;     /* 0x53A89 */
extern void    *g_pkg_fdother_3;     /* 0x53A6D */
extern void    *g_pkg_fdother_4;     /* 0x53A75 */
extern void    *g_pkg_fdother_5;     /* 0x53A81 */
extern void    *g_pkg_fdtxt0;        /* 0x53A7D FDTXT.DAT 块 0（文本字形） */
extern void    *g_pkg_text_evt;       /* 0x53A79 事件对白文本包（按状态装载） */
extern void    *g_pkg_fdother_6;     /* 0x53AD1 */
extern void    *g_magic_sfx_pkg;     /* 0x53B13，FDOTHER.DAT 块 80 */
extern void    *g_palette_ptr;       /* 0x53A65 当前调色板（768B） */
extern void    *g_pkg_fdother_13;    /* 0x53F66 续档场景 */
/* 每个指针绑定的 DAT 块长度。文本与字形解码必须以此为界，不能依赖
 * -1 哨兵穿过损坏或尚未逆明的资源。 */
extern int32_t  g_pkg_fdother_4_size;
extern int32_t  g_pkg_fdother_5_size;
extern int32_t  g_pkg_fdother_3_size;
extern int32_t  g_pkg_fdtxt0_size;
extern int32_t  g_pkg_text_evt_size;

/* 状态机表 */
extern void   (*g_state_enter[FD2_STATE_COUNT])(void);  /* 0x51DE9 */
extern void   (*g_state_exit[FD2_STATE_COUNT])(void);   /* 0x51D71 */
extern uint8_t  g_state_bgm[FD2_STATE_COUNT];           /* 0x51E63 */
extern uint8_t  g_state_bgm_alt[FD2_STATE_COUNT];       /* 0x51E81 敌方相 BGM（battle_turn_end 音乐切换对表） */
extern uint8_t  g_state_roster_menu[FD2_STATE_COUNT];   /* 0x523E7: 1=state_run_scene 走编队界面（子菜单），22-24/27-29 */
extern uint8_t  g_state_config[FD2_STATE_COUNT][FD2_STATE_CFG_SZ]; /* 0x6236E */

/* 实体/花名册表（80B 记录，battle_field_init/战场系统，battle_strike_formula 验证）：
 * 本段偏移 hex 记法并括注十进制（2026-09-12 教训：旧行 "+34" 系 hex
 * 却被按十进制 34 实现，行为号读成 +22h 状态区——ctx 生成单位行为恒 0，
 * stage1 村民逃跑（行为 4）误走攻击链）。
 * +0/+1  格 x/y；+3 阵营(0=玩家,2=敌)；+5 flags(bit0=已编入 roster，bit2=阵亡移除，
 *        bit7=本回合已行动，battle_load_map 开局清)；+6 type(2=玩家操控,1=友军NPC,0=敌)；
 * +7     icon idx；+8 char-id（battle_sync_to_roster 匹配键）；
 * +A..+19 8×2 字节物品/武器槽（十进制 10..25，ent_inventory_add）；
 * +22..+27 状态计时器（十进制 34..39；fx_drain_atk@0x227D0 写 +22h 等）；
 * +1F    AI 子类型（31）；+20 class（32）；+21 level（33）；
 * +34    AI 行为号(+34&0xF)与 +35/+36 参数（52/53/54；standing 记录[17..19]）；
 * +25    参战标记（37，battle_award_exp_list）；+26 已行动标记（38）；
 * +31..+33 掉落物品（49..51）；+37..+46 成长修正后五维（55..70）；+3C exp（60）；
 * +0x40  HP u16；+0x42 MaxHP u16；+0x48 ATK；+0x4A DEF；+0x4C HIT；+0x4E EVADE */
extern uint8_t (*g_ent_table)[FD2_ENT_REC_SIZE];  /* 0x53A45 */
extern int32_t  g_ent_count;                      /* 0x53BEB */
extern uint8_t (*g_roster_table)[FD2_ENT_REC_SIZE]; /* 0x53BF7, 32 项 */
extern int32_t  g_roster_count;                   /* 0x53BFB */

/* ---- 战斗子系统（2026-09-04，见 reverse/battle/digest.md） ------------- */
/* 主循环 anim_pump@0x117E7 每 key 一轮；行动后按序回调：
 * g_state_post_action[g_state](unit) → enemy_phase_check
 * → g_event_script_table[g_pending_event_id](unit)（事件/剧情脚本，见 event.h） */
extern void   (*g_state_post_action[FD2_STATE_COUNT])(int unit); /* 0x51B19，默认 battle_end_check@0x205B4 */
/* g_event_script_table[90] @0x51B91 与 g_pending_event_id @0x51A8F 的声明
 * 已迁至 event.h（2026-09-04；原误称 g_action_post_hook/g_last_action_id）。 */
extern int32_t g_cur_unit_idx;                       /* 0x53AE9，Esc/L 循环起点 */
extern uint32_t g_cursor_x, g_cursor_y;              /* 0x53AB1/0x53AB5 */
extern int32_t g_cursor_view_x, g_cursor_view_y;    /* 0x53AB9/0x53ABD */
extern uint32_t g_action_saved_x, g_action_saved_y;  /* 0x51CF9/0x51CFD */
extern uint32_t g_scroll_x, g_scroll_y;              /* 0x53AA9/0x53AAD（anim_tick_update 视口） */
extern int32_t  g_view_w, g_view_h;                  /* 0x51A87/0x51A8B 视口格数(13,8) */
extern int32_t  g_turn_count;                        /* 0x53BEF，battle_turn_end 递增 */
extern int32_t  g_gold;                              /* 0x53BF3，battle_show_results 累加 */
extern int32_t  g_exp_gained;                        /* 0x53EC8，本次行动经验(上限99) */
extern int32_t  g_unit_acted;                        /* 0x53C53 */
extern int32_t  g_menu_choice;                       /* 0x53C57：1=道具 2=魔法 0=攻击 */
extern int32_t  g_ai_score_target;                   /* 0x53C23 */
extern int32_t  g_ai_target_x, g_ai_target_y;         /* 0x53C27/0x53C2B */
extern int32_t  g_ai_target_item_slot;               /* 0x53C2F */
extern int32_t  g_ai_score_move;                     /* 0x53C33 */
extern int32_t  g_ai_move_x, g_ai_move_y;             /* 0x53C37/0x53C3B */
extern int32_t  g_ai_move_item_slot;                 /* 0x53C3F */
extern int32_t  g_ai_target_unit;                    /* 0x53C4B */
extern int32_t  g_ai_score_attack;                   /* 0x53C4F */
extern int32_t  g_ai_attack_x, g_ai_attack_y;        /* 0x53C43/0x53C47 普攻站位（sub_14237） */
extern int32_t  g_cursor_anim_frame;                 /* 0x53C1F（plan_a 贯穿光标帧） */
extern uint8_t  g_attack_miss_flag;                  /* 0x53C6B（melee_strike_compute 置位） */
extern int32_t  g_disp_num_a;                       /* 0x53AD9 战果/事件显示数（+181 文本id） */
extern int32_t  g_disp_num_c;                       /* 0x53ADD token -5 嵌套文本 id（换装 425 旧道具名） */
extern int32_t  g_disp_num_b;                       /* 0x53AE1 战果/金钱显示数 */
extern uint8_t  g_save_flag_53af9;                  /* 0x53AF9，战斗存档 +12497 */
extern uint8_t  g_save_flag_51aab;                  /* 0x51AAB，战斗存档 +12498 */
extern uint8_t *g_battle_ctx;                        /* 0x53A55，2211B：spawn 定义/杂项（存档 +0） */
extern uint8_t  g_spawner_flags[32];                 /* 0x53AD5（存档 +12451） */
extern uint8_t  g_class_crit_base[];                 /* 0x524A8，按 class-1 暴击率 */
extern uint8_t  g_state_duel_bg[FD2_STATE_COUNT];    /* 0x52470，决斗背景覆盖(TAI.DAT id) */
extern uint8_t  g_ending_duel_bg;                    /* 0x54133，结局蒙太奇决斗标志（≠0 时 duel_scene_load 跳过恢复尾） */
extern const int32_t *g_tile_atk_pct, *g_tile_def_pct; /* 0x51A12/0x51A2A 静态 i32[6] 地形攻/防%（attr=info[5] 索引） */
extern void    *g_death_fx_pkg;                      /* 0x53A81 = g_pkg_fdother_5，死亡特效帧 68-79 */
extern void    *g_pkg_fdother_64;                    /* 0x53B0F FDOTHER blk64：敌方相近战音效包（attack_anim_play），53AF9 门控随 AI 相装卸 */
extern void    *g_field_map;                         /* FDFIELD.DAT 当前地图块 */
extern int32_t  g_field_map_size;                    /* 当前地图块字节数 */
extern void    *g_shape_tiles;                       /* FDSHAP.DAT 偶数块：24x24 tile 流 */
extern int32_t  g_shape_tiles_size;                  /* 偶数块字节数 */
extern void    *g_shape_map;                         /* FDSHAP.DAT 奇数块：4B/tile 属性 */
extern int32_t  g_shape_map_size;                    /* 奇数块字节数 */
extern int32_t  g_field_w, g_field_h;                /* FDFIELD 块头宽/高 */

/* FD2.SAV 22987B 布局（save_game_load@0x10010） */
#define FD2_SAVE_CTX_OFF     0        /* +0..2211   g_battle_ctx */
#define FD2_SAVE_ROSTER_OFF  2211     /* +2211..4771 roster 32x80 */
#define FD2_SAVE_ENT_OFF     4771     /* +4771..12451 ent_table 80*count */
#define FD2_SAVE_SPAWN_OFF   12451    /* +12451..12483 g_spawner_flags 32B */
#define FD2_SAVE_AUX_OFF     12483    /* +12483 aux */
#define FD2_SAVE_COUNT_OFF   12484    /* +12484 ent_count */
#define FD2_SAVE_STATE_OFF   12485    /* +12485 g_state（=进度字节 0x30C5） */
#define FD2_SAVE_SCROLL_OFF  12486    /* +12486/12487 legacy aliases */
#define FD2_SAVE_CURSOR_OFF  12488    /* +12488/12489 legacy aliases */
#define FD2_SAVE_VIEW_OFF    12490    /* +12490/+12491 cursor viewport x/y */
#define FD2_SAVE_ROSTER_COUNT_OFF 12492
#define FD2_SAVE_GOLD_OFF    12493    /* +12493..+12496 little-endian u32 */
#define FD2_SAVE_FLAG_53AF9_OFF 12497
#define FD2_SAVE_FLAG_51AAB_OFF 12498
#define FD2_SAVE_MUSIC_GATE_OFF 12499
#define FD2_SAVE_SFX_GATE_OFF   12500
#define FD2_SAVE_CKSUM_OFF   22983    /* +22983 u32 校验和 */

#endif /* FD2_H */
