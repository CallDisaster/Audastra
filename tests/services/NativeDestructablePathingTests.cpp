#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <algorithm>
#include <array>
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
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "其它字段的字符串0不冒充walkable覆盖");
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
    Rejected(fixture.Validate({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "标准true仍需真实静态通行实现");
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
        if (mode == 0) { // 父虽无字段，仍属于Original覆盖，不能借空记录猜继承。
            Check(document.AddOriginalObject("S002"));
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
        Require(footprint.pathTexture == path && footprint.snapCells == 2, "实际路径原样保留，非C与小写c均为2格");
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
    Rejected(missing.Inspect(), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "非空占地不能猜缺失类别");
    missing.doodads.push_back({{"doodID", "D002"}, {"category", "E"}, {"pathTex", "_"}}); missing.Tables();
    Rejected(missing.Inspect(), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "有category列但目标行缺格也不能猜类别");
    for (const std::string category : {"", "_", "C", "CC", " C", "C ", "\"C\"", " ", "\t", "\x7f", "\xc3\xa9"}) {
        Fixture invalid; prepare(invalid); invalid.doodads[0]["category"] = category; invalid.Tables();
        const auto result = invalid.Inspect();
        Rejected(result, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "类别必须是明确非C、非_的单可打印ASCII字符");
        if (category == "C") Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.message.find("类别目录") != std::string::npos;
        }), "C类明确说明尚缺实际类别目录绑定");
    }
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
            Check(document.AddOriginalObject("D001")); f.SetNative(document);
            Rejected(f.Inspect({id}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "custom parent with any native override remains unsupported");
        }
    }
    for (const auto* key : {"category", "fixedRot"}) {
        Fixture f; f.doodads[0]["pathTex"] = "PathTextures\\base.tga"; f.doodads[0]["category"] = "E";
        f.doodadMetadata.push_back({{"ID", "Pos0"}, {"field", key}, {"slk", "DoodadData"}, {"type", "string"}, {"repeat", "0"}});
        f.Tables(); f.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Pos0", NativeValueType::String, std::string("0")); f.SetNative(document);
        Rejected(f.Inspect(), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "native pose override cannot borrow the standard pose");
        Modification(document, "D001", "Dt00", NativeValueType::String, std::string{}); f.SetNative(document);
        Require(Take(f.Inspect()).footprints.empty(), "explicit zero footprint has no rotation contribution");
    }
    Fixture chain; chain.doodads[0]["pathTex"] = "_"; chain.Tables(); chain.AddDoodadMetadata();
    auto document = Empty(ObjectType::Doodad);
    Check(document.AddCustomObject("C001", "D001")); Check(document.AddCustomObject("C002", "C001")); chain.SetNative(document);
    Rejected(chain.Inspect({"C002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "recursive custom inheritance remains explicit and refused");
}
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
        std::cout << "native destructable pathing: typed fields, bounded inheritance, unique sources and rejection paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
