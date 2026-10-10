#include "JpegDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::Image::detail {
namespace {

// JPEG 之字形（zig-zag）系数顺序。
constexpr std::array<int, 64> kZigZag{
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// 单张图的像素上限（防御损坏尺寸导致的超大分配）。
constexpr std::size_t kMaxPixels = 64u * 1024u * 1024u;
constexpr std::size_t kMaxWorkingBytes = 256u * 1024u * 1024u;

bool StoreCoefficient(std::int64_t value, std::int16_t& destination) {
    if (value < std::numeric_limits<std::int16_t>::min()
        || value > std::numeric_limits<std::int16_t>::max()) return false;
    destination = static_cast<std::int16_t>(value);
    return true;
}

struct HuffTable {
    // 规范 Huffman：按长度计数生成码字。
    std::array<std::uint8_t, 17> counts{}; // counts[1..16]
    std::vector<std::uint8_t> symbols;
    // 快速解码：maxcode/valptr/mincode 按长度。
    std::array<int, 18> maxcode{};
    std::array<int, 18> valptr{};
    std::array<int, 18> mincode{};
    bool defined = false;

    void Build() {
        int c = 0;
        int k = 0;
        for (int len = 1; len <= 16; ++len) {
            valptr[len] = k;
            mincode[len] = c;
            k += counts[len];
            c += counts[len];
            maxcode[len] = counts[len] > 0 ? (c - 1) : -1;
            c <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        defined = true;
    }
};

struct Component {
    int id = 0;
    int h = 1;      // 水平采样因子
    int v = 1;      // 垂直采样因子
    int quant = 0;  // 量化表索引
    int dcTable = 0;
    int acTable = 0;
    std::int64_t dcPred = 0;
    // 按 MCU 对齐的块数（交错扫描按 MCU 遍历）。
    int blocksW = 0;
    int blocksH = 0;
    // 组件自身覆盖的块数（单组件扫描只遍历这些块，不含 MCU 补齐）。
    int ownBlocksW = 0;
    int ownBlocksH = 0;
    // 每块 64 个量化系数（之字形顺序）；所有扫描结束后再反量化与 IDCT。
    std::vector<std::int16_t> coeffs;

    std::int16_t* Block(int row, int col) {
        return coeffs.data() + (static_cast<std::size_t>(row) * blocksW + col) * 64;
    }
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size, std::size_t pos)
        : data_(data), size_(size), pos_(pos) {}

    // 熵码需要数据时遇到 marker/EOF 就永久失败，禁止用补零接受截断块。
    // DecodeBlock 调用方同时检查 Failed()；正常 padding 由 FinishEntropySegment 检查。
    int GetBit() {
        if (bitsLeft_ == 0) {
            if (failed_ || pos_ >= size_) { failed_ = true; return 0; }
            cur_ = data_[pos_++];
            if (cur_ == 0xFF) {
                if (pos_ >= size_) { failed_ = true; return 0; }
                const std::uint8_t next = data_[pos_];
                if (next == 0x00) {
                    ++pos_; // 填充字节，0xFF 有效
                } else {
                    failed_ = true;
                    return 0;
                }
            }
            bitsLeft_ = 8;
        }
        --bitsLeft_;
        return (cur_ >> bitsLeft_) & 1;
    }

    int GetBits(int n) {
        if (n < 0 || n > 16) { failed_ = true; return 0; }
        int v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | GetBit();
        return v;
    }

    // 接收后扩展符号（JPEG receive_and_extend）。
    int Receive(int s) {
        if (s < 0 || s > 15) { failed_ = true; return 0; }
        if (s == 0) return 0;
        int v = GetBits(s);
        if (v < (1 << (s - 1))) v += (-(1 << s)) + 1;
        return v;
    }

    int DecodeHuffman(const HuffTable& table) {
        if (!table.defined) return -1;
        int code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | GetBit();
            if (table.maxcode[len] >= 0 && code <= table.maxcode[len]) {
                const int idx = table.valptr[len] + (code - table.mincode[len]);
                if (idx >= 0 && idx < static_cast<int>(table.symbols.size())) {
                    return table.symbols[idx];
                }
                return -1;
            }
        }
        return -1;
    }

    [[nodiscard]] bool Failed() const noexcept { return failed_; }

    // 剩余位通常为全 1 padding，后面只能是 marker（允许多个 FF 填充）。
    // 不跨未消费的熵数据搜索；返回 marker 前最后一个 FF 的位置。
    bool FinishEntropySegment(std::size_t& markerOffset, std::uint8_t& marker, bool allowFinalZero = false) {
        const int padding = (1 << bitsLeft_) - 1;
        if (failed_ || pos_ >= size_ || data_[pos_] != 0xFF) {
            failed_ = true;
            return false;
        }
        auto at = pos_;
        while (at + 1 < size_ && data_[at + 1] == 0xFF) ++at;
        if (at + 1 >= size_ || data_[at + 1] == 0x00) { failed_ = true; return false; }
        const int remaining = cur_ & padding;
        // 本机经典 BLP 的完整 MCU 后为全 0 尾位且紧接 EOI；不吞字节、不补熵位。
        if (remaining != padding && !(allowFinalZero && data_[at + 1] == 0xD9 && remaining == 0)) {
            failed_ = true;
            return false;
        }
        markerOffset = at;
        marker = data_[at + 1];
        return true;
    }

    void Restart(std::size_t pos) { pos_ = pos; bitsLeft_ = 0; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
    int bitsLeft_ = 0;
    std::uint8_t cur_ = 0;
    bool failed_ = false;
};

// 浮点分离式 8x8 IDCT（对图标与小纹理性能足够，正确性优先）。coeff 为自然顺序。
void Idct8x8(const int* coeff, std::uint8_t* out, int stride) {
    double tmp[64];
    // 函数静态初始化由 C++ 保证同步；发布后只读，允许后台并行解码首次进入 IDCT。
    static const auto cosTable = [] {
        std::array<std::array<double, 8>, 8> table{};
        for (int u = 0; u < 8; ++u) {
            for (int x = 0; x < 8; ++x) {
                table[u][x] = std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
            }
        }
        return table;
    }();
    // 行方向。
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (int u = 0; u < 8; ++u) {
                const double cu = (u == 0) ? 0.70710678118654752440 : 1.0;
                sum += cu * coeff[y * 8 + u] * cosTable[u][x];
            }
            tmp[y * 8 + x] = sum * 0.5;
        }
    }
    // 列方向。
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            double sum = 0.0;
            for (int v = 0; v < 8; ++v) {
                const double cv = (v == 0) ? 0.70710678118654752440 : 1.0;
                sum += cv * tmp[v * 8 + x] * cosTable[v][y];
            }
            // 先在浮点域饱和，避免极端系数在 Windows 的 32 位 long 上越界。
            const double val = std::clamp(sum * 0.5 + 128.0, 0.0, 255.0);
            const int iv = static_cast<int>(std::lround(val));
            out[y * stride + x] = static_cast<std::uint8_t>(iv);
        }
    }
}

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// 一次扫描（SOS）的参数与熵解码状态。baseline 等价于 Ss=0、Se=63、Ah=Al=0 的单次扫描。
struct Scan {
    std::vector<Component*> components;
    bool progressive = false;
    int ss = 0;
    int se = 63;
    int ah = 0;
    int al = 0;
    int eobrun = 0; // 渐进式 AC 扫描的块结束游程
};

class ScanDecoder {
public:
    ScanDecoder(BitReader& reader, const std::array<HuffTable, 4>& dc, const std::array<HuffTable, 4>& ac, Scan& scan)
        : reader_(reader), dc_(dc), ac_(ac), scan_(scan) {}

    bool DecodeBlock(Component& c, std::int16_t* block) {
        if (!scan_.progressive) return Sequential(c, block);
        if (scan_.ss == 0) return scan_.ah == 0 ? DcFirst(c, block) : DcRefine(block);
        return scan_.ah == 0 ? AcFirst(c, block) : AcRefine(c, block);
    }

private:
    bool DecodeDc(Component& c, std::int16_t& destination, int al) {
        const int category = reader_.DecodeHuffman(dc_[c.dcTable]);
        if (category < 0 || category > 11 || al < 0 || al > 13) return false;
        const std::int64_t difference = reader_.Receive(category);
        // 每个已接受的预测值来自 int16 系数边界；仍显式检查中间运算。
        if ((difference > 0 && c.dcPred > std::numeric_limits<std::int64_t>::max() - difference)
            || (difference < 0 && c.dcPred < std::numeric_limits<std::int64_t>::min() - difference)) return false;
        const std::int64_t next = c.dcPred + difference;
        const std::int64_t scale = std::int64_t{1} << al;
        if (next > std::numeric_limits<std::int64_t>::max() / scale
            || next < std::numeric_limits<std::int64_t>::min() / scale
            || !StoreCoefficient(next * scale, destination)) return false;
        c.dcPred = next;
        return true;
    }

    bool Sequential(Component& c, std::int16_t* block) {
        if (!DecodeDc(c, block[0], 0)) return false;
        for (int k = 1; k < 64;) {
            const int rs = reader_.DecodeHuffman(ac_[c.acTable]);
            if (rs < 0) return false;
            const int r = rs >> 4;
            const int s = rs & 0x0F;
            if (s > 10) return false;
            if (s == 0) {
                if (r != 15) break; // EOB
                k += 16;            // ZRL
                continue;
            }
            k += r;
            if (k > 63) return false;
            if (!StoreCoefficient(reader_.Receive(s), block[k++])) return false;
        }
        return true;
    }

    // 渐进式 DC 首次扫描：差分解码后左移 Al 位。
    bool DcFirst(Component& c, std::int16_t* block) {
        return DecodeDc(c, block[0], scan_.al);
    }

    // DC 精化扫描：每块补 1 位。
    bool DcRefine(std::int16_t* block) {
        if (reader_.GetBit() != 0) block[0] = static_cast<std::int16_t>(block[0] | (1 << scan_.al));
        return true;
    }

    // 渐进式 AC 首次扫描（频带 Ss..Se），支持 EOBRUN。
    bool AcFirst(Component& c, std::int16_t* block) {
        if (scan_.eobrun > 0) {
            --scan_.eobrun;
            return true;
        }
        for (int k = scan_.ss; k <= scan_.se;) {
            const int rs = reader_.DecodeHuffman(ac_[c.acTable]);
            if (rs < 0) return false;
            const int r = rs >> 4;
            const int s = rs & 0x0F;
            if (s > 10) return false;
            if (s == 0) {
                if (r < 15) {
                    // EOBr：本块及其后 eobrun 个块的剩余系数均为 0。
                    scan_.eobrun = (1 << r) - 1;
                    if (r > 0) scan_.eobrun += reader_.GetBits(r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            if (!StoreCoefficient(static_cast<std::int64_t>(reader_.Receive(s)) * (std::int64_t{1} << scan_.al),
                block[k++])) return false;
        }
        return true;
    }

    // 已非零系数的精化位：同号方向加 1<<Al。
    bool RefineNonZero(std::int16_t& coef, int p1) {
        if (reader_.GetBit() != 0 && (coef & p1) == 0) {
            return StoreCoefficient(static_cast<std::int64_t>(coef) + (coef >= 0 ? p1 : -p1), coef);
        }
        return true;
    }

    // 渐进式 AC 精化扫描（与 ITU T.81 G.1.2.3 / libjpeg decode_mcu_AC_refine 一致）。
    bool AcRefine(Component& c, std::int16_t* block) {
        const int p1 = 1 << scan_.al;
        int k = scan_.ss;
        if (scan_.eobrun == 0) {
            for (; k <= scan_.se; ++k) {
                const int rs = reader_.DecodeHuffman(ac_[c.acTable]);
                if (rs < 0) return false;
                int r = rs >> 4;
                int s = rs & 0x0F;
                int value = 0;
                if (s != 0) {
                    if (s != 1) return false; // 精化扫描的新系数只能是 ±1
                    value = reader_.GetBit() != 0 ? p1 : -p1;
                } else if (r != 15) {
                    scan_.eobrun = 1 << r;
                    if (r > 0) scan_.eobrun += reader_.GetBits(r);
                    break;
                }
                // 跳过 r 个零系数（途经的非零系数读精化位），停在新系数的位置。
                for (; k <= scan_.se; ++k) {
                    std::int16_t& coef = block[k];
                    if (coef != 0) {
                        if (!RefineNonZero(coef, p1)) return false;
                    } else if (--r < 0) {
                        break;
                    }
                }
                if (value != 0 && k <= scan_.se) block[k] = static_cast<std::int16_t>(value);
            }
        }
        if (scan_.eobrun > 0) {
            // 本块处于 EOB 游程中：只精化剩余的非零系数。
            for (; k <= scan_.se; ++k) {
                if (block[k] != 0 && !RefineNonZero(block[k], p1)) return false;
            }
            --scan_.eobrun;
        }
        return true;
    }

    BitReader& reader_;
    const std::array<HuffTable, 4>& dc_;
    const std::array<HuffTable, 4>& ac_;
    Scan& scan_;
};

} // namespace

Result<JpegDecoded> DecodeJpeg(std::span<const std::byte> data, const JpegDecodeOptions& options) try {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t size = data.size();
    auto fail = [](const char* code, const char* msg) {
        return Result<JpegDecoded>::Failure("Image.Jpeg", code, msg);
    };
    if (size > kMaxJpegInputBytes) return fail("input-budget", "JPEG 输入超过 64 MiB 上限。");
    constexpr std::size_t overheadBytes = 64u * 1024u;
    if (options.maxWorkingBytes == 0 || options.maxWorkingBytes > kMaxWorkingBytes
        || options.callerRetainedBytes > options.maxWorkingBytes
        || size > options.maxWorkingBytes - options.callerRetainedBytes
        || overheadBytes > options.maxWorkingBytes - options.callerRetainedBytes - size) {
        return fail("working-budget", "JPEG 输入或调用方保留数据超过工作集预算。");
    }
    const auto availableBytes = options.maxWorkingBytes - options.callerRetainedBytes - size - overheadBytes;
    if (size < 4 || p[0] != 0xFF || p[1] != 0xD8) return fail("no-soi", "缺少 JPEG SOI 标记。");

    std::array<std::array<std::uint16_t, 64>, 4> quant{};
    std::array<HuffTable, 4> dcTables{};
    std::array<HuffTable, 4> acTables{};
    std::vector<Component> components;
    int width = 0;
    int height = 0;
    int maxH = 1;
    int maxV = 1;
    int mcusPerRow = 0;
    int mcusPerCol = 0;
    std::size_t largestPlaneSize = 0;
    int restartInterval = 0;
    bool progressive = false;
    int scans = 0;
    bool sawEndOfImage = false;

    std::size_t pos = 2;
    while (pos + 2 <= size) {
        if (p[pos] != 0xFF) return fail("bad-marker", "段边界出现非 JPEG 标记字节。");
        const std::uint8_t marker = p[pos + 1];
        if (marker == 0xFF) { ++pos; continue; } // 填充字节
        pos += 2;
        if (marker == 0xD9) { sawEndOfImage = true; break; } // EOI
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // 无长度标记

        if (pos + 2 > size) return fail("bad-seg", "段长度被截断。");
        const int segLen = ReadU16(p + pos);
        if (segLen < 2 || pos + segLen > size) return fail("bad-seg", "段长度非法。");
        const std::uint8_t* seg = p + pos + 2;
        const int segDataLen = segLen - 2;

        if (marker == 0xDB) { // DQT
            int off = 0;
            while (off < segDataLen) {
                const int pq = seg[off] >> 4;
                const int tq = seg[off] & 0x0F;
                ++off;
                if (tq > 3) return fail("dqt-id", "量化表 id 越界。");
                if (off + (pq == 0 ? 64 : 128) > segDataLen) return fail("dqt-size", "量化表越界。");
                for (int i = 0; i < 64; ++i) {
                    if (pq == 0) {
                        quant[tq][i] = seg[off++];
                    } else {
                        quant[tq][i] = ReadU16(seg + off);
                        off += 2;
                    }
                }
            }
        } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) { // SOF0/1 顺序，SOF2 渐进式
            if (!components.empty()) return fail("sof-repeat", "重复的 SOF 帧头。");
            if (segDataLen < 6 || seg[0] != 8) return fail("sof-precision", "只支持 8 位精度。");
            progressive = marker == 0xC2;
            height = ReadU16(seg + 1);
            width = ReadU16(seg + 3);
            const int nc = seg[5];
            if (nc < 1 || nc > 4 || segDataLen < 6 + nc * 3) return fail("sof-comp", "组件数不支持。");
            if (width <= 0 || height <= 0 || static_cast<std::size_t>(width) * height > kMaxPixels) {
                return fail("sof-size", "图像尺寸非法。");
            }
            if ((options.expectedWidth != 0 && options.expectedWidth != static_cast<std::uint32_t>(width))
                || (options.expectedHeight != 0 && options.expectedHeight != static_cast<std::uint32_t>(height))) {
                return fail("sof-dimensions", "JPEG 内部尺寸与 BLP 声明不一致，尚未分配系数。");
            }
            for (int i = 0; i < nc; ++i) {
                Component c;
                c.id = seg[6 + i * 3];
                c.h = seg[7 + i * 3] >> 4;
                c.v = seg[7 + i * 3] & 0x0F;
                c.quant = seg[8 + i * 3];
                if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.quant > 3) return fail("sof-comp", "组件参数非法。");
                components.push_back(c);
            }
            for (const auto& c : components) {
                maxH = std::max(maxH, c.h);
                maxV = std::max(maxV, c.v);
            }
            mcusPerRow = (width + 8 * maxH - 1) / (8 * maxH);
            mcusPerCol = (height + 8 * maxV - 1) / (8 * maxV);
            // uint16 尺寸与 1..4 采样因子先受上方检查；以下使用 uint64 汇总，
            // 所有组件几何和预算检查通过后才分配首个系数向量。
            std::uint64_t coefficientBytes = 0;
            std::uint64_t largestPlaneBytes = 0;
            for (auto& c : components) {
                if (maxH % c.h != 0 || maxV % c.v != 0) return fail("sof-sampling", "不支持的采样因子组合。");
                c.blocksW = mcusPerRow * c.h;
                c.blocksH = mcusPerCol * c.v;
                c.ownBlocksW = ((width * c.h + maxH - 1) / maxH + 7) / 8;
                c.ownBlocksH = ((height * c.v + maxV - 1) / maxV + 7) / 8;
                const auto planeBytes = static_cast<std::uint64_t>(c.blocksW) * c.blocksH * 64;
                coefficientBytes += planeBytes * sizeof(std::int16_t);
                largestPlaneBytes = std::max(largestPlaneBytes, planeBytes);
            }
            const auto pixelCount = static_cast<std::uint64_t>(width) * height;
            const auto componentBytes = pixelCount * components.size();
            const auto decodePeak = coefficientBytes + componentBytes + largestPlaneBytes;
            const auto rgbaPeak = componentBytes + pixelCount * 4;
            if (std::max(decodePeak, rgbaPeak) > availableBytes) {
                return fail("working-budget", "JPEG 系数、像素和平面峰值超过工作集预算，尚未分配系数。");
            }
            largestPlaneSize = static_cast<std::size_t>(largestPlaneBytes);
            for (auto& c : components) {
                c.coeffs.assign(static_cast<std::size_t>(c.blocksW) * c.blocksH * 64, 0);
            }
        } else if ((marker >= 0xC3 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return fail("sof-unsupported", "不支持的 JPEG 编码方式（无损或算术编码）。");
        } else if (marker == 0xC4) { // DHT
            int off = 0;
            while (off < segDataLen) {
                const int tc = seg[off] >> 4; // 0=DC,1=AC
                const int th = seg[off] & 0x0F;
                ++off;
                if (tc > 1 || th > 3 || off + 16 > segDataLen) return fail("dht-id", "Huffman 表越界。");
                HuffTable& t = (tc == 0) ? dcTables[th] : acTables[th];
                int total = 0;
                for (int len = 1; len <= 16; ++len) {
                    t.counts[len] = seg[off++];
                    total += t.counts[len];
                }
                if (off + total > segDataLen) return fail("dht-size", "Huffman 表越界。");
                t.symbols.assign(seg + off, seg + off + total);
                for (const auto symbol : t.symbols) {
                    if (tc == 0 && symbol > 11) return fail("dc-category", "8 位 JPEG 的 DC 类别不能超过 11。");
                    if (tc == 1 && (symbol & 15) > 10) return fail("ac-category", "8 位 JPEG 的 AC 类别不能超过 10。");
                }
                off += total;
                t.Build();
            }
        } else if (marker == 0xDD) { // DRI
            if (segDataLen != 2) return fail("dri-size", "重启间隔段长度非法。");
            restartInterval = ReadU16(seg);
        } else if (marker == 0xDA) { // SOS
            if (components.empty()) return fail("no-sof", "扫描出现在 SOF 帧头之前。");
            if (segDataLen < 1) return fail("sos-comp", "扫描头被截断。");
            const int ns = seg[0];
            if (ns < 1 || ns > 4 || segDataLen < 1 + ns * 2 + 3) return fail("sos-comp", "扫描组件数非法。");
            Scan scan;
            scan.progressive = progressive;
            for (int i = 0; i < ns; ++i) {
                const int cid = seg[1 + i * 2];
                const int tables = seg[2 + i * 2];
                const auto found = std::find_if(components.begin(), components.end(),
                    [cid](const Component& c) { return c.id == cid; });
                if (found == components.end()) return fail("sos-comp", "扫描引用了不存在的组件。");
                found->dcTable = (tables >> 4) & 3;
                found->acTable = tables & 3;
                scan.components.push_back(&*found);
            }
            scan.ss = seg[1 + ns * 2];
            scan.se = seg[2 + ns * 2];
            scan.ah = seg[3 + ns * 2] >> 4;
            scan.al = seg[3 + ns * 2] & 0x0F;
            if (progressive) {
                const bool dc = scan.ss == 0;
                if (scan.se > 63 || scan.ss > scan.se || (dc && scan.se != 0) || (!dc && ns != 1) || scan.al > 13) {
                    return fail("sos-progressive", "渐进式扫描参数非法。");
                }
            } else if (scan.ss != 0 || scan.se != 63 || scan.ah != 0 || scan.al != 0) {
                return fail("sos-sequential", "顺序扫描参数非法。");
            }
            for (auto* c : scan.components) c->dcPred = 0;

            // 熵编码数据：单组件扫描按组件自身的块遍历，交错扫描按 MCU 遍历。
            const bool single = ns == 1;
            Component& first = *scan.components.front();
            const int unitsW = single ? first.ownBlocksW : mcusPerRow;
            const int unitsH = single ? first.ownBlocksH : mcusPerCol;
            const int units = unitsW * unitsH;
            BitReader reader(p, size, pos + segLen);
            ScanDecoder decoder(reader, dcTables, acTables, scan);
            for (int unit = 0; unit < units; ++unit) {
                if (restartInterval > 0 && unit > 0 && unit % restartInterval == 0) {
                    std::size_t sp = 0;
                    std::uint8_t nextMarker = 0;
                    const auto expected = static_cast<std::uint8_t>(0xD0 + ((unit / restartInterval - 1) % 8));
                    if (scan.eobrun != 0 || !reader.FinishEntropySegment(sp, nextMarker) || nextMarker != expected)
                        return fail("restart", "重启边界、padding 或 RST 序号无效。");
                    reader.Restart(sp + 2);
                    for (auto* c : scan.components) c->dcPred = 0;
                    scan.eobrun = 0;
                }
                const int row = unit / unitsW;
                const int col = unit % unitsW;
                if (single) {
                    if (!decoder.DecodeBlock(first, first.Block(row, col)) || reader.Failed()) return fail("huffman", "Huffman 解码失败或熵码被截断。");
                    continue;
                }
                for (auto* c : scan.components) {
                    for (int by = 0; by < c->v; ++by) {
                        for (int bx = 0; bx < c->h; ++bx) {
                            if (!decoder.DecodeBlock(*c, c->Block(row * c->v + by, col * c->h + bx)) || reader.Failed()) {
                                return fail("huffman", "Huffman 解码失败。");
                            }
                        }
                    }
                }
            }
            ++scans;
            std::size_t next = 0;
            std::uint8_t nextMarker = 0;
            if (scan.eobrun != 0 || !reader.FinishEntropySegment(next, nextMarker,
                options.allowBlpFinalZeroPadding && !progressive) || (nextMarker >= 0xD0 && nextMarker <= 0xD7))
                return fail("entropy-end", "扫描结束的 padding、剩余熵码或后续标记无效。");
            pos = next;
            continue;
        }
        pos += segLen;
    }

    if (components.empty()) return fail("no-sof", "缺少有效的 SOF 帧头。");
    if (scans == 0) return fail("no-scan", "缺少扫描数据。");
    if (!sawEndOfImage) return fail("no-eoi", "JPEG 缺少完整 EOI，未接受截断图像。");

    // 反量化与 IDCT 到各组件平面，再按采样因子上采样并交错输出。
    JpegDecoded result;
    result.width = width;
    result.height = height;
    result.components = static_cast<int>(components.size());
    result.pixels.assign(static_cast<std::size_t>(width) * height * components.size(), 0);

    std::vector<std::uint8_t> plane;
    // 一次预留最大平面，避免逐组件扩容时旧、新平面同时存活突破峰值估算。
    plane.reserve(largestPlaneSize);
    for (std::size_t ci = 0; ci < components.size(); ++ci) {
        Component& c = components[ci];
        const int planeW = c.blocksW * 8;
        plane.assign(static_cast<std::size_t>(planeW) * c.blocksH * 8, 0);
        const auto& table = quant[c.quant];
        for (int row = 0; row < c.blocksH; ++row) {
            for (int col = 0; col < c.blocksW; ++col) {
                const std::int16_t* block = c.Block(row, col);
                std::array<int, 64> natural{};
                for (int k = 0; k < 64; ++k) natural[kZigZag[k]] = block[k] * table[k];
                Idct8x8(natural.data(), plane.data() + static_cast<std::size_t>(row) * 8 * planeW + col * 8, planeW);
            }
        }
        const int sx = maxH / c.h;
        const int sy = maxV / c.v;
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* line = plane.data() + static_cast<std::size_t>(y / sy) * planeW;
            for (int x = 0; x < width; ++x) {
                result.pixels[(static_cast<std::size_t>(y) * width + x) * components.size() + ci] = line[x / sx];
            }
        }
    }
    return Result<JpegDecoded>::Success(std::move(result));
} catch (const std::bad_alloc&) {
    return Result<JpegDecoded>::Failure("Image.Jpeg", "allocation", "JPEG 解码内存分配失败。");
} catch (const std::length_error&) {
    return Result<JpegDecoded>::Failure("Image.Jpeg", "allocation", "JPEG 解码分配长度非法。");
}

} // namespace War3Map::Image::detail
