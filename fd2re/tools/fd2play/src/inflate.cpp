/* inflate.cpp — puff 式 DEFLATE（stored / fixed / dynamic Huffman）。
 * 实现参照 zlib puff.c 的规范化解码算法，按 C++ 边界安全重写。 */
#include "inflate.h"

#include <stdexcept>
#include <string>

namespace {

struct BitReader {
    const uint8_t* d;
    size_t n;
    size_t bytepos = 0;
    int bitpos = 0;

    BitReader(const uint8_t* data, size_t size) : d(data), n(size) {}

    int bit()
    {
        if (bytepos >= n)
            throw std::runtime_error("inflate: out of input");
        int v = (d[bytepos] >> bitpos) & 1;
        if (++bitpos == 8) {
            bitpos = 0;
            bytepos++;
        }
        return v;
    }

    int bits(int need)
    {
        int v = 0;
        for (int i = 0; i < need; i++)
            v |= bit() << i;
        return v;
    }

    void align_byte() { bitpos = 0; }
};

struct Huffman {
    /* 规范码：count[len] = 该长度的码数，symbol[] 按码序排列 */
    short count[16] = {0};
    std::vector<short> symbol;

    void build(const uint8_t* lengths, int n);
    int decode(BitReader& br) const;
};

void Huffman::build(const uint8_t* lengths, int n)
{
    for (int i = 0; i < 16; i++)
        count[i] = 0;
    for (int i = 0; i < n; i++)
        count[lengths[i]]++;
    if (count[0] == n)
        return;                         /* 全空码表（0 距离表合法） */
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0)
            throw std::runtime_error("inflate: over-subscribed huffman code");
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = short(offs[len] + count[len]);
    symbol.assign(size_t(n), 0);
    for (int i = 0; i < n; i++)
        if (lengths[i] != 0)
            symbol[size_t(offs[lengths[i]]++)] = short(i);
}

int Huffman::decode(BitReader& br) const
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= br.bit();
        int cnt = count[len];
        if (code - cnt < first)
            return symbol[size_t(index + (code - first))];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    throw std::runtime_error("inflate: invalid huffman code");
}

const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                               51,59,67,83,99,115,131,163,195,227,258};
const uint8_t LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,
                               5,5,5,5,0};
const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,
                                385,513,769,1025,1537,2049,3073,4097,6145,8193,
                                12289,16385,24577};
const uint8_t DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,
                                10,11,11,12,12,13,13};
const uint8_t CLEN_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

void codes(BitReader& br, const Huffman& lencode, const Huffman& distcode,
           std::vector<uint8_t>& out)
{
    for (;;) {
        int sym = lencode.decode(br);
        if (sym < 0 || sym > 285)
            throw std::runtime_error("inflate: bad literal/length symbol");
        if (sym < 256) {
            out.push_back(uint8_t(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            size_t len = LEN_BASE[sym] + size_t(br.bits(LEN_EXTRA[sym]));
            int dsym = distcode.decode(br);
            if (dsym < 0 || dsym > 29)
                throw std::runtime_error("inflate: bad distance symbol");
            size_t dist = DIST_BASE[dsym] + size_t(br.bits(DIST_EXTRA[dsym]));
            if (dist > out.size())
                throw std::runtime_error("inflate: distance too far back");
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; i++)
                out.push_back(out[from + i]);
        }
    }
}

void fixed_tables(Huffman& lencode, Huffman& distcode)
{
    uint8_t lengths[288];
    int i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    lencode.build(lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    distcode.build(lengths, 30);
}

void dynamic_tables(BitReader& br, Huffman& lencode, Huffman& distcode)
{
    uint8_t lengths[288 + 32];
    int nlen = br.bits(5) + 257;
    int ndist = br.bits(5) + 1;
    int ncode = br.bits(4) + 4;
    if (nlen > 288 || ndist > 30)
        throw std::runtime_error("inflate: bad counts");
    uint8_t clen[19] = {0};
    for (int i = 0; i < ncode; i++)
        clen[CLEN_ORDER[i]] = uint8_t(br.bits(3));
    Huffman clcode;
    clcode.build(clen, 19);
    int index = 0;
    while (index < nlen + ndist) {
        int sym = clcode.decode(br);
        if (sym < 0)
            throw std::runtime_error("inflate: bad code length symbol");
        if (sym < 16) {
            lengths[index++] = uint8_t(sym);
        } else {
            int len = 0;
            if (sym == 16) {
                if (index == 0)
                    throw std::runtime_error("inflate: repeat with no prev");
                len = lengths[index - 1];
                sym = br.bits(2) + 3;
            } else if (sym == 17) {
                sym = br.bits(3) + 3;
            } else {
                sym = br.bits(7) + 11;
            }
            if (index + sym > nlen + ndist)
                throw std::runtime_error("inflate: too many lengths");
            while (sym--)
                lengths[index++] = uint8_t(len);
        }
    }
    if (lengths[256] == 0)
        throw std::runtime_error("inflate: no end-of-block code");
    lencode.build(lengths, nlen);
    distcode.build(lengths + nlen, ndist);
}

} // namespace

void inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    BitReader br(data, size);
    int last = 0;
    while (!last) {
        last = br.bit();
        int type = br.bits(2);
        if (type == 0) {
            br.align_byte();
            if (br.bytepos + 4 > br.n)
                throw std::runtime_error("inflate: stored header out of input");
            size_t len = size_t(data[br.bytepos] | (data[br.bytepos + 1] << 8));
            size_t nlen = size_t(data[br.bytepos + 2] | (data[br.bytepos + 3] << 8));
            if ((len ^ 0xFFFF) != nlen)
                throw std::runtime_error("inflate: stored length check");
            br.bytepos += 4;
            if (br.bytepos + len > br.n)
                throw std::runtime_error("inflate: stored data out of input");
            out.insert(out.end(), data + br.bytepos, data + br.bytepos + len);
            br.bytepos += len;
        } else if (type == 1) {
            Huffman lencode, distcode;
            fixed_tables(lencode, distcode);
            codes(br, lencode, distcode, out);
        } else if (type == 2) {
            Huffman lencode, distcode;
            dynamic_tables(br, lencode, distcode);
            codes(br, lencode, distcode, out);
        } else {
            throw std::runtime_error("inflate: invalid block type");
        }
    }
}
