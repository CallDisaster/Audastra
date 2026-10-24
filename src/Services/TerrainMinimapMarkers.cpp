#include "War3MapCore/Services/TerrainMinimapMarkers.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
using namespace MapData;
using namespace ObjectData;
using Bytes = std::vector<std::byte>;
constexpr const char* kSubsystem = "Terrain.MinimapMarkers";
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
std::string Canonical(std::string_view name) { return Take(Resources::CanonicalImportPath(name)); }
void Charge(std::size_t& used, std::size_t count, std::size_t maximum, std::size_t multiple = 1) {
    if (used > maximum || !multiple || count > (maximum - used) / multiple)
        Fail("TERRAIN_MMP_BUDGET", "MMP累计读取或计算保留超过预算。");
    used += count * multiple;
}
std::string_view TextBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    return text;
}
void Validate(const TerrainMinimapMarkerLimits& limits) {
    const TerrainMinimapMarkerLimits hard;
    if (!limits.maximumSourceBytes || limits.maximumSourceBytes > hard.maximumSourceBytes
        || !limits.maximumFileBytes || limits.maximumFileBytes > hard.maximumFileBytes
        || !limits.maximumWorkingBytes || limits.maximumWorkingBytes > hard.maximumWorkingBytes
        || !limits.maximumUnits || limits.maximumUnits > hard.maximumUnits
        || limits.markers.maxBytes > hard.markers.maxBytes || limits.markers.maxMarkers > hard.markers.maxMarkers)
        Fail("TERRAIN_MMP_LIMITS", "MMP捕获预算只能收紧非零硬上限。");
}

// 一次捕获持有实际源/游戏包只读租约；只有明确缺失才读取下一层。
// exact phase先批量捕获所有固定名；颜色贴图名由实际profile构造后另捕获一批。
struct Context {
    const MapArchiveSession& source;
    const TerrainMinimapMarkerLimits& limits;
    std::filesystem::path gameRoot;
    std::uint8_t tileset = 0;
    Render::AnimationSourceGuard guard;
    std::optional<Game::GameDataSource> game;
    std::map<std::string, const MemberReplacement*, std::less<>> replacements;
    std::set<std::string, std::less<>> removals;
    std::map<std::string, ArchiveExactMember, std::less<>> files;
    std::map<std::string, TerrainMinimapMarkerDependency, std::less<>> facts;
    std::map<std::string, ArchiveExactMember, std::less<>> startupFiles;
    std::size_t sourceBytes = 0, workingBytes = 0;

    Context(const MapArchiveSession& archive, std::filesystem::path root, const TerrainMinimapMarkerLimits& budget,
        std::span<const MemberReplacement> output, std::span<const std::string> removed)
        : source(archive), limits(budget), gameRoot(std::move(root)) {
        if (output.size() > 16384 || removed.size() > 16384)
            Fail("TERRAIN_MMP_BUDGET", "MMP最终视图成员数量超限。");
        std::set<std::string, std::less<>> seen;
        std::size_t names = 0;
        for (const auto& item : output) {
            if (item.name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_MMP_BUDGET", "最终成员名超过路径预算。");
            const auto path = Resources::CanonicalImportPath(item.name);
            if (!path.Succeeded() || !path.value) continue; // 无关非ASCII文件仍由原保存门处理。
            Charge(names, path.value->size(), ArchiveExactMemberLimits{}.totalPathBytes);
            if (!seen.insert(*path.value).second) Fail("TERRAIN_MMP_FINAL_CONFLICT", "最终成员含重复路径。");
            replacements.emplace(*path.value, &item);
        }
        for (const auto& name : removed) {
            if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_MMP_BUDGET", "最终删除名超过路径预算。");
            const auto path = Resources::CanonicalImportPath(name);
            if (!path.Succeeded() || !path.value) continue;
            Charge(names, path.value->size(), ArchiveExactMemberLimits{}.totalPathBytes);
            if (!seen.insert(*path.value).second) Fail("TERRAIN_MMP_FINAL_CONFLICT", "最终成员同时删除/替换或重复删除。");
            removals.insert(*path.value);
        }
        Check(guard.Watch(source.SourcePath()));
    }
    void Phase(std::span<const std::string> names) {
        std::vector<std::string> wanted;
        for (const auto& name : names) {
            const auto key = Canonical(name);
            if (files.contains(key)) continue;
            if (const auto found = replacements.find(key); found != replacements.end()) {
                if (found->second->data.size() > limits.maximumFileBytes)
                    Fail("TERRAIN_MMP_BUDGET", "MMP最终依赖超过单文件预算：" + name);
                Charge(sourceBytes, found->second->data.size(), limits.maximumSourceBytes);
                Charge(workingBytes, found->second->data.size(), limits.maximumWorkingBytes, 2); // 保活原字及散列chunk
                files.emplace(key, ArchiveExactMember{name, true, found->second->data});
            } else if (removals.contains(key)) files.emplace(key, ArchiveExactMember{name, false, {}});
            else wanted.push_back(name);
        }
        if (wanted.empty()) return;
        ArchiveExactMemberLimits exact;
        exact.memberCount = 128; exact.memberBytes = limits.maximumFileBytes;
        exact.totalBytes = std::min(limits.maximumSourceBytes - sourceBytes, (limits.maximumWorkingBytes - workingBytes) / 2);
        for (auto& member : Take(source.CaptureExactMembers(wanted, exact))) {
            Charge(sourceBytes, member.bytes.size(), limits.maximumSourceBytes);
            Charge(workingBytes, member.bytes.size(), limits.maximumWorkingBytes, 2);
            files.emplace(Canonical(member.name), std::move(member));
        }
    }
    const ArchiveExactMember& Read(std::string_view name, bool gameFallback) {
        const auto key = Canonical(name);
        const auto found = files.find(key);
        if (found == files.end()) Fail("TERRAIN_MMP_UNCAPTURED", "读取不在固定MMP捕获名单内：" + std::string(name));
        auto& file = found->second;
        if (facts.contains(key)) return file;
        TerrainMinimapMarkerDependency identity;
        identity.name = key;
        identity.source = file.present ? TerrainMinimapMarkerSource::Map : TerrainMinimapMarkerSource::Missing;
        if (!file.present && gameFallback && !gameRoot.empty()) {
            if (!game) {
                Check(guard.WatchGameRoot(gameRoot));
                game = Take(Game::GameDataSource::MountStrict(gameRoot, tileset));
            }
            if (game->HasFile(std::string(name))) {
                auto read = Take(game->ReadFileBoundedWithSource(std::string(name),
                    std::min({limits.maximumFileBytes, limits.maximumSourceBytes - sourceBytes,
                        (limits.maximumWorkingBytes - workingBytes) / 2})));
                Charge(sourceBytes, read.bytes.size(), limits.maximumSourceBytes);
                Charge(workingBytes, read.bytes.size(), limits.maximumWorkingBytes, 2);
                file.present = true; file.bytes = std::move(read.bytes);
                identity.source = TerrainMinimapMarkerSource::Game; identity.gameContainer = std::move(read.archiveName);
            }
        }
        identity.bytes = file.bytes.size();
        if (file.present) identity.sha256 = Take(Sha256(file.bytes));
        facts.emplace(key, std::move(identity));
        return file;
    }
    const ArchiveExactMember& Required(std::string_view name, bool gameFallback) {
        const auto& file = Read(name, gameFallback);
        if (!file.present || file.bytes.empty()) Fail("TERRAIN_MMP_SOURCE_MISSING", "MMP缺少实际非空依赖：" + std::string(name));
        return file;
    }
    // 4012D0在打开地图前加载war3skins；地图自定义UI皮肤不能重写这份启动配置。
    const ArchiveExactMember& Startup(std::string_view name) {
        const auto key = Canonical(name);
        if (const auto found = startupFiles.find(key); found != startupFiles.end()) return found->second;
        if (gameRoot.empty()) Fail("TERRAIN_MMP_GAME_ROOT", "MMP开始位置颜色需要真实游戏启动配置。");
        if (!game) { Check(guard.WatchGameRoot(gameRoot)); game = Take(Game::GameDataSource::MountStrict(gameRoot, tileset)); }
        ArchiveExactMember file{std::string(name), false, {}};
        TerrainMinimapMarkerDependency identity;
        identity.name = "startup:" + key;
        if (game->HasFile(std::string(name))) {
            auto read = Take(game->ReadFileBoundedWithSource(std::string(name),
                std::min({limits.maximumFileBytes, limits.maximumSourceBytes - sourceBytes,
                    (limits.maximumWorkingBytes - workingBytes) / 2})));
            Charge(sourceBytes, read.bytes.size(), limits.maximumSourceBytes);
            Charge(workingBytes, read.bytes.size(), limits.maximumWorkingBytes, 2);
            file.present = true; file.bytes = std::move(read.bytes);
            identity.source = TerrainMinimapMarkerSource::Game; identity.gameContainer = std::move(read.archiveName);
            identity.bytes = file.bytes.size(); identity.sha256 = Take(Sha256(file.bytes));
        }
        facts.emplace(identity.name, std::move(identity));
        return startupFiles.emplace(key, std::move(file)).first->second;
    }
    void ParseReserve(std::size_t bytes) { Charge(workingBytes, bytes, limits.maximumWorkingBytes, 64); }
};

struct Table {
    std::string name;
    Text::SlkCells cells;
    std::map<std::string, int, std::less<>> columns;
    std::map<std::string, int, std::less<>> rows;
    const std::string* Field(std::string_view id, std::string_view column) const {
        const auto r = rows.find(id), c = columns.find(Text::LowerAscii(column));
        if (r == rows.end() || c == columns.end()) return nullptr;
        const auto& row = cells.at(r->second);
        const auto value = row.find(c->second);
        return value == row.end() ? nullptr : &value->second;
    }
};
Table ReadTable(Context& context, std::string_view name) {
    const auto& file = context.Required(name, true);
    context.ParseReserve(file.bytes.size());
    Table result;
    result.name = name;
    result.cells = Take(Text::ParseSlkCells(TextBytes(file.bytes), Text::SlkCellMode::CatalogUnique));
    const auto head = result.cells.find(1);
    if (head == result.cells.end() || !head->second.contains(1))
        Fail("TERRAIN_MMP_TABLE_HEADER", "MMP表缺少首列标题：" + result.name);
    for (const auto& [x, value] : head->second)
        if (value.empty() || !result.columns.emplace(Text::LowerAscii(value), x).second)
            Fail("TERRAIN_MMP_TABLE_COLUMN", "MMP表存在空或重复列：" + result.name);
    for (const auto& [y, row] : result.cells) {
        if (y == 1) continue;
        const auto id = row.find(1);
        if (id == row.end() || id->second.size() != 4 || !result.rows.emplace(id->second, y).second)
            Fail("TERRAIN_MMP_TABLE_ID", "MMP表对象ID必须为唯一四字节：" + result.name);
        for (const auto& [x, value] : row) {
            // 原版按标题名称查询字段；实际UnitBalance也有无标题的数据列。
            // 这些单元格仍完整解析、计量并绑定来源，只是不赋予它们字段名称。
            if (value.size() > 4096)
                Fail("TERRAIN_MMP_TABLE_COLUMN", "MMP表含过长字段：" + result.name);
        }
    }
    return result;
}

std::unique_ptr<NativeObjectDocument> Native(Context& context, std::string_view name, ObjectType type) {
    const auto& file = context.Read(name, false);
    if (!file.present) return {};
    context.ParseReserve(file.bytes.size());
    auto parsed = NativeObjectCodec(type).Parse(file.bytes);
    if (!parsed.Succeeded() || !parsed.document) throw Failure{std::move(parsed.diagnostics)};
    auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
    if (!native || !native->IsStructurallySafe()
        || (native->Version() != 2 && !(type == ObjectType::Unit && native->Version() == 1)))
        Fail("TERRAIN_MMP_OBJECT_LAYOUT", "MMP只解释完整经典v2及已支持的单位v1原生物编：" + std::string(name));
    parsed.document.release();
    return std::unique_ptr<NativeObjectDocument>(native);
}

class UnitSemantics {
public:
    explicit UnitSemantics(Context& context) : context_(context),
        data_(ReadTable(context, "Units/UnitData.slk")), balance_(ReadTable(context, "Units/UnitBalance.slk")),
        ui_(ReadTable(context, "Units/UnitUI.slk")), abilities_(ReadTable(context, "Units/UnitAbilities.slk")),
        abilityData_(ReadTable(context, "Units/AbilityData.slk")),
        units_(Native(context, "war3map.w3u", ObjectType::Unit)),
        nativeAbilities_(Native(context, "war3map.w3a", ObjectType::Ability)) {
        if (units_ && !units_->Objects().empty()) {
            const auto& metadata = context.Required("Units/UnitMetaData.slk", true);
            context.ParseReserve(metadata.bytes.size());
            metadata_ = ReadTable(context, "Units/UnitMetaData.slk");
            ObjectFieldSchemaOptions options;
            options.sources = {{ObjectType::Unit, "Units/UnitMetaData.slk"}};
            options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
            const auto& editorData = context.Read("UI/UnitEditorData.txt", true);
            if (editorData.present) {
                context.ParseReserve(editorData.bytes.size());
                options.unitEditorDataFile = "UI/UnitEditorData.txt";
            }
            schema_ = Take(ObjectFieldSchema::Load([&](const std::string& name) -> Result<Bytes> {
                if (Canonical(name) == Canonical(metadata.name)) return Result<Bytes>::Success(metadata.bytes);
                if (Canonical(name) == Canonical(editorData.name) && editorData.present)
                    return Result<Bytes>::Success(editorData.bytes);
                return Result<Bytes>::Failure(kSubsystem, "OBJECT_CATALOG_SOURCE_MISSING", "MMP不读取无关元数据。");
            }, options));
        }
    }
    std::optional<std::int32_t> Type(const UnitPlacement& placement) {
        if (placement.rawcode == "sloc") return std::nullopt;
        if (!data_.rows.contains(placement.rawcode) && !Record(units_.get(), ObjectType::Unit, placement.rawcode)) {
            if (!items_) items_ = ReadTable(context_, "Units/ItemData.slk");
            if (items_->rows.contains(placement.rawcode)) return std::nullopt;
            Fail("TERRAIN_MMP_OBJECT_UNKNOWN", "MMP对象既无标准单位/物品定义也无已支持的原生单位父项：" + placement.rawcode);
        }
        const auto building = Integer(Effective(placement.rawcode, "isbldg", "UnitBalance", balance_), placement.rawcode);
        if (!building) return std::nullopt;
        if (GoldMine(placement.rawcode)) return 0;
        const auto race = Effective(placement.rawcode, "race", "UnitData", data_);
        const auto normalized = Text::LowerAscii(race);
        static constexpr std::array<std::string_view, 7> neutral{"demon", "creeps", "critters", "other", "commoner", "naga", "unknown"};
        if (normalized != race)
            Fail("TERRAIN_MMP_RACE_UNSUPPORTED", "MMP暂不解释大小写不同的种族文本：" + placement.rawcode);
        if (std::find(neutral.begin(), neutral.end(), normalized) == neutral.end()) {
            // 4FFC80 mode0：未识别文本返回-1，与四个玩家种族一样不生成type1。
            return std::nullopt;
        }
        if (placement.owner != 15) return std::nullopt;
        return Integer(Effective(placement.rawcode, "nbmmIcon", "UnitUI", ui_), placement.rawcode) ? std::optional<std::int32_t>{1} : std::nullopt;
    }
private:
    Context& context_;
    Table data_, balance_, ui_, abilities_, abilityData_, metadata_;
    std::optional<Table> items_;
    std::unique_ptr<NativeObjectDocument> units_, nativeAbilities_;
    std::optional<ObjectFieldSchema> schema_;

    static std::int32_t Integer(std::string_view text, std::string_view object) {
        std::int32_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            Fail("TERRAIN_MMP_OBJECT_VALUE", "MMP字段不是完整Int32：" + std::string(object));
        return value;
    }
    const ObjectRecord* Record(const NativeObjectDocument* document, ObjectType type, std::string_view id) const {
        return document ? document->FindObject({type, std::string(id)}) : nullptr;
    }
    const ObjectFieldDefinition& Field(std::string_view key, std::string_view table) const {
        if (!schema_) Fail("TERRAIN_MMP_METADATA", "原生单位覆盖缺少同次实际元数据。");
        const ObjectFieldDefinition* found = nullptr;
        for (const auto& field : schema_->Definitions()) if (field.objectType == ObjectType::Unit
            && Text::LowerAscii(field.sourceField) == Text::LowerAscii(key)
            && Text::LowerAscii(field.sourceTable) == Text::LowerAscii(table)) {
            if (found) Fail("TERRAIN_MMP_METADATA", "MMP字段元数据有多个候选：" + std::string(key));
            found = &field;
        }
        if (!found || found->repeat || found->data || found->appendIndex || found->effectIndex || found->index != -1)
            Fail("TERRAIN_MMP_METADATA", "MMP需要唯一非重复标量元数据地址：" + std::string(key));
        // 标准schema会过滤坏slk地址；原始唯一候选也必须与其一一对应。
        std::size_t rawCandidates = 0;
        for (const auto& [id, row] : metadata_.rows) {
            (void)row;
            const auto* rawField = metadata_.Field(id, "field"), *rawTable = metadata_.Field(id, "slk");
            if (rawField && rawTable && Text::LowerAscii(*rawField) == Text::LowerAscii(key)
                && Text::LowerAscii(*rawTable) == Text::LowerAscii(table)) {
                if (++rawCandidates != 1 || id != found->fieldId)
                    Fail("TERRAIN_MMP_METADATA", "MMP原始元数据字段不唯一或未可靠绑定。");
            }
        }
        if (rawCandidates != 1) Fail("TERRAIN_MMP_METADATA", "MMP缺少实际原始元数据地址。");
        if (!found->useSpecific.empty() || !found->notSpecific.empty())
            Fail("TERRAIN_MMP_METADATA", "MMP覆盖尚不解释对象选择器。");
        return *found;
    }
    std::string Effective(std::string_view rawcode, std::string_view key, std::string_view table,
        const Table& base) const {
        const auto* record = Record(units_.get(), ObjectType::Unit, rawcode);
        std::string_view parent = rawcode;
        if (record && record->parentRawcode) {
            parent = *record->parentRawcode;
            if (Record(units_.get(), ObjectType::Unit, parent))
                Fail("TERRAIN_MMP_INHERITANCE", "MMP暂不猜测有父覆盖或多层自定义单位的字段继承。");
        }
        if (!data_.rows.contains(parent))
            Fail("TERRAIN_MMP_OBJECT_UNKNOWN", "MMP单位没有唯一标准定义：" + std::string(rawcode));
        if (record) {
            const auto& field = Field(key, table);
            const ObjectModification* selected = nullptr;
            for (const auto& modification : record->modifications) if (modification.key.fieldId == field.fieldId) {
                if (selected || modification.valueWasOpaque || modification.key.level.value_or(0)
                    || modification.key.dataIndex.value_or(0) || modification.key.variation.value_or(0)
                    || units_->ModificationValueType(record->key, modification.key) != field.nativeValueType)
                    Fail("TERRAIN_MMP_OBJECT_TYPE", "MMP原生覆盖含重复地址、索引或错误wire类型。");
                selected = &modification;
            }
            if (selected) {
                if (key == "isbldg" || key == "nbmmIcon") {
                    const auto* integer = std::get_if<std::int32_t>(&selected->value);
                    if (!integer || field.nativeValueType != NativeValueType::Int32)
                        Fail("TERRAIN_MMP_OBJECT_TYPE", "MMP整数覆盖不是实际Int32。");
                    return std::to_string(*integer);
                }
                const auto* string = std::get_if<std::string>(&selected->value);
                if (!string || field.nativeValueType != NativeValueType::String)
                    Fail("TERRAIN_MMP_OBJECT_TYPE", "MMP文本覆盖不是实际String。");
                return *string;
            }
        }
        const auto* value = base.Field(parent, key);
        if (!base.columns.contains(Text::LowerAscii(key)))
            Fail("TERRAIN_MMP_TABLE_COLUMN", "MMP实际表未声明所需字段列：" + std::string(key));
        // 410410的缺单元格整数getter返回0；文本getter对应空字符串。
        if (!value) return key == "isbldg" || key == "nbmmIcon" ? "0" : "";
        return *value;
    }
    bool GoldMine(std::string_view rawcode) const {
        const auto list = Effective(rawcode, "abilList", "UnitAbilities", abilities_);
        if (list.size() > 127) Fail("TERRAIN_MMP_ABILITY_LIST", "MMP能力列表超过原版128字节getter的已支持无截断域。");
        // 500F70传给Storm504的真实分隔字符集，'_'也是空能力哨兵。
        std::size_t at = 0;
        while ((at = list.find_first_not_of(" ,-_\t", at)) != std::string::npos) {
            const auto end = list.find_first_of(" ,-_\t", at);
            const auto token = std::string_view(list).substr(at, end == std::string::npos ? list.size() - at : end - at);
            at = end == std::string::npos ? list.size() : end;
            if (token.size() != 4) Fail("TERRAIN_MMP_ABILITY_LIST", "MMP能力列表需要完整四字节ID，不猜测截断或别名。");
            if (token == "Agld" || token == "Abgm" || token == "Aegm") return true;
            auto base = token;
            if (const auto* object = Record(nativeAbilities_.get(), ObjectType::Ability, token); object && object->parentRawcode) {
                base = *object->parentRawcode;
                if (Record(nativeAbilities_.get(), ObjectType::Ability, base))
                    Fail("TERRAIN_MMP_INHERITANCE", "MMP暂不解释多层自定义能力继承。");
            }
            const auto* code = abilityData_.Field(base, "code");
            if (!code || code->size() != 4)
                Fail("TERRAIN_MMP_ABILITY_UNKNOWN", "MMP能力没有明确的实际基础code：" + std::string(token));
            if (*code == "Agld" || *code == "Abgm" || *code == "Aegm") return true;
        }
        return false;
    }
};

Text::ProfileSections Profile(Context& context, const ArchiveExactMember& file) {
    if (!file.present || file.bytes.empty()) Fail("TERRAIN_MMP_PROFILE_MISSING", "MMP缺少实际非空配置：" + file.name);
    context.ParseReserve(file.bytes.size());
    Text::ProfileSections output;
    Text::VisitProfileFields(TextBytes(file.bytes), [&](std::string_view section, std::string_view key, std::string_view value) {
        if (!output[std::string(section)].emplace(std::string(key), std::string(value)).second)
            Fail("TERRAIN_MMP_PROFILE_DUPLICATE", "MMP配置同节同键重复，不能猜获胜定义：" + file.name);
    });
    return output;
}

std::optional<std::array<double, 4>> CameraMargins(Context& context, const MapSettings& settings) {
    if (settings.flags & 0x400u) return {};
    const std::vector<std::string> names{"UI/MiscData.txt", "UI/WorldEditData.txt"};
    context.Phase(names);
    Text::ProfileSections configuration;
    for (const auto path : {"UI/MiscData.txt", "UI/WorldEditData.txt"}) {
        auto profile = Profile(context, context.Required(path, true));
        for (auto& [section, fields] : profile)
            for (auto& [key, value] : fields) configuration[section][key] = std::move(value);
    }
    const auto group = configuration.find("cameramargins");
    if (group == configuration.end()) Fail("TERRAIN_MMP_CAMERA_CONFIG", "实际配置未声明CameraMargins。");
    std::array<double, 4> output{};
    constexpr std::array<std::string_view, 4> keys{"left", "right", "bottom", "top"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto field = group->second.find(std::string(keys[i]));
        if (field == group->second.end()) Fail("TERRAIN_MMP_CAMERA_CONFIG", "CameraMargins缺少实际字段。");
        const auto text = std::string_view(field->second);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output[i]);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || !std::isfinite(output[i]) || output[i] < 0 || output[i] > 65536)
            Fail("TERRAIN_MMP_CAMERA_CONFIG", "CameraMargins不是完整有界非负数。");
    }
    return output;
}

std::map<std::int32_t, std::array<std::uint8_t, 4>> TeamColors(Context& context,
    std::span<const UnitPlacement> units) {
    std::set<std::int32_t> owners;
    for (const auto& unit : units) if (unit.rawcode == "sloc") {
        if (unit.owner < 0 || unit.owner >= 16)
            Fail("TERRAIN_MMP_START_OWNER", "开始位置owner超过原版16色缓存的安全域。");
        owners.insert(unit.owner);
    }
    if (owners.empty()) return {};
    const auto skins = Profile(context, context.Startup("UI/war3skins.txt"));
    const auto group = skins.find("default");
    if (group == skins.end() || !group->second.contains("teamcolor") || group->second.at("teamcolor").empty())
        Fail("TERRAIN_MMP_TEAM_COLOR_CONFIG", "游戏启动war3skins Default未声明实际TeamColor前缀。");
    const auto& prefix = group->second.at("teamcolor");
    if (prefix.size() > 200) Fail("TERRAIN_MMP_TEAM_COLOR_CONFIG", "TeamColor前缀超过原版260字节路径的安全域。");
    std::map<std::int32_t, std::array<std::string, 2>> paths;
    std::vector<std::string> names;
    for (const auto owner : owners) {
        const auto number = (owner < 10 ? "0" : "") + std::to_string(owner);
        auto& entry = paths[owner]; entry = {Canonical(prefix + number + ".tga"), Canonical(prefix + number + ".blp")};
        names.push_back(entry[0]); names.push_back(entry[1]);
    }
    context.Phase(names);
    std::map<std::int32_t, std::array<std::uint8_t, 4>> colors;
    for (const auto& [owner, entry] : paths) {
        // 4EFCA0先尝试TGA再BLP。已存在却损坏的高层文件明确拒绝，不能偷偷退回低层。
        const auto& tga = context.Read(entry[0], true);
        const auto& file = tga.present ? tga : context.Required(entry[1], true);
        Charge(context.workingBytes, 1024u * 1024u, context.limits.maximumWorkingBytes, 8);
        const auto image = Take(Image::DecodeTexture(file.name, file.bytes, 1024u * 1024u));
        if (!image.Valid()) Fail("TERRAIN_MMP_TEAM_COLOR_IMAGE", "TeamColor未解码为完整图像。");
        colors.emplace(owner, Take(AverageNativeMinimapTeamColor(image.rgba)));
    }
    return colors;
}

struct Captured {
    Bytes bytes;
    std::vector<TerrainMinimapMarkerDependency> dependencies;
    std::size_t count = 0;
};
Captured CaptureMarkers(const MapArchiveSession& source, const std::filesystem::path& gameRoot,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainMinimapMarkerLimits& limits) {
    Validate(limits);
    Context context(source, gameRoot, limits, replacements, removals);
    const std::vector<std::string> initial{"war3map.w3e", "war3map.w3i", "war3mapUnits.doo"};
    context.Phase(initial);
    const auto& terrainFile = context.Required(initial[0], false);
    context.ParseReserve(terrainFile.bytes.size());
    TerrainReadLimits terrainLimits;
    terrainLimits.maxBytes = limits.maximumFileBytes;
    terrainLimits.maxDecodedBytes = std::min(terrainLimits.maxDecodedBytes, limits.maximumWorkingBytes - context.workingBytes);
    const auto terrain = Take(ParseMapTerrain(terrainFile.bytes, terrainLimits));
    const auto grid = terrain.Snapshot();
    if (!grid || grid->header.version != 11) Fail("TERRAIN_MMP_TERRAIN_LAYOUT", "正式MMP需要经典v11 W3E。");
    context.tileset = grid->header.mainTileset;
    const auto& settingsFile = context.Required(initial[1], false);
    context.ParseReserve(settingsFile.bytes.size());
    const auto settings = Take(MapSettings::Parse(settingsFile.bytes));
    const auto& unitFile = context.Read(initial[2], false);
    UnitPlacements placements;
    if (unitFile.present) {
        context.ParseReserve(unitFile.bytes.size());
        PlacementReadLimits unitLimits;
        unitLimits.maxBytes = limits.maximumFileBytes; unitLimits.maxPlacements = limits.maximumUnits;
        unitLimits.maxNestedRecords = limits.maximumUnits * 8;
        placements = Take(ParseUnitPlacements(unitFile.bytes, unitLimits));
        if (!IsClassicPlacementVersion(placements.version, placements.subversion))
            Fail("TERRAIN_MMP_UNITS_LAYOUT", "正式MMP只消费完整经典无皮肤单位表。");
    }
    const auto playable = Take(NativeMinimapPlayableRectangle(*grid, settings, CameraMargins(context, settings)));
    const auto frame = Take(NativeMinimapFrame(*grid, playable));
    std::vector<NativeMinimapMarkerCandidate> candidates;
    Charge(context.workingBytes, placements.units.size(), limits.maximumWorkingBytes, sizeof(NativeMinimapMarkerCandidate));
    candidates.reserve(placements.units.size());
    const auto append = [&](const UnitPlacement& placement, std::int32_t type, std::array<std::uint8_t, 4> color) {
        if (candidates.size() >= limits.markers.maxMarkers || limits.markers.maxBytes < 8
            || candidates.size() >= (limits.markers.maxBytes - 8) / 16)
            Fail("TERRAIN_MMP_BUDGET", "MMP最终记录超过数量或wire预算。");
        candidates.push_back({type, {placement.position[0], placement.position[1]}, color});
    };
    const bool ordinary = std::any_of(placements.units.begin(), placements.units.end(), [](const auto& unit) {
        return unit.rawcode != "sloc";
    });
    if (ordinary) {
        const std::vector<std::string> semantic{"UI/UnitEditorData.txt", "Units/UnitData.slk", "Units/UnitBalance.slk",
            "Units/UnitUI.slk", "Units/UnitAbilities.slk", "Units/AbilityData.slk", "Units/UnitMetaData.slk",
            "Units/ItemData.slk", "war3map.w3u", "war3map.w3a"};
        context.Phase(semantic);
        UnitSemantics semantics(context);
        for (const auto& unit : placements.units)
            if (const auto type = semantics.Type(unit)) append(unit, *type, {255, 255, 255, 255});
    }
    const auto colors = TeamColors(context, placements.units);
    for (const auto& unit : placements.units) if (unit.rawcode == "sloc") append(unit, 2, colors.at(unit.owner));
    Charge(context.workingBytes, 8 + candidates.size() * 16, limits.maximumWorkingBytes, 2);
    const auto generated = Take(BuildNativeMinimapMarkers(frame, candidates, limits.markers));
    Captured output;
    output.bytes = Take(SerializeMapMinimapMarkers(generated, limits.markers)); output.count = generated.records.size();
    for (auto& [key, dependency] : context.facts) { (void)key; output.dependencies.push_back(std::move(dependency)); }
    Check(context.guard.Verify());
    return output;
}

Status Failed(std::vector<Diagnostic> diagnostics) { return Status{std::move(diagnostics)}; }
Status AllocationFailure() {
    return Status::Failure(kSubsystem, "TERRAIN_MMP_ALLOCATION", "MMP捕获分配失败，未发布部分记录或保存资格。");
}
void RequireInstalled(const MapArchiveSession& source, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, std::span<const std::byte> expected) {
    const auto name = Canonical("war3map.mmp");
    for (const auto& removal : removals) {
        const auto path = Resources::CanonicalImportPath(removal);
        if (path.Succeeded() && path.value && *path.value == name)
            Fail("TERRAIN_MMP_OUTPUT_REMOVED", "正式MMP不能同时列为删除。");
    }
    const MemberReplacement* output = nullptr;
    for (const auto& member : replacements) {
        const auto path = Resources::CanonicalImportPath(member.name);
        if (!path.Succeeded() || !path.value || *path.value != name) continue;
        if (output) Fail("TERRAIN_MMP_FINAL_CONFLICT", "最终视图有重复MMP。");
        output = &member;
    }
    if (!output || !std::equal(expected.begin(), expected.end(), output->data.begin(), output->data.end()))
        Fail("TERRAIN_MMP_OUTPUT_MISMATCH", "最终MMP未安装或不等于本次实际来源生成的完整结果。");
    Check(source.VerifySourceUnchanged());
}

} // namespace

Result<std::shared_ptr<const TerrainMinimapMarkers>> TerrainMinimapMarkers::Capture(
    const MapArchiveSession& source, const std::filesystem::path& gameRoot,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainMinimapMarkerLimits& limits) {
    using Output = std::shared_ptr<const TerrainMinimapMarkers>;
    try {
        auto captured = CaptureMarkers(source, gameRoot, replacements, removals, limits);
        auto owner = std::shared_ptr<TerrainMinimapMarkers>(new TerrainMinimapMarkers);
        owner->sourceSha256_ = source.SourceSha256(); owner->gameRoot_ = gameRoot; owner->limits_ = limits;
        owner->bytes_ = std::move(captured.bytes); owner->inputs_ = std::move(captured.dependencies);
        owner->markerCount_ = captured.count;
        return Result<Output>::Success(std::move(owner));
    } catch (const Failure& failure) { Result<Output> result; result.diagnostics = failure.diagnostics; return result; }
    catch (const std::bad_alloc&) { Result<Output> result; result.diagnostics = AllocationFailure().diagnostics; return result; }
    catch (const std::length_error&) { Result<Output> result; result.diagnostics = AllocationFailure().diagnostics; return result; }
}

Status TerrainMinimapMarkers::VerifyFrozenOutput(const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    try {
        if (source.SourceSha256() != sourceSha256_)
            Fail("TERRAIN_MMP_SOURCE_STALE", "MMP源归档SHA已变化。");
        RequireInstalled(source, replacements, removals, bytes_);
        const auto current = CaptureMarkers(source, gameRoot_, replacements, removals, limits_);
        if (current.dependencies != inputs_ || current.bytes != bytes_)
            Fail("TERRAIN_MMP_DEPENDENCY_STALE", "MMP最终来源、游戏winner或正文与生成时不同。");
        return Status::Ok();
    } catch (const Failure& failure) { return Failed(failure.diagnostics); }
    catch (const std::bad_alloc&) { return AllocationFailure(); }
    catch (const std::length_error&) { return AllocationFailure(); }
}

Status TerrainMinimapMarkers::VerifyReadback(const MapArchiveSession& saved) const {
    try {
        const std::vector<std::string> names{"war3map.mmp"};
        ArchiveExactMemberLimits limits;
        limits.memberCount = 1; limits.memberBytes = limits.totalBytes = limits_.markers.maxBytes;
        const auto members = Take(saved.CaptureExactMembers(names, limits));
        if (!members[0].present || members[0].bytes != bytes_)
            Fail("TERRAIN_MMP_READBACK_MISMATCH", "真实回读MMP缺失或正文不同。");
        const auto current = CaptureMarkers(saved, gameRoot_, {}, {}, limits_);
        if (current.dependencies != inputs_ || current.bytes != bytes_)
            Fail("TERRAIN_MMP_READBACK_STALE", "真实回读MMP的依赖正文或游戏winner不同。");
        return Status::Ok();
    } catch (const Failure& failure) { return Failed(failure.diagnostics); }
    catch (const std::bad_alloc&) { return AllocationFailure(); }
    catch (const std::length_error&) { return AllocationFailure(); }
}

} // namespace War3Map::Services
