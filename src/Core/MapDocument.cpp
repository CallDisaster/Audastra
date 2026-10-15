#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/ClassicMapLayouts.h"

#include <algorithm>
#include <fstream>
#include <set>

namespace War3Map {
namespace {
constexpr std::array<std::string_view, 7> names{
    "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3r",
    "war3map.w3c", "war3map.w3s", "war3map.wts"};

std::string Canonical(std::string value) {
    for (auto& c : value) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return value;
}

std::optional<std::uint32_t> NativeW3iVersion(const MapMemberState& settings) {
    if (settings.sources.size() != 1) return {};
    const auto name = Canonical(settings.sources.front().name);
    // 展开目录已把 map/ 前缀识别为同一成员；这里只核资格，不改写实际来源名。
    if (name != "war3map.w3i" && name != "map/war3map.w3i") return {};
    const auto& bytes = settings.sources.front().data;
    if (bytes.size() < 4) return {};
    std::uint32_t version = 0;
    for (unsigned i = 0; i < 4; ++i)
        version |= std::uint32_t(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    return version;
}

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

template<class T, class Parser, class Counter>
void Parse(MapMemberState& member, std::optional<T>& target, Parser parser, Counter count) {
    auto parsed = parser(member.sources.front().data);
    member.diagnostics = std::move(parsed.diagnostics);
    // 诊断已移动，显式检查目的集合，不能在原 Result 上遗漏 Error。
    const bool errors = std::any_of(member.diagnostics.begin(), member.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; });
    if (!parsed.value || errors) { member.state = MapDataState::Invalid; return; }
    target = std::move(*parsed.value);
    member.state = member.diagnostics.empty() ? MapDataState::Ready : MapDataState::Partial;
    member.recordCount = count(*target);
}

template<class Records, class Key>
void CheckUnique(MapMemberState& member, const Records& records, Key key) {
    using KeyType = std::decay_t<decltype(key(*records.begin()))>;
    std::set<KeyType> keys;
    for (const auto& record : records) {
        if (!keys.insert(key(record)).second) {
            member.state = MapDataState::Partial;
            member.diagnostics.push_back({DiagnosticSeverity::Warning, "Map.Document", "MAP_IDENTITY_AMBIGUOUS",
                "存在重复实例 ID 或定义名称，不能建立唯一初始化符号。"});
            break;
        }
    }
}
}

MapDocument::MapDocument() {
    for (std::size_t i = 0; i < names.size(); ++i) {
        members_[i].kind = static_cast<MapDataKind>(i);
        members_[i].memberName = names[i];
    }
}

MapDocument MapDocument::FromMembers(std::span<const MapArchiveMember> members,
    const std::filesystem::path& source) {
    MapDocument document;
    document.source_ = source;
    for (const auto& input : members) {
        const auto canonical = Canonical(input.name);
        for (auto& member : document.members_) {
            if (canonical == Canonical(member.memberName)
                || (member.kind == MapDataKind::Settings && canonical == "table/w3i.ini")) member.sources.push_back(input);
        }
    }
    document.ParseMembers();
    return document;
}

MapDocument MapDocument::FromArchive(const MapArchiveSession& archive) {
    return FromMembers(archive.Members(), archive.SourcePath());
}

Result<MapDocument> MapDocument::OpenReadOnlyArchive(const std::filesystem::path& path) {
    auto archive = MpqWorkspace::OpenReadOnly(path);
    if (!archive.Succeeded()) { Result<MapDocument> failure; failure.diagnostics = std::move(archive.diagnostics); return failure; }
    return FromReadOnlyArchive(*archive.value);
}

Result<MapDocument> MapDocument::FromReadOnlyArchive(const MpqWorkspace& archive) {
    const auto before = archive.VerifyUnchanged();
    if (!before.Succeeded()) return {std::nullopt, before.diagnostics};
    MapDocument document;
    document.source_ = archive.SourcePath();
    for (auto& member : document.members_) {
        if (!archive.Archive().HasFile(member.memberName)) continue;
        auto read = archive.ReadMember(member.memberName, 128 * 1024 * 1024);
        if (read.Succeeded()) member.sources.push_back({member.memberName, std::move(*read.value)});
        else { member.state = MapDataState::Unreadable; member.diagnostics = std::move(read.diagnostics); }
    }
    const auto after = archive.VerifyUnchanged();
    if (!after.Succeeded()) return {std::nullopt, after.diagnostics};
    document.ParseMembers();
    return Result<MapDocument>::Success(std::move(document));
}

Result<MapDocument> MapDocument::FromDirectory(const DirectoryWorkspace& workspace) {
    MapDocument document;
    document.source_ = workspace.Snapshot().root;
    // 仅读取已快照的七类文件及设置的 LNI 表，不探测另一套未快照数据。
    for (const auto& file : workspace.Snapshot().files) {
        const auto path = PathText(file.relativePath);
        const auto canonical = Canonical(path);
        for (auto& member : document.members_) {
            const auto name = Canonical(member.memberName);
            if (canonical != name && canonical != "map/" + name
                && !(member.kind == MapDataKind::Settings && canonical == "table/w3i.ini")) continue;
            if (file.size > 128 * 1024 * 1024) {
                member.state = MapDataState::Unreadable;
                member.diagnostics.push_back({DiagnosticSeverity::Error, "Map.Document", "MAP_MEMBER_TOO_LARGE",
                    path + " 超出本轮基础数据读取预算；源文件保留。"});
                continue;
            }
            std::ifstream input(workspace.Snapshot().root / file.relativePath, std::ios::binary);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file.size));
            if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))
                || input.peek() != std::char_traits<char>::eof()) {
                return Result<MapDocument>::Failure("Map.Document", "MAP_SOURCE_READ_FAILED", path + " 读取失败或长度已变化。");
            }
            const auto fingerprint = Sha256(bytes);
            if (!fingerprint.Succeeded() || *fingerprint.value != file.sha256) {
                return Result<MapDocument>::Failure("Map.Document", "MAP_SOURCE_CHANGED", path + " 与打开时的快照不一致。");
            }
            member.sources.push_back({path, std::move(bytes)});
        }
    }
    document.ParseMembers();
    return Result<MapDocument>::Success(std::move(document));
}

void MapDocument::ParseMembers() {
    // 原始 W3I 版本头是来源证据，即使正文尚不支持/损坏也独立约束地物和镜头布局。
    // 两项规则共用一次头读取；展开目录的 LNI 设置仍沿用已解析版本。
    const auto sourceVersion = NativeW3iVersion(members_[static_cast<std::size_t>(MapDataKind::Settings)]);
    const bool versionedWidgetSource = sourceVersion && *sourceVersion >= 28;
    const bool classicCameraSource = sourceVersion == 18u || sourceVersion == 25u;
    for (auto& member : members_) {
        if (member.state == MapDataState::Unreadable) continue;
        if (member.sources.empty()) continue;
        if (member.sources.size() != 1) {
            member.state = MapDataState::Ambiguous;
            member.diagnostics.push_back({DiagnosticSeverity::Error, "Map.Document", "MAP_MEMBER_AMBIGUOUS",
                "同一地图成员有多个来源，须先解决来源冲突。"});
            continue;
        }
        switch (member.kind) {
        case MapDataKind::Settings:
            if (Canonical(member.sources.front().name) == "table/w3i.ini") {
                member.memberName = member.sources.front().name;
                Parse(member, lniSettings_, MapData::ParseLniMapSettings, [](const auto&) { return 1U; });
                if (lniSettings_) {
                    settings_ = lniSettings_->settings;
                    if (!lniSettings_->fullyParsed) member.state = MapDataState::Partial;
                }
            } else {
                Parse(member, settings_, MapData::MapSettings::Parse, [](const auto&) { return 1U; });
                if (settings_ && !settings_->FullyParsed()) member.state = MapDataState::Partial;
            }
            if (settings_) {
                CheckUnique(member, settings_->players, [](const auto& entry) { return entry.number; });
            }
            break;
        case MapDataKind::Units:
            if (versionedWidgetSource || (settings_ && settings_->formatVersion >= 31)) {
                Parse(member, units_, [](const auto& bytes) { return MapData::ReadUnitPlacementsVersioned(bytes); },
                    [](const auto& doc) { return doc.units.size(); });
            } else {
                Parse(member, units_, [](const auto& bytes) { return MapData::ParseUnitPlacements(bytes); },
                    [](const auto& doc) { return doc.units.size(); });
            }
            if (units_) CheckUnique(member, units_->units, [](const auto& entry) { return entry.creationId; });
            break;
        case MapDataKind::Doodads:
            if (versionedWidgetSource || (settings_ && settings_->formatVersion >= 31)) {
                Parse(member, doodads_, [](const auto& bytes) { return MapData::ReadDoodadPlacementsVersioned(bytes); },
                    [](const auto& doc) { return doc.doodads.size() + doc.specialDoodads.size(); });
            } else {
                Parse(member, doodads_, [](const auto& bytes) { return MapData::ParseDoodadPlacements(bytes); },
                    [](const auto& doc) { return doc.doodads.size() + doc.specialDoodads.size(); });
            }
            if (doodads_) CheckUnique(member, doodads_->doodads, [](const auto& entry) { return entry.creationId; });
            break;
        case MapDataKind::Regions:
            Parse(member, regions_, MapData::ParseRegions, [](const auto& doc) { return doc.entries.size(); });
            if (regions_) CheckUnique(member, regions_->entries, [](const auto& entry) { return entry.creationNumber; });
            break;
        case MapDataKind::Cameras:
            // 经典镜头名可以恰好组成三个有限浮点数，不能对已知经典来源强行双布局试读。
            // 新版或缺失/无效 W3I 则没有经典布局证明，完整试读后拒绝歧义。
            Parse(member, cameras_, classicCameraSource || (lniSettings_ && settings_->formatVersion == 25)
                ? MapData::ParseCameras : MapData::ReadCamerasVersioned,
                [](const auto& doc) { return doc.entries.size(); });
            if (cameras_) CheckUnique(member, cameras_->entries, [](const auto& entry) { return entry.name; });
            break;
        case MapDataKind::Sounds:
            Parse(member, sounds_, MapData::ParseSounds, [](const auto& doc) { return doc.entries.size(); });
            if (sounds_) CheckUnique(member, sounds_->entries, [](const auto& entry) { return entry.name; });
            break;
        case MapDataKind::Strings:
            Parse(member, strings_, MapData::ParseStringTable, [](const auto& doc) { return doc.entries.size(); });
            break;
        default: break;
        }
    }
    for (auto& member : members_) for (auto& diagnostic : member.diagnostics) {
        if (!diagnostic.message.starts_with(member.memberName)) diagnostic.message = member.memberName + ": " + diagnostic.message;
        if (!diagnostic.origin) diagnostic.origin = OriginSpan{source_, 0, 0, {}};
    }
}

const MapMemberState& MapDocument::Member(MapDataKind kind) const {
    return members_.at(static_cast<std::size_t>(kind));
}

std::vector<Diagnostic> MapDocument::Diagnostics() const {
    std::vector<Diagnostic> result;
    for (const auto& member : members_) result.insert(result.end(), member.diagnostics.begin(), member.diagnostics.end());
    return result;
}

Status MapDocument::InitializationSourceStatus() const {
    auto status = MapData::ValidateClassicMapLayouts({units_ ? &*units_ : nullptr,
        doodads_ ? &*doodads_ : nullptr, cameras_ ? &*cameras_ : nullptr, sounds_ ? &*sounds_ : nullptr,
        settings_ ? &*settings_ : nullptr});
    for (const auto& member : members_) {
        // 区域、镜头、声音、字符串成员可省略；已存在但不可可靠解析的成员不可当成空数据。
        const bool required = member.kind == MapDataKind::Settings || member.kind == MapDataKind::Units || member.kind == MapDataKind::Doodads;
        if (member.state == MapDataState::Ready || (!required && member.state == MapDataState::Missing)) continue;
        status.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "script.map-source-unavailable",
            member.memberName + (member.state == MapDataState::Missing ? " 缺失，尚无完整初始化输入。" : " 尚不能作为完整初始化输入；查看地图数据中的具体诊断。")});
    }
    const auto textReference = [&](std::string_view value, const std::string& location) {
        if (!value.starts_with("TRIGSTR_")) return;
        const auto resolved = ResolveString(value);
        if (!resolved.Succeeded()) status.diagnostics.push_back({DiagnosticSeverity::Error,
            "Script.Build", "script.map-string-unresolved", location + " 引用了无法解析的 " + std::string(value)});
    };
    if (settings_) {
        const auto& s = *settings_;
        textReference(s.mapName, "地图名称"); textReference(s.author, "地图作者");
        textReference(s.description, "地图说明"); textReference(s.recommendedPlayers, "推荐玩家");
        for (const auto* screen : {&s.loadingScreen, &s.prologueScreen}) {
            textReference(screen->title, "画面标题"); textReference(screen->subtitle, "画面副标题"); textReference(screen->text, "画面正文");
        }
        for (const auto& p : s.players) textReference(p.name, "玩家名称");
        for (const auto& f : s.forces) textReference(f.name, "队伍名称");
        if (s.custom) {
            for (const auto& table : s.custom->randomUnits) textReference(table.name, "随机单位表名称");
            for (const auto& table : s.custom->randomItems) textReference(table.name, "随机物品表名称");
        }
    }
    if (cameras_) for (const auto& c : cameras_->entries) textReference(c.name, "镜头名称");
    if (regions_) for (const auto& r : regions_->entries) {
        textReference(r.name, "区域名称");
        if (!r.ambientSound.empty() && (!sounds_ || std::none_of(sounds_->entries.begin(), sounds_->entries.end(),
            [&](const auto& s) { return s.name == r.ambientSound; }))) {
            status.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "script.map-sound-unresolved",
                "区域 #" + std::to_string(r.creationNumber) + " 引用的声音定义不存在：" + r.ambientSound});
        }
    }
    if (units_) for (const auto& u : units_->units) {
        if (u.waygateRegionId != -1 && (!regions_ || std::none_of(regions_->entries.begin(), regions_->entries.end(),
            [&](const auto& r) { return r.creationNumber == u.waygateRegionId; }))) {
            status.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "script.map-region-unresolved",
                "实例 #" + std::to_string(u.creationId) + " 的传送门区域不存在：" + std::to_string(u.waygateRegionId)});
        }
    }
    return status;
}

Result<std::string> MapDocument::ResolveString(std::string_view value) const {
    if (!value.starts_with("TRIGSTR_")) return Result<std::string>::Success(std::string(value));
    if (!strings_) return Result<std::string>::Failure("Map.Document", "MAP_STRING_TABLE_UNAVAILABLE",
        "字符串表缺失、无效或有来源冲突，保留原始引用。" );
    return strings_->Resolve(value);
}

} // namespace War3Map
