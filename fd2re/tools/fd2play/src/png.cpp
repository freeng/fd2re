/* png.cpp — PNG 子集编解码。解码侧覆盖 fd2dat 全部导出形态：
 * idx.png / 观看图 = RGBA(6)、ANI 帧 = RGB(2)、字库 = 灰度(0)；
 * PIL 默认非隔行、单 IDAT 或多 IDAT 均处理，CRC 逐块校验。 */
#include "png.h"

#include <cstring>
#include <stdexcept>

#include "inflate.h"
#include "util.h"

namespace {

const uint8_t PNG_SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

uint32_t crc_table[256];
bool crc_table_ready = false;

void make_crc_table()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = true;
}

uint32_t crc32_buf(const uint8_t* d, size_t n)
{
    if (!crc_table_ready)
        make_crc_table();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t adler32_buf(const uint8_t* d, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

void write_be32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void write_chunk(std::vector<uint8_t>& v, const char* type, const uint8_t* d, size_t n)
{
    write_be32(v, uint32_t(n));
    size_t crc_begin = v.size();
    v.insert(v.end(), type, type + 4);
    if (n)
        v.insert(v.end(), d, d + n);
    write_be32(v, crc32_buf(v.data() + crc_begin, v.size() - crc_begin));
}

} // namespace

Image png_decode(const std::string& path)
{
    std::vector<uint8_t> file = read_file(path);
    if (file.size() < 8 || std::memcmp(file.data(), PNG_SIG, 8) != 0)
        die(path + ": not a PNG");

    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat;
    bool seen_ihdr = false;

    size_t pos = 8;
    while (pos + 12 <= file.size()) {
        uint32_t len = be32(&file[pos]);
        const uint8_t* type = &file[pos + 4];
        if (pos + 12 + size_t(len) > file.size())
            die(path + ": truncated chunk");
        const uint8_t* data = &file[pos + 8];
        uint32_t want_crc = be32(&file[pos + 8 + len]);
        uint32_t crc = crc32_buf(&file[pos + 4], 4 + len);
        if (crc != want_crc)
            die(path + ": chunk CRC mismatch");
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13)
                die(path + ": bad IHDR");
            w = int(be32(data));
            h = int(be32(data + 4));
            depth = data[8];
            color = data[9];
            if (data[10] != 0 || data[11] != 0)
                die(path + ": unsupported compression/filter method");
            interlace = data[12];
            seen_ihdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        /* 其余辅助块（fd2dat 导出不产生，遇到即跳过） */
        pos += 12 + len;
    }

    if (!seen_ihdr)
        die(path + ": missing IHDR");
    if (interlace != 0)
        die(path + ": interlaced PNG not supported");
    if (depth != 8)
        die(path + ": only 8-bit depth supported (PIL exports)");
    if (color != 0 && color != 2 && color != 6)
        die(path + ": unsupported color type");

    const int channels = color == 0 ? 1 : color == 2 ? 3 : 4;
    if (w <= 0 || h <= 0 || size_t(w) * size_t(h) > (size_t(1) << 28))
        die(path + ": bad dimensions");

    std::vector<uint8_t> raw;
    inflate(idat.data(), idat.size(), raw);
    const size_t stride = size_t(w) * channels;
    if (raw.size() < (stride + 1) * size_t(h))
        die(path + ": IDAT too short");

    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(size_t(w) * h * 4, 0);

    std::vector<uint8_t> line(stride), prev(stride, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = &raw[size_t(y) * (stride + 1)];
        uint8_t ft = src[0];
        src++;
        std::memcpy(line.data(), src, stride);
        switch (ft) {
        case 0:
            break;
        case 1:
            for (size_t i = channels; i < stride; i++)
                line[i] = uint8_t(line[i] + line[i - channels]);
            break;
        case 2:
            for (size_t i = 0; i < stride; i++)
                line[i] = uint8_t(line[i] + prev[i]);
            break;
        case 3:
            for (size_t i = 0; i < stride; i++) {
                int left = i >= size_t(channels) ? line[i - channels] : 0;
                line[i] = uint8_t(line[i] + ((left + prev[i]) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < stride; i++) {
                int a = i >= size_t(channels) ? line[i - channels] : 0;
                int b = prev[i];
                int c = i >= size_t(channels) ? prev[i - channels] : 0;
                line[i] = uint8_t(line[i] + paeth(a, b, c));
            }
            break;
        default:
            die(path + ": bad filter type");
        }
        for (int x = 0; x < w; x++) {
            uint8_t* dst = &img.rgba[(size_t(y) * w + x) * 4];
            if (channels == 1) {
                dst[0] = dst[1] = dst[2] = line[size_t(x)];
                dst[3] = 255;
            } else if (channels == 3) {
                dst[0] = line[x * 3];
                dst[1] = line[x * 3 + 1];
                dst[2] = line[x * 3 + 2];
                dst[3] = 255;
            } else {
                std::memcpy(dst, &line[size_t(x) * 4], 4);
            }
        }
        std::memcpy(prev.data(), line.data(), stride);
    }
    return img;
}

void png_write_rgb(const std::string& path, int w, int h, const uint8_t* rgb)
{
    /* 行滤波全 0 + 存储式 DEFLATE 块（≤65535B/块），无需压缩器 */
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    size_t pos = 0;
    do {
        size_t chunk = raw.size() - pos;
        if (chunk > 65535)
            chunk = 65535;
        bool final_block = (pos + chunk == raw.size());
        zlib.push_back(uint8_t(final_block ? 1 : 0));
        zlib.push_back(uint8_t(chunk & 0xFF));
        zlib.push_back(uint8_t(chunk >> 8));
        zlib.push_back(uint8_t(~chunk & 0xFF));
        zlib.push_back(uint8_t((~chunk >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + ptrdiff_t(pos),
                    raw.begin() + ptrdiff_t(pos + chunk));
        pos += chunk;
    } while (pos < raw.size());
    write_be32(zlib, adler32_buf(raw.data(), raw.size()));

    uint8_t ihdr[13];
    auto put_be32 = [](uint8_t* d, uint32_t v) {
        d[0] = uint8_t(v >> 24); d[1] = uint8_t(v >> 16);
        d[2] = uint8_t(v >> 8); d[3] = uint8_t(v);
    };
    put_be32(ihdr + 0, uint32_t(w));
    put_be32(ihdr + 4, uint32_t(h));
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 2;   /* truecolor */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    std::vector<uint8_t> out(PNG_SIG, PNG_SIG + 8);
    write_chunk(out, "IHDR", ihdr, 13);
    write_chunk(out, "IDAT", zlib.data(), zlib.size());
    write_chunk(out, "IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        die("cannot write " + path);
    if (std::fwrite(out.data(), 1, out.size(), f) != out.size())
        die("write failed: " + path);
    std::fclose(f);
}
