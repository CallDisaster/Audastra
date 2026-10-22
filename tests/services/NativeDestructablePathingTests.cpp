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
constexpr std::size_t kW3b = 0, kW3d = 1, kDestructables = 2, kDoodads = 3, kMetadata = 4;

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

void Rejected(const Result<std::size_t>& result, std::string_view code, std::string_view label) {
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
    Rejected(unknown.Validate({"D001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "普通doodad继续拒绝");
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
        CompleteSourcesAndWorkingBudget();
        std::cout << "native destructable pathing: typed fields, bounded inheritance, unique sources and rejection paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
