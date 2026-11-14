// 真实大地图上触发器改名/删除各步耗时。只读打开显式给出的地图副本，只改内存文档，不保存。
// 环境变量：AUD_TIMING_MAP（地图副本）、AUD_TIMING_YDWE（YDWE/函数库根）、AUD_TIMING_GAME（游戏根）。
#include "EditorProject.h"
#include "ProjectRemovalCodeAudit.h"
#include "TriggerCommands.h"
#include "TriggerModels.h"
#include "TriggerModule.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodeReferences.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "TriggerCodeAuditBlocks.h"
#include "TriggerRenameEvidence.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Codes(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    return text;
}
template <class F> auto Time(const char* label, F&& action) {
    QElapsedTimer timer; timer.start();
    auto result = action();
    std::cout << "[timing] " << label << " ms=" << timer.elapsed() << std::endl;
    return result;
}
const Trigger::TriggerRecord* Named(const QtProject::EditorProject& project, const std::string& name) {
    const auto& triggers = project.triggerDocument_->triggers;
    const auto found = std::find_if(triggers.begin(), triggers.end(), [&](const auto& t) { return t.name.text == name; });
    return found == triggers.end() ? nullptr : &*found;
}
bool WaitFor(const std::function<bool()>& ready, int timeoutMs) {
    QElapsedTimer timer; timer.start();
    while (!ready()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto source = qEnvironmentVariable("AUD_TIMING_MAP");
    const auto ydwe = qEnvironmentVariable("AUD_TIMING_YDWE");
    const auto game = qEnvironmentVariable("AUD_TIMING_GAME");
    if (source.isEmpty() || ydwe.isEmpty() || game.isEmpty()) {
        std::cout << "AUD_TIMING_MAP / AUD_TIMING_YDWE / AUD_TIMING_GAME required.\n";
        return 77;
    }
    try {
        auto opened = MapArchiveSession::Open(source.toStdWString());
        Require(opened.Succeeded() && opened.value.has_value(), Codes(opened.diagnostics));
        auto catalog = Trigger::UiCatalog::Load(ydwe.toStdWString());
        Require(catalog.Succeeded() && catalog.value.has_value(), Codes(catalog.diagnostics));
        const std::vector<std::string> names{"war3map.wtg", "war3map.wct", "war3map.w3u", "war3map.w3a", "war3map.w3t"};
        const auto members = opened.value->CaptureExactMembers(names);
        Require(members.Succeeded() && members.value.has_value(), Codes(members.diagnostics));
        auto parsed = Time("parse", [&] { return Trigger::WtgWctCodec{}.Parse((*members.value)[0].bytes, (*members.value)[1].bytes, *catalog.value); });
        Require(parsed.Succeeded() && parsed.value.has_value(), Codes(parsed.diagnostics));
        QtProject::EditorProject project;
        project.sourceArchivePath_ = source.toStdWString();
        project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
        project.compilerDataRoot_ = project.uiDataRoot_ = ydwe.toStdWString();
        project.war3GameRoot_ = game.toStdWString();
        project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
        project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
        project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
        project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
        for (std::size_t index = 0; index < 3; ++index) {
            const auto& member = (*members.value)[index + 2];
            if (!member.present) continue;
            auto native = ObjectData::NativeObjectCodec(static_cast<ObjectData::ObjectType>(index)).Parse(member.bytes);
            Require(native.Succeeded() && native.document, Codes(native.diagnostics));
            project.nativeObjectDocuments_[index].reset(dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.release()));
        }
        project.triggerUndoStack_ = new QUndoStack(&project);
        auto& document = *project.triggerDocument_;
        const auto& ui = *project.uiCatalog_;
        std::cout << "[size] triggers=" << document.triggers.size() << " ecas=" << document.ecaNodes.size()
                  << " arguments=" << document.argumentNodes.size() << " header_bytes=" << document.wct.customCode.text.size() << std::endl;

        (void)Time("serialize", [&] { return Trigger::WtgWctCodec{}.Serialize(document, ui).Succeeded(); });
        (void)Time("binding", [&] { return Trigger::CaptureTriggerGraphBinding(document, ui, nullptr).Succeeded(); });
        (void)Time("validate", [&] { return Trigger::WtgWctCodec{}.Validate(document, ui).Succeeded(); });
        (void)Time("document-copy", [&] { Trigger::TriggerDocument copy = document; return copy.triggers.size(); });
        (void)Time("map-object-references", [&] { return project.CaptureTriggerMapObjectReferences().Succeeded(); });
        (void)Time("reference-index", [&] { return project.References().Usages().size(); });

        const auto target = std::find_if(document.triggers.begin(), document.triggers.end(),
            [](const auto& t) { return t.type == 0 && t.customTextFlag == 0 && !t.ecas.empty(); });
        Require(target != document.triggers.end(), "no GUI trigger");
        const auto targetId = target->id;
        const auto targetCategory = target->categoryId;
        const std::string originalName = target->name.text;
        const auto review = Time("rename-review", [&] {
            return Trigger::TriggerDocumentEditor::ReviewTriggerRenameCode(document, ui, targetId, "TimingRenameA"); });
        Require(review.Succeeded(), Codes(review.diagnostics));
        std::size_t mayReference = 0;
        for (const auto& row : *review.value) mayReference += row.mayReference;
        std::cout << "[rename-review] blocks=" << review.value->size() << " may_reference=" << mayReference << std::endl;
        {
            Trigger::TriggerDocument copy = document;
            const auto reviewed = Time("rename-reviewed-core", [&] {
                return Trigger::TriggerDocumentEditor::RenameTriggerReviewed(copy, ui, targetId, "TimingRenameA", {}, true); });
            std::cout << "[rename-reviewed-core] ok=" << reviewed.Succeeded() << " " << Codes(reviewed.diagnostics).substr(0, 300) << std::endl;
        }

        // 代码块清单的分项：目录查找、期望类型、字面量核对、整份清单。
        {
            std::size_t direct = 0, failed = 0, failedNonString = 0;
            (void)Time("catalog-find-all-ecas", [&] {
                std::size_t found = 0;
                for (const auto& eca : document.ecaNodes) found += ui.Find(eca.type, eca.name.text) != nullptr;
                return found; });
            const Trigger::TriggerTypeSystem types(ui);
            std::vector<std::string> expected(document.argumentNodes.size());
            (void)Time("expected-type-direct-args", [&] {
                for (const auto& eca : document.ecaNodes)
                    for (std::size_t i = 0; i < eca.arguments.size(); ++i)
                        if (eca.arguments[i] < expected.size() && document.argumentNodes[eca.arguments[i]].type == 3) {
                            expected[eca.arguments[i]] = types.ExpectedType(document, eca, i); ++direct; }
                return direct; });
            (void)Time("validate-literal-direct-args", [&] {
                for (std::size_t i = 0; i < document.argumentNodes.size(); ++i) {
                    const auto& arg = document.argumentNodes[i];
                    if (arg.type != 3) continue;
                    const auto kind = types.LiteralKind(expected[i]);
                    if (kind == Trigger::TriggerTypeSystem::Literal::Other || kind == Trigger::TriggerTypeSystem::Literal::Script) continue;
                    if (!types.ValidateLiteral(expected[i], arg.value.text).Succeeded()) {
                        ++failed; failedNonString += kind != Trigger::TriggerTypeSystem::Literal::String; }
                }
                return failed; });
            std::cout << "[literals] direct=" << direct << " failed=" << failed << " failed_non_string=" << failedNonString << std::endl;
            (void)Time("type-system-ctor", [&] { const Trigger::TriggerTypeSystem t(ui); return t.Find("integer") != nullptr; });
            std::vector<const Trigger::RawText*> blocks;
            (void)Time("visit-opaque-noop", [&] {
                blocks.clear();
                return Trigger::detail::VisitOpaqueCodeBlocks(document, ui, {},
                    [&](Trigger::AtomicRenameCodeKind, StableId, const Trigger::RawText& text) { blocks.push_back(&text); return Status::Ok(); })
                    .Succeeded(); });
            std::size_t blockBytes = 0;
            for (const auto* block : blocks) blockBytes += block->text.size();
            std::cout << "[opaque] blocks=" << blocks.size() << " bytes=" << blockBytes << std::endl;
            const std::array<std::string, 2> scripts{"TimingNoSuch", "TimingNoSuch"};
            (void)Time("rename-evidence-all-blocks", [&] {
                std::size_t hits = 0;
                for (const auto* block : blocks)
                    hits += Trigger::detail::FindTriggerRenameEvidence(block->text, "TimingNoSuch", scripts).kind
                        != Trigger::TriggerDocumentEditor::RenameEvidence::None;
                return hits; });
            const std::array targets{Trigger::detail::TriggerCodeSymbols(scripts)};
            (void)Time("symbol-uses-all-blocks", [&] {
                std::size_t hits = 0;
                for (const auto* block : blocks) hits += Trigger::detail::FindSymbolUses(block->text, targets).size();
                return hits; });
            (void)Time("opaque-inventory", [&] { return Trigger::InspectOpaqueArgumentInventory(document, ui).Succeeded(); });
            std::vector<Trigger::GraphRecordRef> refs{{Trigger::GraphRecordKind::Trigger, targetId, {}}};
            (void)Time("find-code-references", [&] { return Trigger::FindDeclarationCodeReferences(document, ui, refs).Succeeded(); });
            (void)Time("replay-identity", [&] { return Trigger::CaptureTriggerGraphReplayIdentity(document, ui).Succeeded(); });
            const auto identity = Trigger::CaptureTriggerGraphReplayIdentity(document, ui);
            (void)Time("verify-replay-identity", [&] { return Trigger::VerifyTriggerGraphReplayIdentity(document, ui, *identity.value).Succeeded(); });
        }

        Modules::TriggerModule module(project);
        (void)Time("module-widget", [&] { return module.Widget() != nullptr; });
        module.SetConfirmHandler([](const QString&, const QString&, const QStringList&) { return false; });
        const auto renamed = Time("module-rename", [&] {
            if (!module.RenameDirectoryItem({false, targetId, targetCategory}, QStringLiteral("TimingRenameA"))) return false;
            return WaitFor([&] { return Named(project, "TimingRenameA") != nullptr; }, 600000);
        });
        std::cout << "[module-rename] ok=" << renamed << std::endl;
        (void)Time("module-rename-back", [&] {
            if (!module.RenameDirectoryItem({false, targetId, targetCategory}, QString::fromStdString(originalName))) return false;
            return WaitFor([&] { return Named(project, originalName) != nullptr; }, 600000);
        });

        // 分段：核心批次（含两次绑定、复制和代码核查）与完整发布（另含撤销快照、修订观察和界面刷新）。
        {
            const auto objects = project.CaptureTriggerMapObjectReferences();
            const auto* snapshot = objects.Succeeded() && objects.value && !(*objects.value)->References().empty() ? objects.value->get() : nullptr;
            Trigger::TriggerDocument copy = document;
            const auto binding = Trigger::CaptureTriggerGraphBinding(copy, ui, snapshot);
            Require(binding.Succeeded(), Codes(binding.diagnostics));
            Trigger::TriggerGraphEditBatch batch;
            batch.expected = *binding.value;
            batch.operations.emplace_back(Trigger::GraphRename{{Trigger::GraphRecordKind::Trigger, targetId, {}},
                Named(project, originalName)->name, "TimingRenameB", {}, true});
            const auto core = Time("core-batch-rename", [&] { return Trigger::ApplyTriggerGraphEditBatch(copy, ui, batch, {}, snapshot); });
            std::cout << "[core-batch-rename] ok=" << core.Succeeded() << std::endl;
            std::vector<Trigger::GraphEditOperation> operations;
            operations.emplace_back(Trigger::GraphRename{{Trigger::GraphRecordKind::Trigger, targetId, {}},
                Named(project, originalName)->name, "TimingRenameB", {}, true});
            (void)Time("map-object-references-warm", [&] { return project.CaptureTriggerMapObjectReferences().Succeeded(); });
            (void)Time("emit-document-changed", [&] { emit project.TriggerDocumentChanged(); return 0; });
            const auto published = Time("publish-rename", [&] {
                return QtProject::PublishTriggerGraphOperations(project, std::move(operations), QStringLiteral("timing")); });
            std::cout << "[publish-rename] ok=" << published.Succeeded() << std::endl;
            (void)Time("events-after-publish", [&] { QCoreApplication::processEvents(QEventLoop::AllEvents); return 0; });
            (void)Time("undo-rename", [&] { project.triggerUndoStack_->undo(); QCoreApplication::processEvents(QEventLoop::AllEvents); return 0; });
        }

        // 新建的空触发器：删除前检查（作者报告新建后删除就报 411 处）。
        const auto added = Trigger::TriggerDocumentEditor::AddTrigger(document, targetCategory, "TimingNew");
        Require(added.Succeeded(), Codes(added.diagnostics));
        project.triggerUndoStack_->push(new QUndoCommand("timing new trigger"));
        std::vector<Trigger::GraphRemove> targets(1);
        targets[0].target = {Trigger::GraphRecordKind::Trigger, *added.value, {}};
        targets[0].expectedName = "TimingNew";
        const auto audit = Time("delete-audit-prepare", [&] { return QtProject::RemovalCodeAudit::Prepare(project, targets, {*added.value}); });
        std::atomic_bool stop = false;
        const auto notice = Time("delete-audit-run", [&] { return audit.Run({}, stop); });
        Require(notice.has_value(), "delete audit cancelled");
        std::cout << "[delete-audit] references=" << notice->references << " incomplete=" << notice->incomplete.toStdString() << std::endl;
        for (const auto& line : notice->lines) std::cout << "  " << line.toStdString() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cout << "FAILED: " << error.what() << std::endl;
        return 1;
    }
}
