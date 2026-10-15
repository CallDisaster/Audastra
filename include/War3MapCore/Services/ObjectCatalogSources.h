#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"

#include <memory>

namespace War3Map { class DirectoryWorkspace; class MpqWorkspace; }
namespace War3Map::Services {
class CampaignSession;

// 三种当前地图来源最多选一种；战役为明确绑定的下层。覆盖字节须由宿主事先冻结。
// removals 只移除当前地图/会话成员，不能抹掉下层战役或游戏表。
struct ObjectCatalogSources {
    const MapArchiveSession* archive = nullptr;
    const MpqWorkspace* readOnlyArchive = nullptr;
    const DirectoryWorkspace* directory = nullptr;
    const CampaignSession* campaign = nullptr;
    std::filesystem::path gameRoot;
    std::span<const MemberReplacement> replacements;
    std::span<const std::string> removals;
    std::string overlayIdentity;
    // 受信任字符串表等固定来源；这些名字只查询游戏，不能由地图内容替换。
    std::span<const std::string> gameOnlyNames;
    // 可选的owned来源租约；必须自持有，不捕获可变Project/QObject裸指针。
    ObjectData::ObjectCatalogSourceVerifier overlayVerifier;
};

class ObjectCatalogSourceSnapshot {
public:
    [[nodiscard]] Result<std::optional<ObjectData::EffectiveCatalogFile>> Read(std::string_view name) const;
    [[nodiscard]] Status Verify() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    friend Result<ObjectCatalogSourceSnapshot> CaptureObjectCatalogSources(
        const ObjectCatalogSources&, std::span<const std::string>, std::size_t);
};

// 捕获固定物理名字，归档一次批量查证；不把 listfile 或目录枚举缺项当成缺失证明。
// 结果拥有字节和只读来源租约，不借用 request 中的文档或会话指针。
[[nodiscard]] Result<ObjectCatalogSourceSnapshot> CaptureObjectCatalogSources(
    const ObjectCatalogSources& sources, std::span<const std::string> names,
    std::size_t maximumFileBytes = 32U * 1024U * 1024U);
[[nodiscard]] Result<ObjectData::EffectiveObjectCatalog> LoadEffectiveObjectCatalog(
    const ObjectCatalogSources& sources, const ObjectData::CatalogLoadOptions& options = {});
[[nodiscard]] Status VerifyEffectiveObjectCatalog(const ObjectData::EffectiveObjectCatalog& expected,
    const ObjectCatalogSources& sources);

} // namespace War3Map::Services
