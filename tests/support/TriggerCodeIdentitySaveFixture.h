#pragma once
#include "MapCustomSettingsFixture.h"
#include "War3MapCore/Services/TriggerCodeIdentityJson.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include <sstream>

namespace AudastraTest::CodeIdentity {
using namespace War3Map;
using namespace War3Map::Trigger;
using namespace War3Map::Services;
using CustomSettings::Take;
using CustomSettings::Require;
inline std::string Hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef"; std::string value;
    for (auto b : bytes) { const auto n = std::to_integer<unsigned>(b); value += digits[n >> 4]; value += digits[n & 15]; } return value;
}
// 原创小图；正文只用JASS函数，不需要真实引导器、UI运行库或额外游戏二进制。
struct Fixture : CustomSettings::Fixture {
    UiCatalog catalog;
    TriggerDocument triggers;
    StableId custom = 0;
    Fixture() {
        catalog = Take(UiCatalog::Load(base.inputs.tool));
        auto* wtg = &*std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "war3map.wtg"; });
        auto* wct = &*std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "war3map.wct"; });
        triggers = Take(WtgWctCodec{}.Parse(wtg->data, wct->data, catalog));
        const auto category = triggers.categories.empty() ? Take(TriggerDirectoryEditor::AddCategory(triggers, "Code identity")) : triggers.categories.front().categoryId;
        auto added = Take(TriggerDocumentEditor::AddTrigger(triggers, category, "CodeSource"));
        auto* row = TriggerDocumentEditor::FindTrigger(triggers, added); row->customTextFlag = 1;
        row->customText.text = "// InitTrig_CodeSource stays in comment\r\n"
            "function Trig_CodeSource_Action takes nothing returns nothing\r\nendfunction\r\n"
            "function InitTrig_CodeSource takes nothing returns nothing\r\ncall Trig_CodeSource_Action()\r\nendfunction\r\n";
        const auto encoded = Take(WtgWctCodec{}.Serialize(triggers, catalog)); wtg->data = encoded.wtg; wct->data = encoded.wct;
        triggers = Take(WtgWctCodec{}.Parse(wtg->data, wct->data, catalog));
        custom = std::find_if(triggers.triggers.begin(), triggers.triggers.end(), [](const auto& t) { return t.name.text == "CodeSource"; })->id;
        source = Take(WriteVerifiedArchive(base.root / "code-source.w3x", members));
    }
    TriggerCodeIdentityRequest Edit(std::string name = "CodeNew", bool copy = false) const {
        const auto archive = Take(MapArchiveSession::Open(source.path));
        const auto objects = Take(CaptureArchiveMapObjectReferences(archive, base.inputs.game));
        TriggerCodeIdentityRequest request; request.action = copy ? TriggerCodeIdentityAction::Copy : TriggerCodeIdentityAction::Rename;
        request.expected = Take(CaptureTriggerGraphBinding(triggers, catalog, objects->References().empty() ? nullptr : objects.get()));
        const auto& row = *std::find_if(triggers.triggers.begin(), triggers.triggers.end(), [&](const auto& t) { return t.id == custom; });
        request.triggerId = custom; request.expectedName = row.name; request.expectedCodeSha256 = Take(Sha256(Take(row.customText.ToBytes())));
        request.name = std::move(name); request.categoryId = row.categoryId; request.afterTriggerId = custom;
        request.categoryRecordId = std::find_if(triggers.categories.begin(), triggers.categories.end(), [&](const auto& c) { return c.categoryId == row.categoryId; })->id;
        for (const auto& t : triggers.triggers) if (t.categoryId == row.categoryId) request.expectedCategoryOrder.push_back(t.id);
        return request;
    }
};
inline std::string Envelope(const TriggerCodeIdentityRequest& request, bool dryRun) {
    const auto& b = request.expected;
    std::ostringstream out;
    out << "{\"dry_run\":" << (dryRun ? "true" : "false") << ",\"expected\":{\"source_wtg_sha256\":\"" << b.sourceWtgSha256
        << "\",\"source_wct_sha256\":\"" << b.sourceWctSha256 << "\",\"ui_catalog_sha256\":\"" << b.uiCatalogSha256
        << "\",\"current_wtg_sha256\":\"" << b.currentWtgSha256 << "\",\"current_wct_sha256\":\"" << b.currentWctSha256 << '"';
    if (!b.mapObjectReferencesSha256.empty()) out << ",\"map_object_references_sha256\":\"" << b.mapObjectReferencesSha256 << '"';
    out << "},\"operations\":[{\"op\":\"" << (request.action == TriggerCodeIdentityAction::Copy ? "copy_custom_trigger" : "rename_custom_trigger")
        << "\",\"trigger_id\":\"" << request.triggerId << "\",\"expected_name\":{\"text\":\"" << request.expectedName.text
        << "\",\"encoding\":\"utf8\",\"raw_bytes_hex\":\"" << Hex(request.expectedName.originalBytes)
        << "\"},\"expected_code_sha256\":\"" << request.expectedCodeSha256 << "\",\"name\":\"" << request.name << '"';
    if (request.action == TriggerCodeIdentityAction::Copy) {
        out << ",\"category\":{\"kind\":\"category\",\"id\":\"" << request.categoryRecordId << "\"},\"after_trigger_id\":\"" << request.afterTriggerId << "\",\"expected_category_order\":[";
        bool comma = false; for (auto id : request.expectedCategoryOrder) { if (comma) out << ','; comma = true; out << '"' << id << '"'; } out << ']';
    }
    out << "}]}"; return out.str();
}
}
