/* scene.h — ADV 场景页群（逆向自 0x26000-0x2A000，docs/architecture.md §15.3-15.5）
 *
 * 结构：state_run_scene → scene_render_compose → scene_page_dispatch
 *   按 g_scene_line_idx（场景相位 0-4）分发四页 + 10 帧推近/拉远演出。
 */
#ifndef FD2RE_SCENE_H
#define FD2RE_SCENE_H

#include "fd2.h"

extern uint8_t g_scene_line_idx;
extern const uint8_t *g_shop_stock_ptr;
extern const void *g_scene_bg_pkg;   /* 0x53F66 槽位面板/场景背景包（scene.c 定义） */
/* 0x1C1C3/0x1C142（scene.c 定义）：职业适性判定 / 装备槽切换 */
int  weapon_affinity_check(int unit, uint8_t item);
int  equip_slot(int unit, int slot);

/* 场景相位（g_scene_line_idx @0x53F4A）→ 页面/BGM（§15.3 表）：
 *   0=base_camp(13)  1/3=shop(14/15)  4=prep(11)  2=编队分支(不进页)。 */

/* 页面层 */
int  scene_page_dispatch(void);                         /* 0x2670E */
int  scene_base_camp(void);                             /* 0x29300 据点主页 */
int  scene_shop_page(void);                             /* 0x279BC 商店（相位1/3/5）*/
int  scene_prep_page(void);                             /* 0x29DAA 整备（相位4）*/
void scene_zoom_blit(int cx, int cy, const void *backdrop, int scale);  /* 0x2921A 最近邻推近/拉远 */
void scene_menu_draw(int pass);                         /* 0x26CE4 */
int  scene_menu_input(void);                            /* 0x26E38 →1=确认 */

/* 据点子功能（§15.4） */
void unit_status_browser(void);                         /* 0x29620 */
void save_slot_write_menu(int pick_mode);              /* 0x2968D（据点=1/编队态=0） */
int  save_slot_load_menu(void);                        /* 0x2986F */
/* 0x29BCB：返回 1/-1，槽号入 g_menu_choice；三缓冲严格版，尾不收——
 * 调用方（0x29800/0x29853、0x298FB、0x260AB）menu_buffers_teardown 收 */
int  save_slot_pick_record(int load_mode, const uint8_t *record);
void roster_grid_draw(uint8_t *dst, int selected);   /* 0x2810B 基地 roster 两列网格 */
#include <stdint.h>

int  roster_pick_unit(void);         /* 0x27D33：任意角色网格选人（严格版，尾不收——0x26996 收） */

/* 商店四功能（§15.5-15.6；菜单 choice 0-3） */
void shop_weapon_menu(int count, const uint8_t *stock); /* 0x2872B 买入：适性→选人→
                                                           购后可立即装备（1C142 末槽）*/
void shop_item_sell(void);                              /* 0x28CBD 卖出，价×3/4 */
void shop_item_use(void);                               /* 0x28EFE 使用 */
void inventory_transfer(void);                          /* 0x28F65 转交 */

/* 买入配套（§15.6） */
int  shop_stock_build(uint8_t *out);                    /* 0x26A0D 货源：相位1→
                                                           g_shop_stock_ptr+3×12 项 /
                                                           3→+15×8 / 5→+23×8，0xFF 止 */
int  promote_unit_pick(int n, const uint8_t *units,
                       const uint8_t *news);              /* 0x2A857 */
int  eligible_unit_pick(int n, const uint8_t *units,
                        int item);                      /* 0x27F4A 合格者单列选择
                                                           （3 行滚动窗）→1/-1 */
void shop_unit_list_draw(int n, const uint8_t *units,
                         int item, int sel, void *vram);/* 0x2825B 合格名单渲染 */

/* 商店列表 UI 栈（卖/转交/买共享） */
void shop_list_open(int count, const uint8_t *items, int sell);   /* 0x27738 */
int  shop_list_pick(int count, const uint8_t *items, int sell);   /* 0x275E6 */
void shop_item_list_draw(int count, const uint8_t *items,
                         int sel, void *vram, int sell);          /* 0x272D0 */
void shop_list_scroll_down(void);                       /* 0x27816 */
void shop_list_scroll_up(void);                         /* 0x278E7 */
void shopkeeper_anim(void);                             /* 0x28B41 成交店主动画 */

/* 金币动画对（8 位里程计，VRAM 0xA7A90） */
void gold_gain_animate(int amount);                     /* 0x26A7A g_gold += */
void gold_spend_animate(int amount);                    /* 0x26B91 g_gold -= */

/* 整备子功能（§15.5） */
void temple_revive_menu(void);                          /* 0x2A43E 教会复活 */
void class_promote_menu(void);                          /* 0x2AA00 转职 */
void promote_fx_play(int unit, int new_cls);            /* 0x2FB2C 转职演出：
                                                           BG.DAT 0/1/2 轮换底图 +
                                                           FIGANI 旧/新职业帧包，
                                                           滑入→帧循环→白闪→亮相 */
int  dead_units_collect(uint8_t *out);                  /* 0x2A07A →数量 */
int  promote_list_build(uint8_t *units, uint8_t *news); /* 0x2AE0E 可转名单：
                                                           等级≥20 且职业<18 且≠7；
                                                           新职业 +32/+50/52 三档
                                                           （+50 路径可达 53..65） */
int  promote_stat_rolls(int unit);                      /* 0x2AC7D 转职成长掷点：
                                                           5×growth roll（行 3 起
                                                           滚动）+ classinfo 移动
                                                           奖励 + 等级重置1/
                                                           exp 清零/HP·MP 全满 */

/* 数据表（§15.5）：
 * g_revive_price_lut @0x52397 u16[职业系] —— 复活单价（×等级）；
 * g_promote_item_lut @0x523D5 byte[职业] —— 转职消耗道具 id。 */

#endif /* FD2RE_SCENE_H */
