/* resource.c — DAT 资源包（逆向自 IDA 0x111BA） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"

int32_t g_last_block_size;

void dat_load_fatal(const char *path, int res_id)
{
    fprintf(stderr, "File not found %s!!! / Out of Memory at Load %s Number:%d!!\n",
            path, path, res_id);
    exit(1);
}

/* ---- 宿主：只读 DAT 驻留镜像（SMARTDRV 等价，2026-09-08）------------
 * 原版每次 dat_load_block 都 fopen/fseek/fread/fclose（0x111BA）、
 * ani_play 逐帧 fread（0x20421）——DOS 上这套序列经磁盘高速缓存命中
 * 即内存拷贝。宿主 Windows 的冷页缓存/杀软开档扫描会把同样的序列偶发
 * 拖到几十~几百 ms（片尾→主菜单过渡窗 3 次开档 + ANI 逐帧 636KB 首播
 * 全冷 = "偶发卡顿"根因，原版反核对确认 fd2re 此前逐帧实现本就忠实，
 * 卡顿为宿主环境差而非重建偏差）。等价修复：只读资源 DAT 首次整读
 * 驻留，后续读取全内存命中——游戏侧调用序列与时序语义不变。游戏会
 * 重写的存档（FD2.SAV）不走此层，保持直读避免陈旧镜像。 */
#define DAT_IMAGE_CACHE_MAX 16
static struct { char *path; uint8_t *image; long size; } s_dat_image[DAT_IMAGE_CACHE_MAX];
static int s_dat_image_n;

const uint8_t *dat_file_image(const char *path, long *out_size)
{
    FILE *fp;
    long size;
    uint8_t *image;
    char *path_copy;

    for (int i = 0; i < s_dat_image_n; i++) {
        if (strcmp(s_dat_image[i].path, path) == 0) {
            *out_size = s_dat_image[i].size;
            return s_dat_image[i].image;
        }
    }
    fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    image = malloc(size ? (size_t)size : 1);
    if (!image || (size && fread(image, 1, (size_t)size, fp) != (size_t)size)) {
        free(image);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    path_copy = malloc(strlen(path) + 1);
    if (!path_copy || s_dat_image_n >= DAT_IMAGE_CACHE_MAX) {
        fprintf(stderr, "dat_file_image: cache slot exhausted (%s)\n", path);
        free(path_copy);
        free(image);
        return NULL;
    }
    strcpy(path_copy, path);
    s_dat_image[s_dat_image_n].path = path_copy;
    s_dat_image[s_dat_image_n].image = image;
    s_dat_image[s_dat_image_n].size = size;
    s_dat_image_n++;
    *out_size = size;
    return image;
}

/* 原型（IDA 0x111BA，已动态取证 8 笔 main 调用）：
 *   v7 = fopen(path, "rb")
 *   dir = malloc(8); fseek(v7, 4*id + 6); fread(dir, 1, 8, v7)
 *   len = dir[1] - dir[0]; start = dir[0]
 *   buf = malloc(len); fseek(v7, start); fread(buf, 1, len, v7); fclose(v7)
 * 宿主差异（2026-09-08）：fopen/fseek/fread 序列 → dat_file_image
 * 驻留镜像的两次 memcpy（SMARTDRV 命中路径等价，见上）；原版对坏
 * 目录/越界由 fread 短读触发共享 fatal，此处显式边界判定后同 fatal。
 */
void *dat_load_block(const char *path, void *old_block, int res_id)
{
    const uint8_t *img, *dir;
    long size;
    uint32_t start, next;
    void    *buf;

    if (old_block)
        free(old_block);

    img = dat_file_image(path, &size);
    if (!img)
        dat_load_fatal(path, res_id);

    if (4L * res_id + 6L + 8 > size)
        dat_load_fatal(path, res_id);
    dir = img + 4L * res_id + 6L;
    start = (uint32_t)dir[0] | ((uint32_t)dir[1] << 8)
          | ((uint32_t)dir[2] << 16) | ((uint32_t)dir[3] << 24);
    next = (uint32_t)dir[4] | ((uint32_t)dir[5] << 8)
         | ((uint32_t)dir[6] << 16) | ((uint32_t)dir[7] << 24);
    g_last_block_size = (int32_t)(next - start);

    if (next < start || next > (uint32_t)size)
        dat_load_fatal(path, res_id);

    buf = malloc(g_last_block_size ? (size_t)g_last_block_size : 1);
    if (!buf)
        dat_load_fatal(path, res_id);
    memcpy(buf, img + start, (size_t)g_last_block_size);
    return buf;
}

/* ---- DAT 帧 RLE 解码（2026-09-05 逆向，docs §6.5） ----
 * 原版三件（0x4EC66/0x4ECBF/0x4EBAB）：字节 <=0xC0 = 字面色号(0..192)；
 * 0xC1..0xFF = 后跟色号字节、重复 (b-0xC0) 次；输出 0 = 透明。 */

struct rle_decoder {
    const uint8_t *src;   /* RLE 流指针 */
    uint8_t color;        /* 当前色号（al） */
    uint8_t repeat;       /* 重复剩余（ah） */
};

static uint8_t rle_pixel_next(struct rle_decoder *d)
{
    if (d->repeat) {
        d->repeat--;
        return d->color;
    }
    uint8_t b = *d->src++;
    if (b <= 0xC0) {
        d->color = b;
        return b;
    }
    d->repeat = (uint8_t)(b - 0xC1);   /* 剩余重复 = 次数-1 */
    d->color = *d->src++;
    return d->color;
}

/* 0x15F0E pkg_frame_blit：帧 = 包偏移 u32 表（pkg[4*frame+6]）→ [w,h] i16
 * + RLE 流；逐像素渲染，非零写目标（0=透明），行步进 pitch。
 * 原版 rle_decoder_init（0x4ECBF）先【保存目标区背景】到工作区 +8、头
 * 写 {w, h, 目标偏移}（2026-09-07 pkg_frame_free 反汇编定案——save/
 * restore 对 = 帧"贴上/擦除"动画机制）；随后 rle_frame_blit 贴帧。
 * 返回 malloc 的工作区指针，失败 NULL。 */
void *pkg_frame_blit(const uint8_t *pkg, uint8_t *dst, int pitch,
                     int x, int y, int frame)
{
    if (!pkg)
        return NULL;
    uint32_t off = *(const uint32_t *)(pkg + 4 * frame + 6);
    const uint8_t *desc = pkg + off;
    int w = *(const int16_t *)desc;
    int h = *(const int16_t *)(desc + 2);

    void *work = malloc((size_t)w * h + 8);
    if (!work)
        return NULL;

    *(int16_t *)work = (int16_t)w;
    *(int16_t *)((uint8_t *)work + 2) = (int16_t)h;
    *(int32_t *)((uint8_t *)work + 4) = (int32_t)((long)y * pitch + x);

    uint8_t *out = dst + (size_t)y * pitch + x;
    uint8_t *save = (uint8_t *)work + 8;
    for (int row = 0; row < h; row++)
        memcpy(save + (size_t)w * row, out + (size_t)pitch * row, (size_t)w);

    struct rle_decoder d = { desc + 4, 0, 0 };
    for (int row = 0; row < h; row++) {
        uint8_t *p = out;
        for (int col = 0; col < w; col++) {
            uint8_t c = rle_pixel_next(&d);
            if (c)
                *p = c;
            p++;
        }
        out += pitch;
    }
    return work;
}

/* 0x15E71 pkg_frame_free(dst, pitch, work)：sub_4EC7C 读工作区头
 * {w, h, offset} → sub_4ECA4 按 pitch 逐行把 blit 时保存的背景写回
 * dst 再 free（2026-09-07 定案更正：此前"仅 free"为误读）。 */
void pkg_frame_free(uint8_t *dst, int pitch, void *work)
{
    uint8_t *w8 = (uint8_t *)work;

    if (!w8)
        return;
    int w = *(int16_t *)w8;
    int h = *(int16_t *)(w8 + 2);
    long off = *(int32_t *)(w8 + 4);
    const uint8_t *src = w8 + 8;
    uint8_t *out = dst + off;

    for (int row = 0; row < h; row++) {
        memcpy(out, src, (size_t)w);
        src += w;
        out += pitch;
    }
    free(work);
}

/* 原版多处（melee_panel_render 0x1E611 双立绘、sub_1F1CC/sub_1F30A
 * 幕帘帧等）blit 后不调 pkg_frame_free——工作区直接泄漏。宿主以普通
 * free 收掉（不恢复背景，与原版视觉一致：帧留在目标面上）。 */
void pkg_frame_discard(void *work)
{
    free(work);
}

/* ---- FDSHAP tile 4 操作解码（2026-09-05，docs §6.7） ----
 * 块结构：u16 w + u16 h + u16 tile数 + u32 偏移表（块内绝对偏移，
 * offs[0]=6+4*cnt）+ 每 tile 压缩流。
 * 控制字节 bit7/bit6 选操作、低 6 位 count-1：
 *   00=RLE run(色在后) 01=隔点写(色在后,每2px写1) 10=字面 11=透明跳。 */
int tile4_decode(const uint8_t *stream, size_t len, int w, int h,
                 uint8_t *out /* w*h，透明=0 */)
{
    size_t i = 0;
    int px = 0, total = w * h;
    while (px < total) {
        if (i >= len) return -1;
        uint8_t c = stream[i++];
        int n = (c & 0x3F) + 1;
        switch ((c >> 6) & 3) {
        case 0:
            if (i >= len) return -1;
            for (int k = 0; k < n && px < total; k++) out[px++] = stream[i];
            i++;
            break;
        case 1:
            if (i >= len) return -1;
            for (int k = 0; k < n && px + 1 < total; k++) {
                out[px + 1] = stream[i];
                px += 2;
            }
            i++;
            break;
        case 2:
            for (int k = 0; k < n && px < total && i < len; k++)
                out[px++] = stream[i++];
            break;
        default:
            px += n;                       /* 透明：out 已零初始化 */
            break;
        }
    }
    return (int)i;
}
