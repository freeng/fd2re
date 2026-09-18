/* json.cpp — 递归下降 JSON 解析器（严格子集：无注释/尾逗号） */
#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace {

struct Parser {
    const char* p;
    const char* end;

    explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    [[noreturn]] void fail(const char* what)
    {
        throw std::runtime_error(std::string("json: ") + what);
    }

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }

    char peek()
    {
        if (p >= end)
            fail("unexpected end of input");
        return *p;
    }

    void expect(char c)
    {
        if (p >= end || *p != c)
            fail("unexpected character");
        p++;
    }

    void append_utf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) {
            s.push_back(char(cp));
        } else if (cp < 0x800) {
            s.push_back(char(0xC0 | (cp >> 6)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(char(0xE0 | (cp >> 12)));
            s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    unsigned hex4()
    {
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            char c = peek();
            p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    std::string string()
    {
        expect('"');
        std::string s;
        for (;;) {
            if (p >= end)
                fail("unterminated string");
            char c = *p++;
            if (c == '"')
                break;
            if (c == '\\') {
                char e = peek();
                p++;
                switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4();
                    /* 代理对（数据里不出现，兜底 U+FFFD） */
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6
                        && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            cp = 0xFFFD;
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    append_utf8(s, cp);
                    break;
                }
                default: fail("bad escape");
                }
            } else {
                s.push_back(c);
            }
        }
        return s;
    }

    JValue value()
    {
        ws();
        char c = peek();
        JValue v;
        if (c == '{') {
            p++;
            v.type = JValue::OBJ;
            ws();
            if (peek() == '}') { p++; return v; }
            for (;;) {
                ws();
                std::string key = string();
                ws();
                expect(':');
                v.obj.emplace_back(std::move(key), value());
                ws();
                char d = peek();
                p++;
                if (d == '}')
                    break;
                if (d != ',')
                    fail("expected , or }");
            }
        } else if (c == '[') {
            p++;
            v.type = JValue::ARR;
            ws();
            if (peek() == ']') { p++; return v; }
            for (;;) {
                v.arr.push_back(value());
                ws();
                char d = peek();
                p++;
                if (d == ']')
                    break;
                if (d != ',')
                    fail("expected , or ]");
            }
        } else if (c == '"') {
            v.type = JValue::STR;
            v.str = string();
        } else if (c == 't') {
            v.type = JValue::BOOL; v.b = true;
            p += 4;
        } else if (c == 'f') {
            v.type = JValue::BOOL; v.b = false;
            p += 5;
        } else if (c == 'n') {
            v.type = JValue::NUL;
            p += 4;
        } else {
            /* 数字 */
            const char* start = p;
            if (p < end && (*p == '-' || *p == '+'))
                p++;
            bool ok = false;
            while (p < end && (std::isdigit(unsigned char(*p)) || *p == '.' ||
                               *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) {
                if (std::isdigit(unsigned char(*p)))
                    ok = true;
                p++;
            }
            if (!ok)
                fail("bad number");
            v.type = JValue::NUM;
            v.num = std::strtod(std::string(start, size_t(p - start)).c_str(), nullptr);
        }
        return v;
    }
};

} // namespace

const JValue* JValue::get(const char* key) const
{
    if (type != OBJ)
        return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

const JValue* JValue::at(size_t i) const
{
    if (type != ARR || i >= arr.size())
        return nullptr;
    return &arr[i];
}

long JValue::as_int(long def) const
{
    if (type == NUM)
        return (long)llround(num);
    if (type == BOOL)
        return b ? 1 : 0;
    return def;
}

double JValue::as_num(double def) const
{
    return type == NUM ? num : def;
}

const std::string& JValue::as_str() const
{
    static const std::string empty;
    return type == STR ? str : empty;
}

JValue json_parse(const std::string& text)
{
    Parser parser(text);
    JValue v = parser.value();
    parser.ws();
    if (parser.p != parser.end)
        parser.fail("trailing garbage");
    return v;
}
