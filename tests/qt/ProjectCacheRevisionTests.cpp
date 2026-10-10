#include "EditorProject.h"
#include "ObjectNames.h"
#include "Commands/ObjectSnapshotCommand.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QApplication>
#include <QTest>
#include <QTemporaryDir>
#include <QUndoStack>

#include <memory>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

using namespace War3Map;

namespace {

std::unique_ptr<ObjectData::LniObjectDocument> ParseUnits(std::string_view text) {
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(
        std::as_bytes(std::span(text.data(), text.size())));
    if (!parsed.Succeeded()) return {};
    auto* document = dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get());
    if (!document) return {};
    parsed.document.release();
    return std::unique_ptr<ObjectData::LniObjectDocument>(document);
}

std::unique_ptr<ObjectData::LniObjectDocument> Units(std::string_view name, std::string_view parent) {
    const auto text = "[hfoo]\nunam=\"原创步兵\"\n"
        "[hkni]\nunam=\"原创骑士\"\n"
        "[H001]\n_parent=\"" + std::string(parent) + "\"\nunam=\"" + std::string(name) + "\"\n";
    return ParseUnits(text);
}

// 不创建任何模块、宿主或编辑跟踪器。缓存不能依赖中途界面刷新碰巧查询旧栈位置。
struct Fixture {
    QtProject::EditorProject project;
    ObjectData::LniObjectDocument* document = nullptr;
    QUndoStack* stack = nullptr;

    bool Initialize() {
        project.objectDocuments_[0] = Units("基线名字", "hfoo");
        if (!project.objectDocuments_[0]) return false;
        document = project.objectDocuments_[0].get();
        stack = new QUndoStack(&project);
        project.objectUndoStacks_[0] = stack;
        // 其他类别明确是完整空输入，不把未载入来源当作不存在。
        for (std::size_t index = 1; index < project.objectDocuments_.size(); ++index) {
            auto parsed = ObjectData::LniObjectCodec(static_cast<ObjectData::ObjectType>(index)).Parse({});
            if (!parsed.Succeeded()) return false;
            auto* empty = dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get());
            if (!empty) return false;
            parsed.document.release();
            project.objectDocuments_[index].reset(empty);
        }
        return true;
    }

    bool Push(std::string_view name, std::string_view parent) {
        auto next = Units(name, parent);
        if (!next) return false;
        stack->push(new QtProject::ObjectSnapshotCommand(*document, std::move(*next),
            [current = document](const ObjectData::LniObjectDocument& value) { *current = value; },
            QStringLiteral("修改原创单位")));
        return true;
    }
};

} // namespace

class ProjectCacheRevisionTests final : public QObject {
    Q_OBJECT
private slots:
    void NameCacheRejectsReusedUndoPositionWithoutIntermediateQuery();
    void ReferenceCacheRejectsReusedUndoPositionWithoutIntermediateQuery();
    void NameCacheRechecksActualCatalogAfterSameAddressReplacement();
};

void ProjectCacheRevisionTests::NameCacheRejectsReusedUndoPositionWithoutIntermediateQuery() {
    Fixture fixture;
    QVERIFY(fixture.Initialize());
    const auto originalFingerprint = fixture.document->DocumentFingerprint();
    QVERIFY(fixture.Push("分支甲名字", "hfoo"));
    QCOMPARE(fixture.stack->index(), 1);
    QCOMPARE(fixture.stack->count(), 1);
    const auto first = ResolveProjectName(fixture.project, "H001", "unitcode");
    QCOMPARE(QString::fromUtf8(first.displayName), QStringLiteral("分支甲名字"));

    // 刻意不在undo与新分支push之间读取名称、epoch或引用缓存。
    fixture.stack->undo();
    QCOMPARE(fixture.document->DocumentFingerprint(), originalFingerprint);
    QVERIFY(fixture.Push("分支乙名字", "hkni"));
    QCOMPARE(fixture.project.objectDocuments_[0].get(), fixture.document);
    QCOMPARE(fixture.stack->index(), 1);
    QCOMPARE(fixture.stack->count(), 1);
    const auto second = ResolveProjectName(fixture.project, "H001", "unitcode");
    QCOMPARE(QString::fromUtf8(second.displayName), QStringLiteral("分支乙名字"));
    QCOMPARE(QString::fromUtf8(first.displayName), QStringLiteral("分支甲名字"));

    fixture.stack->undo();
    QCOMPARE(fixture.document->DocumentFingerprint(), originalFingerprint);
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "unitcode").displayName),
        QStringLiteral("基线名字"));
    fixture.stack->redo();
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "unitcode").displayName),
        QStringLiteral("分支乙名字"));
}

void ProjectCacheRevisionTests::ReferenceCacheRejectsReusedUndoPositionWithoutIntermediateQuery() {
    Fixture fixture;
    QVERIFY(fixture.Initialize());
    const auto originalFingerprint = fixture.document->DocumentFingerprint();
    const auto owner = Services::ObjectOwnerId({ObjectData::ObjectType::Unit, "H001"});
    QVERIFY(fixture.Push("分支甲名字", "hfoo"));
    const auto first = fixture.project.References().Outgoing(owner);
    QVERIFY(first.contains("hfoo"));
    QVERIFY(!first.contains("hkni"));
    const auto firstGeneration = fixture.project.ReferenceGeneration();

    // 旧ReferenceStamp的index/count最终仍为1/1；中间不能由模块查询刷新掩盖此路径。
    fixture.stack->undo();
    QCOMPARE(fixture.document->DocumentFingerprint(), originalFingerprint);
    QVERIFY(fixture.Push("分支乙名字", "hkni"));
    QCOMPARE(fixture.project.objectDocuments_[0].get(), fixture.document);
    QCOMPARE(fixture.stack->index(), 1);
    QCOMPARE(fixture.stack->count(), 1);
    const auto second = fixture.project.References().Outgoing(owner);
    QVERIFY(!second.contains("hfoo"));
    QVERIFY(second.contains("hkni"));
    QVERIFY(fixture.project.ReferenceGeneration() > firstGeneration);
    // 查询结果自己拥有副本；后续分支不能把此前观察结果改成新状态。
    QVERIFY(first.contains("hfoo"));
    QVERIFY(!first.contains("hkni"));

    fixture.stack->undo();
    QCOMPARE(fixture.document->DocumentFingerprint(), originalFingerprint);
    const auto undone = fixture.project.References().Outgoing(owner);
    QVERIFY(undone.contains("hfoo"));
    QVERIFY(!undone.contains("hkni"));
    fixture.stack->redo();
    const auto redone = fixture.project.References().Outgoing(owner);
    QVERIFY(!redone.contains("hfoo"));
    QVERIFY(redone.contains("hkni"));
}

void ProjectCacheRevisionTests::NameCacheRechecksActualCatalogAfterSameAddressReplacement() {
    Fixture fixture;
    QVERIFY(fixture.Initialize());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = std::filesystem::path(directory.path().toStdWString());
    std::filesystem::create_directories(root / "ui/base");
    {
        std::ofstream config(root / "ui/config", std::ios::binary);
        config << "base\n";
        QVERIFY(config.good());
    }
    const auto loadAlias = [&](std::string_view baseType) {
        std::ofstream source(root / "ui/base/define.txt", std::ios::binary | std::ios::trunc);
        source << "[TriggerTypes]\ncustomcode=0,1,1,Custom," << baseType << '\n';
        source.close();
        return Trigger::UiCatalog::Load(root);
    };
    auto catalog = loadAlias("unitcode");
    QVERIFY(catalog.Succeeded());
    fixture.project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    const auto* owner = fixture.project.uiCatalog_.get();
    const auto firstSha = owner->Sha256();
    QVERIFY(!firstSha.empty());
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("基线名字"));

    // 同一目录类型下实际物编 Undo 仍必须使名称结果失效。
    QVERIFY(fixture.Push("修改后名字", "hfoo"));
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("修改后名字"));

    auto changed = loadAlias("string");
    QVERIFY(changed.Succeeded());
    QVERIFY(changed.value->Sha256() != firstSha);
    *fixture.project.uiCatalog_ = std::move(*changed.value);
    QCOMPARE(fixture.project.uiCatalog_.get(), owner);
    emit fixture.project.UiLibrariesChanged({QStringLiteral("base")}, false);
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("H001"));

    auto restored = loadAlias("unitcode");
    QVERIFY(restored.Succeeded());
    QCOMPARE(restored.value->Sha256(), firstSha);
    *fixture.project.uiCatalog_ = std::move(*restored.value);
    emit fixture.project.UiLibrariesChanged({QStringLiteral("base")}, false);
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("修改后名字"));
    fixture.stack->undo();
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("基线名字"));

    fixture.project.uiCatalog_.reset();
    emit fixture.project.UiLibrariesChanged({QStringLiteral("base")}, false);
    QCOMPARE(QString::fromUtf8(ResolveProjectName(fixture.project, "H001", "customcode").displayName),
        QStringLiteral("H001"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    ProjectCacheRevisionTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ProjectCacheRevisionTests.moc"
