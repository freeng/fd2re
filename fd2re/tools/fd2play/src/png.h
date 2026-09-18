/* png.h — PNG 解码（fd2dat 导出子集：8bit 灰度/RGB/RGBA、非隔行）
 * 与编码（--shot 输出：RGB、存储式 DEFLATE，无需压缩器） */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   /* w*h*4，灰度/RGB 复制到 RGBA（A=255） */

    uint8_t at(int x, int y, int c) const { return rgba[(size_t(y) * w + x) * 4 + size_t(c)]; }
};

Image png_decode(const std::string& path);
void png_write_rgb(const std::string& path, int w, int h, const uint8_t* rgb);
