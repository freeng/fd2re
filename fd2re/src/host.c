/* host.c — SDL2 宿主层（窗口/纹理/输入桥） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "fd2.h"
#include "host.h"
#include "audio.h"

#if defined(_WIN32)
#include <windows.h>
#endif

enum { HOST_KEY_QUEUE_SIZE = 64 };

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static uint32_t s_pixels[FD2_VRAM_SIZE];
static int s_keys[HOST_KEY_QUEUE_SIZE];
static unsigned s_key_head;
static unsigned s_key_tail;

/* VGA 扫描模拟：原版 CPU 永远直接写 0xA0000，显示硬件并不等待一个
 * “present”函数。宿主在主线程事件泵轮询索引面/DAC，在检测到变化时刷新
 * SDL 纹理；这样所有渲染器都只需写 vram_base()，且不跨线程调用 SDL。 */
static const uint8_t *s_scan_indices;
static const uint8_t *s_scan_dac;
static uint8_t s_previous_indices[FD2_VRAM_SIZE];
static uint8_t s_previous_dac[256 * 3];
static int s_have_previous;
/* SDL must be serviced from one thread.  Several BIOS-style polling helpers
 * call host_event_pump indirectly (kbd_key_avail -> input_wait_key), so keep
 * the primitive non-reentrant at the bottom of the host layer. */
static int s_pump_active;
static uint32_t s_last_present_ms;

static int key_to_scan(SDL_Keycode key)
{
    switch (key) {
    case SDLK_UP: return SCAN_UP;
    case SDLK_DOWN: return SCAN_DOWN;
    case SDLK_LEFT: return SCAN_LEFT;
    case SDLK_RIGHT: return SCAN_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: return SCAN_ENTER;
    case SDLK_ESCAPE: return SCAN_ESC;
    case SDLK_DELETE: return SCAN_DEL;
    case SDLK_SPACE: return 0x39;
    case SDLK_COMMA: return 44;
    case SDLK_l: return 76;
    default: return 0;
    }
}

static void queue_scan(int scan)
{
    unsigned next;
    if (!scan)
        return;
    next = (s_key_tail + 1u) % HOST_KEY_QUEUE_SIZE;
    if (next == s_key_head)
        return;
    s_keys[s_key_tail] = scan;
    s_key_tail = next;
}

static void host_video_render(const uint8_t *indices, const uint8_t *dac_rgb);

void host_event_pump(void)
{
    SDL_Event event;

    if (s_pump_active)
        return;
    s_pump_active = 1;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            /* 关闭窗口即终止进程：此前折成 SCAN_ESC 交给游戏消费，
             * 菜单/场景把 ESC 当导航键吞掉，进程永远退不出
             * （2026-09-09 回归）。音频收尾由 main 注册的
             * atexit(audio_shutdown) 幂等完成。 */
            exit(0);
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym >= SDLK_F1
                && event.key.keysym.sym <= SDLK_F12) {
                /* 裸 F1..F10 = 0x3B..0x44、F11 = 0x57、F12 = 0x58（101 键
                 * 扫描码）。神秘店键 cfg[2] 值域 0x54..0x70 恰为 84 键 AT
                 * 键盘的 BIOS 组合扫描码（int 16h AH 表：Shift+F1..F10 =
                 * 0x54..0x5D、Ctrl+F1..F10 = 0x5E..0x67、Alt+F1..F10 =
                 * 0x68..0x71；30 态 cfg[2] 全部落格，如 st3=0x6A=Alt+F3
                 * 出口位、st2=0x5F=Ctrl+F2）。误判其为"无键可按的 dev
                 * 键"曾致 st4/16/25（0x57/0x58 恰为 F11/F12）之外全部
                 * 章节神秘店不可达。2026-09-12 报障复核改按真实 BIOS
                 * 组合码表还原（Alt+F3 出口位定案）；组合只活在宿主层，
                 * 场景引擎仍是单键比较。Alt+F4(0x6B)无章节使用，不与
                 * 窗口关闭热键冲突。 */
                int n = (int)(event.key.keysym.sym - SDLK_F1) + 1;
                int ctrl = (event.key.keysym.mod & KMOD_CTRL) != 0;
                int shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                int alt = (event.key.keysym.mod & KMOD_ALT) != 0;

                if (alt && n <= 10)
                    queue_scan(0x67 + n);   /* Alt+F1..F10 = 0x68..0x71 */
                else if (ctrl && n <= 10)
                    queue_scan(0x5D + n);   /* Ctrl+F1..F10 = 0x5E..0x67 */
                else if (shift && n <= 10)
                    queue_scan(0x53 + n);   /* Shift+F1..F10 = 0x54..0x5D */
                else if (n <= 10)
                    queue_scan(0x3A + n);   /* F1..F10 = 0x3B..0x44 */
                else
                    queue_scan(0x4C + n);   /* F11 = 0x57、F12 = 0x58 */
                continue;
            }
            /* OS 自动重复（event.key.repeat）等价原版 BIOS typematic 回填
             * 0x41A 环：游戏按住连击正是消费环内重复条目实现的
             * （2026-09-06 DOSBox-X agent 注入实验定案），不可过滤。 */
            queue_scan(key_to_scan(event.key.keysym.sym));
        }
    }

    /* The pure-static browser build has no pthread sequencer. Native builds
     * use a real audio thread, so this is a no-op outside Emscripten. */
    audio_pump();

    /* SDL owns the window on the game thread. Pumping events is also the safe
     * point to emulate VGA scanout and update the host texture. */
    if (s_scan_indices && s_scan_dac && s_renderer && s_texture) {
        /* SDL_RenderPresent is a relatively expensive synchronous operation.
         * The DOS VGA card scans continuously, so a host scanout only needs
         * a bounded cadence; importantly, leave the snapshots untouched when
         * throttled so the newest VRAM state is rendered on the next pump. */
        uint32_t now = SDL_GetTicks();
        if (!s_have_previous || (uint32_t)(now - s_last_present_ms) >= 8u) {
            int changed = !s_have_previous;
            if (!changed && memcmp(s_previous_indices, s_scan_indices,
                                   FD2_VRAM_SIZE) != 0)
                changed = 1;
            if (!changed && memcmp(s_previous_dac, s_scan_dac,
                                   sizeof(s_previous_dac)) != 0)
                changed = 1;
            if (!changed)
                goto pump_done;
            memcpy(s_previous_indices, s_scan_indices, FD2_VRAM_SIZE);
            memcpy(s_previous_dac, s_scan_dac, sizeof(s_previous_dac));
            host_video_render(s_previous_indices, s_previous_dac);
            s_have_previous = 1;
            s_last_present_ms = now;
        }
    }
pump_done:
    s_pump_active = 0;
}

void host_idle_pump(void)
{
    host_event_pump();
    SDL_Delay(1);
}

static void host_video_render(const uint8_t *indices, const uint8_t *dac_rgb)
{
    int output_w, output_h, scale, draw_w, draw_h;
    SDL_Rect dst;
    if (!s_texture || !indices || !dac_rgb)
        return;
    for (int i = 0; i < FD2_VRAM_SIZE; i++) {
        unsigned idx = indices[i];
        uint8_t r = dac_rgb[3 * idx + 0];
        uint8_t g = dac_rgb[3 * idx + 1];
        uint8_t b = dac_rgb[3 * idx + 2];
        s_pixels[i] = 0xFF000000u
                    | ((uint32_t)((r << 2) | (r >> 4)) << 16)
                    | ((uint32_t)((g << 2) | (g >> 4)) << 8)
                    | (uint32_t)((b << 2) | (b >> 4));
    }
    SDL_UpdateTexture(s_texture, NULL, s_pixels, FD2_SCREEN_W * (int)sizeof(s_pixels[0]));
    SDL_GetRendererOutputSize(s_renderer, &output_w, &output_h);
    scale = output_w / FD2_SCREEN_W;
    if (output_h / FD2_SCREEN_H < scale)
        scale = output_h / FD2_SCREEN_H;
    if (scale < 1)
        scale = 1;
    draw_w = FD2_SCREEN_W * scale;
    draw_h = FD2_SCREEN_H * scale;
    dst.x = (output_w - draw_w) / 2;
    dst.y = (output_h - draw_h) / 2;
    dst.w = draw_w;
    dst.h = draw_h;
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, &dst);
    SDL_RenderPresent(s_renderer);
}


int host_video_init(const uint8_t *indices, const uint8_t *dac_rgb)
{
    if (s_window)
        return 1;
#if defined(_WIN32)
    /* 控制台子系统程序从终端/资源管理器启动时，控制台（Windows
     * Terminal 新标签或 conhost 窗口）会参与前台竞争，偶发把键盘
     * 焦点留在终端——开场 Intro 无法被按键跳过、主菜单要点击窗口
     * 才响应（2026-09-09 焦点回归）。游戏窗口自身即完整 UI：创建
     * 窗口前脱离控制台，整类竞争随之消除。错误路径的 stderr 输出
     * 自此丢弃（fatal 路径本就随即 exit）。 */
    FreeConsole();
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 0;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    s_window = SDL_CreateWindow("FD2 Rebuild", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, 960, 600,
                                SDL_WINDOW_RESIZABLE);
    if (!s_window)
        goto fail;
    SDL_SetWindowMinimumSize(s_window, FD2_SCREEN_W, FD2_SCREEN_H);
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer)
        goto fail;
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  FD2_SCREEN_W, FD2_SCREEN_H);
    if (!s_texture)
        goto fail;
    SDL_RaiseWindow(s_window);   /* 创建即置前（配合 FreeConsole 消竞争） */
    s_scan_indices = indices;
    s_scan_dac = dac_rgb;
    s_have_previous = 0;
    s_pump_active = 0;
    s_last_present_ms = 0;
    return 1;

fail:
    fprintf(stderr, "[fd2re] SDL video setup failed: %s\n", SDL_GetError());
    host_video_shutdown();
    return 0;
}

void host_video_shutdown(void)
{
    SDL_DestroyTexture(s_texture);
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;
    s_scan_indices = NULL;
    s_scan_dac = NULL;
    s_pump_active = 0;
    s_last_present_ms = 0;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void host_kbd_ring_flush(void)
{
    host_event_pump();
    s_key_head = s_key_tail;
}

int host_kbd_key_avail(void)
{
    host_event_pump();
    return s_key_head != s_key_tail;
}

int host_kbd_get_scan(void)
{
    int scan;
    host_event_pump();
    if (s_key_head == s_key_tail)
        return 0;
    scan = s_keys[s_key_head];
    s_key_head = (s_key_head + 1u) % HOST_KEY_QUEUE_SIZE;
    return scan;
}
