#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace War3Map::Render {

// 一帧姿势相对其基准帧可能变化的实例（严格升序），来自编辑器的变化清单。
// 插件记录最近几帧；后台复制与渲染线程上传据此沿记录求出任一较早版本到当前版本的变化并集，
// 链断、超深或场景版本不同时调用方回到整份处理。记录发布后不再修改，可跨线程共享。
struct SkinPoseChangeRecord {
    std::uint64_t sceneVersion = 0, contentVersion = 0, baseContentVersion = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> instances;
};
using SkinPoseChangeHistory = std::vector<SkinPoseChangeRecord>; // 按加入先后，旧在前
inline constexpr std::size_t kSkinPoseChangeHistory = 8;

// from → to（同一 sceneVersion）的变化实例并集，升序去重写入 out；out 为空指针时只判断能否求出。
// from == to 为空集。找不到某一段记录时返回 false，out 内容无意义。
[[nodiscard]] inline bool CollectSkinPoseChanges(const SkinPoseChangeHistory* history, std::uint64_t sceneVersion,
    std::uint64_t from, std::uint64_t to, std::vector<std::uint32_t>* out) {
    if (out) out->clear();
    if (!history || !sceneVersion || !from || from > to) return false;
    std::array<const std::vector<std::uint32_t>*, kSkinPoseChangeHistory> parts{};
    std::size_t count = 0;
    for (auto version = to; version != from;) {
        const auto found = std::find_if(history->rbegin(), history->rend(), [&](const SkinPoseChangeRecord& record) {
            return record.sceneVersion == sceneVersion && record.contentVersion == version;
        });
        if (found == history->rend() || !found->instances || found->baseContentVersion < from
            || found->baseContentVersion >= version || count == parts.size()) return false;
        parts[count++] = found->instances.get();
        version = found->baseContentVersion;
    }
    if (!out || !count) return true;
    if (count == 1) { *out = *parts[0]; return true; }
    std::size_t total = 0;
    for (std::size_t k = 0; k < count; ++k) total += parts[k]->size();
    out->reserve(total);
    for (std::size_t k = 0; k < count; ++k) out->insert(out->end(), parts[k]->begin(), parts[k]->end());
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return true;
}

// 加入一条记录得到新的历史（只留最近 kSkinPoseChangeHistory 条）；旧历史对象保持不变，其他线程可继续读。
[[nodiscard]] inline std::shared_ptr<const SkinPoseChangeHistory> AppendSkinPoseChanges(
    const std::shared_ptr<const SkinPoseChangeHistory>& history, SkinPoseChangeRecord record) {
    auto next = std::make_shared<SkinPoseChangeHistory>();
    if (history) {
        const auto keep = (std::min)(history->size(), kSkinPoseChangeHistory - 1);
        next->assign(history->end() - static_cast<std::ptrdiff_t>(keep), history->end());
    }
    next->push_back(std::move(record));
    return next;
}

} // namespace War3Map::Render
