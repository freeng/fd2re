/* util.h — fd2play 公共小件（文件/十六进制/路径/报错） */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

[[noreturn]] void die(const std::string& msg);

std::vector<uint8_t> read_file(const std::string& path);
std::string read_text_file(const std::string& path);
bool file_exists(const std::string& path);

std::vector<uint8_t> hex_decode(const std::string& hex);

/* “b000” 风格块名 */
std::string block_name(int block);

std::string path_join(const std::string& a, const std::string& b);
std::string fmt(const char* f, ...);

/* UTF-8 控制台输出（Windows） */
void console_init(void);
