#include "ObjectColourPreviewSource.h"

#include "EditorProject.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Services/NameResolution.h"

#include <QPointer>

#include <map>
#include <optional>
#include <utility>

namespace War3Map::Modules {
namespace {
using UiKit::WarcraftTextSource;
using UiKit::WarcraftTextSourceState;
constexpr std::size_t MaxSourceBytes = 64U * 1024U * 1024U;
constexpr std::size_t MaxRecords = 131072;
constexpr std::size_t MaxBodyBytes = 1024U * 1024U;

struct Match { std::size_t first = 0; std::optional<std::size_t> duplicate; };
struct Prepared {
    std::shared_ptr<const MapDocument> source;
    std::function<bool()> current;
    ObjectColourPreviewLimits limits;
    std::map<std::uint32_t, Match> ids;
    QString unavailable;
};

WarcraftTextSource Unknown(QString reason) { return {{}, reason.left(1024), WarcraftTextSourceState::Unknown}; }

struct Stamp {
    const MapDocument* document = nullptr;
    const MapArchiveSession* archive = nullptr;
    const DirectoryWorkspace* directory = nullptr;
    const MapData::MapDataEditSession* edits = nullptr;
    std::uint64_t session = 0, mapRevision = 0, editGeneration = 0, editRevision = 0;
    bool suspect = false, confirmed = false;
    bool operator==(const Stamp&) const = default;
};
Stamp ReadStamp(const QtProject::EditorProject& project) {
    const auto* edits = project.MapEdits();
    return {project.mapDocument_.get(), project.archiveSession_.get(), project.directorySnapshot_.get(), edits,
        edits ? edits->SessionIdentity() : 0, edits ? edits->Revision() : 0,
        project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.sourceChangeSuspected_, project.sourceChangeConfirmed_};
}

WarcraftTextSource Resolve(const Prepared& source, const QString& draft) {
    if (!source.current || !source.current())
        return {{}, QObject::tr("当前地图或编辑修订已变化，旧 WTS 正文不再代表当前来源。"), WarcraftTextSourceState::Stale};
    if (draft.size() > 256 * 1024) return Unknown(QObject::tr("草稿超过文字预览预算。"));
    const auto bytes = draft.toUtf8();
    if (QString::fromUtf8(bytes) != draft) return Unknown(QObject::tr("草稿无法精确往返 Unicode。"));
    const std::string_view raw(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const auto id = Services::StringReferenceId(raw);
    Services::NameResolutionContext context;
    context.preserveStringFormatting = true;
    MapData::StringTableDocument one;
    if (id) {
        if (!source.unavailable.isEmpty()) return Unknown(source.unavailable);
        const auto found = source.ids.find(*id);
        if (found != source.ids.end()) {
            const auto& entries = source.source->Strings()->entries;
            if (found->second.duplicate) {
                // 两个实际同 ID 的来源足以复用 N1 的 Ambiguous 判据，不复制无用的大正文。
                one.entries.push_back({entries[found->second.first].id, {}, 0});
                one.entries.push_back({entries[*found->second.duplicate].id, {}, 0});
            } else {
                const auto& entry = entries[found->second.first];
                if (entry.value.size() > source.limits.maxBodyBytes)
                    return Unknown(QObject::tr("当前 WTS 正文超过单项预览预算；原字段值保持不变。"));
                one.entries.push_back(entry); // 保留原 GBK/UTF-8 字节，由实际 N1 解码。
            }
        }
        context.strings = &one;
    }
    const auto resolved = Services::ResolveName(context, raw, "string");
    if (resolved.state != Services::NameResolutionState::Resolved && resolved.state != Services::NameResolutionState::Literal)
        return Unknown(QString::fromUtf8(resolved.reason));
    if (resolved.displayName.size() > source.limits.maxBodyBytes)
        return Unknown(QObject::tr("已解码 WTS 正文超过预览预算；原字段值保持不变。"));
    if (!source.current())
        return {{}, QObject::tr("解析期间来源已变化；拒绝旧 WTS 正文。"), WarcraftTextSourceState::Stale};
    return {QString::fromUtf8(resolved.displayName), id
        ? QObject::tr("当前 WTS 覆盖 · 原字段值 %1；预览不修改引用或字符串表。").arg(draft.left(128))
        : QObject::tr("当前字段草稿；预览不提交字段。"), WarcraftTextSourceState::Available};
}
} // namespace

UiKit::WarcraftTextSourceResolver PrepareObjectColourPreviewSource(std::shared_ptr<const MapDocument> source,
    std::function<bool()> current, const ObjectColourPreviewLimits& limits) {
    auto prepared = std::make_shared<Prepared>();
    prepared->source = std::move(source); prepared->current = std::move(current); prepared->limits = limits;
    if (!limits.maxSourceBytes || limits.maxSourceBytes > MaxSourceBytes || !limits.maxRecords || limits.maxRecords > MaxRecords
        || !limits.maxBodyBytes || limits.maxBodyBytes > MaxBodyBytes) {
        prepared->unavailable = QObject::tr("WTS 预览准备预算无效。");
    } else if (!prepared->source || !prepared->source->Strings()) {
        prepared->unavailable = QObject::tr("当前 WTS 来源未加载或不能安全解析；没有回退旧地图正文。");
    } else {
        const auto& member = prepared->source->Member(MapDataKind::Strings);
        const auto& table = *prepared->source->Strings();
        std::size_t bytes = 0;
        if ((member.state != MapDataState::Ready && member.state != MapDataState::Partial) || table.entries.size() > limits.maxRecords)
            prepared->unavailable = QObject::tr("当前 WTS 来源不完整或记录数超过预览预算。");
        for (const auto& rawSource : member.sources) {
            if (!prepared->unavailable.isEmpty()) break;
            if (rawSource.data.size() > limits.maxSourceBytes - bytes)
                prepared->unavailable = QObject::tr("当前 WTS 原始成员超过预览准备预算。");
            else bytes += rawSource.data.size();
        }
        bytes = 0; // 原成员与解析正文各守同一复制帽，不把未知大小或重复计账当准入。
        for (const auto& entry : table.entries) {
            if (!prepared->unavailable.isEmpty()) break;
            if (entry.value.size() > limits.maxSourceBytes - bytes)
                prepared->unavailable = QObject::tr("当前 WTS 正文合计超过预览准备预算。");
            else bytes += entry.value.size();
        }
        if (prepared->unavailable.isEmpty()) {
            for (std::size_t index = 0; index < table.entries.size(); ++index) {
                auto [item, inserted] = prepared->ids.emplace(table.entries[index].id, Match{index, {}});
                if (!inserted && !item->second.duplicate) item->second.duplicate = index;
            }
        }
    }
    return [prepared = std::move(prepared)](const QString& draft) { return Resolve(*prepared, draft); };
}

UiKit::WarcraftTextSourceResolver CaptureObjectColourPreviewSource(QtProject::EditorProject& project,
    const ObjectColourPreviewLimits& limits) {
    const QPointer<QtProject::EditorProject> owner(&project);
    const auto stamp = ReadStamp(project);
    const auto invalidated = std::make_shared<bool>(false);
    const auto current = [owner, stamp, invalidated] {
        if (*invalidated) return false;
        const bool valid = owner && !stamp.suspect && !stamp.confirmed && ReadStamp(*owner) == stamp;
        if (!valid) *invalidated = true; // 一次来源失效后永不复活旧正文，即使后来撤销或清掉疑似标志。
        return valid;
    };
    std::shared_ptr<const MapDocument> source;
    if (stamp.edits && current() && limits.maxSourceBytes && limits.maxSourceBytes <= MaxSourceBytes) {
        auto captured = project.MapEdits()->Snapshot(MapDataKind::Strings, limits.maxSourceBytes);
        if (captured.Succeeded() && captured.value && current()) source = std::make_shared<const MapDocument>(std::move(*captured.value));
    }
    return PrepareObjectColourPreviewSource(std::move(source), current, limits);
}
} // namespace War3Map::Modules
