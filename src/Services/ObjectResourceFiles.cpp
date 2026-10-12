#include "ObjectResourceQueryDetail.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/SoundInfoTables.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"
#include "War3MapCore/Text/SlkCells.h"

namespace War3Map::Services {
using namespace ObjectResourceDetail;
namespace {
bool AddFile(Builder& builder, ObjectResourceCandidate row, std::set<std::string>& directories,
    std::set<std::string>& files) {
    if (!builder.Tick()) return false;
    const auto kind = PathKind(row.value);
    if (!kind) return true;
    const auto canonical = Resources::CanonicalImportPath(row.value);
    if (!canonical.Succeeded()) {
        builder.Catalog().complete = false;
        builder.Diagnose(Warning("OBJECT_RESOURCE_PATH_INVALID", "未列出不安全资源路径。")); return true;
    }
    const auto key = *canonical.value + ':' + row.state;
    if (!files.insert(key).second) return true;
    row.kind = *kind; row.previewPath = row.value;
    const auto path = Slash(row.value);
    if (row.title.empty()) { const auto slash = path.rfind('/'); row.title = slash == std::string::npos ? row.value : row.value.substr(slash + 1); }
    const auto origin = row.origin;
    if (!builder.Add(std::move(row))) return false;
    for (auto end = path.find('/'); end != std::string::npos; end = path.find('/', end + 1)) {
        const auto directory = path.substr(0, end);
        const auto id = std::string(ObjectResourceKindName(*kind)) + ':' + Text::LowerAscii(directory);
        if (!directories.insert(id).second) continue;
        ObjectResourceCandidate folder;
        folder.id = "directory:" + id; folder.value = directory; folder.kind = *kind; folder.origin = origin;
        folder.directory = true; folder.state = "directory";
        const auto slash = directory.rfind('/'); folder.title = slash == std::string::npos ? directory : directory.substr(slash + 1);
        if (!builder.Add(std::move(folder))) return false;
    }
    return true;
}
void AddSoundLabels(Builder& builder, const Game::GameDataSource& source) {
    for (const auto name : Game::kSoundInfoTables) {
        if (!builder.Tick()) return;
        const std::string path = "UI/SoundInfo/" + std::string(name) + ".slk";
        auto file = source.ReadFileBoundedWithSource(path, 4 * 1024 * 1024);
        if (!file.Succeeded()) {
            builder.Catalog().complete = false;
            for (const auto& diagnostic : file.diagnostics) builder.Diagnose(diagnostic);
            continue;
        }
        auto table = Text::ParseSlkCells(std::string_view(reinterpret_cast<const char*>(file.value->bytes.data()),
            file.value->bytes.size()), Text::SlkCellMode::CatalogStrict);
        if (!table.Succeeded()) {
            builder.Catalog().complete = false;
            for (const auto& diagnostic : table.diagnostics) builder.Diagnose(diagnostic);
            continue;
        }
        int filesColumn = 0, baseColumn = 0;
        const auto headers = table.value->find(1);
        if (headers == table.value->end()) {
            builder.Catalog().complete = false;
            builder.Diagnose(Warning("OBJECT_RESOURCE_SOUND_TABLE_HEADER", "声音标签表缺少表头。"));
            continue;
        }
        for (const auto& [column, title] : headers->second) {
            if (Text::LowerAscii(title) == "filenames") filesColumn = column;
            if (Text::LowerAscii(title) == "directorybase") baseColumn = column;
        }
        for (const auto& [number, row] : *table.value) {
            if (number == 1) continue;
            if (!builder.Tick()) return;
            const auto label = row.find(1), files = row.find(filesColumn), base = row.find(baseColumn);
            if (label == row.end() || label->second.empty() || files == row.end()) continue;
            const auto names = Values(files->second, true);
            if (names.empty() || names.front().empty() || names.front() == "_") continue;
            ObjectResourceCandidate candidate;
            candidate.id = "sound-label:" + std::string(name) + ':' + label->second;
            candidate.title = candidate.value = label->second; candidate.kind = ObjectResourceKind::SoundLabel;
            candidate.origin = ObjectResourceOrigin::Game; candidate.state = "unverified";
            candidate.sourceId = file.value->archiveName + ':' + path;
            candidate.previewPath = base == row.end() ? names.front() : JoinPath(base->second, names.front());
            if (!builder.Add(std::move(candidate))) return;
        }
    }
}
}

Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureGameResourceCatalog(
    const Game::GameDataSource& source, std::string revision, const ObjectResourceCatalogLimits& limits) {
    Builder builder(std::move(revision), ObjectResourceOrigin::Game, limits);
    if (!builder.Valid()) return builder.Finish();
    auto listing = source.ListFilesWithSource();
    if (!listing.Succeeded()) return {std::nullopt, std::move(listing.diagnostics)};
    builder.Catalog().diagnostics = std::move(listing.diagnostics);
    builder.Catalog().complete = builder.Catalog().diagnostics.empty();
    std::set<std::string> directories, files;
    for (const auto& file : *listing.value) {
        ObjectResourceCandidate row;
        row.id = "game:" + file.path; row.value = file.path; row.sourceId = file.archiveName;
        row.origin = ObjectResourceOrigin::Game; row.state = "listed";
        if (!AddFile(builder, std::move(row), directories, files)) return builder.Finish();
    }
    // 原版基础包可能没有 listfile；七张声音表是明确的额外命名来源，不声称全量枚举。
    auto sounds = source.DiscoverSoundFilesFromSoundInfo();
    if (sounds.value) for (const auto& file : *sounds.value) {
        ObjectResourceCandidate row;
        row.id = "game:" + file.path; row.value = file.path; row.sourceId = file.archiveName;
        row.origin = ObjectResourceOrigin::Game; row.state = "listed";
        if (!AddFile(builder, std::move(row), directories, files)) return builder.Finish();
    }
    if (!sounds.diagnostics.empty()) builder.Catalog().complete = false;
    builder.Catalog().diagnostics.insert(builder.Catalog().diagnostics.end(), sounds.diagnostics.begin(), sounds.diagnostics.end());
    AddSoundLabels(builder, source);
    return builder.Finish();
}

Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureImportResourceCatalog(
    const ImportInventorySnapshot& source, const ImportInventoryBinding& expected, const ObjectResourceCatalogLimits& limits) {
    Builder builder(expected.revision, ObjectResourceOrigin::Import, limits);
    if (!builder.Valid()) return builder.Finish();
    std::set<std::string> directories, files;
    // 选择器只需要名字：一次按名单列出，不逐页读取成员内容（大地图逐页核对会反复散列整个归档）。
    auto listed = source.ListNames(expected);
    if (!listed.Succeeded()) return {std::nullopt, std::move(listed.diagnostics)};
    builder.Catalog().complete = builder.Catalog().complete && !source.Description().hasOpaqueArchiveMembers;
    for (const auto& file : *listed.value) {
        if (file.overlayState == "removed") continue;
        ObjectResourceCandidate row;
        row.id = "import:" + file.importId; row.value = file.archivePath.value_or(file.rawPath);
        row.sourceId = file.sourceLayer; row.state = file.state == "listed" ? "present" : file.state;
        row.origin = ObjectResourceOrigin::Import;
        row.sourceVerified = false; // 名单存在不等于内容已核对；预览读取时再按来源核对。
        if (!AddFile(builder, std::move(row), directories, files)) return builder.Finish();
    }
    if (source.Description().hasOpaqueArchiveMembers)
        builder.Diagnose(Warning("OBJECT_RESOURCE_OPAQUE_MEMBERS", "地图包含无名字的成员，未宣称已枚举全部导入资源。"));
    return builder.Finish();
}
} // namespace War3Map::Services
