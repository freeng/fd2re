/* res.h — fd2dat 现代格式目录（dat-modern/）直接消费加载器。
 * 只读 JSON/PNG/WAV/MID，不触碰任何原版 DAT。语义依据：
 * tools/README-dat-modern.md §二/§四 + fd2dat/*.py 导出器逐字段对照。 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.h"
#include "png.h"

struct Box { int x, y, w, h; };

struct Atlas {
    Image img;
    std::vector<Box> boxes;
};

/* wh_anim / dato / sprite24_pkg / image / image_pkg 子块 通用帧包 */
struct FrameMeta {
    int index = 0;
    int x = 0, y = 0;          /* wh_anim 帧头 i16 x/y 定位 */
    int w = 0, h = 0;
    int mode = 0;              /* 帧字节 7 */
    int sfx = 0;               /* 帧字节 5（unit_anim_play 证据） */
    int delay_ticks = 0;       /* 帧字节 6，BIOS tick（18.2065Hz） */
    int atlas_index = -1;
    int icon = -1, phase = -1; /* sprite24_pkg */
};

struct FramePack {
    std::string kind;
    std::string name;
    int frame_count = 0;
    int played = 0;            /* wh_anim 头 u16（经典族 = count） */
    std::vector<FrameMeta> frames;
    Atlas atlas;
};

/* <dir>/<name>.json + 索引真值图集（真值路径规则与 dat_preview 一致） */
FramePack load_frame_pack(const std::string& dir, const std::string& name);

/* FDOTHER bNNN.palette.json → 768B 6bit VGA */
struct Palette768 {
    uint8_t v[768];
};
Palette768 load_palette(const std::string& fdother_dir, int block);

/* ANI bNNN.frames.json → 逐帧操作码负载（ops 原字节） */
struct AniFrame {
    int size = 0;
    int ops = 0;
    std::vector<uint8_t> payload;
};
struct AniDoc {
    int frame_count = 0;
    std::vector<AniFrame> frames;
};
AniDoc load_ani(const std::string& ani_dir, int anim);

/* FDOTHER b004 字库（16×16 点阵图集，cols 列） */
struct FontAtlas {
    Image img;
    int cols = 48;
    int count = 0;
};
FontAtlas load_font(const std::string& fdother_dir);

/* FDOTHER bNNN sfx 包条目 wav 装载（11025Hz 8bit unsigned 单声道） */
struct SfxPack {
    std::string name;
    int rate = 11025;
    std::vector<std::vector<uint8_t>> pcm;   /* 每条目原始采样 */
};
SfxPack load_sfx(const std::string& fdother_dir, int block);

/* 帧图集贴到索引画布（A=0 跳过 = 流级透明；越界裁剪） */
void blit_indexed(uint8_t* dst, int dst_w, int dst_h,
                  const Atlas& atlas, int atlas_index, int x, int y);
