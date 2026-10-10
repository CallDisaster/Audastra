#include "War3MapCore/MapData/MapAuxiliary.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxRecords = 100000u;
constexpr std::size_t kMaxStringBytes = 1024u * 1024u;

Result<std::vector<std::byte>> Failure(std::string code, std::string message) {
    return Result<std::vector<std::byte>>::Failure("MapData.WtsWrite", std::move(code), std::move(message));
}

std::string_view Text(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool SameEntries(std::span<const StringTableEntry> left, std::span<const StringTableEntry> right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].id != right[i].id || left[i].value != right[i].value) return false;
    return true;
}

void Append(std::string& target, std::string_view value) {
    if (value.size() > kMaxBytes - target.size())
        throw std::length_error("WTS output budget");
    target.append(value);
}

} // namespace

Result<std::uint32_t> StringTableDocument::AddString(std::string value) {
    if (value.size() > kMaxStringBytes || value.find('\0') != std::string::npos)
        return Result<std::uint32_t>::Failure("MapData.WtsWrite", "WTS_VALUE_INVALID",
            "字符串超过 1 MiB 或含有 NUL");
    if (entries.size() >= kMaxRecords)
        return Result<std::uint32_t>::Failure("MapData.WtsWrite", "WTS_COUNT_EXCEEDED", "字符串数量超过解析预算");
    std::uint32_t largest = 0;
    std::unordered_set<std::uint32_t> seen;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.id).second)
            return Result<std::uint32_t>::Failure("MapData.WtsWrite", "WTS_DUPLICATE_ID",
                "已有字符串表包含重复 ID，不能新增字符串");
        largest = (std::max)(largest, entry.id);
    }
    if (largest == (std::numeric_limits<std::uint32_t>::max)())
        return Result<std::uint32_t>::Failure("MapData.WtsWrite", "WTS_ID_EXHAUSTED", "TRIGSTR 编号已经耗尽");
    entries.push_back({largest + 1, std::move(value), 0, 0, 0});
    return Result<std::uint32_t>::Success(largest + 1);
}

Result<std::vector<std::byte>> StringTableDocument::Serialize() const {
    if (originalBytes.size() > kMaxBytes || entries.size() > kMaxRecords)
        return Failure("WTS_BUDGET_EXCEEDED", "字符串表超过输出预算");
    const auto old = ParseStringTable(originalBytes);
    if (!old.Succeeded() || !old.value)
        return Failure("WTS_ORIGINAL_INVALID", "原始字符串表无法完整解析，不能修改或写出");
    if (SameEntries(entries, old.value->entries))
        return Result<std::vector<std::byte>>::Success(originalBytes);

    const auto& originals = old.value->entries;
    if (entries.size() < originals.size())
        return Failure("WTS_EDIT_UNSUPPORTED", "本轮不能删除或重排已有字符串 ID");
    std::unordered_set<std::uint32_t> seen;
    std::uint32_t largest = 0;
    for (const auto& entry : originals) {
        if (!seen.insert(entry.id).second)
            return Failure("WTS_DUPLICATE_ID", "原始字符串表含重复 ID，不能安全修改");
        largest = (std::max)(largest, entry.id);
    }
    for (std::size_t i = 0; i < originals.size(); ++i) {
        if (entries[i].id != originals[i].id)
            return Failure("WTS_EDIT_UNSUPPORTED", "已有字符串 ID 的顺序或编号改变");
    }
    for (std::size_t i = originals.size(); i < entries.size(); ++i) {
        if (largest == (std::numeric_limits<std::uint32_t>::max)() || entries[i].id != largest + 1)
            return Failure("WTS_ID_INVALID", "新增字符串必须按现有最大 ID 递增分配");
        largest = entries[i].id;
    }
    try {
        const auto source = Text(originalBytes);
        std::string output;
        output.reserve(source.size());
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < originals.size(); ++i) {
            const auto& base = originals[i];
            const auto& value = entries[i].value;
            if (value.size() > kMaxStringBytes || value.find('\0') != std::string::npos)
                return Failure("WTS_VALUE_INVALID", "字符串超过 1 MiB 或含有 NUL");
            if (base.contentOffset < cursor || base.contentOffset > source.size()
                || base.contentLength > source.size() - base.contentOffset)
                return Failure("WTS_SOURCE_SPAN_INVALID", "原始字符串正文跨度不可信");
            Append(output, source.substr(cursor, base.contentOffset - cursor));
            Append(output, value);
            cursor = base.contentOffset + base.contentLength;
        }
        Append(output, source.substr(cursor));
        const auto newline = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        for (std::size_t i = originals.size(); i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.value.size() > kMaxStringBytes || entry.value.find('\0') != std::string::npos)
                return Failure("WTS_VALUE_INVALID", "新增字符串超过 1 MiB 或含有 NUL");
            if (!output.empty() && output.back() != '\n') Append(output, newline);
            Append(output, "STRING " + std::to_string(entry.id));
            Append(output, newline);
            Append(output, "{");
            Append(output, newline);
            Append(output, entry.value);
            Append(output, newline);
            Append(output, "}");
            Append(output, newline);
        }
        const auto* first = reinterpret_cast<const std::byte*>(output.data());
        std::vector<std::byte> bytes(first, first + output.size());
        const auto reparsed = ParseStringTable(bytes);
        if (!reparsed.Succeeded() || !reparsed.value || !SameEntries(entries, reparsed.value->entries))
            return Failure("WTS_OUTPUT_INVALID", "写出的字符串表不能按目标内容重新解析");
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const std::bad_alloc&) {
        return Failure("WTS_ALLOCATION_FAILED", "字符串表写出内存分配失败");
    } catch (const std::length_error&) {
        return Failure("WTS_BUDGET_EXCEEDED", "字符串表超过输出容量");
    }
}

} // namespace War3Map::MapData
