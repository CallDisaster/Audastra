#include "EditorProject.h"
#include "TriggerCodeChecking.h"
#include "Widgets/CodeView.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUndoCommand>
#include <QUndoStack>
#include <QTimer>
#include <QTest>

#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Codes(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    return text;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto source = qEnvironmentVariable("FORGE_CODE_PROJECT_SOURCE");
    const auto ydwe = qEnvironmentVariable("FORGE_YDWE_ROOT");
    const auto game = qEnvironmentVariable("FORGE_GAME_ROOT");
    if (source.isEmpty() || ydwe.isEmpty() || game.isEmpty()) {
        std::cout << "Explicit readonly source/YDWE/game paths required; default does not read author installation.\n";
        return 77;
    }
    try {
        auto opened = MapArchiveSession::Open(source.toStdWString());
        Require(opened.Succeeded() && opened.value.has_value(), Codes(opened.diagnostics).c_str());
        const auto before = opened.value->SourceSha256();
        auto catalog = Trigger::UiCatalog::Load(ydwe.toStdWString());
        Require(catalog.Succeeded() && catalog.value.has_value(), Codes(catalog.diagnostics).c_str());
        const std::vector<std::string> names{"war3map.wtg", "war3map.wct", "war3map.w3u", "war3map.w3a", "war3map.w3t"};
        const auto members = opened.value->CaptureExactMembers(names);
        Require(members.Succeeded() && members.value.has_value(), Codes(members.diagnostics).c_str());
        auto parsed = Trigger::WtgWctCodec{}.Parse((*members.value)[0].bytes, (*members.value)[1].bytes, *catalog.value);
        Require(parsed.Succeeded() && parsed.value.has_value(), Codes(parsed.diagnostics).c_str());
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
            const auto type = static_cast<ObjectData::ObjectType>(index);
            const auto& member = (*members.value)[index + 2];
            if (member.present) {
                auto native = ObjectData::NativeObjectCodec(type).Parse(member.bytes);
                Require(native.Succeeded() && native.document, Codes(native.diagnostics).c_str());
                project.nativeObjectDocuments_[index].reset(dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.release()));
                Require(project.nativeObjectDocuments_[index] != nullptr, "Native object source type mismatch");
            }
        }
        project.triggerUndoStack_ = new QUndoStack(&project);
        QElapsedTimer timer;
        timer.start();
        auto snapshot = project.CaptureCodeCheckSnapshot();
        const auto coldNanoseconds = timer.nsecsElapsed();
        Require(snapshot && snapshot->triggers && snapshot->catalog && snapshot->map, "Actual UI snapshot incomplete");
        timer.restart();
        for (int count = 0; count < 100; ++count)
            Require(project.CaptureCodeCheckSnapshot() == snapshot, "Unchanged project recopied its snapshot");
        const auto warmNanoseconds = timer.nsecsElapsed();
        const auto originalHeader = Trigger::TriggerCodeEditor::NormalizedText(snapshot->triggers->wct.customCode);
        project.triggerDocument_->wct.customCode.text += "\n// owned test revision\n";
        project.triggerUndoStack_->push(new QUndoCommand("owned snapshot generation"));
        timer.restart();
        auto changed = project.CaptureCodeCheckSnapshot();
        const auto recaptureNanoseconds = timer.nsecsElapsed();
        Require(changed != snapshot && changed->binding != snapshot->binding
            && Trigger::TriggerCodeEditor::NormalizedText(snapshot->triggers->wct.customCode) == originalHeader,
            "Changed UI project mutated an earlier owned worker snapshot");
        Services::CodeCheckProjectCache cache;
        timer.restart();
        const auto result = Services::CheckProjectCode(snapshot, cache, {}, originalHeader, Services::CodeLanguage::VJass, 1);
        const auto checkNanoseconds = timer.nsecsElapsed();
        Require(!result.cancelled && result.generation == 1 && result.contextBinding == snapshot->binding,
            "Actual immutable project check lost its binding");
        Require(result.reason.find("PP_INCLUDE_NOT_FOUND") == std::string::npos,
            "Actual modern code check lost the same enabled UI library roots used by map save");
        if (result.contextComplete && result.declarations.completed && result.externalSourcesComplete) {
            for (const auto& declaration : result.declarations.symbols)
                Require(std::any_of(result.externalSources.begin(), result.externalSources.end(), [&](const auto& frozen) {
                    return frozen.name == declaration.source;
                }), "A complete real declaration index advertises a missing frozen navigation source");
        }
        const auto script = project.archiveSession_->ReadMember("war3map.j");
        Require(script.Succeeded() && script.value && script.value->size() >= 7 * 1024 * 1024,
            "Actual performance source is not a 7MiB or larger war3map.j");
        const auto scriptHash = Sha256(*script.value);
        Require(scriptHash.Succeeded(), "Actual script hash failed");
        const QByteArray scriptBytes(reinterpret_cast<const char*>(script.value->data()), script.value->size());
        UiKit::CodeView view;
        view.resize(1100, 720); view.SetDecoder(Modules::DecodeMapCodeForView); view.show();
        qint64 lastPulse = 0, maxUiGap = 0;
        QTimer pulse; pulse.setInterval(10);
        QObject::connect(&pulse, &QTimer::timeout, &view, [&] {
            const auto now = timer.elapsed(); maxUiGap = (std::max)(maxUiGap, now - lastPulse); lastPulse = now;
        });
        timer.restart(); pulse.start(); view.SetBytesAsync(scriptBytes);
        while ((view.IsLoading() || view.IsChecking()) && timer.elapsed() < 30000) QTest::qWait(10);
        QTest::qWait(30); pulse.stop();
        const auto viewMilliseconds = timer.elapsed();
        Require(!view.IsLoading() && !view.IsChecking() && view.isReadOnly(), "Actual readonly view failed to complete");
        const auto expected = Modules::DecodeMapCodeForView(scriptBytes);
        // QTextDocument 的行分隔固定为 LF；原脚本字节的保持由下方源 SHA 核对。
        QString expectedView = expected.text; expectedView.replace("\r\n", "\n").replace('\r', '\n');
        const auto actualView = view.toPlainText();
        if (actualView != expectedView) {
            qsizetype first = 0;
            while (first < actualView.size() && first < expectedView.size() && actualView[first] == expectedView[first]) ++first;
            std::cerr << "View mismatch actualLength=" << actualView.size() << " expectedLength=" << expectedView.size()
                << " firstOffset=" << first << " actualScalar=" << (first < actualView.size() ? actualView[first].unicode() : 0)
                << " expectedScalar=" << (first < expectedView.size() ? expectedView[first].unicode() : 0) << '\n';
        }
        Require(actualView == expectedView && view.WorkerStats().peakRunning == 1
            && view.WorkerStats().peakPending <= 1, "Actual map script decoder or bounded view scheduling changed");
        Require(project.archiveSession_->VerifySourceUnchanged().Succeeded(), "Readonly author source changed");
        const auto after = Sha256File(source.toStdWString());
        Require(after.Succeeded() && *after.value == before, "Author map SHA changed");
        QJsonObject receipt{{"source", source}, {"sourceSha256", QString::fromStdString(before)},
            {"coldSnapshotMs", coldNanoseconds / 1000000.0}, {"warm100SnapshotMs", warmNanoseconds / 1000000.0},
            {"changedSnapshotMs", recaptureNanoseconds / 1000000.0}, {"workerCheckMs", checkNanoseconds / 1000000.0},
            {"scriptBytes", scriptBytes.size()}, {"scriptSha256", QString::fromStdString(*scriptHash.value)},
            {"readonlyViewMs", viewMilliseconds}, {"maxUiEventGapMs", maxUiGap}, {"scriptBlocks", view.blockCount()},
            {"triggerCount", static_cast<qint64>(snapshot->triggers->triggers.size())},
            {"ecaCount", static_cast<qint64>(snapshot->triggers->ecaNodes.size())},
            {"completed", result.completed}, {"passed", result.Succeeded()}, {"semanticChecked", result.semanticChecked},
            {"contextComplete", result.contextComplete}, {"declarations", static_cast<qint64>(result.declarations.symbols.size())},
            {"reason", QString::fromStdString(result.reason)},
            {"sourceUnchanged", true}, {"evidence", "actual author map readonly; UI snapshot timing and common worker only; no save, compiler process or game"}};
        const auto output = qEnvironmentVariable("FORGE_CODE_PROJECT_RECEIPT");
        if (!output.isEmpty()) {
            QFile file(output); Require(file.open(QIODevice::WriteOnly), "Timing receipt open failed");
            file.write(QJsonDocument(receipt).toJson());
        }
        std::cout << QJsonDocument(receipt).toJson(QJsonDocument::Compact).constData() << '\n';
        Require(coldNanoseconds < 500000000 && recaptureNanoseconds < 500000000,
            "Actual UI snapshot freeze exceeded the 500ms acceptance budget");
        Require(maxUiGap < 500, "Actual 7MiB map script view exceeded the 500ms event-loop budget");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
