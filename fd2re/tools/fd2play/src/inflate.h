/* inflate.h — RFC1951 DEFLATE 解压（puff 式，仅解压；PNG IDAT 用） */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/* 失败抛 std::runtime_error */
void inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
