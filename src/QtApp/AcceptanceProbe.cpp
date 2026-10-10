#include "MainWindow.h"

#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Script/MapScriptBuild.h"

#include <QStandardItemModel>
#include <QRegularExpression>
#include <algorithm>
#include <optional>
#include <string_view>

// 无界面验收探针：对已打开的地图做一组真实的触发器编辑，再走与界面相同的另存路径
// （含游戏脚本重建与 jasshelper / pjass 语法检查）。只写到调用方给出的输出路径。
bool MainWindow::ProbeEditAndSave(const std::filesystem::path& outputPath, QStringList& log) {
    using namespace War3Map::Trigger;
    auto* document = project_->triggerDocument_.get();
    const auto* catalog = project_->uiCatalog_.get();
    if (document == nullptr || catalog == nullptr || document->triggers.empty() || !project_->archiveSession_) {
        log << QStringLiteral("probe: 没有已载入的触发器");
        return false;
    }
    const auto check = [&log](const auto& status, const QString& step) {
        if (status.Succeeded()) return true;
        log << QStringLiteral("probe: %1 失败").arg(step);
        for (const auto& diagnostic : status.diagnostics) log << QString::fromUtf8(diagnostic.message);
        return false;
    };
    // 标记与本次候选名称绑定；只允许安全 ASCII，不能让路径文本成为脚本代码或颜色转义。
    const auto savedPath = outputPath.empty() ? project_->archiveSession_->SourcePath() : outputPath;
    std::string safeStem;
    for (const auto character : savedPath.stem().u8string()) {
        if ((character >= u8'A' && character <= u8'Z') || (character >= u8'a' && character <= u8'z')
            || (character >= u8'0' && character <= u8'9') || character == u8'_' || character == u8'-') {
            safeStem.push_back(static_cast<char>(character));
        }
    }
    // 不截断成可能重复的标记；调用方可用 cycle-01 等短文件名区分每一轮。
    if (safeStem.empty() || safeStem.size() > 80) {
        log << QStringLiteral("probe: 输出文件名清理后须含 1–80 个 ASCII 字母、数字、下划线或连字符");
        return false;
    }
    const std::string marker = "FORGE_STAGE_D_" + safeStem;
    const auto* displayDefinition = catalog->Find(UiDefinitionKind::Action, "DisplayTimedTextToForce");
    if (displayDefinition == nullptr || displayDefinition->revisions.empty()
        || displayDefinition->Active().arguments.size() != 3
        || displayDefinition->Active().arguments[0].type != "force"
        || displayDefinition->Active().arguments[1].type != "real"
        || (displayDefinition->Active().arguments[2].type != "StringExt"
            && displayDefinition->Active().arguments[2].type != "string")) {
        log << QStringLiteral("probe: 当前函数库缺少兼容的 DisplayTimedTextToForce(force, real, string) 动作");
        return false;
    }
    // 1. 新建触发器：地图初始化时向所有玩家显示本轮标记 60 秒。
    const auto categoryId = document->triggers.front().categoryId;
    // 与界面新建触发器相同：名称与脚本名都不和已有触发器冲突。
    const auto triggerName = TriggerDirectoryEditor::UniqueTriggerName(*document, "ForgeProbe_" + safeStem);
    auto added = TriggerDocumentEditor::AddTrigger(*document, categoryId, triggerName);
    if (!check(added, QStringLiteral("新建触发器")) || !added.value) return false;
    const auto newTrigger = *added.value;
    if (!check(TriggerDocumentEditor::AppendRootEca(*document, *catalog, newTrigger, UiDefinitionKind::Event, "MapInitializationEvent"),
            QStringLiteral("添加事件"))) return false;
    const auto display = TriggerDocumentEditor::AppendRootEca(
        *document, *catalog, newTrigger, UiDefinitionKind::Action, "DisplayTimedTextToForce");
    if (!check(display, QStringLiteral("添加可见标记动作")) || !display.value) return false;
    const auto* displayEca = TriggerDocumentEditor::FindEca(*document, display.value->rootEcaId);
    if (displayEca == nullptr || displayEca->arguments.size() != 3) return false;
    const auto& audience = document->argumentNodes[displayEca->arguments[0]];
    if (audience.type != 2 || audience.insertCall == 0 || !audience.callEca
        || *audience.callEca >= document->ecaNodes.size()
        || document->ecaNodes[*audience.callEca].name.text != "GetPlayersAll"
        || !document->ecaNodes[*audience.callEca].arguments.empty()) {
        log << QStringLiteral("probe: 显示动作的默认玩家组不是无参数 GetPlayersAll，已停止验收编辑");
        return false;
    }
    if (!check(TriggerDocumentEditor::SetScalarArgument(*document,
            document->argumentNodes[displayEca->arguments[1]].id, 3, "60.00"), QStringLiteral("设置显示时间"))
        || !check(TriggerDocumentEditor::SetScalarArgument(*document,
            document->argumentNodes[displayEca->arguments[2]].id, 3, marker), QStringLiteral("设置验收标记"))) return false;
    log << QStringLiteral("probe: marker=%1 duration=60.00 action=DisplayTimedTextToForce")
        .arg(QString::fromUtf8(marker));
    // 2. 修改已有触发器：在第一个非注释、已启用的 GUI 触发器末尾追加动作，并改写注释。
    for (const auto& trigger : document->triggers) {
        if (trigger.id == newTrigger || trigger.type != 0 || trigger.enabled == 0 || trigger.customTextFlag != 0) continue;
        if (!check(TriggerDocumentEditor::AppendRootEca(*document, *catalog, trigger.id, UiDefinitionKind::Action, "DoNothing"),
                QStringLiteral("修改已有触发器"))) return false;
        if (!check(TriggerDocumentEditor::SetTriggerComment(*document, trigger.id, "验收探针修改"), QStringLiteral("改写注释"))) return false;
        log << QStringLiteral("probe: 修改触发器 %1").arg(QString::fromUtf8(trigger.name.text));
        break;
    }
    project_->ObserveTriggerEdit();
    const int before = project_->diagnosticsModel_->rowCount();
    // 输出路径为空时写回打开的地图（原图移入备份目录），用于验证原地保存。
    const bool saved = outputPath.empty() ? RunMapSave({}, true, false) : SaveMapArchiveToPath(outputPath);
    for (int row = before; row < project_->diagnosticsModel_->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < project_->diagnosticsModel_->columnCount(); ++column) {
            cells << project_->diagnosticsModel_->item(row, column)->text();
        }
        log << cells.join(QStringLiteral(" | ")).left(600);
    }
    log << QStringLiteral("probe: saved=%1").arg(saved ? QStringLiteral("true") : QStringLiteral("false"));
    if (!saved) return false;

    // 从写出的归档联合解析 WTG/WCT，核对新增 GUI 动作及其空 WCT 槽；不能只检查内存中的编辑。
    const auto candidate = War3Map::MapArchiveSession::Open(savedPath);
    if (!check(candidate, QStringLiteral("重新打开候选")) || !candidate.value) return false;
    const auto wtg = candidate.value->ReadMember("war3map.wtg");
    const auto wct = candidate.value->ReadMember("war3map.wct");
    const auto script = candidate.value->ReadMember(War3Map::Script::MapScriptMemberName(*candidate.value));
    if (!check(wtg, QStringLiteral("回读 WTG")) || !wtg.value
        || !check(wct, QStringLiteral("回读 WCT")) || !wct.value
        || !check(script, QStringLiteral("回读游戏脚本")) || !script.value) return false;
    const auto reopened = WtgWctCodec{}.Parse(*wtg.value, *wct.value, *project_->uiCatalog_);
    if (!check(reopened, QStringLiteral("联合解析候选 WTG/WCT")) || !reopened.value) return false;
    const auto& roundtrip = *reopened.value;
    const auto found = std::find_if(roundtrip.triggers.begin(), roundtrip.triggers.end(),
        [&triggerName](const TriggerRecord& trigger) { return trigger.name.text == triggerName; });
    bool triggerMatches = found != roundtrip.triggers.end()
        && found->type == 0 && found->enabled != 0 && found->initiallyClosed == 0
        && found->customTextFlag == 0 && found->customText.text.empty() && found->ecas.size() == 2;
    if (triggerMatches) {
        const auto& event = roundtrip.ecaNodes[found->ecas[0]];
        const auto& action = roundtrip.ecaNodes[found->ecas[1]];
        triggerMatches = event.type == 0 && event.enabled != 0 && event.name.text == "MapInitializationEvent"
            && action.type == 2 && action.enabled != 0 && action.name.text == "DisplayTimedTextToForce"
            && action.arguments.size() == 3;
        if (triggerMatches) {
            const auto& players = roundtrip.argumentNodes[action.arguments[0]];
            const auto& duration = roundtrip.argumentNodes[action.arguments[1]];
            const auto& text = roundtrip.argumentNodes[action.arguments[2]];
            triggerMatches = players.type == 2 && players.insertCall != 0 && players.callEca
                && *players.callEca < roundtrip.ecaNodes.size()
                && roundtrip.ecaNodes[*players.callEca].name.text == "GetPlayersAll"
                && roundtrip.ecaNodes[*players.callEca].arguments.empty()
                && duration.type == 3 && duration.insertCall == 0 && duration.insertIndex == 0
                && duration.value.text == "60.00"
                && text.type == 3 && text.insertCall == 0 && text.insertIndex == 0 && text.value.text == marker;
        }
    }
    // 匹配独立的调用语句，允许编译器调整空白；注释或仅字符串中出现 marker 不算成功。
    const QString scriptText = QString::fromUtf8(reinterpret_cast<const char*>(script.value->data()),
        static_cast<qsizetype>(script.value->size()));
    const QRegularExpression markerCall(QStringLiteral(
        R"((?m)^[\t ]*call\s+DisplayTimedTextToForce\s*\(\s*GetPlayersAll\s*\(\s*\)\s*,\s*60(?:\.0*)?\s*,\s*"%1"\s*\))")
        .arg(QRegularExpression::escape(QString::fromUtf8(marker))));
    const QRegularExpression initCall(QStringLiteral(
        R"((?m)^[\t ]*call\s+ConditionalTriggerExecute\s*\(\s*gg_trg_%1\s*\))")
        .arg(QRegularExpression::escape(QString::fromUtf8(War3Map::Script::ConvertObjectName(triggerName)))));
    const bool scriptMatches = markerCall.match(scriptText).hasMatch() && initCall.match(scriptText).hasMatch();
    log << QStringLiteral("probe: reopened-wtg-wct=%1 script-marker-init=%2 marker=%3")
        .arg(triggerMatches ? QStringLiteral("true") : QStringLiteral("false"),
            scriptMatches ? QStringLiteral("true") : QStringLiteral("false"), QString::fromUtf8(marker));
    return triggerMatches && scriptMatches;
}

bool MainWindow::ProbeMapDataEditAndSave(const std::filesystem::path& outputPath, QStringList& log,
    bool projectOnly) {
    if (outputPath.empty() || !project_->archiveSession_ || !project_->mapDocument_
        || !project_->mapDocument_->Settings() || !project_->mapDocument_->Units() || !project_->mapEdits_) {
        log << QStringLiteral("map-data-probe: 缺少源归档、地图设置或单位放置文档");
        return false;
    }
    auto settings = *project_->mapDocument_->Settings();
    auto units = *project_->mapDocument_->Units();
    if (units.units.empty()) {
        log << QStringLiteral("map-data-probe: 该地图没有可安全改动的放置单位");
        return false;
    }
    const auto previousX = units.units.front().position[0];
    settings.mapName = "FORGE_Map_Data_Probe";
    units.units.front().position[0] += 64.0f;
    std::vector<War3Map::Status> changes;
    changes.push_back(project_->mapEdits_->EditSettings(settings));
    changes.push_back(project_->mapEdits_->EditUnits(units));
    // 区域、镜头、声音各改第一条（有则改）：区域右边界 +32，镜头距离 +100，声音音量 ±10。
    std::optional<float> regionRight, cameraDistance;
    std::optional<std::int32_t> soundVolume;
    if (const auto& regions = project_->mapDocument_->Regions(); regions && !regions->entries.empty()) {
        auto next = *regions;
        next.entries.front().right += 32.0f;
        regionRight = next.entries.front().right;
        changes.push_back(project_->mapEdits_->EditRegions(next));
    }
    if (const auto& cameras = project_->mapDocument_->Cameras(); cameras && !cameras->entries.empty()) {
        auto next = *cameras;
        next.entries.front().targetDistance += 100.0f;
        cameraDistance = next.entries.front().targetDistance;
        changes.push_back(project_->mapEdits_->EditCameras(next));
    }
    if (const auto& sounds = project_->mapDocument_->Sounds(); sounds && !sounds->entries.empty()) {
        auto next = *sounds;
        auto& volume = next.entries.front().volume;
        // -1 表示使用默认音量；改成明确的 100，避免验收图里出现几乎无声的声音。
        volume = volume < 0 ? 100 : (volume > 50 ? volume - 10 : volume + 10);
        soundVolume = volume;
        changes.push_back(project_->mapEdits_->EditSounds(next));
    }
    for (const auto& change : changes) {
        if (change.Succeeded()) continue;
        for (const auto& diagnostic : change.diagnostics) log << QString::fromUtf8(diagnostic.message);
        return false;
    }
    log << QStringLiteral("map-data-probe: region=%1 camera=%2 sound=%3").arg(
        regionRight ? QStringLiteral("edited") : QStringLiteral("none"),
        cameraDistance ? QStringLiteral("edited") : QStringLiteral("none"),
        soundVolume ? QStringLiteral("edited") : QStringLiteral("none"));
    const int before = project_->diagnosticsModel_->rowCount();
    const bool saved = projectOnly ? SaveEditorProjectToPath(outputPath) : SaveMapArchiveToPath(outputPath);
    for (int row = before; row < project_->diagnosticsModel_->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < project_->diagnosticsModel_->columnCount(); ++column)
            cells << project_->diagnosticsModel_->item(row, column)->text();
        log << cells.join(QStringLiteral(" | ")).left(600);
    }
    if (!saved) return false;
    if (projectOnly) {
        log << QStringLiteral("map-data-probe: project=true mapCandidate=false");
        return true;
    }
    const auto candidate = War3Map::MapArchiveSession::Open(outputPath);
    if (!candidate.Succeeded() || !candidate.value) {
        log << QStringLiteral("map-data-probe: 已保存候选无法重开");
        return false;
    }
    const auto member = candidate.value->ReadMember(War3Map::Script::MapScriptMemberName(*candidate.value));
    const auto candidateData = War3Map::MapDocument::FromArchive(*candidate.value);
    const bool dataMatches = candidateData.Settings() && candidateData.Settings()->mapName == "FORGE_Map_Data_Probe"
        && candidateData.Units() && !candidateData.Units()->units.empty()
        && candidateData.Units()->units.front().position[0] == previousX + 64.0f
        && (!regionRight || (candidateData.Regions() && candidateData.Regions()->entries.front().right == *regionRight))
        && (!cameraDistance || (candidateData.Cameras() && candidateData.Cameras()->entries.front().targetDistance == *cameraDistance))
        && (!soundVolume || (candidateData.Sounds() && candidateData.Sounds()->entries.front().volume == *soundVolume));
    const bool scriptMatches = member.Succeeded() && member.value
        && std::string_view(reinterpret_cast<const char*>(member.value->data()), member.value->size())
            .find("FORGE_Map_Data_Probe") != std::string_view::npos;
    log << QStringLiteral("map-data-probe: data=%1 script=%2")
        .arg(dataMatches ? QStringLiteral("true") : QStringLiteral("false"),
             scriptMatches ? QStringLiteral("true") : QStringLiteral("false"));
    return dataMatches && scriptMatches;
}
