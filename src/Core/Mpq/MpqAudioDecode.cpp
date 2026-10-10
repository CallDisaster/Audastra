// MPQ 音频格式算法派生自 StormLib (MIT)。
// Copyright (c) Ladislav Zezula 1998-2026; ShadowFlare; ADPCM credits: Tom Amigo.
// 上游及完整许可见 third_party/stormlib-audio/LICENSE 与 README.md。
#include "MpqAudioDecode.h"
#include "MpqAudioTables.h"

#include <algorithm>
#include <array>
#include <utility>

namespace War3Map::Mpq {
namespace {

using BytesResult = Result<std::vector<std::byte>>;

BytesResult AudioFailure(const char* code, const char* message) {
    return BytesResult::Failure("Mpq", code, message);
}

class AudioBits {
public:
    explicit AudioBits(std::span<const std::byte> input) : input_(input) {}

    bool Read(unsigned count, unsigned& value) {
        // 本解码器只请求 1 或 8 位；不足时不补零、不越过输入。
        while (available_ < count) {
            if (offset_ == input_.size()) return false;
            bits_ |= static_cast<unsigned>(input_[offset_++]) << available_;
            available_ += 8;
        }
        value = bits_ & ((1u << count) - 1);
        bits_ >>= count;
        available_ -= count;
        return true;
    }

private:
    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
    unsigned bits_ = 0;
    unsigned available_ = 0;
};

// 双向链表按权重从大到小排列；兄弟节点相邻，低权重孩子的 prev 即高孩子。
// 256 字节 + 终止/NYT 两个符号，共最多 515 节点，无输入驱动的节点分配。
class AudioHuffmanTree {
    struct Node {
        Node* previous = nullptr;
        Node* next = nullptr;
        Node* parent = nullptr;
        Node* low = nullptr;
        std::uint64_t weight = 0;
        unsigned value = 0;
    };

public:
    AudioHuffmanTree() {
        head_.next = head_.previous = &head_;
    }

    bool Build(unsigned type) {
        // Storm 的数据类型高四位不参与分布表选择。
        type &= 0x0Fu;
        if (type >= AudioDetail::kDistributions.size()) return false;
        std::uint64_t maximum = 0;
        for (unsigned value = 0; value < 256; ++value) {
            const auto weight = AudioDetail::kDistributions[type][value];
            if (weight == 0) continue;
            auto* leaf = NewNode(value, weight, false);
            if (leaf == nullptr) return false;
            leaves_[value] = leaf;
            maximum = SortNewNode(leaf, maximum);
        }
        leaves_[256] = NewNode(256, 1, true);
        leaves_[257] = NewNode(257, 1, true);
        if (leaves_[256] == nullptr || leaves_[257] == nullptr) return false;
        auto* low = head_.previous;
        while (low != &head_) {
            auto* high = low->previous;
            if (high == &head_) break;
            auto* parent = NewNode(0, low->weight + high->weight, false);
            if (parent == nullptr) return false;
            low->parent = high->parent = parent;
            parent->low = low;
            maximum = SortNewNode(parent, maximum);
            low = high->previous;
        }
        return true;
    }

    bool Decode(AudioBits& bits, unsigned& value) const {
        const Node* item = head_.next;
        if (item == &head_) return false;
        for (std::size_t depth = 0; depth < nodes_.size(); ++depth) {
            if (item->low == nullptr) {
                value = item->value;
                return true;
            }
            unsigned bit = 0;
            if (!bits.Read(1, bit)) return false;
            item = bit != 0 ? item->low->previous : item->low;
            if (item == nullptr || item == &head_) return false;
        }
        return false;
    }

    bool AddByte(unsigned value) {
        if (value >= 256 || leaves_[value] != nullptr || used_ + 2 > nodes_.size()) return false;
        auto* branch = head_.previous;
        auto* high = NewNode(branch->value, branch->weight, true);
        auto* low = NewNode(value, 0, true);
        if (high == nullptr || low == nullptr) return false;
        high->parent = low->parent = branch;
        branch->low = low;
        leaves_[high->value] = high;
        leaves_[value] = low;
        return Rebalance(low);
    }

    bool Increment(unsigned value) {
        return value < leaves_.size() && leaves_[value] != nullptr && Rebalance(leaves_[value]);
    }

private:
    static void Remove(Node* item) {
        if (item->next == nullptr) return;
        item->previous->next = item->next;
        item->next->previous = item->previous;
        item->next = item->previous = nullptr;
    }

    static void LinkAfter(Node* position, Node* item) {
        item->next = position->next;
        item->previous = position;
        position->next->previous = item;
        position->next = item;
    }

    Node* NewNode(unsigned value, std::uint64_t weight, bool atTail) {
        if (used_ == nodes_.size()) return nullptr;
        auto* item = &nodes_[used_++];
        item->value = value;
        item->weight = weight;
        LinkAfter(atTail ? head_.previous : &head_, item);
        return item;
    }

    Node* FindAtLeast(Node* start, std::uint64_t weight) {
        while (start != &head_ && start->weight < weight) start = start->previous;
        return start;
    }

    std::uint64_t SortNewNode(Node* item, std::uint64_t maximum) {
        if (item->weight >= maximum) return item->weight;
        auto* position = FindAtLeast(head_.previous, item->weight);
        Remove(item);
        LinkAfter(position, item);
        return maximum;
    }

    bool Rebalance(Node* item) {
        for (std::size_t depth = 0; item != nullptr && depth < nodes_.size(); ++depth) {
            ++item->weight;
            auto* position = FindAtLeast(item->previous, item->weight);
            auto* peer = position->next;
            if (peer != item) {
                if (item->parent == nullptr || peer == &head_ || peer->parent == nullptr) return false;
                Remove(peer);
                LinkAfter(item, peer);
                Remove(item);
                LinkAfter(position, item);
                // 两个待交换节点可能共用父节点，须在修改前保存 low 身份。
                auto* peerLow = peer->parent->low;
                auto* parent = item->parent;
                if (parent->low == item) parent->low = peer;
                if (peerLow == peer) peer->parent->low = item;
                std::swap(item->parent, peer->parent);
            }
            item = item->parent;
        }
        return item == nullptr;
    }

    std::array<Node, 515> nodes_{};
    std::array<Node*, 258> leaves_{};
    Node head_{};
    std::size_t used_ = 0;
};

BytesResult DecodeHuffman(std::span<const std::byte> input, std::uint32_t capacity) {
    AudioBits bits(input);
    unsigned type = 0;
    AudioHuffmanTree tree;
    if (!bits.Read(8, type) || !tree.Build(type)) {
        return AudioFailure("AUDIO_HUFFMAN_HEADER", "invalid or truncated MPQ Huffman distribution header");
    }
    const bool sparse = type == 0;
    std::vector<std::byte> output;
    output.reserve(std::min<std::size_t>(capacity, 65536));
    for (;;) {
        unsigned value = 0;
        if (!tree.Decode(bits, value)) {
            return AudioFailure("AUDIO_HUFFMAN_TRUNCATED", "MPQ Huffman stream ended before its termination symbol");
        }
        if (value == 256) return BytesResult::Success(std::move(output));
        if (value == 257) {
            if (!bits.Read(8, value)) {
                return AudioFailure("AUDIO_HUFFMAN_TRUNCATED", "MPQ Huffman NYT byte is truncated");
            }
            if (!tree.AddByte(value) || (!sparse && !tree.Increment(value))) {
                return AudioFailure("AUDIO_HUFFMAN_TREE", "invalid or duplicate MPQ Huffman NYT byte");
            }
        }
        if (output.size() == capacity) {
            return AudioFailure("AUDIO_HUFFMAN_CAPACITY", "MPQ Huffman stage exceeds the sector output capacity");
        }
        output.push_back(static_cast<std::byte>(value));
        if (sparse && !tree.Increment(value)) {
            return AudioFailure("AUDIO_HUFFMAN_TREE", "invalid adaptive MPQ Huffman tree");
        }
    }
}

constexpr std::array<int, 32> kNextStep{
    -1, 0, -1, 4, -1, 2, -1, 6, -1, 1, -1, 5, -1, 3, -1, 7,
    -1, 1, -1, 5, -1, 3, -1, 7, -1, 2, -1, 4, -1, 6, -1, 8,
};

constexpr std::array<int, 89> kStepSize{
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

BytesResult DecodeAdpcm(std::span<const std::byte> input, std::uint32_t expectedSize, unsigned channels) {
    const std::size_t headerSize = 2 + channels * 2;
    if (input.size() < headerSize || expectedSize < channels * 2 || (expectedSize & 1u) != 0) {
        return AudioFailure("AUDIO_ADPCM_HEADER", "truncated MPQ ADPCM header or invalid PCM output size");
    }
    // 第一字节为保留字节，Storm 忽略它。第二字节是非负步长右移量。
    const auto shift = static_cast<unsigned>(input[1]);
    if (shift >= 32) {
        return AudioFailure("AUDIO_ADPCM_SHIFT", "MPQ ADPCM bit shift exceeds the 32-bit sample arithmetic");
    }
    std::array<int, 2> predicted{};
    std::array<int, 2> indexes{44, 44};
    std::vector<std::byte> output;
    output.reserve(std::min<std::size_t>(expectedSize, 65536));
    const auto writeSample = [&output, expectedSize](int sample) {
        if (output.size() + 2 > expectedSize) return false;
        const auto value = static_cast<std::uint16_t>(sample);
        output.push_back(static_cast<std::byte>(value & 0xFF));
        output.push_back(static_cast<std::byte>(value >> 8));
        return true;
    };
    for (unsigned channel = 0; channel < channels; ++channel) {
        const auto offset = 2 + channel * 2;
        const int value = static_cast<int>(static_cast<unsigned>(input[offset]) | (static_cast<unsigned>(input[offset + 1]) << 8));
        predicted[channel] = value >= 32768 ? value - 65536 : value;
        if (!writeSample(predicted[channel])) {
            return AudioFailure("AUDIO_ADPCM_CAPACITY", "MPQ ADPCM initial samples exceed output capacity");
        }
    }
    unsigned channel = channels - 1;
    for (std::size_t offset = headerSize; offset < input.size(); ++offset) {
        channel = (channel + 1) % channels;
        const auto encoded = static_cast<unsigned>(input[offset]);
        if (encoded == 0x81) {
            indexes[channel] = std::min(indexes[channel] + 8, 88);
            // 0x81 不输出样本，下个码字仍属于本声道。
            channel = (channel + 1) % channels;
            continue;
        }
        if (encoded == 0x80) {
            indexes[channel] = std::max(indexes[channel] - 1, 0);
        } else {
            const int step = kStepSize[static_cast<std::size_t>(indexes[channel])];
            int difference = step >> shift;
            for (unsigned bit = 0; bit < 6; ++bit) {
                if ((encoded & (1u << bit)) != 0) difference += step >> bit;
            }
            predicted[channel] = std::clamp(predicted[channel] + ((encoded & 0x40u) != 0 ? -difference : difference), -32768, 32767);
            indexes[channel] = std::clamp(indexes[channel] + kNextStep[encoded & 0x1Fu], 0, 88);
        }
        if (!writeSample(predicted[channel])) {
            return AudioFailure("AUDIO_ADPCM_CAPACITY", "MPQ ADPCM samples exceed sector output capacity");
        }
    }
    if (output.size() != expectedSize) {
        return AudioFailure("AUDIO_ADPCM_SIZE", "MPQ ADPCM samples do not fill the declared sector output size");
    }
    return BytesResult::Success(std::move(output));
}

} // namespace

Result<std::vector<std::byte>> DecodeAudioSector(
    std::span<const std::byte> payload, std::uint32_t expectedSize, std::uint8_t mask) {
    if (mask != 0x01 && mask != 0x40 && mask != 0x80 && mask != 0x41 && mask != 0x81) {
        return AudioFailure("AUDIO_UNSUPPORTED_MASK", "unsupported MPQ audio compression chain");
    }
    if (expectedSize == 0) {
        return AudioFailure("AUDIO_OUTPUT_EMPTY", "MPQ audio sector must declare a nonempty output");
    }
    std::vector<std::byte> intermediate;
    if ((mask & 0x01u) != 0) {
        auto huffman = DecodeHuffman(payload, expectedSize);
        if (!huffman.Succeeded()) return huffman;
        intermediate = std::move(*huffman.value);
        payload = intermediate;
    }
    if (mask == 0x01) {
        if (intermediate.size() != expectedSize) {
            return AudioFailure("AUDIO_HUFFMAN_SIZE", "MPQ Huffman output does not match the declared sector size");
        }
        return BytesResult::Success(std::move(intermediate));
    }
    return DecodeAdpcm(payload, expectedSize, (mask & 0x80u) != 0 ? 2u : 1u);
}

} // namespace War3Map::Mpq
