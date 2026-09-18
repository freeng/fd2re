/* video.cpp — SDL2 窗口呈现 + mode 13h 调色板模型 */
#include "video.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "util.h"

namespace {

uint8_t scale6(int v)
{
    int out = v * 255 / 63;
    return out > 255 ? 255 : uint8_t(out);
}

} // namespace

bool App::init(int scale, bool headless, const std::string& title)
{
    scale_ = scale;
    headless_ = headless;
    std::memset(px, 0, sizeof(px));
    std::memset(dac, 0, sizeof(dac));

    if (headless_)
        return true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "fd2play: SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    win_ = SDL_CreateWindow(title.c_str(),
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            W * scale_, H * scale_,
                            SDL_WINDOW_RESIZABLE);
    if (!win_) {
        std::fprintf(stderr, "fd2play: SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED);
    if (!ren_) {
        std::fprintf(stderr, "fd2play: SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* 整数放大保锯齿 */
    SDL_RenderSetLogicalSize(ren_, W, H);
    return true;
}

void App::shutdown()
{
    if (tex_)
        SDL_DestroyTexture(tex_);
    if (ren_)
        SDL_DestroyRenderer(ren_);
    if (win_)
        SDL_DestroyWindow(win_);
    if (!headless_)
        SDL_Quit();
}

void App::palette_set_master(const uint8_t* p768)
{
    master_pal = p768;
}

/* IDA 0x11D40 —— DAC = 主调色板逐通道减 dim，下限 0 */
void App::palette_apply(int first, int last, int dim)
{
    if (!master_pal)
        return;
    for (int i = first; i <= last; i++) {
        int r = master_pal[3 * i + 0] - dim; if (r < 0) r = 0;
        int g = master_pal[3 * i + 1] - dim; if (g < 0) g = 0;
        int b = master_pal[3 * i + 2] - dim; if (b < 0) b = 0;
        dac[3 * i + 0] = uint8_t(r);
        dac[3 * i + 1] = uint8_t(g);
        dac[3 * i + 2] = uint8_t(b);
    }
    present_soft();
}

/* IDA 0x11DF2 —— DAC = 主调色板逐通道加 add，上限 63 */
void App::palette_add(int first, int last, int add)
{
    if (!master_pal)
        return;
    for (int i = first; i <= last; i++) {
        int r = master_pal[3 * i + 0] + add; if (r > 63) r = 63; if (r < 0) r = 0;
        int g = master_pal[3 * i + 1] + add; if (g > 63) g = 63; if (g < 0) g = 0;
        int b = master_pal[3 * i + 2] + add; if (b > 63) b = 63; if (b < 0) b = 0;
        dac[3 * i + 0] = uint8_t(r);
        dac[3 * i + 1] = uint8_t(g);
        dac[3 * i + 2] = uint8_t(b);
    }
    present_soft();
}

/* IDA 0x2DF01 —— out = c + scale*(master[i]-c)/40（c=r,g,b 目标色） */
void App::palette_mix(int start, int end, int scale, int r, int g, int b)
{
    if (!master_pal)
        return;
    for (int i = start; i < end; i++) {
        dac[3 * i + 0] = uint8_t(r + scale * (master_pal[3 * i + 0] - r) / 40);
        dac[3 * i + 1] = uint8_t(g + scale * (master_pal[3 * i + 1] - g) / 40);
        dac[3 * i + 2] = uint8_t(b + scale * (master_pal[3 * i + 2] - b) / 40);
    }
    present_soft();
}

/* IDA 0x1F882 —— 64 步 dim 0..63，每步 delay(2) */
void App::palette_fade_black()
{
    for (int i = 0; i < 64; i++) {
        palette_apply(0, 255, i);
        delay_ms(2);
    }
}

/* IDA 0x1F525 —— 64 步 dim 64..0，每步 delay(2) */
void App::fade_in()
{
    for (int dim = 64; dim >= 0; dim--) {
        palette_apply(0, 255, dim);
        delay_ms(2);
    }
}

void App::delay_ms(int ms)
{
    if (headless_ || ms <= 0) {
        pump();
        return;
    }
    uint32_t t0 = SDL_GetTicks();
    for (;;) {
        pump();
        uint32_t now = SDL_GetTicks();
        if (now - t0 >= uint32_t(ms))
            break;
        uint32_t left = uint32_t(ms) - (now - t0);
        SDL_Delay(left > 5 ? 5 : left);
    }
}

void App::wait_ticks(int ticks)
{
    /* BIOS 0x1C tick = 65536/0x1234DD s ≈ 54.925ms */
    delay_ms(int(ticks * 54925L / 1000L));
}

void App::pump()
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            quit = true;
        } else if (ev.type == SDL_KEYDOWN) {
            int k = int(ev.key.keysym.sym);
            if (k == SDLK_ESCAPE)
                escape = true;
            keys_.push_back(k);
            if (keys_.size() > 32)
                keys_.pop_front();
        }
    }
}

bool App::take_key(int keycode)
{
    for (auto it = keys_.begin(); it != keys_.end(); ++it) {
        if (*it == keycode) {
            keys_.erase(it);
            return true;
        }
    }
    return false;
}

void App::clear_keys()
{
    keys_.clear();
    escape = false;
}

bool App::any_key()
{
    return !keys_.empty();
}

void App::render()
{
    if (headless_ || !ren_)
        return;
    if (!tex_ || tex_w_ != W || tex_h_ != H) {
        if (tex_)
            SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, W, H);
        tex_w_ = W;
        tex_h_ = H;
    }
    static std::vector<uint32_t> buf;
    buf.assign(size_t(W) * H, 0);
    for (int i = 0; i < W * H; i++) {
        const uint8_t* c = &dac[px[i] * 3];
        buf[size_t(i)] = 0xFF000000u
                         | (uint32_t(scale6(c[0])) << 16)
                         | (uint32_t(scale6(c[1])) << 8)
                         | uint32_t(scale6(c[2]));
    }
    SDL_UpdateTexture(tex_, nullptr, buf.data(), W * 4);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    SDL_RenderPresent(ren_);
}

void App::render_rgba(const Image& img)
{
    if (headless_ || !ren_)
        return;
    if (!tex_ || tex_w_ != img.w || tex_h_ != img.h) {
        if (tex_)
            SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, img.w, img.h);
        tex_w_ = img.w;
        tex_h_ = img.h;
        SDL_RenderSetLogicalSize(ren_, img.w, img.h);
    }
    static std::vector<uint32_t> buf;
    buf.assign(size_t(img.w) * img.h, 0);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
            buf[size_t(y) * img.w + x] = 0xFF000000u
                                         | (uint32_t(p[0]) << 16)
                                         | (uint32_t(p[1]) << 8)
                                         | uint32_t(p[2]);
        }
    SDL_UpdateTexture(tex_, nullptr, buf.data(), img.w * 4);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    SDL_RenderPresent(ren_);
}

void App::write_shot_rgb(int w, int h, const uint8_t* rgb)
{
    png_write_rgb(shot_path_, w, h, rgb);
    std::printf("shot: %s (%dx%d, frame %ld)\n", shot_path_.c_str(), w, h,
                frame_counter_);
}

void App::maybe_shot()
{
    if (shot_path_.empty() || shot_taken_)
        return;
    if (frame_counter_ != shot_frame_) {
        if (frame_counter_ > shot_frame_) {
            std::fprintf(stderr,
                         "fd2play: --frames %ld 超出场景实际帧边界，写出最终帧\n",
                         shot_frame_);
            shot_frame_ = frame_counter_;
        } else {
            return;
        }
    }
    std::vector<uint8_t> rgb(size_t(W) * H * 3);
    for (int i = 0; i < W * H; i++) {
        const uint8_t* c = &dac[px[i] * 3];
        rgb[size_t(i) * 3 + 0] = scale6(c[0]);
        rgb[size_t(i) * 3 + 1] = scale6(c[1]);
        rgb[size_t(i) * 3 + 2] = scale6(c[2]);
    }
    write_shot_rgb(W, H, rgb.data());
    shot_taken_ = true;
    std::exit(0);
}

void App::present_soft()
{
    if (!headless_) {
        pump();
        render();
    }
}

void App::present_frame()
{
    frame_counter_++;
    if (headless_) {
        maybe_shot();
        return;
    }
    pump();
    render();
}

void App::present_rgba(const Image& img)
{
    if (headless_) {
        /* 任意尺寸场景：直接写真值截图（RGB） */
        std::vector<uint8_t> rgb(size_t(img.w) * img.h * 3);
        for (int y = 0; y < img.h; y++)
            for (int x = 0; x < img.w; x++) {
                const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
                rgb[(size_t(y) * img.w + x) * 3 + 0] = p[0];
                rgb[(size_t(y) * img.w + x) * 3 + 1] = p[1];
                rgb[(size_t(y) * img.w + x) * 3 + 2] = p[2];
            }
        write_shot_rgb(img.w, img.h, rgb.data());
        std::exit(0);
    }
    pump();
    render_rgba(img);
}

void App::set_shot(const std::string& path, long frame)
{
    shot_path_ = path;
    shot_frame_ = frame;
}
