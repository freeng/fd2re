/* scenes.h — 各资源场景入口 + ANI 操作码重放器（ani/intro 共享） */
#pragma once

#include <string>
#include <vector>

#include "audio.h"
#include "res.h"
#include "video.h"

struct Options {
    std::string root = "dat-modern";
    int scale = 3;
    bool headless = false;
    long frames = -1;
    std::string shot;
    int pal = 0;
    std::string bank;
    int loop = 1;
    int entries = 3;
    int delay = -1;
    int fg = 205, shadow = 76, bg = 74;
};

/* ---- ANI 操作码重放（fd2re/src/duel.c ani_play/ani_decode_ops 移植） ---- */
struct AniReplayer {
    App& app;
    Audio* audio = nullptr;
    uint8_t frame_buf[App::W * App::H];   /* 逐帧复用的 64000B 装载缓冲 */
    uint8_t pal_work[768];                /* 持久 palette work surface */

    explicit AniReplayer(App& a) : app(a)
    {
        std::memset(frame_buf, 0, sizeof(frame_buf));
        std::memset(pal_work, 0, sizeof(pal_work));
    }

    /* 播放整段：每帧解码上屏 + delay；anim1_frame0_sfx = ANI1 首帧音效
     * （原版 ani_play(anim_id==1 && frame==0) 分支）。返回是否完整播完。 */
    bool play(const AniDoc& doc, int frame_delay_ms, bool skippable,
              bool anim1_frame0_sfx, int sfx_id = -1);

private:
    void decode_ops(int count);
};

/* ---- 场景入口（返回进程退出码） ---- */
int run_ani(App& app, Audio& audio, const Options& opt, int anim);
int run_intro(App& app, Audio& audio, const Options& opt);
int run_figani(App& app, Audio& audio, const Options& opt, int block);
int run_icons(App& app, Audio& audio, const Options& opt, int icon);
int run_portrait(App& app, Audio& audio, const Options& opt, int block);
int run_image(App& app, Audio& audio, const Options& opt,
              const std::string& dir, const std::string& name);
int run_map(App& app, Audio& audio, const Options& opt, int state);
int run_text(App& app, Audio& audio, const Options& opt, int block,
             std::vector<int> ids);
int run_music(App& app, Audio& audio, const Options& opt, int track);
int run_sfx(App& app, Audio& audio, const Options& opt, int block);
