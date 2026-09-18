/* json.h — fd2play 最小 JSON DOM（现代格式目录全部 JSON 均为
 * 对象/数组/字符串/数字子集，FDTXT 千串量级无性能压力） */
#pragma once

#include <string>
#include <utility>
#include <vector>

struct JValue {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* get(const char* key) const;   /* 对象成员，缺省 nullptr */
    const JValue* at(size_t i) const;           /* 数组项，越界 nullptr */
    bool has(const char* key) const { return get(key) != nullptr; }
    long as_int(long def = 0) const;
    double as_num(double def = 0) const;
    const std::string& as_str() const;          /* 缺省空串 */
    size_t size() const { return type == ARR ? arr.size() : type == OBJ ? obj.size() : 0; }
};

/* 解析失败抛 std::runtime_error */
JValue json_parse(const std::string& text);
