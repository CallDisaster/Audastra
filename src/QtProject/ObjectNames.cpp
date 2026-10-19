#include "ObjectNames.h"
#include "EditorProject.h"
#include "ProjectQuerySources.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QObject>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace {
ObjectPresentation QtPresentation(const War3Map::Services::NameResolution& resolved) {
    return {QString::fromUtf8(resolved.displayName), QString::fromUtf8(resolved.iconPath),
        QString::fromUtf8(resolved.reason), QString::fromUtf8(resolved.reference)};
}

template<class Document>
ObjectPresentation ResolvePresentationImpl(const War3Map::ObjectData::ObjectKey& key,
    const Document* document, const War3Map::ObjectData::StandardObjectCatalog* catalog,
    const War3Map::MapDocument* map) {
    War3Map::Services::ProjectSearchObjectSource source;
    source.type = key.type;
    source.state = War3Map::Services::ProjectSearchState::Missing;
    if (document) {
        source.records = document->Objects();
        source.state = document->IsStructurallySafe() && document->Type() == key.type
            ? War3Map::Services::ProjectSearchState::Ready : War3Map::Services::ProjectSearchState::Invalid;
    }
    War3Map::Services::NameResolutionContext context;
    context.objects = std::span(&source, 1);
    context.standardObjects = catalog;
    context.mapDocument = map;
    return QtPresentation(War3Map::Services::ResolveObjectName(context, key));
}

// 地图派生数据只复制一次，文字显示热路径不访问归档/磁盘、不逐参数重复解析 WTS。
class ProjectNameMapCache final : public QObject {
public:
    explicit ProjectNameMapCache(War3Map::QtProject::EditorProject* parent) : QObject(parent) {
        setObjectName("projectNameMapCache");
        connect(parent, &War3Map::QtProject::EditorProject::MapDocumentChanged, this, [this] { invalidated = true; });
        connect(parent, &War3Map::QtProject::EditorProject::TriggerDocumentChanged, this, [this] { invalidated = true; });
        connect(parent, &War3Map::QtProject::EditorProject::ObjectDocumentsReplaced, this, [this] { invalidated = true; });
        connect(parent, &War3Map::QtProject::EditorProject::WorldEditStringsChanged, this, [this] { invalidated = true; });
        connect(parent, &War3Map::QtProject::EditorProject::UiLibrariesChanged, this, [this] { invalidated = true; });
    }
    const War3Map::MapDocument* source = nullptr;
    const War3Map::MapData::MapDataEditSession* edits = nullptr;
    std::uint64_t revision = 0;
    std::uint64_t contentEpoch = 0;
    std::optional<War3Map::MapDocument> current;
    bool invalidated = true;
    std::array<const void*, 7> objectDocuments{}, nativeDocuments{};
    std::array<int, 7> objectUndoIndexes{};
    const void* triggerDocument = nullptr;
    const void* standardCatalog = nullptr;
    const void* uiCatalog = nullptr;
    std::unique_ptr<War3Map::Trigger::TriggerTypeSystem> types;
    const War3Map::Trigger::UiCatalog* typeCatalog = nullptr;
    std::string typeCatalogSha256;
    int triggerUndoIndex = -1;
    std::unordered_map<std::string, War3Map::Services::NameResolution> resolved;
    std::array<War3Map::Services::ProjectSearchObjectSource, 7> objects;
};

ProjectNameMapCache& CurrentNameCache(const War3Map::QtProject::EditorProject& project) {
    auto* owner = const_cast<War3Map::QtProject::EditorProject*>(&project);
    ProjectNameMapCache* cache = nullptr;
    for (auto* child : owner->children()) {
        if (child->objectName() == QLatin1String("projectNameMapCache")) {
            cache = dynamic_cast<ProjectNameMapCache*>(child);
            if (cache) break;
        }
    }
    if (!cache) cache = new ProjectNameMapCache(owner);
    const auto contentEpoch = project.ContentCacheEpoch();
    if (cache->contentEpoch != contentEpoch) {
        cache->contentEpoch = contentEpoch;
        cache->invalidated = true;
    }
    const auto revision = project.MapEdits() ? project.MapEdits()->Revision() : 0;
    const auto triggerUndoIndex = project.triggerUndoStack_ ? project.triggerUndoStack_->index() : -1;
    if (cache->triggerDocument != project.triggerDocument_.get() || cache->standardCatalog != project.standardCatalog_.get()
        || cache->uiCatalog != project.uiCatalog_.get() || cache->triggerUndoIndex != triggerUndoIndex) cache->invalidated = true;
    for (std::size_t i = 0; i < cache->objectDocuments.size(); ++i) {
        const auto undoIndex = project.objectUndoStacks_[i] ? project.objectUndoStacks_[i]->index() : -1;
        if (cache->objectDocuments[i] != project.objectDocuments_[i].get() || cache->nativeDocuments[i] != project.nativeObjectDocuments_[i].get()
            || cache->objectUndoIndexes[i] != undoIndex) cache->invalidated = true;
        cache->objectDocuments[i] = project.objectDocuments_[i].get();
        cache->nativeDocuments[i] = project.nativeObjectDocuments_[i].get();
        cache->objectUndoIndexes[i] = undoIndex;
    }
    if (cache->invalidated || cache->source != project.mapDocument_.get() || cache->edits != project.MapEdits()
        || cache->revision != revision) {
        // 类型规则只依赖函数目录。名称、变量与地图覆盖仍按真实内容代次重建；
        // 不因纯参数修改重复解析同一份目录，也不把缓存当成编辑资格。
        const auto* catalog = project.uiCatalog_.get();
        const bool reuseTypes = catalog && cache->types && cache->typeCatalog == catalog
            && !catalog->Sha256().empty() && cache->typeCatalogSha256 == catalog->Sha256();
        if (!reuseTypes) {
            auto nextSha256 = catalog ? catalog->Sha256() : std::string{};
            auto nextTypes = catalog ? std::make_unique<War3Map::Trigger::TriggerTypeSystem>(*catalog) : nullptr;
            cache->types = std::move(nextTypes);
            cache->typeCatalogSha256 = std::move(nextSha256);
            cache->typeCatalog = catalog;
        }
        cache->source = project.mapDocument_.get();
        cache->edits = project.MapEdits();
        cache->revision = revision;
        cache->invalidated = false;
        cache->triggerDocument = project.triggerDocument_.get();
        cache->standardCatalog = project.standardCatalog_.get();
        cache->uiCatalog = project.uiCatalog_.get();
        cache->triggerUndoIndex = triggerUndoIndex;
        cache->resolved.clear();
        cache->objects = War3Map::QtProject::CaptureProjectObjectSources(project);
        cache->current.reset();
        if (project.MapEdits()) cache->current = project.MapEdits()->Snapshot();
    }
    return *cache;
}

const War3Map::MapDocument* CurrentNameMap(const War3Map::QtProject::EditorProject& project) {
    auto& cache = CurrentNameCache(project);
    return cache.current ? &*cache.current : project.mapDocument_.get();
}
template<class Document>
ObjectInheritanceChain ResolveInheritanceImpl(const Document& document,
    const War3Map::ObjectData::ObjectKey& object, const War3Map::ObjectData::StandardObjectCatalog* catalog) {
    using namespace War3Map::ObjectData;
    ObjectInheritanceChain result;
    if (object.type != document.Type()) {
        result.reason = QObject::tr("继承对象类型与文档不一致");
        return result;
    }
    const ObjectRecord* current = document.FindObject(object);
    std::unordered_set<std::string> visited;
    while (current) {
        if (!visited.insert(current->key.rawcode).second) {
            result.reason = QObject::tr("继承链循环：%1").arg(QString::fromStdString(current->key.rawcode));
            return result;
        }
        result.records.push_back(current);
        const std::string parent = current->parentRawcode.value_or(current->key.rawcode);
        if (parent.empty() || parent == current->key.rawcode) {
            // 原版对象的地图覆盖使用 parent=self，须由同类型原版目录确认终点。
            result.standardBase = catalog ? catalog->Find(object.type, current->key.rawcode) : nullptr;
            if (result.standardBase) return result;
            result.catalogUnavailable = catalog == nullptr;
            result.reason = catalog ? (current->parentRawcode && parent == current->key.rawcode
                ? QObject::tr("继承链循环：%1").arg(QString::fromStdString(current->key.rawcode))
                : QObject::tr("继承链没有原版终点：%1").arg(QString::fromStdString(current->key.rawcode)))
                : QObject::tr("原版目录未加载，无法确认继承终点：%1").arg(QString::fromStdString(current->key.rawcode));
            return result;
        }
        if (visited.contains(parent)) {
            result.reason = QObject::tr("继承链循环：%1").arg(QString::fromStdString(parent));
            return result;
        }
        if (const auto* next = document.FindObject({object.type, parent})) {
            current = next;
            continue;
        }
        result.standardBase = catalog ? catalog->Find(object.type, parent) : nullptr;
        if (result.standardBase) return result;
        result.catalogUnavailable = catalog == nullptr;
        result.reason = catalog ? QObject::tr("继承链缺少父对象：%1").arg(QString::fromStdString(parent))
            : QObject::tr("原版目录未加载，无法确认父对象：%1").arg(QString::fromStdString(parent));
        return result;
    }
    result.reason = QObject::tr("继承链的选中对象不存在：%1").arg(QString::fromStdString(object.rawcode));
    return result;
}
} // namespace

War3Map::Services::NameResolution ResolveProjectName(const War3Map::QtProject::EditorProject& project,
    std::string_view rawValue, std::string_view uiType) {
    auto& cache = CurrentNameCache(project);
    const std::string key = "name:" + std::to_string(uiType.size()) + ":" + std::string(uiType) + std::string(rawValue);
    if (const auto found = cache.resolved.find(key); found != cache.resolved.end()) return found->second;
    War3Map::Services::NameResolutionContext context;
    context.objects = cache.objects;
    context.standardObjects = project.standardCatalog_.get();
    context.mapDocument = CurrentNameMap(project);
    context.triggers = project.triggerDocument_.get();
    context.types = cache.types.get();
    auto resolved = War3Map::Services::ResolveName(context, rawValue, uiType);
    // 防止海量不同字面字符串让展示缓存无界增长；缓存只保存引用身份。
    if ((rawValue.size() == 4 || rawValue.starts_with("gg_") || rawValue.starts_with("udg_") || rawValue.starts_with("TRIGSTR_"))
        && cache.resolved.size() < 16384) cache.resolved.emplace(key, resolved);
    return resolved;
}

std::optional<std::string> ResolveProjectStringText(const War3Map::QtProject::EditorProject& project,
    std::string_view rawValue) {
    if (!War3Map::Services::StringReferenceId(rawValue)) return std::nullopt;
    const auto* map = CurrentNameMap(project);
    if (!map) return std::nullopt;
    auto resolved = map->ResolveString(rawValue);
    if (!resolved.Succeeded() || !resolved.value) return std::nullopt;
    return std::move(*resolved.value);
}

ObjectPresentation ResolveProjectObjectPresentation(const War3Map::QtProject::EditorProject& project,
    const War3Map::ObjectData::ObjectKey& key) {
    auto& cache = CurrentNameCache(project);
    const auto cacheKey = "object:" + std::to_string(static_cast<int>(key.type)) + ":" + key.rawcode;
    if (const auto found = cache.resolved.find(cacheKey); found != cache.resolved.end()) return QtPresentation(found->second);
    War3Map::Services::NameResolutionContext context;
    context.objects = cache.objects;
    context.standardObjects = project.standardCatalog_.get();
    context.mapDocument = CurrentNameMap(project);
    context.triggers = project.triggerDocument_.get();
    context.types = cache.types.get();
    auto resolved = War3Map::Services::ResolveObjectName(context, key);
    if (cache.resolved.size() < 16384) cache.resolved.emplace(cacheKey, resolved);
    return QtPresentation(resolved);
}

War3Map::Trigger::ReferencePresentation ResolveProjectReferencePresentation(
    const War3Map::QtProject::EditorProject& project, std::string_view rawValue, std::string_view uiType) {
    const auto name = ResolveProjectName(project, rawValue, uiType);
    War3Map::Trigger::ReferencePresentation result;
    result.rawValue = name.rawValue;
    result.displayName = name.displayName;
    result.iconPath = name.iconPath;
    result.issue = name.reason;
    result.reference = name.reference;
    result.state = War3Map::Services::NameResolutionStateName(name.state);
    result.candidates = name.candidates;
    return result;
}
ObjectPresentation ResolveObjectPresentation(const War3Map::ObjectData::ObjectKey& key,
    const War3Map::ObjectData::LniObjectDocument* document, const War3Map::ObjectData::StandardObjectCatalog* catalog,
    const War3Map::MapDocument* map) {
    return ResolvePresentationImpl(key, document, catalog, map);
}

ObjectPresentation ResolveNativeObjectPresentation(const War3Map::ObjectData::ObjectKey& key,
    const War3Map::ObjectData::NativeObjectDocument* document, const War3Map::ObjectData::StandardObjectCatalog* catalog,
    const War3Map::MapDocument* map) {
    return ResolvePresentationImpl(key, document, catalog, map);
}

ObjectInheritanceChain ResolveObjectInheritance(const War3Map::ObjectData::LniObjectDocument& document,
    const War3Map::ObjectData::ObjectKey& object, const War3Map::ObjectData::StandardObjectCatalog* catalog) {
    return ResolveInheritanceImpl(document, object, catalog);
}

ObjectInheritanceChain ResolveNativeObjectInheritance(const War3Map::ObjectData::NativeObjectDocument& document,
    const War3Map::ObjectData::ObjectKey& object, const War3Map::ObjectData::StandardObjectCatalog* catalog) {
    if (!document.IsStructurallySafe()) {
        ObjectInheritanceChain result; result.reason = QObject::tr("原生对象文档结构不安全"); return result;
    }
    return ResolveInheritanceImpl(document, object, catalog);
}

std::optional<std::string> ObjectFieldValue(const ObjectInheritanceChain& chain,
    std::initializer_list<std::string_view> fieldIds) {
    const auto matches = [&](std::string field) {
        std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::find(fieldIds.begin(), fieldIds.end(), field) != fieldIds.end();
    };
    for (const auto* record : chain.records)
        for (const auto& modification : record->modifications)
            if (matches(modification.key.fieldId)) return War3Map::ObjectData::ValueToDisplayString(modification.value);
    if (chain.IsComplete() && chain.standardBase)
        for (const auto field : fieldIds)
            if (const auto value = chain.standardBase->Field(field); !value.empty()) return std::string(value);
    return std::nullopt;
}

std::optional<War3Map::ObjectData::ObjectType> PlacementType(const War3Map::QtProject::EditorProject& project,
    const std::string& rawcode, War3Map::ObjectData::ObjectType first, War3Map::ObjectData::ObjectType second) {
    const auto known = [&](War3Map::ObjectData::ObjectType type) {
        const auto index = static_cast<std::size_t>(type);
        const auto* lni = project.objectDocuments_[index].get();
        const auto* native = lni ? nullptr : project.nativeObjectDocuments_[index].get();
        return (lni && lni->FindObject({type, rawcode})) ||
            (native && native->IsStructurallySafe() && native->Type() == type && native->FindObject({type, rawcode})) ||
            (project.standardCatalog_ && project.standardCatalog_->Find(type, rawcode));
    };
    const bool a = known(first), b = known(second);
    if (a == b) return std::nullopt; // 未知或跨类型歧义均不猜测。
    return a ? first : second;
}

ObjectPresentation PlacementPresentation(const War3Map::QtProject::EditorProject& project,
    const War3Map::MapDocument& snapshot, const std::string& rawcode,
    War3Map::ObjectData::ObjectType first, War3Map::ObjectData::ObjectType second) {
    if (first == War3Map::ObjectData::ObjectType::Unit && rawcode == "sloc")
        return {QObject::tr("起始位置"), {}, {}, {}};
    const auto type = PlacementType(project, rawcode, first, second);
    if (!type) return {QString::fromUtf8(rawcode), {}, QObject::tr("对象类型尚未确定"), {}};
    const auto index = static_cast<std::size_t>(*type);
    if (!project.objectDocuments_[index] && project.nativeObjectDocuments_[index])
        return ResolveNativeObjectPresentation({*type, rawcode}, project.nativeObjectDocuments_[index].get(),
            project.standardCatalog_.get(), &snapshot);
    return ResolveObjectPresentation({*type, rawcode}, project.objectDocuments_[index].get(),
        project.standardCatalog_.get(), &snapshot);
}
