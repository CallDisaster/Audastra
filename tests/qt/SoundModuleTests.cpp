#include "SoundsModule.h"
#include "EditorProject.h"
#include "Fonts/FontLoader.h"

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QApplication>
#include <QAbstractItemDelegate>
#include <QCheckBox>
#include <QFile>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <array>
#include <bit>
#include <utility>

using namespace War3Map;

namespace {

MapData::SoundRecord OriginalSound() {
    MapData::SoundRecord sound{};
    sound.name = "Thunder";
    sound.filePath = "Sounds\\hit.wav";
    sound.eaxSetting = "DefaultEAXON";
    sound.flags = 0x80u; // 当前 UI 未解释的位必须保留。
    sound.fadeInRate = 0;
    sound.fadeOutRate = 0;
    sound.volume = 90;
    sound.pitch = 1.0f;
    sound.channel = 5;
    sound.minDistance = 100.0f;
    sound.maxDistance = 4000.0f;
    sound.distanceCutoff = 2000.0f;
    sound.coneAngleInside = 360.0f;
    sound.coneAngleOutside = 360.0f;
    sound.coneOutsideVolume = 127;
    return sound;
}

bool SetDocument(QtProject::EditorProject& project, bool withSound,
    MapData::SoundRecord sound = OriginalSound()) {
    std::vector<MapArchiveMember> members;
    if (withSound) {
        MapData::SoundsDocument document{1, {std::move(sound)}, {}};
        const auto bytes = document.Serialize();
        if (!bytes.Succeeded() || !bytes.value) return false;
        members.push_back({"war3map.w3s", *bytes.value});
    }
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    return true;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> output;
    for (const char character : text) output.push_back(static_cast<std::byte>(character));
    return output;
}

} // namespace

class SoundModuleTests final : public QObject {
    Q_OBJECT

private slots:
    void EditsRealSoundBytesWithSharedUndo();
    void InspectorPreservesOpaqueFlagsAndRejectsInvalidDraft();
    void NewSoundInMissingMemberUndoRestoresAbsence();
    void ResourceBrowserUsesRealPendingImport();
    void GameCatalogLoadsOffThreadWithExactSourceAndSearch();
    void DisplaysDefaultsAndPreservesTheirBitsOnUnrelatedEdit();
    void ExplicitDefaultButtonsCanBeUndoneAndNoOpDraftClears();
    void UnbrokenPathsWrapAtActualColumnWidthWithoutChangingData();
    void IndexedRevealPreservesDuplicateIdentityAndDraft();
    void SharedNumericInspectorUsesOneReceiptAndExactUndo();
    void SharedNumericRejectsInvalidMixedDraftWithoutHistory();
    void ExistingFullSoundEditsRemainOutsideMcpFreezeScope();
    void SharedNumericRejectsAmbiguousAndChangedSources();
    void InspectorRejectsStaleRevisionEvenWhenBytesMatch();
    void ExtendedSoundsStayReadableButNumericControlsAreReadonly();
    void NamesUseCurrentWtsAndRawToggleWithoutEditingSources();
    void NavigationUsesDefinitionIdentityAndBlocksDraft();
    void NavigationRejectsStaleRevisionAndIdenticalDefinitions();
    void NavigationRejectsPerDefinitionAndAggregateByteBudgets();
};

void SoundModuleTests::NavigationUsesDefinitionIdentityAndBlocksDraft() {
    QtProject::EditorProject project;
    auto first = OriginalSound(), second = first; second.volume = 110;
    const auto bytes = MapData::SoundsDocument{1, {first, second}, {}}.Serialize(); QVERIFY(bytes.Succeeded());
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.w3s", *bytes.value}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::SoundsModule module(project); auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("soundDefinitionsTable");
    auto* resources = page->findChild<QTableWidget*>("soundResourceTable");
    auto* search = page->findChild<QLineEdit*>("soundResourceSearch");
    auto* volume = page->findChild<QLineEdit*>("soundField_volume"); QVERIFY(table && resources && search && volume);
    QVERIFY(module.Reveal({"sounds", "sound", "Thunder", {{"sourceIndex", QStringLiteral("1")}}}));
    search->setText("hit"); QVERIFY(resources->rowCount() > 0); resources->selectRow(0);
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    QVERIFY(bookmark->value("definition_sha256").isString()); QVERIFY(!bookmark->contains("sourceIndex"));
    QVERIFY(!bookmark->contains("definition"));
    const auto revision = project.MapEdits()->Revision(); const auto undoCount = project.mapDataUndoStack_->count();
    QVERIFY(module.Reveal({"sounds", "sound", "Thunder", {{"sourceIndex", QStringLiteral("0")}}}));
    search->setText("nothing");
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(table->currentRow(), 1); QCOMPARE(volume->text(), QStringLiteral("110"));
    QCOMPARE(search->text(), QStringLiteral("hit")); QCOMPARE(resources->currentRow(), 0);
    QCOMPARE(project.MapEdits()->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), undoCount);
    QCOMPARE(project.MapEdits()->Snapshot().Sounds()->Serialize().value, bytes.value);
    volume->setText("42"); QVERIFY(module.HasPendingDraft());
    QVERIFY(!module.PrepareNavigationLeave());
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Blocked);
    QVERIFY(module.HasPendingDraft()); QCOMPARE(volume->text(), QStringLiteral("42")); QCOMPARE(table->currentRow(), 1);
    QCOMPARE(project.MapEdits()->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), undoCount);
}

void SoundModuleTests::NavigationRejectsStaleRevisionAndIdenticalDefinitions() {
    QtProject::EditorProject project; QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project); module.Widget();
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    auto changed = *project.MapEdits()->Snapshot().Sounds(); changed.entries[0].volume = 2;
    QVERIFY(project.MapEdits()->EditSounds(changed).Succeeded());
    QVERIFY(project.MapEdits()->Replay(project.MapEdits()->LastEdit(), false).Succeeded());
    emit project.MapDocumentChanged();
    const auto view = module.SaveViewState();
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), view); QCOMPARE(project.MapEdits()->Snapshot().Sounds()->entries[0].volume, 90);
    QVERIFY(SetDocument(project, true)); emit project.MapDocumentChanged();
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Invalid);
    const auto duplicates = MapData::SoundsDocument{1, {OriginalSound(), OriginalSound()}, {}}.Serialize(); QVERIFY(duplicates.Succeeded());
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.w3s", *duplicates.value}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_); emit project.MapDocumentChanged();
    QVERIFY(!module.CaptureNavigationBookmark().has_value());
}

void SoundModuleTests::NavigationRejectsPerDefinitionAndAggregateByteBudgets() {
    QtProject::EditorProject project; QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project); auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("soundDefinitionsTable");
    auto* search = page->findChild<QLineEdit*>("soundResourceSearch");
    auto* status = page->findChild<QLabel*>("soundStatus"); QVERIFY(table && search && status);
    const auto original = OriginalSound();
    const auto singleton = MapData::SoundsDocument{1, {original}, {}}.Serialize();
    QVERIFY(singleton.Succeeded() && singleton.value);
    QCOMPARE(singleton.value->size(), std::size_t{79} + original.name.size() + original.filePath.size() + original.eaxSetting.size());
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    for (const bool aggregate : {false, true}) {
        std::vector<MapData::SoundRecord> records{original};
        // 每条仍低于64KiB，但20条的总处理字节超过1MiB；另一组单条先超过64KiB。
        const std::size_t count = aggregate ? 20 : 1;
        // Capture 先核选中项，再完整唯一性扫描；两次选中项都计费。
        std::size_t processedBytes = 2 * singleton.value->size();
        for (std::size_t index = 0; index < count; ++index) {
            auto row = original; row.name = "Budget" + std::to_string(index);
            row.eaxSetting = std::string(aggregate ? 60000 : 70000, 'x');
            const auto rowWire = MapData::SoundsDocument{1, {row}, {}}.Serialize();
            QVERIFY(rowWire.Succeeded() && rowWire.value);
            QCOMPARE(rowWire.value->size(), std::size_t{79} + row.name.size() + row.filePath.size() + row.eaxSetting.size());
            QVERIFY(aggregate ? rowWire.value->size() <= 64 * 1024 : rowWire.value->size() > 64 * 1024);
            processedBytes += rowWire.value->size();
            records.push_back(std::move(row));
        }
        QVERIFY(aggregate ? processedBytes > 1024 * 1024 : processedBytes <= 1024 * 1024);
        const auto wire = MapData::SoundsDocument{1, std::move(records), {}}.Serialize();
        QVERIFY(wire.Succeeded() && wire.value);
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
            std::vector<MapArchiveMember>{{"war3map.w3s", *wire.value}}));
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        emit project.MapDocumentChanged();
        QVERIFY(!module.HasPendingDraft());
        const auto revision = project.MapEdits()->Revision(), session = project.MapEdits()->SessionIdentity();
        const auto undoCount = project.mapDataUndoStack_->count(); const auto view = module.SaveViewState();
        const auto selectedRow = table->currentRow(); const auto filter = search->text();
        QCOMPARE(selectedRow, 0); // 单条超限发生在唯一性循环，不能误称选中项或合计预算失败。
        QVERIFY(!module.CaptureNavigationBookmark().has_value());
        QVERIFY2(status->text().contains(aggregate ? QStringLiteral("1MiB") : QStringLiteral("64KiB")), qPrintable(status->text()));
        const auto expectedFailure = aggregate ? QStringLiteral("sound-navigation-total-byte-budget")
            : QStringLiteral("sound-navigation-row-byte-budget");
        QCOMPARE(status->property("navigationIdentityFailure").toString(), expectedFailure);
        QVERIFY(status->property("error").toBool());
        // 正确绑定当前scope的敌意书签仍须完成有预算的唯一性核对，不能因首行命中就放行。
        auto currentScope = *bookmark;
        currentScope["session_identity"] = QString::number(session);
        currentScope["map_revision"] = QString::number(revision);
        QCOMPARE(module.RestoreNavigationBookmark(currentScope), Shell::NavigationRestoreResult::Invalid);
        QVERIFY2(status->text().contains(aggregate ? QStringLiteral("1MiB") : QStringLiteral("64KiB")), qPrintable(status->text()));
        QCOMPARE(status->property("navigationIdentityFailure").toString(), expectedFailure);
        QCOMPARE(module.SaveViewState(), view); QCOMPARE(table->currentRow(), selectedRow); QCOMPARE(search->text(), filter);
        QCOMPARE(project.MapEdits()->Revision(), revision); QCOMPARE(project.MapEdits()->SessionIdentity(), session);
        QCOMPARE(project.mapDataUndoStack_->count(), undoCount); QVERIFY(!module.HasPendingDraft());
        QCOMPARE(project.MapEdits()->Snapshot().Sounds()->Serialize().value, wire.value);
    }
}

void SoundModuleTests::NamesUseCurrentWtsAndRawToggleWithoutEditingSources() {
    QtProject::EditorProject project;
    auto sound = OriginalSound(); sound.name = "TRIGSTR_008";
    MapData::SoundsDocument sounds{1, {sound}, {}};
    const auto wire = sounds.Serialize(); QVERIFY(wire.Succeeded());
    const std::string wts = "STRING 8\r\n{\r\n|cffff0000\xba\xec\xc9\xab|r\r\n}\r\n";
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3map.w3s", *wire.value}, {"war3map.wts", Bytes(wts)}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::SoundsModule module(project); auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("soundDefinitionsTable");
    auto* raw = page->findChild<QCheckBox*>("soundRawValues"); QVERIFY(table && raw);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("红色"));
    QCOMPARE(table->item(0, 0)->toolTip(), QStringLiteral("TRIGSTR_008"));
    raw->setChecked(true); QCOMPARE(table->item(0, 0)->text(), QStringLiteral("TRIGSTR_008"));
    raw->setChecked(false); QCOMPARE(table->item(0, 0)->text(), QStringLiteral("红色"));
    auto strings = *project.mapEdits_->Snapshot().Strings(); strings.entries[0].value = "当前声音";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded()); emit project.MapDocumentChanged();
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("当前声音"));
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].name, std::string("TRIGSTR_008"));
    QCOMPARE(project.mapDocument_->Strings()->Serialize().value, std::optional(Bytes(wts)));
    QCOMPARE(project.mapDocument_->Sounds()->Serialize().value, wire.value);
}

void SoundModuleTests::ExtendedSoundsStayReadableButNumericControlsAreReadonly() {
    for (const auto version : {2u, 3u}) {
        const auto classic = MapData::SoundsDocument{1, {OriginalSound()}, {}}.Serialize();
        QVERIFY(classic.Succeeded() && classic.value);
        auto wire = *classic.value;
        wire[0] = static_cast<std::byte>(version);
        const auto text = [&](std::string_view value) {
            for (const auto c : value) wire.push_back(static_cast<std::byte>(c));
            wire.push_back(std::byte{0});
        };
        const auto integer = [&](std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                wire.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        };
        text("Thunder"); text("label"); text("Sounds\\hit.wav");
        integer(0xfffffff1u); text("opaque2"); integer(0xffffffe9u); integer(0);
        text("opaque6"); text("opaque7"); text("Face"); text("Group"); text("Face.fdf");
        if (version == 3) integer(0xffffffe1u);
        QtProject::EditorProject project;
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
            std::vector<MapArchiveMember>{{"war3map.w3s", wire}}));
        QVERIFY(project.mapDocument_->Sounds());
        QCOMPARE(project.mapDocument_->Sounds()->version, version);
        QVERIFY(project.mapDocument_->Sounds()->entries[0].extended);
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        Modules::SoundsModule module(project);
        auto* page = module.Widget();
        for (const auto* name : {"volume", "fadeInRate", "fadeOutRate"}) {
            auto* field = page->findChild<QLineEdit*>(QStringLiteral("soundField_%1").arg(name));
            QVERIFY(field && !field->isEnabled());
            QVERIFY(!field->text().isEmpty());
            QVERIFY(!module.EditField(0, name, "50").Succeeded());
        }
        QCOMPARE(project.mapDataUndoStack_->count(), 0);
        QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
        QVERIFY(project.mapEdits_->Replacements().empty());
        QVERIFY(project.mapDocument_->Member(MapDataKind::Sounds).sources.front().data == wire);
    }
}

void SoundModuleTests::SharedNumericInspectorUsesOneReceiptAndExactUndo() {
    auto original = OriginalSound();
    original.name = std::string("Raw_") + std::bit_cast<char>(std::uint8_t{0x81}); // 原名字身份不能来自有损显示文字。
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true, original));
    const auto originalBytes = project.mapDocument_->Member(MapDataKind::Sounds).sources.front().data;
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* volume = page->findChild<QLineEdit*>("soundField_volume");
    auto* fadeIn = page->findChild<QLineEdit*>("soundField_fadeInRate");
    auto* fadeOut = page->findChild<QLineEdit*>("soundField_fadeOutRate");
    QVERIFY(volume && fadeIn && fadeOut);
    volume->setText("0");
    fadeIn->setText("2147483647");
    fadeOut->setText(QStringLiteral("默认"));
    QVERIFY(module.ApplyPendingDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{1});
    const auto receipt = project.mapEdits_->LastEdit();
    QVERIFY(receipt.CommandIdentity() != 0);
    QCOMPARE(receipt.SessionIdentity(), project.mapEdits_->SessionIdentity());
    const auto edited = project.mapEdits_->Snapshot().Sounds();
    QVERIFY(edited);
    QCOMPARE(edited->entries[0].name, original.name);
    QCOMPARE(edited->entries[0].volume, 0);
    QCOMPARE(edited->entries[0].fadeInRate, 2147483647);
    QCOMPARE(edited->entries[0].fadeOutRate, -1);
    const auto afterBytes = edited->Serialize();
    QVERIFY(afterBytes.Succeeded() && afterBytes.value);
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->LastAppliedEdit().CommandIdentity(), receipt.CommandIdentity());
    QVERIFY(project.mapEdits_->LastAppliedEdit().Restoring());
    QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == std::optional(originalBytes));
    QCOMPARE(volume->text(), QStringLiteral("90"));
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->LastAppliedEdit().CommandIdentity(), receipt.CommandIdentity());
    QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == afterBytes.value);
    QCOMPARE(fadeOut->text(), QStringLiteral("默认"));
    const auto revision = project.mapEdits_->Revision();
    QVERIFY(module.EditField(0, "fadeOutRate", "默认").Succeeded());
    QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
}

void SoundModuleTests::SharedNumericRejectsInvalidMixedDraftWithoutHistory() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    const auto initial = project.mapEdits_->Snapshot().Sounds()->Serialize().value;
    for (const auto* value : {"-2", "2147483648", "1.5", "true", "null", ""}) {
        QVERIFY(!module.EditField(0, "fadeInRate", value).Succeeded());
        QVERIFY(!module.EditField(0, "fadeOutRate", value).Succeeded());
        QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
        QCOMPARE(project.mapDataUndoStack_->count(), 0);
        QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == initial);
    }
    auto* volume = page->findChild<QLineEdit*>("soundField_volume");
    auto* fade = page->findChild<QLineEdit*>("soundField_fadeOutRate");
    auto* pitch = page->findChild<QLineEdit*>("soundField_pitch");
    QVERIFY(volume && fade && pitch);
    volume->setText("75");
    pitch->setText("1.5");
    fade->setText("-2");
    QVERIFY(!module.ApplyPendingDraft());
    QVERIFY(module.HasPendingDraft());
    QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == initial);
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    fade->setText("10");
    QVERIFY(module.ApplyPendingDraft());
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto receipt = project.mapEdits_->LastEdit();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].pitch, 1.5f);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].volume, 75);
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->LastAppliedEdit().CommandIdentity(), receipt.CommandIdentity());
    QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == initial);
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->LastAppliedEdit().CommandIdentity(), receipt.CommandIdentity());
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].fadeOutRate, 10);
}

void SoundModuleTests::ExistingFullSoundEditsRemainOutsideMcpFreezeScope() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    QVERIFY(module.EditField(0, "pitch", "1.75").Succeeded());
    QVERIFY(module.EditField(0, "volume", "40").Succeeded());
    QCOMPARE(project.mapDataUndoStack_->count(), 2);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].pitch, 1.75f);
    const auto current = MapData::ProjectMapData(*project.mapDocument_, *project.mapEdits_);
    QVERIFY(current.Succeeded() && current.value);
    const auto freeze = MapData::FreezeOriginalMapDataFields(*project.mapDocument_, *current.value);
    QVERIFY(!freeze.Succeeded()); // 旧 UI 宽属性不变成独立 MCP 字段保存资格。
    QCOMPARE(freeze.diagnostics.front().code, std::string("MAP_FIELD_PROJECTION_SCOPE"));
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].pitch, 1.75f);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].volume, 90);
    project.mapDataUndoStack_->undo();
    QVERIFY(!project.mapEdits_->IsDirty());
    project.mapDataUndoStack_->redo();
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].volume, 40);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].pitch, 1.75f);
}

void SoundModuleTests::SharedNumericRejectsAmbiguousAndChangedSources() {
    for (const auto& names : {std::pair{"Thunder", "Thunder"}, std::pair{"A B", "A_B"},
        std::pair{"Thunder", "gg_snd_Thunder"}}) {
        QtProject::EditorProject project;
        auto first = OriginalSound(), second = OriginalSound();
        first.name = names.first; second.name = names.second;
        const auto bytes = MapData::SoundsDocument{1, {first, second}, {}}.Serialize();
        QVERIFY(bytes.Succeeded() && bytes.value);
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
            std::vector<MapArchiveMember>{{"war3map.w3s", *bytes.value}}));
        const auto& source = project.mapDocument_->Member(MapDataKind::Sounds);
        const bool duplicateRawName = first.name == second.name;
        if (duplicateRawName) {
            QCOMPARE(source.state, MapDataState::Partial);
            QCOMPARE(source.diagnostics.size(), std::size_t{1});
            QCOMPARE(source.diagnostics.front().code, std::string("MAP_IDENTITY_AMBIGUOUS"));
            QCOMPARE(source.diagnostics.front().severity, DiagnosticSeverity::Warning);
        } else {
            QCOMPARE(source.state, MapDataState::Ready);
            QVERIFY(source.diagnostics.empty());
        }
        QVERIFY(project.mapDocument_->Sounds());
        QCOMPARE(project.mapDocument_->Sounds()->entries.size(), std::size_t{2});
        QCOMPARE(project.mapDocument_->Sounds()->entries[0].name, first.name);
        QCOMPARE(project.mapDocument_->Sounds()->entries[1].name, second.name);
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        Modules::SoundsModule module(project);
        const auto rejected = module.EditField(0, "volume", "40");
        QVERIFY(!rejected.Succeeded());
        QCOMPARE(rejected.diagnostics.front().code, std::string(duplicateRawName
            ? "MAP_FIELD_SOURCE_UNSAFE" : "MAP_FIELD_SOUND_NAME_AMBIGUOUS"));
        QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
        QCOMPARE(project.mapDataUndoStack_->count(), 0);
        QVERIFY(!project.mapEdits_->CanUndo());
        QVERIFY(project.mapEdits_->Replacements().empty());
        QVERIFY(source.sources.front().data == *bytes.value);
        QVERIFY(project.mapEdits_->Snapshot().Sounds()->Serialize().value == bytes.value);
    }
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto other = OriginalSound(); other.volume = 3;
    const auto otherBytes = MapData::SoundsDocument{1, {other}, {}}.Serialize();
    QVERIFY(otherBytes.Succeeded() && otherBytes.value);
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.w3s", *otherBytes.value}}));
    const auto rejected = module.EditField(0, "volume", "40");
    QVERIFY(!rejected.Succeeded());
    QCOMPARE(rejected.diagnostics.front().code, std::string("MAP_FIELD_ORIGINAL_STALE"));
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].volume, 90);
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void SoundModuleTests::InspectorRejectsStaleRevisionEvenWhenBytesMatch() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto* volume = module.Widget()->findChild<QLineEdit*>("soundField_volume");
    QVERIFY(volume);
    volume->setText("40");
    auto changed = *project.mapEdits_->Snapshot().Sounds();
    changed.entries[0].volume = 2;
    QVERIFY(project.mapEdits_->EditSounds(changed).Succeeded());
    QVERIFY(project.mapEdits_->Replay(project.mapEdits_->LastEdit(), false).Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].volume, 90);
    const auto revision = project.mapEdits_->Revision();
    QVERIFY(!module.ApplyPendingDraft());
    QVERIFY(module.HasPendingDraft());
    QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void SoundModuleTests::IndexedRevealPreservesDuplicateIdentityAndDraft() {
    QtProject::EditorProject project;
    const auto bytes = MapData::SoundsDocument{1, {OriginalSound(), OriginalSound()}, {}}.Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    const std::vector<MapArchiveMember> members{{"war3map.w3s", *bytes.value}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("soundDefinitionsTable");
    QVERIFY(table && table->rowCount() == 2);
    QVERIFY(module.Reveal({"sounds", "sound", "Thunder", {{"sourceIndex", QStringLiteral("1")}}}));
    QCOMPARE(table->currentRow(), 1);
    for (const auto& bad : {QVariant(QStringLiteral("+1")), QVariant(QStringLiteral("-1")), QVariant(QStringLiteral(" 1")), QVariant(QStringLiteral("1x")), QVariant(QStringLiteral("18446744073709551616")), QVariant(QStringLiteral("2")), QVariant(1)})
        QVERIFY(!module.Reveal({"sounds", "sound", "Thunder", {{"sourceIndex", bad}}}));
    QCOMPARE(table->currentRow(), 1);
    auto* pitch = page->findChild<QLineEdit*>("soundField_pitch"); QVERIFY(pitch);
    pitch->setText("1.2");
    QVERIFY(module.HasPendingDraft());
    QVERIFY(!module.Reveal({"sounds", "sound", "", {{"sourceIndex", QStringLiteral("0")}}}));
    QCOMPARE(table->currentRow(), 1);
}

void SoundModuleTests::EditsRealSoundBytesWithSharedUndo() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    QVERIFY(page != nullptr);
    auto* list = page->findChild<QTableWidget*>("soundDefinitionsTable");
    QVERIFY(list != nullptr);
    QCOMPARE(list->rowCount(), 1);
    QCOMPARE(list->item(0, 0)->text(), QStringLiteral("Thunder"));
    QCOMPARE(module.UndoStack(), project.mapDataUndoStack_);

    QVERIFY(module.EditField(0, "volume", "35").Succeeded());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto pending = project.mapEdits_->Replacements();
    QCOMPARE(pending.size(), std::size_t{1});
    QCOMPARE(pending.front().name, std::string("war3map.w3s"));
    const auto parsed = MapData::ParseSounds(pending.front().data);
    QVERIFY(parsed.Succeeded() && parsed.value);
    QCOMPARE(parsed.value->entries.front().volume, 35);
    QCOMPARE(parsed.value->entries.front().flags, 0x80u);
    QCOMPARE(parsed.value->entries.front().filePath, std::string("Sounds\\hit.wav"));

    const int count = project.mapDataUndoStack_->count();
    QVERIFY(!module.EditField(0, "pitch", "nan").Succeeded());
    QVERIFY(!module.EditField(0, "volume", "2147483648").Succeeded());
    QVERIFY(!module.EditField(0, "volume", "128").Succeeded());
    QVERIFY(!module.EditField(10, "volume", "20").Succeeded());
    QCOMPARE(project.mapDataUndoStack_->count(), count);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, 35);

    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, 90);
    QVERIFY(!project.mapEdits_->IsDirty());
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, 35);
    QVERIFY(project.mapEdits_->IsDirty());
}

void SoundModuleTests::InspectorPreservesOpaqueFlagsAndRejectsInvalidDraft() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* threeD = page->findChild<QCheckBox*>("soundFlag1");
    auto* spatialToggle = page->findChild<QToolButton*>("soundSpatialToggle");
    auto* spatial = page->findChild<QGroupBox*>("soundSpatialFields");
    auto* distance = page->findChild<QLineEdit*>("soundField_minDistance");
    auto* pitch = page->findChild<QLineEdit*>("soundField_pitch");
    auto* apply = page->findChild<QPushButton*>("soundApplyDefinition");
    QVERIFY(threeD && spatialToggle && spatial && distance && pitch && apply);
    QVERIFY(!spatial->isEnabled());
    spatialToggle->click();
    QVERIFY(spatial->isHidden());
    spatialToggle->click();
    QVERIFY(!spatial->isHidden());
    QVERIFY(!threeD->isChecked());
    QVERIFY(!module.HasPendingDraft());
    QVERIFY(!project.mapEdits_->IsDirty());
    threeD->setChecked(true);
    QVERIFY(spatial->isEnabled());
    distance->setText(QStringLiteral("275.5"));
    pitch->setText(QStringLiteral("bad"));
    QVERIFY(module.HasPendingDraft());
    QVERIFY(!module.DuplicateDefinition(0).Succeeded());
    apply->click();
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().flags, 0x80u);
    pitch->setText(QStringLiteral("1.25"));
    QVERIFY(module.ApplyPendingDraft());
    QVERIFY(!module.HasPendingDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto snapshot = project.mapEdits_->Snapshot();
    const auto& edited = snapshot.Sounds()->entries.front();
    QCOMPARE(edited.flags, 0x82u);
    QCOMPARE(edited.minDistance, 275.5f);
    QCOMPARE(edited.pitch, 1.25f);
    QCOMPARE(edited.eaxSetting, std::string("DefaultEAXON"));
}

void SoundModuleTests::NewSoundInMissingMemberUndoRestoresAbsence() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, false));
    Modules::SoundsModule module(project);
    QVERIFY(module.AddDefinition("NewSound", "Sounds\\new.wav").Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.size(), std::size_t{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    project.mapDataUndoStack_->undo();
    QVERIFY(!project.mapEdits_->Snapshot().Sounds());
    QVERIFY(!project.mapEdits_->IsDirty());
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().name, std::string("NewSound"));

    QVERIFY(module.DuplicateDefinition(0).Succeeded());
    const auto sounds = project.mapEdits_->Snapshot().Sounds();
    QCOMPARE(sounds->entries.size(), std::size_t{2});
    QCOMPARE(sounds->entries[0].filePath, sounds->entries[1].filePath);
    QVERIFY(sounds->entries[0].name != sounds->entries[1].name);
}

void SoundModuleTests::ResourceBrowserUsesRealPendingImport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("short.wav");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // 测试资源浏览的真实磁盘来源；此处不启动音频设备。
    QCOMPARE(file.write("RIFF\x04\0\0\0WAVE", 12), qint64{12});
    file.close();

    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    QCOMPARE(project.AddImportFiles({path}), 1);
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* files = page->findChild<QTableWidget*>("soundResourceTable");
    QVERIFY(files != nullptr);
    bool found = false;
    for (int row = 0; row < files->rowCount(); ++row) {
        if (files->item(row, 0)->text() == QStringLiteral("Sounds\\short.wav")) {
            QCOMPARE(files->item(row, 1)->text(), QStringLiteral("待导入"));
            files->selectRow(row);
            found = true;
        }
    }
    QVERIFY(found);
    auto* assign = page->findChild<QPushButton*>("soundAssignResource");
    QVERIFY(assign != nullptr);
    assign->click();
    QVERIFY(module.HasPendingDraft());
    QVERIFY(module.ApplyPendingDraft());
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().filePath,
        std::string("Sounds\\short.wav"));
}

void SoundModuleTests::GameCatalogLoadsOffThreadWithExactSourceAndSearch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto root = std::filesystem::path(dir.path().toStdWString());
    const std::vector<Mpq::MpqBuildFile> base{
        {"Sounds\\hit.wav", Bytes("base"), false},
        {"Sounds\\base.wav", Bytes("base only"), false}};
    const std::vector<Mpq::MpqBuildFile> patch{
        {"sounds/hit.WAV", Bytes("patch"), false},
        {"Sounds\\patch.wav", Bytes("patch only"), false},
        {"UI\\SoundInfo\\AbilitySounds.slk", Bytes(
            "ID;P\r\nC;X1;Y1;K\"SoundName\"\r\n"
            "C;X2;K\"FileNames\"\r\nC;X3;K\"DirectoryBase\"\r\n"
            "C;X1;Y2;K\"Missing\"\r\nC;X2;K\"NotInMpq.wav\"\r\n"
            "C;X3;K\"Sounds\"\r\nE\r\n"), false}};
    QVERIFY(Mpq::WriteMpqArchive(root / "war3.mpq", base).Succeeded());
    QVERIFY(Mpq::WriteMpqArchive(root / "war3patch.mpq", patch).Succeeded());
    const QString overrideFile = dir.filePath("base.wav");
    QFile file(overrideFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("override", 8), qint64{8});
    file.close();

    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    project.war3GameRoot_ = root;
    QCOMPARE(project.AddImportFiles({overrideFile}, QStringLiteral("Sounds")), 1);
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* files = page->findChild<QTableWidget*>("soundResourceTable");
    auto* search = page->findChild<QLineEdit*>("soundResourceSearch");
    QVERIFY(files && search);
    const auto sourceFor = [&files](const QString& path) {
        for (int row = 0; row < files->rowCount(); ++row)
            if (files->item(row, 0)->text().compare(path, Qt::CaseInsensitive) == 0)
                return files->item(row, 1)->text();
        return QString{};
    };
    QTRY_VERIFY_WITH_TIMEOUT(sourceFor(QStringLiteral("Sounds\\patch.wav")) ==
        QStringLiteral("游戏 · war3patch.mpq"), 10000);
    QCOMPARE(sourceFor(QStringLiteral("Sounds\\hit.wav")), QStringLiteral("游戏 · war3patch.mpq"));
    QCOMPARE(sourceFor(QStringLiteral("Sounds\\base.wav")), QStringLiteral("待导入"));
    search->setText(QStringLiteral("PatchOnlyDoesNotExist"));
    QCOMPARE(files->rowCount(), 0);
    search->setText(QStringLiteral("patch.wav"));
    QCOMPARE(files->rowCount(), 1);
    QCOMPARE(files->item(0, 1)->text(), QStringLiteral("游戏 · war3patch.mpq"));
    search->setText(QStringLiteral("war3patch"));
    QCOMPARE(files->rowCount(), 2); // 来源名同样可搜索。
    search->setText(QStringLiteral("NotInMpq"));
    QCOMPARE(files->rowCount(), 0); // SoundInfo 元数据不能冒充实际 MPQ 文件。
}

void SoundModuleTests::DisplaysDefaultsAndPreservesTheirBitsOnUnrelatedEdit() {
    auto sound = OriginalSound();
    sound.volume = -1;
    sound.priority = -1;
    sound.channel = -1;
    sound.pitch = std::bit_cast<float>(0x4f800000u);
    sound.minDistance = std::bit_cast<float>(0x4f800000u);
    sound.coneAngleInside = std::bit_cast<float>(0x4f800000u);
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true, sound));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    const auto text = [page](const char* name) {
        auto* edit = page->findChild<QLineEdit*>(QStringLiteral("soundField_%1").arg(QString::fromLatin1(name)));
        return edit ? edit->text() : QString{};
    };
    QCOMPARE(text("volume"), QStringLiteral("默认"));
    QCOMPARE(text("pitch"), QStringLiteral("默认"));
    QCOMPARE(text("priority"), QStringLiteral("默认"));
    QCOMPARE(text("channel"), QStringLiteral("默认"));
    QCOMPARE(text("minDistance"), QStringLiteral("默认"));
    QCOMPARE(text("coneAngleInside"), QStringLiteral("默认"));
    auto* table = page->findChild<QTableWidget*>("soundDefinitionsTable");
    QVERIFY(table);
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("默认"));

    auto* fadeIn = page->findChild<QLineEdit*>("soundField_fadeInRate");
    QVERIFY(fadeIn);
    fadeIn->setText(QStringLiteral("25"));
    QVERIFY(module.ApplyPendingDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto current = project.mapEdits_->Snapshot().Sounds();
    QVERIFY(current);
    const auto& edited = current->entries.front();
    QCOMPARE(edited.fadeInRate, 25);
    QCOMPARE(edited.volume, -1);
    QCOMPARE(edited.priority, -1);
    QCOMPARE(edited.channel, -1);
    QCOMPARE(std::bit_cast<std::uint32_t>(edited.pitch), 0x4f800000u);
    QCOMPARE(std::bit_cast<std::uint32_t>(edited.minDistance), 0x4f800000u);
    QCOMPARE(std::bit_cast<std::uint32_t>(edited.coneAngleInside), 0x4f800000u);
    const auto bytes = current->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    const auto reparsed = MapData::ParseSounds(*bytes.value);
    QVERIFY(reparsed.Succeeded() && reparsed.value);
    QCOMPARE(std::bit_cast<std::uint32_t>(reparsed.value->entries.front().pitch), 0x4f800000u);
}

void SoundModuleTests::ExplicitDefaultButtonsCanBeUndoneAndNoOpDraftClears() {
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    auto* volumeDefault = page->findChild<QToolButton*>("soundDefault_volume");
    auto* pitchDefault = page->findChild<QToolButton*>("soundDefault_pitch");
    auto* volume = page->findChild<QLineEdit*>("soundField_volume");
    auto* pitch = page->findChild<QLineEdit*>("soundField_pitch");
    QVERIFY(volumeDefault && pitchDefault && volume && pitch);
    const auto initial = project.mapEdits_->Snapshot().Sounds()->Serialize();
    QVERIFY(initial.Succeeded() && initial.value);
    volumeDefault->click();
    pitchDefault->click();
    QCOMPARE(volume->text(), QStringLiteral("默认"));
    QCOMPARE(pitch->text(), QStringLiteral("默认"));
    QVERIFY(module.HasPendingDraft());
    module.DiscardPendingDraft();
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(!project.mapEdits_->IsDirty());
    const auto unchanged = project.mapEdits_->Snapshot().Sounds()->Serialize();
    QVERIFY(unchanged.Succeeded() && unchanged.value);
    QVERIFY(*unchanged.value == *initial.value);
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, 90);
    QCOMPARE(volume->text(), QStringLiteral("90"));

    volumeDefault->click();
    pitchDefault->click();
    QVERIFY(module.ApplyPendingDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    auto current = project.mapEdits_->Snapshot().Sounds();
    QVERIFY(current);
    QCOMPARE(current->entries.front().volume, -1);
    QCOMPARE(std::bit_cast<std::uint32_t>(current->entries.front().pitch), 0x4f800000u);
    QCOMPARE(volume->text(), QStringLiteral("默认"));
    QCOMPARE(pitch->text(), QStringLiteral("默认"));
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, 90);
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries.front().volume, -1);

    volume->setText(QStringLiteral("50"));
    volume->setText(QStringLiteral("默认"));
    QVERIFY(module.HasPendingDraft());
    QVERIFY(module.ApplyPendingDraft());
    QVERIFY(!module.HasPendingDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
}

void SoundModuleTests::UnbrokenPathsWrapAtActualColumnWidthWithoutChangingData() {
    const auto fonts = UiKit::LoadFonts(UiKit::DefaultFontDirectory());
    if (fonts.uiFamilies.isEmpty()) QSKIP("A deployed UI font is required for text-paint verification");
    auto sound = OriginalSound();
    sound.name = "gg_snd_AbominationWeaponAttackVeryLongDefinitionName";
    sound.filePath = "Abilities\\Spells\\Demon\\AbominationWeaponAttackVeryLongUnbrokenResourceName.wav";
    QtProject::EditorProject project;
    QVERIFY(SetDocument(project, true, sound));
    Modules::SoundsModule module(project);
    auto* page = module.Widget();
    page->setFont(UiKit::UiFont(fonts, 14));
    auto* resources = page->findChild<QTableWidget*>("soundResourceTable");
    auto* definitions = page->findChild<QTableWidget*>("soundDefinitionsTable");
    QVERIFY(resources && definitions);
    page->resize(1080, 680);
    page->show();
    QTest::qWait(10);
    QCOMPARE(page->width(), 1080);
    QVERIFY(definitions->columnWidth(0) <= 200);
    QVERIFY(definitions->columnWidth(1) >= 140);
    QVERIFY(definitions->columnWidth(2) <= 80);
    QVERIFY(definitions->columnWidth(3) <= 90);
    QCOMPARE(definitions->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(resources->rowCount(), 1);
    resources->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    resources->setColumnWidth(0, 148);
    auto* delegate = resources->itemDelegateForColumn(0);
    QVERIFY(delegate);
    QStyleOptionViewItem option;
    option.initFrom(resources);
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    option.palette.setColor(QPalette::Text, Qt::black);
    option.palette.setColor(QPalette::Base, Qt::white);
    const auto index = resources->model()->index(0, 0);
    const auto narrow = delegate->sizeHint(option, index);
    QVERIFY(narrow.height() > resources->fontMetrics().height() * 2);
    QTRY_VERIFY(resources->rowHeight(0) >= narrow.height());

    // 检查多行区域确实绘制了文字，而非只给单行文本增加空白高度。
    QImage image(148, narrow.height(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    option.rect = image.rect();
    delegate->paint(&painter, option, index);
    painter.end();
    bool lowerText = false;
    for (int y = image.height() / 2; y < image.height() - 4 && !lowerText; ++y)
        for (int x = 8; x < image.width() - 8; ++x)
            if (qGray(image.pixel(x, y)) < 128) { lowerText = true; break; }
    QVERIFY(lowerText);
    resources->setColumnWidth(0, 520);
    const auto wide = delegate->sizeHint(option, index);
    QVERIFY(wide.height() < narrow.height());
    QTRY_VERIFY(resources->rowHeight(0) < narrow.height());
    QCOMPARE(resources->item(0, 0)->text(), QString::fromStdString(sound.filePath));
    QCOMPARE(definitions->item(0, 0)->text(), QString::fromStdString(sound.name));
    QCOMPARE(project.mapEdits_->Snapshot().Sounds()->entries[0].filePath, sound.filePath);
    QVERIFY(!project.mapEdits_->IsDirty());
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    SoundModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "SoundModuleTests.moc"
