#include "ObjectViewEntries.h"

#include "EditorProject.h"
#include "ProjectObjectCatalog.h"
#include "ObjectModuleShared.h"
#include "ObjectClassification.h"
#include "ObjectNames.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <QObject>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::QtApp {

using Modules::ObjectDetail::Kind;
using Modules::ObjectDetail::U8;

namespace {
ObjectViewClassification ClassificationForChain(const ObjectInheritanceChain& chain,
    ObjectData::ObjectType type) {
    ObjectViewClassification result;
    if (!chain.IsComplete() || !chain.standardBase) {
        result.issue = chain.reason.isEmpty() ? QObject::tr("原版分类来源未加载") : chain.reason;
        result.folders = {QObject::tr("分类未确认")};
        return result;
    }
    // ClassifyCustom 已是物编的共用分类入口。使用已确认的原版父对象身份，
    // 避免自定义 rawcode 的大小写错误地改变英雄类别。
    result.folders = Modules::ObjectDetail::ClassifyCustom(chain, type, chain.standardBase->key.rawcode);
    if (type == ObjectData::ObjectType::Unit) result.hero = chain.standardBase->category == "hero";
    return result;
}

ObjectViewEntry StandardEntry(const ObjectData::StandardObjectEntry& object, const ObjectData::StandardObjectCatalog* catalog,
    const ObjectData::EffectiveObjectCatalog* effective = nullptr) {
    ObjectViewEntry entry;
    entry.kind = Kind(object.key.type);
    entry.id = U8(object.key.rawcode);
    Services::NameResolutionContext context;
    context.standardObjects = catalog;
    const auto shown = Services::ResolveObjectName(context, object.key);
    entry.name = U8(shown.displayName);
    entry.iconPath = U8(shown.iconPath);
    entry.race = U8(object.race);
    entry.category = U8(object.category);
    entry.description = QObject::tr("Warcraft III · 基础对象");
    const auto tip = object.fields.find("ubertip");
    if (tip != object.fields.end()) entry.description = U8(tip->second);
    entry.details = {object.nativeOverridesSupported ? QObject::tr("基础数据：游戏资源")
        : QObject::tr("基础数据含地图/战役 SLK · 当前只读"), entry.iconPath};
    auto files = object.sourceFiles;
    for (const auto& file : {object.definitionSourceFile, object.nameSourceFile, object.iconSourceFile})
        if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) files.push_back(file);
    if (effective) for (const auto& file : files) {
        const auto* input = effective->Input(file);
        if (input) entry.details.push_back(QObject::tr("%1 → %2 · %3")
            .arg(U8(file), U8(input->location.sourceId), U8(input->location.sourcePath)));
    }
    if (!shown.reason.empty()) entry.details.push_back(U8(shown.reason));
    return entry;
}

template <typename Document>
ObjectViewEntry MapEntry(const QtProject::EditorProject& project,
    const Document& document, const ObjectData::ObjectRecord& object, const MapDocument* names = nullptr) {
    ObjectViewEntry entry;
    entry.kind = Kind(object.key.type);
    entry.id = U8(object.key.rawcode);
    const auto chain = [&] {
        if constexpr (std::is_same_v<Document, ObjectData::NativeObjectDocument>)
            return ResolveNativeObjectInheritance(document, object.key, project.standardCatalog_.get());
        else return ResolveObjectInheritance(document, object.key, project.standardCatalog_.get());
    }();
    const auto* base = chain.standardBase;
    // 名称可能是 TRIGSTR 引用，按当前地图字符串表解析（与实例表同一解析）。
    const auto presentation = [&] {
        if constexpr (std::is_same_v<Document, ObjectData::NativeObjectDocument>)
            return ResolveNativeObjectPresentation(object.key, &document, project.standardCatalog_.get(),
                names ? names : project.mapDocument_.get());
        else return ResolveObjectPresentation(object.key, &document, project.standardCatalog_.get(),
            names ? names : project.mapDocument_.get());
    }();
    entry.name = presentation.name;
    entry.iconPath = presentation.iconPath;
    if (const auto race = ObjectFieldValue(chain, {"urac", "arac", "frac", "grac", "race"})) entry.race = U8(*race);
    if (const auto category = ObjectFieldValue(chain, {"dcat", "bcat", "icla", "category", "class"})) entry.category = U8(*category);
    else if (base) entry.category = U8(base->category);
    entry.description = (object.parentRawcode ? QObject::tr("自定义 · 修改了 %1 个字段") : QObject::tr("已修改的标准对象 · %1 个字段"))
        .arg(object.modifications.size());
    entry.details = {QObject::tr("父对象：%1").arg(object.parentRawcode ? U8(*object.parentRawcode) : entry.id), entry.iconPath};
    if (!presentation.nameReference.isEmpty()) entry.details.push_back(QObject::tr("来自地图字符串表：%1").arg(presentation.nameReference));
    if (!presentation.issue.isEmpty()) entry.details.push_back(presentation.issue);
    if (base) entry.details.push_back(QObject::tr("原版基础：%1 · %2").arg(U8(base->displayName), U8(base->key.rawcode)));
    if (!chain.IsComplete()) entry.details.push_back(chain.reason);
    return entry;
}

struct ObjectMapIndex {
    const ObjectData::LniObjectDocument* document = nullptr;
    const ObjectData::NativeObjectDocument* native = nullptr;
    std::span<const ObjectData::ObjectRecord> source;
    std::vector<const ObjectData::ObjectRecord*> ordered;
    std::unordered_map<std::string, const ObjectData::ObjectRecord*> byId;
    std::size_t scanned = 0;
};

// 随工程 QObject 销毁；不持有 IR，也不跨线程或跨来源沿用裸指针。
class ObjectViewIndexCache final : public QObject {
public:
    explicit ObjectViewIndexCache(QObject* parent) : QObject(parent) {}
    std::array<ObjectMapIndex, 7> maps;
    std::array<std::vector<const ObjectData::StandardObjectEntry*>, 7> standards;
    const ObjectData::StandardObjectCatalog* catalog = nullptr;
    const MapData::MapDataEditSession* mapEdits = nullptr;
    const MapDocument* mapDocument = nullptr;
    std::uint64_t mapRevision = 0;
    std::optional<MapDocument> names;
    QString namesIssue;
    bool namesPrepared = false;
    std::uint64_t generation = 0, revision = 0;
    std::size_t standardScanned = 0, estimatedBytes = 4096, byteBudget = 64 * 1024 * 1024;
    QString issue;
    std::array<QString, 7> typeIssues;
    QString standardIssue;

    bool Matches(const QtProject::EditorProject& project) const {
        if (generation != project.editRevision_.Generation() || revision != project.editRevision_.Revision()
            || catalog != project.standardCatalog_.get() || mapEdits != project.mapEdits_.get()
            || mapDocument != project.mapDocument_.get()
            || mapRevision != (project.mapEdits_ ? project.mapEdits_->Revision() : 0)) return false;
        for (std::size_t type = 0; type < maps.size(); ++type) {
            const auto* document = project.objectDocuments_[type].get();
            const auto* native = document ? nullptr : project.nativeObjectDocuments_[type].get();
            const auto records = document ? document->Objects() : native ? native->Objects() : std::span<const ObjectData::ObjectRecord>{};
            if (maps[type].document != document || maps[type].native != native
                || maps[type].source.data() != records.data() || maps[type].source.size() != records.size()) return false;
        }
        return true;
    }
    void Reset(const QtProject::EditorProject& project) {
        maps = {}; standards = {}; typeIssues = {}; standardIssue.clear();
        names.reset(); namesIssue.clear(); namesPrepared = false;
        standardScanned = 0; estimatedBytes = 4096; issue.clear();
        generation = project.editRevision_.Generation(); revision = project.editRevision_.Revision(); catalog = project.standardCatalog_.get();
        mapEdits = project.mapEdits_.get(); mapDocument = project.mapDocument_.get();
        mapRevision = mapEdits ? mapEdits->Revision() : 0;
        for (std::size_t type = 0; type < maps.size(); ++type) {
            auto& index = maps[type]; index.document = project.objectDocuments_[type].get();
            index.native = index.document ? nullptr : project.nativeObjectDocuments_[type].get();
            index.source = index.document ? index.document->Objects() : index.native ? index.native->Objects()
                : std::span<const ObjectData::ObjectRecord>{};
        }
    }
    bool Charge(std::size_t bytes, std::size_t maximum) {
        if (estimatedBytes > maximum || bytes > maximum - estimatedBytes) {
            issue = QObject::tr("资源目录索引超过内存预算，未截断地图物体或回落标准来源"); return false;
        }
        estimatedBytes += bytes; return true;
    }
    bool PrepareNames(std::size_t maximumBytes) {
        if (namesPrepared) { issue = namesIssue; return issue.isEmpty(); }
        namesPrepared = true;
        if (!mapEdits) return true; // 使用只读基线，无额外复制。
        // WTS 原字节、解析正文及记录容器共享预算；在复制前留出解析扩容余量。
        const auto remaining = estimatedBytes < maximumBytes ? maximumBytes - estimatedBytes : 0;
        auto snapshot = mapEdits->Snapshot(MapDataKind::Strings, remaining / 16);
        if (!snapshot.Succeeded()) {
            namesIssue = issue = snapshot.diagnostics.empty() ? QObject::tr("当前字符串来源不可用")
                : U8(snapshot.diagnostics.front().message);
            return false;
        }
        auto candidate = std::move(*snapshot.value);
        const auto& member = candidate.Member(MapDataKind::Strings);
        const auto add = [&](std::size_t bytes) { return Charge(bytes, maximumBytes); };
        bool charged = add(sizeof(MapDocument) + member.sources.capacity() * sizeof(MapArchiveMember)
            + member.diagnostics.capacity() * sizeof(Diagnostic));
        for (const auto& source : member.sources)
            if (charged) charged = add(source.name.capacity() + source.data.capacity() + 1);
        for (const auto& diagnostic : member.diagnostics)
            if (charged) charged = add(diagnostic.subsystem.capacity() + diagnostic.code.capacity()
                + diagnostic.message.capacity() + (diagnostic.origin ? diagnostic.origin->fingerprint.capacity()
                    + diagnostic.origin->source.native().capacity() * sizeof(std::filesystem::path::value_type) : 0) + 4);
        if (charged && candidate.Strings()) {
            const auto& table = *candidate.Strings();
            charged = add(table.originalBytes.capacity() + table.entries.capacity() * sizeof(MapData::StringTableEntry));
            for (const auto& entry : table.entries) if (charged) charged = add(entry.value.capacity() + 1);
        }
        if (!charged) { namesIssue = issue; return false; }
        names.emplace(std::move(candidate)); return true;
    }
    bool Prepare(std::span<const std::size_t> types, bool needStandards, std::size_t maximumWork,
        std::size_t maximumBytes, std::size_t& work) {
        issue.clear();
        if (estimatedBytes > maximumBytes) { Charge(0, maximumBytes); return false; }
        for (const auto type : types) {
            auto& index = maps[type];
            if (!typeIssues[type].isEmpty()) { issue = typeIssues[type]; return false; }
            if (index.native && (!index.native->IsStructurallySafe() || index.native->Type() != static_cast<ObjectData::ObjectType>(type))) {
                typeIssues[type] = issue = QObject::tr("原生地图物体结构或类别不可确认，未回落标准来源"); return false;
            }
            while (index.scanned < index.source.size() && work < maximumWork) {
                const auto& object = index.source[index.scanned++]; ++work;
                if (object.key.type != static_cast<ObjectData::ObjectType>(type) || object.key.rawcode.size() != 4) {
                    typeIssues[type] = issue = QObject::tr("地图物体身份或类别无效，不能确定覆盖来源"); return false;
                }
                // 拥有的 string key、哈希节点及桶、指针数组扩容与分配器开销均计入。
                constexpr auto bytes = sizeof(decltype(index.byId)::value_type) + 12 * sizeof(void*);
                if (!Charge(bytes, maximumBytes)) { typeIssues[type] = issue; return false; }
                if (!index.byId.emplace(object.key.rawcode, &object).second) {
                    typeIssues[type] = issue = QObject::tr("地图物体身份重复，不能确定覆盖来源"); return false;
                }
                index.ordered.push_back(&object);
            }
            if (index.scanned < index.source.size()) return false;
        }
        if (!needStandards) return true;
        if (!standardIssue.isEmpty()) { issue = standardIssue; return false; }
        const auto entries = catalog ? catalog->Entries() : std::span<const ObjectData::StandardObjectEntry>{};
        while (standardScanned < entries.size() && work < maximumWork) {
            const auto& entry = entries[standardScanned++]; ++work;
            const auto type = static_cast<std::size_t>(entry.key.type);
            if (type >= standards.size()) { standardIssue = issue = QObject::tr("标准物体目录含无效类别"); return false; }
            if (!Charge(2 * sizeof(void*), maximumBytes)) { standardIssue = issue; return false; }
            standards[type].push_back(&entry);
        }
        return standardScanned == entries.size();
    }
};

ObjectViewIndexCache& SourceIndex(const QtProject::EditorProject& project) {
    constexpr auto property = "_forgeObjectViewSourceIndex";
    auto* owner = const_cast<QtProject::EditorProject*>(&project);
    auto* cache = dynamic_cast<ObjectViewIndexCache*>(owner->property(property).value<QObject*>());
    if (!cache) {
        cache = new ObjectViewIndexCache(owner);
        owner->setProperty(property, QVariant::fromValue(static_cast<QObject*>(cache)));
    }
    if (!cache->Matches(project)) cache->Reset(project);
    return *cache;
}
} // namespace

ObjectViewClassification ResolveObjectViewClassification(const QtProject::EditorProject& project,
    const ObjectData::ObjectKey& object) {
    const auto index = static_cast<std::size_t>(object.type);
    if (index >= project.objectDocuments_.size()) return {{}, std::nullopt, QObject::tr("对象类型不可用")};
    if (const auto* lni = project.objectDocuments_[index].get(); lni && lni->FindObject(object))
        return ClassificationForChain(ResolveObjectInheritance(*lni, object, project.standardCatalog_.get()), object.type);
    if (!project.objectDocuments_[index])
        if (const auto* native = project.nativeObjectDocuments_[index].get(); native && native->FindObject(object))
            return ClassificationForChain(ResolveNativeObjectInheritance(*native, object, project.standardCatalog_.get()), object.type);
    if (const auto* standard = project.standardCatalog_ ? project.standardCatalog_->Find(object.type, object.rawcode) : nullptr) {
        ObjectViewClassification result;
        result.folders = Modules::ObjectDetail::ClassifyStandard(*standard);
        if (object.type == ObjectData::ObjectType::Unit) result.hero = standard->category == "hero";
        return result;
    }
    return {{QObject::tr("分类未确认")}, std::nullopt, QObject::tr("对象的物编类别尚未确认")};
}

QVector<ObjectViewEntry> BuildObjectViewEntries(const QtProject::EditorProject& project, QVector<ObjectViewEntry>* standardOnly) {
    QVector<ObjectViewEntry> entries;
    if (project.standardCatalog_) {
        for (const auto& object : project.standardCatalog_->Entries()) {
            auto entry = StandardEntry(object, project.standardCatalog_.get(), QtProject::ProjectEffectiveObjectCatalog(project).get());
            if (standardOnly) standardOnly->push_back(entry);
            entries.push_back(std::move(entry));
        }
    }
    for (const auto& document : project.objectDocuments_) {
        if (!document) continue;
        for (const auto& object : document->Objects()) {
            auto entry = MapEntry(project, *document, object);
            auto existing = std::find_if(entries.begin(), entries.end(),
                [&entry](const auto& value) { return value.id == entry.id && value.kind == entry.kind; });
            if (existing != entries.end()) *existing = entry;
            else entries.push_back(std::move(entry));
        }
    }
    return entries;
}

ObjectViewPage QueryObjectViewEntries(const QtProject::EditorProject& project, const ObjectViewQuery& query) {
    ObjectViewPage page;
    const auto refuse = [&](const QString& issue) { page.partial = true; page.issue = issue; return page; };
    if (!query.limit || query.limit > 128 || !query.maximumExamined || query.maximumExamined > 8192
        || query.search.toUtf8().size() > 1024 || query.kind.size() > 64 || query.group.size() > 256
        || query.catalogSet.size() > 64 || query.catalogCategory.size() > 256
        || !query.maximumIndexBytes || query.maximumIndexBytes > 64 * 1024 * 1024
        || (query.source != ObjectViewSource::All && query.source != ObjectViewSource::Map && query.source != ObjectViewSource::Standard))
        return refuse(QObject::tr("资源目录查询超过有界参数范围"));
    if (project.thread() != QThread::currentThread()) return refuse(QObject::tr("资源目录须在工程所属线程查询"));
    std::array<std::size_t, 7> selectedTypes{}; std::size_t selectedCount = 0;
    for (std::size_t type = 0; type < selectedTypes.size(); ++type)
        if (query.kind.isEmpty() || Kind(static_cast<ObjectData::ObjectType>(type)) == query.kind) selectedTypes[selectedCount++] = type;
    if (!selectedCount) return refuse(QObject::tr("资源目录类别不可用"));
    auto& index = SourceIndex(project);
    if (index.byteBudget != query.maximumIndexBytes) { index.Reset(project); index.byteBudget = query.maximumIndexBytes; }
    const auto types = std::span(selectedTypes).first(selectedCount);
    const bool prepared = index.Prepare(types, query.source != ObjectViewSource::Map,
        query.maximumExamined, query.maximumIndexBytes, page.examined);
    if (!index.issue.isEmpty()) return refuse(index.issue);
    if (!prepared || page.examined == query.maximumExamined) {
        page.preparing = true; page.hasMore = true; page.nextOffset = query.offset; return page;
    }
    std::size_t mapSize = 0, standardSize = 0;
    for (const auto type : types) {
        if (query.source != ObjectViewSource::Standard) mapSize += index.maps[type].ordered.size();
        if (query.source != ObjectViewSource::Map) standardSize += index.standards[type].size();
    }
    const auto total = mapSize + standardSize;
    if (query.offset > total) return refuse(QObject::tr("资源目录分页位置已失效"));
    page.nextOffset = query.offset;
    const auto boundedChain = [&](const ObjectData::ObjectRecord& object) {
        const auto* node = &object;
        std::unordered_set<std::string> visited;
        for (unsigned depth = 0; node; ++depth) {
            if (depth >= 32 || node->modifications.size() > 4096) return false;
            for (const auto& field : node->modifications)
                if (field.originalValueToken.size() > 16384
                    || (std::holds_alternative<std::string>(field.value) && std::get<std::string>(field.value).size() > 16384)
                    || (std::holds_alternative<ObjectData::LocalizedStringValue>(field.value) && std::get<ObjectData::LocalizedStringValue>(field.value).value.size() > 16384)
                    || (std::holds_alternative<ObjectData::AssetPathValue>(field.value) && std::get<ObjectData::AssetPathValue>(field.value).value.size() > 16384)) return false;
            if (!visited.insert(node->key.rawcode).second) break; // 语义问题仍由原共用父链报告。
            const auto parent = node->parentRawcode.value_or(node->key.rawcode);
            if (parent == node->key.rawcode) break;
            const auto& byId = index.maps[static_cast<std::size_t>(node->key.type)].byId;
            const auto found = byId.find(parent);
            node = found == byId.end() ? nullptr : found->second;
        }
        return true;
    };
    std::size_t logicalBytes = 0;
    if (!index.PrepareNames(query.maximumIndexBytes)) return refuse(index.issue);
    const auto* names = index.names ? &*index.names : project.mapDocument_.get();
    while (page.nextOffset < total && page.examined < query.maximumExamined
        && static_cast<std::size_t>(page.rows.size()) < query.limit) {
        const auto current = page.nextOffset++;
        ++page.examined;
        ObjectViewRow row;
        if (current < mapSize) {
            const ObjectMapIndex* source = nullptr; const ObjectData::ObjectRecord* object = nullptr;
            auto local = current;
            for (const auto type : types) {
                if (local < index.maps[type].ordered.size()) { source = &index.maps[type]; object = source->ordered[local]; break; }
                local -= index.maps[type].ordered.size();
            }
            row.object = object->key; row.mapObject = true;
            row.customObject = object->parentRawcode && *object->parentRawcode != object->key.rawcode;
            if (!query.kind.isEmpty() && Kind(row.object.type) != query.kind) continue;
            if (boundedChain(*object)) row.entry = source->document
                ? MapEntry(project, *source->document, *object, names)
                : MapEntry(project, *source->native, *object, names);
            else {
                row.entry.kind = Kind(row.object.type); row.entry.id = U8(row.object.rawcode);
                row.entry.name = row.entry.id; row.entry.available = false;
                row.entry.description = QObject::tr("继承或字段超过展示预算，模型不可用");
            }
        } else {
            const ObjectData::StandardObjectEntry* standard = nullptr; auto local = current - mapSize;
            for (const auto type : types) {
                if (local < index.standards[type].size()) { standard = index.standards[type][local]; break; }
                local -= index.standards[type].size();
            }
            const auto& source = *standard;
            if (index.maps[static_cast<std::size_t>(source.key.type)].byId.contains(source.key.rawcode)) continue;
            row.object = source.key;
            if (source.displayName.size() > 4096 || source.iconPath.size() > 4096 || source.race.size() > 256 || source.category.size() > 256
                || source.Field("ubertip").size() > 4096) {
                page.partial = true; page.issue = QObject::tr("部分原版展示文本超过预算"); continue;
            }
            row.entry = StandardEntry(source, project.standardCatalog_.get(), QtProject::ProjectEffectiveObjectCatalog(project).get());
        }
        if (row.entry.available) row.classification = ResolveObjectViewClassification(project, row.object);
        else {
            row.classification.folders = {QObject::tr("分类未确认")};
            row.classification.issue = row.entry.description;
        }
        if (!query.group.isEmpty() && row.entry.race != query.group && row.entry.category != query.group
            && (row.classification.folders.isEmpty() || row.classification.folders.front() != query.group)) continue;
        if (!query.catalogSet.isEmpty() && (row.object.type != ObjectData::ObjectType::Unit
            || row.classification.folders.size() < 2 || row.classification.folders[1] != query.catalogSet)) continue;
        if (!query.catalogCategory.isEmpty() && (row.classification.folders.isEmpty()
            || row.classification.folders.back() != query.catalogCategory)) continue;
        if (!query.search.isEmpty() && !row.entry.name.contains(query.search, Qt::CaseInsensitive)
            && !row.entry.id.contains(query.search, Qt::CaseInsensitive)) continue;
        row.entry.name = row.entry.name.left(1024); row.entry.description = row.entry.description.left(1024);
        row.entry.details.clear();
        const auto bytes = row.entry.name.toUtf8().size() + row.entry.id.toUtf8().size()
            + row.entry.description.toUtf8().size() + row.entry.iconPath.toUtf8().size() + 128;
        if (static_cast<std::size_t>(bytes) > 128 * 1024 - logicalBytes) { --page.nextOffset; break; }
        logicalBytes += static_cast<std::size_t>(bytes);
        page.rows.push_back(std::move(row));
    }
    page.hasMore = page.nextOffset < total;
    if (!project.standardCatalog_ && query.source != ObjectViewSource::Map) {
        page.partial = true; page.issue = QObject::tr("原版目录未加载，只列出当前地图物体");
    }
    return page;
}

} // namespace War3Map::QtApp
