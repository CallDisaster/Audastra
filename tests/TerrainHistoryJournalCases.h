#pragma once

// 在TerrainStrokeJournalTests.cpp的SessionFixture之后包含，全部资源为原创CPU小样例。
bool HasHistoryCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

void BindHistoryPhysicalAnchor(TerrainStrokeJournal& journal, const Bytes& terrain, const Bytes& pathing) {
    journal.sourceSha256.assign(64, 'a'); journal.members.clear();
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
        const auto* bytes = i == 0 ? &terrain : i == 1 ? &pathing : nullptr;
        journal.members.push_back({std::string(TerrainJournalMemberNames[i]), bytes != nullptr,
            bytes ? Take(Sha256(*bytes)) : std::string{}, bytes ? bytes->size() : 0});
    }
}

void HistoryLeaseBindsTheRealSessionRevisionAndOneSavedEpoch() {
    SessionFixture untouched;
    Require(HasHistoryCode(untouched.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_NOT_AVAILABLE"),
        "no typed operations has no optional history lease");
    SessionFixture f; f.Raise();
    const auto first = Take(f.session.PrepareTerrainHistoryForSave());
    Require(!f.session.CommitTerrainHistorySave(first).Succeeded(), "lease cannot claim that an unsaved map has a saved baseline");
    SessionFixture other; other.Raise(); other.session.CommitSaved(other.session.Snapshot());
    Require(!other.session.CommitTerrainHistorySave(first).Succeeded(), "a sealed lease never crosses session identity");
    f.Raise(32);
    Require(!f.session.CommitTerrainHistorySave(first).Succeeded(), "a later edit invalidates the save lease before installation");
    const auto lease = Take(f.session.PrepareTerrainHistoryForSave());
    f.session.CommitSaved(f.session.Snapshot());
    Require(f.session.CommitTerrainHistorySave(lease).Succeeded(), "real same-revision baseline commit installs one history epoch");
    Require(!f.session.CommitTerrainHistorySave(lease).Succeeded(), "a successful save lease cannot be installed twice");
    const auto unchanged = Take(f.session.PrepareTerrainHistoryForSave());
    f.session.CommitSaved(f.session.Snapshot());
    Require(f.session.CommitTerrainHistorySave(unchanged).Succeeded()
        && Take(f.session.ExportTerrainHistoryJournal()).history->savedIndex == 2,
        "unchanged map save preserves the real station instead of rejecting by save count");
    const auto before = Take(SerializeMapTerrain(*f.session.Terrain()));
    Require(!f.session.ExportTerrainHistoryJournal(1).Succeeded()
        && Take(SerializeMapTerrain(*f.session.Terrain())) == before, "history budget refusal preserves the complete raw draft");
    f.Raise(48);
    Require(HasHistoryCode(f.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_EPOCH_LIMIT"),
        "second actual edited save cannot be represented as one station");
    f.session.CommitSaved(f.session.Snapshot());
    Require(HasHistoryCode(f.session.ExportTerrainHistoryJournal(), "TERRAIN_HISTORY_EPOCH_LIMIT"),
        "a second successful save does not silently reuse the first epoch's physical source");
}

void HistoryLinearStackKeepsCoreQtUndoRedoAndFullOldBaseline() {
    SessionFixture f; f.Raise(16); const auto first = f.session.LastEdit();
    const auto firstTerrain = Take(SerializeMapTerrain(*f.session.Terrain()));
    f.Raise(32); const auto second = f.session.LastEdit();
    const auto savedTerrain = Take(SerializeMapTerrain(*f.session.Terrain()));
    const auto lease = Take(f.session.PrepareTerrainHistoryForSave());
    f.session.CommitSaved(f.session.Snapshot()); Require(f.session.CommitTerrainHistorySave(lease).Succeeded(), "install real station");
    Require(f.session.Undo(), "core undo crosses the saved station");
    const auto undoneScope = f.session.TerrainJournalStateFingerprint();
    auto journal = Take(f.session.ExportTerrainHistoryJournal());
    Require(journal.history && journal.history->savedIndex == 2 && journal.history->currentIndex == 1
        && journal.strokes.size() == 2 && journal.history->frozenBaseline,
        "core cursor restores the complete logical stack including redo without parsing source bytes on UI");
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "frozen history cannot be persisted as wire authority");
    Require(MaterializeTerrainJournalPlacements(journal).Succeeded(), "worker materializes all old structural bytes and oldP");
    Require(!journal.history->frozenBaseline && !journal.history->frozenRetainedBytes
        && journal.history->baselineMembers.size() == 10 && journal.history->baselineMembers[0].bytes == f.terrainBytes
        && journal.history->baselineMembers[1].bytes == f.pathing
        && std::all_of(journal.history->baselineMembers.begin() + 2, journal.history->baselineMembers.end(), [](const auto& member) {
            return !member.present && member.bytes.empty();
        }), "history keeps exact old source bytes and absence, never the saved final terrain as its origin");
    BindHistoryPhysicalAnchor(journal, savedTerrain, f.pathing);
    const auto bytes = Take(EncodeTerrainStrokeJournal(journal));
    const auto decoded = Take(DecodeTerrainStrokeJournal(bytes));
    Require(bytes[3] == std::byte{'E'} && decoded.history->savedIndex == 2 && decoded.history->currentIndex == 1
        && Take(EncodeTerrainStrokeJournal(decoded)) == bytes, "FTJE restores exact stack positions and canonical complete bytes");
    const auto old = Take(TerrainHistoryBaselineJournal(decoded));
    Require(!old.history && old.sourceSha256 != decoded.sourceSha256 && old.members[0].sha256 == Take(Sha256(f.terrainBytes))
        && old.members[0].sha256 != decoded.members[0].sha256 && old.strokes.size() == 2,
        "historical structural digest and members remain distinct from the current physical anchor");
    Require(Take(SerializeMapTerrain(*f.session.Terrain())) == firstTerrain && f.session.Redo(),
        "export and codec do not execute or mutate a history command");
    Require(f.session.TerrainJournalStateFingerprint() != undoneScope, "saved and undone positions have distinct lightweight FTJE CAS scopes");
    Require(f.session.Replay(second, false).Succeeded() && f.session.Replay(first, false).Succeeded(), "Qt inverses cross saved station");
    auto qt = Take(f.session.ExportTerrainHistoryJournal());
    Require(qt.history->savedIndex == 2 && qt.history->currentIndex == 0 && qt.strokes.size() == 2,
        "Qt inverse operations reduce logical current index while keeping both redo receipts");
    Require(f.session.Replay(first, true).Succeeded() && f.session.Replay(second, true).Succeeded(), "Qt redos restore original logical branch");
    qt = Take(f.session.ExportTerrainHistoryJournal());
    Require(qt.history->savedIndex == 2 && qt.history->currentIndex == 2
        && Take(SerializeMapTerrain(*f.session.Terrain())) == savedTerrain, "Qt and Core agree on the same saved clean endpoint");
    f.Raise(48);
    const auto next = Take(f.session.ExportTerrainHistoryJournal());
    Require(next.strokes.size() == 3 && next.history->savedIndex == 2 && next.history->currentIndex == 3,
        "new linear commands after a save share the first history window");
    Require(decoded.history->currentIndex == 1 && decoded.strokes.size() == 2
        && decoded.history->baselineMembers[0].bytes == f.terrainBytes, "later live edits cannot change the already frozen cold history");
}

void HistoryRejectsBrokenSavedBranchRawCommandsAndMovedAliases() {
    SessionFixture redo; redo.Raise(); redo.Raise(32); Require(redo.session.Undo(), "first-save redo fixture");
    Require(HasHistoryCode(redo.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_SAVE_REDO"),
        "pre-existing old-epoch redo at the first save is explicit rather than rehashing its original rules");
    Require(redo.session.Undo()
        && HasHistoryCode(redo.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_SAVE_REDO"),
        "an entirely undone old-epoch redo stack is not silently called absent history");
    SessionFixture f; f.Raise(); const auto first = f.session.LastEdit(); f.Raise(32); const auto second = f.session.LastEdit();
    const auto lease = Take(f.session.PrepareTerrainHistoryForSave());
    f.session.CommitSaved(f.session.Snapshot()); Require(f.session.CommitTerrainHistorySave(lease).Succeeded(), "save branch fixture");
    Require(f.session.Replay(second, false).Succeeded() && f.session.Replay(first, false).Succeeded()
        && HasHistoryCode(f.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_EPOCH_LIMIT"),
        "saving after undo to zero is an explicit second epoch, never optional missing history");
    Require(f.session.Replay(first, true).Succeeded() && f.session.Replay(second, true).Succeeded(), "return to saved branch before branch test");
    Require(f.session.Replay(second, false).Succeeded(), "undo to branch ancestor");
    f.Raise(48);
    const auto terrain = Take(SerializeMapTerrain(*f.session.Terrain()));
    Require(HasHistoryCode(f.session.ExportTerrainHistoryJournal(), "TERRAIN_HISTORY_BRANCH")
        && Take(SerializeMapTerrain(*f.session.Terrain())) == terrain, "replaced saved branch keeps raw and rejects invented clean index");
    SessionFixture raw;
    auto changed = raw.terrain; changed.vertices[40].groundHeightRaw += 16; RefreshTerrainDerived(changed, 0, 0, 9, 9);
    Require(raw.session.SetTerrain(Take(SerializeMapTerrain(changed))).Succeeded()
        && HasHistoryCode(raw.session.PrepareTerrainHistoryForSave(), "TERRAIN_HISTORY_UNEXPRESSIBLE"),
        "raw terrain patches cannot acquire typed history by passing a save station");
    SessionFixture frozen; frozen.Raise();
    const auto real = Take(frozen.session.PrepareTerrainHistoryForSave());
    frozen.session.CommitSaved(frozen.session.Snapshot()); Require(frozen.session.CommitTerrainHistorySave(real).Succeeded(), "saved alias fixture");
    auto bad = Take(frozen.session.ExportTerrainHistoryJournal());
    const auto retained = std::move(bad.history->frozenBaseline);
    Require(!MaterializeTerrainJournalPlacements(bad).Succeeded(), "moved-from epoch alias fails before private data dereference");
    (void)first; (void)retained;
}

void HistoryCodecRejectsTruncationIndicesOldVersionsAndTampering() {
    SessionFixture f; f.Raise(); const auto lease = Take(f.session.PrepareTerrainHistoryForSave());
    const auto saved = Take(SerializeMapTerrain(*f.session.Terrain()));
    f.session.CommitSaved(f.session.Snapshot()); Require(f.session.CommitTerrainHistorySave(lease).Succeeded(), "codec fixture station");
    auto journal = Take(f.session.ExportTerrainHistoryJournal()); Require(MaterializeTerrainJournalPlacements(journal).Succeeded(), "materialize codec fixture");
    BindHistoryPhysicalAnchor(journal, saved, f.pathing);
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    for (std::size_t length = 0; length < wire.size(); ++length)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(length)).Succeeded(), "every truncated FTJE rejects the entire history");
    auto trailing = wire; trailing.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(trailing).Succeeded(), "history rejects unknown trailing data");
    for (const char version : {'1', '9', 'A', 'B', 'C', 'D', 'F'}) {
        auto changed = wire; changed[3] = std::byte(version);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "history cannot borrow old or unknown opcode domains");
    }
    for (unsigned badIndex = 0; badIndex < 4; ++badIndex) {
        auto invalid = journal;
        if (badIndex == 0) invalid.history->savedIndex = 0;
        if (badIndex == 1) invalid.history->savedIndex = 2;
        if (badIndex == 2) invalid.history->currentIndex = 2;
        if (badIndex == 3) invalid.history->currentIndex = std::numeric_limits<std::uint32_t>::max();
        Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "saved/current index must describe the complete bounded logical stack");
    }
    auto invalid = journal; invalid.history->baselineMembers.pop_back();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "history requires all ten explicit source member presences");
    invalid = journal; invalid.history->baselineMembers[2].bytes = {std::byte{1}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "absent historical member cannot carry raw payload");
    invalid = journal; invalid.history->frozenRetainedBytes = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "runtime-only frozen bookkeeping is never a persisted qualification");
    // 独立定位外层旧W3E正文：修改raw而保持内层结构manifest不变，必须被完整SHA绑定拒绝。
    const auto begin = std::search(wire.begin(), wire.end(), f.terrainBytes.begin(), f.terrainBytes.end());
    Require(begin != wire.end(), "literal original terrain payload exists in the history envelope");
    auto tampered = wire; tampered[static_cast<std::size_t>(begin - wire.begin()) + f.terrainBytes.size() - 1] ^= std::byte{1};
    Require(!DecodeTerrainStrokeJournal(tampered).Succeeded(), "old baseline bytes cannot drift from the independently nested typed manifest");
    const auto legacy = Take(EncodeTerrainStrokeJournal(Journal()));
    Require(legacy[3] == std::byte{'1'} && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(legacy)))) == legacy,
        "history support leaves original FTJ1 bytes and minimal version unchanged");
}
