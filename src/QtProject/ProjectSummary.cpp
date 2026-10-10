#include "EditorProject.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QStandardItemModel>

#include <map>

// 载入状态摘要：无界面探针与验收记录共用，只读取已载入的模型，不触发任何重读。
namespace War3Map::QtProject {
namespace {

const char* StateName(MapDataState state) {
    switch (state) {
    case MapDataState::Missing: return "missing";
    case MapDataState::Ready: return "ready";
    case MapDataState::Partial: return "partial";
    case MapDataState::Invalid: return "invalid";
    case MapDataState::Ambiguous: return "ambiguous";
    case MapDataState::Unreadable: return "unreadable";
    }
    return "unknown";
}

} // namespace

QStringList EditorProject::DescribeLoadedState() const {
    QStringList lines;
    lines << QStringLiteral("source=%1").arg(QString::fromStdWString(
        (sourceArchivePath_.empty() ? projectRoot_ : sourceArchivePath_).wstring()));
    if (archiveSession_) {
        lines << QStringLiteral("archiveSession=open named=%1 metadata=%2 opaque=%3")
            .arg(archiveSession_->Members().size())
            .arg(archiveSession_->MetadataMembers().size())
            .arg(archiveSession_->OpaqueMemberCount());
    } else {
        lines << QStringLiteral("archiveSession=none");
    }
    if (triggerDocument_ && uiCatalog_) {
        lines << QStringLiteral("triggers=%1 ecas=%2 arguments=%3 variables=%4 uiSources=%5")
            .arg(triggerDocument_->triggers.size())
            .arg(triggerDocument_->ecaNodes.size())
            .arg(triggerDocument_->argumentNodes.size())
            .arg(triggerDocument_->variables.size())
            .arg(uiCatalog_->ParsedSourceCount());
    } else {
        lines << QStringLiteral("triggers=unloaded reason=%1").arg(triggerStatus_);
    }
    for (std::size_t index = 0; index < kObjectTypeCount; ++index) {
        const auto type = static_cast<ObjectData::ObjectType>(index);
        const auto fileName = ObjectData::NativeFileName(type);
        const QString name = QString::fromUtf8(fileName.data(), static_cast<qsizetype>(fileName.size()));
        const auto nativeCount = nativeObjectDocuments_[index]
            ? QString::number(nativeObjectDocuments_[index]->Objects().size()) : QStringLiteral("-");
        const auto lniCount = objectDocuments_[index]
            ? QString::number(objectDocuments_[index]->Objects().size()) : QStringLiteral("-");
        const auto standardCount = standardCatalog_
            ? QString::number(standardCatalog_->Count(type)) : QStringLiteral("-");
        lines << QStringLiteral("objects %1 native=%2 editor=%3 standard=%4")
            .arg(name, nativeCount, lniCount, standardCount);
    }
    if (mapDocument_) {
        for (const auto& member : mapDocument_->Members()) {
            lines << QStringLiteral("mapData %1 state=%2 count=%3")
                .arg(QString::fromStdString(member.memberName), QString::fromUtf8(StateName(member.state)),
                    member.recordCount ? QString::number(*member.recordCount) : QStringLiteral("-"));
        }
    } else {
        lines << QStringLiteral("mapData=unloaded");
    }
    const auto& references = References();
    lines << QStringLiteral("references usages=%1 functions=%2 buildMs=%3")
        .arg(references.Usages().size()).arg(references.CalledFunctions().size()).arg(referenceBuildMs_);
    lines << QStringLiteral("pendingImports=%1").arg(pendingImports_.size());
    lines << QStringLiteral("assetEdits=%1").arg(assetEdits_.size());
    if (diagnosticsModel_ != nullptr) {
        std::map<QString, int> byLevel;
        for (int row = 0; row < diagnosticsModel_->rowCount(); ++row) {
            const QString level = diagnosticsModel_->item(row, 0)->text();
            ++byLevel[level];
            if (level != tr("信息")) {
                lines << QStringLiteral("diagnostic %1 %2 %3")
                    .arg(level, diagnosticsModel_->item(row, 2)->text(), diagnosticsModel_->item(row, 3)->text());
            }
        }
        for (const auto& [level, count] : byLevel) lines << QStringLiteral("diagnostics %1=%2").arg(level).arg(count);
    }
    return lines;
}

} // namespace War3Map::QtProject
