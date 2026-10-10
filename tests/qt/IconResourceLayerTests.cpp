#include "Icons/IconProvider.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QStringList>
#include <QTest>

#include <array>
#include <filesystem>

namespace {
using War3Map::QtApp::IconProvider;

std::filesystem::path Path(const QString& path) { return path.toStdWString(); }

QByteArray Png(const QColor& color) {
    QImage image(1, 1, QImage::Format_RGBA8888);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return bytes;
}

bool Save(const QString& root, const QString& name, const QColor& color) {
    const auto path = QDir(root).filePath(name);
    if (!QDir().mkpath(QFileInfo(path).path())) return false;
    QFile file(path);
    const auto bytes = Png(color);
    return !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool Archive(const QString& path, const QColor& color, const std::string& name = "Textures/Shared.png") {
    if (!QDir().mkpath(QFileInfo(path).path())) return false;
    const auto png = Png(color);
    if (png.isEmpty()) return false;
    std::vector<std::byte> data;
    for (const unsigned char value : png) data.push_back(static_cast<std::byte>(value));
    const std::array files{War3Map::Mpq::MpqBuildFile{name, std::move(data), true}};
    return War3Map::Mpq::WriteMpqArchive(Path(path), files).Succeeded();
}

QColor Pixel(const QIcon& icon) {
    if (icon.isNull()) return {};
    const auto image = icon.pixmap(1, 1).toImage();
    return image.isNull() ? QColor{} : image.pixelColor(0, 0);
}

QColor Pixel(IconProvider& provider, const QString& name = QStringLiteral("Textures/Shared.png")) {
    return Pixel(provider.IconForTexture(name));
}

bool WriteText(const QString& path, const QByteArray& text) {
    if (!QDir().mkpath(QFileInfo(path).path())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
}
} // namespace

class IconResourceLayerTests final : public QObject {
    Q_OBJECT
private slots:
    void ArchiveWorkspaceGameExtraPriority();
    void MapSwitchAndFailedMountInvalidateCache();
    void ResourceRootRefreshAndClear();
    void ExtraRootAndGameRootSwitch();
    void NormalizedRelativePathsShareCache();
    void RejectPathsOutsideResourceRoot();
    void UiLibraryCategoryIconsResolveToolAndLayerPaths();
};

void IconResourceLayerTests::ArchiveWorkspaceGameExtraPriority() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.path();
    const auto map = root + "/map.mpq";
    const auto workspace = root + "/workspace/resource";
    const auto game = root + "/game";
    const auto extra = root + "/extra";
    QVERIFY(Archive(map, QColor(Qt::red)));
    QVERIFY(Save(workspace, "Textures/Shared.png", QColor(Qt::green)));
    QVERIFY(Archive(game + "/war3.mpq", QColor(Qt::blue)));
    QVERIFY(Save(extra, "Textures/Shared.png", QColor(Qt::yellow)));
    IconProvider provider;
    provider.SetExtraIconDir(Path(extra));
    QCOMPARE(Pixel(provider), QColor(Qt::yellow));
    QVERIFY(provider.SetGameRoot(Path(game)));
    QCOMPARE(Pixel(provider), QColor(Qt::blue));
    provider.SetMapResourceDirectory(Path(workspace));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
    QVERIFY(provider.SetMapArchive(Path(map)));
    QCOMPARE(Pixel(provider), QColor(Qt::red));
    QVERIFY(provider.SetMapArchive({}));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
    provider.SetMapResourceDirectory({});
    QCOMPARE(Pixel(provider), QColor(Qt::blue));
}

void IconResourceLayerTests::MapSwitchAndFailedMountInvalidateCache() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto first = temporary.filePath("first.mpq");
    const auto second = temporary.filePath("second.mpq");
    const auto resource = temporary.filePath("resource");
    QVERIFY(Archive(first, QColor(Qt::red)));
    QVERIFY(Archive(second, QColor(Qt::magenta)));
    QVERIFY(Save(resource, "Textures/Shared.png", QColor(Qt::green)));
    IconProvider provider;
    QVERIFY(provider.IconForTexture("Textures/Shared.png").isNull());
    provider.SetMapResourceDirectory(Path(resource));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
    QVERIFY(provider.SetMapArchive(Path(first)));
    QCOMPARE(Pixel(provider), QColor(Qt::red));
    QVERIFY(provider.SetMapArchive(Path(second)));
    QCOMPARE(Pixel(provider), QColor(Qt::magenta));
    QVERIFY(!provider.SetMapArchive(Path(temporary.filePath("does-not-exist.mpq"))));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
    provider.SetMapResourceDirectory({});
    QVERIFY(provider.IconForTexture("Textures/Shared.png").isNull());
}

void IconResourceLayerTests::ResourceRootRefreshAndClear() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto first = temporary.filePath("first");
    const auto second = temporary.filePath("second");
    QVERIFY(Save(first, "Textures/Shared.png", QColor(Qt::red)));
    QVERIFY(Save(second, "Textures/Shared.png", QColor(Qt::blue)));
    IconProvider provider;
    provider.SetMapResourceDirectory(Path(first));
    QCOMPARE(Pixel(provider), QColor(Qt::red));
    // 原路径内容刷新后重设同一目录，也必须刷新成功缓存。
    QVERIFY(Save(first, "Textures/Shared.png", QColor(Qt::green)));
    provider.SetMapResourceDirectory(Path(first));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
    provider.SetMapResourceDirectory(Path(second));
    QCOMPARE(Pixel(provider), QColor(Qt::blue));
    provider.SetMapResourceDirectory({});
    QVERIFY(provider.IconForTexture("Textures/Shared.png").isNull());
    provider.SetMapResourceDirectory(Path(first));
    QCOMPARE(Pixel(provider), QColor(Qt::green));
}

void IconResourceLayerTests::ExtraRootAndGameRootSwitch() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto first = temporary.filePath("first");
    const auto second = temporary.filePath("second");
    QVERIFY(Save(first, "Textures/Shared.png", QColor(Qt::cyan)));
    QVERIFY(Save(second, "Textures/Shared.png", QColor(Qt::magenta)));
    IconProvider provider;
    QVERIFY(provider.IconForTexture("Textures/Shared.png").isNull());
    provider.SetExtraIconDir(Path(first));
    QCOMPARE(Pixel(provider), QColor(Qt::cyan));
    provider.SetExtraIconDir(Path(second));
    QCOMPARE(Pixel(provider), QColor(Qt::magenta));
    provider.SetExtraIconDir({});
    QVERIFY(provider.IconForTexture("Textures/Shared.png").isNull());
    const auto gameFirst = temporary.filePath("game-first");
    const auto gameSecond = temporary.filePath("game-second");
    QVERIFY(Archive(gameFirst + "/war3.mpq", QColor(Qt::red)));
    QVERIFY(Archive(gameSecond + "/war3.mpq", QColor(Qt::blue)));
    QVERIFY(provider.SetGameRoot(Path(gameFirst)));
    QCOMPARE(Pixel(provider), QColor(Qt::red));
    QVERIFY(provider.SetGameRoot(Path(gameSecond)));
    QCOMPARE(Pixel(provider), QColor(Qt::blue));
}

void IconResourceLayerTests::NormalizedRelativePathsShareCache() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(Save(temporary.path(), "Textures/Shared.png", QColor(Qt::red)));
    IconProvider provider;
    provider.SetMapResourceDirectory(Path(temporary.path()));
    const auto original = provider.IconForTexture("Textures/Shared.png");
    QVERIFY(!original.isNull());
    for (const auto& path : {QStringLiteral("textures\\shared.png"), QStringLiteral("TEXTURES/SHARED.PNG"),
        QStringLiteral("  Textures/Shared.png  "), QStringLiteral("Textures/Shared.png,ignored-entry")}) {
        const auto normalized = provider.IconForTexture(path);
        QVERIFY(!normalized.isNull());
        QCOMPARE(normalized.cacheKey(), original.cacheKey());
        QCOMPARE(Pixel(provider, path), QColor(Qt::red));
    }
    const auto archive = temporary.filePath("case.mpq");
    QVERIFY(Archive(archive, QColor(Qt::blue), "Textures/MixedCase.PNG"));
    QVERIFY(provider.SetMapArchive(Path(archive)));
    QCOMPARE(Pixel(provider, "TEXTURES/mixedcase.png"), QColor(Qt::blue));
}

void IconResourceLayerTests::RejectPathsOutsideResourceRoot() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto resource = temporary.filePath("resource");
    QVERIFY(QDir().mkpath(resource));
    QVERIFY(Save(temporary.path(), "outside.png", QColor(Qt::red)));
    QVERIFY(Save(resource, "safe.png", QColor(Qt::green)));
    IconProvider provider;
    provider.SetMapResourceDirectory(Path(resource));
    provider.SetExtraIconDir(Path(temporary.path()));
    QCOMPARE(Pixel(provider, "safe.png"), QColor(Qt::green));
    const QString outside = temporary.filePath("outside.png");
    const QStringList rejected{
        "../outside.png", "..\\outside.png", "folder/../../outside.png", "./safe.png", "folder/../safe.png",
        outside, QDir::toNativeSeparators(outside), "/outside.png", "\\outside.png",
        "\\\\server\\share\\outside.png", "//server/share/outside.png", "C:outside.png",
        QStringLiteral("safe.png") + QChar(0) + QStringLiteral("ignored")};
    for (const auto& path : rejected) {
        QVERIFY2(provider.IconForTexture(path).isNull(), qPrintable(path));
    }
    QCOMPARE(Pixel(provider, "safe.png"), QColor(Qt::green));
}

// 函数库分类图标的三种写法：相对 YDWE 根（双反斜杠）、带 ui\\层名 的根相对路径、相对所在层目录；
// 另有 share 覆盖层替代游戏同名文件、none 表示无图标、category 字段带引号。夹具为原创合成数据。
void IconResourceLayerTests::UiLibraryCategoryIconsResolveToolAndLayerPaths() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString ydwe = temporary.filePath("ydwe");
    QVERIFY(WriteText(ydwe + "/ui/config", "base\r\nlib\r\n"));
    QVERIFY(WriteText(ydwe + "/ui/base/define.txt",
        "[TriggerCategories]\r\n"
        "TC_ROOT=Root,icons\\\\Root.png\r\n"
        "TC_OVERLAY=Overlay,Textures\\Over.png\r\n"
        "TC_NONE=None,none\r\n"));
    QVERIFY(WriteText(ydwe + "/ui/lib/define.txt",
        "[TriggerCategories]\r\n"
        "TC_UIPATH=UiPath,ui\\\\lib\\\\icons\\\\Atom.png\r\n"
        "TC_LAYER=Layer,icons\\\\Layer.png\r\n"));
    QVERIFY(WriteText(ydwe + "/ui/lib/action.txt",
        "[QuotedCategory]\r\ntitle=\"Q\"\r\ndescription=\"Q\"\r\ncategory = \"TC_ROOT\"\r\n"));
    QVERIFY(Save(ydwe, "icons/Root.png", QColor(Qt::red)));
    QVERIFY(Save(ydwe, "ui/lib/icons/Atom.png", QColor(Qt::green)));
    QVERIFY(Save(ydwe, "ui/lib/icons/Layer.png", QColor(Qt::blue)));
    QVERIFY(Save(ydwe, "share/zh-CN/mpq/Textures/Over.png", QColor(Qt::yellow)));
    const QString game = temporary.filePath("game");
    QVERIFY(Archive(game + "/war3.mpq", QColor(Qt::magenta), "Textures/Over.png"));

    auto catalog = War3Map::Trigger::UiCatalog::Load(Path(ydwe));
    QVERIFY(catalog.Succeeded() && catalog.value);
    IconProvider provider;
    QVERIFY(provider.SetGameRoot(Path(game)));
    QCOMPARE(Pixel(provider.IconForCategory("TC_OVERLAY")), QColor{}); // 尚未合并 Catalog
    provider.MergeCatalogCategories(*catalog.value);
    QCOMPARE(provider.CategoryForFunction("QuotedCategory"), QStringLiteral("TC_ROOT"));
    QCOMPARE(Pixel(provider.IconForCategory("TC_OVERLAY")), QColor(Qt::magenta)); // 未设 UI 根：游戏
    QVERIFY(provider.IconForCategory("TC_ROOT").isNull());
    // 传 ui 目录与传 YDWE 根等价。
    provider.SetUiToolRoot(Path(ydwe + "/ui"));
    QCOMPARE(Pixel(provider.IconForCategory("TC_OVERLAY")), QColor(Qt::yellow)); // 覆盖层先于游戏
    QCOMPARE(Pixel(provider.IconForCategory("TC_ROOT")), QColor(Qt::red));
    QCOMPARE(Pixel(provider.IconForFunction("QuotedCategory")), QColor(Qt::red));
    QCOMPARE(Pixel(provider.IconForCategory("TC_UIPATH")), QColor(Qt::green));
    QCOMPARE(Pixel(provider.IconForCategory("TC_LAYER")), QColor(Qt::blue));
    QVERIFY(provider.IconForCategory("TC_NONE").isNull());
    provider.SetUiToolRoot(Path(ydwe));
    QCOMPARE(Pixel(provider.IconForCategory("TC_LAYER")), QColor(Qt::blue));
    provider.SetUiToolRoot({});
    QVERIFY(provider.IconForCategory("TC_ROOT").isNull());
    provider.ResetUiMappings();
    QVERIFY(provider.CategoryForFunction("QuotedCategory").isEmpty());
}

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    IconResourceLayerTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "IconResourceLayerTests.moc"
