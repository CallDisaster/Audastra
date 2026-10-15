#pragma once

#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Resources/ResourceResolver.h"

namespace War3Map::ObjectData {

// 实际选中的整份数据表；bytes 为空仍是命中，不能因此使用低层文件。
struct EffectiveCatalogFile {
    Resources::ResourceLocation location;
    std::string containerIdentity;
    std::vector<std::byte> bytes;
};

struct ObjectCatalogInputIdentity {
    std::string logicalPath;
    // WESTRING 字符串属于受信任元数据，不接受地图/战役/会话覆盖。
    bool gameOnly = false;
    bool present = false;
    Resources::ResourceLocation location;
    std::string containerIdentity;
    std::string sha256;
    std::size_t byteSize = 0;
};

// 成功的 nullopt 仅表示所有来源均已明确缺失；读取/冲突/身份错误必须是 Failure。
using ObjectCatalogReader = std::function<Result<std::optional<EffectiveCatalogFile>>(std::string_view)>;
using ObjectCatalogSourceVerifier = std::function<Status()>;

// 目录仍使用唯一的 StandardObjectCatalog 解析、字段叠加和大小写规则。
// 此类型只增加实际来源与失效检查，不授予修改 SLK/profile 或保存资格。
class EffectiveObjectCatalog {
public:
    [[nodiscard]] static Result<std::vector<std::string>> InputNames(const CatalogLoadOptions& options = {});
    [[nodiscard]] static Result<EffectiveObjectCatalog> Capture(const ObjectCatalogReader& reader,
        const CatalogLoadOptions& options = {}, ObjectCatalogSourceVerifier verifier = {});
    // 先复核持有的来源租约，再比较当前整文件选择；同字节换层也拒绝。
    [[nodiscard]] Status Verify(const ObjectCatalogReader& current) const;
    [[nodiscard]] Status VerifyCapturedSources() const;
    [[nodiscard]] const StandardObjectCatalog& Catalog() const noexcept { return catalog_; }
    [[nodiscard]] std::span<const ObjectCatalogInputIdentity> Inputs() const noexcept { return inputs_; }
    [[nodiscard]] const ObjectCatalogInputIdentity* Input(std::string_view logicalPath) const;
    [[nodiscard]] bool HasNonGameInputs() const noexcept;

private:
    StandardObjectCatalog catalog_;
    std::vector<ObjectCatalogInputIdentity> inputs_;
    std::size_t maximumFileBytes_ = 0;
    ObjectCatalogSourceVerifier verifier_;
};

} // namespace War3Map::ObjectData
