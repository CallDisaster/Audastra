#pragma once

#include "Widgets/WarcraftTextPreview.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace War3Map { class MapDocument; }
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {

struct ObjectColourPreviewLimits {
    std::size_t maxSourceBytes = 16U * 1024U * 1024U;
    std::size_t maxRecords = 65536;
    std::size_t maxBodyBytes = 1024U * 1024U;
};

// 借用工程只在此显式准备入口；一次当前 WTS 快照，不读取地图文件或计算 SHA。
// 返回回调持有快照/索引，后续只核修订并复用实际 N1 解析，不复制/扫描全表。
[[nodiscard]] UiKit::WarcraftTextSourceResolver CaptureObjectColourPreviewSource(
    QtProject::EditorProject& project, const ObjectColourPreviewLimits& limits = {});

// 相同生产准备实现的 owned 来源入口；current 检查必须只读取廉价身份/修订。
// 缺项/重复/超预算/旧来源为 Unknown/Stale，不转码或改写原 TRIGSTR/GBK 来源。
[[nodiscard]] UiKit::WarcraftTextSourceResolver PrepareObjectColourPreviewSource(
    std::shared_ptr<const MapDocument> source, std::function<bool()> current,
    const ObjectColourPreviewLimits& limits = {});

} // namespace War3Map::Modules
