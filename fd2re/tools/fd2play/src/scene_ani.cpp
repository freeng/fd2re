/* scene_ani.cpp — ANI.DAT 流式全屏动画：通用操作码重放播放器。
 *
 * 数据源 = dat-modern/ANI/bNNN.frames.json 的 payload_hex（操作码原字节，
 * pack 侧同源无损），消费语义逐指令对齐 fd2re/src/duel.c ani_play@0x20421
 * + ani_decode_ops@0x36FF4：
 *   - 64000B 帧缓冲逐帧复用（短 payload 读上一帧残留）；
 *   - 768B palette work 持久缓冲；帧内任一 palette 操作后按 fd2dat
 *     render() 语义立即上 DAC（动态证据 §13.12：火焰 ANI 局部堆缓冲
 *     直写 DAC）；
 *   - op7/8/9 u16 偏移可越 64000（video.c 注释：实测 anim1 写到 64000），
 *     宿主钳制不写越界。
 * 帧延时 = ani_play 第二参（原版调用点：ANI0/1=15ms、3/4/6/8=90ms、
 * 5/7=50ms），--delay 覆盖。 */
#include <cstdio>
#include <cstring>

#include "scenes.h"
#include "util.h"

static int default_delay(int anim)
{
    switch (anim) {
    case 0: case 1: return 15;
    case 5: case 7: return 50;
    default: return 90;      /* 3/4/6/8 及其余（dat_preview 同默认） */
    }
}

void AniReplayer::decode_ops(int count)
{
    /* 游标在 frame_buf（64000B）内行进；读尽即放弃本帧余下操作
     * （原版越界读 malloc 堆，fd2dat render() 以 IndexError 等价截断） */
    const uint8_t* d = frame_buf;
    size_t cap = sizeof(frame_buf);
    size_t p = 0;
    bool pal_touched = false;

    auto rd_u16 = [&](size_t at) -> int {
        if (at + 1 >= cap) return -1;
        return d[at] | (d[at + 1] << 8);
    };

    for (int op_i = 0; op_i < count; op_i++) {
        if (p >= cap)
            return;
        uint8_t opcode = d[p++];
        switch (opcode) {
        case 0: {                             /* palette fill */
            if (p >= cap) return;
            std::memset(pal_work, d[p++], 768);
            pal_touched = true;
            break;
        }
        case 1: {                             /* palette full */
            if (p + 768 > cap) return;
            std::memcpy(pal_work, d + p, 768);
            p += 768;
            pal_touched = true;
            break;
        }
        case 2: {                             /* palette byte-RLE */
            size_t o = 0;
            while (o < 768) {
                if (p >= cap) return;
                uint8_t tag = d[p++];
                if ((tag & 0xC0) == 0xC0) {
                    size_t run = tag & 0x3F;
                    if (p >= cap) return;
                    std::memset(pal_work + o, d[p++], run);
                    o += run;
                } else {
                    pal_work[o++] = tag;
                }
            }
            pal_touched = true;
            break;
        }
        case 3: {                             /* palette sparse entries */
            if (p >= cap) return;
            size_t records = d[p++];
            while (records--) {
                if (p + 2 > cap) return;
                size_t entry = d[p], size = d[p + 1];
                p += 2;
                if (p + size > cap) return;
                if (3 * entry + size <= 768)
                    std::memcpy(pal_work + 3 * entry, d + p, size);
                p += size;
            }
            pal_touched = true;
            break;
        }
        case 4: {                             /* VRAM fill */
            if (p >= cap) return;
            std::memset(app.px, d[p++], sizeof(app.px));
            break;
        }
        case 5: {                             /* VRAM full（短拷贝留旧帧） */
            size_t n = cap - p;
            std::memcpy(app.px, d + p, n);
            p = cap;
            break;
        }
        case 6: {                             /* VRAM byte-RLE */
            size_t o = 0;
            while (o < sizeof(app.px)) {
                if (p >= cap) return;
                uint8_t tag = d[p++];
                if ((tag & 0xC0) == 0xC0) {
                    size_t run = tag & 0x3F;
                    if (p >= cap) return;
                    std::memset(app.px + o, d[p++], run);
                    o += run;
                } else {
                    app.px[o++] = tag;
                }
            }
            break;
        }
        case 7: {                             /* sparse pixel */
            int records = rd_u16(p);
            if (records < 0) return;
            p += 2;
            while (records--) {
                int off = rd_u16(p);
                if (off < 0) return;
                p += 2;
                if (p >= cap) return;
                if (off < int(sizeof(app.px)))
                    app.px[off] = d[p];
                p++;
            }
            break;
        }
        case 8: {                             /* sparse run */
            int records = rd_u16(p);
            if (records < 0) return;
            p += 2;
            while (records--) {
                int off = rd_u16(p);
                if (off < 0) return;
                p += 2;
                if (p >= cap) return;
                uint8_t size = d[p++];
                if (p >= cap) return;
                if (off < int(sizeof(app.px))) {
                    size_t n = size;
                    if (off + long(n) > long(sizeof(app.px)))
                        n = size_t(long(sizeof(app.px)) - off);
                    std::memset(app.px + off, d[p], n);
                }
                p++;
            }
            break;
        }
        case 9: {                             /* sparse literal */
            int records = rd_u16(p);
            if (records < 0) return;
            p += 2;
            while (records--) {
                int off = rd_u16(p);
                if (off < 0) return;
                p += 2;
                if (p >= cap) return;
                uint8_t size = d[p++];
                if (p + size > cap) return;
                if (off < int(sizeof(app.px))) {
                    size_t n = size;
                    if (off + long(n) > long(sizeof(app.px)))
                        n = size_t(long(sizeof(app.px)) - off);
                    std::memcpy(app.px + off, d + p, n);
                }
                p += size;
            }
            break;
        }
        default:
            return;                           /* 未知操作码：停（原版 switch 无 default） */
        }
    }
    if (pal_touched) {
        std::memcpy(app.dac, pal_work, 768);
        app.present_soft();
    }
}

bool AniReplayer::play(const AniDoc& doc, int frame_delay_ms, bool skippable,
                       bool anim1_frame0_sfx, int sfx_id)
{
    app.clear_keys();
    for (size_t frame = 0; frame < doc.frames.size(); frame++) {
        const AniFrame& fr = doc.frames[frame];
        /* 原版宿主护栏：u16 size > 64000 或数据越界提前止 */
        if (fr.size > int(sizeof(frame_buf)))
            break;
        std::memcpy(frame_buf, fr.payload.data(),
                    fr.payload.size() <= sizeof(frame_buf)
                        ? fr.payload.size() : sizeof(frame_buf));
        if (anim1_frame0_sfx && frame == 0 && audio && sfx_id >= 0)
            audio->sfx_play(sfx_id);
        decode_ops(fr.ops);
        app.present_frame();
        app.delay_ms(frame_delay_ms);
        if (app.quit)
            return false;
        if (skippable && app.escape) {
            app.clear_keys();
            return false;
        }
    }
    return !app.quit;
}

int run_ani(App& app, Audio& audio, const Options& opt, int anim)
{
    AniDoc doc = load_ani(path_join(opt.root, "ANI"), anim);
    int delay = opt.delay >= 0 ? opt.delay : default_delay(anim);

    std::printf("ANI b%03d: %d 帧，%dms/帧（op 重放，payload 原字节）\n",
                anim, doc.frame_count, delay);

    /* 初始 DAC：--pal 装调色板（intro 路径在调用方安装），否则黑 */
    if (opt.pal >= 0) {
        Palette768 pal = load_palette(path_join(opt.root, "FDOTHER"), opt.pal);
        app.palette_set_master(pal.v);
        app.palette_apply(0, 255, 0);
    }
    std::memset(app.px, 0, sizeof(app.px));

    int sfx_id = -1;
    if (anim == 1) {
        SfxPack pkg = load_sfx(path_join(opt.root, "FDOTHER"), 78);
        if (!pkg.pcm.empty())
            sfx_id = audio.wav_add(pkg.pcm[0], pkg.rate);
    }

    AniReplayer replayer(app);
    replayer.audio = &audio;
    bool finished = replayer.play(doc, delay, true, anim == 1, sfx_id);

    /* 非截图模式且未跳过：末帧停留等 ESC */
    if (!opt.headless && finished && !app.quit) {
        while (!app.escape && !app.quit)
            app.delay_ms(30);
    }
    audio.sfx_stop_all();
    return 0;
}
