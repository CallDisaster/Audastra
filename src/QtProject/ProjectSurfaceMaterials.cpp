#include "ProjectSurfaceMaterials.h"

#include "AssetOwnedPayload.h"
#include "ObjectNames.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"

#include <QObject>

#include <algorithm>
#include <optional>

namespace War3Map::QtProject {
namespace {
using ObjectData::ObjectType;

QString TablePath() {
    return QString::fromLatin1(Render::kSurfaceMaterialTablePath.data(), static_cast<qsizetype>(Render::kSurfaceMaterialTablePath.size()));
}
bool Missing(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.code == "ASSET_SOURCE_MISSING"; });
}
QString FirstMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromStdString(diagnostics.front().message);
}
} // namespace

Result<SurfaceMaterialTableState> ReadSurfaceMaterialTable(const EditorProject& project) {
    SurfaceMaterialTableState state;
    auto read = ReadOwnedMapAsset(project, TablePath(), kSurfaceMaterialTableMaxBytes);
    std::string_view text;
    if (read.Succeeded() && read.value && read.value->bytes) {
        state.exists = true;
        text = std::string_view(reinterpret_cast<const char*>(read.value->bytes->data()), read.value->bytes->size());
        state.document = Services::SurfaceMaterialTableDocument::Parse(text);
    } else if (!Missing(read.diagnostics)) {
        return {std::nullopt, std::move(read.diagnostics)};
    }
    // 归档清单没有列出的表仍按名字读（与此前世界页的读法一致）；资源页删除或改名过的不读回。
    std::optional<std::vector<std::byte>> unlisted;
    const auto key = EditorProject::AssetKey(TablePath());
    if (!state.exists && std::none_of(project.assetEdits_.begin(), project.assetEdits_.end(),
            [&](const auto& edit) { return EditorProject::AssetKey(edit.from) == key; })) {
        if (auto member = project.ReadMapMember(std::string(Render::kSurfaceMaterialTablePath));
            member.Succeeded() && member.value && member.value->size() <= kSurfaceMaterialTableMaxBytes) {
            unlisted = std::move(*member.value);
            state.exists = true;
            text = std::string_view(reinterpret_cast<const char*>(unlisted->data()), unlisted->size());
            state.document = Services::SurfaceMaterialTableDocument::Parse(text);
        }
    }
    auto hash = Sha256Text(text);
    if (!hash.Succeeded() || !hash.value) return {std::nullopt, std::move(hash.diagnostics)};
    state.sha256 = std::move(*hash.value);
    return Result<SurfaceMaterialTableState>::Success(std::move(state));
}

Services::SurfaceMaterialImageSource ReadSurfaceMaterialImage(const EditorProject& project, std::string_view path) {
    using State = Services::SurfaceMaterialImageSource::State;
    Services::SurfaceMaterialImageSource source;
    auto read = ReadOwnedMapAsset(project, QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())),
        kOwnedAssetReadMaxBytes);
    if (read.Succeeded() && read.value && read.value->bytes) {
        source.state = State::Found;
        source.bytes = read.value->bytes;
    } else {
        source.state = Missing(read.diagnostics) ? State::Missing : State::Failed;
        source.problems = std::move(read.diagnostics);
    }
    return source;
}

SurfaceMaterialTypeIndex::SurfaceMaterialTypeIndex(const EditorProject& project) : project_(project) {
    std::optional<MapDocument> snapshot;
    const MapDocument* document = project.mapDocument_.get();
    if (project.mapEdits_) { snapshot.emplace(project.mapEdits_->Snapshot()); document = &*snapshot; }
    if (!document || !document->Doodads()) return;
    for (const auto& doodad : document->Doodads()->doodads) ++placements_[doodad.rawcode];
}

Services::SurfaceMaterialTypeInfo SurfaceMaterialTypeIndex::Info(std::string_view rawcode) const {
    using Category = Services::SurfaceMaterialTypeCategory;
    Services::SurfaceMaterialTypeInfo info;
    if (const auto found = placements_.find(rawcode); found != placements_.end()) info.placements = found->second;
    const std::string code(rawcode);
    const bool doodad = ProjectObjectKnown(project_, ObjectType::Doodad, code);
    const bool destructible = ProjectObjectKnown(project_, ObjectType::Destructable, code);
    info.category = doodad && destructible ? Category::Ambiguous : doodad ? Category::Doodad
        : destructible ? Category::Destructible : Category::Unknown;
    if (doodad != destructible)
        info.name = ResolveProjectObjectPresentation(project_, {doodad ? ObjectType::Doodad : ObjectType::Destructable, code})
            .name.toStdString();
    return info;
}

Services::SurfaceMaterialValidationSources SurfaceMaterialSources(const EditorProject& project,
    const SurfaceMaterialTypeIndex& index, Services::SurfaceMaterialImageCache* cache) {
    Services::SurfaceMaterialValidationSources sources;
    sources.readImage = [&project](std::string_view path) { return ReadSurfaceMaterialImage(project, path); };
    sources.typeInfo = [&index](std::string_view rawcode) { return index.Info(rawcode); };
    sources.cache = cache;
    return sources;
}

QString CommitSurfaceMaterialTable(EditorProject& project, std::string_view expectedSha256,
    const Services::SurfaceMaterialTableDocument& document, std::vector<EditorProject::ImportPayload> images,
    bool replaceImages, const QString& label) {
    const auto current = ReadSurfaceMaterialTable(project);
    if (!current.Succeeded() || !current.value) return FirstMessage(current.diagnostics, QObject::tr("读不出当前材质表。"));
    if (!expectedSha256.empty() && current.value->sha256 != expectedSha256)
        return QObject::tr("材质表已被其他操作修改，请重新读取后再改。");
    if (!replaceImages) {
        const auto entries = project.AssetEntries();
        for (const auto& image : images) {
            const auto key = EditorProject::AssetKey(image.archivePath);
            if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == key; }))
                return QObject::tr("%1 已有文件；要覆盖时须明确允许替换。").arg(image.archivePath);
        }
    }
    const auto text = document.Text();
    if (text.size() > kSurfaceMaterialTableMaxBytes) return QObject::tr("材质表超过 1 MiB。");
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    std::vector<EditorProject::ImportPayload> payloads;
    payloads.push_back({TablePath(), std::make_shared<const std::vector<std::byte>>(begin, begin + text.size())});
    for (auto& image : images) payloads.push_back(std::move(image));
    return project.AddImportPayloads(payloads, true, label);
}
} // namespace War3Map::QtProject
