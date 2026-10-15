#pragma once

#include "EditorProject.h"
#include "ProjectObjectCatalog.h"
#include "../services/ObjectCommandsFixture.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QObject>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

namespace War3Map::Tests {
namespace SyntheticCatalogFixtureDetail {
// 子对象持有原创临时游戏来源，直到工程及其目录快照完成析构。
class GameSource final : public QObject {
public:
    explicit GameSource(QObject& owner) : QObject(&owner) {}
    QTemporaryDir directory;
};
} // namespace SyntheticCatalogFixtureDetail

// 将测试已有的原创目录按默认成员名写入临时游戏 MPQ。
// 不向地图层写表，避免把应可写的游戏基础对象变成只读地图表。
inline bool InstallSyntheticGameCatalog(QtProject::EditorProject& project,
    const ObjectData::StandardObjectCatalog& original) {
    auto source = std::make_unique<SyntheticCatalogFixtureDetail::GameSource>(project);
    if (!source->directory.isValid()) return false;
    std::vector<Mpq::MpqBuildFile> files;
    for (const auto& spec : ObjectData::StandardObjectCatalog::DefaultSources()) {
        std::vector<ObjectRow> rows;
        for (const auto& entry : original.Entries()) {
            if (entry.key.type != spec.type) continue;
            ObjectRow row(entry.fields.begin(), entry.fields.end());
            row.erase("id");
            row["ID"] = entry.key.rawcode;
            rows.push_back(std::move(row));
        }
        if (!rows.empty()) files.push_back({spec.slkFiles.front(), ObjectBytes(ObjectTable(rows)), false});
    }
    if (files.empty()) return false;
    const std::filesystem::path root(source->directory.path().toStdWString());
    if (!Mpq::WriteMpqArchive(root / "war3.mpq", files).Succeeded()) return false;
    project.war3GameRoot_ = root;
    if (!QtProject::EnsureProjectObjectCatalog(project, true).Succeeded()) return false;
    // 原夹具按游戏基础对象执行编辑。来源资格由生产快照授予，不伪造字段。
    for (const auto& entry : original.Entries()) {
        const auto* actual = project.standardCatalog_->Find(entry.key.type, entry.key.rawcode);
        if (!actual || !actual->nativeOverridesSupported) return false;
    }
    source.release(); // QObject 父子关系持有临时来源。
    return true;
}
} // namespace War3Map::Tests
