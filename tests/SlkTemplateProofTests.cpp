#include "War3MapCore/Script/SlkTemplateProof.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Script;
using Row = std::map<std::string, std::string>;
int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Require(const Status& status) {
    if (!status.Succeeded()) throw std::runtime_error("invalid test fixture");
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}
std::string Table(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row) {
        (void)value;
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    }
    std::string text = "ID;PWXL\n";
    for (std::size_t x = 0; x < columns.size(); ++x)
        text += "C;X" + std::to_string(x + 1) + ";Y1;K\"" + columns[x] + "\"\n";
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        if (const auto item = rows[y].find(columns[x]); item != rows[y].end())
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 2) + ";K\"" + item->second + "\"\n";
    }
    return text + "E\n";
}
ObjectFieldSchema Schema(bool ambiguous = false) {
    std::vector<Row> rows{
        {{"ID", "Xdat"}, {"field", "Data"}, {"slk", "AbilityData"}, {"type", "unreal"}, {"repeat", "4"}, {"data", "1"}, {"useSpecific", "B001"}},
        {{"ID", "alev"}, {"field", "levels"}, {"slk", "AbilityData"}, {"type", "int"}},
        {{"ID", "anam"}, {"field", "Name"}, {"slk", "Profile"}, {"type", "string"}, {"index", "0"}},
        {{"ID", "aboo"}, {"field", "Flag"}, {"slk", "AbilityData"}, {"type", "bool"}},
        {{"ID", "area"}, {"field", "Radius"}, {"slk", "AbilityData"}, {"type", "real"}},
        {{"ID", "axxx"}, {"field", "Opaque"}, {"slk", "AbilityData"}, {"type", "UnknownExtension"}},
    };
    if (ambiguous) rows.push_back({{"ID", "Xdax"}, {"field", "Data"}, {"slk", "AbilityData"},
        {"type", "unreal"}, {"repeat", "4"}, {"data", "1"}, {"useSpecific", "B001"}});
    const auto text = Table(rows);
    ObjectFieldSchemaOptions options;
    options.sources = {{ObjectType::Ability, "metadata.slk"}};
    options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
    auto result = ObjectFieldSchema::Load([&](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(text));
    }, options);
    if (!result.Succeeded()) throw std::runtime_error("metadata fixture failed");
    return std::move(*result.value);
}
constexpr SlkTemplateSource kSource{91, 8, 7, 4};
SlkTemplatePlan Plan() {
    SlkTemplatePlan result;
    result.abilities.push_back({"P001", "A001", kSource, {
        {"levels", std::int64_t{2}, kSource},
        {"DataA1", 0.5, kSource},
        {"DataA1", 1.25, kSource},
        {"Name", std::string("测试能力"), kSource},
        {"Flag", true, kSource},
    }});
    return result;
}
SlkTemplateProofOptions Options() {
    SlkTemplateProofOptions options;
    options.resolveStandardParent = [](std::string_view id) {
        if (id != "P001") return Result<ObjectFieldContext>::Failure("Test", "missing", "not a standard parent");
        return Result<ObjectFieldContext>::Success(ObjectFieldContext{"P001", "B001"});
    };
    return options;
}
NativeObjectDocument Native(NativeValueType dataType = NativeValueType::Unreal) {
    auto result = NativeObjectDocument::CreateEmpty(ObjectType::Ability);
    if (!result.Succeeded()) throw std::runtime_error("native fixture failed");
    auto document = std::move(*result.value);
    const ObjectKey object{ObjectType::Ability, "A001"};
    Require(document.AddCustomObject("A001", "P001"));
    Require(document.AddModification(object, {"alev", 0, 0, {}}, NativeValueType::Int32, std::int32_t{2}));
    Require(document.AddModification(object, {"Xdat", 1, 1, {}}, dataType, 1.25f));
    Require(document.AddModification(object, {"anam", 0, 0, {}}, NativeValueType::String, std::string("TRIGSTR_7")));
    Require(document.AddModification(object, {"aboo", 0, 0, {}}, NativeValueType::Int32, std::int32_t{1}));
    // 本 API 不证明额外字段与 new() 的全部继承语义；coverage 必须保留这个边界。
    Require(document.AddModification(object, {"zzzz", 0, 0, {}}, NativeValueType::String, std::string("extra")));
    return document;
}
MapData::StringTableDocument Strings() {
    MapData::StringTableDocument result;
    result.entries.push_back({7, "测试能力", 0});
    return result;
}
void Reject(const Result<SlkTemplateProof>& result, std::string_view code) {
    Check(!result.Succeeded() && !result.value, "failure exposes no partial proof");
    Check(!result.diagnostics.empty() && result.diagnostics.front().severity == DiagnosticSeverity::Error &&
        result.diagnostics.front().code == code, code);
}

void PositiveAndStorage() {
    const auto plan = Plan(); const auto schema = Schema(); const auto strings = Strings(); const auto options = Options();
    auto native = Native();
    NativeObjectCodec codec(ObjectType::Ability);
    const auto before = codec.Serialize(native);
    const auto result = ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options);
    Check(result.Succeeded(), "exact fields, standard code useSpecific, WTS and last write prove");
    if (result.value) {
        Check(result.value->coverage == SlkTemplateProofCoverage::ExplicitAssignmentsOnly, "coverage never claims complete template");
        Check(result.value->abilityCount == 1 && result.value->assignmentCount == 5 && result.value->fields.size() == 4,
            "sequential writes reduced only after all assignments checked");
    }
    const auto after = codec.Serialize(native);
    Check(before.Succeeded() && after.Succeeded() && *before.value == *after.value, "proof and accessors do not mutate native bytes");
    if (before.Succeeded()) {
        const auto decoded = codec.Parse(*before.value);
        const auto* document = dynamic_cast<const NativeObjectDocument*>(decoded.document.get());
        Check(decoded.Succeeded() && document && ProveSlkTemplateExplicitAssignments(plan, *document, schema, strings, options).Succeeded(),
            "serialized native document proves with actual stored type tags");
        auto duplicate = *before.value;
        // 原创 fixture：v2、Original 数为 0、Custom 数为 1；复制完整对象并改成 2 条。
        duplicate[8] = std::byte{2};
        duplicate.insert(duplicate.end(), before.value->begin() + 12, before.value->end());
        const auto ambiguous = codec.Parse(duplicate);
        const auto* unsafe = dynamic_cast<const NativeObjectDocument*>(ambiguous.document.get());
        Check(unsafe && !unsafe->IsEditable(), "duplicate native identity is read-only");
        if (unsafe) {
            Check(!unsafe->ModificationValueType({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}),
                "accessor refuses ambiguous identity");
            Reject(ProveSlkTemplateExplicitAssignments(plan, *unsafe, schema, strings, options), "SLK_PROOF_DOCUMENT_UNSAFE");
        }
    }
    Check(native.ModificationValueType({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}) == NativeValueType::Unreal,
        "accessor returns true Unreal storage");
    Check(!native.ModificationValueType({ObjectType::Ability, "A001"}, {"Xdat", 2, 1, {}}), "accessor needs exact level");
    Check(!native.ModificationValueType({ObjectType::Unit, "A001"}, {"Xdat", 1, 1, {}}), "accessor needs exact object type");
    Require(native.AddCustomObject("A002", "P001"));
    Require(native.AddModification({ObjectType::Ability, "A002"}, {"area", 0, 0, {}}, NativeValueType::Real, 3.0f));
    Require(native.DeleteCustomObject({ObjectType::Ability, "A001"}));
    Check(!native.ModificationValueType({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}), "deleted object has no storage");
    Check(native.ModificationValueType({ObjectType::Ability, "A002"}, {"area", 0, 0, {}}) == NativeValueType::Real,
        "storage accessor follows shifted public object indices");
    Require(native.RemoveModification({ObjectType::Ability, "A002"}, {"area", 0, 0, {}}));
    Check(!native.ModificationValueType({ObjectType::Ability, "A002"}, {"area", 0, 0, {}}), "removed modification has no storage");
}

void IdentityAndMetadata() {
    const auto schema = Schema(); const auto strings = Strings(); const auto options = Options(); const auto native = Native();
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, schema, strings), "SLK_PROOF_PARENT_UNRESOLVED");
    auto plan = Plan(); plan.abilities.front().id = "A099";
    Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_OBJECT_MISSING");
    plan = Plan(); plan.abilities.front().parentId = "P099";
    Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_PARENT_MISMATCH");
    plan = Plan(); plan.abilities.push_back(plan.abilities.front());
    Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_OBJECT_ID_INVALID");
    auto parentOverlay = Native(); Require(parentOverlay.AddOriginalObject("P001"));
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), parentOverlay, schema, strings, options), "SLK_PROOF_PARENT_OVERLAY_UNSUPPORTED");
    auto customParent = Native(); Require(customParent.AddCustomObject("P001", "Q001"));
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), customParent, schema, strings, options), "SLK_PROOF_PARENT_OVERLAY_UNSUPPORTED");
    auto incorrectContext = Options(); incorrectContext.resolveStandardParent = [](std::string_view) {
        return Result<ObjectFieldContext>::Success(ObjectFieldContext{"P001", "B002"});
    };
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, schema, strings, incorrectContext), "SLK_PROOF_FIELD_UNRESOLVED");
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, Schema(true), strings, options), "SLK_PROOF_FIELD_UNRESOLVED");
    plan = Plan(); plan.abilities.front().writes.front().field = "Opaque";
    const auto unknown = ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options);
    Reject(unknown, "SLK_PROOF_FIELD_UNRESOLVED");
    Check(!unknown.diagnostics.empty() && unknown.diagnostics.front().origin &&
        unknown.diagnostics.front().origin->offset == kSource.offset && unknown.diagnostics.front().message.find("7") != std::string::npos,
        "diagnostic keeps template source location");
    plan = Plan(); plan.abilities.front().writes.back().field = "NotInMetadata";
    Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_FIELD_UNRESOLVED");
    auto missing = Native(); Require(missing.RemoveModification({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}));
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), missing, schema, strings, options), "SLK_PROOF_INHERITED_FIELD_UNSUPPORTED");
    Require(missing.AddModification({ObjectType::Ability, "A001"}, {"Xdat", 1, 2, {}}, NativeValueType::Unreal, 1.25f));
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), missing, schema, strings, options), "SLK_PROOF_INHERITED_FIELD_UNSUPPORTED");
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), Native(NativeValueType::Real), schema, strings, options), "SLK_PROOF_NATIVE_TYPE_MISMATCH");
}

void ValuesAndWts() {
    const auto schema = Schema(); const auto options = Options(); const auto native = Native(); const auto strings = Strings();
    for (const SlkTemplateScalar value : {SlkTemplateScalar{2.5}, SlkTemplateScalar{std::int64_t{2147483648LL}},
        SlkTemplateScalar{true}, SlkTemplateScalar{std::string("2")}}) {
        auto plan = Plan(); plan.abilities.front().writes.front().value = value;
        Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_VALUE_UNSUPPORTED");
    }
    for (const double value : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()}) {
        auto plan = Plan(); plan.abilities.front().writes[1].value = value;
        Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_VALUE_UNSUPPORTED");
    }
    auto changed = Native();
    Require(changed.SetModification({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}, std::nextafter(1.25f, 2.0f)));
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), changed, schema, strings, options), "SLK_PROOF_VALUE_MISMATCH");
    auto plan = Plan(); plan.abilities.front().writes[2].value = -0.0;
    Require(changed.SetModification({ObjectType::Ability, "A001"}, {"Xdat", 1, 1, {}}, 0.0f));
    Reject(ProveSlkTemplateExplicitAssignments(plan, changed, schema, strings, options), "SLK_PROOF_VALUE_MISMATCH");
    auto missingStrings = Strings(); missingStrings.entries.clear();
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, schema, missingStrings, options), "SLK_PROOF_WTS_UNRESOLVED");
    auto duplicateStrings = Strings(); duplicateStrings.entries.push_back(duplicateStrings.entries.front());
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, schema, duplicateStrings, options), "SLK_PROOF_WTS_UNRESOLVED");
    auto differentStrings = Strings(); differentStrings.entries.front().value = "不同文本";
    Reject(ProveSlkTemplateExplicitAssignments(Plan(), native, schema, differentStrings, options), "SLK_PROOF_VALUE_MISMATCH");
    plan = Plan(); plan.abilities.front().writes[3].value = std::string("TRIGSTR_7");
    Check(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options).Succeeded(), "expected WTS reference also resolves semantically");
    plan.abilities.front().writes.insert(plan.abilities.front().writes.begin(), {"Name", std::string("TRIGSTR_missing"), kSource});
    Reject(ProveSlkTemplateExplicitAssignments(plan, native, schema, strings, options), "SLK_PROOF_WTS_UNRESOLVED");
}

} // namespace

int main() {
    try { PositiveAndStorage(); IdentityAndMetadata(); ValuesAndWts(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    std::cout << "SlkTemplateProof failures=" << failures << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
