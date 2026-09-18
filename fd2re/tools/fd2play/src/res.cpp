/* res.cpp — 现代格式目录加载器实现 */
#include "res.h"

#include <cstdio>
#include <cstring>

#include "util.h"

namespace {

long jint(const JValue* v, const char* key, long def = 0)
{
    const JValue* f = v ? v->get(key) : nullptr;
    return f ? f->as_int(def) : def;
}

JValue load_json(const std::string& path)
{
    if (!file_exists(path))
        die("missing " + path + "（先运行 dat_export.py 导出）");
    return json_parse(read_text_file(path));
}

/* 索引真值图路径：image 族指向观看图（真值 = 追加 .idx.png），
 * tiles 族直接指向 .idx.png —— 与 dat_preview.atlas_truth_path 同规则 */
std::string truth_path(const std::string& dir, const JValue& atlas)
{
    std::string image = atlas.get("image")->as_str();
    std::string p = path_join(dir, image);
    std::string base = image;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    if (base.size() > 8 && base.substr(base.size() - 8) == ".idx.png")
        return p;
    return p + ".idx.png";
}

Box box_of(const JValue& v)
{
    Box b{0, 0, 0, 0};
    if (v.type != JValue::ARR || v.size() < 4)
        die("bad atlas box");
    b.x = v.at(0)->as_int();
    b.y = v.at(1)->as_int();
    b.w = v.at(2)->as_int();
    b.h = v.at(3)->as_int();
    return b;
}

} // namespace

FramePack load_frame_pack(const std::string& dir, const std::string& name)
{
    std::string jp = path_join(dir, name + ".json");
    JValue doc = load_json(jp);
    FramePack pack;
    pack.name = name;
    pack.kind = doc.get("kind") ? doc.get("kind")->as_str() : "?";

    if (const JValue* fc = doc.get("frame_count"))
        pack.frame_count = int(fc->as_int());
    else if (const JValue* fr = doc.get("frames"))
        pack.frame_count = int(fr->size());

    const JValue* head = doc.get("head_hex");
    if (head && head->type == JValue::STR && head->str.size() >= 10) {
        /* wh_anim 头：[u8 count][u8 flag][u16 played][u32 param]，
         * head_hex = 字节 1..8 → played = head_hex[2:6] */
        std::vector<uint8_t> hb = hex_decode(head->str);
        if (hb.size() >= 6)
            pack.played = hb[2] | (hb[3] << 8);
    }

    const JValue* frames = doc.get("frames");
    const JValue* atlas = doc.get("atlas");
    if (frames) {
        for (size_t i = 0; i < frames->size(); i++) {
            const JValue* f = frames->at(i);
            if (!f)
                continue;
            FrameMeta m;
            m.index = int(jint(f, "index", long(i)));
            m.x = int(jint(f, "x"));
            m.y = int(jint(f, "y"));
            m.w = int(jint(f, "w"));
            m.h = int(jint(f, "h"));
            m.mode = int(jint(f, "mode"));
            m.atlas_index = int(jint(f, "atlas_index", -1));
            m.icon = int(jint(f, "icon", -1));
            m.phase = int(jint(f, "phase", -1));
            if (const JValue* th = f->get("tail_hex"); th && th->type == JValue::STR) {
                std::vector<uint8_t> tb = hex_decode(th->str);
                /* 帧字节 4..8：[4]=挂号 [5]=音效 [6]=延时 tick [7]=mode [8]=挂号 */
                if (tb.size() >= 5) {
                    m.sfx = tb[1];
                    m.delay_ticks = tb[2];
                }
            }
            pack.frames.push_back(m);
        }
    }
    if (atlas) {
        pack.atlas.img = png_decode(truth_path(dir, *atlas));
        if (const JValue* boxes = atlas->get("boxes"))
            for (size_t i = 0; i < boxes->size(); i++)
                pack.atlas.boxes.push_back(box_of(*boxes->at(i)));
    }
    return pack;
}

Palette768 load_palette(const std::string& fdother_dir, int block)
{
    std::string jp = path_join(fdother_dir, block_name(block) + ".palette.json");
    JValue doc = load_json(jp);
    const JValue* pal = doc.get("palette");
    if (!pal || pal->size() != 256)
        die(jp + ": palette must hold 256 entries");
    Palette768 out;
    for (size_t i = 0; i < 256; i++) {
        const JValue* c = pal->at(i);
        if (!c || c->size() < 3)
            die(jp + ": bad palette entry");
        out.v[3 * i + 0] = uint8_t(c->at(0)->as_int());
        out.v[3 * i + 1] = uint8_t(c->at(1)->as_int());
        out.v[3 * i + 2] = uint8_t(c->at(2)->as_int());
    }
    return out;
}

AniDoc load_ani(const std::string& ani_dir, int anim)
{
    std::string jp = path_join(ani_dir, block_name(anim) + ".frames.json");
    JValue doc = load_json(jp);
    AniDoc out;
    out.frame_count = int(doc.get("frame_count")->as_int());
    const JValue* frames = doc.get("frames");
    for (size_t i = 0; i < frames->size(); i++) {
        const JValue* f = frames->at(i);
        AniFrame fr;
        fr.size = int(f->get("size")->as_int());
        fr.ops = int(f->get("ops")->as_int());
        fr.payload = hex_decode(f->get("payload_hex")->as_str());
        out.frames.push_back(std::move(fr));
    }
    return out;
}

FontAtlas load_font(const std::string& fdother_dir)
{
    JValue doc = load_json(path_join(fdother_dir, "b004.json"));
    FontAtlas font;
    font.cols = int(doc.get("cols")->as_int());
    font.count = int(doc.get("glyph_count")->as_int());
    font.img = png_decode(path_join(fdother_dir, "b004.idx.png"));
    return font;
}

SfxPack load_sfx(const std::string& fdother_dir, int block)
{
    std::string name = block_name(block);
    JValue doc = load_json(path_join(fdother_dir, name + ".json"));
    if (doc.get("kind")->as_str() != "sfx_pkg")
        die(name + " 不是 sfx_pkg 块");
    SfxPack pack;
    pack.name = name;
    pack.rate = int(doc.get("rate")->as_int());
    int entries = int(doc.get("entries")->as_int());
    for (int i = 0; i < entries; i++) {
        std::string wav = path_join(fdother_dir, fmt("%s.e%02d.wav", name.c_str(), i));
        std::vector<uint8_t> file = read_file(wav);
        /* RIFF: "RIFF" len "WAVE" "fmt " ... "data" len pcm(8bit unsigned) */
        const uint8_t* p = file.data();
        size_t n = file.size();
        size_t pos = 12;
        const uint8_t* data = nullptr;
        size_t data_len = 0;
        int bits = 0, channels = 0;
        while (pos + 8 <= n) {
            uint32_t clen = uint32_t(p[pos + 4]) | (uint32_t(p[pos + 5]) << 8) |
                            (uint32_t(p[pos + 6]) << 16) | (uint32_t(p[pos + 7]) << 24);
            if (std::memcmp(p + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= n) {
                channels = p[pos + 10] | (p[pos + 11] << 8);
                bits = p[pos + 22] | (p[pos + 23] << 8);
            } else if (std::memcmp(p + pos, "data", 4) == 0) {
                data = p + pos + 8;
                data_len = clen;
                break;
            }
            pos += 8 + clen + (clen & 1);
        }
        if (!data || bits != 8 || channels != 1)
            die(wav + ": 不是 8bit 单声道 PCM");
        pack.pcm.emplace_back(data, data + data_len);
    }
    return pack;
}

void blit_indexed(uint8_t* dst, int dst_w, int dst_h,
                  const Atlas& atlas, int atlas_index, int x, int y)
{
    if (atlas_index < 0 || atlas_index >= int(atlas.boxes.size()))
        die("atlas index out of range");
    const Box& b = atlas.boxes[size_t(atlas_index)];
    for (int yy = 0; yy < b.h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= dst_h)
            continue;
        for (int xx = 0; xx < b.w; xx++) {
            int dx = x + xx;
            if (dx < 0 || dx >= dst_w)
                continue;
            size_t si = (size_t(b.y + yy) * atlas.img.w + size_t(b.x + xx)) * 4;
            if (atlas.img.rgba[si + 3] == 0)
                continue;                       /* 流级透明 */
            dst[size_t(dy) * dst_w + dx] = atlas.img.rgba[si];
        }
    }
}
