#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"
#include "War3MapCore/MapData/PlacementInitializationRules.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Text/SlkCells.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
using Snapshot = Trigger::MapObjectReferenceSnapshot;
using CaptureResult = Result<std::shared_ptr<const Snapshot>>;
using Input = Trigger::MapObjectReferenceInput;
using Reader = MapObjectReferenceCapture::Reader;

constexpr std::string_view kSubsystem = "MapObjectReference";
constexpr std::array<std::string_view, 4> kNames{
    "war3map.w3u", "war3map.w3t", "Units/UnitData.slk", "Units/ItemData.slk"};
constexpr std::array<std::string_view, 4> kRoles{
    "native-unit", "native-item", "standard-unit", "standard-item"};

struct CaptureFailure { std::vector<Diagnostic> diagnostics; };
struct ClassEvidence {
    bool ambiguous = false;
    std::vector<std::string> sources;
};

struct CaptureState {
    std::vector<Input> inputs;
    std::vector<Trigger::MapObjectReference> references;
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, ClassEvidence> units, items;

    void Diagnose(DiagnosticSeverity severity, std::string code, std::string message,
        std::string_view source = {}) {
        Diagnostic diagnostic{severity, std::string(kSubsystem), std::move(code), std::move(message), {}};
        if (!source.empty()) diagnostic.origin = OriginSpan{std::filesystem::path(std::u8string(source.begin(), source.end())), 0, 0, {}};
        diagnostics.push_back(std::move(diagnostic));
    }
    [[noreturn]] void Reject(std::string code, std::string message, std::string_view source = {}) {
        Diagnose(DiagnosticSeverity::Error, std::move(code), std::move(message), source);
        throw CaptureFailure{std::move(diagnostics)};
    }
    void Append(std::vector<Diagnostic> values, std::string_view source = {}) {
        for (auto& diagnostic : values) {
            if (!source.empty()) {
                if (!diagnostic.origin) diagnostic.origin = OriginSpan{};
                diagnostic.origin->source = std::filesystem::path(std::u8string(source.begin(), source.end()));
            }
            diagnostics.push_back(std::move(diagnostic));
        }
    }
    void Hash(Input& input) {
        if (!input.present) return;
        auto hash = Sha256(input.bytes);
        if (!hash.Succeeded()) {
            Append(std::move(hash.diagnostics), input.source);
            Reject("MAP_OBJECT_REFERENCE_HASH_FAILED", "无法绑定捕获输入的实际字节。", input.source);
        }
        input.sha256 = std::move(*hash.value);
    }
};

std::string MapSource(const MapDocument& map, std::string_view member) {
    const auto path = map.SourcePath().generic_u8string();
    if (path.empty()) return std::string(member);
    return std::string(reinterpret_cast<const char*>(path.data()), path.size()) + "!/" + std::string(member);
}

void CapturePlacements(CaptureState& state, const MapDocument& map) {
    const auto& member = map.Member(MapDataKind::Units);
    Input input{"placement-units", MapSource(map, member.memberName), false, {}, {}};
    if (member.state == MapDataState::Missing && member.sources.empty()) {
        state.Diagnose(DiagnosticSeverity::Warning, "MAP_OBJECT_REFERENCE_MEMBER_MISSING",
            "没有预放单位成员；未建立任何预放对象资格。", input.source);
    } else {
        // MapDocument 已证完整布局、仅创建编号重复时，可逐项剔除歧义项；其余Partial不猜测。
        const bool duplicatesOnly = member.state == MapDataState::Partial && !member.diagnostics.empty()
            && std::all_of(member.diagnostics.begin(), member.diagnostics.end(), [](const Diagnostic& diagnostic) {
                return diagnostic.severity != DiagnosticSeverity::Error && diagnostic.code == "MAP_IDENTITY_AMBIGUOUS";
            });
        state.Append(member.diagnostics);
        if ((member.state != MapDataState::Ready && !duplicatesOnly) || member.sources.size() != 1 || !map.Units())
            state.Reject("MAP_OBJECT_REFERENCE_MEMBER_UNQUALIFIED", "预放成员来源、结构或布局不唯一，不能授予引用资格。", input.source);
        const auto& source = member.sources.front();
        if (source.data.size() > MapData::PlacementReadLimits{}.maxBytes)
            state.Reject("MAP_OBJECT_REFERENCE_INPUT_BUDGET", "预放成员超过既有读取预算。", input.source);
        input.source = MapSource(map, source.name);
        input.present = true;
        input.bytes = source.data;
        state.Hash(input);
    }
    state.inputs.push_back(std::move(input));
}

void CaptureInput(CaptureState& state, const Reader& reader, std::size_t index) {
    Input input{std::string(kRoles[index]), std::string(kNames[index]), false, {}, {}};
    if (!reader) {
        input.source = "unconfigured:" + input.source;
        state.Diagnose(DiagnosticSeverity::Warning, "MAP_OBJECT_REFERENCE_STANDARD_UNCONFIGURED",
            "没有标准分类表读取器；仅真实native类别可提供正向资格。", input.source);
    } else {
        Result<MapObjectReferenceSource> read;
        try {
            read = reader(kNames[index]);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            state.Reject("MAP_OBJECT_REFERENCE_SOURCE_READ_FAILED", "分类来源读取器抛出异常，未回退到低层。", input.source);
        }
        const bool valid = read.Succeeded();
        state.Append(std::move(read.diagnostics));
        if (!valid || !read.value)
            state.Reject("MAP_OBJECT_REFERENCE_SOURCE_READ_FAILED", "分类来源读取失败，未回退到低层。", input.source);
        if (read.value->source.empty())
            state.Reject("MAP_OBJECT_REFERENCE_SOURCE_IDENTITY_MISSING", "分类读取结果缺少真实来源身份。", input.source);
        input.source = std::move(read.value->source);
        input.scope = read.value->scope;
        if (input.source.size() > ArchiveExactMemberLimits{}.totalPathBytes)
            state.Reject("MAP_OBJECT_REFERENCE_INPUT_BUDGET", "分类来源身份超过既有路径预算。");
        input.present = read.value->bytes.has_value();
        if (input.present) {
            const auto maximum = index < 2 ? MapData::PlacementReadLimits{}.maxBytes
                : ObjectData::CatalogLoadOptions{}.maximumFileBytes;
            if (read.value->bytes->size() > maximum)
                state.Reject("MAP_OBJECT_REFERENCE_INPUT_BUDGET", "分类输入超过既有成员读取预算。", input.source);
            input.bytes = std::move(*read.value->bytes);
            state.Hash(input);
        } else {
            state.Diagnose(DiagnosticSeverity::Warning, "MAP_OBJECT_REFERENCE_CLASSIFICATION_MISSING",
                "分类来源明确缺失；缺失不证明对象属于另一类别。", input.source);
        }
    }
    state.inputs.push_back(std::move(input));
}

void ReadNative(CaptureState& state, std::size_t inputIndex, ObjectData::ObjectType type) {
    const auto& input = state.inputs[inputIndex];
    if (!input.present) return;
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(input.bytes);
    const auto* document = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
    const bool valid = parsed.Succeeded() && document && document->IsStructurallySafe();
    // v1/v3只读提示不影响已明确的类别；其他语义歧义不能借只读解析变成资格。
    const bool knownWarnings = std::all_of(parsed.diagnostics.begin(), parsed.diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity != DiagnosticSeverity::Error
            && (diagnostic.code == "native-v1-unit-read-only" || diagnostic.code == "native-v1-item-read-only"
                || diagnostic.code == "native-v3-read-only" || diagnostic.code == "native-object-id-duplicate");
    });
    state.Append(std::move(parsed.diagnostics), input.source);
    if (!valid || !knownWarnings)
        state.Reject("MAP_OBJECT_REFERENCE_NATIVE_UNQUALIFIED", "native物编结构或身份不明确，未采用标准表回退。", input.source);
    auto& classes = type == ObjectData::ObjectType::Unit ? state.units : state.items;
    std::set<std::string> seen;
    for (const auto& object : document->Objects()) {
        auto& evidence = classes[object.key.rawcode];
        if (!seen.insert(object.key.rawcode).second) evidence.ambiguous = true;
        else evidence.sources.push_back(input.source);
    }
}

std::string_view SlkText(const Input& input) {
    std::string_view text;
    if (!input.bytes.empty()) text = {reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size()};
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}

void ReadStandard(CaptureState& state) {
    bool present = false, hasIdentityRows = false;
    for (std::size_t i = 3; i < 5; ++i) {
        const auto& input = state.inputs[i];
        if (!input.present) continue;
        present = true;
        // Catalog按rawcode合并行；先复用同一词法解析器标记同表重名，不能让合并掩盖歧义。
        auto cells = Text::ParseSlkCells(SlkText(input), Text::SlkCellMode::CatalogStrict);
        if (!cells.Succeeded()) {
            state.Append(std::move(cells.diagnostics), input.source);
            state.Reject("MAP_OBJECT_REFERENCE_STANDARD_UNQUALIFIED", "标准分类表不完整或无效，未回退到其他来源。", input.source);
        }
        std::set<std::string> seen;
        auto& classes = i == 3 ? state.units : state.items;
        for (const auto& [rowNumber, row] : *cells.value) {
            if (rowNumber == 1) continue;
            const auto id = row.find(1);
            if (id == row.end() || id->second.size() != 4) continue;
            hasIdentityRows = true;
            if (!seen.insert(id->second).second) classes[id->second].ambiguous = true;
        }
    }
    if (!present) return;
    ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectData::ObjectType::Unit, {std::string(kNames[2])}, {}},
        {ObjectData::ObjectType::Item, {std::string(kNames[3])}, {}}};
    options.stringFiles.clear();
    auto catalog = ObjectData::StandardObjectCatalog::Load([&state](const std::string& name) {
        for (std::size_t i = 3; i < 5; ++i) {
            if (name == kNames[i - 1] && state.inputs[i].present)
                return Result<std::vector<std::byte>>::Success(state.inputs[i].bytes);
        }
        return Result<std::vector<std::byte>>::Failure(std::string(kSubsystem),
            "MAP_OBJECT_REFERENCE_STANDARD_MISSING", "已捕获的标准分类槽缺失，不尝试磁盘或其他表。");
    }, options);
    if (!catalog.Succeeded()) {
        // 已完整解析的空身份表没有可授权对象；不能让它阻塞真实native类别。
        if (!hasIdentityRows && std::all_of(catalog.diagnostics.begin(), catalog.diagnostics.end(), [](const Diagnostic& diagnostic) {
                return diagnostic.code == "catalog-empty";
            })) return;
        state.Append(std::move(catalog.diagnostics));
        state.Reject("MAP_OBJECT_REFERENCE_STANDARD_UNQUALIFIED", "标准目录未能由捕获的真实分类表建立。");
    }
    for (const auto& object : catalog.value->Entries()) {
        const bool unit = object.key.type == ObjectData::ObjectType::Unit;
        auto& evidence = (unit ? state.units : state.items)[object.key.rawcode];
        evidence.sources.push_back(state.inputs[unit ? 3 : 4].source);
    }
    // 名称/profile不参与类型资格；缺失已逐槽记录，不能把展示名warning算解析失败。
}

void BuildReferences(CaptureState& state, const MapDocument& map) {
    if (!state.inputs.front().present) return;
    const auto& placements = map.Units()->units;
    const auto& member = state.inputs.front();
    std::map<std::int32_t, std::size_t> identities;
    for (const auto& placement : placements) ++identities[placement.creationId];
    std::set<std::string> reportedClasses;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto& placement = placements[i];
        const auto location = "预放记录[" + std::to_string(i) + "]：";
        const auto omit = [&](std::string code, std::string message) {
            state.Diagnose(DiagnosticSeverity::Warning, std::move(code), location + std::move(message), member.source);
        };
        if (placement.creationId < 0 || identities.at(placement.creationId) != 1) {
            omit("MAP_OBJECT_REFERENCE_PLACEMENT_AMBIGUOUS", "创建编号非法或重复，所有同编号实例均不授权。");
            continue;
        }
        if (placement.rawcode == "sloc" || MapData::IsRandomPlacementMarker(placement.rawcode)
            || !MapData::IsKnownFixedPlacementConfig(placement.random)) {
            omit("MAP_OBJECT_REFERENCE_PLACEMENT_UNSUPPORTED", "出生点、随机占位或未证实的放置配置不授予固定对象引用资格。");
            continue;
        }
        const auto unit = state.units.find(placement.rawcode), item = state.items.find(placement.rawcode);
        const bool hasUnit = unit != state.units.end() && !unit->second.sources.empty();
        const bool hasItem = item != state.items.end() && !item->second.sources.empty();
        const bool duplicate = (unit != state.units.end() && unit->second.ambiguous)
            || (item != state.items.end() && item->second.ambiguous);
        if (duplicate || hasUnit == hasItem) {
            if (reportedClasses.insert(placement.rawcode).second) {
                omit(duplicate || hasUnit ? "MAP_OBJECT_REFERENCE_CLASSIFICATION_AMBIGUOUS" : "MAP_OBJECT_REFERENCE_CLASSIFICATION_UNKNOWN",
                    duplicate || hasUnit ? "类别来源重复或unit/item冲突，未选择某一来源放行。"
                        : "没有正向证明unit或item的真实目录记录；未由非item推断unit。");
            }
            continue;
        }
        const std::string type = hasUnit ? "unit" : "item";
        auto symbol = MapData::GeneratedObjectSymbol(type, placement.rawcode, placement.creationId);
        if (!symbol.Succeeded()) {
            omit("MAP_OBJECT_REFERENCE_SYMBOL_INVALID", "真实rawcode/创建编号不能构成安全生成名。");
            continue;
        }
        const auto& evidence = hasUnit ? unit->second : item->second;
        std::string sources;
        for (const auto& source : evidence.sources) {
            if (!sources.empty()) sources += " | ";
            sources += source;
        }
        state.references.push_back({std::move(*symbol.value), type, placement.rawcode, placement.creationId,
            i, map.Member(MapDataKind::Units).memberName, member.sha256, std::move(sources)});
    }
    std::sort(state.references.begin(), state.references.end(), [](const auto& left, const auto& right) {
        return left.symbol < right.symbol;
    });
    // 独立防线：任何生成名碰撞均剔除整组，不能保留第一条作为资格。
    std::vector<Trigger::MapObjectReference> unique;
    unique.reserve(state.references.size());
    for (std::size_t first = 0; first < state.references.size();) {
        std::size_t end = first + 1;
        while (end < state.references.size() && state.references[end].symbol == state.references[first].symbol) ++end;
        if (end == first + 1) unique.push_back(std::move(state.references[first]));
        else state.Diagnose(DiagnosticSeverity::Warning, "MAP_OBJECT_REFERENCE_SYMBOL_AMBIGUOUS",
            "生成符号重复，整组引用资格均已撤销。", member.source);
        first = end;
    }
    state.references = std::move(unique);
}

Result<std::string> Identity(const CaptureState& state) {
    std::string manifest;
    const auto append = [&](std::string_view value) {
        manifest += std::to_string(value.size());
        manifest += ':';
        manifest += value;
    };
    append("forge-map-object-reference-v2");
    for (const auto& input : state.inputs) {
        append(input.role); append(input.source); append(input.present ? "present" : "missing");
        append(std::to_string(static_cast<int>(input.scope)));
        append(std::to_string(input.bytes.size())); append(input.sha256);
    }
    return Sha256Text(manifest);
}
} // namespace

Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> MapObjectReferenceCapture::Capture(
    const MapDocument& map, const Reader& nativeReader, const Reader& standardReader) {
    try {
        CaptureState state;
        if (!nativeReader) state.Reject("MAP_OBJECT_REFERENCE_READER_MISSING", "必须提供能证明native成员存在或缺失的来源读取器。");
        state.inputs.reserve(5);
        CapturePlacements(state, map);
        for (std::size_t i = 0; i < kNames.size(); ++i) CaptureInput(state, i < 2 ? nativeReader : standardReader, i);
        ReadNative(state, 1, ObjectData::ObjectType::Unit);
        ReadNative(state, 2, ObjectData::ObjectType::Item);
        ReadStandard(state);
        BuildReferences(state, map);
        auto identity = Identity(state);
        if (!identity.Succeeded()) {
            state.Append(std::move(identity.diagnostics));
            state.Reject("MAP_OBJECT_REFERENCE_HASH_FAILED", "捕获身份摘要失败。");
        }
        auto snapshot = std::shared_ptr<Snapshot>(new Snapshot);
        snapshot->identity_ = std::move(*identity.value);
        snapshot->inputs_ = std::move(state.inputs);
        snapshot->references_ = std::move(state.references);
        snapshot->diagnostics_ = std::move(state.diagnostics);
        auto result = CaptureResult::Success(snapshot);
        result.diagnostics = snapshot->diagnostics_;
        return result;
    } catch (CaptureFailure& failure) {
        CaptureResult result;
        result.diagnostics = std::move(failure.diagnostics);
        return result;
    } catch (const std::bad_alloc&) {
        return CaptureResult::Failure(std::string(kSubsystem), "MAP_OBJECT_REFERENCE_ALLOCATION", "捕获真实对象引用时内存不足。");
    } catch (const std::length_error&) {
        return CaptureResult::Failure(std::string(kSubsystem), "MAP_OBJECT_REFERENCE_LENGTH", "捕获真实对象引用时容器长度超限。");
    } catch (const std::filesystem::filesystem_error&) {
        return CaptureResult::Failure(std::string(kSubsystem), "MAP_OBJECT_REFERENCE_SOURCE_IDENTITY_INVALID", "来源路径不能构成有效的捕获身份。");
    }
}

Status MapObjectReferenceCapture::Verify(const Trigger::MapObjectReferenceSnapshot& expected,
    const MapDocument& map, const Reader& nativeReader, const Reader& standardReader) {
    auto current = Capture(map, nativeReader, standardReader);
    if (!current.Succeeded()) return {std::move(current.diagnostics)};
    if ((*current.value)->Identity() != expected.Identity())
        return Status::Failure(std::string(kSubsystem), "MAP_OBJECT_REFERENCE_STALE", "预放成员、分类来源、存在性或实际字节已变化，旧引用资格失效。");
    return {std::move(current.diagnostics)};
}
} // namespace War3Map::Services
