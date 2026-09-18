/* audio.c — 游戏侧音乐/音效（控制流逆向自 IDA 0x25977/0x25A96/0x25B45）
 *
 * 宿主后端（替换原版 Miles Sound System 3.02 驱动层，契约不变）：
 *   MDI → libADLMIDI（Nuked OPL3）：FDMUS.DAT 块为 XMIDI，解析成绝对时间
 *         事件表，序列器线程按 µs 调度发实时事件。音色 = 游戏自带
 *         SAMPLE.OPL（AIL 运行时库，SBPRO2.MDI 描述符扩展名 "OPL" 定案）
 *         经 tools/opl2wopl.py 转成 FD2SAMPLE.wopl（字段映射以 libADLMIDI
 *         自带 MonopolyDeluxe.ad/.wopl 原生对逐字节对拍定案，volume model
 *         = AIL）。打击乐 ch9：AIL 语义为音符号选音色（SAMPLE.OPL 目录
 *         (note,0x7F) 条目），事件层把 note-on 改写成 patch+note-on。
 *   DIG → SDL2 音频回调：8-bit unsigned PCM 11025Hz 双通道混音，音乐与
 *         音效同一设备同一回调内混合。
 *
 * XMIDI 事实语法（FD2 15 块全量自洽验证 + AIL 泵 0x43270 反汇编定案）：
 *   - 块 = FORM XDIRINFO + CAT XMID{ FORM XMID{ TIMB, RBRN, EVNT } }。
 *   - EVNT：事件间 delta 为一个 <0x80 字节（0..127 ticks，连续多字节
 *     相加，0 delta 即省略）；每事件显式 status；note-on = status+note+
 *     vel+VLQ(duration)——AIL 在 duration 耗尽时自动发 note-off（0x43270
 *     的 32 槽 note 队列）；meta 标准（FF 51 tempo / FF 2F EOT）。
 *   - 时基 120 PPQN（delta 直方图 15/30 = 1/32 与 1/16 音符格点对位）。
 *     AIL 事件时间轴恒定：序列泵 0x43270 以 MDI_SERVICE_RATE
 *     （pref[10]=120，AIL_set_preference 默认值，游戏从不改）120Hz 运行，
 *     每泵向 [seq+54h] 加 tempo_pct（初值 100 = AIL_set_sequence_tempo
 *     百分比，FD2 不调），满 100 出 1 tick → 恒 1 tick = 1/120 s。
 *     FF 51 在 0x434F5 只写拍号计数时钟 [seq+6Ch]=16*tempo，不进事件
 *     速率；FD2 转档未把 tempo 烘进 delta（每块恰一个 FF 51@tick0），
 *     原版即恒 120 tick/s（60 BPM 拍基准）播放，宿主同样忽略 FF 51。
 *   - TIMB（2026-09-08 更正）：字节对 (编号, 页) 的预装载清单，页 0 =
 *     旋律 patch 号、0x7F = 打击乐 GM 音符号（与 SAMPLE.OPL 目录键一致，
 *     EXE 内嵌默认 TIMB "01 00 FF FF" 同构）。播放映射为恒等：program P
 *     → 旋律 patch P、ch9 音符 N → 打击条目 N——不存在 program 重映射
 *     （旧记"TIMB 把 program 映射到 timbre 号"系把页字节误读为 timbre）。
 *   - FD2 曲目无 XMIDI 循环控制器（CC110+ 全量为零）；循环全靠
 *     loop_count（0 = 无限），EOT 处重启。
 *   - 序列音量 0..127：AIL 在 CC7 上乘 volume/127（0x42980）；宿主同样
 *     以每通道 CC7 缩放实现淡入/淡出（音量表跟踪事件 CC7，ramp 时重发）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "adlmidi.h"

#include "fd2.h"
#include "resource.h"
#include "host.h"
#include "audio.h"

/* ---- 游戏全局（地址注释 = IDA） ---- */
void *g_mdi_driver, *g_music_seq, *g_dig_driver, *g_sfx_sample_a, *g_sfx_sample_b;
void *g_music_data;
uint8_t g_music_ok, g_dig_ok, g_music_track_cur;
uint8_t g_music_gate = 1;
uint8_t g_sfx_gate = 1;
uint8_t g_sfx_duel_hold;             /* 0x54133，置位方在 battle/duel 模块 */

/* =================================================================== */
/* ---- DIG 后端：SDL 音频回调混音 ------------------------------------ */
/* =================================================================== */

/* AIL_init_sample@0x39521 内部 0x414E0：默认播放率 11025Hz、8-bit
 * unsigned、单声道；sfx_play 全程不改播放率。 */
#define SFX_RATE   11025

typedef struct {
    int           active;            /* AIL_start_sample 后置位 */
    const uint8_t *data;             /* 8-bit unsigned PCM */
    uint32_t      len;               /* 样本数 */
    uint64_t      pos;               /* 32.32 定点位置（整数部分 = 样本索引） */
    uint64_t      step;              /* (SFX_RATE<<32)/host_rate */
    int           loop_left;         /* AIL loop_count：0=无限，N=共播 N 遍 */
    int           vol;               /* AIL 音量 0..127（本游戏恒为默认值） */
} sfx_chan;

/* AIL 3.02 默认偏好 sub_3FCA9 → prefs[5]（0x54320）= 100：DIG 样本默认
 * 音量。AIL_init_sample(0x414E0) 把它拷入 [s+0x40]，重算器 0x40240 按
 * table[b] = (b-128)*2*(vol+1)（单声道）建 256 项查找表，驱动侧
 * out = 0x80 + (Σtable >> 8) —— vol=127 恰为满幅。FD2 从不调用真正的
 * AIL_set_sample_volume(0x399C2)，故全部音效恒为该默认音量。 */
#define SFX_AIL_DEFAULT_VOLUME 100

static SDL_AudioDeviceID s_dig_dev;
static int s_dig_rate;
static sfx_chan s_ch[2];             /* [0]=A 0x53EE4 / [1]=B 0x53EE8 */

/* OPL3 合成器实例（MDI 后端）：音频回调 adl_generate 与序列器线程的
 * adl_rt_* 实时事件共享，一切访问持 SDL 音频设备锁。声明前置供回调使用。 */
static struct ADL_MIDIPlayer *s_adl;

static void SDLCALL dig_callback(void *user, Uint8 *stream, int len)
{
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4;            /* s16 双声道 */
    int f, c;

    SDL_memset(stream, 0, (size_t)len);
    if (s_adl)                       /* 音乐先入底（adl_generate 填满整块） */
        adl_generate(s_adl, frames * 2, out);
    for (f = 0; f < frames; f++) {
        int mix = 0;
        for (c = 0; c < 2; c++) {
            sfx_chan *ch = &s_ch[c];
            uint32_t idx, frac;
            int s;
            if (!ch->active)
                continue;
            idx = (uint32_t)(ch->pos >> 32);
            if (idx >= ch->len) {
                /* AIL_set_sample_loop_count 语义：0=无限循环，N=共播 N
                 * 遍（含当前遍）。一遍播完按剩余次数回绕或自然停止。 */
                if (ch->loop_left == 0)
                    ch->pos -= (uint64_t)ch->len << 32;
                else if (--ch->loop_left > 0)
                    ch->pos -= (uint64_t)ch->len << 32;
                else {
                    ch->active = 0;
                    continue;
                }
                idx = (uint32_t)(ch->pos >> 32);
                if (idx >= ch->len) {
                    ch->active = 0;
                    continue;
                }
            }
            frac = (uint32_t)((ch->pos >> 24) & 0xFF);
            if (idx + 1 < ch->len) {
                int a = (int)ch->data[idx] - 128;
                int b = (int)ch->data[idx + 1] - 128;
                s = a + ((b - a) * (int)frac >> 8);
            } else {
                s = (int)ch->data[idx] - 128;
            }
            s <<= 8;                 /* u8 → s16 振幅 */
            /* 原版 0x40240 查找表增益：(b-128)*2*(vol+1)，驱动 >>8 出
             * 8-bit DMA；vol=127 为满幅 unity。 */
            mix += s * 2 * (ch->vol + 1) / 256;
            ch->pos += ch->step;
        }
        /* 音效叠加到音乐上，饱和保护 */
        {
            int l = out[f * 2] + mix, r = out[f * 2 + 1] + mix;
            if (l > 32767)  l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767)  r = 32767;
            if (r < -32768) r = -32768;
            out[f * 2]     = (Sint16)l;
            out[f * 2 + 1] = (Sint16)r;
        }
    }
}

static int dig_open(void)
{
    SDL_AudioSpec want, have;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return 0;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = dig_callback;
    s_dig_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dig_dev == 0)
        return 0;
    s_dig_rate = have.freq;
    SDL_PauseAudioDevice(s_dig_dev, 0);
    return 1;
}

static void dig_close(void)
{
    if (s_dig_dev) {
        SDL_CloseAudioDevice(s_dig_dev);
        s_dig_dev = 0;
    }
}

/* sfx_play 的 AIL 五步（stop → init → address → loop_count → start）在
 * SDL 回调锁内落状态；通道即原版样本句柄。第三参是循环次数而非音量
 * （0x25B2A 调 0x39AAE = AIL_set_sample_loop_count），音量恒为 AIL
 * 默认偏好 100——原版菜单/战斗音效因此都清晰可闻。 */
static void sfx_trigger(sfx_chan *ch, const void *pkg, int frame_idx, int loop)
{
    const uint8_t *p = (const uint8_t *)pkg;
    uint32_t off, off_next;

    if (!s_dig_dev)
        return;
    SDL_LockAudioDevice(s_dig_dev);
    ch->active = 0;                  /* AIL_stop_sample */
    if (frame_idx == -1) {
        SDL_UnlockAudioDevice(s_dig_dev);
        return;
    }
    off      = p[6 + 4 * frame_idx]         | ((uint32_t)p[6 + 4 * frame_idx + 1] << 8)
             | ((uint32_t)p[6 + 4 * frame_idx + 2] << 16) | ((uint32_t)p[6 + 4 * frame_idx + 3] << 24);
    off_next = p[6 + 4 * (frame_idx + 1)]   | ((uint32_t)p[6 + 4 * (frame_idx + 1) + 1] << 8)
             | ((uint32_t)p[6 + 4 * (frame_idx + 1) + 2] << 16) | ((uint32_t)p[6 + 4 * (frame_idx + 1) + 3] << 24);
    ch->data = p + off;              /* AIL_set_sample_address */
    ch->len  = off_next - off;
    ch->pos  = 0;                    /* AIL_init_sample */
    ch->step = ((uint64_t)SFX_RATE << 32) / (uint64_t)s_dig_rate;
    ch->loop_left = loop;            /* AIL_set_sample_loop_count */
    ch->vol  = SFX_AIL_DEFAULT_VOLUME;
    ch->active = 1;                  /* AIL_start_sample */
    SDL_UnlockAudioDevice(s_dig_dev);
}

/* =================================================================== */
/* ---- MDI 后端：XMIDI 解析 + libADLMIDI 序列器 ------------------------ */
/* =================================================================== */

/* AIL 事件时基：泵 120Hz × 每 100 tempo_pct 单位出 1 tick（初值 100）
 * → 恒 1 tick = 1/120 s，与 FF 51 无关（见文件头注释）。 */
#define XMIDI_TICKS_PER_SEC 120

typedef struct {
    uint64_t t_us;                   /* 事件时刻（序列起点为 0） */
    uint32_t order;                  /* 同刻排序（合成 off 先于触发事件） */
    uint8_t  status;                 /* 通道消息 status（<0xF0） */
    uint8_t  d1, d2;
} seq_ev;

typedef struct seq_build {
    seq_ev  *ev;
    int      n, cap;
    int      failed;
} seq_build;

static void seq_push(seq_build *b, uint64_t t_us, uint32_t order,
                     uint8_t status, uint8_t d1, uint8_t d2)
{
    if (b->n == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 512;
        seq_ev *nv = (seq_ev *)realloc(b->ev, (size_t)ncap * sizeof(seq_ev));
        if (!nv) {
            b->failed = 1;
            return;
        }
        b->ev = nv;
        b->cap = ncap;
    }
    b->ev[b->n].t_us = t_us;
    b->ev[b->n].order = order;
    b->ev[b->n].status = status;
    b->ev[b->n].d1 = d1;
    b->ev[b->n].d2 = d2;
    b->n++;
}

static uint32_t ev_vlq(const uint8_t *ev, uint32_t size, uint32_t *pio)
{
    /* 0x424B0 语义：BE-VLQ，(v<<7)|(b&0x7F)，b<0x80 终止，至多 4 字节 */
    uint32_t v = 0, i = *pio, cnt = 0;

    while (i < size && cnt < 4) {
        uint8_t b = ev[i++];
        v = (v << 7) | (b & 0x7F);
        cnt++;
        if (!(b & 0x80))
            break;
    }
    *pio = i;
    return v;
}

/* IFF 遍历：返回 chunk 数据指针/长度（pad 对齐），无匹配返回 0 */
static const uint8_t *iff_find(const uint8_t *buf, uint32_t size,
                               const char *id, uint32_t *out_len)
{
    uint32_t i = 0;

    while (i + 8 <= size) {
        uint32_t ln = ((uint32_t)buf[i + 4] << 24) | ((uint32_t)buf[i + 5] << 16)
                    | ((uint32_t)buf[i + 6] << 8) | buf[i + 7];
        if (memcmp(buf + i, id, 4) == 0) {
            if (i + 8 + ln > size)
                return NULL;
            *out_len = ln;
            return buf + i + 8;
        }
        i += 8 + ((ln + 1) & ~1u);
    }
    return NULL;
}

/* XMIDI → 事件表。成功返回 malloc 的事件数组（调用方 free），失败 NULL。
 * note-off 依 note-on 的 duration 后缀合成（AIL 0x43270 note 队列语义）。 */
static seq_ev *xmidi_parse(const uint8_t *blk, uint32_t size,
                           int *out_n, uint64_t *out_dur_us)
{
    const uint8_t *cat, *seqform, *body, *ev;
    uint32_t catlen, seqlen = 0, bodylen, evlen, evsize;
    seq_build b;
    uint32_t i;
    uint32_t tick = 0;
    uint64_t t_us = 0;
    uint32_t order = 0;

    *out_n = 0;
    *out_dur_us = 0;
    if (size < 16)
        return NULL;

    cat = iff_find(blk, size, "CAT ", &catlen);
    if (!cat || catlen < 8 || memcmp(cat, "XMID", 4) != 0)
        return NULL;

    /* 第一个 FORM XMID 即序列（AIL init_sequence 0x443D0 同语义） */
    {
        uint32_t k = 4;
        seqform = NULL;
        while (k + 8 <= catlen) {
            uint32_t ln = ((uint32_t)cat[k + 4] << 24) | ((uint32_t)cat[k + 5] << 16)
                        | ((uint32_t)cat[k + 6] << 8) | cat[k + 7];
            if (memcmp(cat + k, "FORM", 4) == 0 && k + 8 + ln <= catlen
                && memcmp(cat + k + 8, "XMID", 4) == 0) {
                seqform = cat + k + 8;
                seqlen = ln;
                break;
            }
            k += 8 + ((ln + 1) & ~1u);
        }
        if (!seqform || seqlen < 8)
            return NULL;
        body = seqform + 4;          /* 跳过 'XMID' 类型字 */
        bodylen = seqlen - 4;
    }

    ev = iff_find(body, bodylen, "EVNT", &evlen);
    if (!ev || evlen == 0)
        return NULL;
    evsize = evlen;

    b.ev = NULL;
    b.n = 0;
    b.cap = 0;
    b.failed = 0;

    i = 0;
    while (i < evsize && !b.failed) {
        uint8_t st = ev[i];
        if (st < 0x80) {             /* 事件间 delta：单字节 0..127 ticks */
            tick += st;
            t_us = (uint64_t)tick * 1000000 / XMIDI_TICKS_PER_SEC;
            i++;
            continue;
        }
        i++;
        if (st == 0xFF) {            /* meta */
            uint8_t mt;
            uint32_t ln;
            if (i >= evsize) { b.failed = 1; break; }
            mt = ev[i++];
            ln = ev_vlq(ev, evsize, &i);
            if (i + ln > evsize) { b.failed = 1; break; }
            if (mt == 0x51 && ln == 3) {
                /* AIL 0x434F5：FF 51 只写拍号时钟 [seq+6Ch]=16*tempo，
                 * 不影响事件速率（恒 1 tick/泵）。FD2 转档未烘 tempo，
                 * 原版即忽略——宿主同样只跳过数据。 */
            }
            /* FF 2F EOT：AIL 置 EOT 标志等待 loop；事件流到此为止 */
            i += ln;
        } else if (st == 0xF0 || st == 0xF7) {
            uint32_t ln = ev_vlq(ev, evsize, &i);
            if (i + ln > evsize) { b.failed = 1; break; }
            i += ln;                 /* sysex 跳过（FD2 曲目未出现） */
        } else if ((st & 0xF0) == 0x90) {   /* note-on + duration 后缀 */
            uint8_t note, vel;
            uint32_t dur;
            uint64_t off_us;
            if (i + 2 > evsize) { b.failed = 1; break; }
            note = ev[i];
            vel  = ev[i + 1];
            if (note >= 0x80 || vel >= 0x80) { b.failed = 1; break; }
            seq_push(&b, t_us, order++, st, note, vel);
            i += 2;
            dur = ev_vlq(ev, evsize, &i);
            off_us = (uint64_t)(tick + dur) * 1000000 / XMIDI_TICKS_PER_SEC;
            if (ev[i - 1] & 0x80) {  /* 防御：VLQ 读穿（不应发生） */
                b.failed = 1;
                break;
            }
            /* vel==0 的 note-on 本身即 off，不再合成。note/vel 必须在
             * VLQ 读取前留存：duration>=128 tick 时 VLQ 为多字节，读后
             * 回取 ev[i-3] 拿到的是 vel 而非 note，合成 off 带错音符号，
             * 真正的长音永远等不到 off（2026-09-09 持续音回归根因，
             * FDMUS 15 曲共 886 个 >=128 tick 音符受影响）。 */
            if (vel != 0)
                seq_push(&b, off_us, order, (uint8_t)(0x80 | (st & 0x0F)),
                         note, 0);
            order += 2;
        } else {
            int nd = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            if (i + nd > evsize) { b.failed = 1; break; }
            if (ev[i] >= 0x80 || (nd == 2 && ev[i + 1] >= 0x80)) {
                b.failed = 1;
                break;
            }
            seq_push(&b, t_us, order++, st, ev[i], nd == 2 ? ev[i + 1] : 0);
            i += nd;
        }
    }
    if (b.failed || b.n == 0) {
        free(b.ev);
        return NULL;
    }
    *out_n = b.n;
    *out_dur_us = t_us;
    return b.ev;
}

static int ev_cmp(const void *pa, const void *pb)
{
    const seq_ev *a = (const seq_ev *)pa, *b2 = (const seq_ev *)pb;

    if (a->t_us != b2->t_us)
        return a->t_us < b2->t_us ? -1 : 1;
    return a->order < b2->order ? -1 : (a->order > b2->order ? 1 : 0);
}

/* ---- 序列器线程状态 ---- */
static SDL_Thread *s_seq_thread;
static SDL_atomic_t s_seq_run;
static SDL_mutex *s_seq_lock;

static seq_ev *s_evs;
static int s_n_evs;
static uint64_t s_seq_dur_us;
static int s_seq_playing;
static int s_seq_idx;
static uint64_t s_seq_start_us;      /* 序列起点（宿主时钟） */
static int s_loop_left;              /* 剩余播放次数；<0 = 无限（loop=0） */

static int s_vol_from, s_vol_to, s_vol_cur, s_vol_applied;
static uint64_t s_vol_t0_us;
static int s_vol_ramp_ms;

static uint8_t s_cc7[16];            /* 事件侧 CC7 原始值（AIL 0x42980 在
                                      * 其上乘 volume/127 出有效 CC7） */

static uint64_t s_perf_freq;

static uint64_t now_us(void)
{
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1000000.0
                      / (double)s_perf_freq);
}

/* 持音频设备锁调用：MIDI 通道消息 → libADLMIDI 实时事件。
 * ch9 = AIL 打击乐：音符号即音色号（SAMPLE.OPL (note,0x7F) 条目），
 * note-on 前先 patchChange 把音符换成打击音色再触发。 */
static void midi_send(uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t ch = status & 0x0F;

    switch (status & 0xF0) {
    case 0x90:
        if (d2 == 0) {
            adl_rt_noteOff(s_adl, ch, d1);
            break;
        }
        if (ch == 9)
            adl_rt_patchChange(s_adl, 9, d1);
        adl_rt_noteOn(s_adl, ch, d1, d2);
        break;
    case 0x80:
        adl_rt_noteOff(s_adl, ch, d1);
        break;
    case 0xB0:
        if (d1 == 7) {
            s_cc7[ch] = d2;
            d2 = (uint8_t)((int)d2 * s_vol_cur / 127);
        }
        adl_rt_controllerChange(s_adl, ch, d1, d2);
        break;
    case 0xC0:
        if (ch != 9)                 /* ch9 的 patch 由音符驱动 */
            adl_rt_patchChange(s_adl, ch, d1);
        break;
    case 0xD0:
        adl_rt_channelAfterTouch(s_adl, ch, d1);
        break;
    case 0xE0:
        adl_rt_pitchBendML(s_adl, ch, d2, d1);
        break;
    default:
        break;
    }
}

/* AIL_stop_sequence 0x3AF5B → 0x447D0：sustain off + 全音符 off */
static void midi_all_notes_off(void)
{
    int ch;

    if (!s_adl)
        return;
    for (ch = 0; ch < 16; ch++) {
        midi_send((uint8_t)(0xB0 | ch), 64, 0);
        midi_send((uint8_t)(0xB0 | ch), 120, 0);
        midi_send((uint8_t)(0xB0 | ch), 123, 0);
    }
}

/* 持音频设备锁调用：序列音量变化 → 全通道重算有效 CC7（AIL 0x42980） */
static void midi_apply_volume(int v)
{
    int ch;

    if (!s_adl)
        return;
    for (ch = 0; ch < 16; ch++)
        adl_rt_controllerChange(s_adl, (uint8_t)ch, 7,
                                (uint8_t)((int)s_cc7[ch] * v / 127));
}

/* 持 s_seq_lock 调用；now = 当前宿主时钟 µs。
 * adl_* 实时事件与音频回调的 adl_generate 共享 s_adl，派发段持
 * SDL 音频设备锁（短临界区：µs 级事件批 + 音量重算）。 */
static void seq_pump_locked(uint64_t now)
{
    if (s_vol_ramp_ms > 0) {
        uint64_t el = now - s_vol_t0_us;
        uint64_t span = (uint64_t)s_vol_ramp_ms * 1000;
        int v;
        if (el >= span) {
            s_vol_cur = s_vol_to;
            s_vol_ramp_ms = 0;
        } else {
            v = s_vol_from + (int)((s_vol_to - s_vol_from) * (int64_t)el
                    / (int64_t)span);
            s_vol_cur = v;
        }
    } else {
        s_vol_cur = s_vol_to;
    }
    if (!s_adl || !s_dig_dev)
        return;

    SDL_LockAudioDevice(s_dig_dev);
    if (s_vol_cur != s_vol_applied) {
        s_vol_applied = s_vol_cur;
        midi_apply_volume(s_vol_cur);
    }
    if (s_seq_playing) {
        while (s_seq_idx < s_n_evs && s_evs[s_seq_idx].t_us <= now - s_seq_start_us) {
            midi_send(s_evs[s_seq_idx].status, s_evs[s_seq_idx].d1,
                      s_evs[s_seq_idx].d2);
            s_seq_idx++;
        }
        if (s_seq_idx >= s_n_evs) {      /* EOT：AIL loop_count 语义 */
            if (s_loop_left < 0 || --s_loop_left > 0) {
                s_seq_idx = 0;
                s_seq_start_us = now;
            } else if (s_loop_left == 0) {
                s_seq_playing = 0;
            } else {
                /* 无限循环 */
                s_seq_idx = 0;
                s_seq_start_us = now;
            }
        }
    }
    SDL_UnlockAudioDevice(s_dig_dev);
}

static int seq_thread_fn(void *data)
{
    while (SDL_AtomicGet(&s_seq_run)) {
        SDL_Delay(1);
        SDL_LockMutex(s_seq_lock);
        seq_pump_locked(now_us());
        SDL_UnlockMutex(s_seq_lock);
    }
    return 0;
}

void audio_pump(void)
{
#if defined(__EMSCRIPTEN__)
    /* Asyncify cannot host a second native thread. The browser event turn is
     * the deterministic replacement for the native 1ms sequence thread. */
    if (!s_adl || !s_seq_lock)
        return;
    SDL_LockMutex(s_seq_lock);
    seq_pump_locked(now_us());
    SDL_UnlockMutex(s_seq_lock);
#endif
}

/* MDI 后端初始化：OPL3 合成器（游戏自带 SAMPLE.OPL → FD2SAMPLE.wopl，
 * volume model=AIL）+ 序列器线程。须在 dig_open 之后调用（事件派发
 * 经 DIG 设备锁）。 */
static int midi_open(void)
{
    int ch;

    s_perf_freq = SDL_GetPerformanceFrequency();
    s_adl = adl_init(s_dig_rate);
    if (!s_adl)
        return 0;
    adl_setNumChips(s_adl, 1);       /* 原版单 OPL3（SB16 一枚 YMF262） */
    if (adl_openBankFile(s_adl, "FD2SAMPLE.wopl") < 0) {
        adl_close(s_adl);
        s_adl = NULL;
        return 0;
    }
    adl_setVolumeRangeModel(s_adl, ADLMIDI_VolumeModel_AIL);

    s_seq_lock = SDL_CreateMutex();
    if (!s_seq_lock) {
        adl_close(s_adl);
        s_adl = NULL;
        return 0;
    }
#if !defined(__EMSCRIPTEN__)
    SDL_AtomicSet(&s_seq_run, 1);
    s_seq_thread = SDL_CreateThread(seq_thread_fn, "fd2midi", NULL);
    if (!s_seq_thread) {
        SDL_DestroyMutex(s_seq_lock);
        s_seq_lock = NULL;
        adl_close(s_adl);
        s_adl = NULL;
        return 0;
    }
#endif
    /* AIL 驱动初始化（sub_43AD0）对各通道发 CC7=默认音量偏好（127）；
     * 事件侧 CC7 基线同设 127，序列音量 ramp 在其上缩放。 */
    SDL_LockAudioDevice(s_dig_dev);
    for (ch = 0; ch < 16; ch++) {
        s_cc7[ch] = 127;
        adl_rt_controllerChange(s_adl, (uint8_t)ch, 7, 127);
    }
    SDL_UnlockAudioDevice(s_dig_dev);
    return 1;
}

static void midi_close(void)
{
    if (!s_adl)
        return;
    if (s_seq_thread) {
        SDL_AtomicSet(&s_seq_run, 0);
        SDL_WaitThread(s_seq_thread, NULL);
        s_seq_thread = NULL;
    }
    if (s_dig_dev) {
        SDL_LockAudioDevice(s_dig_dev);
        {
            struct ADL_MIDIPlayer *d = s_adl;
            s_adl = NULL;            /* 回调此后跳过 adl_generate */
            adl_rt_resetState(d);
            adl_close(d);
        }
        SDL_UnlockAudioDevice(s_dig_dev);
    } else {
        adl_close(s_adl);
        s_adl = NULL;
    }
    if (s_seq_lock) {
        SDL_DestroyMutex(s_seq_lock);
        s_seq_lock = NULL;
    }
    free(s_evs);
    s_evs = NULL;
    s_n_evs = 0;
}

/* 持锁的 AIL 等价操作（music_play 控制流用） */
static void seq_stop_locked(void)
{
    s_seq_playing = 0;
    midi_all_notes_off();
}

static void seq_assign_locked(const void *data, uint32_t size)
{
    int n;
    uint64_t dur;
    seq_ev *evs = xmidi_parse((const uint8_t *)data, size, &n, &dur);

    free(s_evs);
    s_evs = evs;
    s_n_evs = evs ? n : 0;
    s_seq_dur_us = dur;
    if (evs)
        qsort(s_evs, (size_t)s_n_evs, sizeof(seq_ev), ev_cmp);
}

static void seq_set_volume_locked(int vol, int ms, uint64_t now)
{
    s_vol_from = s_vol_cur;
    s_vol_to = vol;
    s_vol_ramp_ms = ms;
    s_vol_t0_us = now;
    if (ms <= 0) {
        s_vol_cur = vol;
        s_vol_ramp_ms = 0;
    }
}

/* =================================================================== */
/* ---- 游戏包装层（对齐原版控制流） ----------------------------------- */
/* =================================================================== */

void audio_init(void)
{
    /* 0x25C01 AIL_startup → MDI（音乐）→ DIG（音效），失败即走不可用路径。
     * 宿主约束：音乐与音效共用同一 SDL 音频设备混音，且 MDI 事件派发
     * 要持 DIG 设备锁——故宿主先开 DIG 再开 MDI（g_music_ok/g_dig_ok
     * 语义不变，仅初始化顺序差异）。 */
    if (dig_open()) {
        g_dig_driver = (void *)1;
        g_dig_ok = 1;
        g_sfx_sample_a = &s_ch[0];
        g_sfx_sample_b = &s_ch[1];
    }
    if (midi_open()) {
        g_mdi_driver = (void *)1;
        g_music_ok = 1;
        g_music_seq = (void *)&s_evs;    /* 序列句柄（宿主内部序列器） */
    }
}

void audio_shutdown(void)
{
    /* 0x37ED8 AIL_shutdown */
    midi_close();
    dig_close();
}

void music_play(int track, int loop)
{
    uint64_t now;

    if (g_music_track_cur == (uint8_t)track)
        return;
    g_music_track_cur = (uint8_t)track;

    if (track == -1) {
        if (s_adl) {
            SDL_LockMutex(s_seq_lock);
            now = now_us();
            seq_set_volume_locked(0, 4000, now);   /* 4 秒淡出 */
            seq_pump_locked(now);
            SDL_UnlockMutex(s_seq_lock);
        }
        return;
    }
    if (!g_music_ok)
        return;

    g_music_data = dat_load_block("FDMUS.DAT", g_music_data, track);
    /* 0x3666C：DPMI 锁页（int 31h AX=0600），宿主 no-op */

    if (s_adl) {
        SDL_LockMutex(s_seq_lock);
        now = now_us();
        if (s_evs)                     /* 0x3AF5B AIL_stop_sequence */
            seq_stop_locked();
        seq_assign_locked(g_music_data, (uint32_t)g_last_block_size);
        if (s_n_evs > 0) {             /* AIL_assign/start_sequence */
            s_seq_idx = 0;
            s_seq_start_us = now;
            if (g_music_gate) {
                if (track == 16 || track == 17)
                    seq_set_volume_locked(127, 0, now);
                else {
                    seq_set_volume_locked(0, 0, now);
                    seq_set_volume_locked(127, 2000, now);
                }
            } else {
                seq_set_volume_locked(0, 0, now);
            }
            s_loop_left = loop == 0 ? -1 : loop;   /* 0 = 无限循环 */
            s_seq_playing = 1;
        }
        seq_pump_locked(now);
        SDL_UnlockMutex(s_seq_lock);
    }
}

void music_seq_set_volume(int vol, int ms)
{
    /* 0x17380 AIL_set_sequence_volume(g_music_seq, vol, ms)：options 菜单
     * 音乐开关切换即时应用（0x1737A off→0 / 0x17393 on→0x7F，ramp 1000）。
     * 宿主序列器未启用时无操作（原版 AIL 对空句柄亦安全）。 */
    uint64_t now;

    if (!s_adl)
        return;
    SDL_LockMutex(s_seq_lock);
    now = now_us();
    seq_set_volume_locked(vol, ms, now);
    seq_pump_locked(now);
    SDL_UnlockMutex(s_seq_lock);
}

static void sfx_common(void *handle, const void *pkg, int frame_idx, int loop)
{
    /* 0x25A96/0x25B45 gate：数字后端 + 音效开关 + 决斗装载抑制 */
    if (!g_dig_ok || !g_sfx_gate || g_sfx_duel_hold)
        return;
    if (handle == g_sfx_sample_a)
        sfx_trigger(&s_ch[0], pkg, frame_idx, loop);
    else
        sfx_trigger(&s_ch[1], pkg, frame_idx, loop);
}

void sfx_play(const void *pkg, int frame_idx, int loop)
{
    sfx_common(g_sfx_sample_a, pkg, frame_idx, loop);
}

void sfx_play_b(const void *pkg, int frame_idx, int loop)
{
    sfx_common(g_sfx_sample_b, pkg, frame_idx, loop);
}
