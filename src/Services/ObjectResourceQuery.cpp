#include "ObjectResourceQueryDetail.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>

namespace War3Map::Services {
using namespace ObjectResourceDetail;

std::optional<ObjectResourceField> DescribeObjectResourceField(const ObjectData::ObjectFieldDefinition& definition) {
    auto type = Text::LowerAscii(definition.valueType);
    const bool list = definition.isList || type.ends_with("list");
    if (type.ends_with("list")) type.resize(type.size() - 4);
    if (type == "model") return ObjectResourceField{ObjectResourceKind::Model, list, true};
    if (type == "icon") return ObjectResourceField{ObjectResourceKind::Icon, list, true};
    if (type == "texture" || type == "pathingtexture") return ObjectResourceField{ObjectResourceKind::Texture, list, true};
    if (type == "sound" || type == "soundfile") return ObjectResourceField{ObjectResourceKind::SoundFile, list, true};
    if (type == "soundlabel") return ObjectResourceField{ObjectResourceKind::SoundLabel, list, false};
    if (type == "unitsound") return ObjectResourceField{ObjectResourceKind::UnitSoundSet, list, false};
    return {};
}
std::string_view ObjectResourceKindName(ObjectResourceKind kind) noexcept {
    switch (kind) {
    case ObjectResourceKind::Model: return "model";
    case ObjectResourceKind::Icon: return "icon";
    case ObjectResourceKind::Texture: return "texture";
    case ObjectResourceKind::SoundFile: return "sound-file";
    case ObjectResourceKind::SoundLabel: return "sound-label";
    case ObjectResourceKind::UnitSoundSet: return "unit-sound-set";
    }
    return "unknown";
}
std::string_view ObjectResourceOriginName(ObjectResourceOrigin origin) noexcept {
    switch (origin) {
    case ObjectResourceOrigin::Object: return "object";
    case ObjectResourceOrigin::Import: return "import";
    case ObjectResourceOrigin::Game: return "game";
    }
    return "unknown";
}

Result<std::shared_ptr<const ObjectResourceCatalog>> ObjectResourceDetail::Builder::Finish() {
    using Return = Result<std::shared_ptr<const ObjectResourceCatalog>>;
    if (!Valid()) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_LIMITS", "资源索引需要修订身份和有效的有界参数。");
    if (cancelled_) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_CANCELLED", "资源索引已取消。");
    std::string identity;
    IdentityPart(identity, catalog_->revision);
    IdentityPart(identity, ObjectResourceOriginName(catalog_->origin));
    IdentityPart(identity, catalog_->complete ? "complete" : "partial");
    for (const auto& row : catalog_->rows) {
        IdentityPart(identity, row.id); IdentityPart(identity, row.value); IdentityPart(identity, row.previewPath);
        IdentityPart(identity, row.title); IdentityPart(identity, row.sourceId); IdentityPart(identity, row.state);
        IdentityPart(identity, ObjectResourceKindName(row.kind));
        IdentityPart(identity, row.fieldId); IdentityPart(identity, row.customObject ? "custom" : "standard");
        IdentityPart(identity, row.directory ? "directory" : "value");
        IdentityPart(identity, row.sourceVerified ? "verified" : "listed");
        if (row.object) { IdentityPart(identity, ObjectData::ToString(row.object->type)); IdentityPart(identity, row.object->rawcode); }
    }
    const auto hash = Sha256Text(identity);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    catalog_->identity = *hash.value;
    return Return::Success(std::move(catalog_));
}

Result<ObjectResourcePage> QueryObjectResources(const ObjectResourceCatalog& catalog,
    const ObjectResourceQuery& query, const std::optional<ObjectResourceCursor>& cursor) {
    using Return = Result<ObjectResourcePage>;
    if (query.expectedRevision.empty() || query.expectedRevision != catalog.revision)
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_STALE", "资源候选属于旧修订，请重新打开选择器。");
    if (query.limit == 0 || query.limit > 128 || query.maxExamined == 0 || query.maxExamined > 8192
        || query.search.size() > 1024 || ObjectResourceKindName(query.kind) == "unknown"
        || (query.objectType && static_cast<unsigned>(*query.objectType) > 6))
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_QUERY_LIMITS", "资源查询参数超出允许范围。");
    std::string directory;
    if (query.directory && !query.directory->empty()) {
        auto canonical = Resources::CanonicalImportPath(*query.directory);
        if (!canonical.Succeeded()) return {std::nullopt, std::move(canonical.diagnostics)};
        directory = Text::LowerAscii(Slash(*canonical.value));
        while (directory.ends_with('/')) directory.pop_back();
    }
    const auto search = Text::LowerAscii(query.search);
    std::string signature;
    IdentityPart(signature, ObjectResourceKindName(query.kind)); IdentityPart(signature, search);
    IdentityPart(signature, query.objectType ? ObjectData::ToString(*query.objectType) : "all");
    IdentityPart(signature, query.directory ? "directory" : "recursive"); IdentityPart(signature, directory);
    const auto hash = Sha256Text(signature);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    if (cursor && (cursor->catalogIdentity != catalog.identity || cursor->queryIdentity != *hash.value
        || cursor->nextIndex > catalog.rows.size()))
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_CURSOR_STALE", "分页游标不属于本次目录或查询。");
    ObjectResourcePage result;
    result.revision = catalog.revision; result.catalogIdentity = catalog.identity;
    result.knownRows = catalog.rows.size(); result.diagnostics = catalog.diagnostics;
    std::size_t index = cursor ? cursor->nextIndex : 0;
    for (; index < catalog.rows.size() && result.examined < query.maxExamined && result.rows.size() < query.limit; ++index) {
        ++result.examined;
        const auto& row = catalog.rows[index];
        const bool compatibleImage = catalog.origin != ObjectResourceOrigin::Object
            && query.kind == ObjectResourceKind::Icon && row.kind == ObjectResourceKind::Texture;
        if (row.kind != query.kind && !compatibleImage) continue;
        if (query.objectType && (!row.object || row.object->type != *query.objectType)) continue;
        if (!query.directory && row.directory) continue;
        if (query.directory) {
            const auto path = Text::LowerAscii(Slash(row.value));
            const auto slash = path.rfind('/');
            const auto parent = slash == std::string::npos ? std::string{} : path.substr(0, slash);
            if (parent != directory) continue;
        }
        if (!search.empty() && Text::LowerAscii(row.title).find(search) == std::string::npos
            && Text::LowerAscii(row.value).find(search) == std::string::npos
            && (!row.object || Text::LowerAscii(row.object->rawcode).find(search) == std::string::npos)) continue;
        result.rows.push_back(row);
    }
    if (index < catalog.rows.size()) result.next = ObjectResourceCursor{catalog.identity, *hash.value, index};
    result.scanBudgetExhausted = result.next && result.examined == query.maxExamined;
    result.complete = !result.next && catalog.complete;
    return Return::Success(std::move(result));
}
} // namespace War3Map::Services
