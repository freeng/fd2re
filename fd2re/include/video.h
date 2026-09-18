/* video.h — VGA 模式/调色板/帧解码与 blit */
#ifndef FD2_VIDEO_H
#define FD2_VIDEO_H

#include "fd2.h"

/* 模式切换（main 内联：int386(0x10)，AX=0x0013 / 0x0003） */
void vga_set_mode(int mode);
/* 无 vga_wait_retrace：2026-09-08 全库定案——原版从不读端口 0x3DA，
 * DAC/VRAM 全部裸写（palette_apply_range@0x11D40 直写 0x3C8/0x3C9，
 * 反汇编无任何 0x3DA 访问），宿主呈现节奏由 host 层自管。 */

/* 宿主抽象：DOS 目标 = 0x3C8/0x3C9 端口写 与 0xA0000 线性帧缓冲 */
void  dac_write(int idx, uint8_t r, uint8_t g, uint8_t b);
uint8_t *vram_base(void);

/* DAC 调色板（IDA 0x11D40 palette_apply_range）：
 * 从 g_palette_ptr 读 768B，写 0x3C8/0x3C9，每通道减 dim。 */
void palette_apply_range(int first, int last, int dim);

/* IDA 0x11DF2 palette_add_range：对 [first..last] 每通道加 add（上限 63，
 * DAC 6-bit）。事件脚本用于闪白：(0,255,255)=全白、(0,255,0)=复原。 */
void palette_add_range(int first, int last, int add);

/* IDA 0x2DF01 palette_mix_range：[start..end) 每通道向 (r,g,b) 按
 * scale/40 混合（out = c + scale*(master_pal[i]-c)/40；读主调色板
 * g_palette_ptr。原误名 save_check）。 */
void palette_mix_range(int start, int end, int scale, int r, int g, int b);

/* IDA 0x1F882：i=0..255 逐步 dim 到全黑，每步 delay(2)。 */
void palette_fade_black(void);

/* IDA 0x1F525 fade_in：调色板 0→64 级渐显（每级 delay(2)）。 */
void fade_in(void);

/* IDA 0x4E98D rle_decode_frame：
 * 帧头 u16 w,h；其后 2-bit 控制 RLE（跳过/字面量游程）。
 * flag == -1 为主路径（其余分支未逆向）。 */
void rle_decode_frame(const uint8_t *src, int x, int y,
                      uint8_t *dst, int pitch, int flag);
/* IDA 0x4E8D3 rle_decode_frame_lut：与 rle_decode_frame flag==-1 路径同构
 * （00 实心 / 01 交错 / 10 字面量 / 11 跳过），但所有输出字节过
 * lut[byte]（FDOTHER blk3 的 23×256B 重映射表 = 结果窗格着色）。 */
void rle_decode_frame_lut(const uint8_t *src, int x, int y,
                          uint8_t *dst, int pitch, const uint8_t *lut);
/* IDA 0x4E29C sprite24_stamp：24x24 满铺精灵（4-op 变体：00=实心取色 /
 * 01=隔列取色跨2 / 10=字面量 / 11=固定色 0x49；无透明跳过，
 * 行进 pitch-24——roster 立绘 tile，2026-09-06 定案）。
 * 调用方仅为城镇 UI 族（roster_grid_draw 0x2822A / shop 列表 0x28339）。 */
void sprite24_stamp(const uint8_t *src, uint8_t *dst, int pitch);
/* IDA 0x4E22A —— 2026-09-08 定案（"背景色不透明"症状修复）：24x24
 * 【透明版】，与 0x4E29C 同构 4-op 但 op11=跳过不写、op01 隔点写
 *（写奇数列）。调用方：scene_render_compose 0x266D5（场景立绘）、
 * tile_info_panel_draw 0x1ADC4/0x1AE86、战场 tile 族（field_tile_render
 * /ent_render_one/tile_stamp_shaded）。 */
void sprite24_stamp_transparent(const uint8_t *src, uint8_t *dst, int pitch);
/* 行上限裁剪版（roster 网格第 4 行 y=190 立绘原版写出 64000 缓冲尾，
 * 宿主堆损坏——按可见行截断，RLE 按行顺序解码截断即精确裁剪）。 */
void sprite24_stamp_transparent_clip(const uint8_t *src, uint8_t *dst,
                                     int pitch, int max_rows);
/* IDA 0x4E1A6 —— 2026-09-13 定案：24x24 暗色版（写出像素 (v&7)+24，
 * op11 跳过）——roster_select_menu 网格未入选单位暗显。 */
void sprite24_stamp_dark(const uint8_t *src, uint8_t *dst, int pitch);
void sprite24_stamp_dark_clip(const uint8_t *src, uint8_t *dst,
                              int pitch, int max_rows);
/* IDA 0x4E127 —— 2026-09-12 定案：24x24 固定色剪影(fx_effect_apply)。 */
void sprite24_stamp_silhouette(const uint8_t *src, uint8_t *dst, int pitch,
                               uint8_t color);
/* IDA 0x4DF84 —— 2026-09-12 定案：24x24 八级调色坡闪烁(fx_unit_flash)。 */
void sprite24_stamp_flash(const uint8_t *src, uint8_t *dst, int pitch,
                          uint8_t base, int ramp);
/* IDA 0x187D6 number_stamp_digits：blk5 帧字形数字（基 31+数字，
 * 6px/位；MAX/>99 特例），duel.c 定义。 */
void number_stamp_digits(uint8_t *dst, int pitch, int val,
                         int base, int len);
/* IDA 0x1875D value_stamp：3 位 cur/max 数字（满值色 31/非满 42），
 * duel.c 定义。 */
void value_stamp(uint8_t *dst, int pitch, int cur, int max);

/* IDA 0x4ED34：RAW 帧族透明版（[u16 w][u16 h]+像素，0=跳过）——菜单图标。 */
void stamp_raw_transparent(const uint8_t *frame, uint8_t *dst, int pitch);
/* IDA 0x4ED0B（sub_1685C 下游）：RAW 帧族不透明版（0 也写）——道具/效果图标。 */
void stamp_raw_opaque(const uint8_t *frame, uint8_t *dst, int pitch);
/* IDA 0x16886 package_blit_frame：帧地址 = pkg + u32[pkg+6+4*frame]。 */
void package_blit_frame(const void *pkg, uint8_t *dst, int pitch, int frame);
/* pkg_frame_blit(…,x,y,frame) 的帧地址解析（u32[pkg+6+4*frame]）；
 * 带 (x,y) 偏移的贴图用 rle_decode_frame(ptr,x,y,dst,pitch)。 */
const uint8_t *package_frame_ptr(const void *pkg, int frame);

/* IDA 0x4EBFF stamp_frame_opaque：字节 RLE 帧不透明 blit（0 也写）。
 * DATO 帧0 肖像 / blk5 面板底 / 场景 blk10 覆盖层共用。 */
void rle_blit_opaque(const uint8_t *frame, uint8_t *dst, int pitch);

/* IDA 0x4EBBA rle_frame_blit：同族字节 RLE 透明 blit（0=跳过）。
 * battle_death_anim 消失特效（blk5 帧 68..79）直调此版。 */
void rle_blit_transparent(const uint8_t *frame, uint8_t *dst, int pitch);

/* IDA 0x11EB0 blit_rows：跨距行拷贝。 */
void blit_rows(uint8_t *dst, int dst_pitch, const uint8_t *src,
               int src_pitch, int row_len, int rows);

/* IDA 0x2EB9F blit_frame_flat：平帧绘制（过场用，含 0xA0000 直写）。 */
void blit_frame_flat(const void *src, int plane, uint8_t *dst, int pitch, int flag);

/* IDA 0x22E5C screen_fade_transition：
 * music 淡出 + FDOTHER.DAT 块 79 + 清 VRAM + 平绘 + 分步延时。 */
void screen_fade_transition(void);

#endif
