#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::ObjectData;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char value : text) result.push_back(static_cast<std::byte>(value));
    return result;
}

bool HasCode(const Status& status, std::string_view code) {
    for (const auto& diagnostic : status.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

std::unique_ptr<LniObjectDocument> ParseLni(ObjectType type, std::string_view text) {
    auto parsed = LniObjectCodec(type).Parse(Bytes(text));
    if (!parsed.Succeeded()) return {};
    auto* document = dynamic_cast<LniObjectDocument*>(parsed.document.release());
    return std::unique_ptr<LniObjectDocument>(document);
}

std::unique_ptr<NativeObjectDocument> ParseNative(ObjectType type, const std::vector<std::byte>& bytes) {
    auto parsed = NativeObjectCodec(type).Parse(bytes);
    if (!parsed.Succeeded()) return {};
    auto* document = dynamic_cast<NativeObjectDocument*>(parsed.document.release());
    return std::unique_ptr<NativeObjectDocument>(document);
}

StandardObjectCatalog SyntheticCatalog() {
    CatalogLoadOptions options;
    options.stringFiles.clear();
    constexpr std::array types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
    for (const ObjectType type : types) {
        options.sources.push_back(CatalogSourceSpec{type, {"base.slk"}, {}, {"name"}, {"art"}});
    }
    const auto reader = [](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(
            "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
            "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Synthetic base\"\nE\n"));
    };
    auto loaded = StandardObjectCatalog::Load(reader, options);
    Check(loaded.Succeeded() && loaded.value.has_value(), "synthetic catalog loaded");
    return loaded.value ? std::move(*loaded.value) : StandardObjectCatalog{};
}

void TestNestedChainAcrossSevenTypes(const StandardObjectCatalog& catalog) {
    constexpr std::array types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
        ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
    const std::string text =
        "[P001]\n_parent = \"hfoo\"\nI001 = 10\n\n"
        "[C001]\n_parent = \"P001\"\nS001 = \"Child\"\n\n"
        "[D001]\n_parent = \"C001\"\nI001 = 30\n\n";
    for (const ObjectType type : types) {
        auto lni = ParseLni(type, text);
        auto native = NativeObjectDocument::CreateEmpty(type);
        Check(lni && native.value.has_value(), "nested fixture created");
        if (!lni || !native.value) continue;
        const auto originalLni = LniObjectCodec(type).Serialize(*lni);
        Check(ApplyLniChangesToNative(*native.value, *lni, nullptr, &catalog).Succeeded(),
            "nested parent chain exported to native");
        const auto encoded = NativeObjectCodec(type).Serialize(*native.value);
        Check(encoded.Succeeded() && encoded.value.has_value(), "flattened native data encoded");
        if (!encoded.value) continue;
        auto reopened = ParseNative(type, *encoded.value);
        Check(reopened != nullptr, "flattened native data reparsed");
        if (!reopened) continue;
        const ObjectRecord* child = reopened->FindObject({type, "C001"});
        const ObjectRecord* grandchild = reopened->FindObject({type, "D001"});
        Check(child && child->parentRawcode == "hfoo" && child->modifications.size() == 2,
            "child inherits parent value and native base ID");
        Check(grandchild && grandchild->parentRawcode == "hfoo" && grandchild->modifications.size() == 2,
            "grandchild inherits effective values and native base ID");
        if (grandchild) {
            bool foundOverride = false;
            for (const auto& field : grandchild->modifications) {
                if (field.key.fieldId == "I001" && field.value == Value{std::int32_t{30}}) foundOverride = true;
            }
            Check(foundOverride, "nearest child override wins");
        }
        Check(ApplyLniChangesToNative(*reopened, *lni, nullptr, &catalog).Succeeded() &&
            NativeObjectCodec(type).Serialize(*reopened).value == encoded.value,
            "saving flattened baseline again is byte-identical");
        Check(LniObjectCodec(type).Serialize(*lni).value == originalLni.value &&
            lni->FindObject({type, "C001"})->parentRawcode == "P001",
            "editing source preserves original inheritance chain");
    }
}

void TestFailuresAreTransactional(const StandardObjectCatalog& catalog) {
    constexpr ObjectType type = ObjectType::Unit;
    const auto initial = NativeObjectDocument::CreateEmpty(type);
    Check(initial.value.has_value(), "failure fixture created");
    if (!initial.value) return;
    const auto originalBytes = NativeObjectCodec(type).Serialize(*initial.value).value;
    struct FailureCase { std::string_view source; std::string_view code; bool catalogAvailable; };
    const std::array cases{
        FailureCase{"[P001]\n_parent = \"hfoo\"\nI001 = 10\n[C001]\n_parent = \"P001\"\n", "lni-parent-catalog-missing", false},
        FailureCase{"[C001]\n_parent = \"ZZZZ\"\n", "lni-parent-base-missing", true},
        FailureCase{"[P001]\n_parent = \"ZZZZ\"\nI001 = 10\n[C001]\n_parent = \"P001\"\n", "lni-parent-base-missing", true},
        FailureCase{"[P001]\n_parent = \"C001\"\n[C001]\n_parent = \"P001\"\n", "lni-parent-cycle", true},
        FailureCase{"[hfoo]\n_parent = \"P001\"\n[P001]\n_parent = \"hfoo\"\n[C001]\n_parent = \"P001\"\n", "lni-parent-standard-id-conflict", true},
        FailureCase{"[P001]\n_parent = \"hfoo\"\nI001 = {\n\"opaque\",\n}\n[C001]\n_parent = \"P001\"\n", "lni-parent-opaque-field", true},
        FailureCase{"[P001]\n_parent = \"hfoo\"\nR001 = 1.5\n[C001]\n_parent = \"P001\"\n", "lni-parent-float-type-unknown", true},
        FailureCase{"[P001]\n_parent = \"hfoo\"\nI001 = 10\n[C001]\n_parent = \"P001\"\nI001 = \"wrong type\"\n", "lni-parent-field-type-conflict", true},
    };
    for (const FailureCase& test : cases) {
        auto document = *initial.value;
        auto lni = ParseLni(type, test.source);
        Check(lni != nullptr, "failure LNI fixture parsed");
        if (!lni) continue;
        const Status result = ApplyLniChangesToNative(document, *lni, nullptr,
            test.catalogAvailable ? &catalog : nullptr);
        Check(!result.Succeeded() && HasCode(result, test.code), test.code);
        Check(NativeObjectCodec(type).Serialize(document).value == originalBytes,
            "failed flatten leaves caller native bytes unchanged");
    }

    auto existing = NativeObjectDocument::CreateEmpty(type);
    Check(existing.value && existing.value->AddCustomObject("P001", "hfoo").Succeeded() &&
        existing.value->AddCustomObject("C001", "P001").Succeeded(),
        "unflattened native fixture created");
    if (!existing.value) return;
    const auto existingBytes = NativeObjectCodec(type).Serialize(*existing.value).value;
    auto lni = ParseLni(type, "[P001]\n_parent = \"hfoo\"\n[C001]\n_parent = \"P001\"\n");
    const Status rejected = lni ? ApplyLniChangesToNative(*existing.value, *lni, nullptr, &catalog)
        : Status::Failure("Test", "fixture-unavailable", "LNI fixture parse failed");
    Check(lni && !rejected.Succeeded() && HasCode(rejected, "lni-existing-parent-unflattened") &&
        NativeObjectCodec(type).Serialize(*existing.value).value == existingBytes,
        "existing native custom parent is not silently reparented or partially changed");
}

void TestInheritedChangeUpdatesSavedChild(const StandardObjectCatalog& catalog) {
    constexpr ObjectType type = ObjectType::Unit;
    auto lni = ParseLni(type,
        "[P001]\n_parent = \"hfoo\"\nI001 = 10\n"
        "[C001]\n_parent = \"P001\"\nS001 = \"Child\"\n");
    auto native = NativeObjectDocument::CreateEmpty(type);
    Check(lni && native.value && ApplyLniChangesToNative(*native.value, *lni, nullptr, &catalog).Succeeded(),
        "inherited change fixture created");
    if (!lni || !native.value) return;
    const auto first = NativeObjectCodec(type).Serialize(*native.value);
    if (!first.value) return;
    auto reopened = ParseNative(type, *first.value);
    Check(lni->SetModification({type, "P001"}, {"I001"}, std::int32_t{20}).Succeeded(),
        "parent edit applied to project document");
    Check(reopened && ApplyLniChangesToNative(*reopened, *lni, nullptr, &catalog).Succeeded(),
        "parent edit updates flattened child");
    if (!reopened) return;
    const auto second = NativeObjectCodec(type).Serialize(*reopened);
    auto verified = second.value ? ParseNative(type, *second.value) : nullptr;
    const ObjectRecord* child = verified ? verified->FindObject({type, "C001"}) : nullptr;
    bool inherited = false;
    if (child) {
        for (const auto& field : child->modifications) {
            if (field.key.fieldId == "I001" && field.value == Value{std::int32_t{20}}) inherited = true;
        }
    }
    Check(inherited, "saved child carries changed inherited value");

    auto direct = ParseLni(type, "[S001]\n_parent = \"hfoo\"\nI001 = 5\n");
    auto empty = NativeObjectDocument::CreateEmpty(type);
    Check(direct && empty.value && ApplyLniChangesToNative(*empty.value, *direct).Succeeded(),
        "single-level custom object retains existing catalog-free behavior");
}

} // namespace

int main() {
    const StandardObjectCatalog catalog = SyntheticCatalog();
    TestNestedChainAcrossSevenTypes(catalog);
    TestFailuresAreTransactional(catalog);
    TestInheritedChangeUpdatesSavedChild(catalog);
    return failures == 0 ? 0 : 1;
}
