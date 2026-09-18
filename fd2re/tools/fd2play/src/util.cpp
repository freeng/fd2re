/* util.cpp — fd2play 公共小件 */
#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <Windows.h>

void die(const std::string& msg)
{
    std::fprintf(stderr, "fd2play: %s\n", msg.c_str());
    std::exit(2);
}

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        die("cannot open " + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    return data;
}

std::string read_text_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        die("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool file_exists(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

std::vector<uint8_t> hex_decode(const std::string& hex)
{
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0)
            die("bad hex string");
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

std::string block_name(int block)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "b%03d", block);
    return buf;
}

std::string path_join(const std::string& a, const std::string& b)
{
    if (a.empty())
        return b;
    if (a.back() == '/' || a.back() == '\\')
        return a + b;
    return a + "/" + b;
}

std::string fmt(const char* f, ...)
{
    va_list ap;
    va_start(ap, f);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string s(size_t(n < 0 ? 0 : n), '\0');
    if (n > 0)
        std::vsnprintf(&s[0], size_t(n) + 1, f, ap2);
    va_end(ap2);
    return s;
}

/* UTF-8 控制台输出（Git Bash / cmd 双兼容） */
void console_init(void)
{
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}
