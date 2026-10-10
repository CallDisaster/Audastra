#include "MpqDecompress.h"

#include <array>
#include <cstring>

namespace War3Map::Mpq {
namespace {

// LSB-first 位读取器（DEFLATE 位序）。
struct BitReader {
    std::span<const std::byte> in;
    std::size_t pos = 0;
    std::uint32_t buf = 0;
    int cnt = 0;
    bool error = false;

    int GetBits(int need) noexcept {
        std::uint32_t val = buf;
        while (cnt < need) {
            if (pos >= in.size()) {
                error = true;
                return 0;
            }
            val |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos++])) << cnt;
            cnt += 8;
        }
        buf = val >> need;
        cnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    void AlignToByte() noexcept {
        buf = 0;
        cnt = 0;
    }
};

// 规范 Huffman 表（Mark Adler puff 结构：按码长计数 + 排序符号表）。
struct Huffman {
    std::array<int, 16> count{};
    std::vector<int> symbol;

    void Build(const int* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) {
            ++count[lengths[i]];
        }
        count[0] = 0;
        std::array<int, 16> offsets{};
        for (int len = 1; len < 15; ++len) {
            offsets[len + 1] = offsets[len] + count[len];
        }
        symbol.assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbol[static_cast<std::size_t>(offsets[lengths[i]]++)] = i;
            }
        }
    }

    int Decode(BitReader& br) const noexcept {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= br.GetBits(1);
            if (br.error) {
                return -1;
            }
            const int c = count[len];
            if (code - first < c) {
                return symbol[static_cast<std::size_t>(index + (code - first))];
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr std::array<int, 29> kLengthBase{
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistBase{
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<int, 19> kCodeLengthOrder{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// 解码一个已构造好 Huffman 表的压缩块，输出追加到 out。
bool InflateBlock(
    BitReader& br,
    const Huffman& litLen,
    const Huffman& dist,
    std::vector<std::byte>& out) {
    while (true) {
        const int symbol = litLen.Decode(br);
        if (symbol < 0) {
            return false;
        }
        if (symbol == 256) {
            return true; // 块结束
        }
        if (symbol < 256) {
            out.push_back(static_cast<std::byte>(symbol));
            continue;
        }
        // 长度/距离对
        const int lenIdx = symbol - 257;
        if (lenIdx >= static_cast<int>(kLengthBase.size())) {
            return false;
        }
        const int length = kLengthBase[lenIdx] + br.GetBits(kLengthExtra[lenIdx]);
        const int distSym = dist.Decode(br);
        if (distSym < 0 || distSym >= static_cast<int>(kDistBase.size())) {
            return false;
        }
        const int distance = kDistBase[distSym] + br.GetBits(kDistExtra[distSym]);
        if (br.error || distance <= 0 ||
            static_cast<std::size_t>(distance) > out.size()) {
            return false;
        }
        const std::size_t start = out.size() - static_cast<std::size_t>(distance);
        for (int i = 0; i < length; ++i) {
            out.push_back(out[start + static_cast<std::size_t>(i)]);
        }
    }
}

void BuildFixedTables(Huffman& litLen, Huffman& dist) {
    std::array<int, 288> litLengths{};
    for (int i = 0; i < 144; ++i) litLengths[i] = 8;
    for (int i = 144; i < 256; ++i) litLengths[i] = 9;
    for (int i = 256; i < 280; ++i) litLengths[i] = 7;
    for (int i = 280; i < 288; ++i) litLengths[i] = 8;
    litLen.Build(litLengths.data(), 288);
    std::array<int, 30> distLengths{};
    for (int i = 0; i < 30; ++i) distLengths[i] = 5;
    dist.Build(distLengths.data(), 30);
}

bool BuildDynamicTables(BitReader& br, Huffman& litLen, Huffman& dist) {
    const int hlit = br.GetBits(5) + 257;
    const int hdist = br.GetBits(5) + 1;
    const int hclen = br.GetBits(4) + 4;
    if (br.error || hlit > 286 || hdist > 30) {
        return false;
    }
    std::array<int, 19> clen{};
    for (int i = 0; i < hclen; ++i) {
        clen[static_cast<std::size_t>(kCodeLengthOrder[i])] = br.GetBits(3);
    }
    if (br.error) {
        return false;
    }
    Huffman codeLen;
    codeLen.Build(clen.data(), 19);

    std::vector<int> lengths;
    lengths.reserve(static_cast<std::size_t>(hlit + hdist));
    while (static_cast<int>(lengths.size()) < hlit + hdist) {
        const int sym = codeLen.Decode(br);
        if (sym < 0) {
            return false;
        }
        if (sym < 16) {
            lengths.push_back(sym);
        } else if (sym == 16) {
            if (lengths.empty()) return false;
            const int prev = lengths.back();
            int repeat = 3 + br.GetBits(2);
            while (repeat-- > 0) lengths.push_back(prev);
        } else if (sym == 17) {
            int repeat = 3 + br.GetBits(3);
            while (repeat-- > 0) lengths.push_back(0);
        } else { // 18
            int repeat = 11 + br.GetBits(7);
            while (repeat-- > 0) lengths.push_back(0);
        }
        if (br.error) {
            return false;
        }
    }
    if (static_cast<int>(lengths.size()) != hlit + hdist) {
        return false;
    }
    litLen.Build(lengths.data(), hlit);
    dist.Build(lengths.data() + hlit, hdist);
    return true;
}

} // namespace

Result<std::vector<std::byte>> Inflate(
    std::span<const std::byte> input, std::uint32_t expectedSize) {
    // 识别并跳过 zlib 头（RFC 1950）：CMF/FLG，method==8 且 (CMF<<8|FLG)%31==0。
    std::size_t start = 0;
    if (input.size() >= 2) {
        const auto cmf = static_cast<unsigned char>(input[0]);
        const auto flg = static_cast<unsigned char>(input[1]);
        if ((cmf & 0x0F) == 8 &&
            ((static_cast<unsigned int>(cmf) << 8 | flg) % 31) == 0) {
            start = 2;
            if ((flg & 0x20) != 0) {
                start += 4; // 预置字典 ID，MPQ 一般不出现
            }
        }
    }
    if (start > input.size()) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "INFLATE_BAD_HEADER", "zlib header exceeds input");
    }

    BitReader br;
    br.in = input.subspan(start);

    std::vector<std::byte> out;
    out.reserve(expectedSize);

    bool lastBlock = false;
    while (!lastBlock) {
        lastBlock = br.GetBits(1) != 0;
        const int type = br.GetBits(2);
        if (br.error) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "INFLATE_TRUNCATED", "DEFLATE stream truncated in block header");
        }
        if (type == 0) {
            br.AlignToByte();
            if (br.pos + 4 > br.in.size()) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "INFLATE_TRUNCATED", "stored block header truncated");
            }
            const int len = static_cast<unsigned char>(br.in[br.pos]) |
                (static_cast<unsigned char>(br.in[br.pos + 1]) << 8);
            br.pos += 4; // 跳过 LEN + NLEN
            if (br.pos + static_cast<std::size_t>(len) > br.in.size()) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "INFLATE_TRUNCATED", "stored block data truncated");
            }
            for (int i = 0; i < len; ++i) {
                out.push_back(br.in[br.pos + static_cast<std::size_t>(i)]);
            }
            br.pos += static_cast<std::size_t>(len);
        } else if (type == 1) {
            Huffman litLen;
            Huffman dist;
            BuildFixedTables(litLen, dist);
            if (!InflateBlock(br, litLen, dist, out)) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "INFLATE_FAILED", "fixed Huffman block decode failed");
            }
        } else if (type == 2) {
            Huffman litLen;
            Huffman dist;
            if (!BuildDynamicTables(br, litLen, dist) ||
                !InflateBlock(br, litLen, dist, out)) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "INFLATE_FAILED", "dynamic Huffman block decode failed");
            }
        } else {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "INFLATE_BAD_BLOCK", "reserved DEFLATE block type");
        }
        if (out.size() > expectedSize) {
            break; // 已够，多余可能是 adler32 之外的填充
        }
    }
    return Result<std::vector<std::byte>>::Success(std::move(out));
}

} // namespace War3Map::Mpq
