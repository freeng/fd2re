/* video.h — mode 13h 模型的宿主呈现：320×200 索引屏 + 6bit DAC 调色板。
 * 调色板操作逐条移植 fd2re/src/video.c（IDA 0x11D40/0x11DF2/0x2DF01/
 * 0x1F882/0x1F525）。--frames/--shot 为无头快照模式：present_frame()
 * 计帧，到指定帧写出 PNG 后退出，供与 dat_preview 产物逐像素对拍。 */
#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "png.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

class App {
public:
    static constexpr int W = 320;
    static constexpr int H = 200;

    bool init(int scale, bool headless, const std::string& title);
    void shutdown();

    /* 索引屏与 DAC（6bit 原值；呈现时 *255/63） */
    uint8_t px[W * H];
    uint8_t dac[768];

    /* g_palette_ptr 等价：渐变函数读主调色板 */
    const uint8_t* master_pal = nullptr;

    void palette_set_master(const uint8_t* p768);   /* 装主调色板 */
    void palette_apply(int first, int last, int dim);
    void palette_add(int first, int last, int add);
    void palette_mix(int start, int end, int scale, int r, int g, int b);
    void palette_fade_black();      /* 64 步 × 2ms（0x1F882） */
    void fade_in();                 /* 64 步 × 2ms（0x1F525） */

    void delay_ms(int ms);          /* 无头模式立即返回（时间冻结） */
    void wait_ticks(int ticks);     /* BIOS tick = 1/18.2065s */
    void present_frame();           /* 帧边界：渲染 + 无头计帧/截图 */
    void present_soft();            /* 中间态渲染（仅窗口模式） */
    void present_rgba(const Image& img);   /* 任意尺寸场景（map） */

    /* 输入（present/delay 内泵事件） */
    bool quit = false;              /* 窗口关闭 / Ctrl+C */
    bool escape = false;            /* ESC：场景内跳过/退出 */
    bool take_key(int keycode);     /* 消费一个按键事件（SDLK_*） */
    void clear_keys();
    bool any_key();

    void set_shot(const std::string& path, long frame);

private:
    void pump();
    void render();
    void render_rgba(const Image& img);
    void maybe_shot();
    void write_shot_rgb(int w, int h, const uint8_t* rgb);

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    int scale_ = 3;
    bool headless_ = false;
    std::deque<int> keys_;
    long frame_counter_ = 0;
    std::string shot_path_;
    long shot_frame_ = -1;
    bool shot_taken_ = false;
};
