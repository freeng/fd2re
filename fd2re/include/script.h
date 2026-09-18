/* script.h — 行走/演出脚本播放器（ui_script_exec 子系统）
 *
 * 2026-09-04 逆向定位（IDA 0x1366A ui_script_exec + 0x4EB48 script_stream_get）：
 * "ui_script" 不是图灵机式 VM，而是一种紧凑的**实体走位/朝向编排字节码**，
 * 由序章 st00_exit_prologue、状态 enter/exit 与 90 项事件脚本
 * （g_event_script_table，见 event.h）共同复用 —— 即道具/魔法特效与
 * 演出脚本的公共底座之一。
 *
 * 字节码格式（g_script_streams[id] 指向的流，全部 u8）：
 *   u8  num_records                      记录条数
 *   重复 num_records 次：
 *     u8  ctrl                           控制/步数（bit7 = 记录类型）
 *     u8  count                          本记录实体对数
 *     count × { u8 ent_idx, u8 dir }     参与实体与方向
 *
 * 记录类型（ctrl 高位）：
 *   bit7=0  行走记录：ctrl = 步数。每步播 6 帧走路动画
 *           （ent+3 = dir，ent+4 = 帧号 1..6，每帧 anim_tick_update +
 *           wait_bios_ticks(1)），步末按 dir 移动一格并清帧号：
 *           dir 0=下(+y) 1=左(-x) 2=上(-y) 3=右(+x)。
 *           播放期间 g_script_fade（0x53AFB）非 0 且非 64 时逐帧
 *           palette_apply_range(0,255,fade++) —— 从暗场渐显。
 *   bit7=1, (ctrl&0x7F)>0  转向/等待记录：所有实体先设朝向，
 *           再等 (ctrl&0x7F) 个 tick（每 tick anim_tick_update+flush）。
 *   bit7=1, (ctrl&0x7F)==0  整屏重绘记录：等待 1 tick 后重绘地图缓存
 *           （sub_11EEE + 逐实体 sub_127E0 + sub_129EC + blit_rows
 *            312x192 合成）—— 用于 teleport/换镜后刷新视口。
 *
 * 每步还会 footstep_sfx(ent[0])（0x32230，按 class 查 28B 步频表，
 * 每 N 帧发一次脚步声，特殊单位用音效 10）。
 *
 * 已观测脚本 id：0-8、13、23、24、30、42、43、44、46、74（事件脚本）、
 * 90-98 / 99-105（序章三相位）。表 g_script_streams @0x627D8 共 **106 项**
 * （id 0..105，表区 0x627D8..0x62980，数据区紧随；id 0 实测 5 记录 27B：
 * 4 实体同步走 6 步 → 转向等待 8/8/8/4 tick；序章 99-105 为 1-5 条短记录）。
 */
#ifndef FD2_SCRIPT_H
#define FD2_SCRIPT_H

#include "fd2.h"

/* IDA 0x1366A —— 播放一条行走/演出脚本（阻塞直至完毕）。 */
void ui_script_exec(int script_id);

/* IDA 0x4EB48 —— 取脚本流：g_script_streams[id]（u32 指针表 @0x627D8）。 */
const uint8_t *script_stream_get(int id);

/* IDA 0x32230 —— 走路脚步声：class 查 28B 表(0x52725)得步频 N，
 * 帧计数 g_step_counter(0x54132) 每 N 帧发 sfx 9/10；特殊单位(sub_1F183)固定 6。 */
void footstep_sfx(int unit);

/* 脚本流指针表（dseg03 0x627D8；具体项数未完全清点，已用最大 id=105）。 */
extern const uint8_t *const g_script_streams[];

/* 播放期间渐显计数（0x53AFB）：非 0/64 时每帧 +1 并压暗调色板。 */
extern int32_t g_script_fade;

#endif /* FD2_SCRIPT_H */
