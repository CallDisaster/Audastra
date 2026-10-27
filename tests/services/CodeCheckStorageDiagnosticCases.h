#pragma once

void UnsupportedLocalStorageWarningReachesCheckAndSave() {
    ModernProjectFixture fixture("// original no handle storage macro\n", true);
    auto document = std::make_shared<Trigger::TriggerDocument>(*fixture.frozen->triggers);
    document->categories.push_back({900002, 0, Trigger::RawText{"原创存取诊断"}, 0, {}});
    const auto trigger = Trigger::TriggerDocumentEditor::AddTrigger(*document, 0, "StorageProbe");
    Require(trigger.Succeeded() && trigger.value, "add original two-action storage probe");
    const auto binding = Trigger::CaptureTriggerGraphBinding(*document, *fixture.frozen->catalog);
    Require(binding.Succeeded() && binding.value, "bind original storage probe graph");
    Trigger::TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(Trigger::GraphReplaceTriggerText{{Trigger::GraphRecordKind::Trigger, *trigger.value, {}},
        "actions:\n    InitBlizzard()\n    RecordInteger(GetHandleId(YDWEGetAnyTypeLocalVariable(\"英雄\")))\n"});
    const auto edited = Trigger::ApplyTriggerGraphEditBatch(*document, *fixture.frozen->catalog, batch);
    std::string details;
    for (const auto& d : edited.diagnostics) details += d.code + ":" + d.message + "\n";
    Require(edited.Succeeded(), "parse the original two-line T probe: " + details);
    CodeCheckProjectCache cache;
    const auto checked = CheckProjectTriggers(fixture.frozen, cache, document, 701);
    details = checked.reason;
    for (const auto& d : checked.diagnostics) details += "\n" + d.code + ":" + d.message;
    const auto warning = std::find_if(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& d) {
        return d.code == "storage-type-unsupported" && d.severity == DiagnosticSeverity::Warning;
    });
    Require(warning != checked.diagnostics.end(), "the shared check retains the generator warning: " + details);
    Require(warning->trigger && warning->trigger->triggerId == *trigger.value && warning->trigger->triggerName == "StorageProbe"
        && warning->trigger->ecaId != 0 && warning->trigger->function == "YDWEGetAnyTypeLocalVariable"
        && warning->trigger->line == 3 && warning->trigger->column == 31,
        "storage warning identifies the nested local read at T line 3 column 31: " + details);
    Require(warning->message.find("具体类型（如单位）") != std::string::npos && warning->message.find("全局表达式") != std::string::npos,
        "warning gives the concrete supported workaround");
    Require(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& d) {
        return d.severity == DiagnosticSeverity::Error;
    }) && !checked.Succeeded(), "syntax errors remain visible; a warning cannot turn unsupported macros into success");

    const auto encoded = Trigger::WtgWctCodec{}.Serialize(*document, *fixture.frozen->catalog);
    Require(encoded.Succeeded() && encoded.value, "serialize the same authored T through the production codec");
    auto original = MapArchiveSession::Open(fixture.source);
    Require(original.Succeeded() && original.value, "open own source for the save regression");
    std::vector<MapArchiveMember> members(original.value->Members().begin(), original.value->Members().end());
    for (auto& member : members) {
        if (member.name == "war3map.wtg") member.data = encoded.value->wtg;
        if (member.name == "war3map.wct") member.data = encoded.value->wct;
    }
    const auto input = WriteVerifiedArchive(fixture.temporary.Path() / "maps/storage-probe.w3x", members);
    Require(input.Succeeded() && input.value, "write a separate original T source without replacing the baseline");
    auto archive = MapArchiveSession::Open(input.value->path);
    Require(archive.Succeeded() && archive.value, "open own exact storage-probe source");
    auto map = MapDocument::FromArchive(*archive.value);
    const auto row = MapData::MapFieldRowKey(map, MapDataKind::Settings, 0);
    const auto* descriptor = MapData::FindMapField(MapDataKind::Settings, "map_name");
    auto before = MapData::ParseMapFieldInput(*descriptor, map.Settings()->mapName);
    auto after = MapData::ParseMapFieldInput(*descriptor, "Original storage diagnostic save");
    const auto sourceMember = archive.value->ReadMember("war3map.w3i");
    const auto sha = sourceMember.Succeeded() ? Sha256(*sourceMember.value) : Result<std::string>{};
    Require(row.Succeeded() && before.Succeeded() && after.Succeeded() && sha.Succeeded(), "bind real settings field CAS");
    MapEditSaveRequest request; request.sourceMap = input.value->path; request.expectedSourceSha256 = input.value->sha256;
    request.targetCandidate = fixture.temporary.Path() / "storage-rejected.w3x";
    const auto game = fixture.temporary.Path() / "game";
    Require(std::filesystem::create_directory(game), "create owned original game-declaration fixture");
    AuxiliarySaveFixtures::Declarations declarations;
    declarations.common = std::string(kOriginalCommon)
        + "native GetHandleId takes handle value returns integer\nnative RecordInteger takes integer value returns nothing\n";
    declarations.blizzard = kOriginalBlizzard;
    Require(Mpq::WriteMpqArchive(game / "war3.mpq", AuxiliarySaveFixtures::GameFiles(declarations)).Succeeded(),
        "normal save uses actual owned MPQ declarations, not an unbound tool directory");
    request.toolRoot = fixture.tool; request.gameRoot = game;
    request.fieldEdits = {{*sha.value, {MapDataKind::Settings, 0, *row.value, "map_name", *before.value, *after.value}}};
    const auto saved = SaveMapEditCandidate(request);
    details.clear();
    for (const auto& diagnostic : saved.diagnostics) details += diagnostic.code + ": " + diagnostic.message + "\n";
    const auto savedWarning = std::find_if(saved.diagnostics.begin(), saved.diagnostics.end(), [](const auto& d) {
        return d.code == "storage-type-unsupported" && d.severity == DiagnosticSeverity::Warning;
    });
    Require(!saved.Succeeded() && !std::filesystem::exists(request.targetCandidate), "unsupported generated syntax still prevents normal candidate publication");
    Require(savedWarning != saved.diagnostics.end() && savedWarning->origin && savedWarning->origin->source == "Map/generated.j"
        && savedWarning->message.find("T第3行31列") != std::string::npos && savedWarning->message.find("eca_id=") != std::string::npos,
        "the normal save receipt retains the same warning and precise T/statement provenance: " + details);
    const auto unchanged = Sha256File(input.value->path);
    Require(unchanged.Succeeded() && *unchanged.value == input.value->sha256, "failed save preserves the authored T source");
    fixture.RequireSourceUnchanged();
}
