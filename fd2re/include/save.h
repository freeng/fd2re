/* save.h — FD2.SAV 存档 */
#ifndef FD2_SAVE_H
#define FD2_SAVE_H

#include "fd2.h"

/* Host-side path is supplied by the build .env; tests retain the local default. */
const char *save_file_path(void);

/* 存档存在性/完整性校验（读 FD2.SAV + 校验和）。
 * 注：原版此处曾被记为 0x2DF01 "save_check"——该地址实为调色板函数
 * palette_mix_range（§13.5）；本校验为 load+checksum 组合，2026-09-04 更正。
 * 记录长 0x59CB；进度字节 +0x30C5（0xFF = 无进度）。 */
int  save_check(void);

/* 续档路径（state_boot_driver 0x25FA6+）：malloc(0x59CB) + fread FD2.SAV。 */
int  save_record_load(uint8_t *record /* size FD2_SAVE_SIZE */);

/* 标题菜单的章节槽路径只执行原版 0x25FCD..0x25FE0：
 * fread(0x59CB) + save_transform，不读取尾校验和。标题菜单本身
 * 另用 save_record_load 做校验来决定条目数；两条语义不能合并。 */
int  save_record_read_decoded(uint8_t *record /* size FD2_SAVE_SIZE */);

/* 0x59CB 记录缓冲（宿主侧单例，替代原版栈/堆局部）。 */
uint8_t *save_record_buf(void);

/* 进度字节语义（决定菜单条目数 1/2/3）。 */
int  save_progress(const uint8_t *record);

/* 写档端（save_menu_run@0x19DF7，2026-09-04 §8）：以旧档为底覆写
 * 14 个字段区（+0 战场 ctx / +2211 roster / +4771 实体 / +12451 spawner /
 * +12483..12500 状态与光标与音频开关 / +22983 校验和）→ fwrite 22987B。 */
int  save_game_write(void);

/* 0x4DF28/0x4DF09 公开原语（2026-09-05，槽位菜单共用）：
 * save_transform = 对称异或流编/解码；save_sum = 尾 4 字节外的字节和。 */
void    save_transform(uint8_t *record, size_t length);
uint32_t save_sum(const uint8_t *record, size_t length);

/* 章节快照槽区（save_slot_write/load_menu@0x2968D/0x2986F，§8）：
 * +12587..+22986 = 4×2600B（roster 2560B + 摘要 10B；槽进度字节=槽+2560，
 * 即 +15147/+17747/+20347/+22947）。据点菜单"存档/读档"用，与战斗存档同文件。 */
#define FD2_SAVE_SLOT_BASE   12587
#define FD2_SAVE_SLOT_STRIDE 2600
#define FD2_SAVE_SLOT_COUNT  4

#endif
