/* save.c — FD2.SAV 存档读入与校验 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "save.h"
#include "audio.h"

/* 原版 0x1006A 等 8 处：fopen("FD2.SAV", ...) 裸文件名，相对进程当前
 * 目录（DOS CWD）；宿主同样以相对路径打开，不做任何路径改写。 */
#define FD2_SAVE_FILE "FD2.SAV"

const char *save_file_path(void)
{
    return FD2_SAVE_FILE;
}

static uint8_t s_record[FD2_SAVE_SIZE];

uint8_t *save_record_buf(void) { return s_record; }

static uint16_t rol16(uint16_t value, unsigned int count)
{
    return (uint16_t)((value << count) | (value >> (16 - count)));
}

/* IDA 0x4DF28: in-place byte transform used before layout restoration.
 * 对称异或流（key 初值 165，每字节 key=rol16(key-28652,3)）——槽位菜单
 * （scene.c）写/读槽区同样需要，2026-09-05 起公开为 save_transform。 */
void save_transform(uint8_t *record, size_t length)
{
    uint16_t key = 165;
    for (size_t i = 0; i < length; i++) {
        key = rol16((uint16_t)(key - 28652u), 3);
        record[i] ^= (uint8_t)key;
    }
}

/* IDA 0x4DF09: unsigned-byte sum excluding the trailing checksum word.
 * 槽位菜单写槽时同样调用，公开为 save_sum。 */
uint32_t save_sum(const uint8_t *record, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < length - 4; i++)
        sum += record[i];
    return sum;
}

int save_record_load(uint8_t *record)
{
    FILE *fp = fopen(save_file_path(), "rb");
    if (!fp)
        return 0;
    if (fread(record, 1, FD2_SAVE_SIZE, fp) != FD2_SAVE_SIZE) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    save_transform(record, FD2_SAVE_SIZE);
    uint32_t stored = (uint32_t)record[FD2_SAVE_SIZE - 4]
                    | ((uint32_t)record[FD2_SAVE_SIZE - 3] << 8)
                    | ((uint32_t)record[FD2_SAVE_SIZE - 2] << 16)
                    | ((uint32_t)record[FD2_SAVE_SIZE - 1] << 24);
    return save_sum(record, FD2_SAVE_SIZE) == stored;
}

int save_record_read_decoded(uint8_t *record)
{
    FILE *fp;

    if (!record)
        return 0;
    fp = fopen(save_file_path(), "rb");
    if (!fp)
        return 0;
    if (fread(record, 1, FD2_SAVE_SIZE, fp) != FD2_SAVE_SIZE) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    /* 0x25FCD..0x25FE0: the chapter-slot branch deliberately does not
     * compare the decoded checksum.  The original also ignores fread's
     * short-read count; fd2re treats that malformed edge as a missing file
     * at the caller and presents an all-FF slot table. */
    save_transform(record, FD2_SAVE_SIZE);
    return 1;
}

int save_check(void)
{
    return save_record_load(s_record);
}

int save_progress(const uint8_t *record)
{
    return record[FD2_SAVE_PROGRESS_OFF] == 0xFF ? -1 : record[FD2_SAVE_PROGRESS_OFF];
}

/* 写档端（save_menu_run@0x19DF7，2026-09-04 §8）：
 * malloc 22987 → 先读旧档作底（无档则槽位进度字节 +15147/+17747/+20347/
 * +22947 填 FF）→ 覆写 +0/+2211/+4771/+12451/+12483..12500（g_turn_count/
 * ent_count/state/scroll/cursor/cursor_view/roster_count/g_gold/音频开关）→
 * +22983 校验和（save_checksum）→ save_decode 终化 → fwrite。 */
int save_game_write(void)
{
    uint8_t record[FD2_SAVE_SIZE];
    FILE *fp;

    if (!g_battle_ctx || !g_roster_table || g_ent_count < 0 || g_ent_count > 96
        || (g_ent_count && !g_ent_table))
        return -1;

    /* The DOS writer starts from the previous record so chapter snapshots
     * and unowned bytes remain intact across a battle save. */
    fp = fopen(save_file_path(), "rb");
    if (fp && fread(record, 1, sizeof(record), fp) == sizeof(record)) {
        fclose(fp);
        save_transform(record, sizeof(record));
    } else {
        if (fp)
            fclose(fp);
        memset(record, 0, sizeof(record));
        for (int slot = 0; slot < FD2_SAVE_SLOT_COUNT; slot++)
            record[FD2_SAVE_SLOT_BASE + slot * FD2_SAVE_SLOT_STRIDE + 2560] = 0xFF;
    }

    memcpy(record + FD2_SAVE_CTX_OFF, g_battle_ctx, 2211);
    memcpy(record + FD2_SAVE_ROSTER_OFF, g_roster_table,
           FD2_ROSTER_MAX * FD2_ENT_REC_SIZE);
    if (g_ent_count)
        memcpy(record + FD2_SAVE_ENT_OFF, g_ent_table,
               (size_t)g_ent_count * FD2_ENT_REC_SIZE);
    memcpy(record + FD2_SAVE_SPAWN_OFF, g_spawner_flags, sizeof(g_spawner_flags));
    record[FD2_SAVE_AUX_OFF] = (uint8_t)g_turn_count;
    record[FD2_SAVE_COUNT_OFF] = (uint8_t)g_ent_count;
    record[FD2_SAVE_STATE_OFF] = (uint8_t)g_state;
    record[FD2_SAVE_SCROLL_OFF] = (uint8_t)g_scroll_x;
    record[FD2_SAVE_SCROLL_OFF + 1] = (uint8_t)g_scroll_y;
    record[FD2_SAVE_CURSOR_OFF] = (uint8_t)g_cursor_x;
    record[FD2_SAVE_CURSOR_OFF + 1] = (uint8_t)g_cursor_y;
    record[FD2_SAVE_VIEW_OFF] = (uint8_t)g_cursor_view_x;
    record[FD2_SAVE_VIEW_OFF + 1] = (uint8_t)g_cursor_view_y;
    record[FD2_SAVE_ROSTER_COUNT_OFF] = (uint8_t)g_roster_count;
    record[FD2_SAVE_GOLD_OFF] = (uint8_t)g_gold;
    record[FD2_SAVE_GOLD_OFF + 1] = (uint8_t)((uint32_t)g_gold >> 8);
    record[FD2_SAVE_GOLD_OFF + 2] = (uint8_t)((uint32_t)g_gold >> 16);
    record[FD2_SAVE_GOLD_OFF + 3] = (uint8_t)((uint32_t)g_gold >> 24);
    record[FD2_SAVE_FLAG_53AF9_OFF] = g_save_flag_53af9;
    record[FD2_SAVE_FLAG_51AAB_OFF] = g_save_flag_51aab;
    record[FD2_SAVE_MUSIC_GATE_OFF] = g_music_gate;
    record[FD2_SAVE_SFX_GATE_OFF] = g_sfx_gate;

    uint32_t checksum = save_sum(record, sizeof(record));
    record[FD2_SAVE_CKSUM_OFF] = (uint8_t)checksum;
    record[FD2_SAVE_CKSUM_OFF + 1] = (uint8_t)(checksum >> 8);
    record[FD2_SAVE_CKSUM_OFF + 2] = (uint8_t)(checksum >> 16);
    record[FD2_SAVE_CKSUM_OFF + 3] = (uint8_t)(checksum >> 24);
    save_transform(record, sizeof(record));

    fp = fopen(save_file_path(), "wb");
    if (!fp)
        return -1;
    int ok = fwrite(record, 1, sizeof(record), fp) == sizeof(record);
    if (fclose(fp) != 0)
        ok = 0;
    return ok ? 0 : -1;
}
