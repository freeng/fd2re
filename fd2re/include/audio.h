/* audio.h — 游戏侧音乐/音效（原版为 Miles Sound System 3.02 静态链接）
 *
 * 原版布局（IDA 地址）：
 *   AIL 内核 0x36xxx–0x45xxx；API 名由二进制内 AIL 调试跟踪字符串
 *   （0x50313+）逐一证实。游戏包装层仅三个函数：
 *     music_play  0x25977   sfx_play 0x25A96   sfx_play_b 0x25B45
 *   初始化/关停在 main：AIL_startup(0x37D3E) → AIL_install_MDI_INI(0x3AA72)
 *   → AIL_allocate_sequence_handle(0x3ACA3) → AIL_install_DIG_INI(0x3908B)
 *   → AIL_allocate_sample_handle(0x392D0)×2；退出 AIL_shutdown(0x37ED8)。
 *
 * 宿主后端（本层之下，audio.c 内部）：DOS 硬件不再存在，MIDI 走
 * winmm midiOut（GM 合成），数字样本走 SDL2 音频回调混音——保留游戏层
 * 契约与控制流，替换 AIL 驱动层。已实证的原版参数：
 *   - DIG 样本：8-bit unsigned PCM 单声道，默认播放率 11025Hz
 *     （AIL_init_sample@0x39521 内部 0x414E0 写 [sample+0x3C]=11025），
 *     sfx_play 全程不设播放率 → 固定 11025。
 *   - MDI 序列：FDMUS.DAT 块为 XMIDI（FORM XDIRINFO + CAT XMID →
 *     FORM XMID{TIMB,EVNT}）。EVNT 事实编码（15 块全量自洽验证）：
 *     delta 为单字节 0..127（0 即省略）；每事件显式 status；note-off 仅
 *     1 个数据字节（velocity 隐 0）；时基 120 PPQN。事件时间轴恒定：
 *     AIL 泵 0x43270 按 MDI_SERVICE_RATE（pref[10]=120）120Hz 运行，
 *     每 100 tempo_pct 单位（初值 100）出 1 tick → 恒 1 tick = 1/120 s；
 *     FF 51 只写拍号计数时钟（0x434F5 [seq+6Ch]=16*tempo），FD2 转档
 *     未烘 tempo，原版即恒 60 BPM 拍基准播放。FD2 曲目不用 XMIDI 循环
 *     控制器（CC110/111 全量扫描为 0），循环全靠
 *     AIL_set_sequence_loop_count。
 *   - 序列音量：0x42980 内 CC7 乘 sequence_volume/127 缩放；宿主以
 *     设备音量 ramp 近似整条序列的音量包络。
 */
#ifndef FD2_AUDIO_H
#define FD2_AUDIO_H

#include "fd2.h"

/* ---- 句柄/全局（对应 dseg02，语义 = 原版 AIL 句柄） ---- */
extern void *g_mdi_driver;        /* 0x53ED8 非 NULL = MIDI 后端可用 */
extern void *g_music_seq;         /* 0x53ED0 序列句柄（宿主=内部序列器） */
extern void *g_dig_driver;        /* 0x53EDC 非 NULL = 数字后端可用 */
extern void *g_sfx_sample_a;      /* 0x53EE4 数字样本通道 A */
extern void *g_sfx_sample_b;      /* 0x53EE8 数字样本通道 B */
extern void *g_music_data;        /* 0x53EE0 当前 FDMUS.DAT 块 */
extern uint8_t g_music_ok;        /* 0x53EF0 */
extern uint8_t g_dig_ok;          /* 0x53EF1 */
extern uint8_t g_music_track_cur; /* 0x51A11 */
extern uint8_t g_music_gate;      /* 0x51E61，战斗存档 +12499 */
extern uint8_t g_sfx_gate;        /* 0x51E62，战斗存档 +12500 */
extern uint8_t g_sfx_duel_hold;   /* 0x54133：决斗场景装载/演出期抑制普通
                                   * 音效（duel_scene_load/duel_exchange/
                                   * ending_sequence_play 置位；音频侧按
                                   * 原版判定，置位方在 battle/duel 模块） */

/* ---- 后端生命周期（main 对应段） ---- */

/* 0x25C01..0x25C5E：AIL_startup + install_MDI/DIG_INI + 句柄分配。
 * 任一后端打开失败时对应 *_ok 保持 0，游戏层自动走“不可用”路径。 */
void audio_init(void);

/* Native builds use the SDL sequencer thread. The single-threaded browser
 * advances the same sequence from host_event_pump instead. */
void audio_pump(void);

/* 0x37ED8 AIL_shutdown：停序列、静音、关设备。退出游戏前调用。 */
void audio_shutdown(void);

/* ---- 游戏包装层（控制流已对齐原版） ---- */

/* 0x25977 music_play(track, loop)：
 *  track == -1 → AIL_set_sequence_volume(seq, 0, 4000)（4 秒淡出）
 *  否则（g_music_ok 时）：旧块非空先 AIL_stop_sequence(0x3AF5B)，
 *  g_music_data = dat_load_block("FDMUS.DAT", old, track)
 *  → DPMI 锁页(0x3666C，宿主 no-op) → assign/start/音量/loop。
 *  音量：g_music_gate 关 = 0；开且 track∈{16,17} = 127 立即；
 *  其余 0 → 127 淡入 2000ms。loop_count 0 = 无限循环。
 *  track 0/2/5/7/9 为 3 字节占位块（" \r\n"），assign 失败即静默。
 */
void music_play(int track, int loop);

/* 0x17380 AIL_set_sequence_volume(g_music_seq, vol, ms)：设置菜单音乐
 * 开关切换即时应用（0x1728C：on→127 / off→0，ramp 1000ms）。 */
void music_seq_set_volume(int vol, int ms);

/* 0x25A96 sfx_play(pkg, frame_idx, loop)：
 *  g_dig_ok 且 g_sfx_gate 且 !g_sfx_duel_hold 时：
 *  stop 样本 → idx==-1 到此为止 → init → addr = pkg+u32[pkg+6+4*idx]，
 *  len = u32[idx+1]-u32[idx] → loop_count → start。
 *  第三参进 0x39AAE = AIL_set_sample_loop_count（0=无限，N=共播 N 遍），
 *  不是音量——FD2 从不调真正的 AIL_set_sample_volume(0x399C2)，音量
 *  恒为 AIL 默认偏好 prefs[5]=100（AIL_startup→0x3FCA9→0x54320，
 *  init_sample 0x414E0 拷入 [s+0x40]，重算器 0x40240 建
 *  (b-128)*2*(vol+1) 查找表，驱动 >>8 出 8-bit DMA）。 */
void sfx_play(const void *pkg, int frame_idx, int loop);

/* 0x25B45 sfx_play_b：同 sfx_play，走第二数字样本通道
 * g_sfx_sample_b@0x53EE8（fx 控制器受击音效双通道交叠）。 */
void sfx_play_b(const void *pkg, int frame_idx, int loop);

#endif
