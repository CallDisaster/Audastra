#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using Bytes = std::vector<std::byte>;
using Row = std::map<std::string, std::string>;
constexpr std::size_t kWorkingBytes = 4u * 1024u * 1024u;
constexpr std::size_t kW3b = 0, kW3d = 1, kDestructables = 2, kDoodads = 3, kMetadata = 4, kDoodadMetadata = 5;

void Require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

std::string Diagnostics(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) text += " | " + diagnostic.code + ": " + diagnostic.message;
    return text;
}

void Check(Status result) {
    if (!result.Succeeded()) throw std::runtime_error("构造合法物编夹具失败" + Diagnostics(result.diagnostics));
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value)
        throw std::runtime_error("构造合法夹具失败" + Diagnostics(result.diagnostics));
    return std::move(*result.value);
}

std::size_t Accepted(const Result<std::size_t>& result, std::string_view label) {
    if (!result.Succeeded() || !result.value || *result.value == 0)
        throw std::runtime_error(std::string(label) + Diagnostics(result.diagnostics));
    return *result.value;
}

template<class T> void Rejected(const Result<T>& result, std::string_view code, std::string_view label) {
    const bool error = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
    if (result.Succeeded() || result.value || !error)
        throw std::runtime_error(std::string(label) + "：拒绝必须带Error且不返回部分工作额度" + Diagnostics(result.diagnostics));
    if (!code.empty() && std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    })) throw std::runtime_error(std::string(label) + "：未命中预期拒绝点 " + std::string(code) + Diagnostics(result.diagnostics));
}

Bytes TextBytes(std::string_view text) {
    Bytes bytes;
    bytes.reserve(text.size());
    for (const unsigned char character : text) bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

std::string Text(const Bytes& bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string Quote(std::string_view value) {
    std::string text = "\"";
    for (const char character : value) {
        text += character;
        if (character == '"') text += '"';
    }
    return text + '"';
}

// 全部rawcode、字段ID及表内容均为原创，不读取游戏或作者地图。
Bytes Table(std::string_view idHeader, const std::vector<Row>& rows) {
    std::vector<std::string> columns{std::string(idHeader)};
    for (const auto& row : rows) for (const auto& [name, value] : row) {
        (void)value;
        if (std::find(columns.begin(), columns.end(), name) == columns.end()) columns.push_back(name);
    }
    std::string text = "ID;PWXL\n";
    for (std::size_t column = 0; column < columns.size(); ++column)
        text += "C;X" + std::to_string(column + 1) + ";Y1;K" + Quote(columns[column]) + '\n';
    for (std::size_t row = 0; row < rows.size(); ++row) for (std::size_t column = 0; column < columns.size(); ++column) {
        const auto field = rows[row].find(columns[column]);
        if (field != rows[row].end())
            text += "C;X" + std::to_string(column + 1) + ";Y" + std::to_string(row + 2) + ";K" + Quote(field->second) + '\n';
    }
    return TextBytes(text + "E\n");
}

void InsertBeforeEnd(Bytes& bytes, std::string_view record) {
    auto text = Text(bytes);
    Require(text.ends_with("E\n"), "SLK坏夹具应从完整结束记录构造");
    text.insert(text.size() - 2, record);
    bytes = TextBytes(text);
}

void ReplaceOnce(Bytes& bytes, std::string_view from, std::string_view to) {
    auto text = Text(bytes);
    const auto at = text.find(from);
    Require(at != std::string::npos && text.find(from, at + from.size()) == std::string::npos, "坏夹具定位应唯一");
    text.replace(at, from.size(), to);
    bytes = TextBytes(text);
}

NativeObjectDocument Empty(ObjectType type = ObjectType::Destructable) {
    return Take(NativeObjectDocument::CreateEmpty(type));
}

void Modification(NativeObjectDocument& document, const std::string& rawcode, const std::string& field,
    NativeValueType type, Value value) {
    Check(document.AddModification({document.Type(), rawcode}, {field}, type, std::move(value)));
}

Bytes Encode(const NativeObjectDocument& document) {
    return Take(NativeObjectCodec(document.Type()).Serialize(document));
}

NativeObjectDocument Original(const std::string& rawcode, const std::string& field = "X001",
    NativeValueType type = NativeValueType::Int32, Value value = std::int32_t{0}) {
    auto document = Empty();
    Check(document.AddOriginalObject(rawcode));
    Modification(document, rawcode, field, type, std::move(value));
    return document;
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "坏wire夹具写入边界");
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 255u);
}

void PutId(Bytes& bytes, std::size_t offset, std::string_view id) {
    Require(id.size() == 4 && offset <= bytes.size() && bytes.size() - offset >= 4, "坏rawcode夹具写入边界");
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>(static_cast<unsigned char>(id[i]));
}

struct Fixture {
    std::vector<Row> destructables{
        {{"destructableID", "S001"}, {"walkable", "1"}, {"code", "B001"}},
        {{"destructableID", "S002"}, {"walkable", "0"}, {"code", "B002"}},
        {{"destructableID", "S003"}, {"walkable", "0"}, {"code", "B003"}}};
    std::vector<Row> doodads{{{"doodID", "D001"}, {"Name", "Synthetic doodad"}}};
    std::vector<Row> metadata{
        {{"ID", "X001"}, {"field", "walkable"}, {"slk", "DestructableData"}, {"type", "bool"},
            {"repeat", "0"}, {"data", "0"}, {"index", "-1"}, {"appendindex", "0"}, {"effectindex", "0"}},
        {{"ID", "Pth0"}, {"field", "pathTex"}, {"slk", "DestructableData"}, {"type", "pathingTexture"}}};
    std::vector<Row> doodadMetadata{
        {{"ID", "Dt00"}, {"field", "pathTex"}, {"slk", "DoodadData"}, {"type", "pathingTexture"}, {"repeat", "0"}, {"index", "0"}},
        {{"ID", "Nm00"}, {"field", "Name"}, {"slk", "DoodadData"}, {"type", "string"}, {"repeat", "0"}}};
    std::vector<ArchiveExactMember> sources;

    Fixture() {
        sources = {{"war3map.w3b", true, Encode(Empty())}, {"war3map.w3d", false, {}},
            {"Units/DestructableData.slk", true, {}}, {"Doodads/Doodads.slk", true, {}},
            {"Units/DestructableMetaData.slk", true, {}}};
        Tables();
    }
    void Tables() {
        sources[kDestructables].bytes = Table("destructableID", destructables);
        sources[kDoodads].bytes = Table("doodID", doodads);
        sources[kMetadata].bytes = Table("ID", metadata);
        if (sources.size() > kDoodadMetadata) sources[kDoodadMetadata].bytes = Table("ID", doodadMetadata);
    }
    void AddDoodadMetadata() {
        Require(sources.size() == kDoodadMetadata, "第六来源只追加一次");
        sources.push_back({"Doodads/DoodadMetaData.slk", true, Table("ID", doodadMetadata)});
    }
    void SetNative(const NativeObjectDocument& document) {
        Require(document.Type() == ObjectType::Destructable || document.Type() == ObjectType::Doodad, "夹具原生类别");
        auto& member = sources[document.Type() == ObjectType::Destructable ? kW3b : kW3d];
        member.present = true;
        member.bytes = Encode(document);
    }
    Result<std::size_t> Validate(std::initializer_list<std::string_view> ids = {"S002"}, std::size_t budget = kWorkingBytes) const {
        return ValidateNativeDestructablePathing(std::span(ids.begin(), ids.size()), sources, budget);
    }
    Result<MapData::TerrainNativeObjectPathing> Inspect(std::initializer_list<std::string_view> ids = {"D001"},
        std::size_t budget = kWorkingBytes) const {
        return InspectNativeDestructablePathing(std::span(ids.begin(), ids.size()), sources, budget);
    }
};

Fixture WalkableBridgeFixture() {
    Fixture fixture;
    fixture.destructables[0]["pathTex"] = "PathTextures\\SyntheticBridge.tga";
    fixture.destructables[0]["fixedRot"] = "-1";
    fixture.Tables();
    return fixture;
}

Fixture BridgeOriginalFixture() {
    auto fixture = WalkableBridgeFixture();
    // 实际DestructableMetaData的平面布局：index=0，无repeat/data列；字段ID不写死。
    fixture.metadata = {
        {{"ID", "X001"}, {"field", "walkable"}, {"slk", "DestructableData"}, {"type", "bool"}, {"index", "0"}},
        {{"ID", "Pth0"}, {"field", "pathTex"}, {"slk", "DestructableData"}, {"type", "pathingTexture"}, {"index", "0"}},
        {{"ID", "Rot0"}, {"field", "fixedRot"}, {"slk", "DestructableData"}, {"type", "unreal"}, {"index", "0"}}};
    fixture.Tables();
    return fixture;
}

void TypedOriginalAndMetadataIdentity() {
    for (const std::string field : {"X001", "Ab9z"}) {
        Fixture fixture;
        fixture.metadata[0]["ID"] = field;
        fixture.Tables();
        auto document = Original("S001", field);
        Modification(document, "S001", "Pth0", NativeValueType::String, std::string("synthetic;\"quoted\".tga"));
        fixture.SetNative(document);
        const auto before = fixture.sources[kW3b].bytes;
        Accepted(fixture.Validate({"S001", "S001"}), "Original真实Int32 0覆盖标准true，字段ID由metadata确定");
        Require(fixture.sources[kW3b].bytes == before, "只读分类不重写原生对象的其它字段");
    }
    Fixture fixture;
    fixture.SetNative(Original("S002", "ZZZ0"));
    Rejected(fixture.Validate(), "TERRAIN_NATIVE_OBJECT_METADATA", "未知原生字段不能借标准false放行");
    fixture.SetNative(Original("S001", "x001"));
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "原生字段ID区分大小写");
    fixture.SetNative(Original("S001", "Pth0", NativeValueType::String, std::string("0")));
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "其它字段字符串0不能借缺失标量地址冒充walkable覆盖");
}

void StandardAndSingleParentContext() {
    Fixture fixture;
    Accepted(fixture.Validate(), "没有该对象覆盖时读取显式标准false");
    fixture.SetNative(Empty(ObjectType::Doodad));
    Accepted(fixture.Validate(), "缺失W3D和合法空W3D均可完成分类");
    auto child = Empty();
    Check(child.AddCustomObject("C001", "S002"));
    fixture.SetNative(child);
    Accepted(fixture.Validate({"C001"}), "单层Custom继承没有Original覆盖的标准false");
    for (const std::string selector : {"S002", "B002"}) {
        fixture.metadata[0]["useSpecific"] = selector;
        fixture.metadata[0]["useUnit"] = "0";
        fixture.Tables();
        Accepted(fixture.Validate({"C001"}), "字段适用性使用真实标准父ID/code，不伪造单位context");
    }
    fixture.metadata[0]["useSpecific"] = "C001";
    fixture.Tables();
    Rejected(fixture.Validate({"C001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "不能用Custom新ID替代标准父context");
    fixture.metadata[0].erase("useSpecific");
    fixture.metadata[0]["notSpecific"] = "S002";
    fixture.Tables();
    Rejected(fixture.Validate({"C001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "标准父被排除时不得忽略元数据适用性");

    Fixture explicitChild;
    auto overridden = Empty();
    Check(overridden.AddCustomObject("C001", "S001"));
    Modification(overridden, "C001", "X001", NativeValueType::Int32, std::int32_t{0});
    explicitChild.SetNative(overridden);
    Accepted(explicitChild.Validate({"C001"}), "单层Custom可显式覆盖标准父true为typed0");
}

void WalkableValuesAndWireTypes() {
    Fixture fixture;
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "标准true缺少明确路径和固定角度仍拒绝");
    fixture.metadata[0]["index"] = "0"; fixture.Tables();
    for (const auto value : {std::int32_t{1}, std::int32_t{-1}, std::int32_t{2}}) {
        fixture.SetNative(Original("S002", "X001", NativeValueType::Int32, value));
        Rejected(fixture.Validate(), value == 1 ? "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED" : "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE",
            "覆盖非法值或true不能回退标准false");
    }
    for (const auto type : {NativeValueType::Real, NativeValueType::Unreal, NativeValueType::String}) {
        const Value value = type == NativeValueType::String ? Value{std::string("0")} : Value{0.0f};
        fixture.SetNative(Original("S002", "X001", type, value));
        Rejected(fixture.Validate(), "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "数值相似不能替代Int32 wire类型");
    }
    for (const std::string value : {"", "false", "-1", "2", "0tail"}) {
        Fixture invalid;
        invalid.destructables[1]["walkable"] = value;
        invalid.Tables();
        Rejected(invalid.Validate(), "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "标准walkable必须显式为0或1");
    }
    Fixture missing;
    missing.destructables[1].erase("walkable");
    missing.Tables();
    Rejected(missing.Validate(), "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "缺失标准walkable不是false");
}

void MetadataMustBeKnownAndUnambiguous() {
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"field", "otherFlag"}, {"type", "unknown-test-type"}, {"type", "real"}, {"type", "string"},
        {"slk", "Profile"}, {"slk", "OtherData"}, {"repeat", "1"}, {"data", "1"},
        {"repeat", "0garbage"}, {"data", "2147483648"}, {"index", ""}, {"appendindex", "bad"}, {"effectindex", "nan"}}) {
        Fixture fixture;
        fixture.metadata[0][key] = value;
        fixture.Tables();
        Rejected(fixture.Validate(), "TERRAIN_NATIVE_OBJECT_METADATA", "未知或损坏metadata地址不能默认成0");
    }
    Fixture duplicate;
    duplicate.metadata.push_back(duplicate.metadata[0]);
    duplicate.metadata.back()["field"] = "otherFlag";
    duplicate.Tables();
    Rejected(duplicate.Validate(), "TERRAIN_NATIVE_OBJECT_ID", "同field ID多行不能被loader覆盖");
    Fixture ambiguous;
    ambiguous.metadata.push_back(ambiguous.metadata[0]);
    ambiguous.metadata.back()["ID"] = "Alt0";
    ambiguous.Tables();
    Rejected(ambiguous.Validate(), "TERRAIN_NATIVE_OBJECT_METADATA", "不同原生ID映射walkable也必须拒绝歧义");
}

void MetadataLogicalCellsMustNotBeDecodedTwice() {
    Fixture normal;
    normal.SetNative(Original("S001"));
    Accepted(normal.Validate({"S001"}), "正常metadata仍可用实际Int32 0覆盖标准true");

    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"field", "walkable"}, {"slk", "DestructableData"}, {"type", "bool"}}) {
        // Quote已做一次合法SLK转义；这些是单元格的真实内容，不是待剥掉的语法。
        for (const auto& logical : {"\"" + value + "\"", " " + value, value + " "}) {
            Fixture changed;
            changed.metadata[0][key] = logical;
            changed.Tables();
            changed.SetNative(Original("S001"));
            Rejected(changed.Validate({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA",
                "逻辑field/slk/type的字面引号或空白不能二次解码成合法通行地址");
        }
    }
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"repeat", "0"}, {"data", "0"}, {"index", "-1"}, {"appendindex", "0"}, {"effectindex", "0"}}) {
        Fixture changed;
        changed.metadata[0][key] = "\"" + value + "\"";
        changed.Tables();
        changed.SetNative(Original("S001"));
        Rejected(changed.Validate({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA",
            "地址整数的字面引号不能被schema二次剥掉后授予保存资格");
    }
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"usespecific", "\"S001\""}, {"usespecific", "\"S001\",S003"},
        {"notspecific", "\"S002\""}, {"notspecific", "S003,\"S002\""}}) {
        Fixture changed;
        changed.metadata[0][key] = value;
        changed.Tables();
        changed.SetNative(Original("S001"));
        Rejected(changed.Validate({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA",
            "适用对象列表的字面引号不能被二次Token解释为可信rawcode");
    }

    Fixture escaped;
    escaped.metadata[0]["field"] = "WalkAble";
    escaped.metadata[0]["slk"] = "destructableDATA";
    escaped.metadata[0]["type"] = "BOOL";
    escaped.metadata[0]["displayname"] = "Synthetic \"quoted\"; label";
    escaped.metadata[0]["usespecific"] = "S001,B001";
    escaped.metadata[0]["notspecific"] = "S002,S003";
    escaped.Tables();
    escaped.SetNative(Original("S001"));
    Accepted(escaped.Validate({"S001"}), "正常大小写归一和无关说明的合法SLK引号转义不能被一并拒绝");
}

void StrictSlkStructurePrecedesLossyLoaders() {
    for (const auto member : {kDestructables, kDoodads, kMetadata}) {
        Fixture duplicateCell;
        InsertBeforeEnd(duplicateCell.sources[member].bytes, "C;X1;Y1;K\"duplicate\"\n");
        Rejected(duplicateCell.Validate(), "slk-cell-duplicate", "三个对象表都拒绝重复坐标");
    }
    Fixture duplicateId;
    duplicateId.destructables.push_back(duplicateId.destructables[1]);
    duplicateId.destructables.back()["walkable"] = "1";
    duplicateId.Tables();
    Rejected(duplicateId.Validate(), "TERRAIN_NATIVE_OBJECT_ID", "标准表重复rawcode不能覆盖为false");
    Fixture duplicateDoodad;
    duplicateDoodad.doodads.push_back(duplicateDoodad.doodads[0]);
    duplicateDoodad.Tables();
    Rejected(duplicateDoodad.Validate(), "TERRAIN_NATIVE_OBJECT_ID", "分类表重复ID不可忽略");
    Fixture duplicateHeader;
    InsertBeforeEnd(duplicateHeader.sources[kMetadata].bytes, "C;X99;Y1;K\"FIELD\"\n");
    Rejected(duplicateHeader.Validate(), "TERRAIN_NATIVE_OBJECT_COLUMN", "列名大小写归一后必须唯一");
    Fixture undeclared;
    InsertBeforeEnd(undeclared.sources[kMetadata].bytes, "C;X99;Y2;K\"hidden\"\n");
    Rejected(undeclared.Validate(), "TERRAIN_NATIVE_OBJECT_COLUMN", "未声明列不能被loader忽略");
    Fixture missingId;
    missingId.metadata[0].erase("ID");
    missingId.Tables();
    Rejected(missingId.Validate(), "TERRAIN_NATIVE_OBJECT_ID", "有数据而没有ID的metadata行必须拒绝");
    for (const std::string record : {"C;X1;Y2;K\"X001\";K\"other\"",
        "C;X1;X2;Y2;K\"X001\"", "C;X1;Y2;Y3;K\"X001\""}) {
        Fixture repeatedTag;
        ReplaceOnce(repeatedTag.sources[kMetadata].bytes, "C;X1;Y2;K\"X001\"", record);
        Rejected(repeatedTag.Validate(), "slk-record-duplicate", "同条C记录重复X/Y/K必须拒绝");
    }
    Fixture truncated;
    truncated.sources[kMetadata].bytes.resize(truncated.sources[kMetadata].bytes.size() - 2);
    Rejected(truncated.Validate(), "slk-incomplete", "缺少E的部分metadata不可接受");
    Fixture quote;
    ReplaceOnce(quote.sources[kMetadata].bytes, "C;X1;Y2;K\"X001\"", "C;X1;Y2;K\"X001");
    Rejected(quote.Validate(), "slk-quote", "未闭合引号不能静默截断");
    Fixture trailing;
    const auto extra = TextBytes("C;X1;Y9;K\"TAIL\"\n");
    trailing.sources[kMetadata].bytes.insert(trailing.sources[kMetadata].bytes.end(), extra.begin(), extra.end());
    Rejected(trailing.Validate(), "slk-trailing-data", "结束记录之后的数据不能被忽略");
}

void NativeDamageAndDuplicates() {
    for (const auto type : {ObjectType::Destructable, ObjectType::Doodad}) {
        const auto member = type == ObjectType::Destructable ? kW3b : kW3d;
        for (const auto mode : {0, 1, 2}) {
            Fixture broken;
            broken.SetNative(Empty(type));
            if (mode == 0) broken.sources[member].bytes.clear();
            else if (mode == 1) broken.sources[member].bytes.pop_back();
            else Put32(broken.sources[member].bytes, 0, 99); // 明确破坏v2文件头的版本字段。
            Rejected(broken.Validate(), {}, "坏W3B/W3D不能当不存在或合法空表");
        }
        auto objects = Empty(type);
        Check(objects.AddOriginalObject("S002"));
        Check(objects.AddOriginalObject("S003"));
        auto duplicated = Encode(objects);
        // v2空Original记录每条12字节；头8字节，第二条ID位于20。
        Require(duplicated.size() == 36, "重复对象夹具的固定v2布局");
        PutId(duplicated, 20, "S002");
        Fixture repeatedObject;
        repeatedObject.sources[member] = {repeatedObject.sources[member].name, true, std::move(duplicated)};
        Rejected(repeatedObject.Validate(), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复原生对象不能使用首条记录分类");

        auto fields = Empty(type);
        Check(fields.AddOriginalObject("S002"));
        Modification(fields, "S002", "X001", NativeValueType::Int32, std::int32_t{0});
        Modification(fields, "S002", "Pth0", NativeValueType::Int32, std::int32_t{0});
        auto repeatedBytes = Encode(fields);
        // v2首条修改位于20；W3B Int32记录16字节，W3D另带variation/data共24字节。
        const std::size_t recordBytes = type == ObjectType::Doodad ? 24 : 16;
        Require(repeatedBytes.size() == 24 + 2 * recordBytes, "重复字段夹具的固定v2布局");
        PutId(repeatedBytes, 20 + recordBytes, "X001");
        Fixture repeatedField;
        repeatedField.sources[member] = {repeatedField.sources[member].name, true, std::move(repeatedBytes)};
        Rejected(repeatedField.Validate(), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复原生字段不能取首条或末条");
    }
    Fixture wireTag;
    wireTag.SetNative(Original("S002"));
    Require(wireTag.sources[kW3b].bytes.size() == 40, "单Int32 W3B记录固定布局");
    Put32(wireTag.sources[kW3b].bytes, 24, 99); // 首条fieldId在20，wire type在24。
    Rejected(wireTag.Validate(), "native-value-type-unsupported", "未知wire type不能按Int32读");
    Fixture endMarker;
    endMarker.SetNative(Original("S002"));
    PutId(endMarker.sources[kW3b].bytes, 32, "BAD0"); // 首条Int32值之后的结束标记。
    Rejected(endMarker.Validate(), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "只允许保源的未知尾标记不能授予通行读取资格");
    Fixture classicV1;
    classicV1.SetNative(Original("S002"));
    Put32(classicV1.sources[kW3b].bytes, 0, 1); // W3B布局相同，但本单元明确限定v2。
    Rejected(classicV1.Validate(), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "v1即使codec能读也不扩张本单元范围");
}

void CrossClassAndUnsupportedInheritance() {
    Fixture sameStandard;
    sameStandard.doodads.push_back({{"doodID", "S002"}});
    sameStandard.Tables();
    Rejected(sameStandard.Validate(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "两种标准目录同名不能猜优先级");
    Fixture originalDoodad;
    auto original = Empty(ObjectType::Doodad);
    Check(original.AddOriginalObject("S002"));
    originalDoodad.SetNative(original);
    Rejected(originalDoodad.Validate(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "W3D Original别名也参与分类");
    Fixture customDoodad;
    auto child = Empty();
    Check(child.AddCustomObject("C001", "S002"));
    customDoodad.SetNative(child);
    auto doodad = Empty(ObjectType::Doodad);
    Check(doodad.AddCustomObject("C001", "D001"));
    customDoodad.SetNative(doodad);
    Rejected(customDoodad.Validate({"C001"}), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "W3D Custom新ID不得冒充可破坏物");

    for (const auto mode : {0, 1, 2, 3, 4, 5}) {
        Fixture fixture;
        auto document = Empty();
        if (mode == 0) { // 父有实际字段，即使值与标准相同也不借等价值猜继承。
            Check(document.AddOriginalObject("S002"));
            Modification(document, "S002", "X001", NativeValueType::Int32, std::int32_t{0});
            Check(document.AddCustomObject("C001", "S002"));
        } else if (mode == 1) {
            Check(document.AddCustomObject("C002", "S002"));
            Check(document.AddCustomObject("C001", "C002"));
        } else if (mode == 2) {
            Check(document.AddCustomObject("C002", "C001"));
            Check(document.AddCustomObject("C001", "C002"));
        } else if (mode == 3) Check(document.AddCustomObject("C001", "D001"));
        else if (mode == 4) Check(document.AddCustomObject("C001", "NONE"));
        else {
            Check(document.AddCustomObject("C001", "S002"));
            auto parentAlias = Empty(ObjectType::Doodad);
            Check(parentAlias.AddOriginalObject("S002"));
            fixture.SetNative(parentAlias);
        }
        fixture.SetNative(document);
        Rejected(fixture.Validate({"C001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "有覆盖/多层/循环/跨类/缺失父均不在单层标准继承范围");
    }
    Fixture shadowsStandard;
    auto shadow = Empty();
    Check(shadow.AddCustomObject("S002", "S003"));
    shadowsStandard.SetNative(shadow);
    Rejected(shadowsStandard.Validate(), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Custom不能同时命中标准自身定义");
    Fixture noBase;
    noBase.SetNative(Original("NONE"));
    Rejected(noBase.Validate({"NONE"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Original记录不是标准对象定义本身");
    Fixture unknown;
    Rejected(unknown.Validate({"NONE"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "未知摆放不能视为零静态贡献");
    Rejected(unknown.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "普通doodad缺少明确pathTex字段仍拒绝");
    Rejected(unknown.Validate({"s002"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "rawcode大小写不能折叠");
    Rejected(unknown.Validate({"bad"}), "TERRAIN_NATIVE_OBJECT_ID", "坏摆放rawcode不能跳过");
}

void MissingW3bStillRequiresUniqueClassification() {
    Fixture standard;
    standard.sources[kW3b] = {"war3map.w3b", false, {}};
    Accepted(standard.Validate({"S002"}), "准确Missing W3B允许实际标准walkable 0对象");
    Rejected(standard.Validate({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED",
        "Missing W3B不能把标准walkable 1视为没有静态贡献");

    Fixture sameStandard;
    sameStandard.sources[kW3b] = {"war3map.w3b", false, {}};
    sameStandard.doodads.push_back({{"doodID", "S002"}});
    sameStandard.Tables();
    Rejected(sameStandard.Validate(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS",
        "没有W3B时仍须拒绝两种标准目录同名");
    for (const bool custom : {false, true}) {
        Fixture sameNative;
        sameNative.sources[kW3b] = {"war3map.w3b", false, {}};
        auto document = Empty(ObjectType::Doodad);
        if (custom) Check(document.AddCustomObject("S002", "D001"));
        else Check(document.AddOriginalObject("S002"));
        sameNative.SetNative(document);
        Rejected(sameNative.Validate(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS",
            "没有W3B时仍须读取W3D的Original和Custom同名分类来源");
    }
}

void StandardDoodadsRequireExplicitDisabledPathing() {
    for (const std::string path : {"", "_", "none"}) {
        Fixture fixture;
        fixture.doodads[0]["pathTex"] = path;
        fixture.Tables();
        Accepted(fixture.Validate({"D001", "S002", "D001"}), "明确禁用pathTex的标准doodad可与非walkable破坏物混合");
        fixture.sources[kW3b] = {"war3map.w3b", false, {}};
        Accepted(fixture.Validate({"D001"}), "无W3B时仍核对明确的doodad字段");
        const auto work = Accepted(fixture.Validate({"D001"}), "新域工作预算基线");
        Accepted(fixture.Validate({"D001"}, work), "新域精确工作预算允许");
        Rejected(fixture.Validate({"D001"}, work - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "新域预算不足不能返回部分资格");
    }
    for (const std::string path : {"NONE", "None", " none", "none ", " _", "_\t", " ", "0",
            "\"\"", "\"_\"", "\"none\"", "PathTextures\\synthetic.tga"}) {
        Fixture fixture;
        fixture.doodads[0]["pathTex"] = path;
        fixture.doodads[0]["walkable"] = "0";
        fixture.Tables();
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED",
            "walkable=0不能代替doodad占地证明，也不能修剪/再次解码pathTex取得资格");
    }
    Fixture missingColumn;
    Rejected(missingColumn.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "缺失pathTex列不等于显式空值");
    Fixture missingCell;
    missingCell.doodads.push_back({{"doodID", "D002"}, {"pathTex", "_"}});
    missingCell.Tables();
    Rejected(missingCell.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "已有pathTex列但该行未提供单元格仍拒绝");
}

void StandardDoodadsDoNotIgnoreNativeOverridesOrOtherObjects() {
    const auto prepare = [](Fixture& fixture) {
        fixture.doodads[0]["pathTex"] = "_";
        fixture.Tables();
    };
    for (const bool unrelated : {false, true}) {
        Fixture fixture; prepare(fixture);
        auto document = Empty(ObjectType::Doodad);
        if (unrelated) Check(document.AddOriginalObject("D002"));
        fixture.SetNative(document);
        Accepted(fixture.Validate({"D001"}), "空W3D或无关对象覆盖不等于当前doodad被覆盖");
    }
    for (const unsigned mode : {0u, 1u, 2u, 3u}) {
        Fixture fixture; prepare(fixture);
        auto document = Empty(ObjectType::Doodad);
        std::string placed = "D001";
        if (mode < 2) {
            Check(document.AddOriginalObject("D001"));
            if (mode == 1) Modification(document, "D001", "dnam", NativeValueType::String, std::string("display-only"));
        } else if (mode == 2) Check(document.AddCustomObject("D001", "D002"));
        else { Check(document.AddCustomObject("C001", "D001")); placed = "C001"; }
        fixture.SetNative(document);
        Rejected(fixture.Validate({placed}), mode == 2 ? "TERRAIN_NATIVE_OBJECT_INHERITANCE" : "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED",
            "相关原生记录需要实际元数据；与标准ID冲突的Custom仍拒绝");
    }
    for (const unsigned mode : {0u, 1u, 2u}) {
        Fixture fixture; prepare(fixture);
        if (mode == 0) {
            fixture.destructables.push_back({{"destructableID", "D001"}, {"walkable", "0"}});
            fixture.Tables();
        } else {
            auto document = Empty();
            if (mode == 1) Check(document.AddOriginalObject("D001"));
            else Check(document.AddCustomObject("D001", "S002"));
            fixture.SetNative(document);
        }
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS",
            "无占地doodad同名Destructable或W3B仍不能猜类别优先级");
    }
    Fixture mixed; prepare(mixed);
    Rejected(mixed.Validate({"D001", "S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED",
        "前面的无占地doodad成功不能跳过后面的walkable破坏物");
    Rejected(mixed.Validate({"S001", "D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "混合对象拒绝不依赖顺序");
    Rejected(mixed.Validate({"D001", "NONE"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "无占地对象后面的未知rawcode仍拒绝");
}

void DoodadOriginalUsesExactMetadataAndStrings() {
    const auto original = [](std::string_view field, NativeValueType type, Value value) {
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", std::string(field), type, std::move(value)); return document;
    };
    const auto prepare = [](Fixture& fixture) {
        fixture.doodads[0]["pathTex"] = "PathTextures\\synthetic.tga";
        fixture.Tables(); fixture.AddDoodadMetadata();
    };
    for (const std::string field : {"Dt00", "Ab9Z"}) for (const std::string value : {"", "_", "none"}) {
        Fixture fixture; fixture.doodadMetadata[0]["ID"] = field;
        if (field == "Ab9Z") fixture.doodadMetadata[0]["data"] = "0";
        prepare(fixture);
        fixture.SetNative(original(field, NativeValueType::String, value));
        const auto bytes = fixture.sources[kW3d].bytes;
        const auto work = Accepted(fixture.Validate({"D001", "S002"}), "String零索引覆盖标准非空pathTex，空串不回退");
        Accepted(fixture.Validate({"D001", "S002"}, work), "六来源精确工作额度");
        Rejected(fixture.Validate({"D001", "S002"}, work - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "新增来源也计入解析工作额度");
        Require(fixture.sources[kW3d].bytes == bytes, "读取Original不改写任何原生字节");
        Rejected(fixture.Validate({"D001", "S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "Original成功后仍检查混合对象");
    }
    for (const bool displayOnly : {false, true}) {
        Fixture fixture; prepare(fixture); fixture.doodads[0]["pathTex"] = "_"; fixture.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        if (displayOnly) Modification(document, "D001", "Nm00", NativeValueType::String, std::string("display"));
        fixture.SetNative(document);
        Accepted(fixture.Validate({"D001"}), "空Original或确定无关字段回退标准显式pathTex");
        fixture.doodads[0]["pathTex"] = "PathTextures\\synthetic.tga"; fixture.Tables();
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "无目标修改不能清空标准占地");
    }
    for (const std::string value : {"PathTextures\\synthetic.tga", "NONE", "None", " ", "none ", " _", "\"\"", "\"none\""}) {
        Fixture fixture; prepare(fixture); fixture.doodads[0]["pathTex"] = "_"; fixture.Tables();
        fixture.SetNative(original("Dt00", NativeValueType::String, value));
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "显式覆盖不因非法/非空值回退或再次归一化");
    }
    Fixture wrongType; prepare(wrongType); wrongType.SetNative(original("Dt00", NativeValueType::Int32, std::int32_t{0}));
    Rejected(wrongType.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "wire整数0不能冒充空字符串");
    for (const auto offset : {28u, 32u}) {
        Fixture indexed; prepare(indexed); indexed.SetNative(original("Dt00", NativeValueType::String, std::string{}));
        Require(indexed.sources[kW3d].bytes.size() == 45, "v2单空String修改有两个索引");
        Put32(indexed.sources[kW3d].bytes, offset, 1);
        Rejected(indexed.Validate({"D001"}), {}, "任一非零wire索引都不能忽略后回退");
    }
    Fixture duplicate; prepare(duplicate);
    auto repeated = original("Dt00", NativeValueType::String, std::string{});
    Modification(repeated, "D001", "Nm00", NativeValueType::String, std::string{});
    duplicate.SetNative(repeated); Require(duplicate.sources[kW3d].bytes.size() == 66, "两个空字符串v2记录布局");
    PutId(duplicate.sources[kW3d].bytes, 41, "Dt00");
    Rejected(duplicate.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复精确地址不可取首条或末条");

    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"slk", "Doodads"}, {"slk", "UnknownDoodadData"}, {"slk", "Profile"},
        {"type", "int"}, {"repeat", "1"}, {"repeat", "bad"}, {"data", "1"},
        {"data", "0tail"}, {"index", "bad"}, {"field", "\"pathTex\""}, {"repeat", "\"0\""}}) {
        Fixture fixture; prepare(fixture); fixture.doodadMetadata[0][key] = value; fixture.Tables();
        fixture.SetNative(original("Dt00", NativeValueType::String, std::string{}));
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "metadata不可因默认或二次解码获得地址");
    }
    for (const unsigned mode : {0u, 1u, 2u, 3u, 4u, 5u}) {
        Fixture fixture; prepare(fixture);
        if (mode == 0) fixture.doodadMetadata[0].erase("repeat");
        else {
            auto alias = fixture.doodadMetadata[0]; alias["ID"] = "Als0";
            if (mode == 2) alias["repeat"] = "1";
            if (mode == 3) { alias["field"] = "pathTe"; alias["data"] = "24"; }
            if (mode == 4) alias["slk"] = "";
            if (mode == 5) alias["notspecific"] = "D001";
            fixture.doodadMetadata.push_back(std::move(alias));
        }
        fixture.Tables(); fixture.SetNative(original("Dt00", NativeValueType::String, std::string{}));
        Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "缺repeat和别名均拒绝，含变体/data拼接/被loader或适用规则过滤的行");
    }
    Fixture unknown; prepare(unknown); unknown.SetNative(original("No00", NativeValueType::String, std::string{}));
    Rejected(unknown.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "未知修改不能视为不影响占地");
    Fixture otherBadIndex; prepare(otherBadIndex); otherBadIndex.doodadMetadata[1]["index"] = "broken"; otherBadIndex.Tables();
    otherBadIndex.SetNative(original("Nm00", NativeValueType::String, std::string("display")));
    Rejected(otherBadIndex.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "拟跳过的展示字段也不能吞掉坏索引文本");
    Fixture reordered; prepare(reordered); reordered.SetNative(original("Dt00", NativeValueType::String, std::string{}));
    std::reverse(reordered.sources.begin(), reordered.sources.end());
    Accepted(reordered.Validate({"D001"}), "六项来源仍按实际名字绑定而不依赖顺序");
    Fixture replaced; prepare(replaced); replaced.sources[kMetadata] = replaced.sources.back(); replaced.sources.pop_back();
    Rejected(replaced.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_SOURCE", "合法第六来源不能顶替五项基础来源");
    Fixture missing; prepare(missing); missing.SetNative(original("Dt00", NativeValueType::String, std::string{}));
    missing.sources.pop_back();
    Rejected(missing.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED", "只在未捕获第六项时请求一次补来源");
    missing.sources.push_back({"Doodads/DoodadMetaData.slk", false, {}});
    Rejected(missing.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_TABLE_MISSING", "已捕获且缺失的第六项是终态失败");
    missing.sources.back().present = true;
    Rejected(missing.Validate({"D001"}), "TERRAIN_NATIVE_OBJECT_TABLE_MISSING", "已捕获空表不能继续请求来源");
    missing.sources.back().bytes = TextBytes("broken");
    Rejected(missing.Validate({"D001"}), {}, "损坏第六项不可回退");
    missing.sources.back().bytes.resize(2u * 1024u * 1024u + 1);
    Rejected(missing.Validate({"D001"}, 256u * 1024u * 1024u), "TERRAIN_NATIVE_OBJECT_BUDGET", "新表保持单表上限");
}

void CompleteSourcesAndWorkingBudget() {
    Fixture baseline;
    const auto work = Accepted(baseline.Validate(), "预算基线为真实合法输入");
    Accepted(baseline.Validate({"S002"}, work), "实际返回工作额度可作为同输入的精确上限");
    Rejected(baseline.Validate({"S002"}, work - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "少一字节的累计预算不能返回部分成功");
    Rejected(baseline.Validate({"S002"}, 0), "TERRAIN_NATIVE_OBJECT_BUDGET", "零额度不能开始解析");
    for (const auto member : {kDestructables, kDoodads, kMetadata}) {
        Fixture absent;
        absent.sources[member].present = false;
        absent.sources[member].bytes.clear();
        Rejected(absent.Validate(), "TERRAIN_NATIVE_OBJECT_TABLE_MISSING",
            "必需来源缺失不可使用默认表");
    }
    for (const auto member : {kW3b, kW3d}) {
        Fixture invalidAbsent;
        invalidAbsent.sources[member].present = false;
        invalidAbsent.sources[member].bytes = Encode(Empty(member == kW3b ? ObjectType::Destructable : ObjectType::Doodad));
        Rejected(invalidAbsent.Validate(), "TERRAIN_NATIVE_OBJECT_SOURCE", "缺失W3B/W3D不得同时携带字节");
    }
    Fixture duplicateSource;
    duplicateSource.sources[kW3d].name = "WAR3MAP.W3B";
    Rejected(duplicateSource.Validate(), "TERRAIN_NATIVE_OBJECT_SOURCE", "来源名字大小写别名不能重复");
    Fixture unknownSource;
    unknownSource.sources[kW3d].name = "unowned.w3d";
    Rejected(unknownSource.Validate(), "TERRAIN_NATIVE_OBJECT_SOURCE", "第五项不能被任意来源替换");
    Fixture incomplete;
    incomplete.sources.pop_back();
    Rejected(incomplete.Validate(), "TERRAIN_NATIVE_OBJECT_SOURCE", "完整来源集合不能漏项");
    Fixture extra;
    extra.sources.push_back({"other.bin", true, {}});
    Rejected(extra.Validate(), "TERRAIN_NATIVE_OBJECT_SOURCE", "来源集合不能追加未捕获输入");
    Fixture reordered;
    std::reverse(reordered.sources.begin(), reordered.sources.end());
    Accepted(reordered.Validate(), "来源按逻辑名字绑定，不依赖调用者排列");
}

void NonemptyDoodadInspectionOwnsUniqueBoundedResults() {
    Fixture fixture;
    const std::string path = "PathTextures\\SyntheticUniform.tga";
    fixture.doodads[0]["pathTex"] = path; fixture.doodads[0]["category"] = "E";
    fixture.doodads.push_back({{"doodID", "D002"}, {"pathTex", path}, {"category", "c"}});
    fixture.Tables();
    auto result = Take(fixture.Inspect({"D002", "S002", "D001", "D002"}));
    Require(result.footprints.size() == 2 && result.footprints[0].rawcode == "D001"
        && result.footprints[1].rawcode == "D002", "相同rawcode只列一次，不同rawcode共用路径仍各自列出并稳定排序");
    for (const auto& footprint : result.footprints)
        Require(footprint.pathTexture == path && footprint.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
            "实际路径原样保留，非C与小写c均为2格");
    Require(Take(fixture.Inspect({"D002", "S002", "D001", "D002"}, result.workBytes)).workBytes == result.workBytes,
        "包含临时索引、拥有字符串和最终结构的精确预算可以重用");
    Rejected(fixture.Inspect({"D002", "S002", "D001", "D002"}, result.workBytes - 1),
        "TERRAIN_NATIVE_OBJECT_BUDGET", "少一字节不能返回部分占地列表");
    Rejected(fixture.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "旧接口仍拒绝非空占地");
    std::reverse(fixture.sources.begin(), fixture.sources.end());
    Require(Take(fixture.Inspect({"D001"})).footprints.front().pathTexture == path, "结构化解析仍按来源名字绑定");
    fixture.sources.clear(); fixture.doodads.clear();
    Require(result.footprints[0].pathTexture == path && result.footprints[1].rawcode == "D002",
        "返回字符串拥有内容，不借用catalog或调用方来源的生命周期");

    for (const std::string value : {"", "_", "none"}) {
        Fixture disabled; disabled.doodads[0]["pathTex"] = value; disabled.Tables();
        const auto inspection = Take(disabled.Inspect());
        Require(inspection.footprints.empty() && inspection.workBytes == Accepted(disabled.Validate({"D001"}), "旧无占地域"),
            "旧无占地域不要求类别、不分配占地结果且两入口工作额度一致");
    }
    for (const std::string value : {std::string("NONE"), std::string(" none "), std::string("\"none\""), std::string(4096, 'p')}) {
        Fixture literal; literal.doodads[0]["pathTex"] = value; literal.doodads[0]["category"] = "E"; literal.Tables();
        const auto inspection = Take(literal.Inspect());
        Require(inspection.footprints.size() == 1 && inspection.footprints[0].pathTexture == value,
            "非禁用路径不经trim/二次解码，真实资源是否可用留给owner");
        Take(literal.Inspect({"D001"}, inspection.workBytes));
        Rejected(literal.Inspect({"D001"}, inspection.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "长路径也完整计入返回预算");
    }
    Fixture many; many.doodads.clear();
    std::vector<std::string> ids;
    for (unsigned i = 0; i < 32; ++i) {
        const auto id = "D0" + std::to_string(i / 10) + std::to_string(i % 10);
        ids.push_back(id); many.doodads.push_back({{"doodID", id}, {"pathTex", path}, {"category", "E"}});
    }
    many.Tables();
    std::vector<std::string_view> placed;
    for (const auto& id : ids) { placed.push_back(id); placed.push_back(id); }
    const auto inspection = Take(InspectNativeDestructablePathing(placed, many.sources, kWorkingBytes));
    Require(inspection.footprints.size() == ids.size(), "多对象只预留并返回唯一rawcode列表");
    Take(InspectNativeDestructablePathing(placed, many.sources, inspection.workBytes));
    Rejected(InspectNativeDestructablePathing(placed, many.sources, inspection.workBytes - 1),
        "TERRAIN_NATIVE_OBJECT_BUDGET", "多对象累计结果预算少一字节仍拒绝整批");
}

void NonemptyDoodadInspectionKeepsTheNarrowSourceDomain() {
    const auto prepare = [](Fixture& fixture) {
        fixture.doodads[0]["pathTex"] = "PathTextures\\synthetic.tga";
        fixture.doodads[0]["category"] = "E"; fixture.Tables();
    };
    Fixture missing; prepare(missing); missing.doodads[0].erase("category"); missing.Tables();
    Require(Take(missing.Inspect()).footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
        "category列缺失按D001第二字节0选择非C两格");
    missing.doodads.push_back({{"doodID", "D002"}, {"category", "E"}, {"pathTex", "_"}}); missing.Tables();
    Require(Take(missing.Inspect()).footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
        "有category列但目标行缺格同样按DOO新ID第二字节回退");
    for (const std::string category : {" C", " ", "\t", "\x7f", "\xc3\xa9", "WESTRING_CATEGORY", "westring_CATEGORY"}) {
        Fixture invalid; prepare(invalid); invalid.doodads[0]["category"] = category; invalid.Tables();
        const auto result = invalid.Inspect();
        Rejected(result, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "类别有效首字节须在支持域，标准本地化文本不能猜解析结果");
    }
    Fixture categoryC; prepare(categoryC); categoryC.doodads[0]["category"] = "C"; categoryC.Tables();
    const auto startup = Take(categoryC.Inspect());
    Require(startup.footprints.size() == 1
        && startup.footprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC,
        "字面C只请求Core捕获真实启动类别目录后选择网格");
    Rejected(categoryC.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "旧无占地兼容入口仍拒绝C非空占地");
    for (const unsigned mode : {0u, 1u, 2u, 3u}) {
        Fixture original; prepare(original);
        auto document = Empty(ObjectType::Doodad);
        if (mode == 3) Check(document.AddCustomObject("D001", "D002"));
        else Check(document.AddOriginalObject("D001"));
        if (mode == 1) Modification(document, "D001", "Nm00", NativeValueType::String, std::string("display"));
        if (mode == 2) Modification(document, "D001", "Dt00", NativeValueType::String, std::string("PathTextures\\synthetic.tga"));
        original.SetNative(document);
        if (mode != 3) Rejected(original.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED", "相关Original仍需要实际第六来源");
        original.AddDoodadMetadata();
        if (mode == 3) Rejected(original.Inspect(), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Custom不能覆盖已有标准ID");
        else Require(Take(original.Inspect()).footprints.size() == 1,
            "完整元数据证明Original只保留或覆盖pathTex且不改姿态后，可返回非空实际路径");
    }
    for (const std::string value : {"", "_", "none"}) {
        Fixture cleared; prepare(cleared); cleared.doodads[0].erase("category"); cleared.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Dt00", NativeValueType::String, value);
        cleared.SetNative(document); cleared.AddDoodadMetadata();
        Require(Take(cleared.Inspect()).footprints.empty(), "Original明确清空标准非空占地继续走旧域，不新增category要求");
    }
    Fixture unrelated; prepare(unrelated);
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D002")); unrelated.SetNative(document);
    Require(Take(unrelated.Inspect()).footprints.size() == 1 && unrelated.sources.size() == 5,
        "无关W3D不引入第六来源，也不拒绝标准非空对象");
    for (const auto* later : {"NONE", "S001"}) {
        Fixture mixed; prepare(mixed);
        Rejected(mixed.Inspect({"D001", later}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "前面的占地描述不能掩盖后续未知或walkable对象");
        Rejected(mixed.Inspect({later, "D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "整批拒绝不依赖对象顺序");
    }
    Fixture ambiguous; prepare(ambiguous); ambiguous.destructables.push_back({{"destructableID", "D001"}, {"walkable", "0"}}); ambiguous.Tables();
    Rejected(ambiguous.Inspect(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "新增占地结果不能绕开跨类别唯一性");
    Fixture w3b; prepare(w3b); w3b.SetNative(Original("D001"));
    Rejected(w3b.Inspect(), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "同rawcode的W3B仍拒绝类别歧义");
    Fixture broken; prepare(broken); broken.sources[kW3d] = {"war3map.w3d", true, TextBytes("broken")};
    Rejected(broken.Inspect(), {}, "未知或损坏W3D不能因为标准表非空而跳过");
}

void PatternedFootprintReceivesTheActualFixedRotation() {
    for (const auto* value : {"-1", "0", "90", "270", "360"}) {
        Fixture f; f.doodads[0]["pathTex"]="PathTextures\\pattern.tga";
        f.doodads[0]["category"]="E"; f.doodads[0]["fixedRot"]=value; f.Tables();
        const auto result=Take(f.Inspect());
        Require(result.footprints.size()==1 && result.footprints[0].fixedRotationDegrees==std::stof(value),
            "fixedRot comes from the actual typed standard row");
    }
    for (const auto* value : {"", "_", "90tail", " 90", "nan", "inf", "\"90\""}) {
        Fixture f; f.doodads[0]["pathTex"]="PathTextures\\pattern.tga";
        f.doodads[0]["category"]="E"; f.doodads[0]["fixedRot"]=value; f.Tables();
        const auto result=Take(f.Inspect());
        Require(!result.footprints[0].fixedRotationDegrees,
            "missing or ambiguous rotation is not defaulted; pixel owner decides whether angle matters");
    }
}

void DoodadOverridesUseBoundedStandardInheritance() {
    for (const bool custom : {false, true}) for (const char* path : {"PathTextures\\changed.tga", ""}) {
        Fixture f;
        f.doodads[0]["pathTex"] = "PathTextures\\base.tga";
        f.doodads[0]["category"] = "E"; f.doodads[0]["fixedRot"] = "90"; f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad);
        const std::string id = custom ? "C001" : "D001";
        if (custom) Check(document.AddCustomObject(id, "D001")); else Check(document.AddOriginalObject(id));
        Modification(document, id, "Dt00", NativeValueType::String, std::string(path));
        f.SetNative(document); const auto before = f.sources[kW3d].bytes;
        const auto parsed = Take(f.Inspect({id, id}));
        if (*path) {
            Require(parsed.footprints.size() == 1 && parsed.footprints[0].rawcode == id
                && parsed.footprints[0].pathTexture == path && parsed.footprints[0].fixedRotationDegrees == 90.0f,
                "native path uses the placed ID and standard parent's actual rotation");
        } else Require(parsed.footprints.empty(), "explicit custom empty path does not fall back to nonempty parent");
        Take(f.Inspect({id, id}, parsed.workBytes));
        Rejected(f.Inspect({id, id}, parsed.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "inherited output remains budgeted");
        Require(f.sources[kW3d].bytes == before, "inspection does not rewrite native custom records");
        if (custom) {
            Check(document.AddOriginalObject("D001"));
            Modification(document, "D001", "Nm00", NativeValueType::String, std::string("parent name"));
            f.SetNative(document);
            Rejected(f.Inspect({id}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "custom parent with any actual field override remains unsupported");
        }
    }
    for (const auto* key : {"category", "fixedRot"}) {
        Fixture f; f.doodads[0]["pathTex"] = "PathTextures\\base.tga"; f.doodads[0]["category"] = "E";
        f.doodadMetadata.push_back({{"ID", "Pos0"}, {"field", key}, {"slk", "DoodadData"}, {"type", "string"}, {"repeat", "0"}});
        f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Pos0", NativeValueType::String, std::string("0")); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "category缺显式index或nonnumeric fixedRot不能借标准姿态");
        Modification(document, "D001", "Dt00", NativeValueType::String, std::string{}); f.SetNative(document);
        Require(Take(f.Inspect()).footprints.empty(), "explicit zero footprint has no rotation contribution");
    }
    Fixture chain; chain.doodads[0]["pathTex"] = "_"; chain.Tables(); chain.AddDoodadMetadata();
    auto document = Empty(ObjectType::Doodad);
    Check(document.AddCustomObject("C001", "D001")); Check(document.AddCustomObject("C002", "C001")); chain.SetNative(document);
    Rejected(chain.Inspect({"C002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "recursive custom inheritance remains explicit and refused");
}

void DoodadCategoriesUseEffectiveFirstByteAndPlacedId() {
    using Policy = MapData::TerrainDoodadGridPolicy;
    for (const auto& [value, expected] : std::vector<std::pair<std::string, Policy>>{
        {"", Policy::TwoCells}, {"_", Policy::TwoCells}, {"_Ctail", Policy::TwoCells},
        {"CC", Policy::StartupCategoryC}, {"C ", Policy::StartupCategoryC},
        {"custom", Policy::TwoCells}, {"E\t\xc3\xa9", Policy::TwoCells}, {"\"C\"", Policy::TwoCells}}) {
        Fixture f; f.doodads[0]["pathTex"] = "PathTextures\\category.tga";
        f.doodads[0]["category"] = value; f.Tables();
        Require(Take(f.Inspect()).footprints[0].gridPolicy == expected,
            "标准category只读原始首字节，不trim/lower或解释后缀");
    }
    for (const bool missing : {false, true}) {
        Fixture f; f.doodads[0]["doodID"] = "DC01"; f.doodads[0]["code"] = "DC01";
        f.doodads[0]["pathTex"] = "PathTextures\\category.tga";
        if (missing) f.doodads[0].erase("category"); else f.doodads[0]["category"] = "_E";
        f.Tables();
        Require(Take(f.Inspect({"DC01"})).footprints[0].gridPolicy == Policy::StartupCategoryC,
            "缺格和任意前导下划线均按DOO第二字节C回退");
    }
    struct Case { const char* parent; const char* placed; const char* standard; const char* overrideValue; Policy policy; };
    for (const auto& sample : std::vector<Case>{
        {"D001", "D001", "E", "Ctail", Policy::StartupCategoryC},
        {"D001", "D001", "C", "E", Policy::TwoCells},
        {"DC01", "DC01", "E", "", Policy::StartupCategoryC},
        {"DE01", "DE01", "C", "_C", Policy::TwoCells},
        {"D001", "XC01", "E", "C", Policy::StartupCategoryC},
        {"D001", "XE01", "C", "E", Policy::TwoCells},
        {"D001", "XE01", "C", nullptr, Policy::StartupCategoryC},
        {"D001", "XC01", "E", nullptr, Policy::TwoCells},
        {"DE01", "XC01", "E", "", Policy::StartupCategoryC},
        {"DC01", "XE01", "C", "_Csuffix", Policy::TwoCells},
        {"D001", "XE01", "WESTRING_CATEGORY", "C", Policy::StartupCategoryC},
        {"D001", "XC01", "E", "WESTRING_CATEGORY", Policy::TwoCells}}) {
        Fixture f; f.doodads[0]["doodID"] = sample.parent; f.doodads[0]["code"] = sample.parent;
        f.doodads[0]["pathTex"] = "PathTextures\\category.tga"; f.doodads[0]["category"] = sample.standard;
        // 字段ID故意不是dcat；实际地址和doodadCategory类型来自同次metadata。
        f.doodadMetadata.push_back({{"ID", "Cat9"}, {"field", "category"}, {"slk", "DoodadData"},
            {"type", "doodadCategory"}, {"repeat", "0"}, {"data", "0"}, {"index", "0"}});
        f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad);
        if (std::string_view(sample.parent) == sample.placed) Check(document.AddOriginalObject(sample.placed));
        else Check(document.AddCustomObject(sample.placed, sample.parent));
        if (sample.overrideValue) Modification(document, sample.placed, "Cat9", NativeValueType::String, std::string(sample.overrideValue));
        f.SetNative(document); const auto before = f.sources[kW3d].bytes;
        const auto parsed = Take(f.Inspect({sample.placed, sample.placed}));
        Require(parsed.footprints.size() == 1 && parsed.footprints[0].rawcode == sample.placed
            && parsed.footprints[0].gridPolicy == sample.policy,
            "Original/一层Custom使用实际覆盖，缺覆盖继承，空/_按新ID而非父ID回退");
        Take(f.Inspect({sample.placed, sample.placed}, parsed.workBytes));
        Rejected(f.Inspect({sample.placed, sample.placed}, parsed.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "category描述符仍完整计入预算");
        Require(f.sources[kW3d].bytes == before, "类别解析保持原生wire字节");
    }
}

void DoodadCategoryOverridesRequireUniqueScalarStringMetadata() {
    const auto prepare = [](Fixture& f) {
        f.doodads[0]["pathTex"] = "PathTextures\\category.tga"; f.doodads[0]["category"] = "E";
        f.doodadMetadata.push_back({{"ID", "Cat9"}, {"field", "category"}, {"slk", "DoodadData"},
            {"type", "doodadCategory"}, {"repeat", "0"}, {"data", "0"}, {"index", "0"}});
    };
    for (unsigned mode = 0; mode < 16; ++mode) {
        Fixture f; prepare(f); auto& field = f.doodadMetadata.back();
        if (mode == 0) field.erase("index");
        if (mode == 1) field["index"] = "1";
        if (mode == 2) field["index"] = "0tail";
        if (mode == 3) field.erase("repeat");
        if (mode == 4) field["repeat"] = "1";
        if (mode == 5) field["data"] = "1";
        if (mode == 6) field["appendindex"] = "1";
        if (mode == 7) field["effectindex"] = "1";
        if (mode == 8) field["useSpecific"] = "D001";
        if (mode == 9) field["notSpecific"] = "D002";
        if (mode == 10 || mode == 11) {
            auto duplicate = field; duplicate["ID"] = "Cat8";
            if (mode == 11) duplicate["slk"] = "";
            f.doodadMetadata.push_back(std::move(duplicate));
        }
        if (mode == 12) f.doodadMetadata.push_back({{"ID", "Cat8"}, {"field", "categor"}, {"slk", "DoodadData"},
            {"type", "string"}, {"repeat", "0"}, {"data", "25"}, {"index", "0"}});
        if (mode == 13) field["slk"] = "";
        if (mode == 14) field["type"] = "int";
        if (mode == 15) field["slk"] = "Doodads";
        f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Cat9", mode == 14 ? NativeValueType::Int32 : NativeValueType::String,
            mode == 14 ? Value{std::int32_t{67}} : Value{std::string("C")}); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "category需要唯一实际String零索引字段，不借默认值、选择器或别名授权");
    }
    for (const bool custom : {false, true}) {
        Fixture f; prepare(f); f.doodadMetadata.back()["useSpecific"] = "D001"; f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad); const std::string id = custom ? "XC01" : "D001";
        if (custom) Check(document.AddCustomObject(id, "D001")); else Check(document.AddOriginalObject(id));
        Modification(document, id, "Cat9", NativeValueType::String, std::string("C")); f.SetNative(document);
        Rejected(f.Inspect({id}), "TERRAIN_NATIVE_OBJECT_METADATA", "Original与Custom均拒绝未证category选择器语义");
    }
    Fixture wire; prepare(wire); wire.Tables(); wire.AddDoodadMetadata();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    Modification(document, "D001", "Cat9", NativeValueType::Int32, std::int32_t{67}); wire.SetNative(document);
    Rejected(wire.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "String元数据的category不能由Int32 wire冒充");
    Fixture unused; prepare(unused); unused.doodadMetadata.back().erase("index"); unused.Tables(); unused.AddDoodadMetadata();
    document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    Modification(document, "D001", "Cat9", NativeValueType::String, std::string("C"));
    Modification(document, "D001", "Dt00", NativeValueType::String, std::string{}); unused.SetNative(document);
    Require(Take(unused.Inspect()).footprints.empty(), "明确禁用pathTex时不新增未贡献category的标量资格要求");
}

void NativeDoodadRotationUsesExactValuesAndSources() {
    const auto prepare = [](Fixture& fixture, NativeValueType type = NativeValueType::Unreal) {
        fixture.doodads[0]["pathTex"] = "PathTextures\\pattern.tga";
        fixture.doodads[0]["category"] = "E"; fixture.doodads[0]["fixedRot"] = "180";
        fixture.doodadMetadata.push_back({{"ID", "Rot0"}, {"field", "fixedRot"}, {"slk", "DoodadData"},
            {"type", type == NativeValueType::Real ? "real" : "unreal"}, {"repeat", "0"}, {"index", "0"}});
        fixture.Tables(); fixture.AddDoodadMetadata();
    };
    for (const bool custom : {false, true}) for (const auto type : {NativeValueType::Real, NativeValueType::Unreal}) {
        for (const float value : {-360.0f, -1.0f, -0.0f, 0.0f, 90.25f, 270.0f, 360.0f}) {
            Fixture f; prepare(f, type);
            auto document = Empty(ObjectType::Doodad);
            const std::string id = custom ? "C001" : "D001";
            if (custom) Check(document.AddCustomObject(id, "D001")); else Check(document.AddOriginalObject(id));
            Modification(document, id, "Rot0", type, value);
            Modification(document, id, "Dt00", NativeValueType::String, std::string("PathTextures\\actual.tga"));
            f.SetNative(document);
            const auto before = f.sources[kW3d].bytes;
            const auto parsed = Take(f.Inspect({id, id}));
            Require(parsed.footprints.size() == 1 && parsed.footprints[0].rawcode == id
                && parsed.footprints[0].pathTexture == "PathTextures\\actual.tga"
                && parsed.footprints[0].fixedRotationDegrees
                && std::bit_cast<std::uint32_t>(*parsed.footprints[0].fixedRotationDegrees) == std::bit_cast<std::uint32_t>(value),
                "Original and direct Custom use the exact metadata-bound wire F32, including signed zero and negative values");
            Take(f.Inspect({id, id}, parsed.workBytes));
            Rejected(f.Inspect({id, id}, parsed.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "effective rotation keeps the exact work budget");
            Require(f.sources[kW3d].bytes == before, "effective pose inspection does not rewrite native records");
            if (custom) {
                Check(document.AddOriginalObject("D001")); f.SetNative(document);
                const auto emptyParent = Take(f.Inspect({id}));
                Require(emptyParent.footprints.size() == 1 && emptyParent.footprints[0].rawcode == id
                    && emptyParent.footprints[0].pathTexture == parsed.footprints[0].pathTexture
                    && emptyParent.footprints[0].fixedRotationDegrees
                    && std::bit_cast<std::uint32_t>(*emptyParent.footprints[0].fixedRotationDegrees) == std::bit_cast<std::uint32_t>(value),
                    "a zero-field Original parent preserves the exact Custom footprint and rotation");
                Modification(document, "D001", "Rot0", type, 180.0f); f.SetNative(document);
                Rejected(f.Inspect({id}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "numeric getter does not authorize a native-overridden parent");
            }
        }
    }
    for (const float value : {-361.0f, 361.0f}) {
        Fixture f; prepare(f);
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Rot0", NativeValueType::Unreal, value); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_DOODAD_FIXED_ROTATION", "native rotation cannot enlarge the existing supported degree range");
        Modification(document, "D001", "Dt00", NativeValueType::String, std::string{}); f.SetNative(document);
        Require(Take(f.Inspect()).footprints.empty(), "an explicitly disabled footprint retains its prior zero-contribution domain");
    }
    for (const auto type : {NativeValueType::Int32, NativeValueType::Real, NativeValueType::String}) {
        Fixture f; prepare(f);
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        const Value value = type == NativeValueType::Int32 ? Value{std::int32_t{90}}
            : type == NativeValueType::String ? Value{std::string("90")} : Value{90.0f};
        Modification(document, "D001", "Rot0", type, value); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "matching numeric text or F32 width cannot replace the actual Unreal wire type");
    }
    for (const std::uint32_t bits : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
        Fixture f; prepare(f);
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Rot0", NativeValueType::Unreal, 90.0f); f.SetNative(document);
        Put32(f.sources[kW3d].bytes, 36, bits);
        Rejected(f.Inspect(), {}, "nonfinite native bits must fail before returning a partial effective pose");
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fixture f; prepare(f);
        if (mode == 0) f.doodadMetadata.back()["repeat"] = "1";
        if (mode == 1) f.doodadMetadata.back()["index"] = "1";
        if (mode == 2) { auto duplicate = f.doodadMetadata.back(); duplicate["ID"] = "Rot1"; f.doodadMetadata.push_back(duplicate); }
        if (mode == 3) { auto hidden = f.doodadMetadata.back(); hidden["ID"] = "Rot1"; hidden["slk"] = ""; f.doodadMetadata.push_back(hidden); }
        if (mode == 4) f.doodadMetadata.push_back({{"ID", "Rot1"}, {"field", "fixedro"}, {"slk", "DoodadData"},
            {"type", "unreal"}, {"repeat", "0"}, {"data", "20"}, {"index", "0"}});
        if (mode == 5) f.doodadMetadata.back().erase("index");
        f.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Rot0", NativeValueType::Unreal, 90.0f); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "fixedRot needs a unique actual nonrepeating zero-index metadata address, including hidden aliases");
    }
    Fixture category; prepare(category); category.doodads[0]["category"] = "C"; category.Tables();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    Modification(document, "D001", "Rot0", NativeValueType::Unreal, 90.0f); category.SetNative(document);
    Require(Take(category.Inspect()).footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC,
        "a numeric override does not promote C to a verified four-cell grid");
    category.doodadMetadata.push_back({{"ID", "Cat0"}, {"field", "category"}, {"slk", "DoodadData"}, {"type", "string"}, {"repeat", "0"}});
    category.Tables(); Modification(document, "D001", "Cat0", NativeValueType::String, std::string("E")); category.SetNative(document);
    Rejected(category.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "有效fixedRot不替代category自己的显式零index元数据");
}

void WalkableBridgeInspectionOwnsUniqueBoundedResults() {
    auto fixture = WalkableBridgeFixture();
    const std::string path = "PathTextures\\SyntheticBridge.tga";
    fixture.destructables.push_back({{"destructableID", "B001"}, {"walkable", "1"}, {"pathTex", path}, {"fixedRot", "90.25"}});
    fixture.doodads[0]["pathTex"] = path;
    fixture.doodads[0]["category"] = "E";
    fixture.doodads[0]["fixedRot"] = "0";
    fixture.Tables();
    const auto w3bBefore = fixture.sources[kW3b].bytes;
    const auto tableBefore = fixture.sources[kDestructables].bytes;
    const auto ids = {std::string_view{"S001"}, std::string_view{"D001"}, std::string_view{"B001"},
        std::string_view{"S002"}, std::string_view{"S001"}};
    auto result = Take(fixture.Inspect(ids));
    Require(result.footprints.size() == 3 && result.specialFootprints.empty()
        && result.footprints[0].rawcode == "B001" && result.footprints[1].rawcode == "D001"
        && result.footprints[2].rawcode == "S001", "桥按唯一rawcode合并并排序，普通RGB与零贡献对象仍各守原类别");
    for (const auto index : {0u, 2u}) {
        const auto& bridge = result.footprints[index];
        Require(bridge.pathTexture == path && bridge.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells
            && bridge.contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
            "标准walkable1描述符拥有实际路径、2格策略和terrain抑制贡献，不输出普通RGB属性");
    }
    Require(result.footprints[0].fixedRotationDegrees == 90.25f && result.footprints[2].fixedRotationDegrees == -1.0f
        && result.footprints[1].contribution == MapData::TerrainNativeFootprintContribution::StaticRgbAttributes,
        "桥实际fixedRot随类型返回，混合普通doodad仍是RGB贡献");
    Require(Take(fixture.Inspect(ids, result.workBytes)).workBytes == result.workBytes,
        "桥描述符和拥有字符串计入可重用的精确工作预算");
    Rejected(fixture.Inspect(ids, result.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "少一字节不能返回部分桥或普通占地");
    Rejected(fixture.Inspect(ids, 0), "TERRAIN_NATIVE_OBJECT_BUDGET", "桥解析零预算仍拒绝");
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "旧zero-only入口仍拒绝桥的非空贡献");
    Require(fixture.sources[kW3b].bytes == w3bBefore && fixture.sources[kDestructables].bytes == tableBefore,
        "桥描述符解析不回写原生对象或标准表");
    std::reverse(fixture.sources.begin(), fixture.sources.end());
    Require(Take(fixture.Inspect({"S001", "S001"})).footprints.size() == 1,
        "来源顺序不改变桥身份，相同rawcode的重复摆放只返回一个类型描述符");
    fixture.sources.clear(); fixture.destructables.clear(); fixture.doodads.clear();
    Require(result.footprints[0].rawcode == "B001" && result.footprints[0].pathTexture == path
        && result.footprints[2].pathTexture == path && result.footprints[2].fixedRotationDegrees == -1.0f,
        "桥结果的rawcode和路径不借用来源或catalog生命周期");

    auto zero = WalkableBridgeFixture();
    zero.destructables[0]["walkable"] = "0"; zero.Tables();
    Require(Take(zero.Inspect({"S001"})).footprints.empty(), "显式标准walkable0保持旧零贡献域，不因存在pathTex变成桥");
    Accepted(zero.Validate({"S001"}), "旧zero-only入口仍接受walkable0");
}

void WalkableBridgeRequiresExplicitStandardFields() {
    for (const std::string field : {"pathTex", "fixedRot"}) {
        auto missing = WalkableBridgeFixture();
        missing.destructables[0].erase(field); missing.Tables();
        Rejected(missing.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "桥不能用缺失标准字段推导默认路径或角度");
        missing.destructables[0][field + "Unknown"] = field == "pathTex" ? "PathTextures\\SyntheticBridge.tga" : "90";
        missing.Tables();
        Rejected(missing.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "相似但未知的标准字段名不能代替pathTex或fixedRot");
    }
    for (const std::string value : {"", "_", "none"}) {
        auto fixture = WalkableBridgeFixture();
        fixture.destructables[0]["pathTex"] = value; fixture.Tables();
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "walkable1的空或明确禁用路径不能冒充已支持桥");
    }
    for (const std::string value : {"", "unknown", "nan", "NaN", "inf", "-inf", "1e39", "-361", "361", "90tail", " 90", "90 "}) {
        auto fixture = WalkableBridgeFixture();
        fixture.destructables[0]["fixedRot"] = value; fixture.Tables();
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "桥fixedRot须完整解析为有限且在支持范围内的标准值");
    }
    for (const auto& [text, expected] : std::vector<std::pair<std::string, float>>{
        {"-360", -360.0f}, {"-1", -1.0f}, {"-0", -0.0f}, {"0", 0.0f}, {"90.25", 90.25f}, {"360", 360.0f}}) {
        auto fixture = WalkableBridgeFixture();
        fixture.destructables[0]["fixedRot"] = text; fixture.Tables();
        const auto result = Take(fixture.Inspect({"S001"}));
        Require(result.footprints.size() == 1 && result.footprints[0].fixedRotationDegrees
            && std::bit_cast<std::uint32_t>(*result.footprints[0].fixedRotationDegrees) == std::bit_cast<std::uint32_t>(expected),
            "桥标准fixedRot保留边界、负值及有符号零，不回退为默认朝向");
    }
    for (const std::string value : {"", "true", "false", "-1", "2", "1tail"}) {
        auto fixture = WalkableBridgeFixture();
        fixture.destructables[0]["walkable"] = value; fixture.Tables();
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "非显式0/1的标准walkable不能借路径获得桥描述符");
    }
    auto missingWalkable = WalkableBridgeFixture();
    missingWalkable.destructables[0].erase("walkable"); missingWalkable.Tables();
    Rejected(missingWalkable.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE", "缺失标准walkable不能推断为可行走桥");
    auto mixed = WalkableBridgeFixture();
    Rejected(mixed.Inspect({"S001", "NONE"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "有效桥不能掩盖同批后续未知对象");
    Rejected(mixed.Inspect({"NONE", "S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "桥批量拒绝不依赖摆放顺序");
}

void WalkableBridgeRejectsRelevantNativeRecordsAndClassCollisions() {
    auto original = WalkableBridgeFixture();
    auto emptyOriginal = Empty(); Check(emptyOriginal.AddOriginalObject("S001")); original.SetNative(emptyOriginal);
    Require(Take(original.Inspect({"S001"})).footprints.size() == 1,
        "空Original没有实际覆盖时沿用标准桥，不强求未使用字段的metadata地址");
    auto custom = BridgeOriginalFixture();
    auto inherited = Empty(); Check(inherited.AddCustomObject("B001", "S001")); custom.SetNative(inherited);
    Require(Take(custom.Inspect({"B001"})).footprints.front().rawcode == "B001",
        "空Custom逐字段沿用无覆盖的标准父，并以摆放新ID绑定实例");
    auto unrelated = WalkableBridgeFixture();
    auto w3b = Empty();
    Check(w3b.AddOriginalObject("S002"));
    Check(w3b.AddCustomObject("B001", "S003"));
    unrelated.SetNative(w3b);
    auto w3d = Empty(ObjectType::Doodad);
    Check(w3d.AddOriginalObject("D002")); unrelated.SetNative(w3d);
    Require(Take(unrelated.Inspect({"S001", "S002"})).footprints.size() == 1 && unrelated.sources.size() == 5,
        "其它对象W3B/W3D记录不拒绝标准桥，也不请求无关DoodadMetaData");
    unrelated.sources[kW3b] = {"war3map.w3b", false, {}};
    Require(Take(unrelated.Inspect({"S001"})).footprints.size() == 1, "准确Missing W3B与合法无关W3B均允许标准桥");

    auto duplicate = WalkableBridgeFixture();
    duplicate.destructables.push_back(duplicate.destructables[0]); duplicate.Tables();
    Rejected(duplicate.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_ID", "桥标准行必须唯一，不能依赖loader覆盖重复定义");
    auto sameStandard = WalkableBridgeFixture();
    sameStandard.doodads.push_back({{"doodID", "S001"}, {"pathTex", "_"}}); sameStandard.Tables();
    Rejected(sameStandard.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "同名标准Doodad即使无占地也不能借桥身份");
    for (const bool custom : {false, true}) {
        auto fixture = WalkableBridgeFixture();
        auto document = Empty(ObjectType::Doodad);
        if (custom) Check(document.AddCustomObject("S001", "D001"));
        else Check(document.AddOriginalObject("S001"));
        fixture.SetNative(document);
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "同名W3D Original或Custom都先拒绝类别歧义");
    }
}

void WalkableBridgeOriginalUsesEffectiveFields() {
    for (const auto wire : {NativeValueType::Real, NativeValueType::Unreal}) {
        for (const float degrees : {-360.0f, -1.0f, 0.0f, 90.25f, 360.0f}) {
            auto fixture = BridgeOriginalFixture();
            fixture.metadata[0]["ID"] = "Wlk9"; fixture.metadata[1]["ID"] = "Pat9"; fixture.metadata[2]["ID"] = "Ang9";
            fixture.metadata[2]["type"] = wire == NativeValueType::Real ? "real" : "unreal";
            fixture.destructables[0]["walkable"] = "0";
            fixture.destructables[0]["pathTex"] = "_";
            fixture.destructables[0]["fixedRot"] = "invalid fallback";
            fixture.Tables();
            auto document = Original("S001", "Wlk9", NativeValueType::Int32, std::int32_t{1});
            const std::string path = "PathTextures\\overridden;\"literal\".tga";
            Modification(document, "S001", "Pat9", NativeValueType::String, path);
            Modification(document, "S001", "Ang9", wire, degrees);
            fixture.SetNative(document);
            const auto source = fixture.sources;
            const auto result = Take(fixture.Inspect({"S001", "S001"}));
            Require(result.footprints.size() == 1 && result.footprints.front().rawcode == "S001"
                && result.footprints.front().pathTexture == path && result.footprints.front().fixedRotationDegrees == degrees
                && result.footprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells
                && result.footprints.front().contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
                "Original用实际metadata三字段覆盖矛盾标准值；wire1/2、显式零和负角均不回退或归一化");
            for (std::size_t i = 0; i < source.size(); ++i)
                Require(source[i].bytes == fixture.sources[i].bytes, "桥Original解析不能重写任何来源");
            Take(fixture.Inspect({"S001", "S001"}, result.workBytes));
            Rejected(fixture.Inspect({"S001", "S001"}, result.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
                "Original字段及输出仍使用同一累计解析预算");
        }
    }
    for (const unsigned kind : {0u, 1u, 2u}) {
        auto fixture = BridgeOriginalFixture();
        auto document = Empty(); Check(document.AddOriginalObject("S001"));
        if (kind == 0) Modification(document, "S001", "X001", NativeValueType::Int32, std::int32_t{1});
        if (kind == 1) Modification(document, "S001", "Pth0", NativeValueType::String, std::string("PathTextures\\only-path.tga"));
        if (kind == 2) Modification(document, "S001", "Rot0", NativeValueType::Unreal, 0.0f);
        fixture.SetNative(document);
        const auto result = Take(fixture.Inspect({"S001"}));
        Require(result.footprints.front().pathTexture == (kind == 1 ? "PathTextures\\only-path.tga" : "PathTextures\\SyntheticBridge.tga")
            && result.footprints.front().fixedRotationDegrees == (kind == 2 ? 0.0f : -1.0f),
            "未修改字段才沿用标准桥值，各单字段Original均可独立解释");
    }
    auto nonwalkable = BridgeOriginalFixture();
    nonwalkable.SetNative(Original("S001", "X001", NativeValueType::Int32, std::int32_t{0}));
    Require(Take(nonwalkable.Inspect({"S001"})).footprints.empty(), "walkable=0覆盖仍属于既有零贡献域");
    Accepted(nonwalkable.Validate({"S001"}), "旧兼容入口仍接受明确零贡献覆盖");
}

void WalkableBridgeOriginalRejectsUnknownOrInexactValues() {
    for (const std::string path : {"", "_", "none"}) {
        auto fixture = BridgeOriginalFixture();
        fixture.SetNative(Original("S001", "Pth0", NativeValueType::String, path));
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "明确禁用的Original pathTex不能回退标准非空桥");
    }
    for (const float degrees : {-360.25f, 360.25f}) {
        auto fixture = BridgeOriginalFixture();
        fixture.SetNative(Original("S001", "Rot0", NativeValueType::Unreal, degrees));
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_DOODAD_FIXED_ROTATION", "Original fixedRot须有限且不扩大现有角度域");
    }
    for (const std::uint32_t bits : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
        auto fixture = BridgeOriginalFixture();
        fixture.SetNative(Original("S001", "Rot0", NativeValueType::Unreal, 0.0f));
        auto& bytes = fixture.sources[kW3b].bytes;
        // serializer拒绝非有限值；直接改经典v2单Original/单modifier的实际F32载荷。
        Require(bytes.size() == 40 && bytes[20] == std::byte{'R'} && bytes[24] == std::byte{2}, "非有限桥角夹具地址须明确");
        Put32(bytes, 28, bits);
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_DOODAD_FIXED_ROTATION", "Original fixedRot非有限wire不得被接受");
    }
    for (const auto wire : {NativeValueType::Int32, NativeValueType::Real, NativeValueType::Unreal}) {
        auto fixture = BridgeOriginalFixture();
        const Value value = wire == NativeValueType::Int32 ? Value{std::int32_t{0}} : Value{0.0f};
        fixture.SetNative(Original("S001", "Pth0", wire, value));
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "pathTex数值wire不能冒充metadata String");
    }
    for (const auto wire : {NativeValueType::Int32, NativeValueType::Real, NativeValueType::String}) {
        auto fixture = BridgeOriginalFixture();
        const Value value = wire == NativeValueType::String ? Value{std::string("0")}
            : wire == NativeValueType::Int32 ? Value{std::int32_t{0}} : Value{0.0f};
        fixture.SetNative(Original("S001", "Rot0", wire, value));
        Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "fixedRot即使同为F32，wire1也不能冒充metadata Unreal wire2");
    }
    auto unsupported = BridgeOriginalFixture();
    unsupported.metadata.push_back({{"ID", "Suf0"}, {"field", "EditorSuffix"}, {"slk", "DestructableData"}, {"type", "string"}, {"index", "0"}});
    unsupported.Tables(); unsupported.SetNative(Original("S001", "Suf0", NativeValueType::String, std::string("bridge")));
    Rejected(unsupported.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "其它已知Original字段没有静态注册证据仍不放行");
    unsupported.SetNative(Original("S001", "ZZZ9", NativeValueType::String, std::string("bridge")));
    Rejected(unsupported.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "未知Original字段不能由标准walkable授予资格");
}

void WalkableBridgeOriginalRequiresUniqueScalarMetadata() {
    for (const std::size_t field : {std::size_t{1}, std::size_t{2}}) {
        for (const auto& [key, value] : std::array{std::pair{"slk", "OtherData"}, std::pair{"index", "1"},
            std::pair{"repeat", "1"}, std::pair{"data", "1"}, std::pair{"appendindex", "1"},
            std::pair{"effectindex", "1"}, std::pair{"index", "0tail"}}) {
            auto fixture = BridgeOriginalFixture();
            fixture.metadata[field][key] = value; fixture.Tables();
            fixture.SetNative(field == 1 ? Original("S001", "Pth0", NativeValueType::String, std::string("PathTextures\\new.tga"))
                : Original("S001", "Rot0", NativeValueType::Unreal, 90.0f));
            Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "Original不能使用未知表、非零或坏文本metadata标量地址");
        }
        auto missing = BridgeOriginalFixture();
        missing.metadata[field].erase("index"); missing.Tables();
        missing.SetNative(field == 1 ? Original("S001", "Pth0", NativeValueType::String, std::string("PathTextures\\new.tga"))
            : Original("S001", "Rot0", NativeValueType::Unreal, 90.0f));
        Rejected(missing.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "缺失index不猜为原版明确0");
        for (const std::string table : {"DestructableData", ""}) {
            auto duplicate = BridgeOriginalFixture();
            auto alias = duplicate.metadata[field]; alias["ID"] = "Dup9"; alias["slk"] = table;
            duplicate.metadata.push_back(alias); duplicate.Tables();
            duplicate.SetNative(field == 1 ? Original("S001", "Pth0", NativeValueType::String, std::string("PathTextures\\new.tga"))
                : Original("S001", "Rot0", NativeValueType::Unreal, 90.0f));
            Rejected(duplicate.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "schema丢弃的缺表候选也不能制造虚假唯一地址");
        }
    }
    auto alias = BridgeOriginalFixture();
    alias.metadata.push_back({{"ID", "Als9"}, {"field", "pathte"}, {"slk", "DestructableData"},
        {"type", "pathingTexture"}, {"index", "0"}, {"data", "24"}});
    alias.Tables(); alias.SetNative(Original("S001", "Pth0", NativeValueType::String, std::string("PathTextures\\new.tga")));
    Rejected(alias.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "data拼接别名不能与标准桥pathTex共存后被忽略");
}

NativeObjectDocument CustomBridge(const std::string& rawcode = "N001", const std::string& parent = "S001") {
    auto document = Empty();
    Check(document.AddCustomObject(rawcode, parent));
    return document;
}

void WalkableCustomBridgeUsesItsOwnWireIdAndEffectiveFields() {
    for (const std::string parent : {"S001", "SC01"}) {
        auto empty = WalkableBridgeFixture();
        empty.destructables[0]["destructableID"] = parent; empty.Tables();
        empty.SetNative(CustomBridge("N001", parent));
        const auto result = Take(empty.Inspect({"N001"}));
        Require(result.footprints.size() == 1 && result.footprints.front().rawcode == "N001"
            && result.footprints.front().pathTexture == "PathTextures\\SyntheticBridge.tga"
            && result.footprints.front().fixedRotationDegrees == -1.0f
            && result.footprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
            "空Custom不要求未用path/rotation元数据；父ID第二字节C不能替代新ID的非C网格");
        for (const auto wire : {NativeValueType::Real, NativeValueType::Unreal}) for (const float degrees : {-1.0f, 0.0f, 90.25f}) {
            auto fixture = BridgeOriginalFixture();
            fixture.destructables[0]["destructableID"] = parent;
            fixture.destructables[0]["walkable"] = "0";
            fixture.destructables[0]["pathTex"] = "_";
            fixture.destructables[0]["fixedRot"] = "bad fallback";
            fixture.metadata[2]["type"] = wire == NativeValueType::Real ? "real" : "unreal";
            fixture.Tables();
            auto document = CustomBridge("N001", parent);
            Modification(document, "N001", "X001", NativeValueType::Int32, std::int32_t{1});
            Modification(document, "N001", "Pth0", NativeValueType::String, std::string("PathTextures\\custom.tga"));
            Modification(document, "N001", "Rot0", wire, degrees);
            fixture.SetNative(document); const auto source = fixture.sources;
            const auto actual = Take(fixture.Inspect({"N001", "N001"}));
            Require(actual.footprints.size() == 1 && actual.footprints.front().rawcode == "N001"
                && actual.footprints.front().pathTexture == "PathTextures\\custom.tga"
                && actual.footprints.front().fixedRotationDegrees == degrees
                && actual.footprints.front().contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
                "Custom覆盖命中新ID，矛盾父值不能盖过实际三字段wire值");
            for (std::size_t i = 0; i < source.size(); ++i)
                Require(source[i].bytes == fixture.sources[i].bytes, "Custom桥分类保持全部来源字节");
            Take(fixture.Inspect({"N001", "N001"}, actual.workBytes));
            Rejected(fixture.Inspect({"N001", "N001"}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "Custom桥仍受精确累计解析预算约束");
        }
    }
    for (const unsigned field : {0u, 1u, 2u}) {
        auto fixture = BridgeOriginalFixture(); auto document = CustomBridge();
        if (field == 0) Modification(document, "N001", "X001", NativeValueType::Int32, std::int32_t{1});
        if (field == 1) Modification(document, "N001", "Pth0", NativeValueType::String, std::string("PathTextures\\single.tga"));
        if (field == 2) Modification(document, "N001", "Rot0", NativeValueType::Unreal, 0.0f);
        fixture.SetNative(document); const auto actual = Take(fixture.Inspect({"N001"}));
        Require(actual.footprints.front().pathTexture == (field == 1 ? "PathTextures\\single.tga" : "PathTextures\\SyntheticBridge.tga")
            && actual.footprints.front().fixedRotationDegrees == (field == 2 ? 0.0f : -1.0f), "Custom仅未覆盖字段回退标准父");
    }
    auto zero = BridgeOriginalFixture(); auto document = CustomBridge();
    Modification(document, "N001", "X001", NativeValueType::Int32, std::int32_t{0}); zero.SetNative(document);
    Require(Take(zero.Inspect({"N001"})).footprints.empty(), "Custom walkable0继续走旧零贡献域");
    Accepted(zero.Validate({"N001"}), "Custom walkable0不追溯收紧旧兼容入口");
}

void WalkableCustomBridgeKeepsInheritanceAndWireGridBoundaries() {
    auto customC = BridgeOriginalFixture(); customC.SetNative(CustomBridge("NC01"));
    Require(Take(customC.Inspect({"NC01"})).footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC,
        "父ID非C不能掩盖Custom新ID第二字节C，仍需可信owner捕获startup选择");
    for (const bool w3d : {false, true}) {
        auto fixture = BridgeOriginalFixture(); auto document = CustomBridge();
        if (w3d) {
            auto parent = Empty(ObjectType::Doodad); Check(parent.AddOriginalObject("S001")); fixture.SetNative(parent);
        } else {
            Check(document.AddOriginalObject("S001"));
            Modification(document, "S001", "X001", NativeValueType::Int32, std::int32_t{1});
        }
        fixture.SetNative(document);
        Rejected(fixture.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "父对象有实际W3B覆盖或任何跨类W3D记录时不能继承桥");
    }
    auto chain = BridgeOriginalFixture(); auto document = CustomBridge(); Check(document.AddCustomObject("N002", "N001")); chain.SetNative(document);
    Rejected(chain.Inspect({"N002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Custom桥不接入多层继承");
    auto missing = BridgeOriginalFixture(); missing.SetNative(CustomBridge("N001", "None"));
    Rejected(missing.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "缺失标准父不能借Custom记录建立类型");
    auto collision = BridgeOriginalFixture(); collision.doodads.push_back({{"doodID", "N001"}, {"pathTex", "_"}});
    collision.Tables(); collision.SetNative(CustomBridge());
    Rejected(collision.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS", "Custom新ID与Doodad同名仍拒绝");
    for (const std::string path : {"", "_", "none"}) {
        auto fixture = BridgeOriginalFixture(); auto custom = CustomBridge();
        Modification(custom, "N001", "Pth0", NativeValueType::String, path); fixture.SetNative(custom);
        Rejected(fixture.Inspect({"N001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "Custom明确禁用path不能回退父非空路径");
    }
    auto wrongWire = BridgeOriginalFixture(); auto custom = CustomBridge();
    Modification(custom, "N001", "Rot0", NativeValueType::Real, 90.0f); wrongWire.SetNative(custom);
    Rejected(wrongWire.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "Custom仍须按实际Unreal元数据读取精确wire2");
}

void WalkableCustomBridgeOnlyRejectsSelectorsOnUsedFields() {
    for (const unsigned field : {0u, 1u, 2u}) for (const auto& [key, value] : std::array{
        std::pair{"useSpecific", "S001"}, std::pair{"useSpecific", "N001"}, std::pair{"notSpecific", "N001"}}) {
        auto fixture = BridgeOriginalFixture(); fixture.metadata[field][key] = value; fixture.Tables();
        auto document = CustomBridge();
        if (field == 1) Modification(document, "N001", "Pth0", NativeValueType::String, std::string("PathTextures\\custom.tga"));
        if (field == 2) Modification(document, "N001", "Rot0", NativeValueType::Unreal, 90.0f);
        fixture.SetNative(document);
        Rejected(fixture.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "Custom桥不以父context推断使用字段的对象选择器");
    }
    auto unused = BridgeOriginalFixture();
    unused.metadata[1]["useSpecific"] = "S001"; unused.metadata[2]["notSpecific"] = "N001";
    unused.metadata.push_back({{"ID", "Name"}, {"field", "Name"}, {"slk", "DestructableData"}, {"type", "string"}, {"useSpecific", "Z001"}});
    unused.Tables(); unused.SetNative(CustomBridge());
    Require(Take(unused.Inspect({"N001"})).footprints.size() == 1, "未用path/rotation及其它字段选择器不导致整表拒绝");
    auto original = BridgeOriginalFixture(); original.metadata[1]["useSpecific"] = "S001"; original.Tables();
    original.SetNative(Original("S001", "Pth0", NativeValueType::String, std::string("PathTextures\\original.tga")));
    Require(Take(original.Inspect({"S001"})).footprints.size() == 1, "Custom选择器收口不改变标准Original已支持上下文");
}

void WalkableBridgeGridUsesWireRawcodeFallback() {
    for (const std::string rawcode : {"S001", "Sc01", "CS01"}) {
        for (const std::string category : {"", "C", "E", "_", "unknown"}) {
            auto fixture = WalkableBridgeFixture();
            fixture.destructables[0]["destructableID"] = rawcode;
            fixture.destructables[0]["code"] = "BC01";
            if (!category.empty()) fixture.destructables[0]["category"] = category;
            fixture.Tables();
            const auto result = Take(fixture.Inspect({rawcode}));
            Require(result.footprints.size() == 1 && result.footprints[0].rawcode == rawcode
                && result.footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
                "缺同名Doodad时仅wire rawcode第二字符决定非C回退，Destructable.category和code不能改网格");
        }
    }
    for (const std::string rawcode : {"SC01", "BC01", "CC01"}) {
        auto fixture = WalkableBridgeFixture();
        fixture.destructables[0]["destructableID"] = rawcode;
        fixture.destructables[0]["category"] = "E";
        fixture.destructables[0]["code"] = "B001"; fixture.Tables();
        const auto actual = Take(fixture.Inspect({rawcode}));
        Require(actual.footprints.size() == 1
            && actual.footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC,
            "wire第二字符C请求可信startup目录，不能用Destructable.category或code伪造2格或4格");
    }
    for (const bool custom : {false, true}) for (const bool modified : {false, true}) {
        auto fixture = BridgeOriginalFixture();
        if (!custom) fixture.destructables[0]["destructableID"] = "BC01";
        fixture.Tables();
        auto document = Empty();
        if (custom) Check(document.AddCustomObject("BC01", "S001")); else Check(document.AddOriginalObject("BC01"));
        if (modified) {
            Modification(document, "BC01", "X001", NativeValueType::Int32, std::int32_t{1});
            Modification(document, "BC01", "Pth0", NativeValueType::String, std::string("PathTextures\\CBridge.tga"));
            Modification(document, "BC01", "Rot0", NativeValueType::Unreal, 90.0f);
        }
        fixture.SetNative(document); const auto before = fixture.sources[kW3b].bytes;
        const auto result = Take(fixture.Inspect({"BC01", "BC01"}));
        Require(result.footprints.size() == 1 && result.footprints[0].rawcode == "BC01"
            && result.footprints[0].gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC
            && result.footprints[0].contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
            "空或有实际覆盖的Original/Custom C桥保留抑制贡献，只委托网格身份给共用owner");
        Take(fixture.Inspect({"BC01", "BC01"}, result.workBytes));
        Rejected(fixture.Inspect({"BC01", "BC01"}, result.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "C桥仍受相同精确解析预算约束");
        Require(fixture.sources[kW3b].bytes == before, "C桥分类不改W3B源字节");
    }
}

void SpecialDoodadFieldsDoNotBorrowOrdinaryPoseRules() {
    Fixture f;
    f.doodads[0]["pathTex"] = "PathTextures\\special.tga";
    f.doodads[0]["category"] = "C"; f.doodads[0]["fixedRot"] = "45";
    f.Tables();
    const std::array<std::string_view, 2> ids{"D001", "D001"};
    auto result = Take(InspectNativeTerrainObjects({}, ids, f.sources, kWorkingBytes));
    Require(result.footprints.empty() && result.specialFootprints.size() == 1
        && result.specialFootprints.front().pathTexture == "PathTextures\\special.tga"
        && result.specialFootprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::SpecialTerrain
        && !result.specialFootprints.front().fixedRotationDegrees,
        "special records use actual pathTex but no ordinary category/rotation/snap rule");
    Take(InspectNativeTerrainObjects({}, ids, f.sources, result.workBytes));
    Rejected(InspectNativeTerrainObjects({}, ids, f.sources, result.workBytes-1), "TERRAIN_NATIVE_OBJECT_BUDGET", "special output has an exact bounded parse budget");
    const auto shared = Take(InspectNativeTerrainObjects(ids, ids, f.sources, kWorkingBytes));
    Require(shared.footprints.size() == 1 && shared.specialFootprints.size() == 1
        && shared.footprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC
        && shared.specialFootprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::SpecialTerrain,
        "同rawcode普通C与special仍各自绑定独立策略，special不能豁免普通实例证明");
    auto document = Empty(ObjectType::Doodad); Check(document.AddCustomObject("C001", "D001"));
    Modification(document, "C001", "Dt00", NativeValueType::String, std::string{}); f.SetNative(document); f.AddDoodadMetadata();
    const std::array<std::string_view, 1> custom{"C001"};
    result = Take(InspectNativeTerrainObjects({}, custom, f.sources, kWorkingBytes));
    Require(result.specialFootprints.size() == 1 && result.specialFootprints[0].pathTexture.empty(),
        "explicit disabled special path remains bound to the actual custom record");
    const std::array<std::string_view, 1> wrong{"S002"};
    Rejected(InspectNativeTerrainObjects({}, wrong, f.sources, kWorkingBytes), "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS",
        "special DOO cannot borrow destructable zero-walkable proof");
}
#include "NativeDoodadNonPathFieldCases.h"
#include "TerrainEmptyParentCases.h"
#include "TerrainBridgeNameCases.h"
} // namespace

int main() {
    try {
        TypedOriginalAndMetadataIdentity();
        StandardAndSingleParentContext();
        WalkableValuesAndWireTypes();
        MetadataMustBeKnownAndUnambiguous();
        MetadataLogicalCellsMustNotBeDecodedTwice();
        StrictSlkStructurePrecedesLossyLoaders();
        NativeDamageAndDuplicates();
        CrossClassAndUnsupportedInheritance();
        MissingW3bStillRequiresUniqueClassification();
        StandardDoodadsRequireExplicitDisabledPathing();
        StandardDoodadsDoNotIgnoreNativeOverridesOrOtherObjects();
        DoodadOriginalUsesExactMetadataAndStrings();
        CompleteSourcesAndWorkingBudget();
        NonemptyDoodadInspectionOwnsUniqueBoundedResults();
        NonemptyDoodadInspectionKeepsTheNarrowSourceDomain();
        PatternedFootprintReceivesTheActualFixedRotation();
        DoodadOverridesUseBoundedStandardInheritance();
        DoodadCategoriesUseEffectiveFirstByteAndPlacedId();
        DoodadCategoryOverridesRequireUniqueScalarStringMetadata();
        NativeDoodadRotationUsesExactValuesAndSources();
        WalkableBridgeInspectionOwnsUniqueBoundedResults();
        WalkableBridgeRequiresExplicitStandardFields();
        WalkableBridgeRejectsRelevantNativeRecordsAndClassCollisions();
        WalkableBridgeOriginalUsesEffectiveFields();
        WalkableBridgeOriginalRejectsUnknownOrInexactValues();
        WalkableBridgeOriginalRequiresUniqueScalarMetadata();
        WalkableCustomBridgeUsesItsOwnWireIdAndEffectiveFields();
        WalkableCustomBridgeKeepsInheritanceAndWireGridBoundaries();
        WalkableCustomBridgeOnlyRejectsSelectorsOnUsedFields();
        WalkableBridgeGridUsesWireRawcodeFallback();
        SpecialDoodadFieldsDoNotBorrowOrdinaryPoseRules();
        DoodadRepeatedColorFieldsPreservePathing();
        EmptyOriginalParentsKeepExactStandardInheritance();
        BridgeNameOverridesKeepExactPathing();
        std::cout << "native destructable pathing: typed fields, bounded inheritance, unique sources and rejection paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
