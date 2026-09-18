/* text.h — FDTXT.DAT 文本渲染与图标 */
#ifndef FD2_TEXT_H
#define FD2_TEXT_H

#include "fd2.h"

/* IDA 0x15F84 text_render_box —— 296 xrefs，最高频绘制入口。
 * 原版真实压栈序（cdecl 右到左）：
 *   (pkg, str_id, dst, pitch, fg, shadow=76, bg, a2, a14)
 * fd2re 形参（历史顺序，语义 2026-09-07 激活）：
 *   flag=a14 打字机初值 / a2 行距原参（单行串不可观测，实现恒 19）/
 *   y=bg 底色 / h=fg 前景 / w=pitch。
 * 两族实参（原版压栈五处实证）：
 *   对话框 (1,19,74,205)；列表/面板 (0,0,0,201 选中/205 常态)
 *   （0x29AB2 槽摘要/0x28197 名册网格/0x18587 道具格/0x1CF4A 法术表/
 *    0x181DA 单位面板）。 */
void text_render_box(int flag, int a2, int y, int h, int w,
                     uint8_t *dst, int str_id, const void *pkg);

/* 0x51A83：渲染期间置位（anim_pump / states 轮询）。 */
extern int32_t g_text_busy;

/* IDA 0x11019 icon_load_entry：从 FDICON.B24 将去重后的 12 帧流写入
 * g_standing_sprites。该缓冲的前 1920B 是 40x12 个 u32 帧偏移，整体
 * 0x32A00B 可原样写入/读回 FD2.TMP。 */
int icon_load_entry(int icon_idx, void *file);
int icon_frame_get(int cache_idx, int frame, const uint8_t **stream,
                   size_t *stream_len);
void icon_cache_reset(void);
/* 把 icon_idx 已加载的 12 帧目录项复制到 dst_slot（名册位置槽）——
 * 修复转职业导致 rec[7] 重复时去重压缩槽位、名册网格按位置读目录
 * 错位（原版 DOS 复用块内旧偏移掩盖，见 text.c 实现注）。 */
int icon_directory_alias(int dst_slot, int icon_idx);

/* 0x16559 dato_frame_stamp（2026-09-06 定名；旧名 text_str_get 系
 * "取字符串"误植——0x16559 与文本串无任何关系，旧实现即因此误写成
 * 指针查询、整个游戏无口型/肖像复位，本批撤销）：
 *   ebx = 0xA0000 + dword_53C67；eax = g_dato_speaker_blk[dir[frame]]
 *   53C67==0x9017(36887) → stamp_frame_opaque_mirror（行内自 origin
 *   向左）否则 stamp_frame_opaque（正向）。
 * 即把当前说话人 DATO 块第 frame 帧不透明盖到 VRAM+g_portrait_origin。
 * 帧 0=闭口、1/2=半张/张、3=菜单期单拍（0x26EDA 用）。
 * 返回 0=已贴 / -1=无说话人块或坏帧。 */
int dato_frame_stamp(int frame);

/* 0x52387 g_dialog_backdrop_ids：场景相位 → 说话人 DATO 块号。
 * {酒馆129, 商店1=128, 出击0(未用，出击确认传 75), 商店3=130,
 *  整备131, 隐藏店132}。 */
extern const uint8_t g_dialog_backdrop_ids[6];

/* 场景窗活期暴露 g_menu_snap_buf 等价快照（金币面板 0x27A8C 双写用）。 */
uint8_t *dialog_backdrop_snapshot(void);

/* 0x16C57 —— 等键 + 动画窗格（mode==1 走 g_death_fx_pkg 帧 18/19）。
 * 原版尾部复用键盘读取路径并返回规范化扫描码；调用者可以忽略该值。 */
int wait_key_anim(int mode);

/* 0x1E5C0 —— 等键或超时：idle_pump 轮询，任一键可用、BIOS tick 差
 * ≥ ticks 或午夜回绕（当前值 < 起始值）即返回；返回前 kbd_flush。
 * sub_1A866 状态播报传 10（≈0.55s）。 */
void wait_key_ticks(int ticks);

/* 0x16E24 族 —— 对白窗文本上滚一行；升级窗成长行（0x1E529
 * stat_growth_roll 的 line==3 特调）消费。 */
int dialog_scroll_text(void);

/* 0x1956B dialog_backdrop_load（场景版对白窗，2026-09-06 全解重建）：
 * 三缓冲 malloc → 快照/框合成（panel9_grid 5,112,19,5）→ 帧按说话人
 * 设肖像位（128..132 → 4283/1707/3939/1398/3644，缺省 36887）→ 帧0
 * 镜像盖进框缓冲 → 6 步滑入（行 177..112 步距 13，无延时）。
 * 0x196CB dialog_backdrop_restore：下滑收尾（125..177）+ 快照恢复 +
 * free + anim_tick_update(0)；0x26996 menu_buffers_teardown 同收尾
 * 无战场重绘（场景页循环每轮用）。 */
void dialog_backdrop_load(int variant);
void dialog_backdrop_restore(void);
/* 0x26996 统一收尾（2026-09-08 扩为双半边）：对话框半边 = s_dialog
 * 三缓冲（下滑收起 125..177 + VRAM←快照 + free）；选人器/列表半边 =
 * g_menu_work_buf/g_menu_snap_buf/g_screen_backup 三缓冲（0x27D33/
 * 0x27F4A/0x27738 族尾不 free，由此收）——同带下滑 + 快照恢复 +
 * free。两半各自守卫，重复调用无害。 */
void menu_buffers_teardown(void);

/* sub_1974C 带滑提交原语（对话框滑入/收尾与选人器/列表三缓冲共用）：
 * work←snap 全屏；box 行 112.. 的 310 宽面板带并到 work 行 row..
 * （86 行或 200-row 截断）；VRAM←work。步间无延时，写入由 VGA 扫描器可见。 */
void dialog_band_blit(int row, uint8_t *work, const uint8_t *box,
                      const uint8_t *snap);

/* dword_53C67 肖像基址读写（0x29645/0x2965E：unit_status_browser
 * 对 unit_status_page 的围保存/恢复）。 */
int  dialog_portrait_origin(void);
void dialog_portrait_origin_set(int origin);

/* 0x29664..0x2967F：unit_status_browser 尾按当前页说话人重载
 * DATO 块（原版 g_dato_speaker_blk 替换式存活——状态页图标块共用
 * 同一句柄会顶替说话人块，故复位）。返回新载块指针（unit_panel_
 * draw 0x17F1C 同路装载后取表帧 0 静态贴）。 */
const uint8_t *dialog_speaker_reload(int variant);

/* 0x19953 confirm_yes_no（2026-09-06 全解重建）：是/否双按钮（FDOTHER
 * blk2 帧 3*{16,17}+闪烁位，影带合成 @屏(168,232..287)）4 步分开滑入；
 * 等待期 blink 闪选 + 店主口型帧0/3；Enter/Space=1、Esc/Del=-1、
 * ←/→=g_menu_choice 0/1。0x197E5 confirm_close_anim 为其收尾（4 步
 * 收敛 + 框区回拷）——两者必须成对调用（原版 12 调用点纪律）。 */
int  confirm_yes_no(void);
void confirm_close_anim(void);

/* ui_script_exec 已迁至 script.h（行走/演出脚本播放器，2026-09-04）。 */

#endif
