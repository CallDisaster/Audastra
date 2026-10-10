#include "MpqDecompress.h"

#include <array>
#include <cstdint>

namespace War3Map::Mpq {
namespace {

// PKWARE DCL explode 位读取器（LSB-first，不反转；反转只发生在 Huffman decode 内）。
struct PkReader {
    std::span<const std::byte> in;
    std::size_t pos = 0;
    std::uint32_t buf = 0;
    int cnt = 0;
    bool error = false;

    int Bits(int need) noexcept {
        while (cnt < need) {
            if (pos >= in.size()) {
                error = true;
                return 0;
            }
            buf |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos++])) << cnt;
            cnt += 8;
        }
        const int val = static_cast<int>(buf & ((1u << need) - 1));
        buf >>= need;
        cnt -= need;
        return val;
    }
};

struct PkHuffman {
    std::array<short, 16> count{};
    std::vector<short> symbol;

    // rep 为紧凑表示：每字节 = ((重复数-1) << 4) | 码长。
    void Construct(const unsigned char* rep, int n) {
        std::array<short, 256> length{};
        int symbolCount = 0;
        int remaining = n;
        do {
            int len = *rep++;
            int left = (len >> 4) + 1;
            len &= 15;
            do {
                length[static_cast<std::size_t>(symbolCount++)] = static_cast<short>(len);
            } while (--left != 0);
        } while (--remaining != 0);

        count.fill(0);
        for (int i = 0; i < symbolCount; ++i) {
            ++count[length[static_cast<std::size_t>(i)]];
        }
        std::array<short, 16> offsets{};
        for (int len = 1; len < 15; ++len) {
            offsets[len + 1] = static_cast<short>(offsets[len] + count[len]);
        }
        symbol.assign(static_cast<std::size_t>(symbolCount), 0);
        for (int i = 0; i < symbolCount; ++i) {
            const int len = length[static_cast<std::size_t>(i)];
            if (len != 0) {
                symbol[static_cast<std::size_t>(offsets[len]++)] = static_cast<short>(i);
            }
        }
    }

    // PKWARE 特有：从位流取位后按位取反再拼码。
    int Decode(PkReader& r) const noexcept {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= r.Bits(1) ^ 1;
            if (r.error) {
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

// blast.c 的紧凑码长表。
constexpr unsigned char kLitLen[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173};
constexpr unsigned char kLenLen[] = {2, 35, 36, 53, 38, 23};
constexpr unsigned char kDistLen[] = {2, 20, 53, 230, 247, 151, 248};

constexpr std::array<short, 16> kBase{
    3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
constexpr std::array<unsigned char, 16> kExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

} // namespace

Result<std::vector<std::byte>> PkwareExplode(
    std::span<const std::byte> input, std::uint32_t expectedSize) {
    PkReader r;
    r.in = input;

    const int literalMode = r.Bits(8); // 0=二进制字面量，1=编码字面量
    const int dictBits = r.Bits(8);    // 4/5/6
    if (r.error || literalMode > 1 || dictBits < 4 || dictBits > 6) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "EXPLODE_BAD_HEADER", "invalid PKWARE DCL header");
    }

    PkHuffman litCode;
    PkHuffman lenCode;
    PkHuffman distCode;
    litCode.Construct(kLitLen, static_cast<int>(sizeof(kLitLen)));
    lenCode.Construct(kLenLen, static_cast<int>(sizeof(kLenLen)));
    distCode.Construct(kDistLen, static_cast<int>(sizeof(kDistLen)));

    std::vector<std::byte> out;
    out.reserve(expectedSize);

    while (true) {
        if (r.Bits(1) != 0) {
            // 长度/距离对
            const int lenSym = lenCode.Decode(r);
            if (lenSym < 0) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "EXPLODE_FAILED", "bad length symbol");
            }
            const int length = kBase[lenSym] + r.Bits(kExtra[lenSym]);
            if (length == 519) {
                break; // 结束标记
            }
            const int lowBits = (length == 2) ? 2 : dictBits;
            int distance = distCode.Decode(r) << lowBits;
            distance += r.Bits(lowBits);
            ++distance;
            if (r.error || static_cast<std::size_t>(distance) > out.size()) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "EXPLODE_FAILED", "invalid back-reference distance");
            }
            const std::size_t from = out.size() - static_cast<std::size_t>(distance);
            for (int i = 0; i < length; ++i) {
                out.push_back(out[from + static_cast<std::size_t>(i)]);
            }
        } else {
            // 字面量
            const int symbol = literalMode != 0 ? litCode.Decode(r) : r.Bits(8);
            if (symbol < 0 || r.error) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "EXPLODE_FAILED", "bad literal");
            }
            out.push_back(static_cast<std::byte>(symbol));
        }
        if (r.error) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "EXPLODE_TRUNCATED", "PKWARE stream truncated");
        }
        if (out.size() >= expectedSize) {
            break;
        }
    }
    return Result<std::vector<std::byte>>::Success(std::move(out));
}

} // namespace War3Map::Mpq
