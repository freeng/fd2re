/* resource.h — LLLLLL DAT 资源包访问（IDA 0x111BA dat_load_block） */
#ifndef FD2_RESOURCE_H
#define FD2_RESOURCE_H

#include "fd2.h"

/* dat_load_block(path, old_block, res_id)
 *  - 打开 DAT，seek 到 4*res_id+6 读 u32 目录对 (start, next)
 *  - malloc(next-start) 读入整块，fclose，返回块指针
 *  - g_last_block_size 副作用记录块长
 *  - old_block 非空则先 free（换包/换曲语义）
 * 块内布局（2026-09-05 真实资产验证，blk6 全量校验）：[0..3] 块魔数
 * （如 'LMI1'）+ [4..5] u16 帧数 + [6..] u32 帧偏移表（帧数+1 项，
 * 末项 = 块尾哨兵）；帧 = u16 w, u16 h, RLE 数据（流恰好 w×h 耗尽，
 * blk6 229/230 帧验证通过；帧 125 为非标准记录待查）。
 */
void *dat_load_block(const char *path, void *old_block, int res_id);

/* 打开失败/内存不足时原版 printf 后退出（0x1005E 共享尾）*/
void dat_load_fatal(const char *path, int res_id);

/* 宿主只读 DAT 驻留镜像（SMARTDRV 等价，2026-09-08）：首次整读缓存，
 * 后续读取全内存命中；文件缺失/读短返回 NULL（调用方自行 fatal）。
 * 游戏会重写的文件（存档）禁止走此层——镜像不会失效。 */
const uint8_t *dat_file_image(const char *path, long *out_size);

/* ---- 帧级解码（2026-09-05 逆向实现，docs §6.5） ----
 * RLE 方案（0x4EC66）：字节 <=0xC0 = 字面色号(0..192)；
 * 0xC1..0xFF = 后跟色号、重复 (b-0xC0) 次；输出 0 = 透明。
 * pkg_frame_blit(0x15F0E)：帧 = pkg[4*frame+6] 偏移 → [w,h] i16 + RLE
 * 流；逐像素渲染非零写 dst+y*pitch+x。rle_decoder_init 会把目标区
 * 背景保存进工作区 +8（头 {w,h,offset}）——pkg_frame_free(dst,pitch,
 * work)（0x15E71）按头逐行恢复背景再 free = 帧"贴上/擦除"动画对
 * （2026-09-07 反汇编定案）。原版不调 free 的泄漏点用 pkg_frame_discard。 */
void *pkg_frame_blit(const uint8_t *pkg, uint8_t *dst, int pitch,
                     int x, int y, int frame);
void  pkg_frame_free(uint8_t *dst, int pitch, void *work);
void  pkg_frame_discard(void *work);

/* FDSHAP tile 4 操作解码（0x4E22A 语义，docs §6.7；FDSHAP 20 块
 * 288+ tile 流耗尽验证）：控制字节 bit7/bit6 选操作（00=RLE run /
 * 01=隔点写 / 10=字面 / 11=透明跳）、低 6 位 count-1。
 * 返回消费字节数（严格流耗尽语义），越界 -1。out 需零初始化。 */
int tile4_decode(const uint8_t *stream, size_t len, int w, int h,
                 uint8_t *out);

#endif
