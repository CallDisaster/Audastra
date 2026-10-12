#include "MainWindow.h"
#include "CampaignModule.h"
#include "TriggerReplaceRealCase.h"
#include "TriggerReplaceDialog.h"
#include <QUndoGroup>
#include "MapSizeDialog.h"
#include "ApplicationIdentity.h"
#include "Widgets/PopoverSurface.h"
#include "Widgets/ResponsiveToolBar.h"
#include "Widgets/ObjectCatalogWidgets.h"
#include "PopupCapture.h"
#include <QStatusBar>
#include "Shell/HeaderBar.h"
#include "Shell/StartupPage.h"
#include "Dock/DockHost.h"
#include "Widgets/ChangesPanel.h"
#include "Fonts/FontLoader.h"
#include "ObjectModule.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
#include "Widgets/CodeView.h"
#include "AssetOwnedPayload.h"
#include "AssetsModule.h"
#include "ObjectModels.h"
#include "WorldModule.h"
#include "ManagerModule.h"
#include "VariablesModule.h"
#include "WorldViewport.h"
#include "WorldOutlinerModel.h"
#include "WorldAnimationPlayer.h"
#include "MapSaveJob.h"
#include "ProjectSaveJob.h"
#include "LiveEditorTools.h"
#include "AgentWindow.h"
#include "AgentConversationView.h"
#include "Commands/TriggerCommands.h"
#include "Commands/MapDataCommands.h"

#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/Services/TerrainBlightSaveService.h"
#include "../MapDataFixtures.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QApplication>
#include <QAbstractAnimation>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QAction>
#include <QDialog>
#include <QDockWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QListWidget>
#include <QListView>
#include <QLockFile>
#include <QLayout>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPainter>
#include <QSettings>
#include "RecoveryPreferences.h"
#include <QSet>
#include <QStandardPaths>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTcpSocket>
#include <QHostAddress>
#include <QToolButton>
#include <QTest>
#include <QTimer>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTreeView>
#include <QUndoStack>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

const char* kWar3MapBuildStamp = "native-workbench-integration-test";

namespace {
QString LastMapSaveDiagnostics(const QAbstractItemModel& model) {
    QStringList rows;
    for (int row = std::max(0, model.rowCount() - 8); row < model.rowCount(); ++row) {
        QStringList columns;
        for (int column = 0; column < model.columnCount(); ++column)
            columns << model.index(row, column).data().toString().left(1500);
        rows << columns.join(QStringLiteral(" | "));
    }
    return rows.join(QLatin1Char('\n'));
}

QString DescribeMapSaveEntry(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    QString type = QStringLiteral("other");
    if (error) type = QStringLiteral("status-error: %1").arg(QString::fromStdString(error.message()));
    else if (std::filesystem::is_symlink(status)) type = QStringLiteral("symlink");
    else if (std::filesystem::is_directory(status)) type = QStringLiteral("directory");
    else if (std::filesystem::is_regular_file(status)) type = QStringLiteral("regular-file");
    else if (!std::filesystem::exists(status)) type = QStringLiteral("missing");
    QString result = QStringLiteral("entry=%1; path=%2; type=%3")
        .arg(QString::fromStdWString(path.filename().wstring()), QString::fromStdWString(path.wstring()), type);
    if (std::filesystem::is_regular_file(status)) {
        const auto size = std::filesystem::file_size(path, error);
        result += error ? QStringLiteral("; size-error=%1").arg(QString::fromStdString(error.message()))
            : QStringLiteral("; bytes=%1").arg(static_cast<qulonglong>(size));
    }
    return result;
}

QString DescribeMapSaveCandidate(const std::filesystem::directory_entry& entry) {
    QStringList rows{DescribeMapSaveEntry(entry.path())};
    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (!error && std::filesystem::is_directory(status)) {
        std::filesystem::directory_iterator child(entry.path(), error), end;
        int count = 0;
        while (!error && child != end && count < 8) {
            rows << QStringLiteral("child[%1]: %2").arg(count).arg(DescribeMapSaveEntry(child->path()));
            ++count;
            child.increment(error);
        }
        if (error) rows << QStringLiteral("children-error=%1").arg(QString::fromStdString(error.message()));
        else rows << QStringLiteral("children-shown=%1; more=%2").arg(count).arg(child != end);
    }
    return rows.join(QLatin1Char('\n'));
}

QString ProbeFixtureDirectory(const QString& directory, const QString& fixtureRoot) {
    const QFileInfo info(directory);
    if (!info.exists()) return QStringLiteral("missing (not created by diagnostic)");
    if (!info.isDir()) return QStringLiteral("not a directory");
    const auto root = QFileInfo(fixtureRoot).canonicalFilePath();
    const auto canonical = info.canonicalFilePath();
    const auto relative = QDir(root).relativeFilePath(canonical);
    if (root.isEmpty() || canonical.isEmpty() || QDir::isAbsolutePath(relative)
        || relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")))
        return QStringLiteral("outside fixture; no probe");
    const QByteArray marker("original-map-save-write-probe\n");
    QString qtResult;
    {
        QTemporaryFile file(QDir(canonical).filePath(QStringLiteral(".map-save-write-probe-XXXXXX.tmp")));
        if (!file.open()) qtResult = QStringLiteral("open failed: %1").arg(file.errorString());
        else if (file.write(marker) != marker.size() || !file.flush())
            qtResult = QStringLiteral("write failed: %1").arg(file.errorString());
        else if (!file.seek(0) || file.readAll() != marker)
            qtResult = QStringLiteral("readback failed: %1").arg(file.errorString());
        else qtResult = file.remove() ? QStringLiteral("write/read/delete ok")
            : QStringLiteral("delete failed: %1").arg(file.errorString());
    }
    QString nativeResult = QStringLiteral("not applicable");
#ifdef _WIN32
    // 与备份创建使用相同访问和共享标志；仅触碰夹具内唯一新文件。
    auto nativePath = QDir::toNativeSeparators(QDir(canonical).filePath(
        QStringLiteral(".map-save-native-probe-%1.tmp").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))).toStdWString();
    if (nativePath.rfind(L"\\\\?\\", 0) != 0)
        nativePath = nativePath.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + nativePath.substr(2) : L"\\\\?\\" + nativePath;
    const auto handle = CreateFileW(nativePath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) nativeResult = QStringLiteral("CreateFileW error=%1").arg(GetLastError());
    else {
        DWORD written = 0, read = 0;
        char buffer[64]{};
        LARGE_INTEGER start{};
        if (!WriteFile(handle, marker.constData(), DWORD(marker.size()), &written, nullptr) || written != DWORD(marker.size()))
            nativeResult = QStringLiteral("WriteFile error=%1 bytes=%2").arg(GetLastError()).arg(written);
        else if (!FlushFileBuffers(handle)) nativeResult = QStringLiteral("FlushFileBuffers error=%1").arg(GetLastError());
        else if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) nativeResult = QStringLiteral("seek error=%1").arg(GetLastError());
        else if (!ReadFile(handle, buffer, DWORD(sizeof(buffer)), &read, nullptr) || QByteArray(buffer, int(read)) != marker)
            nativeResult = QStringLiteral("ReadFile error=%1 bytes=%2").arg(GetLastError()).arg(read);
        else nativeResult = QStringLiteral("write/read ok");
        CloseHandle(handle);
        nativeResult += DeleteFileW(nativePath.c_str()) ? QStringLiteral("; delete ok")
            : QStringLiteral("; DeleteFileW error=%1").arg(GetLastError());
    }
#endif
    return QStringLiteral("QFileInfo.isWritable=%1; Qt=%2; native=%3").arg(info.isWritable()).arg(qtResult, nativeResult);
}

void ReportMapSaveFixture(const QString& phase, const QString& fixtureRoot) {
    const QSettings settings;
    const auto backup = settings.value(QStringLiteral("mapSave/backupDirectory")).toString();
    qInfo().noquote() << "Map save fixture phase=" << phase << "root=" << fixtureRoot
        << "backupDirectory=" << backup << "backupLimit=" << settings.value(QStringLiteral("mapSave/backupLimit"))
        << "settingsFile=" << settings.fileName() << "settingsStatus=" << int(settings.status());
    qInfo().noquote() << "Map save root probe=" << ProbeFixtureDirectory(fixtureRoot, fixtureRoot);
    if (backup.isEmpty()) {
        qInfo().noquote() << "Map save backup probe=not configured; no installation-directory probe";
        return;
    }
    qInfo().noquote() << "Map save backup probe=" << ProbeFixtureDirectory(backup, fixtureRoot);
    if (!QFileInfo::exists(backup)) {
        const auto parent = QFileInfo(backup).absolutePath();
        qInfo().noquote() << "Map save backup parent=" << parent << "probe=" << ProbeFixtureDirectory(parent, fixtureRoot);
    }
}

class ScopedMapBackupPreferences final {
public:
    explicit ScopedMapBackupPreferences(const QString& directory) {
        QSettings settings;
        directory_ = settings.value(QStringLiteral("mapSave/backupDirectory"));
        limit_ = settings.value(QStringLiteral("mapSave/backupLimit"));
        settings.setValue(QStringLiteral("mapSave/backupDirectory"), directory);
        settings.setValue(QStringLiteral("mapSave/backupLimit"), 10);
        settings.sync();
        qInfo().noquote() << "Map backup preference seed status=" << int(settings.status());
        ReportMapSaveFixture(QStringLiteral("backup-preferences-seeded"), QFileInfo(directory).absolutePath());
    }
    ~ScopedMapBackupPreferences() {
        QSettings settings;
        for (const auto& entry : {std::pair{QStringLiteral("mapSave/backupDirectory"), directory_},
                 std::pair{QStringLiteral("mapSave/backupLimit"), limit_}}) {
            if (entry.second.isValid()) settings.setValue(entry.first, entry.second);
            else settings.remove(entry.first);
        }
        settings.sync();
    }
private:
    QVariant directory_, limit_;
};

using War3Map::ObjectData::LniObjectDocument;
using War3Map::ObjectData::NativeObjectCodec;
using War3Map::ObjectData::ObjectType;
using War3Map::ObjectData::ObjectFieldSchema;
using War3Map::ObjectData::StandardObjectCatalog;

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

void AppendI32(std::vector<std::byte>& bytes, std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    for (unsigned shift : {0U, 8U, 16U, 24U}) {
        bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffU));
    }
}

// 原创最小 w3u v2：一个原版 ID 覆盖、一个整数修改、无自定义对象。
// 此测试只验证编辑器载入/会话状态，合成归档不是可运行地图。
std::vector<std::byte> MinimalUnitData(std::int32_t health) {
    std::vector<std::byte> bytes;
    AppendI32(bytes, 2);
    AppendI32(bytes, 1);
    const auto id = Bytes("hfoo");
    bytes.insert(bytes.end(), id.begin(), id.end());
    AppendI32(bytes, 0);
    AppendI32(bytes, 1);
    const auto field = Bytes("uhpm");
    bytes.insert(bytes.end(), field.begin(), field.end());
    AppendI32(bytes, 0);
    AppendI32(bytes, health);
    AppendI32(bytes, 0);
    AppendI32(bytes, 0);
    return bytes;
}

constexpr std::array kClassicObjectV1Types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
    ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};

std::vector<std::byte> MinimalObjectV1(ObjectType type, std::int32_t value = 42) {
    MapDataFixtures::Fixture fixture;
    fixture.Int(1); fixture.Int(1); fixture.Raw("hfoo"); fixture.Int(0); fixture.Int(2);
    fixture.Raw("I001"); fixture.Int(0);
    if (type == ObjectType::Ability || type == ObjectType::Upgrade) { fixture.Int(2); fixture.Int(3); }
    fixture.Int(value); fixture.Raw("hfoo");
    fixture.Raw("S001"); fixture.Int(3);
    if (type == ObjectType::Ability || type == ObjectType::Upgrade) { fixture.Int(4); fixture.Int(5); }
    fixture.String(std::string_view{"A\xd6\xd0", 3}); fixture.Int(0); fixture.Int(0);
    return fixture.bytes;
}

std::filesystem::path NativePath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

bool WriteText(const QString& path, const QByteArray& text) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(text) == text.size();
}

QByteArray ReadFile(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

War3Map::Status MakeArchive(const std::filesystem::path& path, std::int32_t health) {
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", MinimalUnitData(health), false}
    };
    return War3Map::Mpq::WriteMpqArchive(path, files);
}

War3Map::Status MakeRecoveryLifecycleArchive(const std::filesystem::path& path, std::size_t payloadBytes = 8 * 1024 * 1024) {
    // 原创有界资源使真实写出跨过窗口析构；不以 mock 代替 worker，也不加载地图代码。
    std::vector<std::byte> payload(payloadBytes);
    std::uint32_t state = 0x614c3f29U;
    for (auto& byte : payload) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<std::byte>(state & 0xffU);
    }
    std::vector<War3Map::Mpq::MpqBuildFile> files;
    files.push_back({"war3map.w3u", MinimalUnitData(100), false});
    files.push_back({"resource/recovery-lifecycle.bin", std::move(payload), false});
    return War3Map::Mpq::WriteMpqArchive(path, files);
}

std::optional<std::int32_t> Health(const LniObjectDocument& document) {
    if (document.Objects().size() != 1) {
        return std::nullopt;
    }
    for (const auto& modification : document.Objects().front().modifications) {
        if (modification.key.fieldId == "uhpm") {
            if (const auto* value = std::get_if<std::int32_t>(&modification.value)) {
                return *value;
            }
        }
    }
    return std::nullopt;
}

War3Map::Result<StandardObjectCatalog> SyntheticCatalog() {
    // 外部目录通过 Core 公开的 FileReader 接口注入，不使用原版表或图标。
    const std::string slk =
        "ID;PWXL\n"
        "C;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\nC;X3;K\"Race\"\nC;X4;K\"HP\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Synthetic standard unit\"\nC;X3;K\"human\"\nC;X4;K100\n"
        "C;X1;Y3;K\"hrif\"\nC;X2;K\"Synthetic other unit\"\nC;X3;K\"human\"\nC;X4;K200\nE\n";
    War3Map::ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectType::Unit, {"synthetic.slk"}, {}}};
    options.stringFiles.clear();
    return StandardObjectCatalog::Load([&slk](const std::string& path) {
        return path == "synthetic.slk" ? War3Map::Result<std::vector<std::byte>>::Success(Bytes(slk)) :
            War3Map::Result<std::vector<std::byte>>::Failure("Test", "unexpected-file", path);
    }, options);
}

std::string SyntheticSlk(const std::vector<std::vector<std::string>>& rows) {
    std::string result = "ID;PWXL\n";
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t column = 0; column < rows[row].size(); ++column) {
            std::string escaped;
            for (const char c : rows[row][column]) {
                escaped += c;
                if (c == '"') escaped += '"';
            }
            result += "C;X" + std::to_string(column + 1) + ";Y" + std::to_string(row + 1)
                + ";K\"" + escaped + "\"\n";
        }
    }
    return result + "E\n";
}

std::unordered_map<std::string, std::string> SyntheticEditorFiles() {
    // 所有目录、metadata、中文字符串均为原创合成数据，不读取本地 Warcraft 安装。
    return {
        {"units.slk", SyntheticSlk({
            {"alias", "Name", "Race", "HP", "def", "internalSlot"},
            {"hfoo", "合成步兵", "human", "100", "5", "engine-only"},
            {"hrif", "合成射手", "human", "200", "3", "engine-only"}})},
        {"items.slk", SyntheticSlk({
            {"alias", "Name", "goldcost"}, {"itst", "合成物品", "25"}})},
        {"unit-meta.slk", SyntheticSlk({
            {"ID", "field", "slk", "displayName", "category", "type", "index", "useUnit", "useHero", "useBuilding", "useItem", "minVal", "maxVal"},
            {"uhpm", "HP", "UnitBalance", "WESTRING_SYN_HEALTH", "stats", "int", "-1", "1", "1", "1", "0", "0", "1000000"},
            {"udef", "def", "UnitBalance", "WESTRING_SYN_DEFENSE", "combat", "int", "-1", "1", "1", "1", "0", "0", "10000"},
            {"unam", "Name", "Profile", "WESTRING_SYN_NAME", "text", "string", "0", "1", "1", "1", "0", "", ""}})},
        {"item-meta.slk", SyntheticSlk({
            {"ID", "field", "slk", "displayName", "category", "type", "index", "useUnit", "useHero", "useBuilding", "useItem", "minVal", "maxVal"},
            {"igol", "goldcost", "ItemData", "WESTRING_SYN_GOLD", "stats", "int", "-1", "0", "0", "0", "1", "0", "1000000"},
            {"unam", "Name", "Profile", "WESTRING_SYN_NAME", "text", "string", "0", "0", "0", "0", "1", "", ""}})},
        {"editor-strings.txt", "[WorldEditStrings]\nWESTRING_SYN_HEALTH=生命上限\nWESTRING_SYN_DEFENSE=基础防御\n"
            "WESTRING_SYN_NAME=名称\nWESTRING_SYN_GOLD=金币消耗\nWESTRING_SYN_STATS=属性\n"
            "WESTRING_SYN_COMBAT=战斗\nWESTRING_SYN_TEXT=文本\n"},
        {"editor-data.txt", "[ObjectEditorCategories]\nstats=WESTRING_SYN_STATS\ncombat=WESTRING_SYN_COMBAT\ntext=WESTRING_SYN_TEXT\n"}
    };
}

// 普通原版对象表中的单一整数覆盖；单位和物品均使用无等级/变体地址的布局。
std::vector<std::byte> SingleIntegerOverride(std::string_view object, std::string_view field, std::int32_t value) {
    std::vector<std::byte> bytes;
    AppendI32(bytes, 2);
    AppendI32(bytes, 1);
    const auto id = Bytes(object);
    bytes.insert(bytes.end(), id.begin(), id.end());
    AppendI32(bytes, 0);
    AppendI32(bytes, 1);
    const auto key = Bytes(field);
    bytes.insert(bytes.end(), key.begin(), key.end());
    AppendI32(bytes, 0);
    AppendI32(bytes, value);
    AppendI32(bytes, 0);
    AppendI32(bytes, 0);
    return bytes;
}

std::optional<std::int32_t> IntegerOverride(const War3Map::ObjectData::ObjectRecord* object, std::string_view field) {
    if (!object) return std::nullopt;
    for (const auto& modification : object->modifications) {
        if (modification.key.fieldId == field) {
            if (const auto* value = std::get_if<std::int32_t>(&modification.value)) return *value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> StringOverride(const War3Map::ObjectData::ObjectRecord* object, std::string_view field) {
    if (!object) return std::nullopt;
    for (const auto& modification : object->modifications) {
        if (modification.key.fieldId == field) {
            if (const auto* value = std::get_if<std::string>(&modification.value)) return *value;
        }
    }
    return std::nullopt;
}

int FieldRow(ObjectFieldModel& model, std::string_view nativeId) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (const auto* definition = model.DefinitionAt(row); definition && definition->fieldId == nativeId) return row;
    }
    return -1;
}

std::optional<std::int32_t> EffectiveInteger(ObjectFieldModel& model, std::string_view nativeId) {
    const auto value = model.EditorValueAt(FieldRow(model, nativeId));
    if (value) {
        if (const auto* integer = std::get_if<std::int32_t>(&*value)) return *integer;
    }
    return std::nullopt;
}

bool ModelContains(const QAbstractItemModel& model, const QString& text, int column = 0) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.index(row, column).data(Qt::DisplayRole).toString().contains(text)) return true;
    }
    return false;
}

QString LuaQtEvidencePattern() {
    auto root = qEnvironmentVariable("FORGE_LUA_QT_EVIDENCE_DIR");
    if (root.isEmpty()) root = QDir::homePath() + "/.forge-test-artifacts/lua-map-save";
    if (!QDir().mkpath(root)) return {};
    return QDir(root).filePath("native-XXXXXX");
}

QString LuaQtDiagnostics(const auto& result) {
    QStringList messages;
    for (const auto& diagnostic : result.diagnostics)
        messages << QString::fromStdString(diagnostic.code + ": " + diagnostic.message);
    return messages.join('\n');
}

// B/C 共用的真实动效稳定捕获门禁；只比较树视口，不读取整张助手页面的重复像素。
QJsonObject StabilizeDetailTree(QTreeWidget* tree, QTreeWidgetItem* anchor, QTreeWidgetItem* target,
    War3Map::Agent::AgentConversationView* conversation) {
    const int styleDuration = tree->style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, tree);
    const int minimumSettleMs = std::max(250, styleDuration);
    QElapsedTimer settling, unchanged;
    settling.start();
    QImage previousViewport;
    QJsonArray previousRows, visibleRows;
    int stableSamples = 0;
    bool positioned = false, stable = false, rowsDisjoint = false;
    while (settling.elapsed() < 5000) {
        QTest::qWait(40);
        if (settling.elapsed() < minimumSettleMs) continue;
        if (!positioned) {
            tree->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
            conversation->ensureWidgetVisible(tree, 0, 16);
            positioned = true;
            continue;
        }
        bool animationRunning = false;
        for (auto* ancestor = static_cast<QWidget*>(tree); ancestor && ancestor != conversation; ancestor = ancestor->parentWidget()) {
            for (const auto* animation : ancestor->findChildren<QAbstractAnimation*>(QString{}, Qt::FindDirectChildrenOnly))
                animationRunning |= animation->state() == QAbstractAnimation::Running;
        }
        visibleRows = {};
        std::vector<QRect> rectangles;
        rowsDisjoint = true;
        const auto viewportRect = tree->viewport()->rect();
        for (QTreeWidgetItemIterator item(tree); *item; ++item) {
            auto* currentItem = *item;
            bool visible = !currentItem->isHidden();
            for (auto* parent = currentItem->parent(); parent; parent = parent->parent())
                visible &= parent->isExpanded() && !parent->isHidden();
            if (!visible) continue;
            const auto rect = tree->visualItemRect(currentItem);
            if (rect.isEmpty() || !rect.intersects(viewportRect)) continue;
            for (const auto& previous : rectangles) rowsDisjoint &= !previous.intersects(rect);
            rectangles.push_back(rect);
            QStringList path;
            for (auto* parent = currentItem; parent; parent = parent->parent()) path.prepend(parent->text(0));
            visibleRows.append(QJsonObject{{"path", path.join(QLatin1Char('/'))}, {"x", rect.x()}, {"y", rect.y()},
                {"width", rect.width()}, {"height", rect.height()}});
        }
        const auto textRect = tree->visualItemRect(target);
        const bool textFullyVisible = !textRect.isEmpty() && viewportRect.contains(textRect);
        const auto viewport = tree->viewport()->grab().toImage();
        const bool unchangedFrame = !animationRunning && rowsDisjoint && textFullyVisible && !viewport.isNull()
            && viewport == previousViewport && visibleRows == previousRows;
        if (unchangedFrame) {
            ++stableSamples;
            if (!unchanged.isValid()) unchanged.start();
            if (stableSamples >= 5 && unchanged.elapsed() >= 200) { stable = true; break; }
        } else {
            stableSamples = 0;
            unchanged.invalidate();
        }
        previousViewport = viewport;
        previousRows = visibleRows;
    }
    return {{"stable", stable}, {"tree_animation_enabled", tree->isAnimated()}, {"style_animation_duration_ms", styleDuration},
        {"minimum_settle_ms", minimumSettleMs}, {"actual_settle_ms", settling.elapsed()}, {"stable_samples", stableSamples},
        {"stable_window_ms", unchanged.isValid() ? unchanged.elapsed() : 0}, {"visible_rows_disjoint", rowsDisjoint}, {"visible_rows", visibleRows}};
}

} // namespace

class NativeWorkbenchTests final : public QObject {
    Q_OBJECT

private slots:
    void CampaignModuleMainWindowRouting();
    void TriggerReplaceOwnedLargeMapMainHost256();
    void TriggerConversionOwnedLargeMapMainHost();
    void RecentMapsUserOriginAndPrivateRestore();
    void RecentMapsShareSurfacesAndPreserveAllPinned();
    void ArchiveWorkspaceOwnsOnlyDerivedDirectories();
    void ArchiveWorkspaceCancellationAndFailureKeepActiveOwner();
    void ShellFitsCommonWindowSizes();
    void StartupPageSharesEditorAndAssistantHost();
    void AgentPresentationKeepsConversationAndOriginalEditorHost();
    void CommandPaletteQueriesCurrentProjectAndRejectsStaleResults();
    void CommandExecutionMayDeleteWindowOwner();
    void BaselineNotificationDeletionStopsWorkspaceOpen_data();
    void BaselineNotificationDeletionStopsWorkspaceOpen();
    void WatchNotificationDeletionStopsWorkspaceOpen_data();
    void WatchNotificationDeletionStopsWorkspaceOpen();
    void WatchNotificationOwnerReplacementStopsWorkspaceOpen();
    void EarlyWatchNotificationDeletionStopsWorkspaceOpen();
    void TriggerUndoObserverRejectsReplacedPublicStack_data();
    void TriggerUndoObserverRejectsReplacedPublicStack();
    void MapSizeHostMenuAndActivationUseCurrentOwnedArchive();
    void ChangesPanelUsesBaselineAndSharedUndoWithoutWritingSource();
    void WorkspaceActionsToggleAndStatusBarsFollowPresentation();
    void ChangesPanelObjectsAndTriggerUseSharedInverse();
    void MapDataPanelReadsSearchesAndPaginates();
    void MapDataSurvivesProjectRestoreAndSourceSwitch();
    void MapOpenRetainsNativeBaselineAndCurrentSource();
    void MapFileDropUsesOpenTransactionAndKeepsPendingEdits();
    void ResetClearsArchiveImportsAndVisibleList();
    void OpeningLniDoesNotInheritPreviousNativeBaseline();
    void StandardBrowsingIsReadOnlyAndHiddenSelectionKeepsMapTitle();
    void ConsecutiveSaveKeepsPriorEditsAndImports();
    void SaveMapInPlaceWritesBackAndKeepsBackups();
    void RecoveryProjectSavesBackToOriginalMap();
    void TestMapSnapshotPreservesSourceDirtyRevisionAndUndo();
    void TestMapSnapshotRejectsStaleInitializationScript();
    void ApplicationIdentityUsesAudastraWithoutMigration();
    void WindowTitleTracksMapSaveUndoAndClose();
    void AboutSurfaceShowsVersionAndBuildOutsideWindowTitle();
    void ProductMenusRemainAvailableWithoutStartingProbeServices();
    void UpdateCancellationPreservesEditsWithoutDiscardChoice();
    void NativeMenusAndPickersExposeRealPopupGeometry();
    void AgentHttpToolsShareLiveDocumentAndRenderReceipts();
    void OptionalRealMapAgentHttpToolsRestoreOriginalDocument();
    void AgentHttpVariableToolsShareTriggerUndoAndRenderReceipts();
    void OptionalRealMapAgentHttpVariablesRestoreOriginalDocument();
    void AgentHttpEntityToolsShareMapDataUndoAndRenderReceipts();
    void OptionalRealMapAgentHttpEntitiesRestoreOriginalDocument();
    void AgentHttpPlacementsRestoreOriginalDocument();
    void OptionalRealMapAgentHttpPlacementsRestoreOriginalDocument();
    void AgentHttpEntityFiltersPreserveOriginalDocument();
    void OptionalRealMapAgentHttpEntityFiltersPreserveOriginalDocument();
    void AgentHttpEntityPositionRestoresOriginalDocument();
    void OptionalRealMapAgentHttpEntityPositionRestoresOriginalDocument();
    void AgentHttpSoundToolsRestoreOriginalDocument();
    void AgentHttpImportToolsPreserveOriginalDocument();
    void ClassicV1FieldsUseGuiUndoSaveAndBackups();
    void OptionalRealClassicV1FieldUsesGuiSave();
    void CancelAndWriteFailureKeepSessionBaseline();
    void MissingImportRejectsWithoutOutput();
    void UnavailableSourceRejectsWithoutOutput();
    void UnauditedDeleteIsUnavailable();
    void ChineseFieldsRawToggleAndSearchPreserveDocument();
    void InheritedFieldEditAndResetPersistToNativeCandidate();
    void ObjectUndoRedoRemainBoundToCategory();
    void StandardObjectButtonCreatesOriginalOverrideAndReopens();
    void MissingMemberCustomObjectKeepsNameFieldsAndUndoHistory();
    void SavedObjectCreationCanBeUndoneAndRedone();
    void ClonedCustomObjectRoundTripsWithUndo();
    void MalformedExistingMemberCannotBeRecreatedAsEmpty();
    void ParentChainNamesAndIconsReachListPreviewAndReferences();
    void EditorProjectRestoresObjectsAndImportSnapshots();
    void ProjectSaveFailureAndInvalidRestorePreserveSession();
    void ChangedSourceRejectsMapAndProjectSave();
    void ImportsRegisterNativeImportTable();
    void AssetEditsSaveMovesDeletesAndDisabledIcons();
    void BuildCheckDoesNotSaveOrClearEdits();
    void TriggerProjectEmbedsDefinitionsAndRefusesUnbuiltScript();
    void LuaMapTargetSavesGuiTriggersAndRebindsCandidate();
    void LiveEditorNaturalTUsesActualGuiUndoAndLuaMapSave();
    void LuaMapTargetGuardsActiveSaveAndClearsOnMapSwitch();
    void LuaMapTargetOnlyInPlaceSaveWritesCopyAndBacksUp();
    void DirectoryProjectAndInvalidOpenPreserveEdits();
    void CodeDraftDirectoryCtrlSSavesProject();
    void GuiConversionSnapshotSurvivesProjectReopen();
    void GuiConversionSnapshotDoesNotBindReusedTriggerIdentity();
    void CodeDraftFailureKeepsSourceAndExplicitDiscardWorks();
    void IncompleteLuaDraftSurvivesApplyHostRecoveryAndMapGate();
    void MapDataOnlyChangesProduceAutomaticRecovery();
    void FrozenRecoveryKeepsLaterEditsAndRejectsNewSession();
    void ProjectDirectoryUnicodeBaselineRoundTrip();
    void MapSaveWaitsForRecoveryAndCapturesLaterEdit();
    void RecoveryWorkerSurvivesWindowDestruction();
    void RecoveryQuitWaitsForWorkerAndPublishesCompletePoint();
    void RecoveryExitCancellationKeepsPreviousPoint();
    void RecoveryStartupExceptionKeepsPreviousPointAndManualTicket();
    void RecoveryPolicyCadenceDisableAndActualTimeoutKeepOriginalMap();
    void LargeMapAutomaticRecoveryKeepsSourceAndMeasuresUi();
    void CorruptImportTableRejectsWithoutOutput();
    void ImportCannotBypassScriptBuild();
    void MapDataEditRequiresScriptBuildAndSurvivesProjectRestore();
    void StageDSettingsAndSoundDraftsPersistInProject();
    void ProjectUpdatesSameFileAndKeepsBackup();
    void StaleProjectAndRecoveryKeepManualBaseline();
    void EditRevisionFollowsUndoAndIndependentSaveTargets();
    void ProjectJournalSurvivesReopenAndUpdate();
    void ProjectCloseGuardSurvivesUndoAndImportRemoval();
    void LockedImportRejectsProjectAndMapWithoutClearingEdits();
    void ScriptSaveUiAndCliUseSameMembers();
    void NativeWorldFacingChangesSaveAndReopen();
    void NativeWorldAnimationProducesCompletedFrames();
    void NativeWorldPlacementBackgroundEvidence();

private:
    enum class AgentHttpProbeKind { Objects, Variables, Entities, PlacementCopy, EntityFilters, EntityPosition, Sounds, Imports };
    void RunAgentHttpDocumentProbe(bool realMap, AgentHttpProbeKind kind = AgentHttpProbeKind::Objects);
    bool ChangeHealth(MainWindow& window, std::int32_t health);
    bool InstallSyntheticEditorContext(MainWindow& window);
    bool InstallLuaSaveFixture(MainWindow& window, const QTemporaryDir& fixture,
        std::int32_t formatVersion, std::filesystem::path& configuration);
};

bool NativeWorkbenchTests::InstallSyntheticEditorContext(MainWindow& window) {
    const auto files = SyntheticEditorFiles();
    const auto reader = [&files](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? War3Map::Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : War3Map::Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    War3Map::ObjectData::CatalogLoadOptions catalogOptions;
    catalogOptions.sources = {{ObjectType::Unit, {"units.slk"}, {}}, {ObjectType::Item, {"items.slk"}, {}}};
    catalogOptions.stringFiles.clear();
    auto catalog = StandardObjectCatalog::Load(reader, catalogOptions);
    War3Map::ObjectData::ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.sources = {{ObjectType::Unit, "unit-meta.slk"}, {ObjectType::Item, "item-meta.slk"}};
    schemaOptions.stringFiles = {"editor-strings.txt"};
    schemaOptions.worldEditDataFile = "editor-data.txt";
    schemaOptions.unitEditorDataFile.clear();
    auto schema = ObjectFieldSchema::Load(reader, schemaOptions);
    if (!catalog.Succeeded() || !schema.Succeeded()) return false;
    window.project_->standardCatalog_ = std::make_unique<StandardObjectCatalog>(std::move(*catalog.value));
    window.project_->objectFieldSchema_ = std::make_unique<ObjectFieldSchema>(std::move(*schema.value));
    window.objectsModule_->objectFieldModel_->SetContext(window.project_->standardCatalog_.get(), window.project_->objectFieldSchema_.get());
    window.objectsModule_->UpdateObjectReferenceEntries();
    window.objectsModule_->ShowObjectType(0);
    window.objectsModule_->RefreshObjectFieldPresentation();
    return true;
}

bool NativeWorkbenchTests::ChangeHealth(MainWindow& window, std::int32_t health) {
    auto* document = window.project_->objectDocuments_[0].get();
    if (!document || document->Objects().size() != 1 || document->Objects().front().modifications.empty()) return false;
    const auto& object = document->Objects().front();
    const auto result = document->SetModification(object.key, object.modifications.front().key, health);
    if (result.Succeeded()) window.objectsModule_->MarkCurrentObjectDocumentDirty();
    return result.Succeeded();
}

bool NativeWorkbenchTests::InstallLuaSaveFixture(MainWindow& window, const QTemporaryDir& fixture,
    std::int32_t formatVersion, std::filesystem::path& configuration) {
    ReportMapSaveFixture(QStringLiteral("install-lua-fixture"), fixture.path());
    const auto fail = [&window](const QString& message) {
        qWarning().noquote() << message << '\n' << LastMapSaveDiagnostics(*window.project_->diagnosticsModel_);
        return false;
    };
    const auto checker = NativePath(qEnvironmentVariable("FORGE_YDWE_ROOT")) / "bin/lua.exe";
    const auto checkerHash = War3Map::Sha256File(checker);
    if (!checkerHash.Succeeded()) return fail("Real FORGE_YDWE_ROOT/bin/lua.exe required; no skipped Lua execution");
    const auto definitions = fixture.filePath("definitions");
    if (!WriteText(definitions + "/ui/config", "base\n")
        || !WriteText(definitions + "/ui/base/action.txt",
            "[DoNothing]\ntitle=\"空动作\"\ndescription=\"空动作\"\n")) return fail("write original GUI definitions");
    std::vector<std::byte> wtg = Bytes("WTG!"), wct;
    const auto text = [](std::vector<std::byte>& bytes, std::string_view value) {
        const auto data = Bytes(value); bytes.insert(bytes.end(), data.begin(), data.end()); bytes.push_back(std::byte{});
    };
    AppendI32(wtg, 7); AppendI32(wtg, 1); AppendI32(wtg, 1); text(wtg, "Original GUI folder"); AppendI32(wtg, 0);
    AppendI32(wtg, 2); AppendI32(wtg, 0); AppendI32(wtg, 1);
    text(wtg, "Original GUI trigger"); text(wtg, "Original natural-language T, not custom code");
    for (const auto flag : {0, 1, 0, 0, 1, 1, 1, 2}) AppendI32(wtg, flag);
    text(wtg, "DoNothing"); AppendI32(wtg, 1); AppendI32(wtg, 0);
    AppendI32(wct, 1); text(wct, ""); AppendI32(wct, 0); AppendI32(wct, 1); AppendI32(wct, 0);
    War3Map::MapData::MapSettings settings;
    settings.formatVersion = formatVersion; settings.scriptLanguage = 0; settings.tileset = 'L'; settings.custom.emplace();
    settings.mapName = "Original Qt Lua save map";
    if (formatVersion == 31) { settings.gameVersion = {1, 32, 0, 0}; settings.graphicsModes = 3; settings.gameDataVersion = 1; }
    const auto encoded = settings.Serialize();
    if (!encoded.Succeeded()) return fail(LuaQtDiagnostics(encoded));
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", *encoded.value}, {"war3map.wtg", wtg}, {"war3map.wct", wct},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.j", Bytes("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")},
        {"unmanaged.bin", Bytes("original Qt fixture bytes")}};
    const auto written = War3Map::WriteVerifiedArchive(source, members);
    if (!written.Succeeded()) return fail(LuaQtDiagnostics(written));
    if (!window.OpenMapArchivePath(source)) return fail("open original map in actual Qt project");
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(definitions));
    if (!catalog.Succeeded()) return fail(LuaQtDiagnostics(catalog));
    auto parsed = War3Map::Trigger::WtgWctCodec{}.Parse(wtg, wct, *catalog.value);
    if (!parsed.Succeeded()) return fail(LuaQtDiagnostics(parsed));
    window.project_->uiCatalog_ = std::make_unique<War3Map::Trigger::UiCatalog>(std::move(*catalog.value));
    window.project_->activeUiDataRoot_ = NativePath(definitions);
    window.project_->compilerDataRoot_.clear();
    window.project_->war3GameRoot_.clear();
    window.project_->uiLibraryPackages_.clear();
    window.project_->triggerDocument_ = std::make_unique<War3Map::Trigger::TriggerDocument>(std::move(*parsed.value));
    const QByteArray profile =
        "forge-lua-target-v1\nidentity\toriginal-native-qt/lua53\n"
        "type\tplacement\thandle\ntype\ttrigger\thandle\ntype\ttriggeraction\thandle\n"
        "type\tboolexpr\thandle\ntype\ttriggercondition\thandle\n"
        "api\tCreateTrigger\ttrigger\napi\tTriggerAddAction\ttriggeraction\ttrigger\tcode\n"
        "api\tCondition\tboolexpr\tcode\napi\tTriggerAddCondition\ttriggercondition\ttrigger\tboolexpr\n"
        "api\tConditionalTriggerExecute\tnothing\ttrigger\napi\tDoNothing\tnothing\n"
        "api\tGetCameraMargin\treal\tinteger\n"
        "api\tSetCameraBounds\tnothing\treal\treal\treal\treal\treal\treal\treal\treal\n"
        "api\tSetDayNightModels\tnothing\tstring\tstring\napi\tNewSoundEnvironment\tnothing\tstring\n"
        "api\tSetAmbientDaySound\tnothing\tstring\napi\tSetAmbientNightSound\tnothing\tstring\n"
        "api\tSetMapName\tnothing\tstring\napi\tSetMapDescription\tnothing\tstring\n"
        "api\tSetMapMusic\tnothing\tstring\tboolean\tinteger\napi\tInitBlizzard\tnothing\n"
        "api\tInitGenericPlayerSlots\tnothing\napi\tSetPlayers\tnothing\tinteger\napi\tSetTeams\tnothing\tinteger\n"
        "api\tSetGamePlacement\tnothing\tplacement\n"
        "constant\tCAMERA_MARGIN_LEFT\tinteger\nconstant\tCAMERA_MARGIN_RIGHT\tinteger\n"
        "constant\tCAMERA_MARGIN_BOTTOM\tinteger\nconstant\tCAMERA_MARGIN_TOP\tinteger\n"
        "constant\tMAP_PLACEMENT_USE_MAP_SETTINGS\tplacement\n";
    const auto profilePath = fixture.filePath("profile.tsv");
    if (!WriteText(profilePath, profile)) return fail("write explicit original target profile");
    const auto profileHash = War3Map::Sha256File(NativePath(profilePath));
    if (!profileHash.Succeeded()) return fail(LuaQtDiagnostics(profileHash));
    const auto configPath = fixture.filePath("lua-save.tsv");
    const auto config = QByteArray("schema\tforge-lua-map-save-v1\nprofile\t") + profilePath.toUtf8()
        + '\t' + QByteArray::fromStdString(*profileHash.value) + "\nchecker\t"
        + QString::fromStdWString(checker.wstring()).toUtf8() + '\t' + QByteArray::fromStdString(*checkerHash.value)
        + "\nplayer_layout\tclassic12\nlanguage\tselect-lua\nruntime\t"
        + QString::fromStdWString(checker.parent_path().wstring()).toUtf8() + '\n';
    if (!WriteText(configPath, config)) return fail("write explicit Lua save configuration");
    configuration = NativePath(configPath);
    qInfo().noquote() << "Lua Qt original fixture root=" << fixture.path()
        << "source=" << QString::fromStdWString(source.wstring()) << "ui=" << definitions << "configuration=" << configPath;
    return true;
}

void NativeWorkbenchTests::ScriptSaveUiAndCliUseSameMembers() {
    const QByteArray mapPath = qgetenv("FORGE_D1_PARITY_MAP");
    const QByteArray ydweRoot = qgetenv("FORGE_YDWE_ROOT");
    const QByteArray gameRoot = qgetenv("FORGE_WAR3_ROOT");
    if (mapPath.isEmpty() || ydweRoot.isEmpty() || gameRoot.isEmpty()) {
        QSKIP("Set FORGE_D1_PARITY_MAP, FORGE_YDWE_ROOT and FORGE_WAR3_ROOT for the real-map parity probe");
    }
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto path = [](const QByteArray& value) {
        return std::filesystem::path(QString::fromLocal8Bit(value).toStdWString());
    };
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto uiCandidate = NativePath(fixture.filePath("ui.w3x"));
    const auto cliCandidate = NativePath(fixture.filePath("cli.w3x"));
    std::error_code copyError;
    QVERIFY(std::filesystem::copy_file(path(mapPath), source, copyError) && !copyError);
    auto sourceArchive = War3Map::MapArchiveSession::Open(source); QVERIFY(sourceArchive.Succeeded());
    auto terrain = sourceArchive.value->ReadMember("war3map.w3e"); QVERIFY(terrain.Succeeded());
    auto terrainSha = War3Map::Sha256(*terrain.value); QVERIFY(terrainSha.Succeeded());

    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(path(ydweRoot)));
    QVERIFY(window.OpenGameResources(path(gameRoot)));
    QVERIFY(window.OpenMapArchivePath(source));
    auto* edits = window.project_->MapEdits(); QVERIFY(edits && edits->Terrain() && window.project_->uiCatalog_);
    constexpr std::uint32_t x = 10, y = 10, radius = 2;
    const auto grid = edits->Terrain();
    const War3Map::MapData::TerrainStrokePoint center{static_cast<double>(grid->header.offset[0]) + x * 128.0,
        static_cast<double>(grid->header.offset[1]) + y * 128.0};
    War3Map::MapData::TerrainBrush brush;
    brush.kind = War3Map::MapData::TerrainBrushKind::Blight;
    brush.shape = War3Map::MapData::TerrainBrushShape::Circle;
    brush.radius = radius;
    brush.value = true;
    auto stroke = edits->EditTerrain(brush, std::span(&center, 1), false);
    QVERIFY(stroke.Succeeded() && stroke.value && !stroke.value->Empty());
    QVERIFY(window.SaveMapArchiveToPath(uiCandidate));

    War3Map::Services::TerrainBlightSaveRequest request;
    request.sourceMap = source;
    request.targetCandidate = cliCandidate;
    request.toolRoot = path(ydweRoot);
    request.gameRoot = path(gameRoot);
    request.expectedSourceSha256 = sourceArchive.value->SourceSha256();
    request.expectedTerrainSha256 = *terrainSha.value;
    request.effectiveLayers = window.project_->uiCatalog_->Layers();
    request.vertexX = x; request.vertexY = y; request.radius = radius;
    auto saved = War3Map::Services::SaveTerrainBlightCandidate(request);
    QVERIFY(saved.Succeeded() && saved.value);
    auto ui = War3Map::MapArchiveSession::Open(uiCandidate);
    auto cli = War3Map::MapArchiveSession::Open(cliCandidate);
    QVERIFY(ui.Succeeded() && cli.Succeeded());
    QVERIFY(ui.value->Members().size() == cli.value->Members().size());
    for (std::size_t index = 0; index < ui.value->Members().size(); ++index) {
        QVERIFY(ui.value->Members()[index].name == cli.value->Members()[index].name);
        QVERIFY(ui.value->Members()[index].data == cli.value->Members()[index].data);
    }
}

void NativeWorkbenchTests::NativeWorldFacingChangesSaveAndReopen() {
    using namespace War3Map;
    const auto map = qEnvironmentVariable("FORGE_R08_WORLD_MAP");
    if (map.isEmpty()) QSKIP("Set FORGE_R08_WORLD_MAP for the explicit real-map world UI save probe");
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
        QSKIP("Real-map UI probe requires Qt offscreen; desktop windows are not allowed");
    const auto ydwe = qEnvironmentVariable("FORGE_YDWE_ROOT"), game = qEnvironmentVariable("FORGE_WAR3_ROOT");
    QVERIFY2(!ydwe.isEmpty() && !game.isEmpty(), "Real save requires the existing verified YDWE/game toolchain roots");
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto evidence = qEnvironmentVariable("FORGE_R08_WORLD_EVIDENCE_DIR", temporary.path());
    QVERIFY(QDir().mkpath(evidence));
    const auto source = NativePath(map), candidate = NativePath(QDir(evidence).filePath("world-facing-candidate.w3x"));
    QVERIFY2(!std::filesystem::exists(candidate), "Candidate already exists; preserving it without overwrite");
    const auto sourceBytes = ReadFile(source); QVERIFY(!sourceBytes.isEmpty());
    auto originalArchive = MapArchiveSession::Open(source); QVERIFY(originalArchive.Succeeded());
    const auto sourceSha = originalArchive.value->SourceSha256();
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(NativePath(ydwe))); QVERIFY(window.OpenGameResources(NativePath(game)));
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->mapEdits_ && window.project_->standardCatalog_ && window.project_->mapDataUndoStack_);
    const auto original = window.project_->mapEdits_->Snapshot(); QVERIFY(original.Units());
    QVERIFY2(original.Member(MapDataKind::Units).state == MapDataState::Ready,
        "The explicit edit/save probe needs a complete writable unit member; partial sources remain read-only");
    const auto* facingField = MapData::FindMapField(MapDataKind::Units, "facing");
    QVERIFY(facingField && facingField->writable && MapData::MapFieldLayoutAllowsWrite(original, *facingField));
    const auto& units = original.Units()->units;
    const auto found = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
        return unit.rawcode != "sloc" && std::isfinite(unit.rotation)
            && window.project_->standardCatalog_->Find(ObjectType::Unit, unit.rawcode)
            && std::all_of(unit.position.begin(), unit.position.end(), [](float value) { return std::isfinite(value) && std::abs(value) <= 1000000.0f; });
    });
    QVERIFY2(found != units.end(), "Real map has no editable native unit placement");
    const auto index = static_cast<std::uint32_t>(found - units.begin());
    const auto id = MapData::MakePlacementId(MapData::PlacementFile::Units, index, *found);
    auto* world = dynamic_cast<Modules::WorldModule*>(window.modules_.Find("world")); QVERIFY(world);
    auto* page = world->Widget(); page->resize(1300, 900); page->layout()->activate();
    class HideRenderViewports final : public QObject {
    public:
        bool rendererObserved = false;
        bool eventFilter(QObject* object, QEvent* event) override {
            auto* view = static_cast<Modules::WorldViewport*>(object); rendererObserved |= view->RendererReady();
            if (event->type() != QEvent::Show) return false;
            view->hide(); return true;
        }
    } renderGuard;
    const auto renderViews = page->findChildren<Modules::WorldViewport*>(); QVERIFY(!renderViews.isEmpty());
    for (auto* view : renderViews) { view->hide(); view->installEventFilter(&renderGuard); }
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.ExecuteCommand("module.world"); window.show();
    QTRY_VERIFY_WITH_TIMEOUT(page->isVisible() && world->Viewport()->VertexCount() != 0, 60000);
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<Modules::WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    for (int fetch = 0; fetch < 64 && !model->Find(id).isValid() && model->canFetchMore({}); ++fetch) model->fetchMore({});
    QTRY_VERIFY_WITH_TIMEOUT(model->Find(id).isValid(), 60000);
    outliner->expandAll(); outliner->doItemsLayout(); outliner->scrollTo(model->Find(id));
    QVERIFY(outliner->visualRect(model->Find(id)).isValid());
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, Qt::NoModifier, outliner->visualRect(model->Find(id)).center());
    auto* facing = page->findChild<QDoubleSpinBox*>("worldFieldFacing"); QVERIFY(facing);
    QTRY_VERIFY_WITH_TIMEOUT(facing->isEnabled(), 60000);
    QVERIFY(!world->Viewport()->RendererReady()); // Qt 离屏页布局完成；渲染子窗口始终隐藏，只做 CPU/UI 事务。
    auto* input = facing->findChild<QLineEdit*>(); QVERIFY(input);
    const double degrees = std::fmod(std::fmod(double(found->rotation) * 180.0 / std::numbers::pi_v<double> + 15.0, 360.0) + 360.0, 360.0);
    const auto history = window.project_->mapDataUndoStack_->count();
    QVERIFY(facing->isVisible());
    facing->setFocus(); QTest::keyClick(facing, Qt::Key_A, Qt::ControlModifier);
    const auto entered = QString::number(degrees, 'f', 1);
    QTest::keyClicks(facing, entered);
    qInfo() << "R08 facing input" << input->text() << facing->value() << facing->property("worldEdited")
        << facing->property("r08SelectionRevision") << facing->property("r08MapRevision")
        << facing->property("r08MapGeneration");
    QVERIFY(input->text().startsWith(entered)); QTest::keyClick(facing, Qt::Key_Return);
    QStringList fieldStatus;
    if (auto* bar = page->findChild<QWidget*>("worldStatusBar"))
        for (auto* label : bar->findChildren<QLabel*>()) fieldStatus << label->text();
    qInfo() << "R08 facing result" << input->text() << facing->value() << fieldStatus
        << "map revision" << window.project_->mapEdits_->Revision();
    QTRY_COMPARE(window.project_->mapDataUndoStack_->count(), history + 1);
    const auto changed = MapData::FindPlacement(window.project_->mapEdits_->Snapshot(), id); QVERIFY(changed.Succeeded());
    QCOMPARE(changed.value->position, found->position);
    QVERIFY(std::bit_cast<std::uint32_t>(changed.value->rotation) != std::bit_cast<std::uint32_t>(found->rotation));
    const auto images = qEnvironmentVariable("FORGE_R08_WORLD_NATIVE_SCREENSHOT_DIR");
    if (!images.isEmpty()) {
        QVERIFY(QDir().mkpath(images));
        QVERIFY(QFontMetrics(QApplication::font()).inFont(QChar{u'地'}));
        for (const auto size : {QSize{1920, 1080}, QSize{2560, 1440}}) {
            window.resize(size); QTest::qWait(20); QCOMPARE(window.size(), size);
            QVERIFY(window.grab().save(QDir(images).filePath(QStringLiteral("world-native-ui-%1x%2-offscreen.png").arg(size.width()).arg(size.height()))));
        }
    }
    window.ExecuteCommand("project.changes"); QCOMPARE(window.RefreshCommandMapSnapshot(), QString{});
    const auto binding = window.commandMapSnapshot_->Binding();
    const Services::ProjectChangesContext context{window.project_->mapDocument_.get(), window.commandMapDocument_.get(), binding};
    const auto changes = Services::QueryProjectChanges(context, binding, MapDataKind::Units); QVERIFY(changes.Succeeded());
    const auto row = std::find_if(changes.value->rows.begin(), changes.value->rows.end(), [index](const auto& entry) {
        return entry.fieldId == "facing" && entry.currentIndex == index;
    });
    QVERIFY(row != changes.value->rows.end() && row->before && row->after);
    QCOMPARE(row->before->floatBits, std::bit_cast<std::uint32_t>(found->rotation));
    QCOMPARE(row->after->floatBits, std::bit_cast<std::uint32_t>(changed.value->rotation));
    auto* tree = window.changesPanel_->findChild<QTreeView*>("changesTree"); QVERIFY(tree);
    QCOMPARE(tree->model()->match(tree->model()->index(0, 0), Qt::UserRole, QString::fromStdString(row->id), 1,
        Qt::MatchExactly | Qt::MatchRecursive).size(), 1);
    qInfo() << "R08 world native source SHA" << QString::fromStdString(sourceSha) << "candidate" << QString::fromStdWString(candidate.wstring());
    QVERIFY(window.SaveMapArchiveToPath(candidate));
    auto saved = MapArchiveSession::Open(candidate); QVERIFY(saved.Succeeded());
    const auto reopened = MapData::FindPlacement(MapDocument::FromArchive(*saved.value), id); QVERIFY(reopened.Succeeded());
    QCOMPARE(reopened.value->position, changed.value->position);
    QCOMPARE(std::bit_cast<std::uint32_t>(reopened.value->rotation), std::bit_cast<std::uint32_t>(changed.value->rotation));
    QCOMPARE(ReadFile(source), sourceBytes);
    auto sourceAfter = MapArchiveSession::Open(source); QVERIFY(sourceAfter.Succeeded()); QCOMPARE(sourceAfter.value->SourceSha256(), sourceSha);
    for (auto* view : renderViews) { QVERIFY(!view->isVisible()); renderGuard.rendererObserved |= view->RendererReady(); }
    QVERIFY(!renderGuard.rendererObserved);
    const QJsonObject receipt{{"scope", "offscreen-world-ui-save-reopen"}, {"source", map},
        {"source_sha256", QString::fromStdString(sourceSha)}, {"candidate", QString::fromStdWString(candidate.wstring())},
        {"candidate_sha256", QString::fromStdString(saved.value->SourceSha256())}, {"unit_index", QString::number(index)},
        {"rawcode", QString::fromStdString(id.rawcode)}, {"creation_id", QString::number(id.creationId)},
        {"before_facing_raw", QString::fromStdString(MapData::FormatMapFieldRaw(*row->before))},
        {"after_facing_raw", QString::fromStdString(MapData::FormatMapFieldRaw(*row->after))},
        {"source_unchanged", true}, {"renderer_ready_observed", renderGuard.rendererObserved},
        {"render_viewports_hidden", std::all_of(renderViews.begin(), renderViews.end(), [](auto* view) { return !view->isVisible(); })},
        {"original_editor_verified", false}, {"runtime_verified", false}};
    QVERIFY(WriteText(QDir(evidence).filePath("world-facing-receipt.json"), QJsonDocument(receipt).toJson(QJsonDocument::Indented)));
}

void NativeWorkbenchTests::NativeWorldAnimationProducesCompletedFrames() {
    using namespace War3Map;
    const auto evidence = qEnvironmentVariable("FORGE_R08_WORLD_ANIMATION_EVIDENCE_DIR");
    if (evidence.isEmpty()) QSKIP("Explicit native animation evidence probe is not enabled");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    const auto map = qEnvironmentVariable("FORGE_R08_WORLD_MAP");
    const auto ydwe = qEnvironmentVariable("FORGE_YDWE_ROOT"), game = qEnvironmentVariable("FORGE_WAR3_ROOT");
    QVERIFY2(!map.isEmpty() && !ydwe.isEmpty() && !game.isEmpty(), "Supply a real map copy and the verified resource roots");
    QVERIFY(QDir().mkpath(evidence));
    const auto receiptPath = QDir(evidence).filePath("world-animation-receipt.json");
    QVERIFY2(!QFileInfo::exists(receiptPath), "Preserving existing evidence");
    const auto inputSha = Sha256File(NativePath(map)); QVERIFY(inputSha.Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    window.resize(1600, 900);
    QVERIFY(window.UseUiDataRoot(NativePath(ydwe))); QVERIFY(window.OpenGameResources(NativePath(game)));
    QVERIFY(window.OpenMapArchivePath(NativePath(map)));
    QVERIFY(window.project_->mapEdits_ && window.project_->mapDataUndoStack_);
    auto* world = dynamic_cast<Modules::WorldModule*>(window.modules_.Find("world")); QVERIFY(world);
    auto* page = world->Widget(); auto* viewport = world->Viewport(); QVERIFY(viewport);
    auto* player = world->findChild<Modules::WorldAnimationPlayer*>(); QVERIFY(player);
    const auto documentRevision = window.project_->editRevision_.Revision();
    const auto mapRevision = window.project_->mapEdits_->Revision();
    const auto undoCount = window.project_->mapDataUndoStack_->count();
    window.ExecuteCommand("module.world"); window.show();
    // 保留生产默认浏览器/缩略图路径；首帧前不点击播放，也不将离屏成功当窗口证据。
    auto* modelStatus = page->findChild<QLabel*>("worldModelStatus"); QVERIFY(modelStatus);
    const auto animationReady = [&] { return viewport->isVisible() && viewport->RendererReady()
        && player->Ready() && player->Playing() && player->Frame() && player->Frame()->animatedInstances > 0; };
    QElapsedTimer readyWait; readyWait.start(); bool terminalSceneFailure = false;
    // QTRY 宏失败后会追加重试；这里仅一次 60s 等待，明确失败立即终止，保留原验收条件。
    while (!animationReady() && readyWait.elapsed() < 60000) {
        const auto status = modelStatus->text();
        terminalSceneFailure = status.contains(QStringLiteral("动画场景加载失败"))
            || status.startsWith(QStringLiteral("模型上传失败：")) || viewport->RendererFailed();
        if (terminalSceneFailure) break;
        QTest::qWait(20);
    }
    const bool ready = animationReady();
    if (!ready) {
        const auto failureImagePath = QDir(evidence).filePath("world-native-window-failure.png");
        bool failureImageSaved = false;
        if (window.screen() && !QFileInfo::exists(failureImagePath)) {
            const auto image = window.screen()->grabWindow(window.winId());
            failureImageSaved = !image.isNull() && image.save(failureImagePath);
        }
        QJsonArray diagnostics;
        auto* diagnosticModel = window.project_->diagnosticsModel_;
        const int diagnosticCount = diagnosticModel ? diagnosticModel->rowCount() : 0;
        for (int row = std::max(0, diagnosticCount - 128); row < diagnosticCount; ++row) {
            QJsonArray cells; bool truncated = false;
            for (int column = 0; column < std::min(4, diagnosticModel->columnCount()); ++column) {
                const auto text = diagnosticModel->index(row, column).data(Qt::DisplayRole).toString();
                truncated |= text.size() > 8192; cells.append(text.left(8192));
            }
            diagnostics.append(QJsonObject{{"source_row", row}, {"cells", cells}, {"text_truncated", truncated}});
        }
        const auto frame = player->Frame(); const auto scene = player->Scene();
        const QJsonObject failure{{"scope", "windows-production-world-animation-first-frame-failure"}, {"source", map},
            {"source_sha256", QString::fromStdString(*inputSha.value)}, {"wait_ms", readyWait.elapsed()},
            {"terminal_scene_failure", terminalSceneFailure}, {"model_status", modelStatus->text()},
            {"model_status_tooltip", modelStatus->toolTip()}, {"player_ready", player->Ready()},
            {"player_playing", player->Playing()}, {"player_busy", player->Busy()},
            {"scene_present", bool(scene)}, {"scene_animated_instances", scene ? int(scene->animatedInstances) : 0},
            {"frame_present", bool(frame)}, {"frame_animated_instances", frame ? int(frame->animatedInstances) : 0},
            {"frame_scene_version", QString::number(frame ? frame->sceneVersion : 0)},
            {"frame_content_version", QString::number(frame ? frame->contentVersion : 0)},
            {"viewport_visible", viewport->isVisible()}, {"renderer_ready", viewport->RendererReady()},
            {"renderer_failed", viewport->RendererFailed()}, {"viewport_status", viewport->StatusText()},
            {"terrain_vertices", QString::number(viewport->VertexCount())}, {"window_image", failureImagePath},
            {"window_image_saved", failureImageSaved}, {"window_images_composited", false},
            {"project_diagnostics", diagnostics}, {"project_diagnostic_rows_omitted", std::max(0, diagnosticCount - 128)}};
        qInfo().noquote() << "World animation first-frame failure:" << modelStatus->text() << modelStatus->toolTip()
            << "wait_ms" << readyWait.elapsed() << "player ready/playing/busy" << player->Ready() << player->Playing() << player->Busy();
        const auto failurePath = QDir(evidence).filePath("world-animation-first-frame-failure.json");
        if (!WriteText(failurePath, QJsonDocument(failure).toJson(QJsonDocument::Indented)))
            qWarning() << "Could not preserve first-frame diagnostics" << failurePath;
    }
    QVERIFY2(ready, "Production animation not ready: first-frame diagnostic JSON/window image written before teardown");
    QTest::qWait(200); // 给已提交的原生窗口和 Qt 控件完成首轮实际呈现，不捕获上一张加载页。
    QVERIFY(player->Ready() && player->Playing());
    std::array<Render::CapturedFrameRgba, 2> frames;
    QJsonArray samples;
    bool nativeChildNonblank = true;
    auto poseChanged = [&](const std::shared_ptr<const Render::AnimationFrame>& next) {
        if (!next || !frames[0].animation || next->sceneVersion != frames[0].animation->sceneVersion) return false;
        const auto& previous = frames[0].animation->poses;
        return std::any_of(next->poses.begin(), next->poses.end(), [&](const auto& pose) {
            if (!pose.animated || pose.hidden) return false;
            const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) { return entry.identity == pose.identity; });
            return old != previous.end() && old->animated && !old->hidden && old->matrixHash != pose.matrixHash;
        });
    };
    auto poseJson = [](const Render::AnimationPoseLink& pose) {
        return QJsonObject{{"instance", QString::number(pose.identity.value)}, {"generation", QString::number(pose.identity.generation)},
            {"asset", QString::number(pose.asset.value)}, {"asset_generation", QString::number(pose.asset.generation)},
            {"request_index", int(pose.requestIndex)}, {"pose_version", QString::number(pose.poseVersion)},
            {"evaluation_epoch", QString::number(pose.evaluationEpoch)}, {"matrix_hash", QString::number(pose.matrixHash)},
            {"alpha_hash", QString::number(pose.alphaHash)}, {"color_hash", QString::number(pose.colorHash)}};
    };
    for (int sample = 0; sample < 2; ++sample) {
        if (sample) {
            QTest::qWait(200);
            QTRY_VERIFY_WITH_TIMEOUT(poseChanged(player->Frame()), 10000);
        }
        const auto hostFrame = player->Frame(); QVERIFY(hostFrame);
        QVERIFY(window.screen());
        const auto windowImage = window.screen()->grabWindow(window.winId()).toImage();
        QVERIFY(!windowImage.isNull());
        const auto windowPath = QDir(evidence).filePath(QStringLiteral("world-native-window-%1.png").arg(sample + 1));
        const auto viewportPath = QDir(evidence).filePath(QStringLiteral("world-completed-viewport-%1.png").arg(sample + 1));
        QVERIFY(!QFileInfo::exists(windowPath) && !QFileInfo::exists(viewportPath));
        QVERIFY(windowImage.save(windowPath));
        // 捕获本实例的真实 HWND；缺失原生 D3D 子窗口时禁止拼入 RGBA 冒充窗口截图。
        const auto origin = viewport->mapTo(&window, QPoint{}); const auto dpr = windowImage.devicePixelRatio();
        const auto viewImage = windowImage.copy(QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
            qRound(viewport->width() * dpr), qRound(viewport->height() * dpr)));
        int minimum = 765, maximum = 0;
        for (int y = viewImage.height() / 4; y < viewImage.height() * 3 / 4; y += std::max(1, viewImage.height() / 32))
            for (int x = viewImage.width() / 4; x < viewImage.width() * 3 / 4; x += std::max(1, viewImage.width() / 32)) {
                const auto pixel = viewImage.pixel(x, y); const int brightness = qRed(pixel) + qGreen(pixel) + qBlue(pixel);
                minimum = std::min(minimum, brightness); maximum = std::max(maximum, brightness);
            }
        const bool nonblank = maximum > 24 && maximum - minimum > 24; nativeChildNonblank &= nonblank;
        auto completed = viewport->CaptureCompletedFrame(viewport->DocumentRevision());
        for (const auto& diagnostic : completed.diagnostics) qInfo() << QString::fromStdString(diagnostic.code) << QString::fromStdString(diagnostic.message);
        QVERIFY(completed.Succeeded()); frames[sample] = std::move(*completed.value);
        const auto& frame = frames[sample]; QVERIFY(frame.animation && frame.skin);
        QCOMPARE(frame.skin->sceneVersion, frame.animation->sceneVersion); QCOMPARE(frame.skin->contentVersion, frame.animation->contentVersion);
        QCOMPARE(hostFrame->sceneVersion, frame.animation->sceneVersion); QCOMPARE(hostFrame->contentVersion, frame.animation->contentVersion);
        QCOMPARE(frame.skin->frameCount, frame.view.identity.frameCount);
        const QImage rgba(reinterpret_cast<const uchar*>(frame.rgba.data()), int(frame.view.physicalWidth),
            int(frame.view.physicalHeight), int(frame.view.physicalWidth * 4), QImage::Format_RGBA8888);
        QVERIFY(rgba.save(viewportPath));
        QJsonArray poses;
        for (const auto& pose : frame.animation->poses) {
            if (pose.animated && !pose.hidden && poses.size() < 128) poses.append(poseJson(pose));
        }
        QJsonObject row{{"window_image", windowPath}, {"viewport_image", viewportPath}, {"window_native_child_nonblank", nonblank},
            {"window_exact_gpu_frame_known", false}, {"window_host_pose_unchanged_across_grab", true},
            {"document_revision", QString::number(frame.documentRevision)}, {"frame_count", QString::number(frame.view.identity.frameCount)},
            {"surface_epoch", QString::number(frame.view.identity.surfaceEpoch)}, {"view_epoch", QString::number(frame.view.identity.viewEpoch)},
            {"scene_epoch", QString::number(frame.view.identity.sceneEpoch)}, {"physical_width", int(frame.view.physicalWidth)},
            {"physical_height", int(frame.view.physicalHeight)}, {"camera", QJsonObject{{"target_x", frame.camera.targetX},
                {"target_y", frame.camera.targetY}, {"target_z", frame.camera.targetZ}, {"distance", frame.camera.distance},
                {"yaw", frame.camera.yawRadians}, {"pitch", frame.camera.pitchRadians}}},
            {"scene_version", QString::number(frame.animation->sceneVersion)}, {"content_version", QString::number(frame.animation->contentVersion)},
            {"animated_instances", int(frame.animation->animatedInstances)}, {"pose_rows", poses},
            {"pose_rows_omitted", int(frame.animation->animatedInstances) - poses.size()}};
        if (frame.presentation) {
            row.insert("model_epoch", QString::number(frame.presentation->modelEpoch));
            row.insert("presentation_revision", QString::number(frame.presentation->presentationRevision));
        }
        samples.append(row);
    }
    QVERIFY(poseChanged(frames[1].animation));
    QJsonObject changedPose;
    for (const auto& pose : frames[1].animation->poses) {
        if (!pose.animated || pose.hidden) continue;
        const auto& previous = frames[0].animation->poses;
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) { return entry.identity == pose.identity; });
        if (old != previous.end() && old->animated && !old->hidden && old->matrixHash != pose.matrixHash) {
            changedPose = {{"before", poseJson(*old)}, {"after", poseJson(pose)}}; break;
        }
    }
    const bool pixelsChanged = frames[0].rgba != frames[1].rgba;
    auto* pause = page->findChild<QPushButton*>("worldAnimationPause"); QVERIFY(pause && pause->isEnabled());
    pause->click(); QTRY_VERIFY_WITH_TIMEOUT(!player->Playing() && !player->Busy(), 10000);
    const auto paused = player->Frame(); QVERIFY(paused);
    window.ExecuteCommand("module.assets"); QTRY_VERIFY(!viewport->isVisible());
    window.ExecuteCommand("module.world");
    QTRY_VERIFY_WITH_TIMEOUT(viewport->isVisible() && viewport->RendererReady() && player->Ready() && !player->Busy(), 10000);
    QTest::qWait(150); QVERIFY(!player->Playing());
    const auto restored = player->Frame(); QVERIFY(restored); QCOMPARE(restored->states.size(), paused->states.size());
    for (std::size_t index = 0; index < paused->states.size(); ++index) {
        QVERIFY(restored->states[index].identity == paused->states[index].identity);
        QCOMPARE(restored->states[index].elapsedMs, paused->states[index].elapsedMs);
        QCOMPARE(restored->states[index].globalMs, paused->states[index].globalMs);
    }
    const auto afterSha = Sha256File(NativePath(map)); QVERIFY(afterSha.Succeeded()); QCOMPARE(*afterSha.value, *inputSha.value);
    QCOMPARE(window.project_->editRevision_.Revision(), documentRevision);
    QCOMPARE(window.project_->mapEdits_->Revision(), mapRevision); QCOMPARE(window.project_->mapDataUndoStack_->count(), undoCount);
    const QJsonObject receipt{{"scope", "windows-production-world-animation-read-only"}, {"source", map},
        {"source_sha256", QString::fromStdString(*inputSha.value)}, {"source_unchanged", true}, {"quality", "PreviewApproximate"},
        {"autoplay_without_button", true}, {"pause_survives_hide_restore", true}, {"completed_viewport_pixels_changed", pixelsChanged},
        {"window_native_child_nonblank", nativeChildNonblank}, {"samples", samples}, {"changed_pose", changedPose},
        {"window_images_composited", false}, {"original_editor_verified", false}, {"runtime_verified", false}};
    QVERIFY(WriteText(receiptPath, QJsonDocument(receipt).toJson(QJsonDocument::Indented)));
    QVERIFY2(nativeChildNonblank, "Native window capture omitted/blanked the D3D child; viewport PNGs are separate GPU evidence, not substituted window evidence");
    QVERIFY2(pixelsChanged, "Pose changes produced no pixel difference in the completed viewport frames");
}

#include "NativeMapDropChecks.h"
#include "NativeWorldPlacementChecks.h"

void NativeWorkbenchTests::MapOpenRetainsNativeBaselineAndCurrentSource() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto first = NativePath(fixture.filePath(QStringLiteral("first.w3x")));
    const auto second = NativePath(fixture.filePath(QStringLiteral("second.w3x")));
    QVERIFY(MakeArchive(first, 420).Succeeded());
    QVERIFY(MakeArchive(second, 731).Succeeded());
    const QByteArray firstSource = ReadFile(first);
    const QByteArray secondSource = ReadFile(second);
    QVERIFY(!firstSource.isEmpty());
    QVERIFY(!secondSource.isEmpty());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    int boundLoads = 0;
    bool prematureDocument = false;
    QObject::connect(window.project_.get(), &War3Map::QtProject::EditorProject::MapDocumentChanged, &window, [&] {
        if (!window.project_->mapDocument_) return;
        ++boundLoads;
        prematureDocument |= window.project_->sourceArchivePath_.empty() || !window.project_->archiveSession_;
    });
    QVERIFY(window.OpenMapArchivePath(first));
    QVERIFY(!prematureDocument);
    QCOMPARE(boundLoads, 1);
    for (int row = 0; row < window.project_->diagnosticsModel_->rowCount(); ++row)
        QVERIFY(window.project_->diagnosticsModel_->item(row, 2)->text() != QStringLiteral("LNI_FILE_MISSING"));
    QVERIFY(window.project_->sourceArchivePath_ == first);
    QVERIFY(window.project_->nativeObjectDocuments_[0] != nullptr);
    QVERIFY(window.project_->objectDocuments_[0] != nullptr);
    QCOMPARE(window.project_->nativeObjectDocuments_[0]->Version(), 2);
    QCOMPARE(window.project_->nativeObjectDocuments_[0]->OriginalObjectCount(), std::size_t{1});
    QCOMPARE(window.project_->nativeObjectDocuments_[0]->CustomObjectCount(), std::size_t{0});
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 420);
    const auto serializedFirst = NativeObjectCodec(ObjectType::Unit).Serialize(*window.project_->nativeObjectDocuments_[0]);
    QVERIFY(serializedFirst.Succeeded());
    QVERIFY(*serializedFirst.value == MinimalUnitData(420));
    const auto firstWorkspace = window.project_->projectRoot_;
    QVERIFY(std::filesystem::is_regular_file(firstWorkspace / "table" / "unit.ini"));

    QVERIFY(window.OpenMapArchivePath(second));
    QVERIFY(!prematureDocument);
    QCOMPARE(boundLoads, 2);
    QVERIFY(window.project_->sourceArchivePath_ == second);
    QVERIFY(window.project_->projectRoot_ != firstWorkspace);
    QVERIFY(window.project_->nativeObjectDocuments_[0] != nullptr);
    QVERIFY(window.project_->objectDocuments_[0] != nullptr);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 731);
    const auto serializedSecond = NativeObjectCodec(ObjectType::Unit).Serialize(*window.project_->nativeObjectDocuments_[0]);
    QVERIFY(serializedSecond.Succeeded());
    QVERIFY(*serializedSecond.value == MinimalUnitData(731));
    for (std::size_t index = 1; index < window.project_->nativeObjectDocuments_.size(); ++index) {
        QVERIFY(window.project_->nativeObjectDocuments_[index] == nullptr);
    }
    QCOMPARE(ReadFile(first), firstSource);
    QCOMPARE(ReadFile(second), secondSource);
}

void NativeWorkbenchTests::ResetClearsArchiveImportsAndVisibleList() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto map = NativePath(fixture.filePath(QStringLiteral("reset.w3x")));
    QVERIFY(MakeArchive(map, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(map));
    window.project_->pendingImports_.push_back({QStringLiteral("test/input.txt"), fixture.filePath(QStringLiteral("input.txt")), 7});
    window.project_->NotifyImportsChanged();
    QVERIFY(window.assetsModule_->Widget() != nullptr);
    QCOMPARE(window.assetsModule_->PendingCount(), 1);
    QVERIFY(window.assetsModule_->PendingPaths().front().contains(QStringLiteral("test/input.txt")));
    QVERIFY(!window.project_->sourceArchivePath_.empty());
    window.ResetProjectState();
    QVERIFY(window.project_->sourceArchivePath_.empty());
    QVERIFY(window.project_->projectRoot_.empty());
    QVERIFY(window.project_->pendingImports_.empty());
    QCOMPARE(window.assetsModule_->PendingCount(), 0);
    QCOMPARE(window.objectsModule_->objectListModel_->rowCount(), 0);
    QCOMPARE(window.objectsModule_->objectFieldModel_->rowCount(), 0);
    for (const auto& document : window.project_->objectDocuments_) {
        QVERIFY(document == nullptr);
    }
    for (const auto& document : window.project_->nativeObjectDocuments_) {
        QVERIFY(document == nullptr);
    }
    QVERIFY(std::none_of(window.project_->objectDirty_.begin(), window.project_->objectDirty_.end(), [](bool dirty) { return dirty; }));
}

void NativeWorkbenchTests::OpeningLniDoesNotInheritPreviousNativeBaseline() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto map = NativePath(fixture.filePath(QStringLiteral("previous.w3x")));
    QVERIFY(MakeArchive(map, 420).Succeeded());
    const QByteArray source = ReadFile(map);
    const QString nextRoot = fixture.filePath(QStringLiteral("next-workspace"));
    QVERIFY(WriteText(nextRoot + QStringLiteral("/table/unit.ini"), "[hrif]\nuhpm = 975\nunam = \"Separate LNI\"\n"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(map));
    QVERIFY(window.project_->nativeObjectDocuments_[0] != nullptr);
    QVERIFY(window.OpenWorkspacePath(NativePath(nextRoot)));
    QVERIFY(window.project_->projectRoot_ == NativePath(nextRoot));
    QVERIFY(window.project_->sourceArchivePath_.empty());
    QVERIFY(window.project_->pendingImports_.empty());
    QCOMPARE(window.assetsModule_->PendingCount(), 0);
    for (const auto& baseline : window.project_->nativeObjectDocuments_) {
        QVERIFY(baseline == nullptr);
    }
    QVERIFY(window.project_->objectDocuments_[0] != nullptr);
    QCOMPARE(window.project_->objectDocuments_[0]->Objects().size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(window.project_->objectDocuments_[0]->Objects().front().key.rawcode), QStringLiteral("hrif"));
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 975);
    QCOMPARE(ReadFile(map), source);
}

void NativeWorkbenchTests::StandardBrowsingIsReadOnlyAndHiddenSelectionKeepsMapTitle() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString lni = fixture.filePath(QStringLiteral("table/unit.ini"));
    QVERIFY(WriteText(lni, "[hfoo]\nuhpm = 420\nunam = \"Map override name\"\n"));
    const QByteArray source = ReadFile(NativePath(lni));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(fixture.path())));
    auto loaded = SyntheticCatalog();
    QVERIFY(loaded.Succeeded());
    QCOMPARE(loaded.value->Count(ObjectType::Unit), std::size_t{2});
    std::array<const LniObjectDocument*, 7> pointers{};
    std::array<std::string, 7> fingerprints{};
    for (std::size_t index = 0; index < pointers.size(); ++index) {
        pointers[index] = window.project_->objectDocuments_[index].get();
        if (pointers[index]) {
            fingerprints[index] = pointers[index]->DocumentFingerprint();
        }
    }
    window.project_->standardCatalog_ = std::make_unique<StandardObjectCatalog>(std::move(*loaded.value));
    window.objectsModule_->UpdateObjectReferenceEntries();
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(0);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hfoo"));
    QCOMPARE(window.objectsModule_->objectPreviewName_->text(), QStringLiteral("Synthetic standard unit"));
    QVERIFY(window.objectsModule_->standardFieldModel_->rowCount() > 0);
    auto* standardView = qobject_cast<QTableView*>(window.objectsModule_->objectDetailPages_->widget(0));
    QVERIFY(standardView);
    QCOMPARE(standardView->editTriggers(), QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers));
    window.objectsModule_->standardObjectBrowser_->SetSearchText(QStringLiteral("hrif"));
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hrif"));
    QCOMPARE(window.objectsModule_->objectPreviewName_->text(), QStringLiteral("Synthetic other unit"));
    QCOMPARE(window.project_->objectDocuments_[0]->Objects().size(), std::size_t{1});

    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    const QString mapTitle = window.objectsModule_->objectPreviewName_->text();
    const QString mapSource = window.objectsModule_->objectPreviewSource_->text();
    QCOMPARE(mapTitle, QStringLiteral("Map override name"));
    window.objectsModule_->standardObjectBrowser_->SetSearchText({});
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hfoo"));
    const auto hiddenStandard = window.objectsModule_->standardObjectBrowser_->CurrentEntry();
    QVERIFY(hiddenStandard.has_value());
    window.objectsModule_->ShowStandardObject(*hiddenStandard);
    QCOMPARE(window.objectsModule_->objectPreviewName_->text(), mapTitle);
    QCOMPARE(window.objectsModule_->objectPreviewSource_->text(), mapSource);
    QCOMPARE(window.objectsModule_->objectDetailPages_->currentIndex(), 1);
    for (std::size_t index = 0; index < pointers.size(); ++index) {
        QCOMPARE(window.project_->objectDocuments_[index].get(), pointers[index]);
        if (pointers[index]) {
            QVERIFY(pointers[index]->DocumentFingerprint() == fingerprints[index]);
        }
        QVERIFY(!window.project_->objectDirty_[index]);
        QVERIFY(window.project_->nativeObjectDocuments_[index] == nullptr);
    }
    QCOMPARE(ReadFile(NativePath(lni)), source);
}

void NativeWorkbenchTests::ApplicationIdentityUsesAudastraWithoutMigration() {
    const QString previousName = QCoreApplication::applicationName();
    const QString previousDisplay = QGuiApplication::applicationDisplayName();
    const QString previousOrganization = QCoreApplication::organizationName();
    const QString previousVersion = QCoreApplication::applicationVersion();
    const QIcon previousIcon = QGuiApplication::windowIcon();
    const auto restore = qScopeGuard([&] {
        QCoreApplication::setApplicationName(previousName);
        QGuiApplication::setApplicationDisplayName(previousDisplay);
        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationVersion(previousVersion);
        QGuiApplication::setWindowIcon(previousIcon);
    });
    // 与 Main.cpp 共用生产初始化；不加载设置、凭据，也不创建或迁移旧目录。
    War3Map::QtApp::ConfigureApplicationIdentity();
    QCOMPARE(QCoreApplication::applicationName(), QStringLiteral("Audastra"));
    QCOMPARE(QGuiApplication::applicationDisplayName(), QStringLiteral("Audastra"));
    QCOMPARE(QCoreApplication::organizationName(), QStringLiteral("CallDisaster"));
    const auto icon = QGuiApplication::windowIcon();
    QVERIFY(!icon.isNull());
    QCOMPARE(icon.availableSizes(), (QList<QSize>{QSize(16, 16), QSize(24, 24), QSize(32, 32),
        QSize(48, 48), QSize(64, 64), QSize(128, 128), QSize(256, 256)}));
    QWidget dialog;
    QCOMPARE(dialog.windowIcon().cacheKey(), icon.cacheKey());
    const auto image = War3Map::UiKit::ApplicationBrandPixmap().toImage();
    QCOMPARE(image.size(), QSize(512, 512));
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    QVERIFY(image.pixelColor(256, 256).alpha() > 0);
}

void NativeWorkbenchTests::WindowTitleTracksMapSaveUndoAndClose() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("标题验证.w3x")));
    const auto saved = NativePath(fixture.filePath(QStringLiteral("已保存地图.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const QByteArray original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.resize(1280, 900);
    const QString capturePath = qEnvironmentVariable("FORGE_BRAND_CAPTURE_DIR");
    QJsonArray captureFacts;
    const auto capture = [&](const QString& name) {
        window.show();
        QApplication::processEvents();
        QToolButton* changes = nullptr;
        QToolButton* assistant = nullptr;
        for (auto* button : window.Header()->findChildren<QToolButton*>(QStringLiteral("headerAction"))) {
            if (button->defaultAction() == window.changesAction_) changes = button;
            if (button->defaultAction() && button->defaultAction()->objectName() == QStringLiteral("agentWorkspaceAction")) assistant = button;
        }
        if (!changes || !assistant || changes->text() != QStringLiteral("修改")) return false;
        const auto headerRect = [&](QWidget* widget) { return QRect(widget->mapTo(window.Header(), QPoint{}), widget->size()); };
        const auto changesRect = headerRect(changes), assistantRect = headerRect(assistant);
        auto* toolbar = window.Header()->findChild<War3Map::UiKit::ResponsiveToolBar*>(QStringLiteral("headerActions"));
        if (!toolbar) return false;
        if (!changes->isHidden() && !assistant->isHidden() && changesRect.intersects(assistantRect)) return false;
        for (auto* button : {changes, assistant}) {
            if (button->isHidden()) {
                if (!toolbar->OverflowMenu()->actions().contains(button->defaultAction())) return false;
            } else if (!window.Header()->contentsRect().contains(headerRect(button))) return false;
        }
        // Qt 6.8.3 QToolButton::initStyleOption 会把无图标、无箭头的按钮按 TextOnly
        // 绘制；toolButtonStyle() 仍可能返回 TextBesideIcon，不能凭 getter 虚增图标占位。
        const auto iconWidth = [](const QToolButton* button) {
            const bool hasGraphic = !button->icon().isNull() || button->arrowType() != Qt::NoArrow;
            return button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && hasGraphic
                ? button->iconSize().width() + 4 : 0;
        };
        const auto requiredTextWidth = [&](const QToolButton* button) {
            // headerAction 的 QSS：左右各 5px padding + 1px border。
            if (button->toolButtonStyle() == Qt::ToolButtonIconOnly && !button->icon().isNull()) return button->iconSize().width() + 12;
            return button->fontMetrics().horizontalAdvance(button->text()) + iconWidth(button) + 12;
        };
        for (const auto* button : {changes, assistant}) {
            if (button->isHidden()) continue;
            if (button->width() < requiredTextWidth(button) || button->width() < button->sizeHint().width()) {
                qWarning() << "Header text clipped" << button->text() << button->width() << button->sizeHint()
                           << button->toolButtonStyle() << "icon_null" << button->icon().isNull()
                           << "arrow_type" << button->arrowType() << "required_text_width" << requiredTextWidth(button);
                return false;
            }
        }
        const auto rect = [](QRect value) { return QJsonObject{{"x", value.x()}, {"y", value.y()},
            {"width", value.width()}, {"height", value.height()}}; };
        const auto buttonFacts = [&](QToolButton* button) { return QJsonObject{
            {"header_rect", rect(headerRect(button))}, {"global_rect", rect(QRect(button->mapToGlobal(QPoint{}), button->size()))},
            {"content_rect", rect(button->contentsRect())}, {"text", button->text()},
            {"text_advance", button->fontMetrics().horizontalAdvance(button->text())},
            {"tool_button_style", static_cast<int>(button->toolButtonStyle())},
            {"icon_null", button->icon().isNull()}, {"arrow_type", static_cast<int>(button->arrowType())},
            {"rendered_icon_width", iconWidth(button)}, {"required_text_width", requiredTextWidth(button)},
            {"minimum_width", button->minimumWidth()}, {"size_hint_width", button->sizeHint().width()}}; };
        if (capturePath.isEmpty()) return true;
        if (!QDir().mkpath(capturePath)) return false;
        const auto path = QDir(capturePath).filePath(name + QStringLiteral(".png"));
        const bool written = window.grab().save(path);
        captureFacts.append(QJsonObject{{QStringLiteral("file"), name + QStringLiteral(".png")},
            {QStringLiteral("window_title"), window.windowTitle()},
            {QStringLiteral("changes_button"), buttonFacts(changes)}, {QStringLiteral("assistant_button"), buttonFacts(assistant)},
            {QStringLiteral("native_titlebar_in_image"), false}, {QStringLiteral("fixture_only"), true}});
        return written;
    };
    QCOMPARE(window.windowTitle(), QStringLiteral("Audastra · Per Audacia Ad Astra"));
    const auto* brand = window.Header()->findChild<QLabel*>(QStringLiteral("brand"));
    QVERIFY(brand);
    QCOMPARE(brand->text(), QStringLiteral("Audastra"));
    QVERIFY(capture(QStringLiteral("brand-no-map")));
    QVERIFY(window.OpenMapArchivePath(source));
    QCOMPARE(window.windowTitle(), QStringLiteral("标题验证.w3x — Audastra"));
    QVERIFY(capture(QStringLiteral("brand-map")));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    const int healthRow = FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm");
    QVERIFY(healthRow >= 0);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(
        window.objectsModule_->objectFieldModel_->index(healthRow, 3), 640, Qt::EditRole));
    QCOMPARE(window.windowTitle(), QStringLiteral("● 标题验证.w3x — Audastra"));
    QVERIFY(capture(QStringLiteral("brand-modified")));
    window.project_->objectUndoStacks_[0]->undo();
    QCOMPARE(window.windowTitle(), QStringLiteral("标题验证.w3x — Audastra"));
    window.project_->objectUndoStacks_[0]->redo();
    QCOMPARE(window.windowTitle(), QStringLiteral("● 标题验证.w3x — Audastra"));
    const auto projectFile = NativePath(fixture.filePath(QStringLiteral("状态.forge")));
    QVERIFY(window.SaveEditorProjectToPath(projectFile));
    // 工程保存不能让未保存的地图标题假报干净。
    QCOMPARE(window.windowTitle(), QStringLiteral("● 标题验证.w3x — Audastra"));
    QVERIFY(window.SaveMapArchiveToPath(saved));
    QCOMPARE(window.windowTitle(), QStringLiteral("已保存地图.w3x — Audastra"));
    QCOMPARE(ReadFile(source), original);
    window.ResetProjectState();
    QCOMPARE(window.windowTitle(), QStringLiteral("Audastra · Per Audacia Ad Astra"));
    if (!capturePath.isEmpty()) {
        QFile receipt(QDir(capturePath).filePath(QStringLiteral("brand-titles.json")));
        QVERIFY(receipt.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(captureFacts).toJson();
        QCOMPARE(receipt.write(bytes), static_cast<qint64>(bytes.size()));
    }
}

void NativeWorkbenchTests::ProductMenusRemainAvailableWithoutStartingProbeServices() {
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    for (const auto* name : {"applicationFeedbackAction", "applicationUpdatesAction", "applicationSettingsAction"}) {
        const auto* action = window.findChild<QAction*>(QString::fromLatin1(name));
        QVERIFY(action);
        QVERIFY(action->isEnabled());
    }
    window.SetupProductServices();
    QVERIFY(!window.feedbackClient_);
    QVERIFY(!window.updateClient_);
    QVERIFY(!window.productWorker_);
}

void NativeWorkbenchTests::UpdateCancellationPreservesEditsWithoutDiscardChoice() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("原创更新关闭保护.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    const int healthRow = FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm");
    QVERIFY(healthRow >= 0);
    const auto healthIndex = window.objectsModule_->objectFieldModel_->index(healthRow, 3);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(healthIndex, 640, Qt::EditRole));
    const int undoIndex = window.project_->objectUndoStacks_[0]->index();
    bool sawPrompt = false;
    bool offeredDiscard = false;
    bool offeredSave = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            sawPrompt = true;
            for (auto* button : prompt->buttons()) {
                offeredDiscard = offeredDiscard || prompt->buttonRole(button) == QMessageBox::DestructiveRole;
                offeredSave = offeredSave || prompt->buttonRole(button) == QMessageBox::AcceptRole;
            }
            prompt->reject();
        }
    });
    QVERIFY(!window.ConfirmDiscardPendingEdits(QStringLiteral("更新"), false));
    QVERIFY(sawPrompt);
    QVERIFY(offeredSave);
    QVERIFY(!offeredDiscard);
    QCOMPARE(window.objectsModule_->objectFieldModel_->data(healthIndex, Qt::EditRole).toInt(), 640);
    QCOMPARE(window.project_->objectUndoStacks_[0]->index(), undoIndex);
    QVERIFY(window.project_->objectDirty_[0]);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::AboutSurfaceShowsVersionAndBuildOutsideWindowTitle() {
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.resize(1280, 900);
    window.show();
    QApplication::processEvents();
    auto* action = window.findChild<QAction*>(QStringLiteral("applicationAboutAction"));
    QVERIFY(action);
    action->trigger();
    auto* about = window.findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("applicationAbout"));
    QVERIFY(about);
    QTRY_VERIFY(about->isVisible());
    const auto text = [about](const char* name) {
        const auto* label = about->findChild<QLabel*>(QString::fromLatin1(name));
        return label ? label->text() : QString();
    };
    QCOMPARE(text("aboutProductName"), QStringLiteral("Audastra"));
    QCOMPARE(text("aboutTagline"), QStringLiteral("Per Audacia Ad Astra"));
    const auto* logo = about->findChild<QLabel*>(QStringLiteral("aboutLogo"));
    QVERIFY(logo);
    QVERIFY(!logo->pixmap().isNull());
    QCOMPARE(logo->size(), QSize(64, 64));
    QVERIFY(logo->hasScaledContents());
    QVERIFY(!logo->accessibleName().isEmpty());
    QVERIFY(text("aboutBuildStamp").contains(QString::fromUtf8(kWar3MapBuildStamp)));
    QVERIFY(text("aboutLicense").contains(QStringLiteral("PolyForm Noncommercial 1.0.0")));
    QVERIFY(!window.windowTitle().contains(QString::fromUtf8(kWar3MapBuildStamp)));
    const QString capturePath = qEnvironmentVariable("FORGE_BRAND_CAPTURE_DIR");
    if (!capturePath.isEmpty()) {
        QVERIFY(QDir().mkpath(capturePath));
        QVERIFY(about->grab().save(QDir(capturePath).filePath(QStringLiteral("brand-about.png"))));
    }
    QTest::keyClick(about, Qt::Key_Escape);
    QTRY_VERIFY(!about->isVisible());
    action->trigger();
    QTRY_VERIFY(about->isVisible());
    QCOMPARE(window.findChildren<War3Map::UiKit::PopoverSurface*>(QStringLiteral("applicationAbout")).size(), qsizetype{1});
    auto* close = about->findChild<QPushButton*>(QStringLiteral("aboutClose"));
    QVERIFY(close);
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(!about->isVisible());
}

void NativeWorkbenchTests::AgentHttpToolsShareLiveDocumentAndRenderReceipts() {
    RunAgentHttpDocumentProbe(false);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpToolsRestoreOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic HTTP evidence does not establish real-map GUI evidence");
    RunAgentHttpDocumentProbe(true);
}

void NativeWorkbenchTests::AgentHttpVariableToolsShareTriggerUndoAndRenderReceipts() {
    RunAgentHttpDocumentProbe(false, AgentHttpProbeKind::Variables);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpVariablesRestoreOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic variable HTTP evidence is not real-map evidence");
    RunAgentHttpDocumentProbe(true, AgentHttpProbeKind::Variables);
}

void NativeWorkbenchTests::AgentHttpEntityToolsShareMapDataUndoAndRenderReceipts() {
    RunAgentHttpDocumentProbe(false, AgentHttpProbeKind::Entities);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpEntitiesRestoreOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic entity HTTP evidence is not real-map evidence");
    RunAgentHttpDocumentProbe(true, AgentHttpProbeKind::Entities);
}

void NativeWorkbenchTests::AgentHttpPlacementsRestoreOriginalDocument() {
    RunAgentHttpDocumentProbe(false, AgentHttpProbeKind::PlacementCopy);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpPlacementsRestoreOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic placement HTTP evidence is not real-map evidence");
    RunAgentHttpDocumentProbe(true, AgentHttpProbeKind::PlacementCopy);
}

void NativeWorkbenchTests::AgentHttpEntityFiltersPreserveOriginalDocument() {
    RunAgentHttpDocumentProbe(false, AgentHttpProbeKind::EntityFilters);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpEntityFiltersPreserveOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic filter HTTP evidence is not real-map evidence");
    RunAgentHttpDocumentProbe(true, AgentHttpProbeKind::EntityFilters);
}

void NativeWorkbenchTests::AgentHttpEntityPositionRestoresOriginalDocument() {
    RunAgentHttpDocumentProbe(false, AgentHttpProbeKind::EntityPosition);
}

void NativeWorkbenchTests::OptionalRealMapAgentHttpEntityPositionRestoresOriginalDocument() {
    if (qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"))
        QSKIP("FORGE_MCP_GUI_REAL_MAP is unset; synthetic XYZ HTTP evidence is not real-map evidence");
    RunAgentHttpDocumentProbe(true, AgentHttpProbeKind::EntityPosition);
}

void NativeWorkbenchTests::AgentHttpSoundToolsRestoreOriginalDocument() {
    RunAgentHttpDocumentProbe(!qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"), AgentHttpProbeKind::Sounds);
}

void NativeWorkbenchTests::AgentHttpImportToolsPreserveOriginalDocument() {
    RunAgentHttpDocumentProbe(!qEnvironmentVariableIsEmpty("FORGE_MCP_GUI_REAL_MAP"), AgentHttpProbeKind::Imports);
}

void NativeWorkbenchTests::RunAgentHttpDocumentProbe(bool realMap, AgentHttpProbeKind kind) {
    using War3Map::Agent::AgentWindow;
    using War3Map::Agent::AgentConversationView;
    const bool variables = kind == AgentHttpProbeKind::Variables;
    const bool sounds = kind == AgentHttpProbeKind::Sounds;
    const bool imports = kind == AgentHttpProbeKind::Imports;
    const bool resources = sounds || imports;
    const bool placements = kind == AgentHttpProbeKind::PlacementCopy;
    const bool entityFilters = kind == AgentHttpProbeKind::EntityFilters;
    const bool entityPosition = kind == AgentHttpProbeKind::EntityPosition;
    const bool entities = kind == AgentHttpProbeKind::Entities || placements || entityFilters || entityPosition;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto input = qEnvironmentVariable("FORGE_MCP_GUI_REAL_MAP");
    const auto source = NativePath(realMap ? input : fixture.filePath(QStringLiteral("原创 HTTP 地图夹具.w3x")));
    if (realMap) {
        const auto allowedInput = qEnvironmentVariable("FORGE_MCP_GUI_ALLOWED_ROOT");
        QVERIFY2(!allowedInput.isEmpty() && QDir::isAbsolutePath(allowedInput),
            "Real GUI probe requires an explicitly authorized absolute fixture root");
        const auto allowedRoot = QFileInfo(allowedInput).canonicalFilePath();
        QVERIFY2(!allowedRoot.isEmpty() && QFileInfo(allowedRoot).isDir(), "Authorized fixture root must be an existing canonical directory");
        const auto canonical = QFileInfo(input).canonicalFilePath();
        QVERIFY2(!canonical.isEmpty() && QFileInfo(canonical).isFile(), "Real GUI source must be an existing canonical file");
        const auto relative = QDir(allowedRoot).relativeFilePath(canonical);
        QVERIFY2(!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..")
            && !relative.startsWith(QStringLiteral("../")), "Real GUI source must remain inside the explicitly authorized root");
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    } else if (variables || entities || resources) {
        // 原创类型目录与真实编码的 WTG/WCT，仍由 MainWindow 的正常开图路径建立文档。
        const auto definitions = fixture.filePath("variable-definitions/ui");
        QVERIFY(WriteText(definitions + "/config", "base\n"));
        QVERIFY(WriteText(definitions + "/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n"));
        for (const auto* file : {"action.txt", "event.txt", "condition.txt", "call.txt"})
            QVERIFY(WriteText(definitions + "/base/" + QString::fromLatin1(file), ""));
        const auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(definitions));
        QVERIFY(catalog.Succeeded());
        const auto empty = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
        QVERIFY(empty.Succeeded());
        const auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value);
        QVERIFY(encoded.Succeeded());
        std::vector<War3Map::Mpq::MpqBuildFile> files{
            {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct},
            {"war3map.w3u", MinimalUnitData(420)}, {"war3map.j", Bytes("// Original variable HTTP fixture\n")}};
        if (resources) {
            // 原创声音定义与两个不透明导入成员；不播放音频、不解释导入正文。
            War3Map::MapData::SoundRecord sound{};
            sound.name = "OwnedHttpSound"; sound.filePath = "Sound\\Owned.wav";
            sound.eaxSetting = "DefaultEAXON"; sound.volume = 64; sound.pitch = 1;
            sound.fadeInRate = 20; sound.fadeOutRate = 30;
            const auto w3s = War3Map::MapData::SoundsDocument{1, {sound}, {}}.Serialize();
            QVERIFY(w3s.Succeeded());
            auto table = War3Map::Resources::ImportTable::CreateEmpty();
            QVERIFY(table.Upsert("Owned\\first.bin").Succeeded());
            QVERIFY(table.Upsert("Owned\\second.bin").Succeeded());
            const auto imp = table.Serialize(); QVERIFY(imp.Succeeded());
            files.push_back({"war3map.w3s", *w3s.value});
            files.push_back({"war3map.imp", *imp.value});
            files.push_back({"Owned\\first.bin", Bytes("opaque-original-fixture-one")});
            files.push_back({"Owned\\second.bin", Bytes("opaque-original-fixture-two")});
        }
        if (entities) {
            War3Map::MapData::RegionsDocument regions;
            regions.entries.push_back({-256.0f, -128.0f, 256.0f, 128.0f, "原创 HTTP 区域", 1, {}, {}, {}});
            const auto encodedRegions = regions.Serialize();
            QVERIFY(encodedRegions.Succeeded());
            files.push_back({"war3map.w3r", *encodedRegions.value});
            files.push_back({"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes});
            if (placements || entityFilters || entityPosition) {
                War3Map::MapData::UnitPlacements units;
                War3Map::MapData::UnitPlacement unit;
                unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; unit.position = {128, 64, 0};
                unit.creationId = 17; unit.targetAcquisition = -1; unit.unknownOwnerBytes = {0xa5, 0x5a};
                unit.inventory.push_back({0, "ratf"}); unit.abilities.push_back({"Adef", true, 1});
                units.units.push_back(unit);
                if (entityFilters) {
                    unit.creationId = 18; unit.position[0] = 256; unit.rawcode = "hbar"; units.units.push_back(unit);
                    unit.creationId = 19; unit.position[0] = 384; unit.rawcode = "hfoo"; units.units.push_back(unit);
                    unit.creationId = 20; unit.owner = 1; units.units.push_back(unit);
                    unit.creationId = 21; unit.owner = 0; unit.rawcode = "Hfoo"; units.units.push_back(unit);
                }
                const auto encodedUnits = units.Serialize(); QVERIFY(encodedUnits.Succeeded());
                files.push_back({"war3mapUnits.doo", *encodedUnits.value});
            } else files.push_back({"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()});
            files.push_back({"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()});
        }
        QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    } else {
        QVERIFY(MakeArchive(source, 420).Succeeded());
    }
    const QByteArray original = ReadFile(source);
    QVERIFY(!original.isEmpty());
    const auto sourceSha = QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
    if (realMap) {
        const auto expectedSha = qEnvironmentVariable("FORGE_MCP_GUI_SOURCE_SHA256");
        QVERIFY2(expectedSha.size() == 64 && std::all_of(expectedSha.cbegin(), expectedSha.cend(), [](QChar digit) {
            return (digit >= QLatin1Char('0') && digit <= QLatin1Char('9'))
                || (digit >= QLatin1Char('a') && digit <= QLatin1Char('f'))
                || (digit >= QLatin1Char('A') && digit <= QLatin1Char('F'));
        }), "Real GUI probe requires a complete 64-digit source SHA256");
        QCOMPARE(sourceSha, expectedSha.toLower());
    }
    const auto verifySource = qScopeGuard([&] {
        QCOMPARE(ReadFile(source), original); // 即使中途断言失败，也核对从未写入输入地图。
    });
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    if ((variables || entities || resources) && !realMap) QVERIFY(window.UseUiDataRoot(NativePath(fixture.filePath("variable-definitions/ui"))));
    if (realMap) {
        const auto uiRoot = qEnvironmentVariable("FORGE_YDWE_ROOT"), gameRoot = qEnvironmentVariable("FORGE_WAR3_ROOT");
        QVERIFY2(!uiRoot.isEmpty() && !gameRoot.isEmpty(), "Real GUI probe requires actual YDWE and game resource roots");
        QVERIFY(window.UseUiDataRoot(NativePath(uiRoot)));
        QVERIFY(window.OpenGameResources(NativePath(gameRoot)));
    }
    QVERIFY(window.OpenMapArchivePath(source));
    if (!realMap) QVERIFY(InstallSyntheticEditorContext(window));
    else QVERIFY(window.project_->standardCatalog_ && window.project_->objectFieldSchema_);
    auto* project = window.project_.get();
    auto* document = project->objectDocuments_[0].get();
    auto* undo = project->objectUndoStacks_[0];
    QVERIFY(document && undo);
    const auto before = document->DocumentFingerprint();
    const auto undoCount = undo->count();
    const auto originalUndoIndex = undo->index();
    const auto restoreDocument = qScopeGuard([&] {
        if (realMap) {
            while (undo->index() > originalUndoIndex) undo->undo();
            QCOMPARE(document->DocumentFingerprint(), before);
        }
    });
    // 只在内存中传入随机 bearer；回执只记录 JSON-RPC body，绝不序列化 HTTP 认证头。
    const QByteArray token = QUuid::createUuid().toByteArray(QUuid::Id128)
        + QUuid::createUuid().toByteArray(QUuid::Id128);
    War3Map::QtApp::LiveEditorTools* tools = nullptr;
    QString error;
    const auto port = window.StartAgentHttpProbe(token, tools, error);
    QVERIFY2(port != 0 && tools, qPrintable(error));
    QSignalSpy activity(tools, &War3Map::QtApp::LiveEditorTools::Activity);
    window.resize(1280, 900);
    window.show();
    window.ShowAgentWindow();
    auto* agent = window.findChild<AgentWindow*>();
    QVERIFY(agent);
    auto* conversation = agent->findChild<AgentConversationView*>();
    QVERIFY(conversation);
    QJsonArray exchanges;
    int requestId = 0;
    const auto rpc = [&](const QString& method, const QJsonObject& params,
        const QString& expectedErrorMessage = QString{}) -> QJsonObject {
        const auto firstEvent = activity.size();
        const QJsonObject request{{"jsonrpc", "2.0"}, {"id", ++requestId}, {"method", method}, {"params", params}};
        const auto body = QJsonDocument(request).toJson(QJsonDocument::Compact);
        const auto headers = QByteArray("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(port)
            + "\r\nAuthorization: Bearer " + token + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n";
        QTcpSocket socket;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QByteArray response;
        QObject::connect(&socket, &QTcpSocket::connected, &loop, [&] { socket.write(headers + body); });
        QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(5000);
        socket.connectToHost(QHostAddress::LocalHost, port);
        loop.exec();
        response += socket.readAll();
        if (!response.startsWith("HTTP/1.1 200") || response.indexOf("\r\n\r\n") < 0) {
            error = QStringLiteral("HTTP transport failed for %1: %2").arg(method, QString::fromLatin1(response.left(response.indexOf("\r\n"))));
            return {};
        }
        const auto result = QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object();
        if (result.value("id") != request.value("id")) {
            error = QStringLiteral("JSON-RPC identity failure for %1").arg(method);
            return {};
        }
        if (!expectedErrorMessage.isEmpty()) {
            const auto rpcError = result.value("error").toObject();
            if (result.contains("result") || !rpcError.value("code").isDouble()
                || rpcError.value("code").toDouble() != -32602
                || rpcError.value("message").toString() != expectedErrorMessage) {
                // 仅诊断响应身份及错误文本；不打印请求、认证头或 error.data。
                const QJsonObject diagnostic{{"id", result.value("id")}, {"has_result", result.contains("result")},
                    {"error", QJsonObject{{"code", rpcError.value("code")}, {"message", rpcError.value("message")}}}};
                auto actual = QString::fromUtf8(QJsonDocument(diagnostic).toJson(QJsonDocument::Compact));
                actual.replace(QString::fromLatin1(token), QStringLiteral("[redacted]"));
                error = QStringLiteral("Expected JSON-RPC invalid-params response was not received for %1; actual=%2").arg(method, actual);
                return {};
            }
        } else if (result.contains("error")) {
            error = QStringLiteral("Unexpected JSON-RPC error for %1").arg(method);
            return {};
        }
        QJsonArray brokerIds;
        for (auto index = firstEvent; index < activity.size(); ++index) {
            const auto event = activity.at(index).first().toJsonObject();
            if (event.value("type").toString() == QStringLiteral("tool_start")) brokerIds.append(event.value("id"));
        }
        exchanges.append(QJsonObject{{"request", request}, {"response", result}, {"http_status", 200}, {"broker_call_ids", brokerIds}});
        // 仅显式期望的参数校验错误返回完整信封；普通工具调用仍须取得成功信封中的 result。
        if (!expectedErrorMessage.isEmpty()) return result;
        return result.value("result").toObject();
    };
    const auto initialized = rpc(QStringLiteral("initialize"), {{"protocolVersion", "2025-03-26"},
        {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "native-gui-http-probe"}, {"version", "1"}}}});
    QVERIFY2(initialized.contains("serverInfo"), qPrintable(error));
    const auto catalog = rpc(QStringLiteral("tools/list"), {});
    const auto definitions = catalog.value("tools").toArray();
    const QStringList requiredTools = sounds
        ? QStringList{"editor_state", "sounds_list", "sounds_get", "sounds_update"} : imports
        ? QStringList{"editor_state", "imports_list", "imports_get"} : entityFilters
        ? QStringList{"editor_state", "entities_query"} : placements
        ? QStringList{"editor_state", "entities_query", "entities_copy", "entities_rotate"} : entities
        ? QStringList{"editor_state", "entities_query", "entities_set_fields"} : variables
        ? QStringList{"editor_state", "variables_list", "variables_get", "variables_create", "variables_update", "variables_rename", "variables_delete"}
        : QStringList{"editor_state", "objects_list", "objects_get", "objects_set_fields"};
    for (const auto& name : requiredTools) {
        const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) {
            return value.toObject().value("name").toString() == name;
        });
        QVERIFY2(found != definitions.end(), qPrintable(name));
        QVERIFY((*found).toObject().value("inputSchema").isObject());
    }
    const auto call = [&](const QString& name, const QJsonObject& args) {
        const auto response = rpc(QStringLiteral("tools/call"), {{"name", name}, {"arguments", args}});
        const auto content = response.value("content").toArray();
        return content.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(content.first().toObject()
            .value("text").toString().toUtf8()).object();
    };
    const auto state = call(QStringLiteral("editor_state"), {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(error));
    if (resources) {
        const auto explain = [](const QJsonObject& value) {
            return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
        };
        const auto hashBytes = [](const std::vector<std::byte>& bytes) {
            return QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex());
        };
        auto* mapEdits = project->MapEdits(); auto* mapUndo = project->mapDataUndoStack_;
        auto* mapDocument = project->mapDocument_.get(); auto* triggerDocument = project->triggerDocument_.get();
        auto* triggerUndo = project->triggerUndoStack_;
        QVERIFY(mapEdits && mapUndo && mapDocument && triggerDocument && triggerUndo && project->uiCatalog_);
        QVERIFY(project->archiveSession_);
        const auto baseline = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(baseline.Succeeded());
        const auto triggerBaseline = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(triggerBaseline.Succeeded());
        const auto impBaseline = project->archiveSession_->ReadMember("war3map.imp"); QVERIFY(impBaseline.Succeeded());
        const auto originalMapIndex = mapUndo->index(), originalMapCount = mapUndo->count();
        const auto originalTriggerIndex = triggerUndo->index(), originalTriggerCount = triggerUndo->count();
        const auto originalProjectRevision = project->editRevision_.Revision(), originalMapRevision = mapEdits->Revision();
        std::array<std::string, 7> objectFingerprints;
        std::array<const LniObjectDocument*, 7> objectPointers{};
        std::array<QUndoStack*, 7> objectUndos{};
        std::array<int, 7> objectIndices{}, objectCounts{};
        for (std::size_t i = 0; i < objectFingerprints.size(); ++i) {
            objectPointers[i] = project->objectDocuments_[i].get(); objectUndos[i] = project->objectUndoStacks_[i];
            if (objectPointers[i]) objectFingerprints[i] = objectPointers[i]->DocumentFingerprint();
            if (objectUndos[i]) { objectIndices[i] = objectUndos[i]->index(); objectCounts[i] = objectUndos[i]->count(); }
        }
        const auto restoreMap = qScopeGuard([&] {
            while (mapUndo->index() > originalMapIndex) mapUndo->undo();
        });
        QCOMPARE(state.value("source_sha256").toString(), sourceSha);
        QJsonArray views;
        QJsonObject operation;
        const auto lastCallId = [&] {
            const auto ids = exchanges.last().toObject().value("broker_call_ids").toArray();
            return ids.size() == 1 ? ids.first().toString() : QString{};
        };
        const auto addView = [&](const QString& id, const QStringList& path, const QString& name,
            const QString& anchor = QString{}) {
            QJsonArray fields; for (const auto& field : path) fields.append(field);
            views.append(QJsonObject{{"call_id", id}, {"path", fields}, {"name", name}, {"anchor", anchor}});
        };
        if (sounds) {
            const auto listed = call(QStringLiteral("sounds_list"), {{"page", QJsonObject{{"limit", 4}}}});
            QVERIFY2(listed.value("ok").toBool(), qPrintable(explain(listed)));
            const auto listId = lastCallId();
            const auto rows = listed.value("rows").toArray(); QVERIFY(!rows.isEmpty());
            qsizetype selected = -1;
            for (qsizetype i = 0; i < rows.size(); ++i) if (rows.at(i).toObject().value("writable").toBool()) { selected = i; break; }
            QVERIFY2(selected >= 0, "The bounded sound fixture must expose a unique writable classic definition");
            const auto row = rows.at(selected).toObject();
            const auto soundId = row.value("sound_id").toString(); QVERIFY(soundId.startsWith(QStringLiteral("sound:")));
            const QJsonObject getArgs{{"sound_id", soundId}, {"fields", QJsonArray{"volume", "fade_in", "fade_out"}}};
            const auto read = call(QStringLiteral("sounds_get"), getArgs);
            QVERIFY2(read.value("ok").toBool(), qPrintable(explain(read))); const auto getId = lastCallId();
            const auto readRows = read.value("rows").toArray(); QCOMPARE(readRows.size(), 1);
            const auto fields = readRows.first().toObject().value("fields").toArray(); QCOMPARE(fields.size(), 3);
            QJsonArray changes;
            QJsonObject expected;
            for (const auto& value : fields) {
                const auto field = value.toObject(); const auto id = field.value("id").toString();
                QVERIFY(id == QStringLiteral("volume") || id == QStringLiteral("fade_in") || id == QStringLiteral("fade_out"));
                bool valid = false; const auto oldValue = field.value("raw").toString().toInt(&valid); QVERIFY(valid);
                const int newValue = id == QStringLiteral("volume") ? (oldValue == 65 ? 64 : 65) : (oldValue == 21 ? 22 : 21);
                QVERIFY(newValue != oldValue);
                changes.append(QJsonObject{{"field", id}, {"expected_raw", field.value("raw")}, {"value", newValue}});
                expected.insert(id, newValue);
            }
            const QJsonObject updateArgs{{"project_identity", read.value("project_identity")}, {"expected_revision", read.value("revision")},
                {"sound_id", soundId}, {"expected_member_sha256", read.value("member").toObject().value("member_sha256")}, {"changes", changes}};
            const auto edited = call(QStringLiteral("sounds_update"), updateArgs);
            QVERIFY2(edited.value("ok").toBool(), qPrintable(explain(edited))); const auto editId = lastCallId();
            QCOMPARE(edited.value("applied_fields").toString(), QStringLiteral("3"));
            QVERIFY(edited.value("changed").toBool() && !edited.value("saved").toBool() && !edited.value("runtime_verified").toBool());
            QCOMPARE(mapUndo->index(), originalMapIndex + 1); QCOMPARE(mapUndo->count(), originalMapCount + 1);
            const auto reread = call(QStringLiteral("sounds_get"), getArgs); QVERIFY2(reread.value("ok").toBool(), qPrintable(explain(reread)));
            const auto afterFields = reread.value("rows").toArray().first().toObject().value("fields").toArray(); QCOMPARE(afterFields.size(), 3);
            for (const auto& value : afterFields) {
                const auto field = value.toObject(); const auto id = field.value("id").toString(); QVERIFY(expected.contains(id));
                QCOMPARE(field.value("raw").toString(), QString::number(expected.value(id).toInt()));
                QCOMPARE(field.value("value").toInt(), expected.value(id).toInt());
            }
            const auto changedDocument = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(changedDocument.Succeeded());
            const auto& originalSounds = baseline.value->Member(War3Map::MapDataKind::Sounds).sources;
            const auto& changedSounds = changedDocument.value->Member(War3Map::MapDataKind::Sounds).sources;
            QCOMPARE(originalSounds.size(), std::size_t{1}); QCOMPARE(changedSounds.size(), std::size_t{1});
            QVERIFY(changedSounds.front().data != originalSounds.front().data);
            // 全记录独立构造预期W3S，确认仅三字段变化，其余定义、顺序和尾部字节保留。
            QVERIFY(baseline.value->Sounds()); auto expectedSounds = *baseline.value->Sounds();
            bool indexValid = false; const auto soundIndex = row.value("source_index").toString().toULongLong(&indexValid);
            QVERIFY(indexValid && soundIndex < expectedSounds.entries.size());
            auto& native = expectedSounds.entries[static_cast<std::size_t>(soundIndex)];
            native.volume = expected.value("volume").toInt(); native.fadeInRate = expected.value("fade_in").toInt(); native.fadeOutRate = expected.value("fade_out").toInt();
            const auto expectedW3s = expectedSounds.Serialize(); QVERIFY(expectedW3s.Succeeded());
            QCOMPARE(changedSounds.front().data, *expectedW3s.value);
            for (const auto& member : baseline.value->Members()) {
                if (member.kind == War3Map::MapDataKind::Sounds) continue;
                const auto& current = changedDocument.value->Member(member.kind);
                QCOMPARE(current.state, member.state); QCOMPARE(current.sources.size(), member.sources.size());
                for (std::size_t i = 0; i < member.sources.size(); ++i) {
                    QCOMPARE(current.sources[i].name, member.sources[i].name); QCOMPARE(current.sources[i].data, member.sources[i].data);
                }
            }
            mapUndo->undo(); QCOMPARE(mapUndo->index(), originalMapIndex);
            const auto restored = call(QStringLiteral("sounds_get"), getArgs); QVERIFY(restored.value("ok").toBool());
            QCOMPARE(restored.value("rows").toArray().first().toObject().value("fields"), QJsonValue(fields));
            addView(listId, {"rows", QStringLiteral("[%1]").arg(selected)}, "sounds-list");
            addView(getId, {"rows", "[0]", "fields", "[0]"}, "sounds-get-original");
            const auto changedItems = edited.value("changes").toObject().value("items").toArray(); QCOMPARE(changedItems.size(), 3);
            for (qsizetype i = 0; i < changedItems.size(); ++i) {
                const auto item = changedItems.at(i).toObject(); const auto id = item.value("field_id").toString();
                QVERIFY(expected.contains(id)); QCOMPARE(item.value("after").toString(), QString::number(expected.value(id).toInt()));
                addView(editId, {"changes", "items", QStringLiteral("[%1]").arg(i)}, "sounds-update-" + id);
            }
            operation = {{"sound_id", soundId}, {"requested_changes", changes}, {"edit_call_id", editId},
                {"w3s_before_sha256", hashBytes(originalSounds.front().data)}, {"w3s_edited_sha256", hashBytes(changedSounds.front().data)},
                {"three_fields_read_back", true}, {"single_original_undo_command", true}, {"other_sound_bytes_preserved", true}};
        } else {
            const auto parsedImp = War3Map::Resources::ImportTable::Parse(*impBaseline.value); QVERIFY(parsedImp.Succeeded());
            QVERIFY(parsedImp.value->Entries().size() >= 2);
            const auto first = call(QStringLiteral("imports_list"), {{"page", QJsonObject{{"limit", 1}}}});
            QVERIFY2(first.value("ok").toBool(), qPrintable(explain(first))); const auto listId = lastCallId();
            QCOMPARE(first.value("binding").toObject().value("source_sha256").toString(), sourceSha);
            QCOMPARE(first.value("imp_record_count").toString(), QString::number(parsedImp.value->Entries().size()));
            QCOMPARE(first.value("imp_sha256").toString(), hashBytes(*impBaseline.value));
            QVERIFY(first.value("read_only").toBool() && first.value("payloads_opaque").toBool() && !first.value("text_available").toBool());
            const auto firstRows = first.value("rows").toArray(); QCOMPARE(firstRows.size(), 1);
            const auto impRow = firstRows.first().toObject(); QCOMPARE(impRow.value("kind").toString(), QStringLiteral("imp-record"));
            const auto& rawRecord = parsedImp.value->Entries().front();
            QCOMPARE(impRow.value("flags").toInt(), static_cast<int>(rawRecord.flags));
            QCOMPARE(impRow.value("raw_path_hex").toString(), QString::fromLatin1(QByteArray::fromStdString(rawRecord.path).toHex()));
            const auto cursor = first.value("next_cursor").toObject(); QVERIFY(!cursor.isEmpty());
            QCOMPARE(cursor.value("binding"), first.value("binding")); QCOMPARE(cursor.value("inventory_sha256"), first.value("inventory_sha256"));
            QCOMPARE(cursor.value("next_index").toString(), QStringLiteral("1"));
            const auto second = call(QStringLiteral("imports_list"), {{"page", QJsonObject{{"limit", 1}, {"cursor", cursor}}}});
            QVERIFY2(second.value("ok").toBool(), qPrintable(explain(second)));
            QCOMPARE(second.value("binding"), first.value("binding")); QCOMPARE(second.value("inventory_sha256"), first.value("inventory_sha256"));
            const auto secondRows = second.value("rows").toArray(); QCOMPARE(secondRows.size(), 1);
            QVERIFY(secondRows.first().toObject().value("import_id") != impRow.value("import_id"));
            QCOMPARE(secondRows.first().toObject().value("raw_path_hex").toString(),
                QString::fromLatin1(QByteArray::fromStdString(parsedImp.value->Entries().at(1).path).toHex()));
            const QJsonObject getIdentity{{"project_identity", first.value("project_identity")}, {"expected_revision", first.value("revision")},
                {"expected_inventory_sha256", first.value("inventory_sha256")}};
            auto impArgs = getIdentity; impArgs.insert("import_id", impRow.value("import_id"));
            const auto detail = call(QStringLiteral("imports_get"), impArgs); QVERIFY2(detail.value("ok").toBool(), qPrintable(explain(detail)));
            const auto impId = lastCallId(); QCOMPARE(detail.value("row").toObject(), impRow);
            // 只查物理IMP成员的元数据，不调用任何导入正文读取接口。
            const auto physicalList = call(QStringLiteral("imports_list"), {{"query", "war3map.imp"}, {"page", QJsonObject{{"limit", 8}}}});
            QVERIFY2(physicalList.value("ok").toBool(), qPrintable(explain(physicalList)));
            QJsonObject physical;
            for (const auto& value : physicalList.value("rows").toArray()) {
                const auto candidate = value.toObject();
                if (candidate.value("kind") == "physical-member" && candidate.value("raw_path") == "war3map.imp") physical = candidate;
            }
            QVERIFY(!physical.isEmpty()); QVERIFY(physical.value("flags").isNull());
            QCOMPARE(physical.value("state").toString(), QStringLiteral("present"));
            QCOMPARE(physical.value("bytes").toString(), QString::number(impBaseline.value->size()));
            QCOMPARE(physical.value("sha256").toString(), hashBytes(*impBaseline.value));
            auto physicalArgs = getIdentity; physicalArgs.insert("import_id", physical.value("import_id"));
            const auto physicalDetail = call(QStringLiteral("imports_get"), physicalArgs); QVERIFY(physicalDetail.value("ok").toBool());
            const auto physicalId = lastCallId(); QCOMPARE(physicalDetail.value("row").toObject(), physical);
            for (const auto& field : {QStringLiteral("source_sha256"), QStringLiteral("inventory_sha256")}) {
                auto forged = cursor;
                if (field == QStringLiteral("source_sha256")) {
                    auto binding = forged.value("binding").toObject(); binding.insert(field, QString(64, '0')); forged.insert("binding", binding);
                } else forged.insert(field, QString(64, '0'));
                const auto rejected = call(QStringLiteral("imports_list"), {{"page", QJsonObject{{"limit", 1}, {"cursor", forged}}}});
                QVERIFY(!rejected.value("ok").toBool());
                QCOMPARE(rejected.value("diagnostics").toArray().first().toObject().value("code").toString(), QStringLiteral("IMPORT_CURSOR_STALE"));
            }
            addView(listId, {"next_cursor", "binding"}, "imports-source-cursor");
            addView(impId, {"row", "raw_path_hex"}, "imports-imp-record", "flags");
            addView(physicalId, {"row", "source_path"}, "imports-physical-member", "raw_path");
            operation = {{"imp_record", impRow}, {"physical_member", physical}, {"cursor", cursor},
                {"source_and_inventory_cursor_rejected", true}, {"import_payloads_read", false}, {"source_imp_sha256", hashBytes(*impBaseline.value)}};
        }
        const auto finalMap = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(finalMap.Succeeded());
        for (const auto& member : baseline.value->Members()) {
            const auto& finalMember = finalMap.value->Member(member.kind); QCOMPARE(finalMember.state, member.state);
            QCOMPARE(finalMember.sources.size(), member.sources.size());
            for (std::size_t i = 0; i < member.sources.size(); ++i) {
                QCOMPARE(finalMember.sources[i].name, member.sources[i].name); QCOMPARE(finalMember.sources[i].data, member.sources[i].data);
            }
        }
        QCOMPARE(project->MapEdits(), mapEdits); QCOMPARE(project->mapDataUndoStack_, mapUndo); QCOMPARE(mapUndo->index(), originalMapIndex);
        QCOMPARE(project->triggerDocument_.get(), triggerDocument); QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(triggerUndo->index(), originalTriggerIndex); QCOMPARE(triggerUndo->count(), originalTriggerCount);
        const auto triggerFinal = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_); QVERIFY(triggerFinal.Succeeded());
        QCOMPARE(triggerFinal.value->wtg, triggerBaseline.value->wtg); QCOMPARE(triggerFinal.value->wct, triggerBaseline.value->wct);
        for (std::size_t i = 0; i < objectFingerprints.size(); ++i) {
            QCOMPARE(project->objectDocuments_[i].get(), objectPointers[i]); QCOMPARE(project->objectUndoStacks_[i], objectUndos[i]);
            if (objectPointers[i]) QCOMPARE(objectPointers[i]->DocumentFingerprint(), objectFingerprints[i]);
            if (objectUndos[i]) { QCOMPARE(objectUndos[i]->index(), objectIndices[i]); QCOMPARE(objectUndos[i]->count(), objectCounts[i]); }
        }
        const auto finalImp = project->archiveSession_->ReadMember("war3map.imp"); QVERIFY(finalImp.Succeeded());
        QCOMPARE(*finalImp.value, *impBaseline.value); QCOMPARE(ReadFile(source), original);
        if (imports) {
            QCOMPARE(mapUndo->count(), originalMapCount); QCOMPARE(mapEdits->Revision(), originalMapRevision);
            QCOMPARE(project->editRevision_.Revision(), originalProjectRevision);
        }

        // 三层身份逐项相等：真实HTTP响应、同次Activity、卡片持有的完整JSON；不补造展示事件。
        QJsonArray events; QSet<QString> starts, ends; QStringList orderedCalls; QJsonObject resultById, nameById, titleById;
        int changeEvents = 0;
        for (const auto& arguments : activity) {
            const auto event = arguments.front().toJsonObject(); events.append(event); const auto type = event.value("type").toString();
            if (type == QStringLiteral("changes")) {
                ++changeEvents; const auto id = event.value("tool_call_id").toString(); QVERIFY(ends.contains(id));
                auto expectedEvent = resultById.value(id).toObject().value("changes").toObject();
                expectedEvent.insert("type", "changes"); expectedEvent.insert("tool_call_id", id);
                QCOMPARE(event, expectedEvent); continue;
            }
            QVERIFY(type == QStringLiteral("tool_start") || type == QStringLiteral("tool_end"));
            const auto id = event.value("id").toString(); QVERIFY(!QUuid(id).isNull());
            auto& ids = type == QStringLiteral("tool_start") ? starts : ends; QVERIFY(!ids.contains(id)); ids.insert(id);
            if (type == QStringLiteral("tool_start")) {
                orderedCalls.append(id); nameById.insert(id, event.value("tool_name")); titleById.insert(id, event.value("title"));
            } else {
                QVERIFY(starts.contains(id)); QCOMPARE(event.value("tool_name"), nameById.value(id));
                QCOMPARE(event.value("title"), titleById.value(id)); resultById.insert(id, event.value("details"));
                QCOMPARE(event.value("success").toBool(), event.value("details").toObject().value("ok").toBool());
            }
        }
        QCOMPARE(starts, ends); QCOMPARE(changeEvents, sounds ? 1 : 0); QCOMPARE(conversation->ToolCount(), starts.size());
        QCOMPARE(starts.size(), sounds ? qsizetype{6} : qsizetype{8});
        QCOMPARE(conversation->findChildren<QWidget*>(QStringLiteral("agentInlineChanges")).size(), sounds ? qsizetype{3} : qsizetype{0});
        int actualCalls = 0;
        for (const auto& value : exchanges) {
            const auto exchange = value.toObject(); const auto request = exchange.value("request").toObject();
            if (request.value("method") != "tools/call") continue;
            ++actualCalls; const auto ids = exchange.value("broker_call_ids").toArray(); QCOMPARE(ids.size(), 1);
            const auto id = ids.first().toString(); QVERIFY(starts.contains(id));
            QCOMPARE(nameById.value(id), request.value("params").toObject().value("name"));
            const auto content = exchange.value("response").toObject().value("result").toObject().value("content").toArray(); QCOMPARE(content.size(), 1);
            QCOMPARE(QJsonDocument::fromJson(content.first().toObject().value("text").toString().toUtf8()).object(), resultById.value(id).toObject());
            if (imports && nameById.value(id) != "editor_state") {
                const auto result = resultById.value(id).toObject(); QJsonArray rows = result.value("rows").toArray();
                if (result.contains("row")) rows.append(result.value("row"));
                for (const auto& rowValue : rows) {
                    const auto row = rowValue.toObject(); QVERIFY(row.value("payload_opaque").toBool());
                    for (const auto* key : {"text", "data", "base64", "content", "payload"}) QVERIFY(!row.contains(key));
                }
            }
        }
        QCOMPARE(actualCalls, static_cast<int>(starts.size()));
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page));
        QApplication::processEvents();
        const auto toggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
        const auto trees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QCOMPARE(toggles.size(), starts.size()); QCOMPARE(trees.size(), starts.size());
        const auto child = [](QTreeWidgetItem* parent, const QString& key) -> QTreeWidgetItem* {
            for (int i = 0; parent && i < parent->childCount(); ++i) if (parent->child(i)->text(0) == key) return parent->child(i);
            return nullptr;
        };
        const QJsonObject shortTitles{{"editor_state", "读取编辑器状态"}, {"sounds_list", "查询声音定义"}, {"sounds_get", "读取声音定义"},
            {"sounds_update", "修改声音参数"}, {"imports_list", "查询导入资源"}, {"imports_get", "读取导入信息"}};
        for (qsizetype i = 0; i < trees.size(); ++i) {
            auto* details = child(trees.at(i)->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(details);
            const auto id = orderedCalls.at(i);
            QCOMPARE(details->data(0, Qt::UserRole + 1).value<QJsonValue>(), resultById.value(id));
            auto* card = trees.at(i)->parentWidget()->parentWidget();
            auto* title = card->findChild<QLabel*>(QStringLiteral("agentToolTitle")); QVERIFY(title);
            QCOMPARE(title->text(), shortTitles.value(nameById.value(id).toString()).toString());
            auto* description = child(trees.at(i)->invisibleRootItem(), QStringLiteral("说明")); QVERIFY(description);
            QCOMPARE(description->text(1), titleById.value(id).toString());
        }
        const auto captureRoot = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
        const auto capturePath = captureRoot.isEmpty() ? QString{} : QDir(captureRoot).filePath(sounds ? QStringLiteral("sounds") : QStringLiteral("imports"));
        if (!capturePath.isEmpty()) QVERIFY(QDir().mkpath(capturePath));
        QJsonArray captures;
        for (const int width : {1280, 1920}) {
            window.resize(width, 1080); QTRY_COMPARE(window.width(), width);
            for (const auto& viewValue : views) {
                const auto view = viewValue.toObject(); const auto id = view.value("call_id").toString(); const auto index = orderedCalls.indexOf(id);
                QVERIFY(index >= 0);
                for (auto* toggle : toggles) toggle->setChecked(false);
                auto* tree = trees.at(index); toggles.at(index)->setChecked(true); QTRY_VERIFY(tree->isVisible());
                tree->collapseAll(); auto* details = child(tree->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(details);
                auto* item = details;
                for (const auto& key : view.value("path").toArray()) {
                    item->setExpanded(true); item = child(item, key.toString()); QVERIFY2(item, qPrintable(explain(view)));
                }
                auto* anchor = item;
                if (!view.value("anchor").toString().isEmpty()) {
                    anchor = child(item->parent(), view.value("anchor").toString()); QVERIFY(anchor);
                }
                tree->setCurrentItem(item);
                auto* preview = tree->parentWidget()->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview")); QVERIFY(preview);
                const auto selectedValue = item->data(0, Qt::UserRole + 1).value<QJsonValue>();
                if (selectedValue.isObject()) QCOMPARE(QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object(), selectedValue.toObject());
                else QCOMPARE(preview->toPlainText(), selectedValue.toString());
                auto* title = tree->parentWidget()->parentWidget()->findChild<QLabel*>(QStringLiteral("agentToolTitle")); QVERIFY(title);
                QApplication::processEvents();
                conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->value()
                    + title->mapTo(conversation->viewport(), QPoint{}).y() - 12);
                auto proof = StabilizeDetailTree(tree, anchor, item, conversation);
                QVERIFY2(proof.value("stable").toBool(), qPrintable(explain(proof)));
                const auto rect = [&](QWidget* widget) { return QRect(widget->mapTo(conversation->viewport(), QPoint{}), widget->size()); };
                const auto titleRect = rect(title), treeRect = rect(tree), previewRect = rect(preview);
                QVERIFY(conversation->viewport()->rect().contains(titleRect)); QVERIFY(conversation->viewport()->rect().contains(previewRect));
                QVERIFY(!titleRect.intersects(treeRect) && !titleRect.intersects(previewRect) && !treeRect.intersects(previewRect));
                const auto pixmap = window.grab(); QVERIFY(!pixmap.isNull());
                QCOMPARE(pixmap.width(), qRound(window.width() * window.devicePixelRatioF()));
                QCOMPARE(pixmap.height(), qRound(window.height() * window.devicePixelRatioF()));
                proof.insert("tool_call_id", id); proof.insert("tool_name", nameById.value(id)); proof.insert("tool_title", title->text());
                proof.insert("selected_path", view.value("path")); proof.insert("selected_value", selectedValue);
                proof.insert("host_width", window.width()); proof.insert("host_height", window.height());
                proof.insert("agent_width", agent->width()); proof.insert("agent_height", agent->height());
                proof.insert("dpr", window.devicePixelRatioF()); proof.insert("pixel_width", pixmap.width()); proof.insert("pixel_height", pixmap.height());
                proof.insert("title_and_preview_visible", true);
                if (!capturePath.isEmpty()) {
                    const auto fileName = (realMap ? QStringLiteral("real-") : QStringLiteral("synthetic-"))
                        + view.value("name").toString() + QStringLiteral("-%1.png").arg(width);
                    const auto path = QDir(capturePath).filePath(fileName); QVERIFY(!QFile::exists(path)); QVERIFY(pixmap.save(path));
                    proof.insert("file", fileName);
                }
                captures.append(proof);
            }
        }
        if (!capturePath.isEmpty()) {
            const auto exeSha = War3Map::Sha256File(NativePath(QCoreApplication::applicationFilePath())); QVERIFY(exeSha.Succeeded());
            const QJsonObject receipt{{"schema", "e0-e1-native-http-ui-v1"}, {"map_kind", realMap ? "actualMap" : "synthetic"},
                {"fixture_only", !realMap}, {"real_http", true}, {"same_gui_document", true}, {"same_original_undo_stack", true},
                {"source_path", QString::fromStdWString(source.wstring())}, {"source_sha256", sourceSha},
                {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
                {"source_bytes_unchanged", ReadFile(source) == original}, {"all_map_members_restored", true}, {"trigger_bytes_unchanged", true},
                {"object_documents_unchanged", true}, {"imp_bytes_unchanged", true}, {"operation", operation},
                {"map_save_attempted", false}, {"map_save_verified", false}, {"native_desktop_capture", false}, {"native_titlebar_in_image", false},
                {"platform", QGuiApplication::platformName()}, {"test_executable", QCoreApplication::applicationFilePath()},
                {"test_executable_sha256", QString::fromStdString(*exeSha.value)}, {"tool_card_count", conversation->ToolCount()},
                {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
            const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented); QVERIFY(!bytes.contains(token));
            QFile file(QDir(capturePath).filePath(realMap ? QStringLiteral("real-http-receipt.json") : QStringLiteral("synthetic-http-receipt.json")));
            QVERIFY(!file.exists()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
        }
        return;
    }
    if (entityFilters) {
        auto* mapEdits = project->MapEdits(); auto* mapUndo = project->mapDataUndoStack_;
        auto* mapDocument = project->mapDocument_.get();
        auto* triggerDocument = project->triggerDocument_.get(); auto* triggerUndo = project->triggerUndoStack_;
        QVERIFY(mapEdits && mapUndo && mapDocument && triggerDocument && triggerUndo && project->uiCatalog_);
        const auto projected = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(projected.Succeeded());
        const auto& baseline = *projected.value; QVERIFY(baseline.Units());
        const auto& units = baseline.Units()->units;
        QVERIFY2(units.size() <= 4096, "This bounded GUI acceptance fixture must fit the native scan budget");
        const auto originalMapRevision = mapEdits->Revision();
        const auto originalProjectRevision = project->editRevision_.Revision();
        const auto mapIndex = mapUndo->index(), mapCount = mapUndo->count();
        const auto triggerIndex = triggerUndo->index(), triggerCount = triggerUndo->count();
        const auto triggerBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_); QVERIFY(triggerBytes.Succeeded());
        std::array<const LniObjectDocument*, 7> objects{}; std::array<std::string, 7> fingerprints{};
        std::array<QUndoStack*, 7> objectUndos{}; std::array<int, 7> objectIndices{}, objectCounts{};
        for (std::size_t i = 0; i < objects.size(); ++i) {
            objects[i] = project->objectDocuments_[i].get(); if (objects[i]) fingerprints[i] = objects[i]->DocumentFingerprint();
            objectUndos[i] = project->objectUndoStacks_[i];
            if (objectUndos[i]) { objectIndices[i] = objectUndos[i]->index(); objectCounts[i] = objectUndos[i]->count(); }
        }
        const auto sameMembers = [](const War3Map::MapDocument& a, const War3Map::MapDocument& b) {
            if (a.Members().size() != b.Members().size()) return false;
            for (std::size_t i = 0; i < a.Members().size(); ++i) {
                const auto& left = a.Members()[i]; const auto& right = b.Members()[i];
                if (left.state != right.state || left.memberName != right.memberName || left.sources.size() != right.sources.size()) return false;
                for (std::size_t j = 0; j < left.sources.size(); ++j)
                    if (left.sources[j].name != right.sources[j].name || left.sources[j].data != right.sources[j].data) return false;
            }
            return true;
        };
        const auto unchanged = [&] {
            const auto current = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
            return current.Succeeded() && sameMembers(*current.value, baseline)
                && project->MapEdits() == mapEdits && project->mapDataUndoStack_ == mapUndo
                && project->mapDocument_.get() == mapDocument && sameMembers(*mapDocument, baseline) && mapEdits->Revision() == originalMapRevision
                && project->editRevision_.Revision() == originalProjectRevision && mapUndo->index() == mapIndex && mapUndo->count() == mapCount;
        };
        const auto protectReadOnly = qScopeGuard([&] { QVERIFY(unchanged()); QCOMPARE(ReadFile(source), original); });
        const auto explain = [](const QJsonObject& value) { return QString::fromUtf8(QJsonDocument(value).toJson()); };
        const auto canonicalType = [](const std::string& rawcode) {
            return QStringLiteral("hex:") + QString::fromLatin1(QByteArray(rawcode.data(), static_cast<qsizetype>(rawcode.size())).toHex());
        };
        std::size_t selected = units.size();
        for (std::size_t i = 0; i < units.size(); ++i) {
            const auto& candidate = units[i];
            const auto text = QString::fromUtf8(candidate.rawcode.data(), static_cast<qsizetype>(candidate.rawcode.size()));
            if (text.toUtf8().toStdString() != candidate.rawcode || candidate.rawcode.size() != 4) continue;
            if (std::none_of(candidate.rawcode.begin(), candidate.rawcode.end(), [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); })) continue;
            if (std::count_if(units.begin(), units.end(), [&](const auto& row) {
                return row.rawcode == candidate.rawcode && row.owner == candidate.owner;
            }) >= 2) { selected = i; break; }
        }
        QVERIFY2(selected < units.size(), "Real pagination requires two actual units with the same exact rawcode and owner");
        const auto& chosen = units[selected];
        const auto literalType = QString::fromUtf8(chosen.rawcode.data(), 4);
        const QJsonObject combined{{"type_id", literalType}, {"player", chosen.owner}};
        const QJsonObject canonicalCombined{{"type_id", canonicalType(chosen.rawcode)}, {"player", chosen.owner}};
        const auto matching = [&](const std::optional<std::string>& rawcode, const std::optional<std::int32_t>& player) {
            std::vector<std::size_t> indices;
            for (std::size_t i = 0; i < units.size(); ++i)
                if ((!rawcode || units[i].rawcode == *rawcode) && (!player || units[i].owner == *player)) indices.push_back(i);
            return indices;
        };
        const auto expectedCombined = matching(chosen.rawcode, chosen.owner);
        const auto query = [&](const QJsonObject& filter, const QJsonObject& page) {
            return call(QStringLiteral("entities_query"), {{"kind", "units"}, {"filter", filter}, {"page", page}});
        };
        bool pageChecked = false;
        const auto checkPage = [&](const QJsonObject& result, const QJsonObject& canonicalFilter,
            const std::vector<std::size_t>& expected, std::size_t offset, std::size_t limit) {
            pageChecked = false;
            QVERIFY2(result.value("ok").toBool(), qPrintable(explain(result)));
            QCOMPARE(result.value("filter").toObject(), canonicalFilter);
            QVERIFY(result.value("matched_total_complete").toBool());
            QCOMPARE(result.value("matched_total").toString(), QString::number(expected.size()));
            QVERIFY(result.value("examined").isString());
            bool examinedValid = false; const auto examined = result.value("examined").toString().toULongLong(&examinedValid);
            QVERIFY(examinedValid); QCOMPARE(examined, static_cast<qulonglong>(units.size()));
            QVERIFY(result.value("scan_budget_exhausted").isBool());
            QVERIFY(!result.value("scan_budget_exhausted").toBool());
            const auto count = std::min(limit, expected.size() - offset);
            const auto rows = result.value("rows").toArray(); QCOMPARE(rows.size(), static_cast<qsizetype>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const auto sourceIndex = expected[offset + i]; const auto& native = units[sourceIndex];
                const auto row = rows.at(static_cast<qsizetype>(i)).toObject();
                QCOMPARE(row.value("source_index").toString(), QString::number(sourceIndex));
                QCOMPARE(row.value("rawcode_hex").toString(), canonicalType(native.rawcode).mid(4));
                QVERIFY(row.value("owner").isDouble() && row.value("creation_number").isDouble());
                QCOMPARE(row.value("owner").toInt(), native.owner);
                QCOMPARE(row.value("creation_number").toInt(), native.creationId);
                QVERIFY(!row.value("row_key").toString().isEmpty());
                QCOMPARE(row.value("locator_scope").toString(), QStringLiteral("snapshot-revision"));
                const auto position = row.value("position_world").toArray(); QCOMPARE(position.size(), 3);
                for (int axis = 0; axis < 3; ++axis) QCOMPARE(position.at(axis).toDouble(), static_cast<double>(native.position[axis]));
            }
            const bool more = offset + count < expected.size();
            QVERIFY(result.value("truncated").isBool());
            QCOMPARE(result.value("truncated").toBool(), more);
            if (more) {
                QVERIFY(result.value("next_cursor").isObject());
                const auto cursor = result.value("next_cursor").toObject();
                QCOMPARE(cursor.value("filter").toObject(), canonicalFilter);
                QCOMPARE(cursor.value("kind").toString(), QStringLiteral("units"));
                QCOMPARE(cursor.value("binding"), result.value("binding"));
                QCOMPARE(cursor.value("member_sha256"), result.value("member").toObject().value("member_sha256"));
                bool valid = false; const auto next = cursor.value("next_index").toString().toULongLong(&valid);
                QVERIFY(valid && next > expected[offset + count - 1] && next <= expected[offset + count]);
            } else QVERIFY(result.value("next_cursor").isNull());
            QVERIFY(unchanged());
            pageChecked = true;
        };
        const auto first = query(combined, {{"limit", 1}});
        checkPage(first, canonicalCombined, expectedCombined, 0, 1); QVERIFY(pageChecked);
        const auto cursor = first.value("next_cursor").toObject();
        const auto second = query(combined, {{"limit", 1}, {"cursor", cursor}});
        checkPage(second, canonicalCombined, expectedCombined, 1, 1); QVERIFY(pageChecked);
        QVERIFY(first.value("rows").toArray().first().toObject().value("source_index") != second.value("rows").toArray().first().toObject().value("source_index"));
        const auto typeOnly = query({{"type_id", literalType}}, {{"limit", 16}});
        checkPage(typeOnly, {{"type_id", canonicalType(chosen.rawcode)}}, matching(chosen.rawcode, std::nullopt), 0, 16); QVERIFY(pageChecked);
        const auto playerOnly = query({{"player", chosen.owner}}, {{"limit", 16}});
        checkPage(playerOnly, {{"player", chosen.owner}}, matching(std::nullopt, chosen.owner), 0, 16); QVERIFY(pageChecked);
        const auto hexEquivalent = query(canonicalCombined, {{"limit", 1}});
        checkPage(hexEquivalent, canonicalCombined, expectedCombined, 0, 1); QVERIFY(pageChecked);
        QCOMPARE(hexEquivalent.value("rows"), first.value("rows"));
        auto changedCase = chosen.rawcode;
        for (auto& c : changedCase) {
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); break; }
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); break; }
        }
        const auto caseSensitive = query({{"type_id", QString::fromUtf8(changedCase.data(), 4)}, {"player", chosen.owner}}, {{"limit", 16}});
        checkPage(caseSensitive, {{"type_id", canonicalType(changedCase)}, {"player", chosen.owner}}, matching(changedCase, chosen.owner), 0, 16); QVERIFY(pageChecked);
        std::string absent(4, '\0');
        while (std::any_of(units.begin(), units.end(), [&](const auto& row) { return row.rawcode == absent; })) {
            for (auto& c : absent) { c = static_cast<char>(static_cast<unsigned char>(c) + 1); if (c != 0) break; }
        }
        const auto empty = query({{"type_id", canonicalType(absent)}}, {{"limit", 16}});
        checkPage(empty, {{"type_id", canonicalType(absent)}}, {}, 0, 16); QVERIFY(pageChecked);
        // 有效 cursor 配合不同过滤条件，仅证明 filter 绑定拒绝，不冒充文档 revision 过期验证。
        const auto rejected = query({{"type_id", canonicalType(absent)}}, {{"limit", 1}, {"cursor", cursor}});
        QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!rejected.value("diagnostics").toArray().isEmpty());
        QCOMPARE(rejected.value("diagnostics").toArray().first().toObject().value("code").toString(), QStringLiteral("ENTITY_QUERY_CURSOR_STALE"));
        QVERIFY(unchanged());
        QCOMPARE(project->triggerDocument_.get(), triggerDocument); QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(triggerUndo->index(), triggerIndex); QCOMPARE(triggerUndo->count(), triggerCount);
        const auto finalTriggers = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_); QVERIFY(finalTriggers.Succeeded());
        QVERIFY(finalTriggers.value->wtg == triggerBytes.value->wtg && finalTriggers.value->wct == triggerBytes.value->wct);
        for (std::size_t i = 0; i < objects.size(); ++i) {
            QCOMPARE(project->objectDocuments_[i].get(), objects[i]); if (objects[i]) QCOMPARE(objects[i]->DocumentFingerprint(), fingerprints[i]);
            QCOMPARE(project->objectUndoStacks_[i], objectUndos[i]);
            if (objectUndos[i]) { QCOMPARE(objectUndos[i]->index(), objectIndices[i]); QCOMPARE(objectUndos[i]->count(), objectCounts[i]); }
        }
        QCOMPARE(ReadFile(source), original);
        QJsonArray events; QSet<QString> starts, ends; QStringList orderedCalls;
        for (const auto& arguments : activity) {
            const auto event = arguments.first().toJsonObject(); events.append(event);
            const auto type = event.value("type").toString();
            if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
            const auto id = event.value("id").toString(); QVERIFY(!QUuid(id).isNull());
            auto& ids = type == QStringLiteral("tool_start") ? starts : ends; QVERIFY(!ids.contains(id)); ids.insert(id);
            if (type == QStringLiteral("tool_start")) orderedCalls.append(id);
        }
        QCOMPARE(starts, ends); QCOMPARE(starts.size(), 9); QCOMPARE(conversation->ToolCount(), 9);
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page)); window.resize(1280, 1000);
        const auto toggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
        const auto trees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QCOMPARE(toggles.size(), 9); QCOMPARE(trees.size(), 9);
        auto* tree = trees.at(1); toggles.at(1)->setChecked(true); QTRY_VERIFY(tree->isVisible());
        const auto child = [](QTreeWidgetItem* parent, const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; parent && i < parent->childCount(); ++i) if (parent->child(i)->text(0) == text) return parent->child(i);
            return nullptr;
        };
        const auto captureRoot = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
        const auto capturePath = captureRoot.isEmpty() ? QString{} : QDir(captureRoot).filePath(QStringLiteral("entity-filters"));
        if (!capturePath.isEmpty()) QVERIFY(QDir().mkpath(capturePath));
        QJsonArray captures; bool captured = false;
        const auto capture = [&](const QString& suffix) {
            captured = false; tree->collapseAll();
            auto* details = child(tree->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(details); details->setExpanded(true);
            auto* rows = child(details, QStringLiteral("rows")); QVERIFY(rows); rows->setExpanded(true);
            auto* row = child(rows, QStringLiteral("[0]")); QVERIFY(row); row->setExpanded(true);
            auto* selectedItem = child(row, QStringLiteral("owner")); QVERIFY(selectedItem);
            tree->setCurrentItem(selectedItem);
            auto* card = tree->parentWidget()->parentWidget();
            auto* title = card->findChild<QLabel*>(QStringLiteral("agentToolTitle")); QVERIFY(title);
            QCOMPARE(title->text(), QStringLiteral("查询地图实体"));
            auto* preview = tree->parentWidget()->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview")); QVERIFY(preview);
            QCOMPARE(preview->toPlainText(), QString::number(chosen.owner));
            QApplication::processEvents();
            conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->value() + title->mapTo(conversation->viewport(), QPoint{}).y() - 12);
            auto proof = StabilizeDetailTree(tree, selectedItem, selectedItem, conversation);
            QVERIFY2(proof.value("stable").toBool(), qPrintable(explain(proof)));
            const auto rect = [&](QWidget* widget) { return QRect(widget->mapTo(conversation->viewport(), QPoint{}), widget->size()); };
            const auto titleRect = rect(title), treeRect = rect(tree), previewRect = rect(preview);
            QVERIFY(conversation->viewport()->rect().contains(titleRect)); QVERIFY(conversation->viewport()->rect().contains(previewRect));
            QVERIFY(!titleRect.intersects(treeRect) && !titleRect.intersects(previewRect) && !treeRect.intersects(previewRect));
            const auto jsonRect = [](const QRect& r) { return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}; };
            proof.insert("title_rect", jsonRect(titleRect)); proof.insert("tree_rect", jsonRect(treeRect)); proof.insert("preview_rect", jsonRect(previewRect));
            proof.insert("expanded_tool_id", orderedCalls.at(1)); proof.insert("selected_result_field", "rows[0].owner");
            proof.insert("source_index", first.value("rows").toArray().first().toObject().value("source_index")); proof.insert("filter", canonicalCombined);
            proof.insert("host_width", window.width()); proof.insert("host_height", window.height()); proof.insert("agent_width", agent->width());
            proof.insert("agent_height", agent->height()); proof.insert("mode", static_cast<int>(agent->Mode())); proof.insert("dpr", window.devicePixelRatioF());
            if (!capturePath.isEmpty()) {
                const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + suffix + QStringLiteral(".png");
                const auto path = QDir(capturePath).filePath(fileName); QVERIFY(!QFile::exists(path)); QVERIFY(window.grab().save(path)); proof.insert("file", fileName);
            }
            captures.append(proof); captured = true;
        };
        capture(QStringLiteral("entity-filters-page-1280")); QVERIFY(captured);
        window.resize(1920, 1080); window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Sidebar));
        QTRY_COMPARE(agent->Mode(), AgentWindow::PresentationMode::Sidebar);
        auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock")); QVERIFY(sidebar);
        window.resizeDocks({sidebar}, {420}, Qt::Horizontal); QTRY_COMPARE(agent->width(), 420);
        capture(QStringLiteral("entity-filters-sidebar-420")); QVERIFY(captured);
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page)); window.resize(1920, 1080);
        capture(QStringLiteral("entity-filters-page-1920")); QVERIFY(captured);
        if (!capturePath.isEmpty()) {
            QJsonArray memberProof;
            const auto current = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(current.Succeeded());
            for (std::size_t i = 0; i < baseline.Members().size(); ++i) {
                QJsonArray priorHashes, finalHashes;
                const auto hash = [](const auto& data) { return QString::fromLatin1(QCryptographicHash::hash(
                    QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())), QCryptographicHash::Sha256).toHex()); };
                for (const auto& member : baseline.Members()[i].sources) priorHashes.append(hash(member.data));
                for (const auto& member : current.value->Members()[i].sources) finalHashes.append(hash(member.data));
                memberProof.append(QJsonObject{{"member", QString::fromStdString(baseline.Members()[i].memberName)},
                    {"before_state", static_cast<int>(baseline.Members()[i].state)}, {"after_state", static_cast<int>(current.value->Members()[i].state)},
                    {"before_sha256", priorHashes}, {"after_sha256", finalHashes}});
            }
            const QJsonObject receipt{{"map_kind", realMap ? "actualMap" : "synthetic"}, {"fixture_only", !realMap},
                {"real_http", true}, {"filter", canonicalCombined}, {"native_unit_count", QString::number(units.size())},
                {"native_matched_count", QString::number(expectedCombined.size())}, {"first_page", first}, {"second_page", second},
                {"cursor_filter_mismatch_rejected", true}, {"document_revision_stale_cursor_tested", false},
                {"read_only", true}, {"same_map_edits", true}, {"same_map_data_undo_stack", true}, {"all_original_documents_unchanged", true},
                {"source_sha256", sourceSha}, {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
                {"source_bytes_unchanged", ReadFile(source) == original}, {"member_proof", memberProof},
                {"map_save_attempted", false}, {"runtime_verified", false}, {"native_desktop_capture", false}, {"platform", QGuiApplication::platformName()},
                {"tool_card_count", conversation->ToolCount()}, {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
            const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented); QVERIFY(!bytes.contains(token));
            QFile file(QDir(capturePath).filePath((realMap ? QStringLiteral("real-") : QString{}) + QStringLiteral("entity-filters-http-receipt.json")));
            QVERIFY(!file.exists()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
        }
        return;
    }
    if (entityPosition) {
        auto* mapEdits = project->MapEdits(); auto* mapUndo = project->mapDataUndoStack_;
        auto* mapDocument = project->mapDocument_.get();
        auto* triggerDocument = project->triggerDocument_.get(); auto* triggerUndo = project->triggerUndoStack_;
        QVERIFY(mapEdits && mapUndo && mapDocument && triggerDocument && triggerUndo && project->uiCatalog_);
        const auto sourceProjection = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(sourceProjection.Succeeded());
        const auto& sourceMap = *sourceProjection.value; QVERIFY(sourceMap.Units());
        QCOMPARE(sourceMap.Units()->recordLayout, War3Map::MapData::PlacementRecordLayout::Classic);
        const auto sourceMapIndex = mapUndo->index(), sourceMapCount = mapUndo->count();
        const auto triggerIndex = triggerUndo->index(), triggerCount = triggerUndo->count();
        const auto triggerBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_); QVERIFY(triggerBytes.Succeeded());
        std::array<const LniObjectDocument*, 7> objects{}; std::array<std::string, 7> fingerprints{};
        std::array<QUndoStack*, 7> objectUndos{}; std::array<int, 7> objectIndices{}, objectCounts{};
        for (std::size_t i = 0; i < objects.size(); ++i) {
            objects[i] = project->objectDocuments_[i].get(); if (objects[i]) fingerprints[i] = objects[i]->DocumentFingerprint();
            objectUndos[i] = project->objectUndoStacks_[i];
            if (objectUndos[i]) { objectIndices[i] = objectUndos[i]->index(); objectCounts[i] = objectUndos[i]->count(); }
        }
        const auto sameMembers = [](const War3Map::MapDocument& a, const War3Map::MapDocument& b, bool exceptUnits = false) {
            if (a.Members().size() != b.Members().size()) return false;
            for (std::size_t i = 0; i < a.Members().size(); ++i) {
                if (exceptUnits && a.Members()[i].kind == War3Map::MapDataKind::Units) continue;
                const auto& left = a.Members()[i]; const auto& right = b.Members()[i];
                if (left.state != right.state || left.memberName != right.memberName || left.sources.size() != right.sources.size()) return false;
                for (std::size_t j = 0; j < left.sources.size(); ++j)
                    if (left.sources[j].name != right.sources[j].name || left.sources[j].data != right.sources[j].data) return false;
            }
            return true;
        };
        const auto matches = [&](const War3Map::MapDocument& expected) {
            const auto current = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
            return current.Succeeded() && sameMembers(*current.value, expected);
        };
        const auto restoreSource = qScopeGuard([&] {
            while (mapUndo->index() > sourceMapIndex) mapUndo->undo();
            QVERIFY(matches(sourceMap)); QCOMPARE(ReadFile(source), original);
        });
        const auto selected = std::find_if(sourceMap.Units()->units.begin(), sourceMap.Units()->units.end(), [](const auto& row) {
            return row.rawcode != "sloc" && !row.skinId && std::all_of(row.position.begin(), row.position.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) < 999990.0f;
            });
        });
        QVERIFY(selected != sourceMap.Units()->units.end());
        const auto sourceIndex = static_cast<std::size_t>(selected - sourceMap.Units()->units.begin());
        // 模拟已存在的作者编辑，但使用真实原会话及正式 Undo 命令；不制造 Agent 工具卡。
        auto authorUnits = *sourceMap.Units();
        authorUnits.units[sourceIndex].rotation = selected->rotation == 1.25f ? 2.5f : 1.25f;
        QVERIFY(mapEdits->EditUnits(authorUnits).Succeeded());
        War3Map::QtProject::PushMapDataEdit(*project, QStringLiteral("已有作者朝向编辑"), "C4 前置作者编辑保护夹具");
        QVERIFY(project->ObserveMapDataEdit("C4 前置作者编辑保护夹具"));
        const auto authorIndex = mapUndo->index(), authorCount = mapUndo->count();
        QCOMPARE(authorIndex, sourceMapIndex + 1); QCOMPARE(authorCount, sourceMapCount + 1);
        const auto* authorCommand = mapUndo->command(authorIndex - 1); QVERIFY(authorCommand);
        const auto authorCommandId = mapEdits->LastAppliedEdit().CommandIdentity();
        const auto authorProjection = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(authorProjection.Succeeded());
        const auto& authorMap = *authorProjection.value; QVERIFY(sameMembers(sourceMap, authorMap, true));
        const auto explain = [](const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson()); };
        const auto field = [](const QJsonObject& row, const QString& id) {
            for (const auto& value : row.value("fields").toArray()) if (value.toObject().value("id").toString() == id) return value.toObject();
            return QJsonObject{};
        };
        const auto query = [&] { return call(QStringLiteral("entities_query"), {{"kind", "units"},
            {"page", QJsonObject{{"start_index", QString::number(sourceIndex)}, {"limit", 1}}}}); };
        const auto queried = query(); QVERIFY2(queried.value("ok").toBool(), qPrintable(explain(queried)));
        const auto beforeRow = queried.value("rows").toArray().first().toObject();
        QCOMPARE(beforeRow.value("source_index").toString(), QString::number(sourceIndex));
        const std::array<QString, 3> coordinateFields{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")};
        std::array<float, 3> coordinates{}; QJsonArray changes;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto prior = field(beforeRow, coordinateFields[axis]); QVERIFY(prior.value("writable").toBool());
            QCOMPARE(prior.value("value").toDouble(), static_cast<double>(selected->position[axis]));
            coordinates[axis] = selected->position[axis] + static_cast<float>(axis + 1);
            changes.append(QJsonObject{{"source_index", QString::number(sourceIndex)}, {"row_key", beforeRow.value("row_key")},
                {"field", coordinateFields[axis]}, {"expected_raw", prior.value("raw")}, {"value", QString::number(coordinates[axis], 'g', 9)}});
        }
        const QJsonObject edit{{"kind", "units"}, {"project_identity", queried.value("project_identity")}, {"expected_revision", queried.value("revision")},
            {"expected_member_sha256", queried.value("member").toObject().value("member_sha256")}, {"changes", changes}};
        const auto edited = call(QStringLiteral("entities_set_fields"), edit); QVERIFY2(edited.value("ok").toBool(), qPrintable(explain(edited)));
        QCOMPARE(edited.value("session_identity").toString().toULongLong(), mapEdits->SessionIdentity());
        QCOMPARE(edited.value("command_identity").toString().toULongLong(), mapEdits->LastAppliedEdit().CommandIdentity());
        QVERIFY(!edited.value("saved").toBool() && !edited.value("runtime_verified").toBool());
        QCOMPARE(edited.value("applied_fields").toString(), QStringLiteral("3"));
        QCOMPARE(edited.value("changed_rows").toString(), QStringLiteral("1"));
        const auto beforeRows = edited.value("before").toObject().value("rows").toArray(), afterRows = edited.value("after").toObject().value("rows").toArray();
        QCOMPARE(beforeRows.size(), 1); QCOMPARE(afterRows.size(), 1); QCOMPARE(beforeRows.first().toObject(), beforeRow);
        const auto afterRow = afterRows.first().toObject();
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto actual = field(afterRow, coordinateFields[axis]);
            QCOMPARE(actual.value("value").toDouble(), static_cast<double>(coordinates[axis]));
            QCOMPARE(actual.value("raw").toString(), QStringLiteral("f32:%1").arg(std::bit_cast<std::uint32_t>(coordinates[axis]), 8, 16, QLatin1Char('0')));
        }
        const auto items = edited.value("changes").toObject().value("items").toArray(); QCOMPARE(items.size(), 3);
        for (const auto& item : items) {
            QVERIFY(item.toObject().value("object_id").isString()); QVERIFY(item.toObject().value("object_id").toString().isEmpty());
        }
        QCOMPARE(mapUndo->index(), authorIndex + 1); QCOMPARE(mapUndo->count(), authorCount + 1);
        QCOMPARE(mapUndo->command(authorIndex - 1), authorCommand);
        const auto afterProjection = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(afterProjection.Succeeded());
        const auto& afterMap = *afterProjection.value; QVERIFY(sameMembers(authorMap, afterMap, true));
        auto expectedUnits = *authorMap.Units(); expectedUnits.units[sourceIndex].position = coordinates;
        const auto expectedWire = expectedUnits.Serialize(), actualWire = afterMap.Units()->Serialize();
        QVERIFY(expectedWire.Succeeded() && actualWire.Succeeded()); QVERIFY(*expectedWire.value == *actualWire.value);
        const auto active = query(); QVERIFY(active.value("ok").toBool()); QCOMPARE(active.value("rows").toArray().first().toObject(), afterRow);
        const auto afterRevision = mapEdits->Revision(); const auto afterProjectRevision = project->editRevision_.Revision();
        const auto stale = call(QStringLiteral("entities_set_fields"), edit); QVERIFY(!stale.value("ok").toBool());
        QCOMPARE(stale.value("diagnostics").toArray().first().toObject().value("code").toString(), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
        QVERIFY(matches(afterMap)); QCOMPARE(mapUndo->index(), authorIndex + 1); QCOMPARE(mapUndo->count(), authorCount + 1);
        QCOMPARE(mapEdits->Revision(), afterRevision); QCOMPARE(project->editRevision_.Revision(), afterProjectRevision);
        auto invalid = edit; invalid.insert("expected_revision", active.value("revision"));
        invalid.insert("expected_member_sha256", active.value("member").toObject().value("member_sha256"));
        QJsonArray invalidChanges;
        for (std::size_t axis = 0; axis < 3; ++axis) invalidChanges.append(QJsonObject{{"source_index", QString::number(sourceIndex)},
            {"row_key", afterRow.value("row_key")}, {"field", coordinateFields[axis]}, {"expected_raw", field(afterRow, coordinateFields[axis]).value("raw")},
            {"value", axis == 2 ? QStringLiteral("1000000.001") : QString::number(coordinates[axis] + 4.0f, 'g', 9)}});
        invalid.insert("changes", invalidChanges);
        const auto rejected = call(QStringLiteral("entities_set_fields"), invalid); QVERIFY(!rejected.value("ok").toBool());
        QVERIFY(!rejected.value("diagnostics").toArray().isEmpty());
        QCOMPARE(rejected.value("diagnostics").toArray().first().toObject().value("code").toString(), QStringLiteral("MAP_FIELD_INPUT_INVALID"));
        QVERIFY(matches(afterMap)); QCOMPARE(mapUndo->index(), authorIndex + 1); QCOMPARE(mapUndo->count(), authorCount + 1);
        QCOMPARE(mapEdits->Revision(), afterRevision); QCOMPARE(project->editRevision_.Revision(), afterProjectRevision);
        mapUndo->undo(); QVERIFY(matches(authorMap)); QCOMPARE(query().value("rows").toArray().first().toObject(), beforeRow);
        mapUndo->redo(); QVERIFY(matches(afterMap)); QCOMPARE(query().value("rows").toArray().first().toObject(), afterRow);
        mapUndo->undo(); QVERIFY(matches(authorMap)); QCOMPARE(query().value("rows").toArray().first().toObject(), beforeRow);
        QCOMPARE(mapUndo->index(), authorIndex); QCOMPARE(mapUndo->count(), authorCount + 1);
        QCOMPARE(mapUndo->command(authorIndex - 1), authorCommand);
        QCOMPARE(project->MapEdits(), mapEdits); QCOMPARE(project->mapDataUndoStack_, mapUndo); QCOMPARE(project->mapDocument_.get(), mapDocument);
        QVERIFY(sameMembers(*mapDocument, sourceMap));
        QCOMPARE(project->triggerDocument_.get(), triggerDocument); QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(triggerUndo->index(), triggerIndex); QCOMPARE(triggerUndo->count(), triggerCount);
        const auto finalTriggers = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_); QVERIFY(finalTriggers.Succeeded());
        QVERIFY(finalTriggers.value->wtg == triggerBytes.value->wtg && finalTriggers.value->wct == triggerBytes.value->wct);
        for (std::size_t i = 0; i < objects.size(); ++i) {
            QCOMPARE(project->objectDocuments_[i].get(), objects[i]); if (objects[i]) QCOMPARE(objects[i]->DocumentFingerprint(), fingerprints[i]);
            QCOMPARE(project->objectUndoStacks_[i], objectUndos[i]);
            if (objectUndos[i]) { QCOMPARE(objectUndos[i]->index(), objectIndices[i]); QCOMPARE(objectUndos[i]->count(), objectCounts[i]); }
        }
        QJsonArray events; QSet<QString> starts, ends; QStringList orderedCalls; QString editCallId;
        for (const auto& arguments : activity) {
            const auto event = arguments.first().toJsonObject(); events.append(event); const auto type = event.value("type").toString();
            if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
            const auto id = event.value("id").toString(); QVERIFY(!QUuid(id).isNull());
            auto& ids = type == QStringLiteral("tool_start") ? starts : ends; QVERIFY(!ids.contains(id)); ids.insert(id);
            if (type == QStringLiteral("tool_start")) {
                orderedCalls.append(id); if (event.value("tool_name").toString() == QStringLiteral("entities_set_fields") && editCallId.isEmpty()) editCallId = id;
            }
        }
        QCOMPARE(starts, ends); QCOMPARE(starts.size(), 9); QCOMPARE(conversation->ToolCount(), 9);
        const auto toggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
        const auto trees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QCOMPARE(toggles.size(), 9); QCOMPARE(trees.size(), 9);
        const auto editIndex = orderedCalls.indexOf(editCallId); QVERIFY(editIndex >= 0);
        auto* tree = trees.at(editIndex); toggles.at(editIndex)->setChecked(true);
        const auto child = [](QTreeWidgetItem* parent, const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; parent && i < parent->childCount(); ++i) if (parent->child(i)->text(0) == text) return parent->child(i);
            return nullptr;
        };
        const auto captureRoot = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
        const auto capturePath = captureRoot.isEmpty() ? QString{} : QDir(captureRoot).filePath(QStringLiteral("entity-position"));
        if (!capturePath.isEmpty()) QVERIFY(QDir().mkpath(capturePath));
        QJsonArray captures; bool captured = false;
        const auto capture = [&](const QString& phase, const QString& selectedField, const QString& suffix) {
            captured = false; tree->collapseAll();
            auto* details = child(tree->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(details); details->setExpanded(true);
            auto* phaseItem = child(details, phase); QVERIFY(phaseItem); phaseItem->setExpanded(true);
            auto* rows = child(phaseItem, QStringLiteral("rows")); QVERIFY(rows); rows->setExpanded(true);
            auto* row = child(rows, QStringLiteral("[0]")); QVERIFY(row); row->setExpanded(true);
            auto* fieldsItem = child(row, QStringLiteral("fields")); QVERIFY(fieldsItem); fieldsItem->setExpanded(true);
            const auto fields = edited.value(phase).toObject().value("rows").toArray().first().toObject().value("fields").toArray();
            qsizetype index = -1;
            for (qsizetype i = 0; i < fields.size(); ++i) if (fields.at(i).toObject().value("id").toString() == selectedField) index = i;
            QVERIFY(index >= 0); auto* selectedItem = child(fieldsItem, QStringLiteral("[%1]").arg(index)); QVERIFY(selectedItem);
            tree->setCurrentItem(selectedItem);
            auto* title = tree->parentWidget()->parentWidget()->findChild<QLabel*>(QStringLiteral("agentToolTitle")); QVERIFY(title);
            QCOMPARE(title->text(), QStringLiteral("修改实体字段"));
            auto* preview = tree->parentWidget()->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview")); QVERIFY(preview);
            QCOMPARE(QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object(), fields.at(index).toObject());
            QApplication::processEvents();
            conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->value() + title->mapTo(conversation->viewport(), QPoint{}).y() - 12);
            auto proof = StabilizeDetailTree(tree, fieldsItem, selectedItem, conversation);
            QVERIFY2(proof.value("stable").toBool(), qPrintable(explain(proof)));
            const auto rect = [&](QWidget* widget) { return QRect(widget->mapTo(conversation->viewport(), QPoint{}), widget->size()); };
            const auto titleRect = rect(title), treeRect = rect(tree), previewRect = rect(preview);
            QVERIFY(conversation->viewport()->rect().contains(titleRect)); QVERIFY(conversation->viewport()->rect().contains(previewRect));
            QVERIFY(!titleRect.intersects(treeRect) && !titleRect.intersects(previewRect) && !treeRect.intersects(previewRect));
            const auto jsonRect = [](const QRect& r) { return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}; };
            proof.insert("title_rect", jsonRect(titleRect)); proof.insert("tree_rect", jsonRect(treeRect)); proof.insert("preview_rect", jsonRect(previewRect));
            proof.insert("expanded_tool_id", editCallId); proof.insert("selected_result_field", phase + ".rows[0].fields." + selectedField);
            proof.insert("field", fields.at(index)); proof.insert("host_width", window.width()); proof.insert("host_height", window.height());
            proof.insert("agent_width", agent->width()); proof.insert("agent_height", agent->height());
            proof.insert("mode", static_cast<int>(agent->Mode())); proof.insert("dpr", window.devicePixelRatioF());
            if (!capturePath.isEmpty()) {
                const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + suffix + QStringLiteral(".png");
                const auto path = QDir(capturePath).filePath(fileName); QVERIFY(!QFile::exists(path)); QVERIFY(window.grab().save(path)); proof.insert("file", fileName);
            }
            captures.append(proof); captured = true;
        };
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page)); window.resize(1280, 1000);
        capture(QStringLiteral("after"), QStringLiteral("x"), QStringLiteral("entity-position-after-1280")); QVERIFY(captured);
        window.resize(1920, 1080); window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Sidebar));
        QTRY_COMPARE(agent->Mode(), AgentWindow::PresentationMode::Sidebar);
        auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock")); QVERIFY(sidebar);
        window.resizeDocks({sidebar}, {420}, Qt::Horizontal); QTRY_COMPARE(agent->width(), 420);
        capture(QStringLiteral("after"), QStringLiteral("y"), QStringLiteral("entity-position-sidebar-420")); QVERIFY(captured);
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page)); window.resize(1920, 1080);
        capture(QStringLiteral("after"), QStringLiteral("z"), QStringLiteral("entity-position-page-1920")); QVERIFY(captured);
        for (const auto* row : conversation->findChildren<QWidget*>(QStringLiteral("agentInlineChanges"))) {
            const auto* jump = row->findChild<QPushButton*>(QStringLiteral("agentInlineNavigate")); QVERIFY(jump && !jump->isVisible());
        }
        // 作者基线保留已在上方验证；此处只清理测试自己建立的前置编辑，恢复原文件态。
        mapUndo->undo(); QCOMPARE(mapUndo->index(), sourceMapIndex); QVERIFY(matches(sourceMap));
        QCOMPARE(mapUndo->command(authorIndex - 1), authorCommand); QCOMPARE(ReadFile(source), original);
        if (!capturePath.isEmpty()) {
            const auto finalProjection = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(finalProjection.Succeeded());
            QJsonArray memberProof;
            const auto hash = [](const auto& bytes) { return QString::fromLatin1(QCryptographicHash::hash(QByteArray(
                reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex()); };
            for (std::size_t i = 0; i < sourceMap.Members().size(); ++i) {
                QJsonArray beforeHashes, afterHashes;
                for (const auto& member : sourceMap.Members()[i].sources) beforeHashes.append(hash(member.data));
                for (const auto& member : finalProjection.value->Members()[i].sources) afterHashes.append(hash(member.data));
                memberProof.append(QJsonObject{{"member", QString::fromStdString(sourceMap.Members()[i].memberName)},
                    {"before_state", static_cast<int>(sourceMap.Members()[i].state)}, {"after_state", static_cast<int>(finalProjection.value->Members()[i].state)},
                    {"before_sha256", beforeHashes}, {"after_sha256", afterHashes}});
            }
            const QJsonObject receipt{{"map_kind", realMap ? "actualMap" : "synthetic"}, {"fixture_only", !realMap}, {"real_http", true},
                {"operation", "entities_set_fields"}, {"fields", QJsonArray{"x", "y", "z"}}, {"source_index", QString::number(sourceIndex)},
                {"before", edited.value("before")}, {"after", edited.value("after")}, {"edit_tool_id", editCallId},
                {"prior_author_command_identity", QString::number(authorCommandId)}, {"prior_author_command_preserved", true},
                {"undo_restored_author_baseline", true}, {"redo_restored_xyz", true}, {"final_undo_restored_author_baseline", true},
                {"test_author_seed_cleanup_restored_source", true}, {"same_map_edits", true}, {"same_map_data_undo_stack", true},
                {"stale_revision_rejected", true}, {"invalid_third_axis_rejected_atomically", true}, {"original_trigger_and_object_documents_unchanged", true},
                {"source_sha256", sourceSha}, {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
                {"source_bytes_unchanged", ReadFile(source) == original}, {"member_proof", memberProof}, {"map_save_attempted", false},
                {"runtime_verified", false}, {"native_desktop_capture", false}, {"platform", QGuiApplication::platformName()},
                {"tool_card_count", conversation->ToolCount()}, {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
            const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented); QVERIFY(!bytes.contains(token));
            QFile file(QDir(capturePath).filePath((realMap ? QStringLiteral("real-") : QString{}) + QStringLiteral("entity-position-http-receipt.json")));
            QVERIFY(!file.exists()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
        }
        return;
    }
    if (entities) {
        auto* mapEdits = project->MapEdits();
        auto* mapUndo = project->mapDataUndoStack_;
        auto* mapDocument = project->mapDocument_.get();
        auto* triggerDocument = project->triggerDocument_.get();
        auto* triggerUndo = project->triggerUndoStack_;
        QVERIFY(mapEdits && mapUndo && mapDocument && triggerDocument && triggerUndo && project->uiCatalog_);
        const auto originalMapIndex = mapUndo->index(), originalMapCount = mapUndo->count();
        const auto originalTriggerIndex = triggerUndo->index(), originalTriggerCount = triggerUndo->count();
        const auto projectedBaseline = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
        QVERIFY(projectedBaseline.Succeeded());
        const auto& baseline = *projectedBaseline.value;
        const auto triggerBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(triggerBytes.Succeeded());
        std::array<const LniObjectDocument*, 7> objectPointers{};
        std::array<std::string, 7> objectFingerprints{};
        std::array<QUndoStack*, 7> objectUndos{};
        std::array<int, 7> objectUndoIndices{}, objectUndoCounts{};
        for (std::size_t i = 0; i < objectPointers.size(); ++i) {
            objectPointers[i] = project->objectDocuments_[i].get();
            if (objectPointers[i]) objectFingerprints[i] = objectPointers[i]->DocumentFingerprint();
            objectUndos[i] = project->objectUndoStacks_[i];
            if (objectUndos[i]) { objectUndoIndices[i] = objectUndos[i]->index(); objectUndoCounts[i] = objectUndos[i]->count(); }
        }
        const auto sameMembers = [](const War3Map::MapDocument& first, const War3Map::MapDocument& second) {
            for (std::size_t i = 0; i < first.Members().size(); ++i) {
                const auto& a = first.Members()[i]; const auto& b = second.Members()[i];
                if (a.state != b.state || a.memberName != b.memberName || a.sources.size() != b.sources.size()) return false;
                for (std::size_t j = 0; j < a.sources.size(); ++j)
                    if (a.sources[j].name != b.sources[j].name || a.sources[j].data != b.sources[j].data) return false;
            }
            return true;
        };
        const auto currentMapMatches = [&](const War3Map::MapDocument& expected) {
            const auto current = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
            return current.Succeeded() && sameMembers(*current.value, expected);
        };
        const auto restoreMap = qScopeGuard([&] {
            while (mapUndo->index() > originalMapIndex) mapUndo->undo();
            QCOMPARE(project->MapEdits(), mapEdits);
            QCOMPARE(project->mapDataUndoStack_, mapUndo);
            QVERIFY(currentMapMatches(baseline));
        });
        const auto explain = [](const QJsonObject& value) { return QString::fromUtf8(QJsonDocument(value).toJson()); };
        const auto field = [](const QJsonObject& row, const QString& id) {
            for (const auto& value : row.value("fields").toArray())
                if (value.toObject().value("id").toString() == id) return value.toObject();
            return QJsonObject{};
        };
        const auto fieldId = placements ? QStringLiteral("x") : QStringLiteral("left");
        const auto editTool = placements ? QStringLiteral("entities_copy") : QStringLiteral("entities_set_fields");
        std::size_t selectedIndex = 0;
        if (placements) {
            QVERIFY(baseline.Units());
            const auto& units = baseline.Units()->units;
            const auto selected = std::find_if(units.begin(), units.end(), [](const auto& row) { return row.rawcode != "sloc"; });
            QVERIFY(selected != units.end());
            selectedIndex = static_cast<std::size_t>(selected - units.begin());
        }
        QString queryIndex = QString::number(selectedIndex);
        const auto query = [&] { return call(QStringLiteral("entities_query"),
            {{"kind", placements ? "units" : "regions"}, {"page", QJsonObject{{"start_index", queryIndex}, {"limit", 1}}}}); };
        const auto queried = query();
        QVERIFY2(queried.value("ok").toBool(), qPrintable(explain(queried)));
        QVERIFY(!queried.value("rows").toArray().isEmpty());
        const auto originalRow = queried.value("rows").toArray().first().toObject();
        const auto left = field(originalRow, fieldId), right = field(originalRow, QStringLiteral("right"));
        QVERIFY(left.value("value").isDouble());
        if (!placements) QVERIFY(left.value("writable").toBool() && right.value("value").isDouble());
        const auto originalLeft = left.value("value").toDouble();
        const auto originalRight = placements ? originalLeft + 2.0 : right.value("value").toDouble();
        QVERIFY(std::isfinite(originalLeft) && std::isfinite(originalRight) && originalRight > originalLeft);
        const double changedLeft = static_cast<double>(static_cast<float>(originalLeft + std::min(1.0, (originalRight - originalLeft) / 2.0)));
        QVERIFY(changedLeft != originalLeft && changedLeft < originalRight);
        const auto sourceIndex = originalRow.value("source_index").toString(), rowKey = originalRow.value("row_key").toString();
        QCOMPARE(sourceIndex, QString::number(selectedIndex));
        QVERIFY(!rowKey.isEmpty());
        QCOMPARE(originalRow.value("locator_scope").toString(), QStringLiteral("snapshot-revision"));
        const auto originalRaw = left.value("raw").toString();
        QVERIFY(originalRaw.startsWith(QStringLiteral("f32:")));
        QJsonObject edit{{"project_identity", queried.value("project_identity")}, {"expected_revision", queried.value("revision")},
            {"kind", placements ? "units" : "regions"}, {"expected_member_sha256", queried.value("member").toObject().value("member_sha256")}};
        if (placements) {
            edit.insert("sources", QJsonArray{QJsonObject{{"source_index", sourceIndex}, {"row_key", rowKey}}});
            edit.insert("offset", QJsonObject{{"x", 1.0}, {"y", 0.0}, {"z", 0.0}});
        } else edit.insert("changes", QJsonArray{QJsonObject{{"source_index", sourceIndex}, {"row_key", rowKey}, {"field", fieldId},
            {"expected_raw", originalRaw}, {"value", QString::number(changedLeft, 'g', 9)}}});
        const auto edited = call(editTool, edit);
        QVERIFY2(edited.value("ok").toBool(), qPrintable(explain(edited)));
        QVERIFY(!edited.value("saved").toBool() && !edited.value("runtime_verified").toBool());
        QCOMPARE(edited.value("session_identity").toString().toULongLong(), mapEdits->SessionIdentity());
        QCOMPARE(edited.value("command_identity").toString().toULongLong(), mapEdits->LastAppliedEdit().CommandIdentity());
        const auto beforeFields = edited.value("before").toObject().value("rows").toArray();
        const auto afterFields = edited.value("after").toObject().value("rows").toArray();
        QCOMPARE(beforeFields.size(), 1); QCOMPARE(afterFields.size(), 1);
        QCOMPARE(field(beforeFields.first().toObject(), fieldId).value("raw").toString(), originalRaw);
        QCOMPARE(field(beforeFields.first().toObject(), fieldId).value("value").toDouble(), originalLeft);
        const auto changedField = field(afterFields.first().toObject(), fieldId);
        QCOMPARE(changedField.value("value").toDouble(), changedLeft);
        const auto changedRaw = changedField.value("raw").toString();
        QVERIFY(!changedRaw.isEmpty() && changedRaw != originalRaw);
        QCOMPARE(project->MapEdits(), mapEdits); QCOMPARE(project->mapDataUndoStack_, mapUndo);
        QCOMPARE(project->mapDocument_.get(), mapDocument);
        QCOMPARE(mapUndo->count(), originalMapCount + 1);
        const auto projectedAfter = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
        QVERIFY(projectedAfter.Succeeded());
        const auto& afterMap = *projectedAfter.value;
        QVERIFY(!sameMembers(afterMap, baseline));
        if (placements) {
            QVERIFY(afterMap.Units());
            QCOMPARE(afterMap.Units()->units.size(), baseline.Units()->units.size() + 1);
            QCOMPARE(afterMap.Units()->units.at(selectedIndex).creationId, baseline.Units()->units.at(selectedIndex).creationId);
            QVERIFY(afterMap.Units()->units.back().creationId != baseline.Units()->units.at(selectedIndex).creationId);
            // 比较整个 typed 记录重编码：嵌套物品、能力及未知保留字节都必须随源复制。
            auto expectedUnits = *baseline.Units();
            auto expectedCopy = expectedUnits.units.at(selectedIndex);
            expectedCopy.position[0] = static_cast<float>(changedLeft);
            expectedCopy.creationId = afterMap.Units()->units.back().creationId;
            for (const auto& existing : expectedUnits.units) QVERIFY(existing.creationId != expectedCopy.creationId);
            expectedUnits.units.push_back(expectedCopy);
            const auto expectedWire = expectedUnits.Serialize(), actualWire = afterMap.Units()->Serialize();
            QVERIFY(expectedWire.Succeeded() && actualWire.Succeeded());
            QVERIFY(*expectedWire.value == *actualWire.value);
            queryIndex = QString::number(baseline.Units()->units.size());
            QCOMPARE(afterFields.first().toObject().value("source_index").toString(), queryIndex);
            const auto placementChanges = edited.value("changes").toObject().value("items").toArray();
            QCOMPARE(placementChanges.size(), 1);
            const auto placementChange = placementChanges.first().toObject();
            QVERIFY(placementChange.contains("object_id"));
            QVERIFY(placementChange.value("object_id").isString());
            QVERIFY(placementChange.value("object_id").toString().isEmpty());
        } else {
            QVERIFY(afterMap.Regions());
            QCOMPARE(static_cast<double>(afterMap.Regions()->entries.front().left), changedLeft);
        }
        auto active = query();
        QVERIFY2(active.value("ok").toBool(), qPrintable(explain(active)));
        QCOMPARE(field(active.value("rows").toArray().first().toObject(), fieldId).value("raw").toString(), changedRaw);
        if (placements) QCOMPARE(active.value("rows").toArray().first().toObject(), afterFields.first().toObject());
        QVERIFY(active.value("revision") != queried.value("revision"));
        QVERIFY(active.value("member").toObject().value("member_sha256") != queried.value("member").toObject().value("member_sha256"));
        const auto editedRevision = project->editRevision_.Revision(), editedMapRevision = mapEdits->Revision();
        const auto stale = call(editTool, edit);
        QVERIFY(!stale.value("ok").toBool());
        QCOMPARE(stale.value("diagnostics").toArray().at(0).toObject().value("code").toString(), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
        QVERIFY(currentMapMatches(afterMap));
        QCOMPARE(mapUndo->count(), originalMapCount + 1);
        QCOMPARE(project->editRevision_.Revision(), editedRevision); QCOMPARE(mapEdits->Revision(), editedMapRevision);
        auto invalid = edit;
        invalid.insert("expected_revision", active.value("revision"));
        invalid.insert("expected_member_sha256", active.value("member").toObject().value("member_sha256"));
        QJsonObject schemaRejection;
        if (placements) {
            auto schemaInvalid = invalid;
            schemaInvalid.insert("offset", QJsonObject{{"x", "not-a-number"}, {"y", 0}, {"z", 0}});
            const auto priorActivityCount = activity.size(), priorExchangeCount = exchanges.size();
            const auto priorUndoIndex = mapUndo->index(), priorUndoCount = mapUndo->count();
            schemaRejection = rpc(QStringLiteral("tools/call"), {{"name", editTool}, {"arguments", schemaInvalid}},
                QStringLiteral("arguments.offset.x must be a finite number"));
            QVERIFY2(!schemaRejection.isEmpty(), qPrintable(error));
            QCOMPARE(schemaRejection.value("id").toInt(), requestId);
            QCOMPARE(schemaRejection.value("error").toObject().value("code").toInt(), -32602);
            QCOMPARE(schemaRejection.value("error").toObject().value("message").toString(),
                QStringLiteral("arguments.offset.x must be a finite number"));
            QCOMPARE(exchanges.size(), priorExchangeCount + 1);
            QCOMPARE(exchanges.last().toObject().value("http_status").toInt(), 200);
            QVERIFY(exchanges.last().toObject().value("broker_call_ids").toArray().isEmpty());
            QCOMPARE(activity.size(), priorActivityCount);
            QCOMPARE(mapUndo->index(), priorUndoIndex); QCOMPARE(mapUndo->count(), priorUndoCount);
            QCOMPARE(project->editRevision_.Revision(), editedRevision); QCOMPARE(mapEdits->Revision(), editedMapRevision);
            QVERIFY(currentMapMatches(afterMap));
            invalid.insert("offset", QJsonObject{{"x", 1000000.001}, {"y", 0}, {"z", 0}});
        }
        else invalid.insert("changes", QJsonArray{QJsonObject{{"source_index", sourceIndex},
            {"row_key", active.value("rows").toArray().first().toObject().value("row_key")}, {"field", fieldId},
            {"expected_raw", changedRaw}, {"value", "not-a-number"}}});
        const auto rejected = call(editTool, invalid);
        QVERIFY(!rejected.value("ok").toBool());
        if (!placements) QCOMPARE(rejected.value("diagnostics").toArray().at(0).toObject().value("code").toString(), QStringLiteral("MAP_FIELD_INPUT_INVALID"));
        else {
            QVERIFY(!rejected.value("diagnostics").toArray().isEmpty());
            QCOMPARE(rejected.value("diagnostics").toArray().first().toObject().value("code").toString(),
                QStringLiteral("PLACEMENT_COORDINATE_INVALID"));
        }
        QVERIFY(currentMapMatches(afterMap));
        QCOMPARE(mapUndo->count(), originalMapCount + 1);
        QCOMPARE(project->editRevision_.Revision(), editedRevision); QCOMPARE(mapEdits->Revision(), editedMapRevision);
        const auto verifyQueryValue = [&](double number, const QString& raw) {
            const auto current = query();
            return current.value("ok").toBool()
                && field(current.value("rows").toArray().at(0).toObject(), fieldId).value("raw").toString() == raw
                && field(current.value("rows").toArray().at(0).toObject(), fieldId).value("value").toDouble() == number;
        };
        mapUndo->undo(); QVERIFY(currentMapMatches(baseline)); queryIndex = sourceIndex;
        QVERIFY(verifyQueryValue(originalLeft, originalRaw));
        mapUndo->redo(); QVERIFY(currentMapMatches(afterMap));
        if (placements) queryIndex = QString::number(baseline.Units()->units.size());
        QVERIFY(verifyQueryValue(changedLeft, changedRaw));
        mapUndo->undo(); QVERIFY(currentMapMatches(baseline)); queryIndex = sourceIndex;
        QVERIFY(verifyQueryValue(originalLeft, originalRaw));
        QCOMPARE(mapUndo->index(), originalMapIndex);
        QCOMPARE(project->MapEdits(), mapEdits); QCOMPARE(project->mapDataUndoStack_, mapUndo);
        QCOMPARE(project->mapDocument_.get(), mapDocument);
        QVERIFY(sameMembers(*mapDocument, baseline));
        QCOMPARE(project->triggerDocument_.get(), triggerDocument); QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(triggerUndo->index(), originalTriggerIndex); QCOMPARE(triggerUndo->count(), originalTriggerCount);
        const auto finalTriggers = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(finalTriggers.Succeeded());
        QVERIFY(finalTriggers.value->wtg == triggerBytes.value->wtg && finalTriggers.value->wct == triggerBytes.value->wct);
        for (std::size_t i = 0; i < objectPointers.size(); ++i) {
            QCOMPARE(project->objectDocuments_[i].get(), objectPointers[i]);
            if (objectPointers[i]) QCOMPARE(objectPointers[i]->DocumentFingerprint(), objectFingerprints[i]);
            QCOMPARE(project->objectUndoStacks_[i], objectUndos[i]);
            if (objectUndos[i]) { QCOMPARE(objectUndos[i]->index(), objectUndoIndices[i]); QCOMPARE(objectUndos[i]->count(), objectUndoCounts[i]); }
        }
        QCOMPARE(ReadFile(source), original);

        QJsonArray events; QSet<QString> starts, ends; QStringList orderedCalls;
        QString editCallId;
        for (const auto& arguments : activity) {
            const auto event = arguments.front().toJsonObject(); events.append(event);
            const auto type = event.value("type").toString();
            if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
            const auto id = event.value("id").toString(); QVERIFY(!QUuid(id).isNull());
            auto& identities = type == QStringLiteral("tool_start") ? starts : ends;
            QVERIFY(!identities.contains(id)); identities.insert(id);
            if (type == QStringLiteral("tool_start")) {
                orderedCalls.append(id);
                if (event.value("tool_name").toString() == editTool && editCallId.isEmpty()) editCallId = id;
            }
        }
        QCOMPARE(starts, ends); QCOMPARE(starts.size(), 9); QCOMPARE(conversation->ToolCount(), starts.size());
        QVERIFY(!editCallId.isEmpty());
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page)); window.resize(1280, 1000);
        QApplication::processEvents();
        const auto toggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
        const auto trees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QCOMPARE(toggles.size(), starts.size()); QCOMPARE(trees.size(), starts.size());
        int queryTitles = 0, editTitles = 0;
        for (const auto* title : conversation->findChildren<QLabel*>(QStringLiteral("agentToolTitle"))) {
            QVERIFY2(title->text().size() <= 40, qPrintable(title->text()));
            if (title->text() == QStringLiteral("查询地图实体")) ++queryTitles;
            if (title->text() == (placements ? QStringLiteral("复制预放实体") : QStringLiteral("修改实体字段"))) ++editTitles;
        }
        QCOMPARE(queryTitles, 5); QCOMPARE(editTitles, 3);
        const auto changeRows = conversation->findChildren<QWidget*>(QStringLiteral("agentInlineChanges"));
        QVERIFY(!changeRows.isEmpty());
        for (const auto* row : changeRows) {
            const auto* jump = row->findChild<QPushButton*>(QStringLiteral("agentInlineNavigate"));
            QVERIFY(jump && !jump->isVisible()); // C1 没有实体跳转实现，不能显示伪造的导航入口。
        }
        const auto editIndex = orderedCalls.indexOf(editCallId); QVERIFY(editIndex >= 0);
        auto* tree = trees.at(editIndex); toggles.at(editIndex)->setChecked(true); QTRY_VERIFY(tree->isVisible());
        const auto child = [](QTreeWidgetItem* parent, const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; parent && i < parent->childCount(); ++i) if (parent->child(i)->text(0) == text) return parent->child(i);
            return nullptr;
        };
        auto* details = child(tree->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(details);
        const auto captureRoot = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
        const auto capturePath = captureRoot.isEmpty() ? QString{} : QDir(captureRoot).filePath(placements ? QStringLiteral("placements") : QStringLiteral("entities"));
        QJsonArray captures;
        for (const auto& phase : {QStringLiteral("before"), QStringLiteral("after")}) {
            const auto rows = edited.value(phase).toObject().value("rows").toArray();
            const auto fields = rows.at(0).toObject().value("fields").toArray();
            qsizetype fieldIndex = -1;
            for (qsizetype i = 0; i < fields.size(); ++i) if (fields.at(i).toObject().value("id").toString() == fieldId) fieldIndex = i;
            QVERIFY(fieldIndex >= 0);
            tree->collapseAll(); details->setExpanded(true);
            auto* phaseItem = child(details, phase); QVERIFY(phaseItem); phaseItem->setExpanded(true);
            auto* rowsItem = child(phaseItem, QStringLiteral("rows")); QVERIFY(rowsItem); rowsItem->setExpanded(true);
            auto* rowItem = child(rowsItem, QStringLiteral("[0]")); QVERIFY(rowItem); rowItem->setExpanded(true);
            auto* fieldsItem = child(rowItem, QStringLiteral("fields")); QVERIFY(fieldsItem); fieldsItem->setExpanded(true);
            auto* fieldItem = child(fieldsItem, QStringLiteral("[%1]").arg(fieldIndex)); QVERIFY(fieldItem);
            tree->setCurrentItem(fieldItem);
            auto* preview = tree->parentWidget()->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview"));
            QVERIFY(preview);
            const auto shown = QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object();
            QCOMPARE(shown, fields.at(fieldIndex).toObject());
            QCOMPARE(shown.value("id").toString(), fieldId);
            QCOMPARE(shown.value("raw").toString(), phase == QStringLiteral("before") ? originalRaw : changedRaw);
            QCOMPARE(shown.value("value").toDouble(), phase == QStringLiteral("before") ? originalLeft : changedLeft);
            const auto stability = StabilizeDetailTree(tree, fieldsItem, fieldItem, conversation);
            QVERIFY2(stability.value("stable").toBool(), qPrintable(explain(stability)));
            if (!capturePath.isEmpty()) {
                QVERIFY(QDir().mkpath(capturePath));
                const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + (placements ? QStringLiteral("placements-unit-copy-x-") : QStringLiteral("entities-region-left-")) + phase + QStringLiteral(".png");
                const auto path = QDir(capturePath).filePath(fileName); QVERIFY(!QFile::exists(path)); QVERIFY(window.grab().save(path));
                auto capture = stability; capture.insert("file", fileName); capture.insert("expanded_tool_id", editCallId);
                capture.insert("selected_result_field", phase + QStringLiteral(".rows[0].fields.") + fieldId); capture.insert("field", shown);
                capture.insert("host_width", window.width()); capture.insert("host_height", window.height()); capture.insert("dpr", window.devicePixelRatioF());
                captures.append(capture);
            }
        }
        QJsonObject rotationReceipt;
        if (placements) {
            // 三种实测宽度均使用同一真实 HTTP 回执；不创建替身卡片或关闭动效。
            bool placementCaptureSucceeded = false;
            const auto capturePlacement = [&](QTreeWidget* resultTree, const QJsonObject& result,
                const QString& selectedField, const QString& titleText, const QString& callId,
                const QString& suffix) {
                placementCaptureSucceeded = false;
                resultTree->collapseAll();
                auto* resultDetails = child(resultTree->invisibleRootItem(), QStringLiteral("详情")); QVERIFY(resultDetails);
                resultDetails->setExpanded(true);
                auto* afterItem = child(resultDetails, QStringLiteral("after")); QVERIFY(afterItem); afterItem->setExpanded(true);
                auto* rowsItem = child(afterItem, QStringLiteral("rows")); QVERIFY(rowsItem); rowsItem->setExpanded(true);
                auto* rowItem = child(rowsItem, QStringLiteral("[0]")); QVERIFY(rowItem); rowItem->setExpanded(true);
                auto* fieldsItem = child(rowItem, QStringLiteral("fields")); QVERIFY(fieldsItem); fieldsItem->setExpanded(true);
                const auto fields = result.value("after").toObject().value("rows").toArray().first().toObject().value("fields").toArray();
                qsizetype index = -1;
                for (qsizetype i = 0; i < fields.size(); ++i) if (fields.at(i).toObject().value("id").toString() == selectedField) index = i;
                QVERIFY(index >= 0);
                auto* fieldItem = child(fieldsItem, QStringLiteral("[%1]").arg(index)); QVERIFY(fieldItem);
                resultTree->setCurrentItem(fieldItem);
                auto* card = resultTree->parentWidget()->parentWidget();
                auto* title = card->findChild<QLabel*>(QStringLiteral("agentToolTitle")); QVERIFY(title);
                QCOMPARE(title->text(), titleText);
                auto* preview = resultTree->parentWidget()->findChild<QPlainTextEdit*>(QStringLiteral("agentInlineDetailPreview")); QVERIFY(preview);
                QCOMPARE(QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object(), fields.at(index).toObject());
                QApplication::processEvents();
                conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->value()
                    + title->mapTo(conversation->viewport(), QPoint{}).y() - 12);
                auto proof = StabilizeDetailTree(resultTree, fieldsItem, fieldItem, conversation);
                QVERIFY2(proof.value("stable").toBool(), qPrintable(explain(proof)));
                const auto rect = [&](QWidget* widget) { return QRect(widget->mapTo(conversation->viewport(), QPoint{}), widget->size()); };
                const auto titleRect = rect(title), previewRect = rect(preview), treeRect = rect(resultTree);
                QVERIFY(conversation->viewport()->rect().contains(titleRect));
                QVERIFY(conversation->viewport()->rect().contains(previewRect));
                QVERIFY(!titleRect.intersects(previewRect) && !titleRect.intersects(treeRect) && !treeRect.intersects(previewRect));
                const auto jsonRect = [](const QRect& r) { return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}; };
                proof.insert("title_rect", jsonRect(titleRect)); proof.insert("preview_rect", jsonRect(previewRect));
                proof.insert("tree_rect", jsonRect(treeRect)); proof.insert("title_and_after_preview_visible", true);
                proof.insert("selected_result_field", "after.rows[0].fields." + selectedField);
                proof.insert("field", fields.at(index)); proof.insert("expanded_tool_id", callId);
                proof.insert("mode", static_cast<int>(agent->Mode())); proof.insert("host_width", window.width());
                proof.insert("host_height", window.height()); proof.insert("agent_width", agent->width());
                proof.insert("agent_height", agent->height()); proof.insert("dpr", window.devicePixelRatioF());
                if (!capturePath.isEmpty()) {
                    const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + suffix + QStringLiteral(".png");
                    const auto path = QDir(capturePath).filePath(fileName); QVERIFY(!QFile::exists(path)); QVERIFY(window.grab().save(path));
                    proof.insert("file", fileName);
                }
                captures.append(proof);
                placementCaptureSucceeded = true;
            };
            window.resize(1920, 1080);
            window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Sidebar));
            QTRY_COMPARE(agent->Mode(), AgentWindow::PresentationMode::Sidebar);
            auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock")); QVERIFY(sidebar);
            window.resizeDocks({sidebar}, {420}, Qt::Horizontal);
            QTRY_COMPARE(agent->width(), 420);
            capturePlacement(tree, edited, QStringLiteral("x"), QStringLiteral("复制预放实体"), editCallId,
                QStringLiteral("placements-copy-sidebar-420"));
            QVERIFY(placementCaptureSucceeded);
            window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page));
            window.resize(1920, 1080);
            capturePlacement(tree, edited, QStringLiteral("x"), QStringLiteral("复制预放实体"), editCallId,
                QStringLiteral("placements-copy-page-1920"));
            QVERIFY(placementCaptureSucceeded);

            // 复制已最终 Undo 后，再用当前查询身份旋转原行；弧度 raw 与度数展示分别核对。
            queryIndex = sourceIndex;
            const auto rotationQuery = query(); QVERIFY(rotationQuery.value("ok").toBool());
            const auto rotationSource = rotationQuery.value("rows").toArray().first().toObject();
            const auto originalFacing = field(rotationSource, QStringLiteral("facing"));
            const QJsonObject rotateArgs{{"kind", "units"}, {"project_identity", rotationQuery.value("project_identity")},
                {"expected_revision", rotationQuery.value("revision")},
                {"expected_member_sha256", rotationQuery.value("member").toObject().value("member_sha256")},
                {"source", QJsonObject{{"source_index", sourceIndex}, {"row_key", rotationSource.value("row_key")}}},
                {"facing_radians", 1.5}};
            const auto rotationStartEvent = activity.size();
            rotationReceipt = call(QStringLiteral("entities_rotate"), rotateArgs);
            QVERIFY2(rotationReceipt.value("ok").toBool(), qPrintable(explain(rotationReceipt)));
            QCOMPARE(rotationReceipt.value("session_identity").toString().toULongLong(), mapEdits->SessionIdentity());
            QCOMPARE(rotationReceipt.value("command_identity").toString().toULongLong(), mapEdits->LastAppliedEdit().CommandIdentity());
            QVERIFY(!rotationReceipt.value("saved").toBool() && !rotationReceipt.value("runtime_verified").toBool());
            QCOMPARE(rotationReceipt.value("rotation_unit").toString(), QStringLiteral("radians"));
            QCOMPARE(rotationReceipt.value("field_display_angle_unit").toString(), QStringLiteral("degrees"));
            const auto rotationBefore = rotationReceipt.value("before").toObject().value("rows").toArray();
            const auto rotationAfter = rotationReceipt.value("after").toObject().value("rows").toArray();
            QCOMPARE(rotationBefore.size(), 1); QCOMPARE(rotationAfter.size(), 1);
            QCOMPARE(rotationBefore.first().toObject(), rotationSource);
            const auto rotatedFacing = field(rotationAfter.first().toObject(), QStringLiteral("facing"));
            QCOMPARE(rotatedFacing.value("raw").toString(), QStringLiteral("f32:3fc00000"));
            QVERIFY(std::abs(rotatedFacing.value("value").toDouble() - 1.5 * 180.0 / std::numbers::pi_v<double>) < 1e-9);
            const auto rotationChanges = rotationReceipt.value("changes").toObject().value("items").toArray();
            QCOMPARE(rotationChanges.size(), 1);
            QVERIFY(rotationChanges.first().toObject().contains("object_id"));
            QVERIFY(rotationChanges.first().toObject().value("object_id").isString());
            QVERIFY(rotationChanges.first().toObject().value("object_id").toString().isEmpty());
            const auto rotatedProjection = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits); QVERIFY(rotatedProjection.Succeeded());
            const auto& rotatedMap = *rotatedProjection.value;
            auto expectedRotation = *baseline.Units(); expectedRotation.units.at(selectedIndex).rotation = 1.5f;
            const auto expectedRotationWire = expectedRotation.Serialize(), actualRotationWire = rotatedMap.Units()->Serialize();
            QVERIFY(expectedRotationWire.Succeeded() && actualRotationWire.Succeeded());
            QVERIFY(*expectedRotationWire.value == *actualRotationWire.value);
            QCOMPARE(query().value("rows").toArray().first().toObject(), rotationAfter.first().toObject());
            const auto rotationRevision = mapEdits->Revision();
            const auto rotationProjectRevision = project->editRevision_.Revision();
            const auto rotationUndoIndex = mapUndo->index(), rotationUndoCount = mapUndo->count();
            const auto staleRotation = call(QStringLiteral("entities_rotate"), rotateArgs);
            QVERIFY(!staleRotation.value("ok").toBool());
            QCOMPARE(staleRotation.value("diagnostics").toArray().first().toObject().value("code").toString(), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
            QCOMPARE(mapEdits->Revision(), rotationRevision); QCOMPARE(project->editRevision_.Revision(), rotationProjectRevision);
            QCOMPARE(mapUndo->index(), rotationUndoIndex); QCOMPARE(mapUndo->count(), rotationUndoCount);
            QVERIFY(currentMapMatches(rotatedMap));
            mapUndo->undo(); QVERIFY(currentMapMatches(baseline));
            QCOMPARE(field(query().value("rows").toArray().first().toObject(), QStringLiteral("facing")), originalFacing);
            mapUndo->redo(); QVERIFY(currentMapMatches(rotatedMap));
            QCOMPARE(field(query().value("rows").toArray().first().toObject(), QStringLiteral("facing")), rotatedFacing);
            mapUndo->undo(); QVERIFY(currentMapMatches(baseline));
            QCOMPARE(field(query().value("rows").toArray().first().toObject(), QStringLiteral("facing")), originalFacing);
            QCOMPARE(mapUndo->index(), originalMapIndex);
            QCOMPARE(project->MapEdits(), mapEdits); QCOMPARE(project->mapDataUndoStack_, mapUndo);
            QCOMPARE(project->mapDocument_.get(), mapDocument); QVERIFY(sameMembers(*mapDocument, baseline));
            QCOMPARE(project->triggerDocument_.get(), triggerDocument); QCOMPARE(project->triggerUndoStack_, triggerUndo);
            QCOMPARE(triggerUndo->index(), originalTriggerIndex); QCOMPARE(triggerUndo->count(), originalTriggerCount);
            const auto preservedTriggers = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
            QVERIFY(preservedTriggers.Succeeded());
            QVERIFY(preservedTriggers.value->wtg == triggerBytes.value->wtg && preservedTriggers.value->wct == triggerBytes.value->wct);
            for (std::size_t i = 0; i < objectPointers.size(); ++i) {
                QCOMPARE(project->objectDocuments_[i].get(), objectPointers[i]);
                if (objectPointers[i]) QCOMPARE(objectPointers[i]->DocumentFingerprint(), objectFingerprints[i]);
                QCOMPARE(project->objectUndoStacks_[i], objectUndos[i]);
                if (objectUndos[i]) { QCOMPARE(objectUndos[i]->index(), objectUndoIndices[i]); QCOMPARE(objectUndos[i]->count(), objectUndoCounts[i]); }
            }
            QCOMPARE(ReadFile(source), original);
            QString rotationCallId;
            for (auto i = rotationStartEvent; i < activity.size(); ++i) {
                const auto event = activity.at(i).first().toJsonObject();
                if (event.value("type").toString() == QStringLiteral("tool_start") && rotationCallId.isEmpty()) rotationCallId = event.value("id").toString();
            }
            // 原复制的9个UUID断言已执行；附加旋转7次调用必须也逐一配对进入同一回执。
            events = {}; starts.clear(); ends.clear(); orderedCalls.clear();
            for (const auto& arguments : activity) {
                const auto event = arguments.first().toJsonObject(); events.append(event);
                const auto type = event.value("type").toString();
                if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
                const auto id = event.value("id").toString(); QVERIFY(!QUuid(id).isNull());
                auto& identities = type == QStringLiteral("tool_start") ? starts : ends;
                QVERIFY(!identities.contains(id)); identities.insert(id);
                if (type == QStringLiteral("tool_start")) orderedCalls.append(id);
            }
            QCOMPARE(starts, ends); QCOMPARE(starts.size(), 16); QCOMPARE(conversation->ToolCount(), 16);
            const auto rotationIndex = orderedCalls.indexOf(rotationCallId); QVERIFY(rotationIndex >= 0);
            const auto rotationTrees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
            const auto rotationToggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
            QCOMPARE(rotationTrees.size(), 16); QCOMPARE(rotationToggles.size(), 16);
            for (auto* toggle : rotationToggles) toggle->setChecked(false);
            rotationToggles.at(rotationIndex)->setChecked(true);
            window.resize(1280, 1000);
            capturePlacement(rotationTrees.at(rotationIndex), rotationReceipt, QStringLiteral("facing"),
                QStringLiteral("调整实体朝向"), rotationCallId, QStringLiteral("placements-rotate-page-1280"));
            QVERIFY(placementCaptureSucceeded);
            for (const auto* row : conversation->findChildren<QWidget*>(QStringLiteral("agentInlineChanges"))) {
                const auto* jump = row->findChild<QPushButton*>(QStringLiteral("agentInlineNavigate")); QVERIFY(jump && !jump->isVisible());
            }
        }
        if (!capturePath.isEmpty()) {
            const auto projectedFinal = War3Map::MapData::ProjectMapData(*mapDocument, *mapEdits);
            QVERIFY(projectedFinal.Succeeded());
            const auto& finalMap = *projectedFinal.value;
            QJsonArray memberProof;
            const auto memberHash = [](const std::vector<std::byte>& bytes) {
                return QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex());
            };
            for (std::size_t i = 0; i < baseline.Members().size(); ++i) {
                const auto& prior = baseline.Members()[i]; const auto& final = finalMap.Members()[i];
                QJsonArray beforeHashes, afterHashes;
                for (const auto& member : prior.sources) beforeHashes.append(memberHash(member.data));
                for (const auto& member : final.sources) afterHashes.append(memberHash(member.data));
                memberProof.append(QJsonObject{{"member", QString::fromStdString(prior.memberName)}, {"before_sha256", beforeHashes}, {"after_sha256", afterHashes}});
            }
            const QJsonObject receipt{{"map_kind", realMap ? "actualMap" : "synthetic"}, {"fixture_only", !realMap},
                {"real_http", true}, {"same_map_edits", true}, {"same_map_data_undo_stack", true}, {"source_index", sourceIndex},
                {"operation", editTool}, {"before", edited.value("before")}, {"after", edited.value("after")},
                {"rotation", rotationReceipt}, {"schema_rejection", schemaRejection},
                {"row_key", rowKey}, {"locator_scope", "snapshot-revision"}, {"field", fieldId}, {"before_raw", originalRaw},
                {"after_raw", changedRaw}, {"before_value", originalLeft}, {"after_value", changedLeft}, {"edit_tool_id", editCallId},
                {"source_sha256", sourceSha}, {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
                {"source_bytes_unchanged", ReadFile(source) == original}, {"source_path", QString::fromStdWString(source.wstring())},
                {"final_members_restored", true}, {"member_proof", memberProof}, {"original_trigger_and_object_documents_unchanged", true},
                {"undo_restored_original", true}, {"redo_restored_edit", true}, {"final_document_restored", true},
                {"stale_revision_rejected", true}, {"invalid_value_rejected", true}, {"map_save_attempted", false}, {"map_save_verified", false},
                {"native_desktop_capture", false}, {"native_titlebar_in_image", false}, {"platform", QGuiApplication::platformName()},
                {"tool_card_count", conversation->ToolCount()}, {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
            const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented); QVERIFY(!bytes.contains(token));
            QFile file(QDir(capturePath).filePath((realMap ? QStringLiteral("real-") : QString{})
                + (placements ? QStringLiteral("placements-http-receipt.json") : QStringLiteral("entities-http-receipt.json"))));
            QVERIFY(!file.exists()); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
        }
        return;
    }
    if (variables) {
        auto* triggerDocument = project->triggerDocument_.get();
        auto* triggerUndo = project->triggerUndoStack_;
        QVERIFY(triggerDocument && triggerUndo && project->uiCatalog_);
        QVERIFY(project->uiCatalog_->Find(War3Map::Trigger::UiDefinitionKind::Define, "integer", "TriggerTypes"));
        const auto originalTriggerIndex = triggerUndo->index();
        const auto originalTriggerCount = triggerUndo->count();
        const auto originalVariableCount = triggerDocument->variables.size();
        const auto baseline = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(baseline.Succeeded());
        const auto sameTriggerBytes = [&](const auto& expected) {
            const auto current = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
            return current.Succeeded() && current.value->wtg == expected.wtg && current.value->wct == expected.wct;
        };
        const auto restoreTrigger = qScopeGuard([&] {
            while (triggerUndo->index() > originalTriggerIndex) triggerUndo->undo();
            QCOMPARE(project->triggerDocument_.get(), triggerDocument);
            QCOMPARE(project->triggerUndoStack_, triggerUndo);
            QVERIFY(sameTriggerBytes(*baseline.value));
        });
        const auto explain = [](const QJsonObject& value) { return QString::fromUtf8(QJsonDocument(value).toJson()); };
        const auto valueText = [](const QJsonObject& row) { return row.value("value").toObject().value("text").toString(); };
        const auto context = [](const QJsonObject& value) {
            return QJsonObject{{"project_identity", value.value("project_identity")}, {"expected_revision", value.value("revision")}};
        };
        auto createArgs = context(state);
        const auto variableName = QStringLiteral("AudastraHttpVariable_") + QUuid::createUuid().toString(QUuid::Id128);
        createArgs.insert("name", variableName);
        createArgs.insert("type_name", "integer");
        createArgs.insert("initialized", true);
        createArgs.insert("initial_value", "42");
        const auto created = call(QStringLiteral("variables_create"), createArgs);
        QVERIFY2(created.value("ok").toBool(), qPrintable(explain(created)));
        QVERIFY(created.value("before").isNull());
        QCOMPARE(valueText(created.value("after").toObject()), QStringLiteral("42"));
        QCOMPARE(created.value("after").toObject().value("type_name").toObject().value("text").toString(), QStringLiteral("integer"));
        QCOMPARE(created.value("after").toObject().value("flags").toObject().value("initialized").toInt(), 1);
        QVERIFY(!created.value("saved").toBool());
        const auto variableId = created.value("id").toString();
        bool validId = false;
        const auto stableId = variableId.toULongLong(&validId);
        QVERIFY(validId && stableId != 0 && QString::number(stableId) == variableId);
        QCOMPARE(triggerDocument->variables.size(), originalVariableCount + 1);
        const auto originalRecord = std::find_if(triggerDocument->variables.begin(), triggerDocument->variables.end(),
            [stableId](const auto& row) { return row.id == stableId; });
        QVERIFY(originalRecord != triggerDocument->variables.end());
        QCOMPARE(originalRecord->name.text, variableName.toStdString());
        QCOMPARE(originalRecord->value.text, std::string("42"));
        QCOMPARE(triggerUndo->count(), originalTriggerCount + 1);
        const auto createdBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(createdBytes.Succeeded());
        auto updateArgs = context(created);
        updateArgs.insert("id", variableId);
        updateArgs.insert("changes", QJsonObject{{"initial_value", "73"}});
        const auto updated = call(QStringLiteral("variables_update"), updateArgs);
        QVERIFY2(updated.value("ok").toBool(), qPrintable(explain(updated)));
        QCOMPARE(valueText(updated.value("before").toObject()), QStringLiteral("42"));
        QCOMPARE(valueText(updated.value("after").toObject()), QStringLiteral("73"));
        QCOMPARE(updated.value("id").toString(), variableId);
        QVERIFY(updated.value("revision") != created.value("revision"));
        QVERIFY(!updated.value("saved").toBool());
        QCOMPARE(project->triggerDocument_.get(), triggerDocument);
        QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(triggerUndo->count(), originalTriggerCount + 2);
        const auto updatedBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
        QVERIFY(updatedBytes.Succeeded());
        QVERIFY(updatedBytes.value->wtg != createdBytes.value->wtg);
        const auto stale = call(QStringLiteral("variables_update"), updateArgs);
        QVERIFY(!stale.value("ok").toBool());
        QCOMPARE(stale.value("diagnostics").toArray().at(0).toObject().value("code").toString(), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
        QVERIFY(sameTriggerBytes(*updatedBytes.value));
        QCOMPARE(triggerUndo->count(), originalTriggerCount + 2);
        const QJsonObject getArgs{{"id", variableId}};
        const auto readVariable = [&] { return call(QStringLiteral("variables_get"), getArgs); };
        auto current = readVariable();
        QVERIFY2(current.value("ok").toBool(), qPrintable(explain(current)));
        QCOMPARE(valueText(current.value("variable").toObject()), QStringLiteral("73"));
        triggerUndo->undo();
        QVERIFY(sameTriggerBytes(*createdBytes.value));
        current = readVariable();
        QVERIFY(current.value("ok").toBool());
        QCOMPARE(valueText(current.value("variable").toObject()), QStringLiteral("42"));
        triggerUndo->redo();
        QVERIFY(sameTriggerBytes(*updatedBytes.value));
        current = readVariable();
        QVERIFY(current.value("ok").toBool());
        QCOMPARE(valueText(current.value("variable").toObject()), QStringLiteral("73"));
        triggerUndo->undo(); // 撤销修改，再撤销创建；两类输入最终都恢复原文档。
        QVERIFY(sameTriggerBytes(*createdBytes.value));
        triggerUndo->undo();
        QCOMPARE(triggerUndo->index(), originalTriggerIndex);
        QVERIFY(sameTriggerBytes(*baseline.value));
        QCOMPARE(triggerDocument->variables.size(), originalVariableCount);
        QCOMPARE(project->triggerDocument_.get(), triggerDocument);
        QCOMPARE(project->triggerUndoStack_, triggerUndo);
        QCOMPARE(document->DocumentFingerprint(), before);
        QCOMPARE(undo->index(), originalUndoIndex);
        QCOMPARE(ReadFile(source), original);

        QJsonArray events;
        QSet<QString> starts, ends;
        QStringList orderedCalls;
        QString createCallId, updateCallId;
        for (const auto& arguments : activity) {
            const auto event = arguments.front().toJsonObject();
            events.append(event);
            const auto type = event.value("type").toString();
            if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
            const auto id = event.value("id").toString();
            QVERIFY(!QUuid(id).isNull());
            auto& identities = type == QStringLiteral("tool_start") ? starts : ends;
            QVERIFY(!identities.contains(id));
            identities.insert(id);
            if (type == QStringLiteral("tool_start")) {
                orderedCalls.append(id);
                if (event.value("tool_name").toString() == QStringLiteral("variables_create")) createCallId = id;
                if (event.value("tool_name").toString() == QStringLiteral("variables_update") && updateCallId.isEmpty()) updateCallId = id;
            }
        }
        QCOMPARE(starts, ends);
        QCOMPARE(starts.size(), 7); // state、create、update、stale、三次 get，均来自实际 loopback RPC。
        QVERIFY(!createCallId.isEmpty() && !updateCallId.isEmpty() && createCallId != updateCallId);
        QCOMPARE(conversation->ToolCount(), starts.size());
        window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page));
        window.resize(1280, 1000);
        QApplication::processEvents();
        const auto toggles = conversation->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
        const auto trees = conversation->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QCOMPARE(toggles.size(), starts.size());
        QCOMPARE(trees.size(), starts.size());
        int createTitles = 0, updateTitles = 0;
        for (const auto* title : conversation->findChildren<QLabel*>(QStringLiteral("agentToolTitle"))) {
            QVERIFY2(title->text().size() <= 40, qPrintable(title->text()));
            if (title->text() == QStringLiteral("创建变量")) ++createTitles;
            if (title->text() == QStringLiteral("修改变量")) ++updateTitles;
        }
        QCOMPARE(createTitles, 1);
        QCOMPARE(updateTitles, 2);
        const auto changeRows = conversation->findChildren<QWidget*>(QStringLiteral("agentInlineChanges"));
        QCOMPARE(changeRows.size(), 2);
        for (const auto* row : changeRows) {
            const auto* title = row->findChild<QLabel*>(QStringLiteral("agentToolTitle"));
            const auto* details = row->findChild<QTreeWidget*>(QStringLiteral("agentChangeDetails"));
            QVERIFY(title && details);
            QVERIFY(title->toolTip().contains(variableName));
            bool fullNameAvailable = false;
            for (int i = 0; i < details->topLevelItemCount(); ++i) {
                const auto* item = details->topLevelItem(i);
                if (item->text(0) == QStringLiteral("对象"))
                    fullNameAvailable = item->text(1) == variableName;
            }
            QVERIFY2(fullNameAvailable, "Compact change titles must preserve the complete variable name in expanded object details");
        }
        const auto updateIndex = orderedCalls.indexOf(updateCallId);
        QVERIFY(updateIndex >= 0);
        auto* tree = trees.at(updateIndex);
        toggles.at(updateIndex)->setChecked(true);
        QTRY_VERIFY(tree->isVisible());
        const auto child = [](QTreeWidgetItem* parent, const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; parent && i < parent->childCount(); ++i)
                if (parent->child(i)->text(0) == text) return parent->child(i);
            return nullptr;
        };
        // 外部 HTTP 的原 Activity payload 使用 details；不补造模型 result 事件。
        auto* resultRow = child(tree->invisibleRootItem(), QStringLiteral("详情"));
        QVERIFY(resultRow);
        auto* beforeRow = child(resultRow, QStringLiteral("before"));
        auto* afterRow = child(resultRow, QStringLiteral("after"));
        QVERIFY(beforeRow && afterRow);
        bool descriptionPreserved = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == QStringLiteral("说明"))
                descriptionPreserved = tree->topLevelItem(i)->text(1).contains(QStringLiteral("原子修改"));
        QVERIFY2(descriptionPreserved, "Full variable operation description must remain in expanded details");
        const auto captureRoot = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
        const auto capturePath = captureRoot.isEmpty() ? QString{} : QDir(captureRoot).filePath(QStringLiteral("variables"));
        QJsonArray captures;
        for (const auto& field : {QStringLiteral("before"), QStringLiteral("after")}) {
            tree->collapseAll();
            resultRow->setExpanded(true);
            auto* row = field == QStringLiteral("before") ? beforeRow : afterRow;
            auto* value = child(row, QStringLiteral("value"));
            QVERIFY(value);
            row->setExpanded(true);
            value->setExpanded(true);
            auto* text = child(value, QStringLiteral("text"));
            QVERIFY(text);
            QCOMPARE(text->text(1), field == QStringLiteral("before") ? QStringLiteral("42") : QStringLiteral("73"));
            tree->setCurrentItem(value);
            const auto stability = StabilizeDetailTree(tree, row, text, conversation);
            QVERIFY2(stability.value("stable").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(stability).toJson())));
            if (!capturePath.isEmpty()) {
                QVERIFY(QDir().mkpath(capturePath));
                const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + QStringLiteral("variables-update-") + field + QStringLiteral(".png");
                const auto path = QDir(capturePath).filePath(fileName);
                QVERIFY(!QFile::exists(path));
                QVERIFY(window.grab().save(path));
                auto capture = stability;
                capture.insert("file", fileName); capture.insert("expanded_tool_id", updateCallId);
                capture.insert("selected_result_field", field); capture.insert("value", text->text(1));
                capture.insert("host_width", window.width()); capture.insert("host_height", window.height());
                capture.insert("dpr", window.devicePixelRatioF());
                captures.append(capture);
            }
        }
        if (!capturePath.isEmpty()) {
            const auto hash = [](const std::vector<std::byte>& bytes) {
                return QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex());
            };
            const auto finalBytes = War3Map::Trigger::WtgWctCodec{}.Serialize(*triggerDocument, *project->uiCatalog_);
            QVERIFY(finalBytes.Succeeded());
            const QJsonObject receipt{{"map_kind", realMap ? "actualMap" : "synthetic"}, {"fixture_only", !realMap},
                {"real_http", true}, {"same_gui_trigger_document", true}, {"same_trigger_undo_stack", true},
                {"source_path", QString::fromStdWString(source.wstring())}, {"source_sha256", sourceSha},
                {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
                {"source_bytes_unchanged", ReadFile(source) == original}, {"original_wtg_sha256", hash(baseline.value->wtg)},
                {"original_wct_sha256", hash(baseline.value->wct)}, {"final_wtg_sha256", hash(finalBytes.value->wtg)},
                {"final_wct_sha256", hash(finalBytes.value->wct)}, {"variable_id", variableId}, {"variable_name", variableName},
                {"create_tool_id", createCallId}, {"update_tool_id", updateCallId}, {"created_value", "42"}, {"updated_value", "73"},
                {"stale_revision_rejected", true}, {"undo_restored_created", true}, {"redo_restored_update", true},
                {"final_document_restored", true}, {"map_save_attempted", false}, {"map_save_verified", false},
                {"native_desktop_capture", false}, {"native_titlebar_in_image", false}, {"platform", QGuiApplication::platformName()},
                {"tool_card_count", conversation->ToolCount()}, {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
            const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
            QVERIFY(!bytes.contains(token));
            QFile file(QDir(capturePath).filePath(realMap ? QStringLiteral("real-variables-http-receipt.json") : QStringLiteral("variables-http-receipt.json")));
            QVERIFY(!file.exists());
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size());
        }
        return;
    }
    const auto objectId = realMap ? qEnvironmentVariable("FORGE_MCP_GUI_OBJECT_ID", QStringLiteral("hfoo")) : QStringLiteral("hfoo");
    const auto listed = call(QStringLiteral("objects_list"), {{"kind", "unit"}, {"query", objectId}});
    QVERIFY(listed.value("ok").toBool());
    QVERIFY(!listed.value("items").toArray().isEmpty());
    bool exactObject = false;
    for (const auto& row : listed.value("items").toArray()) exactObject |= row.toObject().value("id").toString() == objectId;
    QVERIFY(exactObject);
    const QJsonObject query{{"kind", "unit"}, {"id", objectId}, {"fields", QJsonArray{"uhpm"}}};
    const auto health = [](const QJsonObject& result) {
        const auto fields = result.value("fields").toArray();
        return fields.isEmpty() ? -1 : fields.first().toObject().value("value").toInt(-1);
    };
    const auto originalFields = call(QStringLiteral("objects_get"), query);
    QVERIFY(originalFields.value("ok").toBool());
    const int originalHealth = health(originalFields);
    if (!realMap) QCOMPARE(originalHealth, 420);
    else {
        QVERIFY(originalHealth >= 1 && originalHealth <= 1000000);
        QVERIFY(originalFields.value("fields").toArray().first().toObject().value("editable").toBool());
    }
    const int changedHealth = realMap ? (originalHealth < 1000000 ? originalHealth + 1 : originalHealth - 1) : 640;
    const QJsonObject edit{{"kind", "unit"}, {"id", objectId},
        {"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", changedHealth}}}}};
    const auto changed = call(QStringLiteral("objects_set_fields"), edit);
    QVERIFY2(changed.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(changed).toJson())));
    QVERIFY(changed.value("revision") != state.value("revision"));
    QVERIFY(!changed.value("saved").toBool());
    QCOMPARE(project->objectDocuments_[0].get(), document);
    QCOMPARE(project->objectUndoStacks_[0], undo);
    QCOMPARE(undo->count(), undoCount + 1);
    const auto after = document->DocumentFingerprint();
    QVERIFY(after != before);
    const auto stale = call(QStringLiteral("objects_set_fields"), edit);
    QVERIFY(!stale.value("ok").toBool());
    QVERIFY(QJsonDocument(stale).toJson().contains("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(document->DocumentFingerprint(), after);
    QCOMPARE(undo->count(), undoCount + 1);
    QCOMPARE(health(call(QStringLiteral("objects_get"), query)), changedHealth);
    undo->undo();
    QCOMPARE(document->DocumentFingerprint(), before);
    QCOMPARE(health(call(QStringLiteral("objects_get"), query)), originalHealth);
    undo->redo();
    QCOMPARE(document->DocumentFingerprint(), after);
    QCOMPARE(health(call(QStringLiteral("objects_get"), query)), changedHealth);
    if (realMap) {
        undo->undo();
        QCOMPARE(undo->index(), originalUndoIndex);
        QCOMPARE(document->DocumentFingerprint(), before);
        QCOMPARE(health(call(QStringLiteral("objects_get"), query)), originalHealth);
    }
    QCOMPARE(ReadFile(source), original);
    QJsonArray events;
    QSet<QString> starts, ends;
    QStringList orderedCalls;
    for (const auto& arguments : activity) {
        const auto event = arguments.front().toJsonObject();
        events.append(event);
        const auto type = event.value("type").toString();
        if (type != QStringLiteral("tool_start") && type != QStringLiteral("tool_end")) continue;
        const auto id = event.value("id").toString();
        QVERIFY(!QUuid(id).isNull());
        auto& identities = type == QStringLiteral("tool_start") ? starts : ends;
        QVERIFY(!identities.contains(id));
        identities.insert(id);
        if (type == QStringLiteral("tool_start")) orderedCalls.append(id);
    }
    QCOMPARE(starts, ends);
    QCOMPARE(starts.size(), realMap ? 9 : 8);
    QCOMPARE(conversation->ToolCount(), starts.size()); // 每次真实 HTTP 调用只出现一张卡。
    const QString capturePath = qEnvironmentVariable("FORGE_MCP_GUI_CAPTURE_DIR");
    QJsonArray captures;
    const auto capture = [&](const QString& name) {
        QApplication::processEvents();
        conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->maximum());
        QApplication::processEvents();
        const auto* attach = agent->findChild<QToolButton*>(QStringLiteral("agentAttach"));
        const auto* send = agent->findChild<QPushButton*>(QStringLiteral("agentSend"));
        if (!attach || !send || attach->size() != QSize(32, 32)
            || send->size() != QSize(32, 32)) return false;
        if (capturePath.isEmpty()) return true;
        if (!QDir().mkpath(capturePath)) return false;
        const auto fileName = (realMap ? QStringLiteral("real-") : QString{}) + name + QStringLiteral(".png");
        const auto path = QDir(capturePath).filePath(fileName);
        if (QFile::exists(path) || !window.grab().save(path)) return false;
        captures.append(QJsonObject{{"file", fileName}, {"mode", static_cast<int>(agent->Mode())},
            {"host_width", window.width()}, {"host_height", window.height()}, {"agent_width", agent->width()},
            {"agent_height", agent->height()}, {"dpr", window.devicePixelRatioF()},
            {"attach_width", attach->width()}, {"attach_height", attach->height()},
            {"send_width", send->width()}, {"send_height", send->height()}});
        return true;
    };
    QVERIFY(capture(QStringLiteral("http-page")));
    window.resize(1920, 1080);
    window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Sidebar));
    QTRY_COMPARE(agent->Mode(), AgentWindow::PresentationMode::Sidebar);
    QVERIFY(capture(QStringLiteral("http-sidebar")));
    window.PresentAgentWorkspace(static_cast<int>(AgentWindow::PresentationMode::Page));
    window.resize(1280, 900);
    const auto toggles = agent->findChildren<QToolButton*>(QStringLiteral("agentToolToggle"));
    const auto trees = agent->findChildren<QTreeWidget*>(QStringLiteral("agentToolDetails"));
    QCOMPARE(toggles.size(), starts.size());
    QCOMPARE(trees.size(), starts.size());
    int readTitles = 0;
    for (const auto* title : agent->findChildren<QLabel*>(QStringLiteral("agentToolTitle"))) {
        QVERIFY2(title->text().size() <= 40, "Tool cards must display short operation titles, not schema descriptions");
        if (title->text() == QStringLiteral("读取对象字段")) ++readTitles;
    }
    QCOMPARE(readTitles, realMap ? 5 : 4);
    toggles.last()->setChecked(true);
    trees.last()->expandAll();
    QVERIFY(toggles.last()->isChecked());
    QVERIFY(trees.last()->topLevelItemCount() > 0);
    bool descriptionPreserved = false;
    for (int i = 0; i < trees.last()->topLevelItemCount(); ++i) {
        const auto* row = trees.last()->topLevelItem(i);
        if (row->text(0) == QStringLiteral("说明")) {
            descriptionPreserved = row->text(1).contains(QStringLiteral("读取对象当前值"));
        }
    }
    QVERIFY2(descriptionPreserved, "Full tool description must remain available in expanded details");
    QTRY_VERIFY(trees.last()->isVisible());
    QVERIFY(capture(QStringLiteral("http-expanded-conversation")));
    if (!capturePath.isEmpty()) {
        const QJsonObject receipt{{"map_kind", realMap ? "actualMap" : "synthetic"}, {"real_http", true}, {"same_gui_document", true},
            {"fixture_only", !realMap}, {"native_desktop_capture", false}, {"native_titlebar_in_image", false},
            {"source_path", QString::fromStdWString(source.wstring())}, {"source_sha256", sourceSha},
            {"source_sha256_after", QString::fromLatin1(QCryptographicHash::hash(ReadFile(source), QCryptographicHash::Sha256).toHex())},
            {"object_id", objectId}, {"field", "uhpm"}, {"before_value", originalHealth}, {"edited_value", changedHealth},
            {"final_document_restored", realMap}, {"map_save_attempted", false}, {"map_save_verified", false},
            {"expanded_tool_id", orderedCalls.last()}, {"expanded_tree_root_count", trees.last()->topLevelItemCount()},
            {"platform", QGuiApplication::platformName()}, {"source_bytes_unchanged", ReadFile(source) == original},
            {"undo_restored_original", true}, {"redo_restored_edit", true}, {"tool_card_count", conversation->ToolCount()},
            {"http_exchanges", exchanges}, {"broker_events", events}, {"captures", captures}};
        const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
        QVERIFY(!bytes.contains(token));
        QFile file(QDir(capturePath).filePath(realMap ? QStringLiteral("real-http-receipt.json") : QStringLiteral("http-receipt.json")));
        QVERIFY(!file.exists());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
}

void NativeWorkbenchTests::NativeMenusAndPickersExposeRealPopupGeometry() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("原创浮层夹具.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.resize(1280, 900);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.ExecuteCommand("module.objects");
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    window.show();
    QCoreApplication::processEvents();
    const QString output = qEnvironmentVariable("FORGE_POPUP_CAPTURE_DIR");
    auto* fileMenu = window.findChild<QMenu*>(QStringLiteral("applicationFileMenu"));
    QVERIFY(fileMenu);
    QToolButton* fileButton = nullptr;
    for (auto* button : window.Header()->findChildren<QToolButton*>(QStringLiteral("headerAction")))
        if (button->defaultAction() && button->defaultAction()->menu() == fileMenu) fileButton = button;
    QVERIFY(fileButton);
    fileMenu->popup(fileButton->mapToGlobal(QPoint(0, fileButton->height())));
    QTRY_VERIFY(fileMenu->isVisible());
    QString failure = PopupCapture::Save(window, *fileMenu, fileButton, output, QStringLiteral("topbar-file-menu"));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QTest::keyClick(fileMenu, Qt::Key_Escape);
    QTRY_VERIFY(!fileMenu->isVisible());

    // 使用正式对象选择器及当前工程目录数据；独立夹具不写任何地图字段。
    QVERIFY(!window.objectsModule_->objectViewEntries_.isEmpty());
    War3Map::QtApp::ObjectPickerPopup picker(window.project_->iconProvider_.get(), &window);
    picker.SetEntries(window.objectsModule_->objectViewEntries_);
    picker.SetTitle(QStringLiteral("选择单位 · 原创目录夹具"));
    picker.SetKindFilter(QStringLiteral("unit"));
    auto* anchor = window.objectsModule_->objectFieldSearch_;
    QVERIFY(anchor && anchor->isVisible());
    picker.OpenAt(anchor);
    QTRY_VERIFY(picker.isVisible());
    failure = PopupCapture::Save(window, picker, anchor, output, QStringLiteral("object-picker"));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    const auto insideScreen = [](QWidget* popup) {
        const QRect bounds = popup->screen()->availableGeometry().adjusted(8, 8, -8, -8);
        const QRect actual(popup->mapToGlobal(QPoint{}), popup->size());
        const bool contained = bounds.contains(actual);
        if (!contained) {
            qWarning() << "Popup exceeds available screen" << popup->objectName() << "bounds" << bounds
                       << "actual" << actual << "minimum" << popup->minimumSize()
                       << "minimumHint" << popup->minimumSizeHint();
            for (const auto* child : popup->findChildren<QWidget*>()) {
                if (!child->isVisible()) continue;
                qWarning() << "Popup child" << child->metaObject()->className() << child->objectName()
                           << "size" << child->size() << "minimum" << child->minimumSize()
                           << "minimumHint" << child->minimumSizeHint();
            }
        }
        return contained;
    };
    // 两种浮层都保存真实几何后再断言，首项失败不遮蔽第二项的诊断。
    const bool pickerInsideScreen = insideScreen(&picker);
    auto* pickerClose = picker.findChild<QPushButton*>(QStringLiteral("objectPickerClose"));
    QVERIFY(pickerClose);
    QTest::mouseClick(pickerClose, Qt::LeftButton);
    QTRY_VERIFY(!picker.isVisible());

    window.ShowCommandPalette(QStringLiteral(">"));
    QTRY_VERIFY(window.commandPalette_->isVisible());
    failure = PopupCapture::Save(window, *window.commandPalette_, window.globalSearch_, output, QStringLiteral("command-palette"));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    const bool paletteInsideScreen = insideScreen(window.commandPalette_);
    auto* results = window.commandPalette_->findChild<QListView*>(QStringLiteral("commandPaletteResults"));
    QVERIFY(results && results->model() && results->model()->rowCount() > 0);
    auto* footer = window.commandPalette_->findChild<QLabel*>(QStringLiteral("commandPaletteFooter"));
    QVERIFY(footer && footer->isVisible());
    QVERIFY(window.commandPalette_->contentsRect().contains(QRect(footer->mapTo(window.commandPalette_, QPoint{}), footer->size())));
    QVERIFY(!QRect(results->mapTo(window.commandPalette_, QPoint{}), results->size())
        .intersects(QRect(footer->mapTo(window.commandPalette_, QPoint{}), footer->size())));
    QVERIFY(results->viewport()->height() >= 34); // 至少一行可读结果，其余由原列表滚动。
    auto* search = window.commandPalette_->findChild<QLineEdit*>(QStringLiteral("commandPaletteSearch"));
    QVERIFY(search);
    QTest::keyClick(search, Qt::Key_Escape);
    QTRY_VERIFY(!window.commandPalette_->isVisible());
    QCOMPARE(ReadFile(source), original);
    QVERIFY2(pickerInsideScreen, "Object picker must fit within the available screen with an 8px margin");
    QVERIFY2(paletteInsideScreen, "Command palette must fit within the available screen with an 8px margin");
}

void NativeWorkbenchTests::ConsecutiveSaveKeepsPriorEditsAndImports() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("source.w3x")));
    const auto first = NativePath(fixture.filePath(QStringLiteral("saved-first.w3x")));
    const auto second = NativePath(fixture.filePath(QStringLiteral("saved-second.w3x")));
    const auto third = NativePath(fixture.filePath(QStringLiteral("saved-third.w3x")));
    // 1024 字节原创前缀覆盖旧代码只读 512 字节的截断缺陷。
    std::vector<std::byte> prefix(1024, std::byte{0});
    prefix[0] = std::byte{'H'}; prefix[1] = std::byte{'M'};
    prefix[2] = std::byte{'3'}; prefix[3] = std::byte{'W'};
    prefix[900] = std::byte{0x51};
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", MinimalUnitData(420), false},
        {"Original/Payload.bin", Bytes("untouched synthetic member"), false}
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(original, files, prefix).Succeeded());
    const QByteArray sourceBytes = ReadFile(original);
    const QString importPath = fixture.filePath(QStringLiteral("payload.txt"));
    QVERIFY(WriteText(importPath, "new imported payload"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(original));
    QVERIFY(ChangeHealth(window, 640));
    window.project_->pendingImports_.push_back({QStringLiteral("Custom/Payload.txt"), importPath, 20});
    window.project_->NotifyImportsChanged();
    QVERIFY(window.SaveMapArchiveToPath(first));
    QVERIFY(window.project_->sourceArchivePath_ == first);
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(window.project_->pendingImports_.empty());
    QCOMPARE(window.assetsModule_->PendingCount(), 0);
    const auto firstBaseline = NativeObjectCodec(ObjectType::Unit).Serialize(*window.project_->nativeObjectDocuments_[0]);
    QVERIFY(firstBaseline.Succeeded());
    QVERIFY(*firstBaseline.value == MinimalUnitData(640));
    // 无新编辑再另存一次，仍必须复制上次候选，而非退回最初源归档。
    QVERIFY(window.SaveMapArchiveToPath(second));
    QVERIFY(window.project_->sourceArchivePath_ == second);
    for (const auto& path : {first, second}) {
        auto opened = War3Map::Mpq::MpqArchive::OpenReadOnly(path);
        QVERIFY(opened.Succeeded());
        const auto unit = opened.value->ReadFile("war3map.w3u");
        QVERIFY(unit.Succeeded());
        QVERIFY(*unit.value == MinimalUnitData(640));
        const auto originalMember = opened.value->ReadFile("original/payload.bin");
        QVERIFY(originalMember.Succeeded());
        QVERIFY(*originalMember.value == Bytes("untouched synthetic member"));
        const auto imported = opened.value->ReadFile("custom/payload.txt");
        QVERIFY(imported.Succeeded());
        QVERIFY(*imported.value == Bytes("new imported payload"));
        const auto outputPrefix = opened.value->ReadArchivePrefix();
        QVERIFY(outputPrefix.Succeeded());
        QVERIFY(*outputPrefix.value == prefix);
    }
    QVERIFY(ChangeHealth(window, 860));
    QVERIFY(window.SaveMapArchiveToPath(third));
    auto finalArchive = War3Map::Mpq::MpqArchive::OpenReadOnly(third);
    QVERIFY(finalArchive.Succeeded());
    auto finalUnit = finalArchive.value->ReadFile("war3map.w3u");
    QVERIFY(finalUnit.Succeeded());
    QVERIFY(*finalUnit.value == MinimalUnitData(860));
    QVERIFY(finalArchive.value->HasFile("Custom/Payload.txt"));
    QCOMPARE(ReadFile(original), sourceBytes);
}

void NativeWorkbenchTests::TestMapSnapshotPreservesSourceDirtyRevisionAndUndo() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("test-copy.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto originalBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.testMapAction_->isEnabled());
    QCOMPARE(window.testMapAction_->shortcut(), QKeySequence(Qt::Key_F9));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    auto* model = window.objectsModule_->objectFieldModel_;
    const auto healthRow = FieldRow(*model, "uhpm"); QVERIFY(healthRow >= 0);
    QVERIFY(model->setData(model->index(healthRow, 3), 555, Qt::EditRole));
    auto* stack = window.project_->objectUndoStacks_[0];
    const auto revision = window.project_->editRevision_.Revision();
    const auto undoCount = stack->count(), undoIndex = stack->index(), cleanIndex = stack->cleanIndex();
    const auto* session = window.project_->archiveSession_.get();
    QVERIFY(window.RunMapSave(candidate, false, false, true));
    QCOMPARE(ReadFile(source), originalBytes);
    QVERIFY(window.project_->sourceArchivePath_ == source);
    QCOMPARE(window.project_->archiveSession_.get(), session);
    QVERIFY(window.project_->archiveSession_->VerifySourceUnchanged().Succeeded());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QVERIFY(window.project_->objectDirty_[0]);
    QCOMPARE(window.project_->editRevision_.Revision(), revision);
    QCOMPARE(stack->count(), undoCount);
    QCOMPARE(stack->index(), undoIndex);
    QCOMPARE(stack->cleanIndex(), cleanIndex);
    auto reopened = War3Map::MapArchiveSession::Open(candidate);
    QVERIFY(reopened.Succeeded());
    const auto member = reopened.value->ReadMember("war3map.w3u");
    QVERIFY(member.Succeeded());
    QVERIFY(*member.value == MinimalUnitData(555));
    stack->undo();
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 420);
    stack->redo();
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 555);
    // 错误目标和误用原地标志也不能改变原图、撤销或未保存状态。
    QVERIFY(!window.RunMapSave(source, true, false, true));
    QVERIFY(!window.RunMapSave(candidate, false, false, true));
    QCOMPARE(ReadFile(source), originalBytes);
    QVERIFY(window.project_->sourceArchivePath_ == source);
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QCOMPARE(stack->count(), undoCount);
}

void NativeWorkbenchTests::TestMapSnapshotRejectsStaleInitializationScript() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("stale-copy.w3x")));
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3u", MinimalUnitData(420)},
        {"war3map.j", Bytes("// original frozen initialization script\n")}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->mapEdits_ && window.project_->mapEdits_->Snapshot().Settings());
    auto settings = *window.project_->mapEdits_->Snapshot().Settings();
    settings.mapName = "changed initialization name";
    QVERIFY(window.project_->mapEdits_->EditSettings(settings).Succeeded());
    QVERIFY(!window.RunMapSave(candidate, false, false, true));
    QCOMPARE(ReadFile(source), original);
    QVERIFY(window.project_->sourceArchivePath_ == source);
    QVERIFY(window.project_->mapEdits_->IsDirty());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QVERIFY(!window.mapTestLease_);
}

void NativeWorkbenchTests::SaveMapInPlaceWritesBackAndKeepsBackups() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("in-place.w3x")));
    QVERIFY(MakeArchive(original, 420).Succeeded());
    const QByteArray sourceBytes = ReadFile(original);
    const auto backupDirectory = NativePath(fixture.filePath(QStringLiteral("War3MapEditor Backups")));
    const ScopedMapBackupPreferences backupPreferences(QString::fromStdWString(backupDirectory.wstring()));
    const auto backups = [&backupDirectory] {
        std::vector<std::filesystem::path> files;
        if (!std::filesystem::exists(backupDirectory)) return files;
        for (const auto& entry : std::filesystem::directory_iterator(backupDirectory))
            if (entry.is_regular_file() && entry.path().extension() == ".w3x") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        return files;
    };
    const auto savedHealth = [&original]() -> std::optional<std::int32_t> {
        auto opened = War3Map::Mpq::MpqArchive::OpenReadOnly(original);
        if (!opened.Succeeded()) return std::nullopt;
        const auto unit = opened.value->ReadFile("war3map.w3u");
        if (!unit.Succeeded()) return std::nullopt;
        for (const std::int32_t candidate : {555, 666}) {
            if (*unit.value == MinimalUnitData(candidate)) return candidate;
        }
        return std::nullopt;
    };

    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(original));
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    QVERIFY(ChangeHealth(window, 555));
    QVERIFY(window.project_->HasUnsavedMapChanges());
    // 同步执行：写回源地图，原图复制到用户配置的备份文件夹。
    const auto inPlaceSaved = window.RunMapSave({}, true, false);
    ReportMapSaveFixture(QStringLiteral("after-first-in-place-save"), fixture.path());
    QVERIFY2(inPlaceSaved, qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(window.project_->sourceArchivePath_ == original);
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    QVERIFY(!window.project_->savingMap_);
    QCOMPARE(savedHealth().value_or(-1), 555);
    auto kept = backups();
    QCOMPARE(kept.size(), std::size_t{1});
    QCOMPARE(ReadFile(kept.front()), sourceBytes);
    QVERIFY(window.project_->archiveSession_->VerifySourceUnchanged().Succeeded());

    // 后台线程执行（Ctrl+S 的实际路径）：进度框与工作线程结束后同样提交新基线。
    const QByteArray firstSaved = ReadFile(original);
    QVERIFY(ChangeHealth(window, 666));
    const auto backgroundSaved = window.RunMapSave({}, true, true);
    ReportMapSaveFixture(QStringLiteral("after-background-in-place-save"), fixture.path());
    QVERIFY2(backgroundSaved, qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(!window.project_->savingMap_);
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    QCOMPARE(savedHealth().value_or(-1), 666);
    kept = backups();
    QCOMPARE(kept.size(), std::size_t{2});
    QVERIFY(std::any_of(kept.begin(), kept.end(), [&](const auto& path) { return ReadFile(path) == sourceBytes; }));
    QVERIFY(std::any_of(kept.begin(), kept.end(), [&](const auto& path) { return ReadFile(path) == firstSaved; }));
    for (const auto& entry : std::filesystem::directory_iterator(NativePath(fixture.path()))) {
        QString failureDetails;
        if (entry.path().filename().wstring().find(L".saving-") != std::wstring::npos) {
            failureDetails = QStringLiteral("temporary candidate leaked\n%1\nlast map-save diagnostics:\n%2")
                .arg(DescribeMapSaveCandidate(entry), LastMapSaveDiagnostics(*window.project_->diagnosticsModel_));
        }
        QVERIFY2(entry.path().filename().wstring().find(L".saving-") == std::wstring::npos, qPrintable(failureDetails));
    }
    // 没有修改时写回被跳过，不生成新备份。
    window.SaveMapInPlace();
    QCOMPARE(backups().size(), std::size_t{2});
}

void NativeWorkbenchTests::RecoveryProjectSavesBackToOriginalMap() {
    // 作者反馈：点“恢复编辑”后保存地图写到了临时副本（Win32 1175），原图从未更新。
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("author-map.w3x")));
    QVERIFY(MakeArchive(original, 420).Succeeded());
    const QByteArray sourceBytes = ReadFile(original);
    const ScopedMapBackupPreferences backupPreferences(fixture.filePath(QStringLiteral("War3MapEditor Backups")));
    const auto savedUnits = [&original] {
        auto opened = War3Map::Mpq::MpqArchive::OpenReadOnly(original);
        if (!opened.Succeeded()) return std::vector<std::byte>{};
        auto unit = opened.value->ReadFile("war3map.w3u");
        return unit.Succeeded() ? *unit.value : std::vector<std::byte>{};
    };
    const auto recovery = NativePath(fixture.filePath(QStringLiteral("recovery.forge")));
    {
        MainWindow window(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(window.OpenMapArchivePath(original));
        QVERIFY(ChangeHealth(window, 555));
        QVERIFY(window.SaveEditorProjectToPath(recovery, true));
    }
    {
        // 原图未变：恢复后打开的就是原图，保存写回原位置并留备份。
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(recovery));
        QVERIFY(restored.project_->sourceArchivePath_ == original);
        QVERIFY(!restored.project_->MapSourceIsRecoveryCopy());
        QVERIFY(restored.project_->HasUnsavedMapChanges());
        QVERIFY2(restored.RunMapSave({}, true, false), qPrintable(LastMapSaveDiagnostics(*restored.project_->diagnosticsModel_)));
        QCOMPARE(savedUnits(), MinimalUnitData(555));
        QVERIFY(!restored.project_->HasUnsavedMapChanges());
    }
    {
        // 原图已变：只能打开临时基线；保存地图拒绝写临时副本，另存为仍可用，原图不被改动。
        QVERIFY(std::filesystem::remove(original));
        QVERIFY(MakeArchive(original, 430).Succeeded());
        const auto changedBytes = ReadFile(original);
        QVERIFY(changedBytes != sourceBytes);
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(recovery));
        QVERIFY(restored.project_->MapSourceIsRecoveryCopy());
        QVERIFY(restored.project_->recordedSourceMapPath_ == original);
        QVERIFY(restored.project_->AuthorMapPath() == original);
        const auto copy = restored.project_->sourceArchivePath_;
        const auto copyBytes = ReadFile(copy);
        QVERIFY(!restored.RunMapSave({}, true, false));
        QVERIFY(LastMapSaveDiagnostics(*restored.project_->diagnosticsModel_).contains(QStringLiteral("MAP_SAVE_TARGET_RECOVERY_COPY")));
        QCOMPARE(ReadFile(copy), copyBytes);
        QCOMPARE(ReadFile(original), changedBytes);
        // 临时基线会话再写恢复点时沿用原图路径，不把临时目录记成原图。
        const auto next = NativePath(fixture.filePath(QStringLiteral("recovery-2.forge")));
        QVERIFY(restored.SaveEditorProjectToPath(next, true));
        {
            MainWindow again(MainWindow::StartupMode::HeadlessProbe);
            QVERIFY(again.OpenEditorProjectPath(next));
            QVERIFY(again.project_->recordedSourceMapPath_ == original);
        }
        const auto output = NativePath(fixture.filePath(QStringLiteral("author-map (恢复).w3x")));
        QVERIFY2(restored.SaveMapArchiveToPath(output), qPrintable(LastMapSaveDiagnostics(*restored.project_->diagnosticsModel_)));
        QVERIFY(std::filesystem::exists(output));
        // 另存为之后作者的地图就是新文件，之后的保存与恢复点都指向它。
        QVERIFY(!restored.project_->MapSourceIsRecoveryCopy());
        QVERIFY(restored.project_->AuthorMapPath() == output);
    }
}

void NativeWorkbenchTests::ClassicV1FieldsUseGuiUndoSaveAndBackups() {
    using namespace War3Map;
    const auto root = qEnvironmentVariable("FORGE_OBJECT_V1_EVIDENCE_DIR",
        QDir::homePath() + QStringLiteral("/.forge-test-artifacts/object-v1"));
    QVERIFY(QDir().mkpath(root));
    QTemporaryDir fixture(QDir(root).filePath(QStringLiteral("qt-synthetic-XXXXXX")));
    QVERIFY(fixture.isValid()); fixture.setAutoRemove(false);
    const auto source = NativePath(fixture.filePath(QStringLiteral("original.w3x")));
    const auto working = NativePath(fixture.filePath(QStringLiteral("working-copy.w3x")));
    const auto undone = NativePath(fixture.filePath(QStringLiteral("undone.w3x")));
    const auto redone = NativePath(fixture.filePath(QStringLiteral("redone.w3x")));
    std::vector<MapArchiveMember> members{{"Unmanaged/preserve.bin", Bytes("original opaque payload")}};
    for (const auto type : kClassicObjectV1Types)
        members.push_back({std::string(ObjectData::NativeFileName(type)), MinimalObjectV1(type)});
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    QVERIFY(std::filesystem::copy_file(source, working));
    const auto sourceHash = Sha256File(source); QVERIFY(sourceHash.Succeeded() && sourceHash.value);
    const auto originalBytes = ReadFile(source); QVERIFY(!originalBytes.isEmpty());
    const ScopedMapBackupPreferences backupPreferences(fixture.filePath(QStringLiteral("War3MapEditor Backups")));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(working));
    // 原创物编保存夹具不伪造触发器或执行本机编译器；沿用既有纯数据保存策略。
    window.project_->compilerDataRoot_ = NativePath(fixture.filePath(QStringLiteral("no-compiler")));
    window.project_->activeUiDataRoot_.clear();
    window.project_->standardCatalog_.reset();
    window.project_->objectFieldSchema_.reset();
    window.objectsModule_->objectFieldModel_->SetContext(nullptr, nullptr);
    window.ActivateModule("objects");
    window.objectsModule_->showRawFields_->setChecked(true);
    const auto selectType = [&window](ObjectType type) {
        window.objectsModule_->objectCategories_->setCurrentRow(static_cast<int>(type));
        window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
        window.objectsModule_->objectListView_->selectRow(0);
        window.objectsModule_->ShowSelectedObject();
    };
    const auto valueRow = [&window] {
        const auto* model = window.objectsModule_->objectFieldModel_;
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto* modification = model->ModificationAt(row);
            if (modification && modification->key.fieldId == "I001") return row;
        }
        return -1;
    };
    for (const auto type : kClassicObjectV1Types) {
        const auto index = static_cast<std::size_t>(type);
        selectType(type);
        auto* native = window.project_->nativeObjectDocuments_[index].get();
        QVERIFY(native && native->Version() == 1 && !native->IsEditable());
        QVERIFY(native->Supports(ObjectData::NativeObjectOperation::EditExistingValue));
        auto* model = window.objectsModule_->objectFieldModel_;
        const int row = valueRow(); QVERIFY(row >= 0);
        QVERIFY(model->flags(model->index(row, 3)).testFlag(Qt::ItemIsEditable));
        const auto changed = std::int32_t{142} + static_cast<std::int32_t>(type);
        QVERIFY(model->setData(model->index(row, 3), changed, Qt::EditRole));
        QCOMPARE(window.project_->objectUndoStacks_[index]->count(), 1);
        QVERIFY(window.objectsModule_->undoObjectEdit_->isEnabled());
        window.objectsModule_->undoObjectEdit_->click();
        ObjectData::NativeObjectDocument preview = *native;
        QVERIFY(ObjectData::ApplyLniChangesToNative(preview, *window.project_->objectDocuments_[index]).Succeeded());
        QVERIFY(NativeObjectCodec(type).Serialize(preview).value == std::optional{MinimalObjectV1(type)});
        QVERIFY(window.objectsModule_->redoObjectEdit_->isEnabled());
        window.objectsModule_->redoObjectEdit_->click();
        QCOMPARE(IntegerOverride(window.project_->objectDocuments_[index]->FindObject({type, "hfoo"}), "I001").value_or(-1), changed);
    }
    const auto classicSaved = window.RunMapSave({}, true, false);
    ReportMapSaveFixture(QStringLiteral("after-classic-v1-in-place-save"), fixture.path());
    QVERIFY2(classicSaved, qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(window.project_->sourceArchivePath_ == working && !window.project_->HasUnsavedMapChanges());
    const auto backupDirectory = NativePath(fixture.filePath(QStringLiteral("War3MapEditor Backups")));
    QVERIFY(std::filesystem::is_directory(backupDirectory));
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(backupDirectory))
        if (entry.is_regular_file() && entry.path().extension() == ".w3x") backups.push_back(entry.path());
    QCOMPARE(backups.size(), std::size_t{1});
    QCOMPARE(ReadFile(backups.front()), originalBytes);
    const auto checkMembers = [&](const std::filesystem::path& path, bool changed) {
        auto archive = MapArchiveSession::Open(path);
        if (!archive.Succeeded() || !archive.value) return false;
        for (const auto type : kClassicObjectV1Types) {
            const auto member = archive.value->ReadMember(ObjectData::NativeFileName(type));
            const auto expected = MinimalObjectV1(type, changed ? 142 + static_cast<std::int32_t>(type) : 42);
            if (!member.Succeeded() || member.value != std::optional{expected}) return false;
            auto parsed = NativeObjectCodec(type).Parse(*member.value);
            const auto* native = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
            if (!native || native->Version() != 1) return false;
        }
        return archive.value->ReadMember("Unmanaged/preserve.bin").value == std::optional{Bytes("original opaque payload")};
    };
    QVERIFY(checkMembers(working, true));
    for (const auto type : kClassicObjectV1Types) {
        selectType(type);
        QVERIFY(window.objectsModule_->undoObjectEdit_->isEnabled());
        window.objectsModule_->undoObjectEdit_->click();
    }
    QVERIFY2(window.SaveMapArchiveToPath(undone), qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(checkMembers(undone, false));
    for (const auto type : kClassicObjectV1Types) {
        selectType(type);
        QVERIFY(window.objectsModule_->redoObjectEdit_->isEnabled());
        window.objectsModule_->redoObjectEdit_->click();
    }
    QVERIFY2(window.SaveMapArchiveToPath(redone), qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(checkMembers(redone, true));
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenMapArchivePath(redone));
    for (const auto type : kClassicObjectV1Types) {
        const auto index = static_cast<std::size_t>(type);
        QVERIFY(reopened.project_->nativeObjectDocuments_[index]);
        QCOMPARE(reopened.project_->nativeObjectDocuments_[index]->Version(), 1);
        QCOMPARE(IntegerOverride(reopened.project_->objectDocuments_[index]->FindObject({type, "hfoo"}), "I001").value_or(-1),
            142 + static_cast<std::int32_t>(type));
    }
    // LNI 可表达新增字段不代表 v1 可以写出该结构；实际 GUI 保存入口必须整体拒绝。
    const auto rejected = NativePath(fixture.filePath(QStringLiteral("rejected-new-field.w3x")));
    const auto redoneHash = Sha256File(redone); QVERIFY(redoneHash.Succeeded());
    auto* unit = reopened.project_->objectDocuments_[0].get(); QVERIFY(unit);
    const auto frozenNative = NativeObjectCodec(ObjectType::Unit).Serialize(*reopened.project_->nativeObjectDocuments_[0]);
    QVERIFY(unit->SetModification({ObjectType::Unit, "hfoo"}, {"NEW1"}, std::int32_t{99}).Succeeded());
    reopened.objectsModule_->objectCategories_->setCurrentRow(0);
    reopened.objectsModule_->MarkCurrentObjectDocumentDirty();
    QVERIFY(!reopened.SaveMapArchiveToPath(rejected));
    QVERIFY(!std::filesystem::exists(rejected));
    QVERIFY(NativeObjectCodec(ObjectType::Unit).Serialize(*reopened.project_->nativeObjectDocuments_[0]).value == frozenNative.value);
    QVERIFY(Sha256File(redone).value == redoneHash.value);
    QVERIFY(Sha256File(source).value == sourceHash.value);
    QCOMPARE(ReadFile(backups.front()), originalBytes);
    QByteArray receipt("evidence=Qt ObjectFieldModel edit, category Undo/Redo, MapSave, version1 reread, exact member undo, in-place backup\n");
    receipt += "source_sha256=" + QByteArray::fromStdString(*sourceHash.value) + "\n";
    receipt += "redone_sha256=" + QByteArray::fromStdString(*redoneHash.value) + "\n";
    auto workingArchive = MapArchiveSession::Open(working), undoneArchive = MapArchiveSession::Open(undone);
    auto redoneArchive = MapArchiveSession::Open(redone);
    QVERIFY(workingArchive.Succeeded() && undoneArchive.Succeeded() && redoneArchive.Succeeded());
    const auto backupHash = Sha256File(backups.front()); QVERIFY(backupHash.Succeeded() && backupHash.value);
    receipt += "working_sha256=" + QByteArray::fromStdString(workingArchive.value->SourceSha256()) + "\nundo_sha256="
        + QByteArray::fromStdString(undoneArchive.value->SourceSha256()) + "\nbackup_sha256=" + QByteArray::fromStdString(*backupHash.value) + "\n";
    for (const auto type : kClassicObjectV1Types) {
        const auto name = ObjectData::NativeFileName(type);
        const auto editedMember = redoneArchive.value->ReadMember(name), undoMember = undoneArchive.value->ReadMember(name);
        QVERIFY(editedMember.Succeeded() && editedMember.value && undoMember.Succeeded() && undoMember.value);
        const auto originalHash = Sha256(MinimalObjectV1(type)), editedHash = Sha256(*editedMember.value), undoHash = Sha256(*undoMember.value);
        QVERIFY(originalHash.Succeeded() && originalHash.value && editedHash.Succeeded() && editedHash.value && undoHash.Succeeded() && undoHash.value);
        receipt += QByteArray::fromStdString(std::string(name)) + "\t" + QByteArray::fromStdString(*originalHash.value) + "\t"
            + QByteArray::fromStdString(*editedHash.value) + "\t" + QByteArray::fromStdString(*undoHash.value) + "\n";
    }
    receipt += "script_rebuilt=false; visible_window=false; native_editor=false; game_runtime=false\n";
    QVERIFY(WriteText(fixture.filePath(QStringLiteral("receipt.txt")), receipt));
    qInfo().noquote() << "Object v1 Qt evidence:" << fixture.path() << "source SHA" << QString::fromStdString(*sourceHash.value);
}

void NativeWorkbenchTests::OptionalRealClassicV1FieldUsesGuiSave() {
    using namespace War3Map;
    const auto input = qEnvironmentVariable("FORGE_OBJECT_V1_MAP");
    if (input.isEmpty()) QSKIP("FORGE_OBJECT_V1_MAP is unset; synthetic tests do not establish real-map evidence");
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
        QSKIP("The real v1 map probe is background-only and never controls desktop windows");
    const auto root = qEnvironmentVariable("FORGE_OBJECT_V1_EVIDENCE_DIR",
        QDir::homePath() + QStringLiteral("/.forge-test-artifacts/object-v1"));
    QVERIFY(QDir().mkpath(root));
    QTemporaryDir fixture(QDir(root).filePath(QStringLiteral("qt-real-XXXXXX")));
    QVERIFY(fixture.isValid()); fixture.setAutoRemove(false);
    const auto source = NativePath(input);
    const auto copy = NativePath(fixture.filePath(QStringLiteral("readonly-input-copy.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("edited.w3x")));
    const auto undo = NativePath(fixture.filePath(QStringLiteral("undo.w3x")));
    const auto sourceHash = Sha256File(source); QVERIFY(sourceHash.Succeeded() && sourceHash.value);
    QVERIFY(std::filesystem::copy_file(source, copy));
    QVERIFY(Sha256File(copy).value == sourceHash.value);
    auto original = MapArchiveSession::Open(copy); QVERIFY(original.Succeeded() && original.value);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(copy));
    window.project_->compilerDataRoot_ = NativePath(fixture.filePath(QStringLiteral("no-compiler")));
    window.project_->activeUiDataRoot_.clear();
    window.project_->standardCatalog_.reset(); window.project_->objectFieldSchema_.reset();
    window.objectsModule_->objectFieldModel_->SetContext(nullptr, nullptr);
    window.ActivateModule("objects");
    window.objectsModule_->showRawFields_->setChecked(true);
    std::optional<ObjectData::ObjectKey> selected;
    ObjectData::ModificationKey field;
    std::int32_t before = 0;
    for (const auto type : kClassicObjectV1Types) {
        const auto* native = window.project_->nativeObjectDocuments_[static_cast<std::size_t>(type)].get();
        if (!native || native->Version() != 1) continue;
        for (const auto& object : native->Objects()) {
            if (object.parentRawcode) continue;
            for (const auto& modification : object.modifications) {
                if (const auto* value = std::get_if<std::int32_t>(&modification.value)) {
                    selected = object.key; field = modification.key; before = *value;
                    break;
                }
            }
            if (selected) break;
        }
        if (selected) break;
    }
    QVERIFY2(selected.has_value(), "Explicit real v1 fixture needs an existing original-object integer; no substitute map is used");
    const auto type = selected->type;
    window.objectsModule_->objectCategories_->setCurrentRow(static_cast<int>(type));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    int objectRow = -1;
    for (int row = 0; row < window.objectsModule_->objectListModel_->rowCount(); ++row) {
        const auto* object = window.objectsModule_->objectListModel_->ObjectAt(row);
        if (object && object->key == *selected) { objectRow = row; break; }
    }
    QVERIFY(objectRow >= 0);
    const auto proxy = window.objectsModule_->objectFilter_->mapFromSource(window.objectsModule_->objectListModel_->index(objectRow, 0));
    QVERIFY(proxy.isValid());
    window.objectsModule_->objectListView_->selectRow(proxy.row());
    window.objectsModule_->ShowSelectedObject();
    auto* model = window.objectsModule_->objectFieldModel_;
    int row = -1;
    for (int index = 0; index < model->rowCount(); ++index) {
        const auto* modification = model->ModificationAt(index);
        if (modification && modification->key == field) { row = index; break; }
    }
    QVERIFY(row >= 0);
    const auto after = before == std::numeric_limits<std::int32_t>::max() ? before - 1 : before + 1;
    QVERIFY(model->setData(model->index(row, 3), after, Qt::EditRole));
    QVERIFY(window.SaveMapArchiveToPath(candidate));
    auto saved = MapArchiveSession::Open(candidate); QVERIFY(saved.Succeeded() && saved.value);
    const auto name = ObjectData::NativeFileName(type);
    const auto sourceMember = original.value->ReadMember(name); QVERIFY(sourceMember.Succeeded() && sourceMember.value);
    const auto changedMember = saved.value->ReadMember(name); QVERIFY(changedMember.Succeeded() && changedMember.value);
    QVERIFY(changedMember.value != sourceMember.value);
    auto parsed = NativeObjectCodec(type).Parse(*changedMember.value);
    const auto* native = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
    QVERIFY(native && native->Version() == 1);
    const auto* changedObject = native->FindObject(*selected); QVERIFY(changedObject);
    QVERIFY(std::any_of(changedObject->modifications.begin(), changedObject->modifications.end(),
        [&](const auto& value) { return value.key == field && value.value == ObjectData::Value{after}; }));
    for (const auto& member : original.value->Members()) {
        if (ObjectData::ObjectTypeFromNativeFileName(member.name) == type) continue;
        QVERIFY(saved.value->ReadMember(member.name).value == std::optional{member.data});
    }
    QVERIFY(window.objectsModule_->undoObjectEdit_->isEnabled());
    window.objectsModule_->undoObjectEdit_->click();
    QVERIFY(window.SaveMapArchiveToPath(undo));
    auto restored = MapArchiveSession::Open(undo); QVERIFY(restored.Succeeded() && restored.value);
    for (const auto& member : original.value->Members())
        QVERIFY(restored.value->ReadMember(member.name).value == std::optional{member.data});
    QVERIFY(Sha256File(source).value == sourceHash.value && Sha256File(copy).value == sourceHash.value);
    const auto beforeMemberHash = Sha256(*sourceMember.value), afterMemberHash = Sha256(*changedMember.value);
    QVERIFY(beforeMemberHash.Succeeded() && afterMemberHash.Succeeded());
    QByteArray receipt("evidence=real source copy, Qt existing-field edit, common pure-data save, version1 reread, exact member undo\n");
    receipt += "source_sha256=" + QByteArray::fromStdString(*sourceHash.value) + "\n";
    receipt += "member=" + QByteArray::fromStdString(std::string(name)) + "\nobject=" + QByteArray::fromStdString(selected->rawcode)
        + "\nfield=" + QByteArray::fromStdString(field.fieldId) + "\nbefore_member_sha256=" + QByteArray::fromStdString(*beforeMemberHash.value)
        + "\nafter_member_sha256=" + QByteArray::fromStdString(*afterMemberHash.value) + "\ncandidate_sha256="
        + QByteArray::fromStdString(saved.value->SourceSha256()) + "\nundo_sha256=" + QByteArray::fromStdString(restored.value->SourceSha256()) + "\n";
    receipt += "before_value=" + QByteArray::number(before) + "\nafter_value=" + QByteArray::number(after) + "\n";
    receipt += "script_rebuilt=false; visible_window=false; native_editor=false; game_runtime=false\n";
    QVERIFY(WriteText(fixture.filePath(QStringLiteral("receipt.txt")), receipt));
    qInfo().noquote() << "Real object v1 Qt evidence:" << fixture.path() << "source SHA" << QString::fromStdString(*sourceHash.value);
}

void NativeWorkbenchTests::CancelAndWriteFailureKeepSessionBaseline() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("cancel-source.w3x")));
    QVERIFY(MakeArchive(original, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(original));
    QVERIFY(ChangeHealth(window, 777));
    auto* baseline = window.project_->nativeObjectDocuments_[0].get();
    QVERIFY(baseline);
    const auto before = NativeObjectCodec(ObjectType::Unit).Serialize(*baseline);
    QVERIFY(before.Succeeded());
    bool dialogCancelled = false;
    QObject cancellationScope;
    QTimer::singleShot(0, &cancellationScope, [&dialogCancelled] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                dialogCancelled = true;
                dialog->reject();
            }
        }
    });
    QVERIFY(!window.SaveAsMapArchive());
    QVERIFY(dialogCancelled);
    QVERIFY(!window.SaveMapArchiveToPath({}));
    const QString existing = fixture.filePath(QStringLiteral("must-remain.w3x"));
    QVERIFY(WriteText(existing, "existing output sentinel"));
    QVERIFY(!window.SaveMapArchiveToPath(NativePath(existing)));
    QCOMPARE(ReadFile(NativePath(existing)), QByteArray("existing output sentinel"));
    const auto absentParent = NativePath(fixture.filePath(QStringLiteral("absent-parent/new.w3x")));
    QVERIFY(!window.SaveMapArchiveToPath(absentParent));
    QVERIFY(!std::filesystem::exists(absentParent));
    QVERIFY(window.project_->sourceArchivePath_ == original);
    QCOMPARE(window.project_->nativeObjectDocuments_[0].get(), baseline);
    const auto after = NativeObjectCodec(ObjectType::Unit).Serialize(*baseline);
    QVERIFY(after.Succeeded());
    QVERIFY(*after.value == *before.value);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 777);
    QVERIFY(window.project_->objectDirty_[0]);
    QVERIFY(window.project_->pendingImports_.empty());
}

void NativeWorkbenchTests::MissingImportRejectsWithoutOutput() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("import-source.w3x")));
    const auto output = NativePath(fixture.filePath(QStringLiteral("must-not-exist.w3x")));
    QVERIFY(MakeArchive(original, 420).Succeeded());
    const auto sourceBytes = ReadFile(original);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(original));
    QVERIFY(ChangeHealth(window, 888));
    auto* baseline = window.project_->nativeObjectDocuments_[0].get();
    window.project_->pendingImports_.push_back({QStringLiteral("Custom/missing.txt"), fixture.filePath(QStringLiteral("missing.txt")), 7});
    window.project_->NotifyImportsChanged();
    QVERIFY(!window.SaveMapArchiveToPath(output));
    QVERIFY(!std::filesystem::exists(output));
    QVERIFY(window.project_->sourceArchivePath_ == original);
    QCOMPARE(window.project_->nativeObjectDocuments_[0].get(), baseline);
    const auto encoded = NativeObjectCodec(ObjectType::Unit).Serialize(*baseline);
    QVERIFY(encoded.Succeeded());
    QVERIFY(*encoded.value == MinimalUnitData(420));
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 888);
    QVERIFY(window.project_->objectDirty_[0]);
    QCOMPARE(window.project_->pendingImports_.size(), std::size_t{1});
    QCOMPARE(window.assetsModule_->PendingCount(), 1);
    QCOMPARE(ReadFile(original), sourceBytes);
}

void NativeWorkbenchTests::UnavailableSourceRejectsWithoutOutput() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto original = NativePath(fixture.filePath(QStringLiteral("valid-source.w3x")));
    const auto missing = NativePath(fixture.filePath(QStringLiteral("unavailable-source.w3x")));
    const auto output = NativePath(fixture.filePath(QStringLiteral("missing-source-output.w3x")));
    QVERIFY(MakeArchive(original, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(original));
    QVERIFY(ChangeHealth(window, 999));
    // 模拟源存储不可达，不删除仍被图标层持有的文件句柄。
    window.project_->sourceArchivePath_ = missing;
    auto* baseline = window.project_->nativeObjectDocuments_[0].get();
    QVERIFY(!window.SaveMapArchiveToPath(output));
    QVERIFY(!std::filesystem::exists(output));
    QVERIFY(window.project_->sourceArchivePath_ == missing);
    QCOMPARE(window.project_->nativeObjectDocuments_[0].get(), baseline);
    const auto encoded = NativeObjectCodec(ObjectType::Unit).Serialize(*baseline);
    QVERIFY(encoded.Succeeded());
    QVERIFY(*encoded.value == MinimalUnitData(420));
    QVERIFY(window.project_->objectDirty_[0]);
}

void NativeWorkbenchTests::UnauditedDeleteIsUnavailable() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    QVERIFY(WriteText(fixture.filePath(QStringLiteral("table/unit.ini")), "[U001]\n_parent = \"hfoo\"\nuhpm = 420\n"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(fixture.path())));
    auto* document = window.project_->objectDocuments_[0].get();
    QVERIFY(document);
    const auto before = document->DocumentFingerprint();
    window.objectsModule_->DeleteSelectedObject();
    QVERIFY(document->DocumentFingerprint() == before);
    QCOMPARE(document->Objects().size(), std::size_t{1});
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(window.project_->diagnosticsModel_->match(window.project_->diagnosticsModel_->index(0, 2), Qt::DisplayRole,
        QStringLiteral("OBJECT_DELETE_AUDIT_UNAVAILABLE"), 1, Qt::MatchExactly).size() == 1);
}

void NativeWorkbenchTests::ChineseFieldsRawToggleAndSearchPreserveDocument() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto lniPath = fixture.filePath(QStringLiteral("table/unit.ini"));
    QVERIFY(WriteText(lniPath, "[hfoo]\nudef = 7\n"));
    const auto sourceBytes = ReadFile(NativePath(lniPath));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(fixture.path())));
    QVERIFY(InstallSyntheticEditorContext(window));
    QVERIFY(window.objectsModule_->showRawFields_);
    QVERIFY(!window.objectsModule_->showRawFields_->isChecked());
    auto* document = window.project_->objectDocuments_[0].get();
    QVERIFY(document);
    const auto fingerprint = document->DocumentFingerprint();
    const auto objectCount = document->Objects().size();
    window.ActivateModule("objects");
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(0);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hfoo"));
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("生命上限")));
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("基础防御")));
    QVERIFY(!ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("internalslot")));
    const int normalCount = window.objectsModule_->standardFieldModel_->rowCount();
    QVERIFY(normalCount >= 3);

    window.objectsModule_->showRawFields_->setChecked(true);
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("hp")));
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("internalslot")));
    QVERIFY(!ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("生命上限")));
    QCOMPARE(document->DocumentFingerprint(), fingerprint);
    QVERIFY(!window.project_->objectDirty_[0]);
    window.objectsModule_->showRawFields_->setChecked(false);
    QCOMPARE(window.objectsModule_->standardFieldModel_->rowCount(), normalCount);
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldModel_, QStringLiteral("生命上限")));

    const int combatCategory = window.objectsModule_->objectFieldCategory_->findData(QStringLiteral("战斗"));
    QVERIFY(combatCategory >= 0);
    window.objectsModule_->objectFieldCategory_->setCurrentIndex(combatCategory);
    QCOMPARE(window.objectsModule_->standardFieldFilter_->rowCount(), 1);
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldFilter_, QStringLiteral("基础防御")));
    window.objectsModule_->objectFieldSearch_->setText(QStringLiteral("生命上限"));
    QCOMPARE(window.objectsModule_->standardFieldFilter_->rowCount(), 0);
    window.objectsModule_->objectFieldCategory_->setCurrentIndex(0);
    QCOMPARE(window.objectsModule_->standardFieldFilter_->rowCount(), 1);
    window.objectsModule_->objectFieldSearch_->setText(QStringLiteral("HP"));
    QCOMPARE(window.objectsModule_->standardFieldFilter_->rowCount(), 1);
    QVERIFY(ModelContains(*window.objectsModule_->standardFieldFilter_, QStringLiteral("生命上限")));
    window.objectsModule_->objectFieldSearch_->clear();

    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    QVERIFY(ModelContains(*window.objectsModule_->objectFieldModel_, QStringLiteral("生命上限")));
    const int mapCombatCategory = window.objectsModule_->objectFieldCategory_->findData(QStringLiteral("战斗"));
    QVERIFY(mapCombatCategory >= 0);
    window.objectsModule_->objectFieldCategory_->setCurrentIndex(mapCombatCategory);
    QCOMPARE(window.objectsModule_->objectFieldModel_->rowCount(), 1);
    QCOMPARE(FieldRow(*window.objectsModule_->objectFieldModel_, "udef"), 0);
    window.objectsModule_->objectFieldCategory_->setCurrentIndex(0);
    window.objectsModule_->objectFieldSearch_->setText(QStringLiteral("生命上限"));
    QCOMPARE(window.objectsModule_->objectFieldModel_->rowCount(), 1);
    window.objectsModule_->objectFieldSearch_->setText(QStringLiteral("uhpm"));
    QCOMPARE(window.objectsModule_->objectFieldModel_->rowCount(), 1);
    QCOMPARE(FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm"), 0);
    window.objectsModule_->objectFieldSearch_->clear();
    window.objectsModule_->showRawFields_->setChecked(true);
    QVERIFY(ModelContains(*window.objectsModule_->objectFieldModel_, QStringLiteral("uhpm")));
    QCOMPARE(document->DocumentFingerprint(), fingerprint);
    window.objectsModule_->showRawFields_->setChecked(false);
    QVERIFY(ModelContains(*window.objectsModule_->objectFieldModel_, QStringLiteral("生命上限")));
    QCOMPARE(document->DocumentFingerprint(), fingerprint);
    QCOMPARE(document->Objects().size(), objectCount);
    QVERIFY(!window.project_->objectDirty_[0]);
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 0);
    QCOMPARE(ReadFile(NativePath(lniPath)), sourceBytes);
}

void NativeWorkbenchTests::InheritedFieldEditAndResetPersistToNativeCandidate() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("inherited-source.w3x")));
    const auto edited = NativePath(fixture.filePath(QStringLiteral("inherited-edited.w3x")));
    const auto reset = NativePath(fixture.filePath(QStringLiteral("inherited-reset.w3x")));
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", SingleIntegerOverride("hfoo", "udef", 7), false},
        {"Synthetic/keep.txt", Bytes("unrelated member must survive"), false}
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    const auto sourceBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.ActivateModule("objects");
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    auto* model = window.objectsModule_->objectFieldModel_;
    auto* document = window.project_->objectDocuments_[0].get();
    const War3Map::ObjectData::ObjectKey key{ObjectType::Unit, "hfoo"};
    QVERIFY(document);
    QVERIFY(!IntegerOverride(document->FindObject(key), "uhpm").has_value());
    QCOMPARE(EffectiveInteger(*model, "uhpm").value_or(-1), 100);
    int row = FieldRow(*model, "uhpm");
    QVERIFY(row >= 0);
    QVERIFY(!model->CanReset(row));
    QVERIFY(model->flags(model->index(row, 3)).testFlag(Qt::ItemIsEditable));
    QVERIFY(model->setData(model->index(row, 3), 640, Qt::EditRole));
    QCOMPARE(IntegerOverride(document->FindObject(key), "uhpm").value_or(-1), 640);
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    QVERIFY(window.project_->objectDirty_[0]);

    // 经真实 model 写入 LNI 后，先独立序列化/解析，再走实际候选打包接口。
    War3Map::ObjectData::LniObjectCodec lniCodec(ObjectType::Unit);
    const auto encodedLni = lniCodec.Serialize(*document);
    QVERIFY(encodedLni.Succeeded());
    auto parsedLni = lniCodec.Parse(*encodedLni.value);
    QVERIFY(parsedLni.Succeeded());
    const auto* lni = dynamic_cast<const LniObjectDocument*>(parsedLni.document.get());
    QVERIFY(lni);
    QCOMPARE(IntegerOverride(lni->FindObject(key), "uhpm").value_or(-1), 640);
    QVERIFY(window.SaveMapArchiveToPath(edited));
    {
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(edited);
        QVERIFY(archive.Succeeded());
        auto member = archive.value->ReadFile("war3map.w3u");
        QVERIFY(member.Succeeded());
        auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(*member.value);
        QVERIFY(parsed.Succeeded());
        const auto* native = dynamic_cast<const War3Map::ObjectData::NativeObjectDocument*>(parsed.document.get());
        QVERIFY(native);
        QCOMPARE(IntegerOverride(native->FindObject(key), "uhpm").value_or(-1), 640);
        QCOMPARE(IntegerOverride(native->FindObject(key), "udef").value_or(-1), 7);
    }

    row = FieldRow(*model, "uhpm");
    QVERIFY(row >= 0);
    QVERIFY(model->CanReset(row));
    QVERIFY(model->ResetField(row));
    QVERIFY(!IntegerOverride(document->FindObject(key), "uhpm").has_value());
    QCOMPARE(EffectiveInteger(*model, "uhpm").value_or(-1), 100);
    QVERIFY(window.project_->objectDirty_[0]);
    QVERIFY(window.SaveMapArchiveToPath(reset));
    {
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(reset);
        QVERIFY(archive.Succeeded());
        auto member = archive.value->ReadFile("war3map.w3u");
        QVERIFY(member.Succeeded());
        auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(*member.value);
        QVERIFY(parsed.Succeeded());
        const auto* native = dynamic_cast<const War3Map::ObjectData::NativeObjectDocument*>(parsed.document.get());
        QVERIFY(native);
        QVERIFY(native->FindObject(key));
        QVERIFY(!IntegerOverride(native->FindObject(key), "uhpm").has_value());
        QCOMPARE(IntegerOverride(native->FindObject(key), "udef").value_or(-1), 7);
        QCOMPARE(native->FindObject(key)->modifications.size(), std::size_t{1});
        const auto preserved = archive.value->ReadFile("Synthetic/keep.txt");
        QVERIFY(preserved.Succeeded());
        QVERIFY(*preserved.value == Bytes("unrelated member must survive"));
    }
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::ObjectUndoRedoRemainBoundToCategory() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto unitPath = fixture.filePath(QStringLiteral("table/unit.ini"));
    const auto itemPath = fixture.filePath(QStringLiteral("table/item.ini"));
    QVERIFY(WriteText(unitPath, "[hfoo]\nudef = 7\n"));
    QVERIFY(WriteText(itemPath, "[itst]\nigol = 33\n"));
    const auto unitSource = ReadFile(NativePath(unitPath));
    const auto itemSource = ReadFile(NativePath(itemPath));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(fixture.path())));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.ActivateModule("objects");
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    auto* model = window.objectsModule_->objectFieldModel_;
    const War3Map::ObjectData::ObjectKey unitKey{ObjectType::Unit, "hfoo"};
    const War3Map::ObjectData::ObjectKey itemKey{ObjectType::Item, "itst"};
    auto* unit = window.project_->objectDocuments_[0].get();
    auto* item = window.project_->objectDocuments_[2].get();
    QVERIFY(unit && item);
    int row = FieldRow(*model, "uhpm");
    QVERIFY(row >= 0);
    QVERIFY(model->setData(model->index(row, 3), 222, Qt::EditRole));
    window.objectsModule_->objectCategories_->setCurrentRow(2);
    QCOMPARE(window.objectsModule_->currentObjectType_, 2);
    QCOMPARE(EffectiveInteger(*model, "igol").value_or(-1), 33);
    row = FieldRow(*model, "igol");
    QVERIFY(row >= 0);
    QVERIFY(model->setData(model->index(row, 3), 77, Qt::EditRole));
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 1);
    QVERIFY(window.objectsModule_->undoObjectEdit_->isEnabled());
    window.objectsModule_->undoObjectEdit_->click();
    QCOMPARE(IntegerOverride(item->FindObject(itemKey), "igol").value_or(-1), 33);
    QCOMPARE(IntegerOverride(unit->FindObject(unitKey), "uhpm").value_or(-1), 222);

    window.objectsModule_->objectCategories_->setCurrentRow(0);
    QVERIFY(window.objectsModule_->undoObjectEdit_->isEnabled());
    window.objectsModule_->undoObjectEdit_->click();
    QVERIFY(!IntegerOverride(unit->FindObject(unitKey), "uhpm").has_value());
    QCOMPARE(EffectiveInteger(*model, "uhpm").value_or(-1), 100);
    QCOMPARE(IntegerOverride(item->FindObject(itemKey), "igol").value_or(-1), 33);
    window.objectsModule_->objectCategories_->setCurrentRow(2);
    QVERIFY(window.objectsModule_->redoObjectEdit_->isEnabled());
    window.objectsModule_->redoObjectEdit_->click();
    QCOMPARE(IntegerOverride(item->FindObject(itemKey), "igol").value_or(-1), 77);
    QVERIFY(!IntegerOverride(unit->FindObject(unitKey), "uhpm").has_value());
    window.objectsModule_->objectCategories_->setCurrentRow(0);
    QVERIFY(window.objectsModule_->redoObjectEdit_->isEnabled());
    window.objectsModule_->redoObjectEdit_->click();
    QCOMPARE(IntegerOverride(unit->FindObject(unitKey), "uhpm").value_or(-1), 222);
    QCOMPARE(IntegerOverride(item->FindObject(itemKey), "igol").value_or(-1), 77);
    QCOMPARE(ReadFile(NativePath(unitPath)), unitSource);
    QCOMPARE(ReadFile(NativePath(itemPath)), itemSource);
}

void NativeWorkbenchTests::StandardObjectButtonCreatesOriginalOverrideAndReopens() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("standard-source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("standard-candidate.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto sourceBytes = ReadFile(source);
    const War3Map::ObjectData::ObjectKey key{ObjectType::Unit, "hrif"};
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(InstallSyntheticEditorContext(window));
    window.ActivateModule("objects");
    auto* edit = window.findChild<QPushButton*>(QStringLiteral("editStandardObjectButton"));
    QVERIFY(edit);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hrif"));
    QVERIFY(!edit->isEnabled());
    QVERIFY(window.OpenMapArchivePath(source));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(0);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hrif"));
    QVERIFY(edit->isEnabled());
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 0);
    edit->click();
    QCOMPARE(window.objectsModule_->objectSourceTabs_->currentIndex(), 1);
    auto* document = window.project_->objectDocuments_[0].get();
    QVERIFY(document);
    QVERIFY(document->FindObject(key));
    QVERIFY(!document->FindObject(key)->parentRawcode.has_value());
    QCOMPARE(document->Objects().size(), std::size_t{2});
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    QCOMPARE(EffectiveInteger(*window.objectsModule_->objectFieldModel_, "uhpm").value_or(-1), 200);
    const int row = FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm");
    QVERIFY(row >= 0);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(window.objectsModule_->objectFieldModel_->index(row, 3), 375, Qt::EditRole));
    QCOMPARE(IntegerOverride(document->FindObject(key), "uhpm").value_or(-1), 375);

    // 再次进入已有覆盖只改变选择，不产生第二个对象或撤销命令。
    const int commandCount = window.project_->objectUndoStacks_[0]->count();
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(0);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hrif"));
    edit->click();
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), commandCount);
    QCOMPARE(document->Objects().size(), std::size_t{2});
    QVERIFY(window.SaveMapArchiveToPath(candidate));
    QVERIFY(!window.project_->objectDirty_[0]);
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenMapArchivePath(candidate));
    const auto* native = reopened.project_->nativeObjectDocuments_[0].get();
    QVERIFY(native);
    QCOMPARE(native->OriginalObjectCount(), std::size_t{2});
    QCOMPARE(native->CustomObjectCount(), std::size_t{0});
    QVERIFY(native->FindObject(key));
    QCOMPARE(IntegerOverride(native->FindObject(key), "uhpm").value_or(-1), 375);
    QCOMPARE(IntegerOverride(native->FindObject({ObjectType::Unit, "hfoo"}), "uhpm").value_or(-1), 420);
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::MissingMemberCustomObjectKeepsNameFieldsAndUndoHistory() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("missing-item-source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("new-item-candidate.w3x")));
    const auto emptyCandidate = NativePath(fixture.filePath(QStringLiteral("creation-undone-before-save.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto sourceBytes = ReadFile(source);
    const War3Map::ObjectData::ObjectKey key{ObjectType::Item, "I001"};
    const auto name = QStringLiteral("合成自定义物品");
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.ActivateModule("objects");
    QVERIFY(!window.project_->objectDocuments_[2]);
    QVERIFY(!window.project_->nativeObjectDocuments_[2]);
    QVERIFY(window.objectsModule_->CreateObjectFromBase(2, QStringLiteral("itst"), QStringLiteral("I001"), name));
    QCOMPARE(window.objectsModule_->currentObjectType_, 2);
    QCOMPARE(window.objectsModule_->objectSourceTabs_->currentIndex(), 1);
    auto* document = window.project_->objectDocuments_[2].get();
    QVERIFY(document);
    QVERIFY(window.project_->nativeObjectDocuments_[2]);
    QCOMPARE(window.project_->nativeObjectDocuments_[2]->OriginalObjectCount(), std::size_t{0});
    QCOMPARE(window.project_->nativeObjectDocuments_[2]->CustomObjectCount(), std::size_t{0});
    QVERIFY(document->FindObject(key));
    QCOMPARE(document->FindObject(key)->parentRawcode.value_or(""), std::string("itst"));
    QCOMPARE(StringOverride(document->FindObject(key), "unam").value_or(""), name.toStdString());
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 1);
    QCOMPARE(EffectiveInteger(*window.objectsModule_->objectFieldModel_, "igol").value_or(-1), 25);
    int row = FieldRow(*window.objectsModule_->objectFieldModel_, "igol");
    QVERIFY(row >= 0);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(window.objectsModule_->objectFieldModel_->index(row, 3), 275, Qt::EditRole));
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 2);
    const auto editedFingerprint = document->DocumentFingerprint();
    window.project_->objectUndoStacks_[2]->undo();
    QVERIFY(!IntegerOverride(document->FindObject(key), "igol").has_value());
    QCOMPARE(EffectiveInteger(*window.objectsModule_->objectFieldModel_, "igol").value_or(-1), 25);
    window.project_->objectUndoStacks_[2]->undo();
    QVERIFY(!document->FindObject(key));
    QCOMPARE(window.objectsModule_->objectListModel_->rowCount(), 0);
    QCOMPARE(window.objectsModule_->objectFieldModel_->rowCount(), 0);
    QVERIFY(window.SaveMapArchiveToPath(emptyCandidate));
    {
        auto emptyArchive = War3Map::Mpq::MpqArchive::OpenReadOnly(emptyCandidate);
        QVERIFY(emptyArchive.Succeeded());
        QVERIFY(!emptyArchive.value->HasFile("war3map.w3t"));
    }
    window.project_->objectUndoStacks_[2]->redo();
    QVERIFY(document->FindObject(key));
    QCOMPARE(StringOverride(document->FindObject(key), "unam").value_or(""), name.toStdString());
    window.project_->objectUndoStacks_[2]->redo();
    QCOMPARE(document->DocumentFingerprint(), editedFingerprint);
    QCOMPARE(IntegerOverride(document->FindObject(key), "igol").value_or(-1), 275);
    QVERIFY(window.SaveMapArchiveToPath(candidate));

    auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(candidate);
    QVERIFY(archive.Succeeded());
    QVERIFY(archive.value->HasFile("war3map.w3t"));
    const auto unchangedUnit = archive.value->ReadFile("war3map.w3u");
    QVERIFY(unchangedUnit.Succeeded());
    QVERIFY(*unchangedUnit.value == MinimalUnitData(420));
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenMapArchivePath(candidate));
    QVERIFY(InstallSyntheticEditorContext(reopened));
    const auto* native = reopened.project_->nativeObjectDocuments_[2].get();
    QVERIFY(native);
    QCOMPARE(native->Version(), 2);
    QCOMPARE(native->OriginalObjectCount(), std::size_t{0});
    QCOMPARE(native->CustomObjectCount(), std::size_t{1});
    QVERIFY(native->FindObject(key));
    QCOMPARE(native->FindObject(key)->parentRawcode.value_or(""), std::string("itst"));
    QCOMPARE(StringOverride(native->FindObject(key), "unam").value_or(""), name.toStdString());
    QCOMPARE(IntegerOverride(native->FindObject(key), "igol").value_or(-1), 275);
    reopened.objectsModule_->SelectMapObject(2, QStringLiteral("I001"));
    QCOMPARE(EffectiveInteger(*reopened.objectsModule_->objectFieldModel_, "igol").value_or(-1), 275);
    QCOMPARE(reopened.objectsModule_->objectPreviewName_->text(), name);
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::SavedObjectCreationCanBeUndoneAndRedone() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("creation-source.w3x")));
    const auto created = NativePath(fixture.filePath(QStringLiteral("creation-saved.w3x")));
    const auto undone = NativePath(fixture.filePath(QStringLiteral("creation-undone.w3x")));
    const auto redone = NativePath(fixture.filePath(QStringLiteral("creation-redone.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto sourceBytes = ReadFile(source);
    const War3Map::ObjectData::ObjectKey originalKey{ObjectType::Unit, "hrif"};
    const War3Map::ObjectData::ObjectKey customKey{ObjectType::Item, "I001"};
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(0);
    window.objectsModule_->standardObjectBrowser_->SetCurrentId(QStringLiteral("hrif"));
    window.objectsModule_->EditSelectedStandardObject();
    QVERIFY(window.project_->objectDocuments_[0]->FindObject(originalKey));
    QVERIFY(window.objectsModule_->CreateObjectFromBase(2, QStringLiteral("itst"), QStringLiteral("I001"), QStringLiteral("撤销测试物品")));
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 1);
    QVERIFY(window.SaveMapArchiveToPath(created));
    const auto savedBytes = ReadFile(created);
    QVERIFY(window.project_->objectUndoStacks_[0]->isClean());
    QVERIFY(window.project_->objectUndoStacks_[2]->isClean());

    // 保存点之后撤销创建，下一次另存必须同时移除 Original 覆盖和 Custom 表记录。
    window.project_->objectUndoStacks_[0]->undo();
    window.project_->objectUndoStacks_[2]->undo();
    QVERIFY(!window.project_->objectDocuments_[0]->FindObject(originalKey));
    QVERIFY(!window.project_->objectDocuments_[2]->FindObject(customKey));
    QVERIFY(window.project_->objectDirty_[0]);
    QVERIFY(window.project_->objectDirty_[2]);
    QVERIFY(window.SaveMapArchiveToPath(undone));
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenMapArchivePath(undone));
    QVERIFY(reopened.project_->nativeObjectDocuments_[0]);
    QCOMPARE(reopened.project_->nativeObjectDocuments_[0]->OriginalObjectCount(), std::size_t{1});
    QVERIFY(!reopened.project_->nativeObjectDocuments_[0]->FindObject(originalKey));
    QCOMPARE(IntegerOverride(reopened.project_->nativeObjectDocuments_[0]->FindObject({ObjectType::Unit, "hfoo"}), "uhpm").value_or(-1), 420);
    QVERIFY(reopened.project_->nativeObjectDocuments_[2]);
    QCOMPARE(reopened.project_->nativeObjectDocuments_[2]->CustomObjectCount(), std::size_t{0});
    QVERIFY(!reopened.project_->nativeObjectDocuments_[2]->FindObject(customKey));

    window.project_->objectUndoStacks_[0]->redo();
    window.project_->objectUndoStacks_[2]->redo();
    QVERIFY(window.project_->objectDocuments_[0]->FindObject(originalKey));
    QVERIFY(window.project_->objectDocuments_[2]->FindObject(customKey));
    QVERIFY(window.SaveMapArchiveToPath(redone));
    MainWindow reopenedRedo(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopenedRedo.OpenMapArchivePath(redone));
    QCOMPARE(reopenedRedo.project_->nativeObjectDocuments_[0]->OriginalObjectCount(), std::size_t{2});
    QVERIFY(reopenedRedo.project_->nativeObjectDocuments_[0]->FindObject(originalKey));
    QCOMPARE(reopenedRedo.project_->nativeObjectDocuments_[2]->CustomObjectCount(), std::size_t{1});
    QCOMPARE(StringOverride(reopenedRedo.project_->nativeObjectDocuments_[2]->FindObject(customKey), "unam").value_or(""),
        QStringLiteral("撤销测试物品").toStdString());
    QCOMPARE(ReadFile(created), savedBytes);
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::ClonedCustomObjectRoundTripsWithUndo() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("clone-source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("clone-candidate.w3x")));
    const auto project = NativePath(fixture.filePath(QStringLiteral("clone-pending.forge")));
    const auto restoredFirst = NativePath(fixture.filePath(QStringLiteral("clone-restored-first.w3x")));
    const auto restoredSecond = NativePath(fixture.filePath(QStringLiteral("clone-restored-second.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto sourceBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    const QString name = QStringLiteral("复制来源物品");
    QVERIFY(window.objectsModule_->CreateObjectFromBase(2, QStringLiteral("itst"), QStringLiteral("I001"), name));
    QVERIFY(window.objectsModule_->CloneCustomObject(2, QStringLiteral("I001"), QStringLiteral("I002")));
    QVERIFY(!window.objectsModule_->CloneCustomObject(2, QStringLiteral("I001"), QStringLiteral("I002")));
    QVERIFY(!window.objectsModule_->CloneCustomObject(2, QStringLiteral("I001"), QStringLiteral("itst")));
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 2);
    const War3Map::ObjectData::ObjectKey copied{ObjectType::Item, "I002"};
    QCOMPARE(StringOverride(window.project_->objectDocuments_[2]->FindObject(copied), "unam").value_or(""), name.toStdString());
    window.project_->objectUndoStacks_[2]->undo();
    QVERIFY(!window.project_->objectDocuments_[2]->FindObject(copied));
    window.project_->objectUndoStacks_[2]->redo();
    QVERIFY(window.project_->objectDocuments_[2]->FindObject(copied));
    // 新建来源和克隆都尚未写入原生成员时保存工程；恢复必须保留完整复制来源。
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(window.project_->objectDirty_[2]);
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QVERIFY(restored.project_->objectDocuments_[2]);
    QCOMPARE(StringOverride(restored.project_->objectDocuments_[2]->FindObject(copied), "unam").value_or(""), name.toStdString());
    QVERIFY(restored.SaveMapArchiveToPath(restoredFirst));
    QVERIFY(restored.SaveMapArchiveToPath(restoredSecond));
    std::vector<std::byte> firstItemMember;
    for (const auto& path : {restoredFirst, restoredSecond}) {
        // 直接读取候选MPQ的实际成员，不以已打开窗口的内存对象代替写盘证明。
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(path);
        QVERIFY(archive.Succeeded());
        const auto member = archive.value->ReadFile("war3map.w3t");
        QVERIFY(member.Succeeded());
        auto parsed = NativeObjectCodec(ObjectType::Item).Parse(*member.value);
        QVERIFY(parsed.Succeeded());
        const auto* native = dynamic_cast<const War3Map::ObjectData::NativeObjectDocument*>(parsed.document.get());
        QVERIFY(native);
        QCOMPARE(native->CustomObjectCount(), std::size_t{2});
        for (const auto* rawcode : {"I001", "I002"}) {
            const War3Map::ObjectData::ObjectKey key{ObjectType::Item, rawcode};
            const auto* nativeObject = native->FindObject(key);
            QVERIFY(nativeObject);
            QCOMPARE(nativeObject->parentRawcode.value_or(""), std::string("itst"));
            QCOMPARE(StringOverride(nativeObject, "unam").value_or(""), name.toStdString());
        }
        if (firstItemMember.empty()) firstItemMember = *member.value;
        else QVERIFY(*member.value == firstItemMember);
    }
    QVERIFY(window.SaveMapArchiveToPath(candidate));
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenMapArchivePath(candidate));
    QVERIFY(reopened.project_->nativeObjectDocuments_[2]);
    QCOMPARE(reopened.project_->nativeObjectDocuments_[2]->CustomObjectCount(), std::size_t{2});
    const auto* nativeCopy = reopened.project_->nativeObjectDocuments_[2]->FindObject(copied);
    QVERIFY(nativeCopy);
    QCOMPARE(nativeCopy->parentRawcode.value_or(""), std::string("itst"));
    QCOMPARE(StringOverride(nativeCopy, "unam").value_or(""), name.toStdString());
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::MalformedExistingMemberCannotBeRecreatedAsEmpty() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("broken-item-source.w3x")));
    const auto candidate = NativePath(fixture.filePath(QStringLiteral("broken-item-preserved.w3x")));
    const auto malformed = Bytes("synthetic invalid item bytes");
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", MinimalUnitData(420), false},
        {"war3map.w3t", malformed, false}
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    const auto sourceBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    QVERIFY(!window.project_->objectDocuments_[2]);
    QVERIFY(!window.project_->nativeObjectDocuments_[2]);
    QVERIFY(!window.objectsModule_->EnsureObjectDocument(2));
    QVERIFY(!window.objectsModule_->CreateObjectFromBase(2, QStringLiteral("itst"), QStringLiteral("I001"), QStringLiteral("不能创建")));
    QVERIFY(!window.project_->objectDocuments_[2]);
    QVERIFY(!window.project_->nativeObjectDocuments_[2]);
    QVERIFY(!window.project_->objectDirty_[2]);
    QCOMPARE(window.project_->objectUndoStacks_[2]->count(), 0);
    QVERIFY(window.project_->diagnosticsModel_->match(window.project_->diagnosticsModel_->index(0, 2), Qt::DisplayRole,
        QStringLiteral("OBJECT_DOCUMENT_UNAVAILABLE"), 1, Qt::MatchExactly).size() == 1);
    QVERIFY(ChangeHealth(window, 812));
    QVERIFY(window.SaveMapArchiveToPath(candidate));
    auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(candidate);
    QVERIFY(archive.Succeeded());
    const auto preserved = archive.value->ReadFile("war3map.w3t");
    QVERIFY(preserved.Succeeded());
    QVERIFY(*preserved.value == malformed);
    const auto editedUnit = archive.value->ReadFile("war3map.w3u");
    QVERIFY(editedUnit.Succeeded());
    QVERIFY(*editedUnit.value == MinimalUnitData(812));
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::ParentChainNamesAndIconsReachListPreviewAndReferences() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto path = fixture.filePath(QStringLiteral("table/unit.ini"));
    const auto text = QStringLiteral(
        "[hfoo]\nunam = \"标准覆盖名称\"\nuico = \"Synthetic/Base.blp\"\n"
        "[U001]\n_parent = \"hfoo\"\nunam = \"父对象名称\"\nuico = \"Synthetic/Parent.blp\"\n"
        "[U002]\n_parent = \"U001\"\nudef = 9\n"
        "[U003]\n_parent = \"U002\"\nuico = \"\"\n").toUtf8();
    QVERIFY(WriteText(path, text));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(fixture.path())));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->SelectMapObject(0, QStringLiteral("U002"));
    QCOMPARE(window.objectsModule_->objectPreviewName_->text(), QStringLiteral("父对象名称"));
    QVERIFY(window.objectsModule_->objectPreviewSource_->toolTip().contains(QStringLiteral("Synthetic/Parent.blp")));
    const auto entryFor = [&window](const QString& rawcode) {
        return std::find_if(window.objectsModule_->objectViewEntries_.cbegin(), window.objectsModule_->objectViewEntries_.cend(),
            [&rawcode](const auto& entry) { return entry.kind == QStringLiteral("unit") && entry.id == rawcode; });
    };
    auto child = entryFor(QStringLiteral("U002"));
    QVERIFY(child != window.objectsModule_->objectViewEntries_.cend());
    QCOMPARE(child->name, QStringLiteral("父对象名称"));
    QCOMPARE(child->iconPath, QStringLiteral("Synthetic/Parent.blp"));
    QCOMPARE(window.objectsModule_->objectListView_->currentIndex().siblingAtColumn(1).data().toString(), QStringLiteral("父对象名称"));
    const auto cleared = entryFor(QStringLiteral("U003"));
    QVERIFY(cleared != window.objectsModule_->objectViewEntries_.cend());
    QVERIFY(cleared->iconPath.isEmpty());

    window.objectsModule_->SelectMapObject(0, QStringLiteral("U001"));
    const int row = FieldRow(*window.objectsModule_->objectFieldModel_, "unam");
    QVERIFY(row >= 0);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(window.objectsModule_->objectFieldModel_->index(row, 3), QStringLiteral("已编辑父对象"), Qt::EditRole));
    window.objectsModule_->SelectMapObject(0, QStringLiteral("U002"));
    QCOMPARE(window.objectsModule_->objectPreviewName_->text(), QStringLiteral("已编辑父对象"));
    child = entryFor(QStringLiteral("U002"));
    QVERIFY(child != window.objectsModule_->objectViewEntries_.cend());
    QCOMPARE(child->name, QStringLiteral("已编辑父对象"));
    QCOMPARE(child->iconPath, QStringLiteral("Synthetic/Parent.blp"));
    QCOMPARE(window.objectsModule_->objectListView_->currentIndex().siblingAtColumn(1).data().toString(), QStringLiteral("已编辑父对象"));
    QCOMPARE(ReadFile(NativePath(path)), text);
}

void NativeWorkbenchTests::EditorProjectRestoresObjectsAndImportSnapshots() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("edit.forge"));
    const auto output = NativePath(fixture.filePath("restored.w3x"));
    const auto asset = fixture.filePath("source.bin");
    QVERIFY(MakeArchive(source, 420).Succeeded());
    QVERIFY(WriteText(asset, "captured import"));
    const auto originalBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 991));
    window.assetsModule_->AddFiles({asset});
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(window.project_->objectDirty_[0]); // 工程保存不冒充原生地图已写回。
    QVERIFY(!window.project_->lastProjectFingerprint_.empty());
    // MPQ 名称大小写与斜杠等价；恢复提取和清单查询必须使用同一规则。
    auto savedProject = War3Map::MapArchiveSession::Open(project);
    QVERIFY(savedProject.Succeeded());
    std::vector<War3Map::MapArchiveMember> equivalentMembers(savedProject.value->Members().begin(), savedProject.value->Members().end());
    for (auto& member : equivalentMembers) {
        if (member.name == "imports/0") member.name = "Imports\\0";
        if (member.name == "baseline/source.w3x") member.name = "BASELINE\\source.w3x";
    }
    const auto equivalentProject = NativePath(fixture.filePath("equivalent.forge"));
    QVERIFY(War3Map::WriteVerifiedArchive(equivalentProject, equivalentMembers).Succeeded());
    QVERIFY(QFile::remove(asset));
    QVERIFY(QFile::remove(QString::fromStdWString(source.wstring())));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(equivalentProject));
    QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 991);
    QCOMPARE(restored.project_->pendingImports_.size(), std::size_t{1});
    QCOMPARE(ReadFile(NativePath(restored.project_->pendingImports_[0].diskPath)), QByteArray("captured import"));
    QVERIFY(restored.SaveMapArchiveToPath(output));
    auto archive = War3Map::MapArchiveSession::Open(output);
    QVERIFY(archive.Succeeded());
    QVERIFY(archive.value->HasMember("source.bin"));
    QVERIFY(archive.value->HasMember("war3map.imp"));
    const auto member = archive.value->ReadMember("war3map.w3u");
    QVERIFY(*member.value == MinimalUnitData(991));
    const auto baseline = ReadFile(restored.project_->sourceArchivePath_);
    QVERIFY(!baseline.isEmpty());
    QVERIFY(!originalBytes.isEmpty());
}

void NativeWorkbenchTests::ProjectSaveFailureAndInvalidRestorePreserveSession() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto bad = NativePath(fixture.filePath("bad.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    QVERIFY(MakeArchive(bad, 42).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 771));
    auto* document = window.project_->objectDocuments_[0].get();
    const auto bytes = ReadFile(source);
    QVERIFY(!window.SaveEditorProjectToPath(source));
    QCOMPARE(ReadFile(source), bytes);
    QVERIFY(window.project_->lastProjectFingerprint_.empty());
    QVERIFY(!window.OpenEditorProjectPath(bad));
    QCOMPARE(window.project_->objectDocuments_[0].get(), document);
    QCOMPARE(Health(*document).value_or(-1), 771);
    QVERIFY(window.project_->objectDirty_[0]);
}

void NativeWorkbenchTests::ChangedSourceRejectsMapAndProjectSave() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto output = NativePath(fixture.filePath("output.w3x"));
    const auto project = NativePath(fixture.filePath("output.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 777));
    QVERIFY(QFile::remove(QString::fromStdWString(source.wstring())));
    QVERIFY(MakeArchive(source, 123).Succeeded());
    QVERIFY(!window.SaveMapArchiveToPath(output));
    QVERIFY(!window.SaveEditorProjectToPath(project));
    QVERIFY(!std::filesystem::exists(output));
    QVERIFY(!std::filesystem::exists(project));
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 777);
    QVERIFY(window.project_->objectDirty_[0]);
}

// 资源改名 / 删除在另存副本时生效并同步导入清单；缺少暂停图标的按钮图标在保存时生成 DISBTN（灰度、BLP1）。
void NativeWorkbenchTests::AssetEditsSaveMovesDeletesAndDisabledIcons() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("assets.w3x"));
    const auto output = NativePath(fixture.filePath("assets-copy.w3x"));
    War3Map::Image::DecodedImage icon;
    icon.width = 4;
    icon.height = 4;
    for (int pixel = 0; pixel < 16; ++pixel) {
        for (const int value : {pixel * 16, 200 - pixel * 8, 40, 255}) icon.rgba.push_back(static_cast<std::byte>(value));
    }
    const auto blp = War3Map::Image::EncodeBlpPaletted(icon);
    QVERIFY(blp.Succeeded());
    const auto roundTrip = War3Map::Image::DecodeBlp(*blp.value);
    QVERIFY(roundTrip.Succeeded() && roundTrip.value->rgba == icon.rgba);
    std::vector<std::byte> imp;
    const auto u32 = [&imp](std::uint32_t value) { for (int shift = 0; shift < 32; shift += 8) imp.push_back(static_cast<std::byte>((value >> shift) & 0xFF)); };
    const auto record = [&imp](std::string_view path) {
        imp.push_back(std::byte{13});
        for (const char c : path) imp.push_back(static_cast<std::byte>(c));
        imp.push_back(std::byte{0});
    };
    u32(1); u32(2);
    record("war3mapImported\\BTNHero.blp");
    record("Units\\Foo.mdx");
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", MinimalUnitData(420), false},
        {"war3map.imp", imp, false},
        {"war3mapImported\\BTNHero.blp", *blp.value, false},
        {"Units\\Foo.mdx", Bytes("MDLX synthetic"), false},
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    auto& project = *window.project_;
    QCOMPARE(project.MissingDisabledIcons(), QStringList{QStringLiteral("war3mapImported\\BTNHero.blp")});
    // 非法路径被拒绝，不改变状态。
    QVERIFY(!project.MoveAsset(QStringLiteral("Units\\Foo.mdx"), QStringLiteral("单位\\Foo.mdx")).isEmpty());
    QVERIFY(!project.MoveAsset(QStringLiteral("Units\\Foo.mdx"), QStringLiteral("war3map.j")).isEmpty());
    QVERIFY(project.assetEdits_.empty());
    QVERIFY(project.MoveAsset(QStringLiteral("war3mapImported\\BTNHero.blp"),
        QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp")).isEmpty());
    QVERIFY(project.RemoveAsset(QStringLiteral("Units\\Foo.mdx")).isEmpty());
    QCOMPARE(project.assetEdits_.size(), std::size_t{2});
    QVERIFY(project.HasPendingAssetEdits());
    QVERIFY(window.SaveMapArchiveToPath(output));
    QVERIFY(project.assetEdits_.empty());

    auto archive = War3Map::MapArchiveSession::Open(output);
    QVERIFY(archive.Succeeded());
    QVERIFY(archive.value->HasMember("ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
    QVERIFY(!archive.value->HasMember("war3mapImported\\BTNHero.blp"));
    QVERIFY(!archive.value->HasMember("Units\\Foo.mdx"));
    const auto disabled = archive.value->ReadMember("ReplaceableTextures\\CommandButtonsDisabled\\DISBTNHero.blp");
    QVERIFY(disabled.Succeeded());
    const auto gray = War3Map::Image::DecodeBlp(*disabled.value);
    QVERIFY(gray.Succeeded() && gray.value->width == 4);
    for (std::size_t pixel = 0; pixel < 16; ++pixel) {
        QCOMPARE(gray.value->rgba[pixel * 4], gray.value->rgba[pixel * 4 + 1]);
        QVERIFY(std::to_integer<int>(gray.value->rgba[pixel * 4]) <= 140); // 压暗
    }
    const auto table = War3Map::Resources::ImportTable::Parse(*archive.value->ReadMember("war3map.imp").value);
    QVERIFY(table.Succeeded());
    QStringList paths;
    for (const auto& entry : table.value->Entries()) paths << QString::fromStdString(entry.path);
    QCOMPARE(paths, (QStringList{QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNHero.blp"),
        QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\DISBTNHero.blp")}));
    // 保存后暂停图标已存在，不再提示。
    QVERIFY(project.MissingDisabledIcons().isEmpty());
}

void NativeWorkbenchTests::ImportsRegisterNativeImportTable() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto output = NativePath(fixture.filePath("output.w3x"));
    const auto asset = fixture.filePath("test.wav");
    QVERIFY(MakeArchive(source, 420).Succeeded());
    QVERIFY(WriteText(asset, "synthetic wave bytes"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QCOMPARE(window.assetsModule_->AssetCount(), 0); // 地图自身的数据文件（war3map.*）不作为资源列出
    window.assetsModule_->AddFiles({asset, asset});
    QCOMPARE(window.project_->pendingImports_.size(), std::size_t{1});
    QVERIFY(window.SaveMapArchiveToPath(output));
    auto archive = War3Map::MapArchiveSession::Open(output);
    QVERIFY(archive.Succeeded());
    auto bytes = archive.value->ReadMember("war3map.imp");
    QVERIFY(bytes.Succeeded());
    const auto table = War3Map::Resources::ImportTable::Parse(*bytes.value);
    QVERIFY(table.Succeeded());
    QCOMPARE(table.value->Entries().size(), std::size_t{1});
    const auto resolved = War3Map::Resources::ResolveArchivePath(table.value->Entries()[0]);
    QVERIFY(resolved.Succeeded());
    QVERIFY(archive.value->HasMember(*resolved.value));
}

void NativeWorkbenchTests::BuildCheckDoesNotSaveOrClearEdits() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 999));
    const auto bytes = ReadFile(source);
    const auto count = window.project_->diagnosticsModel_->rowCount();
    window.CheckGameBuild();
    QVERIFY(window.project_->diagnosticsModel_->rowCount() > count);
    QCOMPARE(ReadFile(source), bytes);
    QVERIFY(window.project_->objectDirty_[0]);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 999);
}

void NativeWorkbenchTests::TriggerProjectEmbedsDefinitionsAndRefusesUnbuiltScript() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("triggers.w3x"));
    const auto project = NativePath(fixture.filePath("triggers.forge"));
    const auto output = NativePath(fixture.filePath("triggers-copy.w3x"));
    const auto definitions = fixture.filePath("definitions");
    QVERIFY(WriteText(definitions + "/ui/config", "base\n"));
    QVERIFY(WriteText(definitions + "/ui/base/action.txt", "[DoNothing]\ntitle=\"Empty action\"\ndescription=\"Empty action\"\n"));
    std::vector<std::byte> wtg = Bytes("WTG!");
    const auto text = [](std::vector<std::byte>& bytes, std::string_view value) {
        const auto data = Bytes(value); bytes.insert(bytes.end(), data.begin(), data.end()); bytes.push_back(std::byte{0});
    };
    AppendI32(wtg, 7); AppendI32(wtg, 1); AppendI32(wtg, 1); text(wtg, "Synthetic"); AppendI32(wtg, 0);
    AppendI32(wtg, 2); AppendI32(wtg, 0); AppendI32(wtg, 1);
    text(wtg, "Before"); text(wtg, "Synthetic recovery fixture");
    for (const auto flag : {0, 1, 0, 1, 0, 1, 1, 2}) AppendI32(wtg, flag);
    text(wtg, "DoNothing"); AppendI32(wtg, 1); AppendI32(wtg, 0);
    std::vector<std::byte> wct;
    AppendI32(wct, 1); text(wct, ""); AppendI32(wct, 0); AppendI32(wct, 1); AppendI32(wct, 0);
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.wtg", wtg}, {"war3map.wct", wct}, {"war3map.j", Bytes("original script bytes")}
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(definitions));
    QVERIFY(catalog.Succeeded());
    auto parsed = War3Map::Trigger::WtgWctCodec{}.Parse(wtg, wct, *catalog.value);
    QVERIFY(parsed.Succeeded());
    window.project_->uiCatalog_ = std::make_unique<War3Map::Trigger::UiCatalog>(std::move(*catalog.value));
    window.project_->activeUiDataRoot_ = NativePath(definitions);
    window.project_->triggerDocument_ = std::make_unique<War3Map::Trigger::TriggerDocument>(std::move(*parsed.value));
    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RenameTrigger(*window.project_->triggerDocument_, window.project_->triggerDocument_->triggers[0].id, "Recovered").Succeeded());
    window.project_->triggerDirty_ = true;
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(QFile::remove(definitions + "/ui/config"));
    QVERIFY(QFile::remove(QString::fromStdWString(source.wstring())));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QVERIFY(restored.project_->triggerDocument_);
    QCOMPARE(restored.project_->triggerDocument_->triggers[0].name.text, std::string("Recovered"));
    QVERIFY(restored.project_->triggerDirty_);
    // 触发器改动必须真正重建游戏脚本；缺少编译器时拒绝写出，不能只写 WTG 沿用旧 JASS 报成功。
    QVERIFY(!restored.SaveMapArchiveToPath(output));
    QVERIFY(!std::filesystem::exists(output));
    QVERIFY(restored.project_->triggerDirty_);
    QCOMPARE(restored.project_->triggerDocument_->triggers[0].name.text, std::string("Recovered"));
    // 恢复点捕获仍保有焦点的注释编辑；可信前一恢复包可由共用 Store 后台替换。
    const auto recovery = NativePath(fixture.filePath("recovery.forge"));
    QVERIFY(restored.SaveEditorProjectToPath(recovery, true));
    restored.project_->recoveryPath_ = recovery;
    const auto oldRecoveryHash = War3Map::Sha256File(recovery); QVERIFY(oldRecoveryHash.Succeeded());
    restored.project_->recoveryFileSha256_ = *oldRecoveryHash.value;
    QVERIFY(restored.triggersModule_->Reveal({"triggers", "trigger",
        QString::number(restored.project_->triggerDocument_->triggers[0].id), {}}));
    restored.triggersModule_->CommentEditor()->setPlainText("Unfocused recovery comment");
    const auto previousRecovery = QSettings{}.value("project/latestRecovery");
    restored.WriteRecovery();
    QTRY_VERIFY_WITH_TIMEOUT(!restored.writingRecovery_, 10000);
    QCOMPARE(restored.project_->triggerDocument_->triggers[0].description.text, std::string("Unfocused recovery comment"));
    if (previousRecovery.isValid()) QSettings{}.setValue("project/latestRecovery", previousRecovery);
    else QSettings{}.remove("project/latestRecovery");
    QCOMPARE(restored.project_->recoveryPath_, recovery);
    MainWindow recovered(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(recovered.OpenEditorProjectPath(recovery));
    QCOMPARE(recovered.project_->triggerDocument_->triggers[0].description.text, std::string("Unfocused recovery comment"));
}

void NativeWorkbenchTests::LuaMapTargetSavesGuiTriggersAndRebindsCandidate() {
    const auto pattern = LuaQtEvidencePattern();
    QVERIFY(!pattern.isEmpty());
    QTemporaryDir fixture(pattern);
    QVERIFY(fixture.isValid());
    fixture.setAutoRemove(false); // 保留原创源图、UI、档案和配置，供同契约CLI/MCP继续验证。
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    std::filesystem::path configuration;
    QVERIFY(InstallLuaSaveFixture(window, fixture, 31, configuration));
    const auto source = window.project_->sourceArchivePath_;
    const auto originalHash = War3Map::Sha256File(source);
    QVERIFY(originalHash.Succeeded());
    auto& document = *window.project_->triggerDocument_;
    QCOMPARE(document.triggers.size(), std::size_t{1});
    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RenameTrigger(document,
        document.triggers[0].id, "自然语言T已保存").Succeeded());
    window.project_->triggerDirty_ = true;
    window.project_->ObserveTriggerEdit();
    const auto configured = window.project_->ConfigureLuaMapTarget(configuration);
    QVERIFY2(configured.Succeeded(), qPrintable(LuaQtDiagnostics(configured)));
    QVERIFY(window.project_->luaMapTarget_.has_value());
    QCOMPARE(window.project_->luaMapTargetSource_, source);
    const auto before = window.project_->archiveSession_->ReadMember("war3map.w3i");
    QVERIFY(before.Succeeded());
    QCOMPARE(War3Map::MapData::MapSettings::Parse(*before.value).value->scriptLanguage, std::int32_t{0});

    const auto candidate = NativePath(fixture.filePath("candidate-lua.w3x"));
    const bool saved = window.SaveMapArchiveToPath(candidate); // 实际Qt Prepare -> Run -> Commit，无模态窗口。
    ReportMapSaveFixture(QStringLiteral("after-lua-save-as"), fixture.path());
    QVERIFY2(saved, qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY(!window.project_->triggerDirty_);
    QVERIFY(!window.project_->savingMap_);
    QCOMPARE(window.project_->sourceArchivePath_, candidate);
    QCOMPARE(window.project_->luaMapTargetSource_, candidate);
    QCOMPARE(War3Map::Sha256File(source).value, originalHash.value);
    const auto reopened = War3Map::MapArchiveSession::Open(candidate);
    QVERIFY(reopened.Succeeded());
    QCOMPARE(window.project_->archiveSession_->SourceSha256(), reopened.value->SourceSha256());
    const std::array<std::string, 4> scripts{"war3map.lua", "scripts\\war3map.lua", "war3map.j", "scripts\\war3map.j"};
    const auto exact = reopened.value->CaptureExactMembers(scripts);
    QVERIFY(exact.Succeeded());
    QVERIFY((*exact.value)[0].present && !(*exact.value)[1].present
        && !(*exact.value)[2].present && !(*exact.value)[3].present);
    const auto& luaBytes = (*exact.value)[0].bytes;
    const std::string_view lua(reinterpret_cast<const char*>(luaBytes.data()), luaBytes.size());
    QVERIFY(lua.find("function config()") != lua.npos && lua.find("function main()") != lua.npos
        && lua.find("DoNothing()") != lua.npos);
    const auto settingsBytes = reopened.value->ReadMember("war3map.w3i");
    QVERIFY(settingsBytes.Succeeded());
    const auto settings = War3Map::MapData::MapSettings::Parse(*settingsBytes.value);
    QVERIFY(settings.Succeeded());
    QCOMPARE(settings.value->SourceFormatVersion(), std::optional<std::int32_t>{31});
    QCOMPARE(settings.value->scriptLanguage, std::int32_t{1});
    const auto wtg = reopened.value->ReadMember("war3map.wtg"), wct = reopened.value->ReadMember("war3map.wct");
    QVERIFY(wtg.Succeeded() && wct.Succeeded());
    const auto gui = War3Map::Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *window.project_->uiCatalog_);
    QVERIFY(gui.Succeeded());
    QCOMPARE(gui.value->triggers.size(), std::size_t{1});
    QCOMPARE(gui.value->triggers[0].name.text, std::string("自然语言T已保存"));
    QCOMPARE(gui.value->triggers[0].customTextFlag, std::int32_t{0});
    QVERIFY(gui.value->triggers[0].customText.text.empty() && gui.value->wct.customCode.text.empty());
    QCOMPARE(gui.value->triggers[0].ecas.size(), std::size_t{1});
    QCOMPARE(gui.value->ecaNodes[gui.value->triggers[0].ecas[0]].name.text, std::string("DoNothing"));
    QCOMPARE(reopened.value->ReadMember("unmanaged.bin").value,
        std::optional(Bytes("original Qt fixture bytes")));
    qInfo().noquote() << "Lua Qt SaveAs candidate=" << QString::fromStdWString(candidate.wstring())
        << "SHA=" << QString::fromStdString(reopened.value->SourceSha256())
        << "original retained=" << fixture.path() << "; actual Lua syntax/archive only, no game acceptance";
}

void NativeWorkbenchTests::LiveEditorNaturalTUsesActualGuiUndoAndLuaMapSave() {
    const auto pattern = LuaQtEvidencePattern();
    QVERIFY(!pattern.isEmpty());
    QTemporaryDir fixture(pattern);
    QVERIFY(fixture.isValid());
    fixture.setAutoRemove(false); // 保留唯一原创源图、副本、实际工具身份与失败证据。
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    std::filesystem::path configuration;
    QVERIFY(InstallLuaSaveFixture(window, fixture, 31, configuration));
    const auto source = window.project_->sourceArchivePath_;
    const auto sourceHash = War3Map::Sha256File(source);
    QVERIFY(sourceHash.Succeeded());

    // 小样例仍是自然语言 T。通过正常成对绑定通知实际模块，建立可靠打开基线。
    window.project_->ReplaceTriggerWorkspace(
        std::make_unique<War3Map::Trigger::UiCatalog>(*window.project_->uiCatalog_),
        std::make_unique<War3Map::Trigger::TriggerDocument>(*window.project_->triggerDocument_));
    window.ResetEditTracking();
    QVERIFY(window.project_->editTrackingReady_ && !window.project_->editTrackingFailed_);
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    const auto configured = window.project_->ConfigureLuaMapTarget(configuration);
    QVERIFY2(configured.Succeeded(), qPrintable(LuaQtDiagnostics(configured)));

    auto* document = window.project_->triggerDocument_.get();
    auto* undo = window.project_->triggerUndoStack_;
    QVERIFY(document && undo);
    QCOMPARE(document->triggers.size(), std::size_t{1});
    QCOMPARE(undo->count(), 0);
    const auto baseline = War3Map::Trigger::WtgWctCodec{}.Serialize(*document, *window.project_->uiCatalog_);
    QVERIFY(baseline.Succeeded());

    War3Map::QtApp::LiveEditorTools tools(*window.project_);
    tools.SetBeforeWrite([&window] {
        if (window.project_->savingMap_) return false;
        window.triggersModule_->CommitPendingEdits();
        return true;
    });
    int saveCalls = 0;
    std::filesystem::path delegatedPath;
    bool delegatedInPlace = true;
    tools.SetSaveHandler([&](const std::filesystem::path& path, bool inPlace) {
        ++saveCalls; delegatedPath = path; delegatedInPlace = inPlace;
        // 真正 Qt Prepare -> 脚本生成/真实 Lua 检查 -> 归档写出/回读 -> Commit。
        // 不以回调桩、手写 Lua 或直接 MPQ 替换代替原地图保存。
        const auto saved = window.RunMapSave(path, inPlace, false);
        ReportMapSaveFixture(QStringLiteral("after-live-editor-lua-save"), fixture.path());
        if (!saved) qWarning().noquote() << LastMapSaveDiagnostics(*window.project_->diagnosticsModel_);
        return saved;
    });
    const auto context = [](const QJsonObject& state) {
        return QJsonObject{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")}};
    };
    const auto diagnostics = [](const QJsonObject& result) {
        return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    };
    const auto state = tools.Invoke("editor_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(diagnostics(state)));
    QCOMPARE(state.value("scope").toString(), QStringLiteral("gui-live-document"));
    QCOMPARE(state.value("source_sha256").toString(), QString::fromStdString(*sourceHash.value));
    auto queryRequest = context(state); queryRequest.insert("table", "triggers");
    const auto queried = tools.Invoke("editor_query_trigger_graph", queryRequest);
    QVERIFY2(queried.value("ok").toBool(), qPrintable(diagnostics(queried)));
    QCOMPARE(queried.value("rows").toArray().size(), qsizetype{1});
    const auto originalRow = queried.value("rows").toArray()[0].toObject();
    QCOMPARE(originalRow.value("ref").toObject().value("id").toString(), QString::number(document->triggers[0].id));
    const QString name = QStringLiteral("0-0-1 实时Agent自然语言T");
    auto editRequest = context(state);
    editRequest.insert("expected", queried.value("binding"));
    editRequest.insert("operations", QJsonArray{QJsonObject{{"op", "rename"}, {"target", originalRow.value("ref")},
        {"expected_name", originalRow.value("name")}, {"name", name}}});
    const auto edited = tools.Invoke("editor_edit_trigger_graph", editRequest);
    QVERIFY2(edited.value("ok").toBool(), qPrintable(diagnostics(edited)));
    QCOMPARE(window.project_->triggerDocument_.get(), document);
    QCOMPARE(window.project_->triggerUndoStack_, undo);
    QCOMPARE(undo->count(), 1);
    QCOMPARE(document->triggers[0].name.text, name.toStdString());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    const auto after = War3Map::Trigger::WtgWctCodec{}.Serialize(*document, *window.project_->uiCatalog_);
    QVERIFY(after.Succeeded());
    QVERIFY(after.value->wtg != baseline.value->wtg);
    QCOMPARE(after.value->wct, baseline.value->wct);
    undo->undo();
    QCOMPARE(window.project_->triggerDocument_.get(), document);
    const auto undone = War3Map::Trigger::WtgWctCodec{}.Serialize(*document, *window.project_->uiCatalog_);
    QVERIFY(undone.Succeeded());
    QCOMPARE(undone.value->wtg, baseline.value->wtg); QCOMPARE(undone.value->wct, baseline.value->wct);
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    undo->redo();
    QCOMPARE(window.project_->triggerDocument_.get(), document);
    QCOMPARE(window.project_->triggerUndoStack_, undo);
    const auto redone = War3Map::Trigger::WtgWctCodec{}.Serialize(*document, *window.project_->uiCatalog_);
    QVERIFY(redone.Succeeded());
    QCOMPARE(redone.value->wtg, after.value->wtg); QCOMPARE(redone.value->wct, after.value->wct);

    const auto beforeSave = tools.Invoke("editor_state", {});
    queryRequest = context(beforeSave); queryRequest.insert("table", "triggers");
    const auto currentGraph = tools.Invoke("editor_query_trigger_graph", queryRequest);
    QVERIFY2(currentGraph.value("ok").toBool(), qPrintable(diagnostics(currentGraph)));
    const auto currentRow = currentGraph.value("rows").toArray()[0].toObject();
    QVERIFY(!currentRow.value("name_derived_trigger_symbol").toString().isEmpty());
    // 查询中的名称派生符号是 WE/JASS 名称事实；Lua 后端按保存前的稳定 T 身份
    // 生成 F<ID>，重命名不会更换该身份。不能把两种后端的符号混成同一规则。
    const auto luaSuffix = "F" + currentRow.value("ref").toObject().value("id").toString().toStdString();
    const auto luaVariable = "gg_trg_" + luaSuffix, luaConditions = "Trig_" + luaSuffix + "_Conditions";
    const auto luaActions = "Trig_" + luaSuffix + "_Actions", luaInitializer = "InitTrig_" + luaSuffix;
    const auto candidate = NativePath(fixture.filePath("live-editor-natural-t-lua.w3x"));
    QVERIFY(!std::filesystem::exists(candidate));
    auto saveRequest = context(beforeSave);
    saveRequest.insert("path", QString::fromStdWString(candidate.wstring())); saveRequest.insert("in_place", false);
    QSignalSpy activity(&tools, &War3Map::QtApp::LiveEditorTools::Activity);
    const auto saved = tools.Invoke("editor_save_map", saveRequest);
    if (!saved.value("ok").toBool()) {
        for (int row = 0; row < window.project_->diagnosticsModel_->rowCount(); ++row) {
            QStringList line;
            for (int column = 0; column < window.project_->diagnosticsModel_->columnCount(); ++column)
                line << window.project_->diagnosticsModel_->index(row, column).data().toString();
            qWarning().noquote() << line.join(" | ");
        }
    }
    QVERIFY2(saved.value("ok").toBool(), qPrintable(diagnostics(saved)));
    QCOMPARE(saveCalls, 1); QCOMPARE(delegatedPath, candidate); QVERIFY(!delegatedInPlace);
    QVERIFY(!window.project_->savingMap_ && !window.project_->triggerDirty_);
    QCOMPARE(window.project_->sourceArchivePath_, candidate);
    QCOMPARE(window.project_->luaMapTargetSource_, candidate);
    QVERIFY(ModelContains(*window.project_->diagnosticsModel_, QStringLiteral("SCRIPT_REBUILT"), 2));
    QVERIFY2(ModelContains(*window.project_->diagnosticsModel_, QStringLiteral("MAP_SAVE_COMPLETED"), 2),
        qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
    int changeEvents = 0;
    for (const auto& event : activity) if (event[0].toJsonObject().value("type") == QStringLiteral("changes")) ++changeEvents;
    QCOMPARE(changeEvents, 1);
    QCOMPARE(saved.value("changes").toObject().value("files").toInt(), 1);

    const auto reopened = War3Map::MapArchiveSession::Open(candidate);
    QVERIFY2(reopened.Succeeded(), qPrintable(LuaQtDiagnostics(reopened)));
    QCOMPARE(saved.value("sha256").toString(), QString::fromStdString(reopened.value->SourceSha256()));
    const auto wtg = reopened.value->ReadMember("war3map.wtg"), wct = reopened.value->ReadMember("war3map.wct");
    QVERIFY(wtg.Succeeded() && wct.Succeeded());
    QCOMPARE(*wtg.value, after.value->wtg); QCOMPARE(*wct.value, baseline.value->wct);
    const auto gui = War3Map::Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *window.project_->uiCatalog_);
    QVERIFY2(gui.Succeeded(), qPrintable(LuaQtDiagnostics(gui)));
    QCOMPARE(gui.value->triggers.size(), std::size_t{1});
    QCOMPARE(gui.value->triggers[0].name.text, name.toStdString());
    QCOMPARE(gui.value->triggers[0].customTextFlag, std::int32_t{0});
    QVERIFY(gui.value->triggers[0].customText.text.empty() && gui.value->wct.customCode.text.empty());
    QCOMPARE(gui.value->ecaNodes[gui.value->triggers[0].ecas[0]].name.text, std::string("DoNothing"));
    const std::array<std::string, 4> scriptNames{"war3map.lua", "scripts\\war3map.lua", "war3map.j", "scripts\\war3map.j"};
    const auto scripts = reopened.value->CaptureExactMembers(scriptNames);
    QVERIFY(scripts.Succeeded());
    QVERIFY((*scripts.value)[0].present && !(*scripts.value)[1].present && !(*scripts.value)[2].present && !(*scripts.value)[3].present);
    const auto& scriptBytes = (*scripts.value)[0].bytes;
    const std::string_view script(reinterpret_cast<const char*>(scriptBytes.data()), scriptBytes.size());
    QVERIFY(script.find("function config()") != script.npos && script.find("function main()") != script.npos);
    QVERIFY(script.find(luaVariable + " = nil") != script.npos);
    QVERIFY(script.find("function " + luaConditions + "()") != script.npos);
    const auto actionStart = script.find("function " + luaActions + "()");
    const auto initializerStart = script.find("function " + luaInitializer + "()");
    const auto customStart = script.find("function InitCustomTriggers()");
    const auto runStart = script.find("function RunInitializationTriggers()");
    QVERIFY(actionStart != script.npos && initializerStart != script.npos && customStart != script.npos && runStart != script.npos);
    QVERIFY(actionStart < initializerStart && initializerStart < customStart && customStart < runStart);
    // 检查本 T 动作体、native 挂接与初始化调用链；全局任意位置出现一个名称不能代替这些证明。
    const auto actions = script.substr(actionStart, initializerStart - actionStart);
    QVERIFY(actions.find("DoNothing()") != actions.npos);
    const auto initializer = script.substr(initializerStart, customStart - initializerStart);
    QVERIFY(initializer.find("CreateTrigger()") != initializer.npos && initializer.find(luaVariable) != initializer.npos);
    QVERIFY(initializer.find("TriggerAddCondition(") != initializer.npos && initializer.find(luaConditions) != initializer.npos);
    QVERIFY(initializer.find("TriggerAddAction(") != initializer.npos && initializer.find(luaActions) != initializer.npos);
    QVERIFY(script.substr(customStart, runStart - customStart).find(luaInitializer + "()") != script.npos);
    const auto nextFunction = script.find("\nfunction ", runStart + 1);
    const auto run = script.substr(runStart, nextFunction == script.npos ? script.npos : nextFunction - runStart);
    QVERIFY(run.find("ConditionalTriggerExecute(") != run.npos && run.find(luaVariable) != run.npos);
    const auto mainStart = script.find("function main()");
    const auto nextMainFunction = script.find("\nfunction ", mainStart + 1);
    const auto main = script.substr(mainStart, nextMainFunction == script.npos ? script.npos : nextMainFunction - mainStart);
    QVERIFY(main.find("InitCustomTriggers()") != main.npos && main.find("RunInitializationTriggers()") != main.npos);
    const auto settingsBytes = reopened.value->ReadMember("war3map.w3i"); QVERIFY(settingsBytes.Succeeded());
    const auto settings = War3Map::MapData::MapSettings::Parse(*settingsBytes.value); QVERIFY(settings.Succeeded());
    QCOMPARE(settings.value->scriptLanguage, std::int32_t{1});
    QCOMPARE(reopened.value->ReadMember("unmanaged.bin").value, std::optional(Bytes("original Qt fixture bytes")));
    const auto afterSave = tools.Invoke("editor_state", {});
    QVERIFY(afterSave.value("project_identity") != beforeSave.value("project_identity"));
    QVERIFY(!tools.Invoke("editor_query_trigger_graph", queryRequest).value("ok").toBool());
    QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
    qInfo().noquote() << "Live GUI T -> original save chain, actual Lua checker and archive readback: source="
        << QString::fromStdWString(source.wstring()) << "source SHA=" << QString::fromStdString(*sourceHash.value)
        << "candidate=" << QString::fromStdWString(candidate.wstring()) << "candidate SHA=" << saved.value("sha256").toString()
        << "profile/config=" << QString::fromStdWString(configuration.wstring())
        << "; original natural T retained; syntax/archive evidence only, no Warcraft gameplay claim";
}

void NativeWorkbenchTests::LuaMapTargetGuardsActiveSaveAndClearsOnMapSwitch() {
    const auto pattern = LuaQtEvidencePattern();
    QVERIFY(!pattern.isEmpty());
    QTemporaryDir fixture(pattern), classicFixture(pattern);
    QVERIFY(fixture.isValid() && classicFixture.isValid());
    fixture.setAutoRemove(false); classicFixture.setAutoRemove(false);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    std::filesystem::path configuration;
    QVERIFY(InstallLuaSaveFixture(window, fixture, 31, configuration));
    const auto source = window.project_->sourceArchivePath_;
    const auto originalHash = War3Map::Sha256File(source);
    QVERIFY(originalHash.Succeeded());
    QVERIFY(window.project_->ConfigureLuaMapTarget(configuration).Succeeded());
    const auto generation = window.project_->luaMapTargetGeneration_;
    QVERIFY(!window.project_->triggerDirty_); // 单选Lua目标也须真的重建，不要求制造无关编辑。
    const auto candidate = NativePath(fixture.filePath("unchanged-t-lua.w3x"));
    auto job = window.project_->PrepareMapSave(candidate, false);
    QVERIFY(job && job->script && job->script->luaTarget);
    bool observedProgress = false, targetWasGuarded = true;
    job->progress = [&](const QString&) {
        observedProgress = true;
        const auto configured = window.project_->ConfigureLuaMapTarget(configuration);
        const auto cleared = window.project_->ClearLuaMapTarget();
        const auto refused = [](const auto& result) {
            return !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "LUA_MAP_SAVE_ACTIVE"; });
        };
        targetWasGuarded &= refused(configured) && refused(cleared)
            && window.project_->luaMapTargetGeneration_ == generation;
    };
    // 与Qt同步保存驱动相同：工作期锁住工程，再由唯一提交点接受任务结果。
    window.project_->savingMap_ = true;
    War3Map::QtProject::EditorProject::RunMapSave(*job);
    window.project_->savingMap_ = false;
    QVERIFY(observedProgress && targetWasGuarded);
    QVERIFY2(job->succeeded && job->scriptRebuilt, qPrintable(LuaQtDiagnostics(*job)));
    const auto committed = window.project_->CommitMapSave(*job);
    ReportMapSaveFixture(QStringLiteral("after-lua-guarded-save"), fixture.path());
    QVERIFY2(committed && committed->scriptRebuilt, qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QCOMPARE(window.project_->luaMapTargetSource_, candidate);
    QCOMPARE(window.project_->luaMapTargetGeneration_, generation);
    QCOMPARE(War3Map::Sha256File(source).value, originalHash.value);
    qInfo().noquote() << "Lua Qt unchanged-T candidate=" << QString::fromStdWString(candidate.wstring())
        << "checker log=" << QString::fromStdWString((job->scriptWorkspace / "lua-check/lua-syntax.log").wstring());

    std::filesystem::path classicConfiguration;
    QVERIFY(InstallLuaSaveFixture(window, classicFixture, 25, classicConfiguration)); // 真实OpenMap触发Reset清绑定。
    QVERIFY(!window.project_->luaMapTarget_ && window.project_->luaMapTargetSource_.empty());
    QVERIFY(window.project_->luaMapTargetGeneration_ > generation);
    const auto classicSource = window.project_->sourceArchivePath_;
    const auto classicHash = War3Map::Sha256File(classicSource);
    const auto refused = window.project_->ConfigureLuaMapTarget(classicConfiguration);
    QVERIFY(!refused.Succeeded());
    QVERIFY(std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "LUA_MAP_TARGET_VERSION_UNSUPPORTED"; }));
    QVERIFY(!window.project_->luaMapTarget_ && window.project_->luaMapTargetSource_.empty());
    QCOMPARE(War3Map::Sha256File(classicSource).value, classicHash.value);
}

void NativeWorkbenchTests::LuaMapTargetOnlyInPlaceSaveWritesCopyAndBacksUp() {
    const auto pattern = LuaQtEvidencePattern();
    QVERIFY(!pattern.isEmpty());
    QTemporaryDir fixture(pattern);
    QVERIFY(fixture.isValid());
    fixture.setAutoRemove(false);
    MainWindow sourceWindow(MainWindow::StartupMode::HeadlessProbe);
    std::filesystem::path configuration;
    QVERIFY(InstallLuaSaveFixture(sourceWindow, fixture, 31, configuration));
    const auto source = sourceWindow.project_->sourceArchivePath_;
    const auto sourceHash = War3Map::Sha256File(source);
    QVERIFY(sourceHash.Succeeded());
    const auto originalWtg = sourceWindow.project_->archiveSession_->ReadMember("war3map.wtg");
    const auto originalWct = sourceWindow.project_->archiveSession_->ReadMember("war3map.wct");
    QVERIFY(originalWtg.Succeeded() && originalWct.Succeeded());
    const auto catalog = *sourceWindow.project_->uiCatalog_;
    const auto document = *sourceWindow.project_->triggerDocument_;
    const auto definitions = sourceWindow.project_->activeUiDataRoot_;
    const auto copy = NativePath(fixture.filePath("in-place-test-copy.w3x"));
    const ScopedMapBackupPreferences backupPreferences(fixture.filePath(QStringLiteral("War3MapEditor Backups")));
    QVERIFY(std::filesystem::copy_file(source, copy));
    const auto copyBefore = War3Map::Sha256File(copy);
    QCOMPARE(copyBefore.value, sourceHash.value);
    // 副本在另一个自有窗口首次打开；源窗口没有切图或保存操作，不触发弃改确认。
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    qInfo().noquote() << "Lua in-place entering OpenMapArchivePath(copy), fresh window";
    QVERIFY(window.OpenMapArchivePath(copy));
    qInfo().noquote() << "Lua in-place returned OpenMapArchivePath(copy)";
    window.project_->activeUiDataRoot_ = definitions;
    window.project_->compilerDataRoot_.clear();
    window.project_->war3GameRoot_.clear();
    window.project_->uiLibraryPackages_.clear();
    // 正常成对换入目录/文档并通知模块刷新。先证明仍是副本原字节，再建立打开时基线。
    window.project_->ReplaceTriggerWorkspace(std::make_unique<War3Map::Trigger::UiCatalog>(catalog),
        std::make_unique<War3Map::Trigger::TriggerDocument>(document));
    const auto baseline = War3Map::Trigger::WtgWctCodec{}.Serialize(
        *window.project_->triggerDocument_, *window.project_->uiCatalog_);
    QVERIFY(baseline.Succeeded());
    QCOMPARE(baseline.value->wtg, *originalWtg.value);
    QCOMPARE(baseline.value->wct, *originalWct.value);
    window.ResetEditTracking();
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    const auto configured = window.project_->ConfigureLuaMapTarget(configuration);
    QVERIFY2(configured.Succeeded(), qPrintable(LuaQtDiagnostics(configured)));
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    QCOMPARE(window.project_->luaMapTargetSource_, copy);

    qInfo().noquote() << "Lua in-place entering SaveMapInPlace";
    window.SaveMapInPlace(); // 真正Ctrl+S入口：自有Qt进度框、后台工作线程、自动备份和提交。
    qInfo().noquote() << "Lua in-place returned SaveMapInPlace";
    ReportMapSaveFixture(QStringLiteral("after-lua-in-place-save"), fixture.path());
    QVERIFY(!window.project_->savingMap_);
    QCOMPARE(window.project_->sourceArchivePath_, copy);
    QCOMPARE(window.project_->luaMapTargetSource_, copy);
    QVERIFY2(ModelContains(*window.project_->diagnosticsModel_, QStringLiteral("SCRIPT_REBUILT"), 2),
        qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    QVERIFY2(ModelContains(*window.project_->diagnosticsModel_, QStringLiteral("MAP_SAVE_COMPLETED"), 2),
        qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    const auto copyAfter = War3Map::Sha256File(copy);
    QVERIFY(copyAfter.Succeeded());
    QVERIFY(copyAfter.value != copyBefore.value);
    QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
    QCOMPARE(window.project_->archiveSession_->SourceSha256(), *copyAfter.value);
    QVERIFY(window.project_->archiveSession_->VerifySourceUnchanged().Succeeded());

    const auto reopened = War3Map::MapArchiveSession::Open(copy);
    QVERIFY(reopened.Succeeded());
    const std::array<std::string, 4> scriptNames{"war3map.lua", "scripts\\war3map.lua", "war3map.j", "scripts\\war3map.j"};
    const auto scripts = reopened.value->CaptureExactMembers(scriptNames);
    QVERIFY(scripts.Succeeded());
    QVERIFY((*scripts.value)[0].present && !(*scripts.value)[1].present
        && !(*scripts.value)[2].present && !(*scripts.value)[3].present);
    const auto settingsBytes = reopened.value->ReadMember("war3map.w3i");
    QVERIFY(settingsBytes.Succeeded());
    const auto settings = War3Map::MapData::MapSettings::Parse(*settingsBytes.value);
    QVERIFY(settings.Succeeded());
    QCOMPARE(settings.value->SourceFormatVersion(), std::optional<std::int32_t>{31});
    QCOMPARE(settings.value->scriptLanguage, std::int32_t{1});
    const auto wtg = reopened.value->ReadMember("war3map.wtg"), wct = reopened.value->ReadMember("war3map.wct");
    QCOMPARE(wtg.value, originalWtg.value);
    QCOMPARE(wct.value, originalWct.value);
    const auto gui = War3Map::Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, catalog);
    QVERIFY(gui.Succeeded());
    QCOMPARE(gui.value->triggers.size(), std::size_t{1});
    QCOMPARE(gui.value->triggers[0].customTextFlag, std::int32_t{0});
    QVERIFY(gui.value->triggers[0].customText.text.empty() && gui.value->wct.customCode.text.empty());
    QCOMPARE(gui.value->ecaNodes[gui.value->triggers[0].ecas[0]].name.text, std::string("DoNothing"));

    const auto backupDirectory = copy.parent_path() / "War3MapEditor Backups";
    QVERIFY(std::filesystem::is_directory(backupDirectory));
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(backupDirectory))
        if (entry.is_regular_file()) backups.push_back(entry.path());
    QCOMPARE(backups.size(), std::size_t{1});
    QCOMPARE(War3Map::Sha256File(backups[0]).value, copyBefore.value);
    QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
    qInfo().noquote() << "Lua Qt actual SaveMapInPlace copy=" << QString::fromStdWString(copy.wstring())
        << "SHA=" << QString::fromStdString(*copyAfter.value)
        << "backup=" << QString::fromStdWString(backups[0].wstring())
        << "backup SHA=" << QString::fromStdString(*copyBefore.value)
        << "untouched original=" << QString::fromStdWString(source.wstring());
    if (auto* result = window.findChild<QMessageBox*>(QStringLiteral("mapSaveResult"))) result->close();
}

void NativeWorkbenchTests::BaselineNotificationDeletionStopsWorkspaceOpen_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("directory-first-baseline") << 0;
    QTest::newRow("archive-first-baseline") << 1;
    QTest::newRow("archive-second-baseline") << 2;
}

void NativeWorkbenchTests::BaselineNotificationDeletionStopsWorkspaceOpen() {
    QFETCH(int, kind);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto archive = NativePath(fixture.filePath("owned.w3x"));
    std::filesystem::path source;
    if (kind == 0) {
        QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
        source = NativePath(root + "/table/unit.ini");
    } else {
        const auto built = MakeArchive(archive, 420);
        QVERIFY(built.Succeeded());
        source = archive;
    }
    const auto original = ReadFile(source);
    QVERIFY(!original.isEmpty());
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    const QPointer<MainWindow> liveWindow(window.get());
    const QPointer<War3Map::QtProject::EditorProject> liveProject(window->project_.get());
    QObject receiver;
    int notifications = 0;
    const int deleteAt = kind == 2 ? 2 : 1;
    bool actualReadyBaseline = false;
    connect(liveProject.data(), &War3Map::QtProject::EditorProject::EditTrackingBaselineChanged, &receiver,
        [&](quint64 generation, quint64 revision) {
            ++notifications;
            if (notifications != deleteAt) return;
            actualReadyBaseline = liveProject && liveProject->editTrackingReady_ && !liveProject->editTrackingFailed_
                && generation == liveProject->editRevision_.Generation() && revision == liveProject->editRevision_.Revision();
            // 真正销毁通知中的主窗口和工程，不用空指针或模拟信号代替实际 Reset 调用链。
            window.reset();
        }, Qt::DirectConnection);
    const bool opened = kind == 0 ? window->OpenWorkspacePath(NativePath(root)) : window->OpenMapArchivePath(archive);
    QVERIFY(!opened);
    QCOMPARE(notifications, deleteAt);
    QVERIFY(actualReadyBaseline);
    QVERIFY(!window && !liveWindow && !liveProject);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::WatchNotificationDeletionStopsWorkspaceOpen_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("directory-first-watch") << 0;
    QTest::newRow("archive-first-watch") << 1;
    QTest::newRow("archive-second-watch") << 2;
}

void NativeWorkbenchTests::EarlyWatchNotificationDeletionStopsWorkspaceOpen() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
    const auto source = NativePath(root + "/table/unit.ini");
    const auto original = ReadFile(source);
    QVERIFY(!original.isEmpty());
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    const QPointer<MainWindow> liveWindow(window.get());
    const QPointer<War3Map::QtProject::EditorProject> liveProject(window->project_.get());
    const QPointer<QFileSystemWatcher> liveWatcher(liveProject->projectWatch_);
    QObject receiver;
    int notifications = 0;
    bool actualClearWatch = false;
    connect(liveProject.data(), &War3Map::QtProject::EditorProject::SourceStateChanged, &receiver, [&] {
        ++notifications;
        // 首个生产 ResetProjectWatch 清空通知；不筛掉未配置来源的早期边界。
        actualClearWatch = liveProject && liveWatcher && liveProject->projectRoot_.empty()
            && !liveProject->directorySnapshot_ && liveWatcher->files().isEmpty() && liveWatcher->directories().isEmpty();
        window.reset();
    }, Qt::DirectConnection);
    const bool opened = window->OpenWorkspacePath(NativePath(root));
    QVERIFY(!opened);
    QVERIFY(actualClearWatch);
    QCOMPARE(notifications, 1);
    QVERIFY(!window && !liveWindow && !liveProject && !liveWatcher);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::TriggerUndoObserverRejectsReplacedPublicStack_data() {
    QTest::addColumn<bool>("foreign");
    QTest::newRow("null-public-stack") << false;
    QTest::newRow("foreign-public-stack") << true;
}

void NativeWorkbenchTests::TriggerUndoObserverRejectsReplacedPublicStack() {
    QFETCH(bool, foreign);
    namespace Trigger = War3Map::Trigger;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto definitions = fixture.filePath("definitions");
    const auto root = fixture.filePath("workspace");
    QVERIFY(WriteText(definitions + "/ui/config", "base\n"));
    QVERIFY(WriteText(definitions + "/ui/base/define.txt", "[TriggerTypes]\nboolean=0,1,1,Boolean,boolean\n"));
    QVERIFY(WriteText(definitions + "/ui/base/action.txt",
        "[SetFlag]\ntitle=\"Set flag\"\ndescription=\"Set ${flag}\"\n[[.args]]\ntype=boolean\ndefault=\"true\"\n"));
    auto catalog = Trigger::UiCatalog::Load(NativePath(definitions));
    QVERIFY(catalog.Succeeded() && catalog.value);
    // 复用 ChangesPanelObjectsAndTriggerUseSharedInverse 的公开 CreateEmpty/AppendRootEca 夹具路线。
    auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    QVERIFY(empty.Succeeded() && empty.value);
    Trigger::CategoryRecord category; category.id = 100; category.categoryId = 1; category.name.text = "Content";
    empty.value->categories.push_back(category);
    const auto triggerId = Trigger::TriggerDocumentEditor::AddTrigger(*empty.value, 1, "Flag");
    QVERIFY(triggerId.Succeeded() && triggerId.value);
    const auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(*empty.value, *catalog.value,
        *triggerId.value, Trigger::UiDefinitionKind::Action, "SetFlag");
    QVERIFY(appended.Succeeded());
    QVERIFY(!empty.value->argumentNodes.empty());
    QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*empty.value, empty.value->argumentNodes.front().id, 3, "true").Succeeded());
    const auto encoded = Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value);
    QVERIFY(encoded.Succeeded() && encoded.value);
    auto parsed = Trigger::WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *catalog.value);
    QVERIFY(parsed.Succeeded() && parsed.value);
    const auto sourceWtg = NativePath(root + "/map/war3map.wtg");
    const auto sourceWct = NativePath(root + "/map/war3map.wct");
    QVERIFY(WriteText(root + "/map/war3map.wtg", QByteArray(reinterpret_cast<const char*>(encoded.value->wtg.data()), qsizetype(encoded.value->wtg.size()))));
    QVERIFY(WriteText(root + "/map/war3map.wct", QByteArray(reinterpret_cast<const char*>(encoded.value->wct.data()), qsizetype(encoded.value->wct.size()))));
    QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
    const auto originalWtg = ReadFile(sourceWtg), originalWct = ReadFile(sourceWct);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    window.project_->activeUiDataRoot_ = NativePath(definitions);
    window.project_->ReplaceTriggerWorkspace(std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value)),
        std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value)));
    window.ResetEditTracking();
    QVERIFY(window.project_->editTrackingReady_ && !window.project_->editTrackingFailed_);
    auto* const actualStack = window.project_->triggerUndoStack_;
    QVERIFY(actualStack);
    QUndoStack foreignStack;
    const QPointer<War3Map::QtProject::EditorProject> liveProject(window.project_.get());
    const auto restoreStack = qScopeGuard([&] { if (liveProject) liveProject->triggerUndoStack_ = actualStack; });
    QObject receiver; // 先断开回调，再由 guard 恢复公开栈；不删除 Qt 正在 push 的 actualStack。
    int notifications = 0;
    const auto connection = connect(liveProject.data(), &War3Map::QtProject::EditorProject::TriggerDocumentChanged, &receiver, [&] {
        ++notifications;
        liveProject->triggerUndoStack_ = foreign ? &foreignStack : nullptr;
    }, Qt::DirectConnection);
    const auto title = window.windowTitle();
    const auto status = window.statusBar()->currentMessage();
    const auto binding = Trigger::CaptureTriggerGraphBinding(*liveProject->triggerDocument_, *liveProject->uiCatalog_);
    QVERIFY(binding.Succeeded() && binding.value);
    const auto& argument = liveProject->triggerDocument_->argumentNodes.front();
    Trigger::TriggerGraphEditBatch batch; batch.expected = *binding.value;
    batch.operations.emplace_back(Trigger::GraphSet{{Trigger::GraphRecordKind::Argument, argument.id, {}},
        Trigger::GraphField::ArgumentScalar, Trigger::GraphScalarValue{argument.type, argument.value.text}, Trigger::GraphScalarValue{3, "false"}});
    auto receipt = Trigger::PrepareTriggerGraphScalarEditBatch(*liveProject->triggerDocument_, *liveProject->uiCatalog_, batch);
    QVERIFY(receipt.Succeeded() && receipt.value);
    auto command = std::make_unique<War3Map::QtProject::TriggerGraphSnapshotUndoCommand>(*liveProject, std::move(*receipt.value), QStringLiteral("Actual scalar observer binding"));
    QVERIFY(command->Ready());
    const auto published = command->FirstPublication();
    actualStack->push(command.release());
    QCOMPARE(notifications, 1);
    QVERIFY(*published);
    QCOMPARE(actualStack->count(), 1);
    QCOMPARE(foreignStack.count(), 0);
    QVERIFY(liveProject->editTrackingReady_ && !liveProject->editTrackingFailed_);
    QCOMPARE(liveProject->triggerDocument_->argumentNodes.front().value.text, std::string("false"));
    QCOMPARE(window.windowTitle(), title);
    QCOMPARE(window.statusBar()->currentMessage(), status);
    disconnect(connection);
    liveProject->triggerUndoStack_ = actualStack;
    actualStack->undo();
    const auto restored = Trigger::WtgWctCodec{}.Serialize(*liveProject->triggerDocument_, *liveProject->uiCatalog_);
    QVERIFY(restored.Succeeded() && restored.value);
    QVERIFY(restored.value->wtg == encoded.value->wtg && restored.value->wct == encoded.value->wct);
    QCOMPARE(ReadFile(sourceWtg), originalWtg);
    QCOMPARE(ReadFile(sourceWct), originalWct);
}

void NativeWorkbenchTests::WatchNotificationDeletionStopsWorkspaceOpen() {
    QFETCH(int, kind);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto archive = NativePath(fixture.filePath("owned.w3x"));
    std::filesystem::path source;
    if (kind == 0) {
        QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
        source = NativePath(root + "/table/unit.ini");
    } else {
        const auto built = MakeArchive(archive, 420);
        QVERIFY(built.Succeeded());
        source = archive;
    }
    const auto original = ReadFile(source);
    QVERIFY(!original.isEmpty());
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    const QPointer<MainWindow> liveWindow(window.get());
    const QPointer<War3Map::QtProject::EditorProject> liveProject(window->project_.get());
    const QPointer<QFileSystemWatcher> liveWatcher(liveProject->projectWatch_);
    QVERIFY(liveWatcher);
    const QString watchedSource = QDir::cleanPath(QFileInfo(kind == 0 ? root
        : QString::fromStdWString(archive.wstring())).absoluteFilePath());
    QObject receiver;
    int watchNotifications = 0;
    int baselineNotifications = 0;
    int baselinesAtDeletion = -1;
    const int deleteAt = kind == 2 ? 2 : 1;
    bool actualConfiguredWatch = false;
    connect(liveProject.data(), &War3Map::QtProject::EditorProject::EditTrackingBaselineChanged, &receiver,
        [&](quint64, quint64) { ++baselineNotifications; }, Qt::DirectConnection);
    connect(liveProject.data(), &War3Map::QtProject::EditorProject::SourceStateChanged, &receiver, [&] {
        // ResetProjectWatch 的更早清空通知不属于本负例；只接实际来源已配置的 Refresh 尾通知。
        if (!liveProject || !liveWatcher || !liveProject->directorySnapshot_ || liveProject->projectRoot_.empty()) return;
        const bool configured = kind == 0 ? liveWatcher->directories().contains(watchedSource)
            : liveWatcher->files().contains(watchedSource);
        if (!configured) return;
        ++watchNotifications;
        if (watchNotifications != deleteAt) return;
        actualConfiguredWatch = true;
        baselinesAtDeletion = baselineNotifications;
        // 真实删除 Main -> Project -> watcher，覆盖 Refresh 局部 blocker 的析构边界。
        window.reset();
    }, Qt::DirectConnection);
    const bool opened = kind == 0 ? window->OpenWorkspacePath(NativePath(root)) : window->OpenMapArchivePath(archive);
    QVERIFY(!opened);
    QVERIFY(actualConfiguredWatch);
    QCOMPARE(watchNotifications, deleteAt);
    QCOMPARE(baselinesAtDeletion, deleteAt - 1);
    QCOMPARE(baselineNotifications, baselinesAtDeletion); // 删除通知之后不得继续 Reset。
    QVERIFY(!window && !liveWindow && !liveProject && !liveWatcher);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::WatchNotificationOwnerReplacementStopsWorkspaceOpen() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
    const auto source = NativePath(root + "/table/unit.ini");
    const auto original = ReadFile(source);
    QVERIFY(!original.isEmpty());
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    const QPointer<MainWindow> liveWindow(window.get());
    const QPointer<War3Map::QtProject::EditorProject> originalProject(window->project_.get());
    const QPointer<QFileSystemWatcher> originalWatcher(originalProject->projectWatch_);
    QVERIFY(originalWatcher);
    std::unique_ptr<War3Map::QtProject::EditorProject> heldOriginal;
    // Module 仍借用旧 Project；任何断言提前返回都先恢复 owner，再析构窗口。
    const auto restoreOwner = qScopeGuard([&] {
        if (window && heldOriginal) window->project_ = std::move(heldOriginal);
    });
    QPointer<War3Map::QtProject::EditorProject> replacementProject;
    const QString watchedSource = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    QObject receiver;
    int watchNotifications = 0;
    int baselineNotifications = 0;
    std::uint64_t originalGeneration = 0;
    std::uint64_t replacementGeneration = 0;
    connect(originalProject.data(), &War3Map::QtProject::EditorProject::EditTrackingBaselineChanged, &receiver,
        [&](quint64, quint64) { ++baselineNotifications; }, Qt::DirectConnection);
    connect(originalProject.data(), &War3Map::QtProject::EditorProject::SourceStateChanged, &receiver, [&] {
        if (heldOriginal || !window || !originalProject || !originalWatcher || !originalProject->directorySnapshot_
            || originalProject->projectRoot_.empty() || !originalWatcher->directories().contains(watchedSource)) return;
        ++watchNotifications;
        originalGeneration = originalProject->editRevision_.Generation();
        auto replacement = std::make_unique<War3Map::QtProject::EditorProject>();
        replacementProject = replacement.get();
        replacementGeneration = replacement->editRevision_.Generation();
        connect(replacement.get(), &War3Map::QtProject::EditorProject::EditTrackingBaselineChanged, &receiver,
            [&](quint64, quint64) { ++baselineNotifications; }, Qt::DirectConnection);
        heldOriginal = std::move(window->project_);
        window->project_ = std::move(replacement);
    }, Qt::DirectConnection);
    const bool opened = window->OpenWorkspacePath(NativePath(root));
    QVERIFY(!opened);
    QCOMPARE(watchNotifications, 1);
    QVERIFY(liveWindow && originalProject && originalWatcher && replacementProject);
    QVERIFY(heldOriginal.get() == originalProject.data());
    QVERIFY(window->project_.get() == replacementProject.data());
    QCOMPARE(baselineNotifications, 0);
    QCOMPARE(originalProject->editRevision_.Generation(), originalGeneration);
    QCOMPARE(replacementProject->editRevision_.Generation(), replacementGeneration);
    QVERIFY(!replacementProject->editTrackingReady_);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::DirectoryProjectAndInvalidOpenPreserveEdits() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    const auto project = NativePath(fixture.filePath("directory.forge"));
    QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    QVERIFY(ChangeHealth(window, 812));
    auto* current = window.project_->objectDocuments_[0].get();
    QVERIFY(!window.OpenWorkspacePath(NativePath(fixture.filePath("missing"))));
    QCOMPARE(window.project_->objectDocuments_[0].get(), current);
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(QFile::remove(root + "/table/unit.ini"));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 812);
    QVERIFY(!window.SaveEditorProjectToPath(NativePath(fixture.filePath("changed.forge"))));
}

void NativeWorkbenchTests::CodeDraftDirectoryCtrlSSavesProject() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("workspace");
    QVERIFY(WriteText(root + "/table/unit.ini", "[hfoo]\n_parent=\"hfoo\"\nuhpm=420\n"));
    QVERIFY(WriteText(root + "/ui/config", "base\n"));
    QVERIFY(WriteText(root + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(root));
    QVERIFY(catalog.Succeeded());
    auto initial = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    QVERIFY(initial.Succeeded());
    initial.value->wct.customCode.text = "// baseline\r\n";
    auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*initial.value, *catalog.value);
    QVERIFY(encoded.Succeeded());
    const auto array = [](const auto& bytes) { return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())); };
    QVERIFY(WriteText(root + "/map/war3map.wtg", array(encoded.value->wtg)));
    QVERIFY(WriteText(root + "/map/war3map.wct", array(encoded.value->wct)));
    const auto original = ReadFile(NativePath(root + "/map/war3map.wct"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    QVERIFY(window.project_->triggerDocument_);
    QVERIFY(window.triggersModule_->CreateCustomTrigger(QStringLiteral("HostBase")));
    const auto projectPath = NativePath(fixture.filePath("active.forge"));
    QVERIFY(window.SaveEditorProjectToPath(projectPath));
    window.ActivateModule("triggers");
    window.resize(1100, 800);
    window.show();
    QVERIFY(window.triggersModule_->ShowGlobalCustomCode());
    auto* editor = window.triggersModule_->CodeEditor();
    auto* codeView = qobject_cast<War3Map::UiKit::CodeView*>(editor); QVERIFY(codeView);
    QTRY_VERIFY_WITH_TIMEOUT(!codeView->IsLoading(), 5000);
    editor->setFocus();
    QCoreApplication::processEvents();
    auto* draftNote = window.triggersModule_->Widget()->findChild<QLabel*>("triggerCodeDraftNote");
    QVERIFY2(!editor->isReadOnly(), draftNote ? qPrintable(draftNote->text()) : "Host code view is readonly after own project save");
    const int graphIndex = window.project_->triggerUndoStack_->index();
    QVERIFY(graphIndex > 0 && window.UndoAction()->isEnabled());
    const auto baselineText = editor->toPlainText();
    QSignalSpy textChanges(editor, &QPlainTextEdit::textChanged);
    QTest::keyClicks(editor, "// local typing");
    QVERIFY(textChanges.size() > 0); QVERIFY(editor->toPlainText() != baselineText);
    QVERIFY(window.triggersModule_->HasPendingCodeDraft());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), baselineText);
    QCOMPARE(window.project_->triggerUndoStack_->index(), graphIndex);
    QVERIFY(!window.triggersModule_->HasPendingCodeDraft());
    window.triggersModule_->CodeEditor()->setPlainText(QStringLiteral("// directory author draft\n"));
    QVERIFY(window.triggersModule_->HasPendingCodeDraft());
    window.SaveMapInPlace(); // 实际 Ctrl+S，目录项目应保存工程，不能误走地图保存。
    QVERIFY(!window.triggersModule_->HasPendingCodeDraft());
    QVERIFY(!window.project_->archiveSession_);
    QCOMPARE(window.project_->triggerDocument_->wct.customCode.text, std::string("// directory author draft\r\n"));
    QCOMPARE(ReadFile(NativePath(root + "/map/war3map.wct")), original);
    auto saved = War3Map::MapArchiveSession::Open(projectPath);
    QVERIFY(saved.Succeeded());
    const auto savedWct = saved.value->ReadMember("overlay/map/war3map.wct");
    const auto savedWtg = saved.value->ReadMember("overlay/map/war3map.wtg");
    QVERIFY(savedWct.Succeeded() && savedWtg.Succeeded());
    auto reopened = War3Map::Trigger::WtgWctCodec{}.Parse(*savedWtg.value, *savedWct.value, *window.project_->uiCatalog_);
    QVERIFY(reopened.Succeeded());
    QCOMPARE(reopened.value->wct.customCode.text, window.project_->triggerDocument_->wct.customCode.text);
}

void NativeWorkbenchTests::GuiConversionSnapshotSurvivesProjectReopen() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("snapshot-workspace");
    QVERIFY(WriteText(root + "/ui/config", "base\n"));
    QVERIFY(WriteText(root + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(root)); QVERIFY(catalog.Succeeded());
    auto document = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); QVERIFY(document.Succeeded());
    const auto category = War3Map::Trigger::TriggerDirectoryEditor::AddCategory(*document.value, "Synthetic"); QVERIFY(category.Succeeded());
    auto plan = War3Map::Trigger::TriggerCodeEditor::PrepareNewCustom(*document.value, *catalog.value,
        document.value->categories.front().categoryId, "SnapshotExample"); QVERIFY(plan.Succeeded());
    QVERIFY(War3Map::Trigger::TriggerCodeEditor::Apply(*document.value, *catalog.value, *plan.value).Succeeded());
    War3Map::Trigger::GuiTriggerFragment fragment;
    War3Map::Trigger::EcaNode disabled; disabled.type = 2; disabled.name.text = "DoNothing"; disabled.enabled = 0;
    fragment.ecas.push_back(disabled); fragment.roots.push_back(0);
    const auto id = plan.value->Target().triggerId;
    auto gui = War3Map::Trigger::TriggerCodeEditor::PrepareRecovery(*document.value, *catalog.value, id, fragment); QVERIFY(gui.Succeeded());
    QVERIFY(War3Map::Trigger::TriggerCodeEditor::Apply(*document.value, *catalog.value, *gui.value).Succeeded());
    const auto original = War3Map::Trigger::WtgWctCodec{}.Serialize(*document.value, *catalog.value); QVERIFY(original.Succeeded());
    const auto bytes = [](const auto& data) { return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())); };
    QVERIFY(WriteText(root + "/map/war3map.wtg", bytes(original.value->wtg)));
    QVERIFY(WriteText(root + "/map/war3map.wct", bytes(original.value->wct)));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    window.triggersModule_->SetConfirmHandler([](const auto&, const auto&, const auto&) { return true; });
    QVERIFY(window.triggersModule_->Reveal({"triggers", "trigger", "SnapshotExample", {}}));
    const auto initialFingerprint = window.project_->ProjectFingerprint(); QVERIFY(!initialFingerprint.empty());
    QVERIFY(window.triggersModule_->ConvertCurrentTriggerToCustomCode());
    const auto codeFingerprint = window.project_->ProjectFingerprint(); QVERIFY(codeFingerprint != initialFingerprint);
    window.project_->triggerUndoStack_->undo();
    QVERIFY(window.project_->guiConversionSnapshots_.Empty());
    QCOMPARE(window.project_->ProjectFingerprint(), initialFingerprint);
    window.project_->triggerUndoStack_->redo();
    QCOMPARE(window.project_->ProjectFingerprint(), codeFingerprint);
    const auto codeWire = War3Map::Trigger::WtgWctCodec{}.Serialize(*window.project_->triggerDocument_, *window.project_->uiCatalog_);
    QVERIFY(codeWire.Succeeded());
    const auto projectPath = NativePath(fixture.filePath("snapshot.forge"));
    QVERIFY(window.SaveEditorProjectToPath(projectPath));
    auto archive = War3Map::MapArchiveSession::Open(projectPath); QVERIFY(archive.Succeeded());
    QVERIFY(archive.value->HasMember("overlay/editor/gui-conversion-snapshots.bin"));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(projectPath));
    QVERIFY(!restored.project_->guiConversionSnapshots_.Empty());
    restored.triggersModule_->SetConfirmHandler([](const auto&, const auto&, const auto&) { return true; });
    QVERIFY(restored.triggersModule_->Reveal({"triggers", "trigger", "SnapshotExample", {}}));
    QVERIFY(restored.triggersModule_->ConvertCurrentCodeToTrigger());
    const auto recovered = War3Map::Trigger::WtgWctCodec{}.Serialize(*restored.project_->triggerDocument_, *restored.project_->uiCatalog_);
    QVERIFY(recovered.Succeeded());
    QCOMPARE(recovered.value->wtg, original.value->wtg); QCOMPARE(recovered.value->wct, original.value->wct);
    restored.project_->triggerUndoStack_->undo();
    QVERIFY(restored.triggersModule_->ConvertCurrentCodeToTrigger()); // 恢复后撤销不丢快照。
    QCOMPARE(ReadFile(NativePath(root + "/map/war3map.wtg")), bytes(original.value->wtg));
    QCOMPARE(ReadFile(NativePath(root + "/map/war3map.wct")), bytes(original.value->wct));
    // 损坏边车在切换会话前拒绝，原来已经恢复的编辑仍保留。
    std::vector<War3Map::MapArchiveMember> members(archive.value->Members().begin(), archive.value->Members().end());
    for (auto& member : members) if (member.name == "overlay/editor/gui-conversion-snapshots.bin") member.data.pop_back();
    const auto bad = NativePath(fixture.filePath("broken-snapshot.forge"));
    QVERIFY(War3Map::WriteVerifiedArchive(bad, members).Succeeded());
    auto* current = restored.project_->triggerDocument_.get();
    QVERIFY(!restored.OpenEditorProjectPath(bad)); QCOMPARE(restored.project_->triggerDocument_.get(), current);
    const auto missing = NativePath(fixture.filePath("absent/failed.forge"));
    QVERIFY(!restored.SaveEditorProjectToPath(missing));
    QVERIFY(!restored.project_->guiConversionSnapshots_.Empty());

    // 原创的代码态地图与工程快照。普通地图保存不携带快照；同一会话继续保存 .forge 仍需保留它。
    // 此处只改导入文本，走现有纯数据保存链，不以合成脚本冒充编译器或游戏验收。
    const auto mapSource = NativePath(fixture.filePath("snapshot-source.w3x"));
    const std::vector<War3Map::MapArchiveMember> mapMembers{
        {"war3map.wtg", codeWire.value->wtg}, {"war3map.wct", codeWire.value->wct},
        {"war3map.j", Bytes("// original synthetic script\n")}};
    QVERIFY(War3Map::WriteVerifiedArchive(mapSource, mapMembers).Succeeded());
    const auto sourceBytes = ReadFile(mapSource);
    MainWindow mapWindow(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(mapWindow.UseUiDataRoot(NativePath(root)));
    QVERIFY(mapWindow.OpenMapArchivePath(mapSource));
    QVERIFY(mapWindow.project_->triggerDocument_ && mapWindow.project_->uiCatalog_);
    mapWindow.project_->compilerDataRoot_ = NativePath(fixture.filePath("missing-toolchain"));
    const auto sourceIdentity = "archive:" + mapWindow.project_->archiveSession_->SourceSha256();
    const auto transport = window.project_->guiConversionSnapshots_.Serialize(*window.project_->triggerDocument_,
        *window.project_->uiCatalog_, sourceIdentity); QVERIFY(transport.Succeeded());
    auto mapSnapshots = War3Map::Trigger::GuiConversionSnapshots::Parse(*transport.value,
        *mapWindow.project_->triggerDocument_, *mapWindow.project_->uiCatalog_, sourceIdentity); QVERIFY(mapSnapshots.Succeeded());
    mapWindow.project_->guiConversionSnapshots_ = std::move(*mapSnapshots.value);
    const auto beforeMapProject = NativePath(fixture.filePath("before-map-save.forge"));
    QVERIFY(mapWindow.SaveEditorProjectToPath(beforeMapProject));
    const auto imported = fixture.filePath("snapshot-note.txt"); QVERIFY(WriteText(imported, "original imported note"));
    mapWindow.assetsModule_->AddFiles({imported});
    QVERIFY(mapWindow.project_->HasPendingAssetEdits());
    const auto mapCopy = NativePath(fixture.filePath("snapshot-saved.w3x"));
    QVERIFY2(mapWindow.SaveMapArchiveToPath(mapCopy), qPrintable(LastMapSaveDiagnostics(*mapWindow.project_->diagnosticsModel_)));
    QCOMPARE(ReadFile(mapSource), sourceBytes);
    const auto savedMap = War3Map::MapArchiveSession::Open(mapCopy); QVERIFY(savedMap.Succeeded());
    QVERIFY(savedMap.value->SourceSha256() != sourceIdentity.substr(std::string("archive:").size()));
    QVERIFY(!savedMap.value->HasMember("overlay/editor/gui-conversion-snapshots.bin"));
    const auto mapId = mapWindow.project_->triggerDocument_->triggers.front().id;
    QVERIFY(mapWindow.project_->guiConversionSnapshots_.Find(*mapWindow.project_->triggerDocument_, *mapWindow.project_->uiCatalog_, mapId));
    const auto resavedProject = NativePath(fixture.filePath("after-map-save.forge"));
    QVERIFY(mapWindow.SaveEditorProjectToPath(resavedProject));
    MainWindow mapRestored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(mapRestored.OpenEditorProjectPath(resavedProject));
    const auto savedCodeFingerprint = mapRestored.project_->ProjectFingerprint();
    mapRestored.triggersModule_->SetConfirmHandler([](const auto&, const auto&, const auto&) { return true; });
    QVERIFY(mapRestored.triggersModule_->Reveal({"triggers", "trigger", "SnapshotExample", {}}));
    QVERIFY(mapRestored.triggersModule_->ConvertCurrentCodeToTrigger());
    const auto mapRecovered = War3Map::Trigger::WtgWctCodec{}.Serialize(*mapRestored.project_->triggerDocument_, *mapRestored.project_->uiCatalog_);
    QVERIFY(mapRecovered.Succeeded());
    QCOMPARE(mapRecovered.value->wtg, original.value->wtg); QCOMPARE(mapRecovered.value->wct, original.value->wct);
    mapRestored.project_->triggerUndoStack_->undo();
    QCOMPARE(mapRestored.project_->ProjectFingerprint(), savedCodeFingerprint);
    mapRestored.project_->triggerUndoStack_->redo();
    QVERIFY(mapRestored.project_->guiConversionSnapshots_.ActiveFingerprint(
        *mapRestored.project_->triggerDocument_, *mapRestored.project_->uiCatalog_).empty());
}

void NativeWorkbenchTests::GuiConversionSnapshotDoesNotBindReusedTriggerIdentity() {
    using namespace War3Map;
    using namespace War3Map::Trigger;
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto root = fixture.filePath("identity-workspace");
    QVERIFY(WriteText(root + "/ui/config", "base\n"));
    QVERIFY(WriteText(root + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = UiCatalog::Load(NativePath(root)); QVERIFY(catalog.Succeeded());
    auto initial = WtgWctCodec{}.CreateEmpty(*catalog.value); QVERIFY(initial.Succeeded());
    QVERIFY(TriggerDirectoryEditor::AddCategory(*initial.value, "Synthetic").Succeeded());
    const auto initialWire = WtgWctCodec{}.Serialize(*initial.value, *catalog.value); QVERIFY(initialWire.Succeeded());
    const auto bytes = [](const auto& data) { return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())); };
    QVERIFY(WriteText(root + "/map/war3map.wtg", bytes(initialWire.value->wtg)));
    QVERIFY(WriteText(root + "/map/war3map.wct", bytes(initialWire.value->wct)));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(root)));
    window.triggersModule_->SetConfirmHandler([](const auto&, const auto&, const auto&) { return true; });
    // A 在会话中新建，使用可复用的生成域 ID；不能以源文件解析出的 ID 代替此反例。
    QVERIFY(window.triggersModule_->CreateCustomTrigger(QStringLiteral("ReusedSnapshot")));
    auto& document = *window.project_->triggerDocument_;
    const auto originalId = document.triggers.front().id;
    GuiTriggerFragment disabledGui;
    EcaNode disabled; disabled.type = 2; disabled.name.text = "DoNothing"; disabled.enabled = 0;
    disabledGui.ecas.push_back(disabled); disabledGui.roots.push_back(0);
    const auto seedGui = TriggerCodeEditor::PrepareRecovery(document, *window.project_->uiCatalog_, originalId, disabledGui);
    QVERIFY(seedGui.Succeeded()); QVERIFY(TriggerCodeEditor::Apply(document, *window.project_->uiCatalog_, *seedGui.value).Succeeded());
    window.project_->triggerUndoStack_->clear();
    window.project_->ObserveTriggerEdit(); window.triggersModule_->RefreshEditor();
    QVERIFY(window.triggersModule_->Reveal({"triggers", "trigger", "ReusedSnapshot", {}}));
    const auto originalGui = WtgWctCodec{}.Serialize(document, *window.project_->uiCatalog_); QVERIFY(originalGui.Succeeded());
    QVERIFY(window.triggersModule_->ConvertCurrentTriggerToCustomCode());
    const auto code = TriggerCodeEditor::NormalizedText(document.triggers.front().customText);
    QVERIFY(window.project_->guiConversionSnapshots_.Find(document, *window.project_->uiCatalog_, originalId));
    window.triggersModule_->DeleteCurrentItem();
    QVERIFY(document.triggers.empty()); QVERIFY(window.project_->guiConversionSnapshots_.Empty());
    QVERIFY(window.triggersModule_->CreateCustomTrigger(QStringLiteral("ReusedSnapshot")));
    QCOMPARE(document.triggers.front().id, originalId);
    window.triggersModule_->CodeEditor()->setPlainText(QString::fromStdString(code));
    QVERIFY(window.triggersModule_->CommitPendingEdits());
    QVERIFY(!window.project_->guiConversionSnapshots_.Find(document, *window.project_->uiCatalog_, originalId));
    QVERIFY(window.triggersModule_->ConvertCurrentCodeToTrigger());
    QVERIFY(document.triggers.front().ecas.empty()); // B 只能从代码解析，不能复活 A 的禁用动作。
    for (int step = 0; step < 4; ++step) window.project_->triggerUndoStack_->undo(); // 转回、贴正文、新建 B、删除 A。
    QCOMPARE(TriggerCodeEditor::NormalizedText(document.triggers.front().customText), code);
    const auto* restored = window.project_->guiConversionSnapshots_.Find(document, *window.project_->uiCatalog_, originalId);
    QVERIFY(restored && restored->ecas.size() == 1 && restored->ecas.front().enabled == 0);
    window.project_->triggerUndoStack_->redo(); // 重做删除 A，快照再次随命令移走。
    QVERIFY(document.triggers.empty()); QVERIFY(window.project_->guiConversionSnapshots_.Empty());
    window.project_->triggerUndoStack_->undo();
    QVERIFY(window.triggersModule_->Reveal({"triggers", "trigger", "ReusedSnapshot", {}}));
    QVERIFY(window.triggersModule_->ConvertCurrentCodeToTrigger());
    const auto restoredGui = WtgWctCodec{}.Serialize(document, *window.project_->uiCatalog_); QVERIFY(restoredGui.Succeeded());
    QCOMPARE(restoredGui.value->wtg, originalGui.value->wtg); QCOMPARE(restoredGui.value->wct, originalGui.value->wct);
    // 转回 GUI 后缓存仍存在；UI/MCP 共用的整图命令删除该记录时也必须暂存它。
    auto before = document, after = document;
    QVERIFY(TriggerDocumentEditor::RemoveTrigger(after, originalId).Succeeded());
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(*window.project_, std::move(before),
        std::move(after), QStringLiteral("Remove cached snapshot via common graph command"));
    QVERIFY(command->Ready()); window.project_->triggerUndoStack_->push(command.release());
    QVERIFY(document.triggers.empty()); QVERIFY(window.project_->guiConversionSnapshots_.Empty());
    window.project_->triggerUndoStack_->undo(); // 恢复 GUI 与缓存。
    window.project_->triggerUndoStack_->undo(); // 撤销此前转回，恢复 A 的代码态。
    restored = window.project_->guiConversionSnapshots_.Find(document, *window.project_->uiCatalog_, originalId);
    QVERIFY(restored && restored->ecas.size() == 1 && restored->ecas.front().enabled == 0);
}

void NativeWorkbenchTests::IncompleteLuaDraftSurvivesApplyHostRecoveryAndMapGate() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    QVERIFY(QDir{}.mkpath(fixture.filePath("source-workspace")));
    const auto uiRoot = fixture.filePath("original-ui");
    QVERIFY(WriteText(uiRoot + "/ui/config", "base\n"));
    QVERIFY(WriteText(uiRoot + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(uiRoot)); QVERIFY(catalog.Succeeded());
    auto initial = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); QVERIFY(initial.Succeeded());
    const auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*initial.value, *catalog.value); QVERIFY(encoded.Succeeded());
    const auto source = NativePath(fixture.filePath("source-workspace/source.w3x"));
    const QByteArray originalLua("-- original\r\nreturn 1\r\n");
    const auto originalSpan = std::as_bytes(std::span(originalLua.constData(), static_cast<std::size_t>(originalLua.size())));
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct},
        {"Scripts\\Draft.lua", {originalSpan.begin(), originalSpan.end()}}
    };
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSource = War3Map::Sha256File(source); QVERIFY(originalSource.Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(NativePath(uiRoot))); QVERIFY(window.OpenMapArchivePath(source));
    window.ActivateModule("triggers"); window.resize(1100, 800); window.show();
    auto* scripts = window.triggersModule_->LuaScripts(); QVERIFY(scripts);
    auto* tabs = window.triggersModule_->Widget()->findChild<QTabWidget*>("triggerWorkspaceTabs"); QVERIFY(tabs);
    tabs->setCurrentWidget(scripts); QVERIFY(scripts->SelectScript("Scripts\\Draft.lua"));
    const auto selectedGeneration = window.project_->editRevision_.Generation();
    const bool suspectedAtSelection = window.project_->sourceChangeSuspected_;
    auto* editor = scripts->Editor(); QVERIFY(editor);
    QCoreApplication::processEvents(); // 真实鼠标点击需要页内布局已经完成。
    qInfo() << "Lua host binding before/after events generation" << selectedGeneration << window.project_->editRevision_.Generation()
        << "suspected" << suspectedAtSelection << window.project_->sourceChangeSuspected_;
    editor->SetText(QStringLiteral("function unfinished(\n")); QVERIFY(scripts->HasPendingDraft());
    auto* apply = scripts->findChild<QPushButton*>("luaApplyDraft"); QVERIFY(apply && apply->isEnabled());
    QVERIFY(apply->isVisible() && apply->width() > 0 && apply->height() > 0);
    QSignalSpy clicked(apply, &QPushButton::clicked); QTest::mouseClick(apply, Qt::LeftButton);
    QCOMPARE(clicked.size(), 1); QVERIFY2(!scripts->HasPendingDraft(), qPrintable(scripts->StatusText()));
    const auto applied = War3Map::QtProject::ReadOwnedAsset(*window.project_, "Scripts\\Draft.lua"); QVERIFY(applied.Succeeded());
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(applied.value->bytes->data()), static_cast<qsizetype>(applied.value->bytes->size())),
        QByteArray("function unfinished(\r\n"));
    // 第二次尚未结束的草稿由宿主保存入口提交，不能只证明手动 Apply。
    editor->SetText(QStringLiteral("function unfinished(\n-- 保存时仍未完成\n")); QVERIFY(scripts->HasPendingDraft());
    const auto recovery = NativePath(fixture.filePath("incomplete-lua-recovery.forge"));
    QVERIFY(window.SaveEditorProjectToPath(recovery, true)); QVERIFY(!scripts->HasPendingDraft());
    const auto frozen = War3Map::QtProject::ReadOwnedAsset(*window.project_, "Scripts\\Draft.lua"); QVERIFY(frozen.Succeeded());
    const auto expected = QStringLiteral("function unfinished(\r\n-- 保存时仍未完成\r\n").toUtf8();
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(frozen.value->bytes->data()), static_cast<qsizetype>(frozen.value->bytes->size())), expected);
    QTRY_VERIFY_WITH_TIMEOUT(!editor->IsChecking() && editor->CheckResult().completed, 5000);
    QVERIFY(editor->CheckResult().syntaxChecked); QVERIFY(!editor->CheckResult().diagnostics.isEmpty());
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(recovery));
    QCOMPARE(restored.project_->pendingImports_.size(), std::size_t{1});
    const auto& payload = restored.project_->pendingImports_.front();
    QVERIFY(payload.ownedPayload && payload.runtimeLuaDraft && payload.diskPath.isEmpty());
    QCOMPARE(*payload.ownedPayload, *frozen.value->bytes); QCOMPARE(War3Map::Sha256(*payload.ownedPayload).value, std::optional<std::string>{frozen.value->sha256});
    QVERIFY(!std::filesystem::exists(restored.project_->projectRoot_ / "imports/0"));
    const auto forbidden = NativePath(fixture.filePath("forbidden-map.w3x"));
    QVERIFY(!restored.SaveMapArchiveToPath(forbidden)); QVERIFY(!std::filesystem::exists(forbidden));
    bool profileGate = false;
    for (int row = 0; row < restored.project_->diagnosticsModel_->rowCount(); ++row)
        profileGate |= restored.project_->diagnosticsModel_->index(row, 2).data().toString() == "MAP_RUNTIME_LUA_PROFILE_UNCONFIRMED";
    QVERIFY(profileGate); QVERIFY(!restored.project_->savingMap_);
    QCOMPARE(War3Map::Sha256File(source).value, originalSource.value);
}

void NativeWorkbenchTests::MapDataOnlyChangesProduceAutomaticRecovery() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    QVERIFY(QDir{}.mkpath(fixture.filePath("source-workspace")));
    const auto source = NativePath(fixture.filePath("source-workspace/map-data-source.w3x"));
    auto settings = War3Map::MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes); QVERIFY(settings.Succeeded());
    settings.value->mapName = "Recovery baseline";
    const auto encoded = settings.value->Serialize(); QVERIFY(encoded.Succeeded());
    const std::vector<War3Map::MapArchiveMember> members{{"war3map.w3i", *encoded.value}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe); QVERIFY(window.OpenMapArchivePath(source));
    const auto recovery = NativePath(fixture.filePath("map-data-recovery.forge"));
    window.project_->recoveryPath_ = recovery;
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    window.WriteRecovery(); QVERIFY(!std::filesystem::exists(recovery));
    QCOMPARE(QSettings{}.value("project/latestRecovery"), previous);
    auto* name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
    name->setText(QStringLiteral("仅地图数据也应恢复")); QVERIFY(window.settingsModule_->ApplyDraft());
    QVERIFY(window.project_->mapEdits_ && window.project_->mapEdits_->IsDirty());
    QVERIFY(!window.project_->triggerDirty_ && !window.project_->HasPendingAssetEdits());
    QVERIFY(std::none_of(window.project_->objectDirty_.begin(), window.project_->objectDirty_.end(), [](bool dirty) { return dirty; }));
    window.WriteRecovery(); QTRY_VERIFY_WITH_TIMEOUT(!window.writingRecovery_ && std::filesystem::exists(recovery), 10000);
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(recovery));
    QVERIFY(restored.project_->mapEdits_ && restored.project_->mapEdits_->Snapshot().Settings());
    QCOMPARE(restored.project_->mapEdits_->Snapshot().Settings()->mapName, QStringLiteral("仅地图数据也应恢复").toUtf8().toStdString());
    QVERIFY(restored.project_->mapEdits_->IsDirty()); QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::RecoveryPolicyCadenceDisableAndActualTimeoutKeepOriginalMap() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    QVERIFY(QDir{}.mkpath(fixture.filePath("source-workspace")));
    const auto uiRoot = fixture.filePath("original-ui");
    QVERIFY(WriteText(uiRoot + "/ui/config", "base\n"));
    QVERIFY(WriteText(uiRoot + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(uiRoot)); QVERIFY(catalog.Succeeded());
    auto initial = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); QVERIFY(initial.Succeeded());
    const auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*initial.value, *catalog.value); QVERIFY(encoded.Succeeded());
    const auto source = NativePath(fixture.filePath("source-workspace/recovery-policy.w3x"));
    auto settings = War3Map::MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes); QVERIFY(settings.Succeeded());
    // 通用二进制夹具含不可解码名称；恢复策略测试需要实际可编辑的 UTF-8 字段。
    settings.value->mapName = "Recovery policy baseline";
    const auto settingsBytes = settings.value->Serialize(); QVERIFY(settingsBytes.Succeeded());
    const std::string lua = "return 1\n";
    const auto luaBytes = std::as_bytes(std::span(lua.data(), lua.size()));
    const std::vector<War3Map::MapArchiveMember> members{{"war3map.w3i", *settingsBytes.value},
        {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct},
        {"Scripts\\Draft.lua", {luaBytes.begin(), luaBytes.end()}}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(NativePath(uiRoot))); QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.autoRecoveryEnabled_); QCOMPARE(window.project_->recoveryTimer_->interval(), 300000);
    QVERIFY(!window.project_->recoveryTimer_->isActive());
    // 只开启生产计时分支；不启动普通窗口的网络、Agent 或更新服务。
    window.startupMode_ = MainWindow::StartupMode::Normal;
    const auto recovery = NativePath(fixture.filePath("policy-recovery.forge"));
    window.project_->recoveryPath_ = recovery;
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    auto* name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
    name->setText(QStringLiteral("有界自动恢复")); QVERIFY(window.settingsModule_->ApplyDraft());
    QVERIFY(window.project_->mapEdits_ && window.project_->mapEdits_->IsDirty());
    QTRY_VERIFY_WITH_TIMEOUT(window.project_->recoveryTimer_->isActive(), 2000);
    const auto timerId = window.project_->recoveryTimer_->timerId();
    window.ScheduleRecovery(); window.ScheduleRecovery();
    QCOMPARE(window.project_->recoveryTimer_->timerId(), timerId);
    window.ApplyRecoveryPreferences({false, 2});
    QVERIFY(!window.project_->recoveryTimer_->isActive()); QVERIFY(!window.autoRecoveryEnabled_);
    QCOMPARE(window.project_->recoveryTimer_->interval(), 120000);
    window.ScheduleRecovery(); window.WriteRecovery();
    QVERIFY(!window.project_->recoveryTimer_->isActive()); QVERIFY(!std::filesystem::exists(recovery));
    QCOMPARE(QSettings{}.value("project/latestRecovery"), previous); QCOMPARE(ReadFile(source), original);
    window.ApplyRecoveryPreferences({true, 2});
    QVERIFY(window.project_->recoveryTimer_->isActive());
    window.project_->savingMap_ = true;
    window.WriteRecovery(); QVERIFY(!std::filesystem::exists(recovery));
    QVERIFY(window.project_->recoveryTimer_->isActive()); QCOMPARE(ReadFile(source), original);
    window.project_->savingMap_ = false;
    auto* scripts = window.triggersModule_->LuaScripts(); QVERIFY(scripts);
    QVERIFY(scripts->SelectScript("Scripts\\Draft.lua"));
    const QString draft = QStringLiteral("local incomplete =\n");
    scripts->Editor()->SetText(draft); QVERIFY(scripts->HasPendingDraft());
    // 短化测试计时，不等待五分钟；timeout 仍连接实际生产 WriteRecovery。
    window.project_->recoveryTimer_->stop(); window.project_->recoveryTimer_->setInterval(25);
    window.ScheduleRecovery(); QTRY_VERIFY_WITH_TIMEOUT(!window.writingRecovery_ && std::filesystem::exists(recovery), 10000);
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(recovery));
    QVERIFY(restored.project_->mapEdits_ && restored.project_->mapEdits_->Snapshot().Settings());
    QCOMPARE(restored.project_->mapEdits_->Snapshot().Settings()->mapName, QStringLiteral("有界自动恢复").toUtf8().toStdString());
    QVERIFY(!scripts->HasPendingDraft()); QVERIFY(restored.project_->HasPendingAssetEdits());
    const auto recovered = War3Map::QtProject::ReadOwnedAsset(*restored.project_, "Scripts\\Draft.lua"); QVERIFY(recovered.Succeeded());
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(recovered.value->bytes->data()),
        static_cast<qsizetype>(recovered.value->bytes->size())), draft.toUtf8());
    QCOMPARE(ReadFile(source), original); QVERIFY(!window.project_->recoveryTimer_->isActive());
    window.ApplyRecoveryPreferences({true, 0}); QCOMPARE(window.project_->recoveryTimer_->interval(), 300000);
    window.ApplyRecoveryPreferences({false, 5});
}

void NativeWorkbenchTests::LargeMapAutomaticRecoveryKeepsSourceAndMeasuresUi() {
    const auto sourceName = qEnvironmentVariable("FORGE_RECOVERY_REAL_MAP");
    const auto uiRoot = qEnvironmentVariable("FORGE_YDWE_ROOT");
    const auto receiptName = qEnvironmentVariable("FORGE_RECOVERY_REAL_RECEIPT");
    if (sourceName.isEmpty() || uiRoot.isEmpty() || receiptName.isEmpty())
        QSKIP("Set FORGE_RECOVERY_REAL_MAP, FORGE_YDWE_ROOT and FORGE_RECOVERY_REAL_RECEIPT for the large-map copy probe");
    QVERIFY(QFileInfo(sourceName).isAbsolute()); QVERIFY(QFileInfo(uiRoot).isAbsolute());
    QVERIFY(QFileInfo(receiptName).isAbsolute()); QVERIFY(!QFileInfo::exists(receiptName));
    const auto source = NativePath(sourceName);
    const auto sourceBytes = QFileInfo(sourceName).size();
    QVERIFY2(sourceBytes >= 300ll * 1024 * 1024, "This probe requires an actual large map, not a tiny synthetic substitute");
    const auto sourceHash = War3Map::Sha256File(source); QVERIFY(sourceHash.Succeeded());
    const auto expectedHash = qEnvironmentVariable("FORGE_RECOVERY_REAL_SHA256");
    if (!expectedHash.isEmpty()) QCOMPARE(QString::fromStdString(*sourceHash.value), expectedHash);
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto copyName = fixture.filePath("large-map-copy.w3x");
    QVERIFY(QFile::copy(sourceName, copyName));
    const auto copy = NativePath(copyName);
    const auto copyHash = War3Map::Sha256File(copy); QVERIFY(copyHash.Succeeded());
    QCOMPARE(*copyHash.value, *sourceHash.value);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(NativePath(uiRoot))); QVERIFY(window.OpenMapArchivePath(copy));
    const auto recovery = NativePath(fixture.filePath("large-map-recovery.forge"));
    window.project_->recoveryPath_ = recovery;
    window.startupMode_ = MainWindow::StartupMode::Normal;
    auto* name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
    const QString changedName = QStringLiteral("Audastra recovery benchmark");
    name->setText(changedName); QVERIFY(window.settingsModule_->ApplyDraft());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    window.project_->recoveryTimer_->stop(); window.project_->recoveryTimer_->setInterval(25);
    QElapsedTimer elapsed; elapsed.start();
    qint64 lastHeartbeat = 0, maxGap = 0, callbackCompleted = -1;
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, &window, [&] {
        const auto now = elapsed.elapsed(); maxGap = std::max(maxGap, now - lastHeartbeat);
        lastHeartbeat = now; ++heartbeatCount;
    });
    connect(window.project_->recoveryTimer_, &QTimer::timeout, &window, [&] {
        if (callbackCompleted < 0) callbackCompleted = elapsed.elapsed();
    });
    heartbeat.start(); window.ScheduleRecovery();
    QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(recovery) && !window.writingRecovery_, 120000);
    const auto automaticMs = elapsed.elapsed();
    maxGap = std::max(maxGap, automaticMs - lastHeartbeat); heartbeat.stop();
    QVERIFY(callbackCompleted >= 0); QVERIFY(window.project_->HasUnsavedMapChanges());
    const auto recoveryHash = War3Map::Sha256File(recovery); QVERIFY(recoveryHash.Succeeded());
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(recovery));
    QVERIFY(restored.project_->mapEdits_ && restored.project_->mapEdits_->Snapshot().Settings());
    QCOMPARE(restored.project_->mapEdits_->Snapshot().Settings()->mapName, changedName.toUtf8().toStdString());
    const auto sourceAfter = War3Map::Sha256File(source); QVERIFY(sourceAfter.Succeeded());
    const auto copyAfter = War3Map::Sha256File(copy); QVERIFY(copyAfter.Succeeded());
    QCOMPARE(*sourceAfter.value, *sourceHash.value); QCOMPARE(*copyAfter.value, *copyHash.value);
    QJsonObject receipt{{"sourceBytes", QString::number(sourceBytes)},
        {"sourceSha256", QString::fromStdString(*sourceHash.value)}, {"sourceUnchanged", true},
        {"copyUnchanged", true}, {"automaticRecoveryMs", automaticMs},
        {"timerCallbackCompletedMs", callbackCompleted}, {"maxUiEventGapMs", maxGap},
        {"heartbeatCount", heartbeatCount}, {"recoverySha256", QString::fromStdString(*recoveryHash.value)},
        {"recoveryBytes", QString::number(static_cast<qulonglong>(std::filesystem::file_size(recovery)))},
        {"restoredEditedMapName", true}, {"originalStillDirty", true},
        {"evidence", "actual production timer on one large map copy; no game or native editor"}};
    if (qEnvironmentVariable("FORGE_RECOVERY_REAL_REPEAT") == QStringLiteral("1")) {
        // 同一个副本再写一份，实测 CAS 更新及旧恢复备份清理，不复制第二套工程。
        name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
        name->setText(QStringLiteral("Audastra second recovery benchmark")); QVERIFY(window.settingsModule_->ApplyDraft());
        window.project_->recoveryTimer_->stop();
        heartbeatCount = 0; lastHeartbeat = 0; maxGap = 0; elapsed.restart(); heartbeat.start();
        window.WriteRecovery();
        QTRY_VERIFY_WITH_TIMEOUT(!window.writingRecovery_, 120000);
        const auto secondMs = elapsed.elapsed();
        maxGap = std::max(maxGap, secondMs - lastHeartbeat); heartbeat.stop();
        QCOMPARE(window.project_->recoveryPath_, recovery);
        const auto secondHash = War3Map::Sha256File(recovery); QVERIFY(secondHash.Succeeded());
        QVERIFY(*secondHash.value != *recoveryHash.value);
        QVERIFY(QDir(fixture.path()).entryList({"large-map-recovery.forge.backup-*.forge"}, QDir::Files).isEmpty());
        MainWindow secondRestored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(secondRestored.OpenEditorProjectPath(recovery));
        QCOMPARE(secondRestored.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("Audastra second recovery benchmark"));
        QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
        QCOMPARE(War3Map::Sha256File(copy).value, copyHash.value);
        receipt["secondRecoveryMs"] = secondMs;
        receipt["secondMaxUiEventGapMs"] = maxGap;
        receipt["secondHeartbeatCount"] = heartbeatCount;
        receipt["secondRecoverySha256"] = QString::fromStdString(*secondHash.value);
        receipt["secondRestoredEditedMapName"] = true;
        receipt["confirmedOldRecoveryBackupsRemaining"] = 0;
    }
    QFile output(receiptName); QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    const auto json = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
    QCOMPARE(output.write(json), json.size()); output.close();
    qInfo().noquote() << QString::fromUtf8(json);
    QVERIFY2(receipt.value("maxUiEventGapMs").toInteger() <= 1000, "Automatic recovery must keep the GUI event gap within one second");
    if (receipt.contains("secondMaxUiEventGapMs"))
        QVERIFY2(receipt.value("secondMaxUiEventGapMs").toInteger() <= 1000, "Recovery replacement must keep the GUI event gap within one second");
    QVERIFY2(callbackCompleted <= 1000, "Recovery snapshot capture must return within one second");
}

void NativeWorkbenchTests::FrozenRecoveryKeepsLaterEditsAndRejectsNewSession() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto other = NativePath(fixture.filePath("other.w3x"));
    auto settings = War3Map::MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes); QVERIFY(settings.Succeeded());
    settings.value->mapName = "Recovery lifecycle baseline";
    const auto settingsBytes = settings.value->Serialize(); QVERIFY(settingsBytes.Succeeded());
    const auto members = std::vector<War3Map::MapArchiveMember>{{"war3map.w3i", *settingsBytes.value}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    QVERIFY(War3Map::WriteVerifiedArchive(other, members).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe); QVERIFY(window.OpenMapArchivePath(source));
    auto* name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
    name->setText("Frozen first revision"); QVERIFY(window.settingsModule_->ApplyDraft());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    const auto baseline = window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map);
    const auto path = NativePath(fixture.filePath("first.forge"));
    auto first = window.project_->PrepareProjectSave(path, true); QVERIFY(first.Succeeded());
    name = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName"); QVERIFY(name);
    name->setText("Later unsaved revision"); QVERIFY(window.settingsModule_->ApplyDraft());
    window.project_->sourceChangeSuspected_ = true;
    window.project_->sourceChangeConfirmed_ = true;
    War3Map::QtProject::EditorProject::RunProjectSave(**first.value);
    QVERIFY((*first.value)->Outcome().Succeeded());
    QVERIFY(window.project_->CommitProjectSave(**first.value).Succeeded());
    QVERIFY(window.project_->sourceChangeSuspected_ && window.project_->sourceChangeConfirmed_);
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes, baseline->scopes);
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("Later unsaved revision"));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(path));
    QCOMPARE(restored.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("Frozen first revision"));
    window.project_->sourceChangeSuspected_ = false; window.project_->sourceChangeConfirmed_ = false;
    auto rebound = window.project_->PrepareProjectSave(NativePath(fixture.filePath("rebound.forge")), true); QVERIFY(rebound.Succeeded());
    auto samePathSource = War3Map::MapArchiveSession::Open(source); QVERIFY(samePathSource.Succeeded());
    window.project_->archiveSession_ = std::make_shared<War3Map::MapArchiveSession>(std::move(*samePathSource.value));
    War3Map::QtProject::EditorProject::RunProjectSave(**rebound.value);
    QVERIFY((*rebound.value)->Outcome().Succeeded());
    QVERIFY(!window.project_->CommitProjectSave(**rebound.value).Succeeded());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    auto stale = window.project_->PrepareProjectSave(NativePath(fixture.filePath("stale.forge")), true); QVERIFY(stale.Succeeded());
    bool discardedOwnedFixture = false;
    QTimer discardConfirmation;
    discardConfirmation.setSingleShot(true);
    connect(&discardConfirmation, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog) return;
        for (auto* button : dialog->buttons()) {
            if (dialog->buttonRole(button) == QMessageBox::DestructiveRole) {
                discardedOwnedFixture = true;
                button->click();
                return;
            }
        }
        dialog->reject();
    });
    discardConfirmation.start(0);
    QVERIFY(window.OpenMapArchivePath(other));
    QVERIFY(discardedOwnedFixture);
    const auto newGeneration = window.project_->editRevision_.Generation();
    War3Map::QtProject::EditorProject::RunProjectSave(**stale.value);
    QVERIFY((*stale.value)->Outcome().Succeeded());
    QVERIFY(!window.project_->CommitProjectSave(**stale.value).Succeeded());
    QCOMPARE(window.project_->editRevision_.Generation(), newGeneration);
    QCOMPARE(window.project_->sourceArchivePath_, other);
    QVERIFY(window.project_->recoveryPath_.empty()); QVERIFY(!window.project_->HasUnsavedMapChanges());
    const auto cancelledPath = NativePath(fixture.filePath("cancelled.forge"));
    auto cancelled = window.project_->PrepareProjectSave(cancelledPath); QVERIFY(cancelled.Succeeded());
    QVERIFY(window.project_->CancelPreparedProjectSave(**cancelled.value).Succeeded());
    War3Map::QtProject::EditorProject::RunProjectSave(**cancelled.value);
    QVERIFY(!std::filesystem::exists(cancelledPath));
    QVERIFY(window.SaveEditorProjectToPath(NativePath(fixture.filePath("manual-after-cancel.forge"))));
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::MapSaveWaitsForRecoveryAndCapturesLaterEdit() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const std::vector<War3Map::MapArchiveMember> members{{"war3map.w3u", MinimalUnitData(100)}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto original = ReadFile(source);
    const auto recovery = NativePath(fixture.filePath("recovery.forge"));
    const auto saved = NativePath(fixture.filePath("saved.w3x"));
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    MainWindow window(MainWindow::StartupMode::HeadlessProbe); QVERIFY(window.OpenMapArchivePath(source));
    window.project_->recoveryPath_ = recovery;
    QVERIFY(ChangeHealth(window, 200)); window.WriteRecovery(); QVERIFY(window.writingRecovery_);
    QVERIFY(ChangeHealth(window, 300));
    int beatsDuringWait = 0;
    QTimer heartbeat; heartbeat.setInterval(1);
    connect(&heartbeat, &QTimer::timeout, &window, [&] { if (window.project_->savingMap_) ++beatsDuringWait; });
    heartbeat.start();
    QVERIFY2(window.SaveMapArchiveToPath(saved), qPrintable(LastMapSaveDiagnostics(*window.project_->diagnosticsModel_)));
    heartbeat.stop();
    QVERIFY(beatsDuringWait > 0); QVERIFY(!window.writingRecovery_ && !window.project_->savingMap_);
    QVERIFY(!window.project_->HasUnsavedMapChanges());
    auto output = War3Map::MapArchiveSession::Open(saved); QVERIFY(output.Succeeded());
    const auto units = output.value->ReadMember("war3map.w3u"); QVERIFY(units.Succeeded());
    QCOMPARE(*units.value, MinimalUnitData(300));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(recovery));
    QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 200);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::RecoveryWorkerSurvivesWindowDestruction() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("source.forge"));
    const auto recovery = NativePath(fixture.filePath("closed-window-recovery.forge"));
    QVERIFY(MakeRecoveryLifecycleArchive(source).Succeeded());
    const auto sourceSha = War3Map::Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto sourceBytes = ReadFile(source);
    {
        MainWindow producer(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(producer.OpenMapArchivePath(source));
        QVERIFY(producer.SaveEditorProjectToPath(project));
    }
    const auto projectSha = War3Map::Sha256File(project); QVERIFY(projectSha.Succeeded());
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window->OpenEditorProjectPath(project));
    QVERIFY(window->project_->mapWorkspace_ && window->project_->restoredFiles_);
    const auto workspace = NativePath(window->project_->mapWorkspace_->path());
    const auto extraction = NativePath(window->project_->restoredFiles_->path());
    // 原地图与基线逐字节相同：恢复直接打开原地图；窗口销毁后私有目录清理，原地图不删不改、不留占用。
    QCOMPARE(window->project_->sourceArchivePath_, source);
    const std::weak_ptr<QTemporaryDir> workspaceOwner = window->project_->mapWorkspace_;
    const std::weak_ptr<QTemporaryDir> extractionOwner = window->project_->restoredFiles_;
    const std::weak_ptr<War3Map::MapArchiveSession> archiveOwner = window->project_->archiveSession_;
    window->project_->recoveryPath_ = recovery;
    QVERIFY(ChangeHealth(*window, 777));
    window->WriteRecovery();
    QVERIFY(window->writingRecovery_ && window->recoveryWorker_);
    const QPointer<QThread> worker(window->recoveryWorker_);
    bool finished = false;
    QObject completion;
    connect(worker.data(), &QThread::finished, &completion, [&] { finished = true; });
    // 断言失败时仍等自有 worker 释放文件，之后才能销毁夹具；不删除运行中的线程。
    const auto joinWorker = qScopeGuard([&] { if (worker) worker->wait(); });
    QVERIFY(worker->isRunning());
    const QPointer<MainWindow> windowGuard(window.get());
    window.reset();
    QVERIFY(windowGuard.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(finished && worker.isNull(), 30000);
    QVERIFY(workspaceOwner.expired() && extractionOwner.expired() && archiveOwner.expired());
    QVERIFY(!std::filesystem::exists(workspace)); QVERIFY(!std::filesystem::exists(extraction));
    QVERIFY(std::filesystem::is_regular_file(source));
    QCOMPARE(QSettings{}.value("project/latestRecovery"), previous);
    QVERIFY(std::filesystem::is_regular_file(recovery));
    {
        // 走完整工程打开，验证 frozen overlay 和完整地图基线均可回读。
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(recovery));
        QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 777);
        QVERIFY(restored.project_->HasUnsavedMapChanges());
        const auto baseline = War3Map::Sha256File(restored.project_->sourceArchivePath_);
        QVERIFY(baseline.Succeeded()); QCOMPARE(baseline.value, sourceSha.value);
        const auto resource = restored.project_->archiveSession_->ReadMember("resource/recovery-lifecycle.bin");
        QVERIFY(resource.Succeeded()); QCOMPARE(resource.value->size(), std::size_t(8 * 1024 * 1024));
        const auto original = War3Map::MapArchiveSession::Open(source); QVERIFY(original.Succeeded());
        const auto originalResource = original.value->ReadMember("resource/recovery-lifecycle.bin");
        QVERIFY(originalResource.Succeeded()); QCOMPARE(resource.value, originalResource.value);
    }
    QCOMPARE(ReadFile(source), sourceBytes);
    QCOMPARE(War3Map::Sha256File(project).value, projectSha.value);
#ifdef _WIN32
    // 回读窗口已销毁，再要求完全无共享的读/写/删除访问；只打开，不改动任何字节。
    for (const auto& path : {source, project, recovery}) {
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY2(handle != INVALID_HANDLE_VALUE, qPrintable(QStringLiteral("Exclusive reopen failed: %1; Win32=%2")
            .arg(QString::fromStdWString(path.wstring())).arg(GetLastError())));
        QVERIFY(CloseHandle(handle));
    }
#endif
}

void NativeWorkbenchTests::RecoveryExitCancellationKeepsPreviousPoint() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto recovery = NativePath(fixture.filePath("previous-recovery.forge"));
    QVERIFY(MakeRecoveryLifecycleArchive(source, 32 * 1024 * 1024).Succeeded());
    const auto sourceSha = War3Map::Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source)); QVERIFY(ChangeHealth(window, 200));
    QVERIFY(window.SaveEditorProjectToPath(recovery, true));
    const auto previousSha = War3Map::Sha256File(recovery); QVERIFY(previousSha.Succeeded());
    window.project_->recoveryPath_ = recovery;
    window.project_->recoveryFileSha256_ = *previousSha.value;
    QSettings{}.setValue("project/latestRecovery", QString::fromStdWString(recovery.wstring()));
    QVERIFY(ChangeHealth(window, 300));
    window.WriteRecovery();
    QVERIFY(window.writingRecovery_ && window.recoveryWorker_ && window.recoveryJob_);
    const QPointer<QThread> worker(window.recoveryWorker_);
    const auto job = window.recoveryJob_;
    const auto join = qScopeGuard([&] { if (worker) worker->wait(); });
    bool sawDialog = false;
    QObject timers;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QStringLiteral("recoveryExitDialog")) return;
        sawDialog = dialog->windowTitle() == QStringLiteral("正在写入恢复点");
        if (auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelRecoveryWrite"))) cancel->click();
    });
    QVERIFY(window.WaitForRecoveryExit());
    QTRY_VERIFY_WITH_TIMEOUT(!window.writingRecovery_ && !window.recoveryWorker_, 10000);
    QVERIFY(sawDialog && job->Cancelled() && !job->PublicationStarted());
    QCOMPARE(War3Map::Sha256File(recovery).value, previousSha.value);
    QCOMPARE(window.project_->recoveryFileSha256_, *previousSha.value);
    QCOMPARE(NativePath(QSettings{}.value("project/latestRecovery").toString()), recovery);
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 300);
    {
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(recovery));
        QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 200);
    }
    QCOMPARE(War3Map::Sha256File(source).value, sourceSha.value);
}

void NativeWorkbenchTests::RecoveryStartupExceptionKeepsPreviousPointAndManualTicket() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    QVERIFY(MakeRecoveryLifecycleArchive(source).Succeeded());
    const auto sourceSha = War3Map::Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    for (int failingStage = 1; failingStage <= 3; ++failingStage) {
        const auto recovery = NativePath(fixture.filePath(QStringLiteral("previous-%1.forge").arg(failingStage)));
        const auto manual = NativePath(fixture.filePath(QStringLiteral("manual-%1.forge").arg(failingStage)));
        MainWindow window(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(window.OpenMapArchivePath(source)); QVERIFY(ChangeHealth(window, 200));
        QVERIFY(window.SaveEditorProjectToPath(recovery, true));
        const auto previousSha = War3Map::Sha256File(recovery); QVERIFY(previousSha.Succeeded());
        window.project_->recoveryPath_ = recovery;
        window.project_->recoveryFileSha256_ = *previousSha.value;
        QSettings{}.setValue("project/latestRecovery", QString::fromStdWString(recovery.wstring()));
        QVERIFY(ChangeHealth(window, 300));
        int observedStages = 0;
        window.recoveryStartupStageHook_ = [&](int stage) {
            ++observedStages;
            if (stage == failingStage) throw std::runtime_error("owned recovery startup fault");
        };
        const auto beforeDiagnostics = window.project_->diagnosticsModel_->rowCount();
        window.WriteRecovery();
        QCOMPARE(observedStages, failingStage);
        QVERIFY(!window.writingRecovery_ && !window.recoveryWorker_ && !window.recoveryJob_);
        bool sawStartWarning = false;
        for (int row = beforeDiagnostics; row < window.project_->diagnosticsModel_->rowCount(); ++row)
            if (window.project_->diagnosticsModel_->index(row, 2).data().toString() == QStringLiteral("RECOVERY_WORKER_START_FAILED"))
                sawStartWarning = true;
        QVERIFY(sawStartWarning);
        QVERIFY(window.WaitForRecoveryExit()); // 未启动任务不能留下无限等待。
        QCOMPARE(War3Map::Sha256File(recovery).value, previousSha.value);
        QCOMPARE(window.project_->recoveryFileSha256_, *previousSha.value);
        QCOMPARE(NativePath(QSettings{}.value("project/latestRecovery").toString()), recovery);
        QVERIFY(window.project_->HasUnsavedMapChanges());
        QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 300);
        window.recoveryStartupStageHook_ = {};
        auto prepared = window.project_->PrepareProjectSave(manual);
        QVERIFY(prepared.Succeeded() && prepared.value); // 恢复失败不能占据手动工程票据。
        auto job = std::move(*prepared.value);
        War3Map::QtProject::EditorProject::RunProjectSave(*job);
        const auto committed = window.project_->CommitProjectSave(*job);
        QVERIFY(committed.Succeeded() && committed.value);
        QCOMPARE(War3Map::Sha256File(manual).value, std::optional<std::string>(committed.value->sha256));
        QCOMPARE(War3Map::Sha256File(recovery).value, previousSha.value);
        QCOMPARE(War3Map::Sha256File(source).value, sourceSha.value);
    }
}

void NativeWorkbenchTests::RecoveryQuitWaitsForWorkerAndPublishesCompletePoint() {
    if (qEnvironmentVariable("FORGE_RECOVERY_QUIT_LIFECYCLE_CHILD") != QStringLiteral("1")) {
        // aboutToQuit 只在同一测试程序的独立子进程中触发，不能结束父进程的测试事件循环。
        const auto originalTempRoot = QCoreApplication::instance()->property("nativeWorkbenchOriginalTempRoot").toString();
        QVERIFY(!originalTempRoot.isEmpty() && QFileInfo(originalTempRoot).isDir());
        QTemporaryDir childDirectory(QDir(originalTempRoot).filePath(QStringLiteral("aq-XXXXXX")));
        QVERIFY2(childDirectory.isValid(), qPrintable(childDirectory.errorString()));
        const auto childLogName = childDirectory.filePath(QStringLiteral("child-qt.log"));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FORGE_RECOVERY_QUIT_LIFECYCLE_CHILD"), QStringLiteral("1"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("TMP"), childDirectory.path());
        environment.insert(QStringLiteral("TEMP"), childDirectory.path());
        child.setProcessEnvironment(environment);
        child.setWorkingDirectory(childDirectory.path());
        const auto stopOwnedChild = qScopeGuard([&] {
            if (child.state() != QProcess::NotRunning) { child.kill(); (void)child.waitForFinished(5000); }
        });
        child.start(QCoreApplication::applicationFilePath(),
            {QStringLiteral("RecoveryQuitWaitsForWorkerAndPublishesCompletePoint"), QStringLiteral("-silent"),
                QStringLiteral("-o"), childLogName + QStringLiteral(",txt")});
        const bool started = child.waitForStarted(5000);
        const bool completed = started && child.waitForFinished(30000);
        if (!completed && child.state() != QProcess::NotRunning) { child.kill(); (void)child.waitForFinished(5000); }
        const auto output = child.readAllStandardOutput();
        const auto errors = child.readAllStandardError();
        QFile childLog(childLogName);
        const bool hasTestLog = childLog.open(QIODevice::ReadOnly);
        const auto testOutput = hasTestLog ? childLog.read(64 * 1024) : QByteArrayLiteral("QtTest logfile was not created/readable; check startup stderr.");
        const bool testLogTruncated = hasTestLog && !childLog.atEnd();
        childLog.close();
        if (!started || !completed || child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0 || !hasTestLog) {
            // 日志位于本次测试独立 AppData；不能放在进程退出就回收的 TMP 根里。
            const auto logDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/lifecycle-failures");
            (void)QDir().mkpath(logDirectory);
            QTemporaryFile log(QDir(logDirectory).filePath(QStringLiteral("recovery-quit-lifecycle-XXXXXX.log")));
            QString logName;
            if (log.open()) {
                (void)log.write(output); (void)log.write("\n--- stderr ---\n"); (void)log.write(errors);
                (void)log.write("\n--- QtTest (up to 64 KiB) ---\n"); (void)log.write(testOutput);
                if (testLogTruncated) (void)log.write("\nQtTest log truncated at 64 KiB.\n");
                log.setAutoRemove(false); logName = log.fileName(); log.close();
            }
            const auto failure = QStringLiteral("Owned child failed: started=%1 completed=%2 exit=%3 error=%4 log=%5\n%6\n%7")
                .arg(started).arg(completed).arg(child.exitCode()).arg(child.errorString(), logName,
                    QString::fromUtf8(output.right(8192)), QString::fromUtf8(errors.right(8192)))
                + QStringLiteral("\n--- QtTest ---\n") + QString::fromUtf8(testOutput.right(8192));
            QFAIL(qPrintable(failure));
        }
        return;
    }
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto recovery = NativePath(fixture.filePath("quit-recovery.forge"));
    QVERIFY(MakeRecoveryLifecycleArchive(source).Succeeded());
    const auto original = ReadFile(source);
    const auto sourceSha = War3Map::Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto previous = QSettings{}.value("project/latestRecovery");
    const auto restoreSettings = qScopeGuard([&] {
        if (previous.isValid()) QSettings{}.setValue("project/latestRecovery", previous);
        else QSettings{}.remove("project/latestRecovery");
    });
    auto window = std::make_unique<MainWindow>(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window->OpenMapArchivePath(source));
    window->project_->recoveryPath_ = recovery;
    QVERIFY(ChangeHealth(*window, 888));
    window->WriteRecovery();
    QVERIFY(window->writingRecovery_ && window->recoveryWorker_);
    const QPointer<QThread> worker(window->recoveryWorker_);
    const auto joinWorker = qScopeGuard([&] { if (worker) worker->wait(); });
    QVERIFY(worker->isRunning());
    bool aboutToQuitObserved = false, settledAtQuit = false;
    QObject observation;
    const auto quittingObservation = connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &observation, [&] {
        aboutToQuitObserved = true;
        // 生产连接先建立；此处必须已经结束 worker 并发布回执，不能只证明线程最终结束。
        settledAtQuit = !window->writingRecovery_ && !window->recoveryWorker_
            && window->project_->recoveryPath_ == recovery
            && !window->project_->recoveryFileSha256_.empty();
    });
    QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::exit(0); });
    QCOMPARE(QCoreApplication::exec(), 0);
    QObject::disconnect(quittingObservation);
    QVERIFY(aboutToQuitObserved && settledAtQuit);
    QVERIFY(!window->writingRecovery_ && !window->recoveryWorker_);
    QVERIFY(window->project_->HasUnsavedMapChanges());
    QCOMPARE(NativePath(QSettings{}.value("project/latestRecovery").toString()).lexically_normal(), recovery.lexically_normal());
    const auto recoverySha = War3Map::Sha256File(recovery); QVERIFY(recoverySha.Succeeded());
    QCOMPARE(*recoverySha.value, window->project_->recoveryFileSha256_);
    QTRY_VERIFY_WITH_TIMEOUT(worker.isNull(), 10000);
    window.reset();
    {
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(recovery));
        QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 888);
        const auto baseline = War3Map::Sha256File(restored.project_->sourceArchivePath_);
        QVERIFY(baseline.Succeeded()); QCOMPARE(baseline.value, sourceSha.value);
    }
    QCOMPARE(ReadFile(source), original);
#ifdef _WIN32
    for (const auto& path : {source, recovery}) {
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY2(handle != INVALID_HANDLE_VALUE, qPrintable(QStringLiteral("Exclusive reopen after quit failed: Win32=%1")
            .arg(GetLastError())));
        QVERIFY(CloseHandle(handle));
    }
#endif
}

void NativeWorkbenchTests::ProjectDirectoryUnicodeBaselineRoundTrip() {
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto sourceRoot = fixture.filePath(QStringLiteral("原创目录源"));
    QVERIFY(WriteText(sourceRoot + QStringLiteral("/table/unit.ini"), "[hfoo]\nudef = 7\n"));
    const auto relative = QStringLiteral("resource/atlas/geometry.bin");
    const QByteArray bytes("\x00\x01\x7f\xff\x00" "A", 6);
    const auto original = NativePath(sourceRoot + QLatin1Char('/') + relative);
    QVERIFY(WriteText(QString::fromStdWString(original.wstring()), bytes));
    const auto originalBytes = ReadFile(original);
    const auto originalSha = War3Map::Sha256File(original); QVERIFY(originalSha.Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenWorkspacePath(NativePath(sourceRoot)));
    QVERIFY(!window.project_->archiveSession_ && window.project_->directorySnapshot_);
    const auto target = NativePath(fixture.filePath(QStringLiteral("中文目录恢复.forge")));
    auto prepared = window.project_->PrepareProjectSave(target, true); QVERIFY(prepared.Succeeded());
    auto job = std::move(*prepared.value);
    War3Map::QtProject::EditorProject::RunProjectSave(*job);
    for (const auto& diagnostic : job->Outcome().diagnostics)
        qInfo().noquote() << QString::fromStdString(diagnostic.code) << QString::fromStdString(diagnostic.message);
    QVERIFY(job->Outcome().Succeeded());
    QVERIFY(window.project_->CommitProjectSave(*job).Succeeded());
    auto archive = War3Map::MapArchiveSession::Open(target); QVERIFY(archive.Succeeded());
    const auto stored = archive.value->ReadMember(std::string("baseline/workspace/") + relative.toUtf8().toStdString());
    QVERIFY(stored.Succeeded()); QCOMPARE(*stored.value, originalBytes);
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe); QVERIFY(restored.OpenEditorProjectPath(target));
    const auto recovered = restored.project_->projectRoot_ / NativePath(relative);
    QCOMPARE(ReadFile(recovered), originalBytes);
    QCOMPARE(War3Map::Sha256File(recovered).value, originalSha.value);
    QCOMPARE(ReadFile(original), originalBytes); QCOMPARE(War3Map::Sha256File(original).value, originalSha.value);
    // 公共 MPQ 仍按 ASCII 成员合同拒绝未支持的 Unicode 内部名字；来源目录/目标路径可为 Unicode。
    const auto unsupportedRoot = fixture.filePath(QStringLiteral("另一个原创目录源"));
    QVERIFY(WriteText(unsupportedRoot + "/table/unit.ini", "[hfoo]\nudef = 7\n"));
    const auto unsupportedFile = unsupportedRoot + QStringLiteral("/resource/中文目录/星图✨/地图😀.bin");
    QVERIFY(WriteText(unsupportedFile, bytes));
    const auto unsupportedSourceSha = War3Map::Sha256File(NativePath(unsupportedFile)); QVERIFY(unsupportedSourceSha.Succeeded());
    MainWindow unsupported(MainWindow::StartupMode::HeadlessProbe); QVERIFY(unsupported.OpenWorkspacePath(NativePath(unsupportedRoot)));
    const auto rejectedPath = NativePath(fixture.filePath("unsupported.forge"));
    auto rejected = unsupported.project_->PrepareProjectSave(rejectedPath, true); QVERIFY(rejected.Succeeded());
    War3Map::QtProject::EditorProject::RunProjectSave(**rejected.value);
    QVERIFY(!(*rejected.value)->Outcome().Succeeded());
    QVERIFY(std::any_of((*rejected.value)->Outcome().diagnostics.begin(), (*rejected.value)->Outcome().diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "BUILD_NAME_UNSAFE"; }));
    QVERIFY(!std::filesystem::exists(rejectedPath));
    QCOMPARE(War3Map::Sha256File(NativePath(unsupportedFile)).value, unsupportedSourceSha.value);
}

void NativeWorkbenchTests::CodeDraftFailureKeepsSourceAndExplicitDiscardWorks() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto uiRoot = fixture.filePath("original-ui");
    QVERIFY(WriteText(uiRoot + "/ui/config", "base\n"));
    QVERIFY(WriteText(uiRoot + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(uiRoot));
    QVERIFY(catalog.Succeeded());
    auto initial = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    QVERIFY(initial.Succeeded());
    initial.value->wct.customCode.text = "// baseline\r\n";
    const auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*initial.value, *catalog.value);
    QVERIFY(encoded.Succeeded());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    QVERIFY(War3Map::WriteVerifiedArchive(source, std::vector<War3Map::MapArchiveMember>{
        {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct}, {"war3map.w3u", MinimalUnitData(420)}}).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.UseUiDataRoot(NativePath(uiRoot)));
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->triggerDocument_);
    // 已有 ASCII 正文用 Latin1 提示，以真实不可编码中文草稿验证失败留稿。
    window.project_->triggerDocument_->wct.customCode.encoding = War3Map::Trigger::TextEncoding::Latin1;
    const auto projectPath = NativePath(fixture.filePath("active.forge"));
    QVERIFY(window.SaveEditorProjectToPath(projectPath));
    const auto originalSource = ReadFile(source), originalProject = ReadFile(projectPath);
    const auto originalBody = window.project_->triggerDocument_->wct.customCode.text;
    const int undoCount = window.project_->triggerUndoStack_->count();
    QVERIFY(window.triggersModule_->ShowGlobalCustomCode());
    const auto invalidDraft = QStringLiteral("// 中文不可写入 Latin1\n");
    window.triggersModule_->CodeEditor()->setPlainText(invalidDraft);
    QVERIFY(window.triggersModule_->HasPendingCodeDraft());
    const auto rejectedProject = NativePath(fixture.filePath("rejected.forge"));
    const auto rejectedMap = NativePath(fixture.filePath("rejected.w3x"));
    QVERIFY(!window.SaveEditorProjectToPath(rejectedProject));
    QVERIFY(!window.SaveMapArchiveToPath(rejectedMap));
    QVERIFY(!std::filesystem::exists(rejectedProject) && !std::filesystem::exists(rejectedMap));
    QCOMPARE(window.triggersModule_->CodeEditor()->toPlainText(), invalidDraft);
    QCOMPARE(window.project_->triggerDocument_->wct.customCode.text, originalBody);
    QCOMPARE(window.project_->triggerUndoStack_->count(), undoCount);
    const auto answerClose = [&](QMessageBox::ButtonRole role) {
        bool answered = false;
        QTimer response;
        response.setSingleShot(true);
        connect(&response, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            for (auto* button : dialog->buttons()) if (dialog->buttonRole(button) == role) {
                answered = true; button->click(); return;
            }
            dialog->reject();
        });
        response.start(0);
        const bool result = window.ConfirmDiscardPendingEdits(QStringLiteral("关闭"));
        response.stop();
        return std::pair{answered, result};
    };
    const auto failedSaveClose = answerClose(QMessageBox::AcceptRole);
    QVERIFY(failedSaveClose.first && !failedSaveClose.second);
    QVERIFY(window.triggersModule_->HasPendingCodeDraft());
    QCOMPARE(ReadFile(projectPath), originalProject); // 旧 fingerprint 不能放行保存失败。
    const auto explicitDiscard = answerClose(QMessageBox::DestructiveRole);
    QVERIFY(explicitDiscard.first && explicitDiscard.second);
    QVERIFY(!window.triggersModule_->HasPendingCodeDraft());
    QCOMPARE(window.project_->triggerDocument_->wct.customCode.text, originalBody);
    QCOMPARE(ReadFile(source), originalSource);
    QCOMPARE(ReadFile(projectPath), originalProject);
    QVERIFY(window.triggersModule_->ShowGlobalCustomCode());
    window.triggersModule_->CodeEditor()->setPlainText(QStringLiteral("// valid draft on old baseline\n"));
    window.project_->triggerDocument_->wct.customComment.text = "external header metadata";
    QVERIFY(!window.SaveEditorProjectToPath(rejectedProject));
    QVERIFY(!window.SaveMapArchiveToPath(rejectedMap));
    QVERIFY(window.triggersModule_->HasPendingCodeDraft());
    QCOMPARE(window.project_->triggerDocument_->wct.customCode.text, originalBody);
    QVERIFY(!std::filesystem::exists(rejectedProject) && !std::filesystem::exists(rejectedMap));
}

void NativeWorkbenchTests::CorruptImportTableRejectsWithoutOutput() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto output = NativePath(fixture.filePath("bad-output.w3x"));
    const auto asset = fixture.filePath("asset.bin");
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.w3u", MinimalUnitData(420)}, {"war3map.imp", Bytes("broken import table")}
    };
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    QVERIFY(WriteText(asset, "asset bytes"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 988));
    window.assetsModule_->AddFiles({asset});
    QVERIFY(!window.SaveMapArchiveToPath(output));
    QVERIFY(!std::filesystem::exists(output));
    QCOMPARE(window.project_->pendingImports_.size(), std::size_t{1});
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 988);
}

void NativeWorkbenchTests::ImportCannotBypassScriptBuild() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto output = NativePath(fixture.filePath("output.w3x"));
    const auto script = fixture.filePath("war3map.j");
    const std::vector<War3Map::Mpq::MpqBuildFile> files{{"Scripts/war3map.j", Bytes("original script")}};
    QVERIFY(War3Map::Mpq::WriteMpqArchive(source, files).Succeeded());
    QVERIFY(WriteText(script, "unvalidated replacement"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    window.assetsModule_->AddFiles({script});
    QVERIFY(!window.SaveMapArchiveToPath(output));
    QVERIFY(!std::filesystem::exists(output));
    QVERIFY(window.project_->archiveSession_->ReadMember("Scripts/war3map.j").value == std::optional(Bytes("original script")));
    QCOMPARE(window.project_->pendingImports_.size(), std::size_t{1});
}

void NativeWorkbenchTests::MapDataEditRequiresScriptBuildAndSurvivesProjectRestore() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto candidate = NativePath(fixture.filePath("candidate.w3x"));
    const auto projectPath = NativePath(fixture.filePath("map-data.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->mapEdits_ != nullptr);
    auto settings = War3Map::MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded());
    settings.value->mapName = "ChangedSettings";
    QVERIFY(window.project_->mapEdits_->EditSettings(*settings.value).Succeeded());
    QVERIFY(window.project_->HasUnsavedMapChanges());
    QVERIFY(!window.SaveMapArchiveToPath(candidate));
    QVERIFY(!std::filesystem::exists(candidate));
    QCOMPARE(ReadFile(source), original);
    QVERIFY(window.project_->mapEdits_->IsDirty());
    QVERIFY(window.SaveEditorProjectToPath(projectPath));

    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(projectPath));
    QVERIFY(restored.project_->mapEdits_ != nullptr && restored.project_->mapEdits_->IsDirty());
    const auto edits = restored.project_->mapEdits_->Replacements();
    QCOMPARE(edits.size(), std::size_t{1});
    const auto reopened = War3Map::MapData::MapSettings::Parse(edits[0].data);
    QVERIFY(reopened.Succeeded());
    QCOMPARE(reopened.value->mapName, std::string("ChangedSettings"));
    QVERIFY(!restored.SaveMapArchiveToPath(candidate));
    QVERIFY(!std::filesystem::exists(candidate));
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::StageDSettingsAndSoundDraftsPersistInProject() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("stage-d-source.w3x"));
    const auto projectPath = NativePath(fixture.filePath("stage-d.forge"));
    const auto rejected = NativePath(fixture.filePath("invalid-draft.forge"));
    War3Map::MapData::SoundRecord sound{};
    sound.name = "FirstSound";
    sound.filePath = "Sounds\\hit.wav";
    sound.eaxSetting = "DefaultEAXON";
    sound.volume = 80;
    sound.pitch = 1.0f;
    sound.channel = 5;
    sound.minDistance = 100.0f;
    sound.maxDistance = 4000.0f;
    sound.distanceCutoff = 2000.0f;
    sound.coneAngleInside = 360.0f;
    sound.coneAngleOutside = 360.0f;
    sound.coneOutsideVolume = 127;
    War3Map::MapData::SoundsDocument sounds{1, {sound}, {}};
    const auto w3s = sounds.Serialize();
    QVERIFY(w3s.Succeeded());
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.w3s", *w3s.value}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto original = ReadFile(source);

    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    auto* settingsPage = window.settingsModule_->Widget();
    auto* author = settingsPage->findChild<QLineEdit*>("mapSettings_author");
    QVERIFY(author);
    QSignalSpy settingsDrafts(window.settingsModule_, &War3Map::Modules::MapSettingsModule::DraftChanged);
    QSignalSpy soundDrafts(window.soundsModule_, &War3Map::Modules::SoundsModule::DraftChanged);
    QCOMPARE(window.windowTitle(), QStringLiteral("stage-d-source.w3x — Audastra"));
    author->setText(QStringLiteral("只在界面中的草稿"));
    QCOMPARE(window.windowTitle(), QStringLiteral("● stage-d-source.w3x — Audastra"));
    QCOMPARE(settingsDrafts.size(), qsizetype{1});
    window.settingsModule_->DiscardDraft();
    QCOMPARE(window.windowTitle(), QStringLiteral("stage-d-source.w3x — Audastra"));
    QCOMPARE(settingsDrafts.size(), qsizetype{2});
    QVERIFY(!settingsDrafts.back().front().toBool());
    auto* initialVolume = window.soundsModule_->Widget()->findChild<QLineEdit*>("soundField_volume");
    QVERIFY(initialVolume);
    initialVolume->setText(QStringLiteral("73"));
    QCOMPARE(window.windowTitle(), QStringLiteral("● stage-d-source.w3x — Audastra"));
    QCOMPARE(soundDrafts.size(), qsizetype{1});
    window.soundsModule_->DiscardPendingDraft();
    QCOMPARE(window.windowTitle(), QStringLiteral("stage-d-source.w3x — Audastra"));
    QCOMPARE(soundDrafts.size(), qsizetype{2});
    QVERIFY(!soundDrafts.back().front().toBool());
    QCOMPARE(ReadFile(source), original);
    author = settingsPage->findChild<QLineEdit*>("mapSettings_author");
    QVERIFY(author);
    author->setText(QStringLiteral("新的作者"));
    QVERIFY(window.settingsModule_->HasPendingDraft());
    QVERIFY(window.soundsModule_->Widget() != nullptr);
    QVERIFY(window.soundsModule_->EditField(0, "volume", "65").Succeeded());
    QVERIFY(window.SaveEditorProjectToPath(projectPath));
    QVERIFY(!window.settingsModule_->HasPendingDraft());
    QCOMPARE(ReadFile(source), original);
    auto projectArchive = War3Map::MapArchiveSession::Open(projectPath);
    QVERIFY(projectArchive.Succeeded());
    const auto editedW3i = projectArchive.value->ReadMember("overlay/map/war3map.w3i");
    const auto editedW3s = projectArchive.value->ReadMember("overlay/map/war3map.w3s");
    QVERIFY(editedW3i.Succeeded() && editedW3s.Succeeded());
    QCOMPARE(War3Map::MapData::MapSettings::Parse(*editedW3i.value).value->author,
        std::string("新的作者"));
    QCOMPARE(War3Map::MapData::ParseSounds(*editedW3s.value).value->entries.front().volume, 65);

    author = settingsPage->findChild<QLineEdit*>("mapSettings_author");
    QVERIFY(author);
    // 界面已改为具名选项；程序性注入无效数据集，验证保存服务仍拒绝坏草稿。
    auto* dataSet = settingsPage->findChild<QComboBox*>("mapSettings_gameDataSet");
    QVERIFY(dataSet);
    dataSet->addItem(QStringLiteral("无效测试值"), QStringLiteral("invalid-data-set"));
    dataSet->setCurrentIndex(dataSet->count() - 1);
    QVERIFY(window.settingsModule_->HasPendingDraft());
    QVERIFY(!window.SaveEditorProjectToPath(rejected));
    QVERIFY(!std::filesystem::exists(rejected));
    QCOMPARE(ReadFile(source), original);
    window.settingsModule_->DiscardDraft();

    auto* volumeDraft = window.soundsModule_->Widget()->findChild<QLineEdit*>("soundField_volume");
    QVERIFY(volumeDraft);
    volumeDraft->setText(QStringLiteral("not-a-volume"));
    QVERIFY(window.soundsModule_->HasPendingDraft());
    QVERIFY(!window.SaveEditorProjectToPath(rejected));
    QVERIFY(!std::filesystem::exists(rejected));
    window.soundsModule_->DiscardPendingDraft();

    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(projectPath));
    QVERIFY(restored.project_->MapEdits() != nullptr);
    const auto current = restored.project_->MapEdits()->Snapshot();
    QVERIFY(current.Settings() && current.Sounds());
    QCOMPARE(current.Settings()->author, std::string("新的作者"));
    QCOMPARE(current.Sounds()->entries.front().volume, 65);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::MapDataPanelReadsSearchesAndPaginates() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("data.mpq"));
    std::string strings = "STRING 42\n{\n地图说明\n}\n";
    for (int i = 1000; i < 1250; ++i) strings += "STRING " + std::to_string(i) + "\n{\nEntry " + std::to_string(i) + "\n}\n";
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}, {"war3map.wts", Bytes(strings)}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(!window.findChild<QAction*>("mapDataAction")->isEnabled());
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->mapDocument_);
    QVERIFY(window.project_->mapDocument_->InitializationSourceStatus().Succeeded());
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("mapDataDialog");
        QVERIFY(dialog);
        struct Close { QDialog* dialog; ~Close() { dialog->reject(); } } close{dialog};
        auto* nav = dialog->findChild<QListWidget*>("mapDataCategories");
        auto* values = dialog->findChild<QTreeWidget*>("mapDataValues");
        auto* search = dialog->findChild<QLineEdit*>("mapDataSearch");
        QVERIFY(nav && values && search);
        QCOMPARE(nav->count(), 7);
        QCOMPARE(values->editTriggers(), QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers));
        QCOMPARE(values->topLevelItem(0)->child(2)->text(1), QStringLiteral("地图说明"));
        nav->setCurrentRow(6);
        QCOMPARE(values->topLevelItemCount(), 100);
        auto* next = dialog->findChild<QPushButton*>("mapDataNext");
        QVERIFY(next->isEnabled()); next->click();
        QVERIFY(values->topLevelItem(0)->text(0).contains("1099"));
        search->setText("Entry 1249");
        QCOMPARE(values->topLevelItemCount(), 1);
        QVERIFY(values->topLevelItem(0)->text(0).contains("1249"));
        QVERIFY(!next->isEnabled());
        nav->setCurrentRow(3);
        QVERIFY(dialog->findChild<QLabel*>("mapDataHeading")->text().contains(QStringLiteral("未提供")));
    });
    window.ShowMapData();
    QVERIFY(window.project_->archiveSession_->VerifySourceUnchanged().Succeeded());
    QVERIFY(!window.project_->triggerDirty_);
    window.ResetProjectState();
    QVERIFY(!window.project_->mapDocument_);
    QVERIFY(!window.findChild<QAction*>("mapDataAction")->isEnabled());
}

void NativeWorkbenchTests::MapDataSurvivesProjectRestoreAndSourceSwitch() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.mpq"));
    const auto project = NativePath(fixture.filePath("data.forge"));
    const auto copy = NativePath(fixture.filePath("copy.mpq"));
    const std::vector<War3Map::MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.wts", Bytes("STRING 42\n{\n保留的说明\n}\n")}, {"war3map.w3c", Bytes("broken")}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QCOMPARE(window.project_->mapDocument_->Member(War3Map::MapDataKind::Cameras).state, War3Map::MapDataState::Invalid);
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(QFile::rename(QString::fromStdWString(source.wstring()), fixture.filePath("moved-original.mpq")));
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QVERIFY(restored.project_->mapDocument_ && restored.project_->mapDocument_->Settings());
    QVERIFY(restored.project_->mapDocument_->ResolveString("TRIGSTR_42").value == std::optional(std::string("保留的说明")));
    QVERIFY(restored.SaveMapArchiveToPath(copy));
    QCOMPARE(restored.project_->mapDocument_->SourcePath(), copy);
    QVERIFY(restored.project_->mapDocument_->Member(War3Map::MapDataKind::Cameras).sources[0].data == Bytes("broken"));
    QVERIFY(restored.SaveEditorProjectToPath(project));
    const auto empty = fixture.filePath("empty"); QVERIFY(QDir().mkpath(empty));
    QVERIFY(restored.OpenWorkspacePath(NativePath(empty)));
    QVERIFY(restored.project_->mapDocument_ && !restored.project_->mapDocument_->Settings());
    QCOMPARE(restored.project_->mapDocument_->Member(War3Map::MapDataKind::Cameras).state, War3Map::MapDataState::Missing);
}

void NativeWorkbenchTests::ProjectUpdatesSameFileAndKeepsBackup() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("current.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto original = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 610));
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(std::filesystem::equivalent(window.project_->projectFilePath_, project));
    const auto first = ReadFile(project);
    const auto firstSha = window.project_->projectFileSha256_;
    QVERIFY(ChangeHealth(window, 720));
    // Ctrl+S 对应入口不再要求重新选择路径。
    window.SaveEditorProject();
    QVERIFY(ReadFile(project) != first);
    QVERIFY(window.project_->projectFileSha256_ != firstSha);
    QCOMPARE(window.project_->projectFileSha256_, *War3Map::Sha256File(project).value);
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    const auto backups = QDir(fixture.path()).entryList({"current.forge.backup-*.forge"}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    QCOMPARE(ReadFile(NativePath(fixture.filePath(backups[0]))), first);
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QCOMPARE(Health(*restored.project_->objectDocuments_[0]).value_or(-1), 720);
    QVERIFY(ChangeHealth(restored, 830));
    restored.SaveEditorProject();
    MainWindow reopened(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(reopened.OpenEditorProjectPath(project));
    QCOMPARE(Health(*reopened.project_->objectDocuments_[0]).value_or(-1), 830);
    QCOMPARE(ReadFile(source), original);
}

void NativeWorkbenchTests::StaleProjectAndRecoveryKeepManualBaseline() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("shared.forge"));
    const auto recovery = NativePath(fixture.filePath("recovery.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow first(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(first.OpenMapArchivePath(source));
    QVERIFY(first.SaveEditorProjectToPath(project));
    MainWindow second(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(second.OpenEditorProjectPath(project));
    const auto previousSha = second.project_->projectFileSha256_;
    const auto previousBaseline = second.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes;
    QVERIFY(ChangeHealth(first, 650));
    QVERIFY(first.SaveEditorProjectToPath(project));
    const auto latest = ReadFile(project);
    QVERIFY(ChangeHealth(second, 980));
    QVERIFY(!second.SaveEditorProjectToPath(project));
    QCOMPARE(ReadFile(project), latest);
    QCOMPARE(second.project_->projectFileSha256_, previousSha);
    QCOMPARE(second.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes, previousBaseline);
    QVERIFY(second.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(second.SaveEditorProjectToPath(recovery, true));
    QVERIFY(std::filesystem::equivalent(second.project_->projectFilePath_, project));
    QCOMPARE(second.project_->projectFileSha256_, previousSha);
    QVERIFY(second.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    MainWindow recovered(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(recovered.OpenEditorProjectPath(recovery));
    QCOMPARE(Health(*recovered.project_->objectDocuments_[0]).value_or(-1), 980);
    QVERIFY(recovered.project_->projectFilePath_.empty());
    QVERIFY(recovered.project_->projectFileSha256_.empty());
    QVERIFY(recovered.project_->lastProjectFingerprint_.empty());
    QVERIFY(recovered.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(recovered.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    QTimer::singleShot(0, [] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) message->reject();
    });
    QVERIFY(!recovered.ConfirmDiscardPendingEdits(QStringLiteral("关闭")));
    QVERIFY(recovered.SaveEditorProjectToPath(NativePath(fixture.filePath("recovered-manual.forge"))));
    QVERIFY(!recovered.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
}

void NativeWorkbenchTests::EditRevisionFollowsUndoAndIndependentSaveTargets() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("edit.forge"));
    const auto copy = NativePath(fixture.filePath("copy.w3x"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    const int row = FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm");
    QVERIFY(row >= 0);
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(window.objectsModule_->objectFieldModel_->index(row, 3), 777, Qt::EditRole));
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    const auto changedRevision = window.project_->editRevision_.Revision();
    window.project_->objectUndoStacks_[0]->undo();
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(window.project_->editRevision_.Revision() > changedRevision);
    window.project_->objectUndoStacks_[0]->redo();
    QVERIFY(window.project_->objectDirty_[0]);
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    QVERIFY(window.SaveMapArchiveToPath(copy));
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    // 完整地图基线已经改变，工程需要保存新来源，不能仅比较物编字段。
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    window.SaveEditorProject();
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    window.project_->objectUndoStacks_[0]->undo();
    QVERIFY(window.project_->objectDirty_[0]);
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
}

void NativeWorkbenchTests::ProjectJournalSurvivesReopenAndUpdate() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("journal.forge"));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 700));
    QVERIFY(window.SaveEditorProjectToPath(project));
    const auto journal = [&] {
        const auto archive = War3Map::MapArchiveSession::Open(project);
        if (!archive.Succeeded()) return QJsonArray{};
        const auto bytes = archive.value->ReadMember("project.json");
        const QByteArray json(reinterpret_cast<const char*>(bytes.value->data()), static_cast<qsizetype>(bytes.value->size()));
        return QJsonDocument::fromJson(json).object()["journal"].toArray();
    };
    const auto firstJournal = journal();
    QVERIFY(!firstJournal.empty());
    MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QVERIFY(ChangeHealth(restored, 800));
    QVERIFY(restored.SaveEditorProjectToPath(project));
    const auto secondJournal = journal();
    QVERIFY(secondJournal.size() > firstJournal.size());
    QCOMPARE(secondJournal[0], firstJournal[0]);
    QVERIFY(restored.SaveEditorProjectToPath(project));
    QCOMPARE(journal(), secondJournal); // 重复保存不能重复追加历史。
}

void NativeWorkbenchTests::ProjectCloseGuardSurvivesUndoAndImportRemoval() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("active.forge"));
    const auto asset = fixture.filePath("asset.bin");
    QVERIFY(MakeArchive(source, 420).Succeeded());
    QVERIFY(WriteText(asset, "imported bytes"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(InstallSyntheticEditorContext(window));
    window.objectsModule_->objectSourceTabs_->setCurrentIndex(1);
    window.objectsModule_->objectListView_->selectRow(0);
    window.objectsModule_->ShowSelectedObject();
    const int row = FieldRow(*window.objectsModule_->objectFieldModel_, "uhpm");
    QVERIFY(window.objectsModule_->objectFieldModel_->setData(window.objectsModule_->objectFieldModel_->index(row, 3), 777, Qt::EditRole));
    QVERIFY(window.SaveEditorProjectToPath(project));
    window.project_->objectUndoStacks_[0]->undo();
    QVERIFY(!window.project_->objectDirty_[0]);
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    bool prompted = false;
    QTimer cancel;
    cancel.setSingleShot(true);
    connect(&cancel, &QTimer::timeout, &window, [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            prompted = true;
            dialog->reject();
        }
    });
    cancel.start(0);
    const bool mayCloseAfterUndo = window.ConfirmDiscardPendingEdits(QStringLiteral("关闭"));
    cancel.stop();
    QVERIFY(prompted);
    QVERIFY(!mayCloseAfterUndo);
    QVERIFY(window.SaveEditorProjectToPath(project));
    window.assetsModule_->AddFiles({asset});
    QVERIFY(window.SaveEditorProjectToPath(project));
    window.project_->pendingImports_.clear();
    window.project_->NotifyImportsChanged();
    QVERIFY(window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Project));
    QVERIFY(!window.project_->editRevision_.IsDirty(War3Map::EditSaveTarget::Map));
    prompted = false;
    cancel.start(0);
    const bool mayCloseAfterRemoval = window.ConfirmDiscardPendingEdits(QStringLiteral("关闭"));
    cancel.stop();
    QVERIFY(prompted);
    QVERIFY(!mayCloseAfterRemoval);
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(window.ConfirmDiscardPendingEdits(QStringLiteral("关闭")));
}

void NativeWorkbenchTests::LockedImportRejectsProjectAndMapWithoutClearingEdits() {
    QTemporaryDir fixture;
    const auto source = NativePath(fixture.filePath("source.w3x"));
    const auto project = NativePath(fixture.filePath("locked.forge"));
    const auto copy = NativePath(fixture.filePath("copy.w3x"));
    const auto asset = fixture.filePath("asset.bin");
    QVERIFY(MakeArchive(source, 420).Succeeded());
    QVERIFY(WriteText(asset, "same length imported bytes"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(ChangeHealth(window, 640));
    window.assetsModule_->AddFiles({asset});
    const auto projectBaseline = window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes;
    const auto mapBaseline = window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes;
    QFile externalWriter(asset);
    QVERIFY(externalWriter.open(QIODevice::ReadWrite));
    QVERIFY(!window.SaveEditorProjectToPath(project));
    QVERIFY(!window.SaveMapArchiveToPath(copy));
    QVERIFY(!std::filesystem::exists(project) && !std::filesystem::exists(copy));
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes, projectBaseline);
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes, mapBaseline);
    QCOMPARE(window.project_->pendingImports_.size(), std::size_t{1});
    QVERIFY(window.project_->objectDirty_[0]);
    externalWriter.close();
    QVERIFY(window.SaveEditorProjectToPath(project));
    QVERIFY(window.SaveMapArchiveToPath(copy));
    QVERIFY(window.project_->pendingImports_.empty());
}

void NativeWorkbenchTests::WorkspaceActionsToggleAndStatusBarsFollowPresentation() {
    using Mode = War3Map::Agent::AgentWindow::PresentationMode;
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.SetupAgentWorkspace(false);
    window.resize(1920, 1080);
    window.show();
    window.ActivateModule("world");
    QCoreApplication::processEvents();
    auto* changes = window.findChild<QAction*>(QStringLiteral("projectChangesAction"));
    auto* assistant = window.findChild<QAction*>(QStringLiteral("agentWorkspaceAction"));
    QVERIFY(changes && assistant && changes->isCheckable() && assistant->isCheckable());
    for (int click = 0; click < 4; ++click) {
        changes->trigger();
        QCoreApplication::processEvents();
        const bool shown = click % 2 == 0;
        QCOMPARE(window.changesDock_->isVisible(), shown);
        QCOMPARE(changes->isChecked(), shown);
    }
    changes->trigger();
    QVERIFY(changes->isChecked());
    window.changesDock_->close();
    QCoreApplication::processEvents();
    QVERIFY(!changes->isChecked());
    for (int click = 0; click < 4; ++click) {
        assistant->trigger();
        QCoreApplication::processEvents();
        const bool shown = click % 2 == 0;
        QCOMPARE(window.AgentWorkspaceVisible(), shown);
        QCOMPARE(assistant->isChecked(), shown);
    }
    const auto visibleStatusBars = [&]() {
        int count = 0;
        for (const auto* bar : window.findChildren<QStatusBar*>()) if (bar->isVisible()) ++count;
        return count;
    };
    QCOMPARE(window.findChildren<QStatusBar*>().size(), qsizetype{1});
    for (int cycle = 0; cycle < 5; ++cycle) {
        window.PresentAgentWorkspace(static_cast<int>(Mode::Page));
        QCoreApplication::processEvents();
        QCOMPARE(visibleStatusBars(), 0);
        QVERIFY(!window.Dock()->isVisible());
        QVERIFY(assistant->isChecked());
        window.PresentAgentWorkspace(static_cast<int>(Mode::Sidebar));
        QCoreApplication::processEvents();
        QCOMPARE(visibleStatusBars(), 1);
        QVERIFY(window.Dock()->isVisible());
        const auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("agentWorkspaceStack"));
        QVERIFY(stack);
        QVERIFY(stack->rect().contains(window.Dock()->geometry()));
        QVERIFY(assistant->isChecked());
    }
    window.HideAgentWorkspace();
    QCoreApplication::processEvents();
    QCOMPARE(visibleStatusBars(), 1);
    QVERIFY(!assistant->isChecked());
    window.PresentAgentWorkspace(static_cast<int>(Mode::Detached));
    auto* agent = window.findChild<War3Map::Agent::AgentWindow*>();
    QVERIFY(agent && agent->isVisible());
    agent->close();
    QCoreApplication::processEvents();
    QVERIFY(!assistant->isChecked());
    window.PresentAgentWorkspace(static_cast<int>(Mode::Page));
    window.ActivateModule("triggers");
    QCoreApplication::processEvents();
    QVERIFY(!assistant->isChecked());
    QCOMPARE(visibleStatusBars(), 1);
}

void NativeWorkbenchTests::ChangesPanelUsesBaselineAndSharedUndoWithoutWritingSource() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("r08-changes.w3x"));
    auto settings = War3Map::MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded());
    settings.value->mapName = "R08修改基线"; // 原夹具包含旧编码字节；可逆字段要求 UTF-8 原值。
    const auto encoded = settings.value->Serialize();
    QVERIFY(encoded.Succeeded());
    const std::array<War3Map::MapArchiveMember, 1> members{{{"war3map.w3i", *encoded.value}}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    auto* title = window.settingsModule_->Widget()->findChild<QLineEdit*>("mapSettings_mapName");
    QVERIFY(title);
    title->setText(QStringLiteral("R08当前地图名"));
    QVERIFY(window.settingsModule_->ApplyDraft());
    auto* undo = window.project_->mapDataUndoStack_;
    QVERIFY(undo == window.settingsModule_->UndoStack());
    QCOMPARE(undo->count(), 1);
    window.InvalidateCommandContext();
    QVERIFY(!window.changesDock_->isVisible());
    QVERIFY(!window.changesRefreshQueued_);
    QCoreApplication::processEvents();
    QVERIFY(!window.commandMapSnapshot_); // 隐藏明细不排整张图差分；打开/显式查询仍使用完整服务。
    QCOMPARE(window.changesAction_->text(), QStringLiteral("修改"));
    QVERIFY(window.changesAction_->toolTip().contains(QStringLiteral("展开")));
    QCOMPARE(window.RefreshCommandMapSnapshot(), QString{});
    QVERIFY(window.commandMapSnapshot_ && window.commandMapDocument_);
    const auto binding = window.commandMapSnapshot_->Binding();
    const War3Map::Services::ProjectChangesContext context{
        window.project_->mapDocument_.get(), window.commandMapDocument_.get(), binding};
    const auto page = War3Map::Services::QueryProjectChanges(context, binding, War3Map::MapDataKind::Settings);
    QVERIFY(page.Succeeded());
    const auto found = std::find_if(page.value->rows.begin(), page.value->rows.end(),
        [](const auto& row) { return row.fieldId == "map_name"; });
    QVERIFY(found != page.value->rows.end());
    const auto row = *found;
    QVERIFY(row.revertible && row.before && row.after);
    QCOMPARE(row.before->bytes, std::string("R08修改基线"));
    QCOMPARE(row.after->bytes, std::string("R08当前地图名"));
    QVERIFY(row.beforeMemberSha256 && row.afterMemberSha256);
    QVERIFY(row.beforeMemberSha256 != row.afterMemberSha256);
    window.resize(1920, 1080); window.show(); window.ExecuteCommand("project.changes"); QTest::qWait(20);
    QVERIFY(window.changesDock_ && window.changesDock_->isVisible());
    auto* tree = window.changesPanel_->findChild<QTreeView*>("changesTree");
    auto* revert = window.changesPanel_->findChild<QPushButton*>("changesRevert");
    QVERIFY(tree && revert);
    const auto matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole,
        QString::fromStdString(row.id), 1, Qt::MatchExactly | Qt::MatchRecursive);
    QCOMPARE(matches.size(), 1);
    tree->scrollTo(matches.front());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(matches.front()).center());
    QCOMPARE(window.changesPanel_->SelectedId(), QString::fromStdString(row.id));
    QVERIFY(revert->isEnabled());
    auto* before = window.changesPanel_->findChild<QLabel*>("changesBefore");
    auto* after = window.changesPanel_->findChild<QLabel*>("changesAfter");
    QVERIFY(before && after);
    QCOMPARE(before->toolTip(), QStringLiteral("R08修改基线"));
    QCOMPARE(after->toolTip(), QStringLiteral("R08当前地图名"));
    QTest::mouseClick(window.changesPanel_->findChild<QPushButton*>("changesReveal"), Qt::LeftButton);
    QCOMPARE(window.CurrentModule(), std::optional<std::string>{"map-settings"});
    QCOMPARE(window.changesPanel_->SelectedId(), QString::fromStdString(row.id));
    // 显式输出真实 Qt 离屏停靠布局；不作为用户前台窗口或高 DPI 验收。
    const auto images = qEnvironmentVariable("FORGE_R08_CHANGES_SCREENSHOT_DIR");
    if (!images.isEmpty()) {
        QVERIFY(QFontMetrics(QApplication::font()).inFont(QChar{u'地'}));
        QVERIFY(QDir().mkpath(images));
        for (const auto size : {QSize{1920, 1080}, QSize{2560, 1440}}) {
            window.resize(size); QTest::qWait(20);
            QCOMPARE(window.size(), size);
            QCOMPARE(window.changesPanel_->SelectedId(), QString::fromStdString(row.id));
            QVERIFY(window.grab().save(images + QStringLiteral("/changes-%1x%2-offscreen.png").arg(size.width()).arg(size.height())));
        }
    }
    QTest::mouseClick(revert, Qt::LeftButton);
    QCOMPARE(undo->count(), 2);
    QCOMPARE(undo->index(), 2);
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08修改基线"));
    undo->undo();
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08当前地图名"));
    undo->redo();
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08修改基线"));
    undo->undo(); // 值再次相同也不能复用旧修订的行。
    const auto revision = window.project_->mapEdits_->Revision();
    const auto index = undo->index();
    window.RevertProjectChange(row);
    QCOMPARE(window.project_->mapEdits_->Revision(), revision);
    QCOMPARE(undo->index(), index);
    QCOMPARE(undo->count(), 2);
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08当前地图名"));
    QCOMPARE(ReadFile(source), sourceBytes);
    const auto projectBaseline = window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes;
    const auto mapBaseline = window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes;
    const auto diagnosticCount = window.project_->diagnosticsModel_->rowCount();
    window.CheckChangesSave();
    QVERIFY(!window.changesChecking_); // 有修改且没有工具链时，现有保存准备门禁应同步拒绝。
    auto* preflight = window.changesPanel_->findChild<QLabel*>("changesPreflight");
    QVERIFY(preflight);
    QVERIFY(preflight->text().contains(QStringLiteral("保存准备被拒绝")));
    QVERIFY(window.project_->diagnosticsModel_->rowCount() > diagnosticCount);
    QCOMPARE(window.project_->mapEdits_->Revision(), revision);
    QCOMPARE(undo->index(), index);
    QCOMPARE(undo->count(), 2);
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes, projectBaseline);
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes, mapBaseline);
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08当前地图名"));
    QCOMPARE(ReadFile(source), sourceBytes);
    undo->undo(); // 未改内容无需脚本重建，用同一夹具验证真实后台预检。
    QCOMPARE(undo->index(), 0);
    QVERIFY(!window.project_->mapEdits_->IsDirty());
    const auto cleanRevision = window.project_->mapEdits_->Revision();
    window.CheckChangesSave();
    QVERIFY(window.changesChecking_);
    QTRY_VERIFY(!window.changesChecking_);
    QVERIFY(preflight->text().contains(QStringLiteral("未写出")));
    QCOMPARE(window.project_->mapEdits_->Revision(), cleanRevision);
    QCOMPARE(undo->index(), 0);
    QCOMPARE(undo->count(), 2);
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Project)->scopes, projectBaseline);
    QCOMPARE(window.project_->editRevision_.Baseline(War3Map::EditSaveTarget::Map)->scopes, mapBaseline);
    QCOMPARE(window.project_->mapEdits_->Snapshot().Settings()->mapName, std::string("R08修改基线"));
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::ChangesPanelObjectsAndTriggerUseSharedInverse() {
    namespace Services = War3Map::Services;
    namespace Trigger = War3Map::Trigger;
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("content-changes.w3x"));
    QVERIFY(MakeArchive(source, 420).Succeeded()); const auto sourceBytes = ReadFile(source);
    const auto definitions = fixture.filePath("definitions");
    QVERIFY(WriteText(definitions + "/ui/config", "base\n"));
    QVERIFY(WriteText(definitions + "/ui/base/action.txt",
        "[SetFlag]\ntitle=\"Set flag\"\ndescription=\"Set ${flag}\"\n[[.args]]\ntype=boolean\ndefault=\"true\"\n"));
    auto catalog = Trigger::UiCatalog::Load(NativePath(definitions)); QVERIFY(catalog.Succeeded());
    auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); QVERIFY(empty.Succeeded());
    Trigger::CategoryRecord category; category.id = 100; category.categoryId = 1; category.name.text = "Content";
    empty.value->categories.push_back(category);
    const auto triggerId = Trigger::TriggerDocumentEditor::AddTrigger(*empty.value, 1, "Flag"); QVERIFY(triggerId.Succeeded());
    auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(*empty.value, *catalog.value,
        *triggerId.value, Trigger::UiDefinitionKind::Action, "SetFlag"); QVERIFY(appended.Succeeded());
    const auto argumentId = empty.value->argumentNodes.front().id;
    QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*empty.value, argumentId, 3, "true").Succeeded());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe); QVERIFY(window.OpenMapArchivePath(source));
    window.project_->uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    window.project_->triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*empty.value));
    window.ResetEditTracking();
    auto* object = window.project_->objectDocuments_[0].get(); QVERIFY(object);
    const auto objectKey = object->Objects().front().key;
    const auto objectFingerprint = object->DocumentFingerprint();
    QVERIFY(object->SetModification(objectKey, {"uhpm"}, std::int32_t(999)).Succeeded());
    QVERIFY(object->SetModification(objectKey, {"umvs"}, std::int32_t(270)).Succeeded());
    QVERIFY(object->SetModification(objectKey, {"unam"}, std::string("Changed")).Succeeded());
    window.ObserveObjectEdit(0);
    QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*window.project_->triggerDocument_, argumentId, 3, "false").Succeeded());
    window.ObserveTriggerEdit(); window.RefreshChangesPanel();
    QCOMPARE(window.changesAction_->text(), QStringLiteral("修改"));
    QVERIFY(window.changesAction_->toolTip().startsWith(QStringLiteral("已确认 4 项，列出 4 项")));
    auto* tree = window.changesPanel_->findChild<QTreeView*>("changesTree"); QVERIFY(tree);
    std::function<int(const QModelIndex&)> leaves = [&](const QModelIndex& parent) {
        int total = 0;
        for (int i = 0; i < tree->model()->rowCount(parent); ++i) {
            const auto index = tree->model()->index(i, 0, parent);
            total += index.data(Qt::UserRole).isValid() ? 1 : leaves(index);
        }
        return total;
    };
    QCOMPARE(leaves({}), 4);
    const auto query = [&](Services::ProjectChangeDomain domain) {
        (void)window.RefreshCommandMapSnapshot();
        const auto binding = window.commandMapSnapshot_->Binding();
        const auto context = window.project_->CaptureContentChangesContext(binding);
        if (!context.Succeeded()) return War3Map::Result<Services::ProjectChangesPage>{std::nullopt, context.diagnostics};
        return Services::QueryProjectContentChanges(*context.value, binding, domain);
    };
    auto page = query(Services::ProjectChangeDomain::Objects); QVERIFY(page.Succeeded()); QCOMPARE(page.value->rows.size(), std::size_t(3));
    const auto stale = page.value->rows.front();
    window.RevertProjectChange(stale);
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    window.RevertProjectChange(stale); // Same map revision cannot hide a changed object revision.
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 1);
    for (int i = 0; i < 2; ++i) {
        page = query(Services::ProjectChangeDomain::Objects); QVERIFY(page.Succeeded()); QVERIFY(!page.value->rows.empty());
        window.RevertProjectChange(page.value->rows.front());
    }
    QCOMPARE(object->DocumentFingerprint(), objectFingerprint);
    QCOMPARE(window.project_->objectUndoStacks_[0]->count(), 3);
    page = query(Services::ProjectChangeDomain::Triggers); QVERIFY(page.Succeeded()); QCOMPARE(page.value->rows.size(), std::size_t(1));
    QVERIFY(page.value->rows.front().revertible); window.RevertProjectChange(page.value->rows.front());
    QCOMPARE(window.project_->triggerDocument_->argumentNodes.front().value.text, std::string("true"));
    QCOMPARE(window.project_->triggerUndoStack_->count(), 1);
    window.project_->triggerUndoStack_->undo();
    QCOMPARE(window.project_->triggerDocument_->argumentNodes.front().value.text, std::string("false"));
    window.project_->triggerUndoStack_->redo();
    QCOMPARE(window.project_->triggerDocument_->argumentNodes.front().value.text, std::string("true"));
    QCOMPARE(ReadFile(source), sourceBytes);
}

// 外壳：常见窗口尺寸下任何区域都不能把主窗口撑得比窗口本身更宽（否则右侧内容被裁掉）。
void NativeWorkbenchTests::CommandExecutionMayDeleteWindowOwner() {
    QPointer<MainWindow> window = new MainWindow(MainWindow::StartupMode::HeadlessProbe);
    War3Map::Services::CommandDefinition close;
    close.id = "test.close-owner";
    close.title = "Close owner in command callback";
    close.execute = [window]() {
        delete window.data();
        return War3Map::Services::CommandExecution{
            War3Map::Services::CommandExecutionStatus::Failed, {}, "Owner closed"};
    };
    QVERIFY(window->commandRegistry_.Register(std::move(close)));
    window->ExecuteCommand("test.close-owner");
    QVERIFY(window.isNull());
}

void NativeWorkbenchTests::CommandPaletteQueriesCurrentProjectAndRejectsStaleResults() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath("r08-search.w3x"));
    War3Map::MapData::SoundRecord sound{};
    sound.name = "R08声音"; sound.filePath = "Sounds\\sample.wav";
    sound.eaxSetting = "DefaultEAXON"; sound.volume = 80; sound.pitch = 1;
    sound.channel = 5; sound.minDistance = 100; sound.maxDistance = 4000; sound.distanceCutoff = 2000;
    sound.coneAngleInside = 360; sound.coneAngleOutside = 360; sound.coneOutsideVolume = 127;
    const auto sounds = War3Map::MapData::SoundsDocument{1, {sound, sound}, {}}.Serialize();
    QVERIFY(sounds.Succeeded());
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3s", *sounds.value}, {"war3map.w3u", MinimalUnitData(100)}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceBytes = ReadFile(source);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    int sharedCalls = 0;
    QAction shared(QStringLiteral("R08 共用动作"), &window);
    connect(&shared, &QAction::triggered, [&sharedCalls]() { ++sharedCalls; });
    window.RegisterCommandAction("test.shared", &shared, QStringLiteral("测试"));
    shared.trigger();
    QVERIFY(window.commandRegistry_.Execute("test.shared").Succeeded());
    QCOMPARE(sharedCalls, 2);
    shared.setEnabled(false);
    QCOMPARE(window.commandRegistry_.Execute("test.shared").status, War3Map::Services::CommandExecutionStatus::Disabled);
    QCOMPARE(sharedCalls, 2);
    auto content = window.QueryCommandPalette(QStringLiteral("R08"), false);
    std::vector<CommandPalette::Entry> soundHits;
    for (const auto& entry : content) if (entry.label == QStringLiteral("R08声音")) soundHits.push_back(entry);
    QCOMPARE(soundHits.size(), std::size_t{2});
    QVERIFY(soundHits[0].stableId != soundHits[1].stableId);
    soundHits[1].action();
    QCOMPARE(window.CurrentModule(), std::optional<std::string>{"sounds"});
    auto* definitions = window.soundsModule_->Widget()->findChild<QTableView*>("soundDefinitionsTable");
    QVERIFY(definitions);
    QCOMPARE(definitions->currentIndex().row(), 1);
    window.ExecuteCommand("module.assets");
    QVERIFY(ChangeHealth(window, 64));
    const auto afterEdit = window.CurrentModule();
    soundHits[0].action(); // 编辑后保留的旧结果必须失效，不能重新打开另一个同名声音。
    QCOMPARE(window.CurrentModule(), afterEdit);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("工程已变化，请重新搜索"));
    const auto refreshed = window.QueryCommandPalette(QStringLiteral("R08"), false);
    QVERIFY(std::any_of(refreshed.begin(), refreshed.end(), [](const auto& entry) { return entry.label == QStringLiteral("R08声音"); }));
    const auto onlyCommands = window.QueryCommandPalette(QStringLiteral("R08"), true);
    QVERIFY(std::all_of(onlyCommands.begin(), onlyCommands.end(), [](const auto& entry) { return entry.section == QStringLiteral("命令"); }));
    window.resize(1920, 1080); window.show(); window.activateWindow();
    auto* globalSearch = window.findChild<QLineEdit*>("globalSearch");
    QVERIFY(globalSearch);
    globalSearch->setFocus(); QTest::qWait(20);
    QTest::keyClick(globalSearch, Qt::Key_P, Qt::ControlModifier);
    QTRY_VERIFY(window.commandPalette_->isVisible());
    auto* paletteSearch = window.commandPalette_->findChild<QLineEdit*>("commandPaletteSearch");
    auto* paletteResults = window.commandPalette_->findChild<QListView*>("commandPaletteResults");
    QVERIFY(paletteSearch && paletteResults);
    QTest::keyClick(paletteSearch, Qt::Key_Escape);
    QTRY_VERIFY(!window.commandPalette_->isVisible());
    globalSearch->setFocus();
    QTest::keyClick(globalSearch, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(window.commandPalette_->isVisible());
    QCOMPARE(paletteSearch->text(), QStringLiteral(">"));
    paletteSearch->setText(QStringLiteral("> sounds"));
    QTest::keyClick(paletteSearch, Qt::Key_Return);
    QTRY_VERIFY(!window.commandPalette_->isVisible());
    QCOMPARE(window.CurrentModule(), std::optional<std::string>{"sounds"});
    // 仅显式环境路径才导出离屏 Qt 控件图；这不是用户桌面/高DPI验收。
    const auto images = qEnvironmentVariable("FORGE_R08_SCREENSHOT_DIR");
    if (!images.isEmpty()) {
        QVERIFY(QFontMetrics(QApplication::font()).inFont(QChar{u'地'}));
        qInfo() << "capture font:" << QApplication::font().families();
        QVERIFY(QDir().mkpath(images));
        for (const auto size : {QSize{1920, 1080}, QSize{2560, 1440}}) {
            window.resize(size); window.ShowCommandPalette(QStringLiteral("声音")); QTest::qWait(20);
            QImage composed(size, QImage::Format_ARGB32_Premultiplied); composed.fill(Qt::transparent);
            window.render(&composed);
            QPainter painter(&composed);
            painter.drawPixmap(window.commandPalette_->pos() - window.pos(), window.commandPalette_->grab()); painter.end();
            QVERIFY(composed.save(images + QStringLiteral("/commands-%1x%2-offscreen.png").arg(size.width()).arg(size.height())));
            window.commandPalette_->hide();
        }
    }
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::MapSizeHostMenuAndActivationUseCurrentOwnedArchive() {
    namespace Size = War3Map::Services;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("map-size-host.w3x")));
    // 原创小资源只用于已持有的成员元数据；不授予模型、纹理、声音或游戏执行验收。
    const std::vector<War3Map::MapArchiveMember> members{
        {"war3map.w3u", MinimalUnitData(420)},
        {"Models\\Host.mdx", std::vector<std::byte>(13, std::byte{0x31})},
        {"Textures\\Host.blp", std::vector<std::byte>(7, std::byte{0x32})},
        {"Sounds\\Host.wav", std::vector<std::byte>(11, std::byte{0x33})},
        {"Scripts\\Host.lua", std::vector<std::byte>(9, std::byte{0x34})}};
    QVERIFY(War3Map::WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceBytes = ReadFile(source);
    QVERIFY(!sourceBytes.isEmpty());
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(source));
    QVERIFY(window.project_->archiveSession_);
    const auto sourceSha = window.project_->archiveSession_->SourceSha256();
    auto* undo = window.project_->AssetUndoStack(); QVERIFY(undo);
    const auto initialUndoCount = undo->count(), initialUndoIndex = undo->index();
    const auto revision = window.CommandContextRevision();
    const auto initial = window.QueryMapSizeAnalysis(revision);
    QVERIFY(initial.Succeeded() && initial.value);
    QCOMPARE(initial.value->contextRevision, revision);
    QCOMPARE(initial.value->sourceKind, Size::MapSizeSourceKind::ArchiveMembers);
    for (const auto [category, bytes] : std::array{
        std::pair{Size::MapSizeCategory::Model, std::uint64_t{13}},
        std::pair{Size::MapSizeCategory::Texture, std::uint64_t{7}},
        std::pair{Size::MapSizeCategory::Sound, std::uint64_t{11}},
        std::pair{Size::MapSizeCategory::Script, std::uint64_t{9}}}) {
        const auto index = static_cast<std::size_t>(category);
        QCOMPARE(initial.value->baseline.categories[index].knownBytes, std::optional<std::uint64_t>{bytes});
        QCOMPARE(initial.value->effective.categories[index].knownBytes, std::optional<std::uint64_t>{bytes});
    }
    const auto rowFor = [](const Size::MapSizeReport& report, const QString& path) -> std::optional<Size::MapSizeRow> {
        const auto key = War3Map::QtProject::EditorProject::AssetKey(path);
        for (const auto& row : report.top20)
            if (War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(row.currentPath)) == key) return row;
        return {};
    };
    const auto model = rowFor(*initial.value, QStringLiteral("Models\\Host.mdx"));
    const auto script = rowFor(*initial.value, QStringLiteral("Scripts\\Host.lua"));
    QVERIFY(model && script && model->navigationAllowed && script->navigationAllowed);
    QCOMPARE(model->originalLogicalBytes, std::optional<std::uint64_t>{13});
    QCOMPARE(model->currentLogicalBytes, std::optional<std::uint64_t>{13});
    const auto sourceMember = std::find_if(window.project_->archiveSession_->Members().begin(),
        window.project_->archiveSession_->Members().end(), [&](const auto& member) { return member.name == model->originalPath; });
    QVERIFY(sourceMember != window.project_->archiveSession_->Members().end());
    QCOMPARE(static_cast<std::uint64_t>(sourceMember->data.size()), *model->originalLogicalBytes);

    auto* action = window.findChild<QAction*>(QStringLiteral("mapSizeAnalysisAction"));
    QVERIFY(action && action->isEnabled()); action->trigger();
    QVERIFY(window.mapSizeDialog_); QTRY_VERIFY(window.mapSizeDialog_->isVisible());
    QVERIFY(!window.mapSizeDialog_->isModal());
    auto* top = window.mapSizeDialog_->findChild<QTableView*>(QStringLiteral("mapSizeTopMembers"));
    QVERIFY(top && top->model());
    QCOMPARE(top->model()->rowCount(), static_cast<int>(initial.value->top20.size()));
    window.ActivateMapSizeRow(*model, revision);
    QCOMPARE(window.CurrentModule(), std::optional<std::string>{"assets"});
    auto selected = window.assetsModule_->CaptureNavigationBookmark(); QVERIFY(selected);
    QCOMPARE(selected->value("current_path").toString(), QStringLiteral("Models\\Host.mdx"));
    QCOMPARE(undo->count(), initialUndoCount); QCOMPARE(undo->index(), initialUndoIndex);

    // 使用正式资源改名命令制造新的编辑修订；下面的查询/拒绝不能再增加这一个撤销条目。
    const auto renameProblem = window.assetsModule_->RenameAsset(QStringLiteral("Models\\Host.mdx"), QStringLiteral("Models\\Renamed.mdx"));
    QVERIFY2(renameProblem.isEmpty(), qPrintable(renameProblem));
    QCOMPARE(undo->count(), initialUndoCount + 1); QCOMPARE(undo->index(), initialUndoIndex + 1);
    const auto renamedRevision = window.CommandContextRevision(); QVERIFY(renamedRevision != revision);
    const auto renamed = window.QueryMapSizeAnalysis(renamedRevision); QVERIFY(renamed.Succeeded() && renamed.value);
    const auto currentModel = rowFor(*renamed.value, QStringLiteral("Models\\Renamed.mdx"));
    const auto currentScript = rowFor(*renamed.value, QStringLiteral("Scripts\\Host.lua"));
    QVERIFY(currentModel && currentScript && currentModel->navigationAllowed && currentScript->navigationAllowed);
    QCOMPARE(currentModel->origin, Size::MapSizeRowOrigin::Renamed);
    QCOMPARE(currentModel->identity, model->identity); QCOMPARE(currentModel->originalPath, model->originalPath);
    window.ActivateMapSizeRow(*currentScript, renamedRevision);
    const auto beforeRejected = window.assetsModule_->CaptureNavigationBookmark(); QVERIFY(beforeRejected);
    QCOMPARE(beforeRejected->value("current_path").toString(), QStringLiteral("Scripts\\Host.lua"));
    const auto rejectedUndoCount = undo->count(), rejectedUndoIndex = undo->index();
    const auto moduleBeforeRejected = window.CurrentModule();
    const auto stale = window.QueryMapSizeAnalysis(revision); QVERIFY(!stale.Succeeded());
    QVERIFY(std::any_of(stale.diagnostics.begin(), stale.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "MAP_SIZE_STALE_CONTEXT";
    }));
    window.ActivateMapSizeRow(*model, revision); // 修订失效。
    auto afterRejected = window.assetsModule_->CaptureNavigationBookmark();
    QVERIFY(afterRejected && *afterRejected == *beforeRejected);
    window.ActivateMapSizeRow(*model, renamedRevision); // 传当前修订也不能用旧成员行。
    afterRejected = window.assetsModule_->CaptureNavigationBookmark();
    QVERIFY(afterRejected && *afterRejected == *beforeRejected);
    QCOMPARE(window.CurrentModule(), moduleBeforeRejected);
    QCOMPARE(undo->count(), rejectedUndoCount); QCOMPARE(undo->index(), rejectedUndoIndex);
    window.ActivateMapSizeRow(*currentModel, renamedRevision);
    selected = window.assetsModule_->CaptureNavigationBookmark(); QVERIFY(selected);
    QCOMPARE(selected->value("current_path").toString(), QStringLiteral("Models\\Renamed.mdx"));
    QCOMPARE(undo->count(), rejectedUndoCount); QCOMPARE(undo->index(), rejectedUndoIndex);
    QCOMPARE(window.project_->archiveSession_->SourceSha256(), sourceSha);
    QVERIFY(window.project_->archiveSession_->VerifySourceUnchanged().Succeeded());
    QCOMPARE(ReadFile(source), sourceBytes);
    window.mapSizeDialog_->hide();
}

void NativeWorkbenchTests::AgentPresentationKeepsConversationAndOriginalEditorHost() {
    using War3Map::Agent::AgentConversationView;
    using War3Map::Agent::AgentWindow;
    using Mode = AgentWindow::PresentationMode;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto uiRoot = fixture.filePath(QStringLiteral("original-ui"));
    QVERIFY(WriteText(uiRoot + "/ui/config", "base\n"));
    QVERIFY(WriteText(uiRoot + "/ui/base/action.txt", "[DoNothing]\n"));
    auto catalog = War3Map::Trigger::UiCatalog::Load(NativePath(uiRoot));
    QVERIFY2(catalog.Succeeded(), qPrintable(LuaQtDiagnostics(catalog)));
    auto initial = War3Map::Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    QVERIFY(initial.Succeeded());
    const auto category = War3Map::Trigger::TriggerDirectoryEditor::AddCategory(*initial.value, "原创宿主回归");
    QVERIFY(category.Succeeded());
    const auto trigger = War3Map::Trigger::TriggerDocumentEditor::AddTrigger(*initial.value, *category.value, "0-0-1 原始业务");
    QVERIFY(trigger.Succeeded());
    const auto encoded = War3Map::Trigger::WtgWctCodec{}.Serialize(*initial.value, *catalog.value);
    QVERIFY(encoded.Succeeded());
    const auto source = NativePath(fixture.filePath(QStringLiteral("original-host-fixture.w3x")));
    QVERIFY(War3Map::WriteVerifiedArchive(source, std::vector<War3Map::MapArchiveMember>{
        {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct},
        {"war3map.w3u", MinimalUnitData(420)}}).Succeeded());
    const auto sourceHash = War3Map::Sha256File(source);
    QVERIFY(sourceHash.Succeeded());
    const auto agentViewCount = [] {
        const auto widgets = QApplication::allWidgets();
        return static_cast<int>(std::count_if(widgets.begin(), widgets.end(), [](QWidget* widget) {
            return qobject_cast<AgentWindow*>(widget) != nullptr;
        }));
    };
    const int previousAgentViews = agentViewCount();
    QPointer<AgentWindow> agentGuard;
    QPointer<AgentConversationView> conversationGuard;
    QPointer<QStackedWidget> presentationGuard;
    QPointer<QDockWidget> sidebarGuard;
    std::unique_ptr<QTemporaryDir> capture;
    const auto captureRoot = qEnvironmentVariable("FORGE_AI_WORKSPACE_CAPTURE_DIR");
    if (!captureRoot.isEmpty()) {
        QVERIFY(QDir().mkpath(captureRoot));
        capture = std::make_unique<QTemporaryDir>(QDir(captureRoot).filePath("native-host-XXXXXX"));
        QVERIFY(capture->isValid());
        capture->setAutoRemove(false);
    }
    {
        MainWindow window(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(window.UseUiDataRoot(NativePath(uiRoot)));
        QVERIFY(window.OpenMapArchivePath(source));
        auto* project = window.project_.get();
        auto* document = project->triggerDocument_.get();
        auto* mapDocument = project->mapDocument_.get();
        auto* objects = project->objectDocuments_[0].get();
        auto* undo = project->triggerUndoStack_;
        auto* originalCentral = window.centralWidget();
        const auto* originalDock = window.Dock();
        const auto* originalUndoGroup = window.UndoGroup();
        QVERIFY(document && mapDocument && objects && undo && originalCentral && originalDock);
        QCOMPARE(document->triggers.size(), std::size_t{1});
        const auto originalName = document->triggers[0].name.text;
        const std::string draftName = "0-0-1 保留的自然语言T编辑";
        const auto triggerId = document->triggers[0].id;
        QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RenameTrigger(*document, triggerId, draftName).Succeeded());
        undo->push(new RenameTriggerUndoCommand(document, triggerId, originalName, draftName, QStringLiteral("原编辑区 T 改名")));
        QCOMPARE(undo->count(), 1);
        const auto* undoCommand = undo->command(0);
        const auto revision = project->editRevision_.Revision();

        // 唯一显式空配置入口：不读取账户设置/凭据，不监听、不发模型请求。
        window.SetupAgentWorkspace(false);
        QVERIFY(window.aiWorkspace_ != nullptr);
        const auto* workspace = window.aiWorkspace_;
        window.resize(1280, 900);
        window.show(); // 由根在 offscreen 原生测试进程执行，不控制作者桌面。
        window.ShowAgentWindow();
        auto* agent = window.findChild<AgentWindow*>();
        QVERIFY(agent);
        agentGuard = agent;
        QCOMPARE(agent->Mode(), Mode::Page);
        QVERIFY(agent->minimumWidth() <= 360);
        auto* presentation = window.findChild<QStackedWidget*>(QStringLiteral("agentWorkspaceStack"));
        QVERIFY(presentation);
        presentationGuard = presentation;
        QCOMPARE(window.centralWidget(), static_cast<QWidget*>(presentation));
        QCOMPARE(presentation->currentWidget(), static_cast<QWidget*>(agent));
        QVERIFY(presentation->indexOf(originalCentral) >= 0);
        auto* prompt = agent->findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt"));
        auto* conversation = agent->findChild<AgentConversationView*>(QStringLiteral("agentConversation"));
        auto* models = agent->findChild<QComboBox*>(QStringLiteral("agentModel"));
        auto* send = agent->findChild<QPushButton*>(QStringLiteral("agentSend"));
        QVERIFY(prompt && conversation && models && send);
        conversationGuard = conversation;
        QCOMPARE(models->count(), 0);
        QVERIFY(!send->isEnabled());
        const QString draft = QStringLiteral("保留这份自然语言 T 草稿，不执行修改或保存。\n切换后仍可继续输入。");
        const QString message = QStringLiteral("这是原创宿主回归的会话展示，尚未调用模型或地图工具。");
        prompt->setPlainText(draft);
        agent->AppendEvent({{"type", "assistant_message"}, {"text", message}});
        // 合成事件仅测试真实呈现控件；不伪装成地图工具调用或保存证据。
        agent->AppendEvent({{"type", "tool_start"}, {"id", "host-presentation-fixture"},
            {"tool_name", "editor_query_trigger_graph"}, {"title", "原创工具展示记录"},
            {"details", QJsonObject{{"arguments", QJsonObject{{"table", "triggers"}, {"synthetic_ui_event", true}}}}}});
        agent->AppendEvent({{"type", "tool_end"}, {"id", "host-presentation-fixture"}, {"success", true},
            {"details", QJsonObject{{"result", QJsonObject{{"synthetic_ui_event", true}, {"saved", false}}}}}});
        QCOMPARE(conversation->ToolCount(), 1);
        auto* disclosure = agent->findChild<QToolButton*>(QStringLiteral("agentToolToggle"));
        auto* details = agent->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"));
        QVERIFY(disclosure && details && details->topLevelItemCount() > 0);
        if (!disclosure->isChecked()) disclosure->click();
        QVERIFY(disclosure->isChecked());
        auto* expandedDetail = details->topLevelItem(0);
        expandedDetail->setExpanded(true);
        const auto transcript = conversation->toPlainText();
        QVERIFY(transcript.contains(message));
        const auto statePreserved = [&] {
            return window.aiWorkspace_ == workspace && window.Dock() == originalDock && window.UndoGroup() == originalUndoGroup
                && project == window.project_.get() && document == project->triggerDocument_.get()
                && mapDocument == project->mapDocument_.get() && objects == project->objectDocuments_[0].get()
                && undo == project->triggerUndoStack_ && undo->count() == 1 && undo->index() == 1 && undo->command(0) == undoCommand
                && project->editRevision_.Revision() == revision && document->triggers[0].name.text == draftName
                && window.findChild<AgentWindow*>() == agent && agentViewCount() == previousAgentViews + 1
                && prompt == agent->findChild<QPlainTextEdit*>(QStringLiteral("agentPrompt")) && prompt->toPlainText() == draft
                && conversation == agent->findChild<AgentConversationView*>(QStringLiteral("agentConversation"))
                && conversation->toPlainText() == transcript && conversation->ToolCount() == 1
                && disclosure == agent->findChild<QToolButton*>(QStringLiteral("agentToolToggle")) && disclosure->isChecked()
                && details == agent->findChild<QTreeWidget*>(QStringLiteral("agentToolDetails"))
                && details->topLevelItem(0) == expandedDetail && expandedDetail->isExpanded()
                && presentation == window.centralWidget() && presentation->indexOf(originalCentral) >= 0;
        };
        const auto screenshot = [&](const char* name) {
            if (!capture) return true;
            QApplication::processEvents();
            const auto path = capture->filePath(QString::fromUtf8(name) + QStringLiteral(".png"));
            const bool written = window.grab().save(path);
            const auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock"));
            qInfo().noquote() << "Offscreen original-fixture AI host capture=" << path << "size=" << window.size()
                << "mode=" << static_cast<int>(agent->Mode()) << "agentWidth=" << agent->width()
                << "sidebarWidth=" << (sidebar ? sidebar->width() : 0)
                << "sidebarVisible=" << (sidebar && sidebar->isVisible()) << "; no author-visible/model/game acceptance";
            return written;
        };
        const auto layoutState = [&](int requestedWidth, int iteration) {
            const auto describe = [](const QString& name, const QWidget* widget) {
                if (!widget) return name + QStringLiteral(" absent");
                const auto size = widget->size();
                const auto minimum = widget->minimumSize();
                const auto hint = widget->minimumSizeHint();
                const auto maximum = widget->maximumSize();
                return QStringLiteral("%1 size=%2x%3 minimum=%4x%5 minimumSizeHint=%6x%7 maximum=%8x%9 visible=%10")
                    .arg(name).arg(size.width()).arg(size.height()).arg(minimum.width()).arg(minimum.height())
                    .arg(hint.width()).arg(hint.height()).arg(maximum.width()).arg(maximum.height()).arg(widget->isVisible());
            };
            return QStringList{QStringLiteral("requestedHostWidth=%1 mode=%2 iteration=%3")
                .arg(requestedWidth).arg(static_cast<int>(agent->Mode())).arg(iteration),
                describe(QStringLiteral("window"), &window), describe(QStringLiteral("originalEditor"), originalCentral),
                describe(QStringLiteral("sidebar"), window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock"))),
                describe(QStringLiteral("content"), window.findChild<QWidget*>(QStringLiteral("agentWorkspaceDockContent"))),
                describe(QStringLiteral("agent"), agent)}.join(QStringLiteral(" | "));
        };
        for (const int width : {1280, 1600, 1920}) {
            window.resize(width, 900);
            QApplication::processEvents();
            for (int iteration = 0; iteration < 2; ++iteration) {
                window.ShowAgentWindow();
                QCOMPARE(agent->Mode(), Mode::Page);
                QCOMPARE(presentation->currentWidget(), static_cast<QWidget*>(agent));
                QVERIFY2(statePreserved(), "Page replaced the editor or discarded conversation state");
                if (width == 1600 && iteration == 0) QVERIFY(screenshot("page-1600"));
                if (width == 1920 && iteration == 0) QVERIFY(screenshot("page-1920"));
                agent->PresentationModeRequested(Mode::Sidebar); // 实际窗口发意图，原宿主承接。
                if (width == 1280) {
                    // 原编辑区真实最小宽度无法再容纳 420 侧栏；回整页，不把侧栏挤成 360。
                    QCOMPARE(agent->Mode(), Mode::Page);
                    QCOMPARE(presentation->currentWidget(), static_cast<QWidget*>(agent));
                    QTRY_VERIFY2_WITH_TIMEOUT(window.width() == width, qPrintable(layoutState(width, iteration)), 2000);
                    QVERIFY(agent->minimumWidth() <= 360);
                    const auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock"));
                    QVERIFY(!sidebar || sidebar->isHidden());
                    QVERIFY2(statePreserved(), "Capacity fallback discarded existing state");
                    if (iteration == 0) QVERIFY(screenshot("page-1280-fallback"));
                } else {
                    QCOMPARE(agent->Mode(), Mode::Sidebar);
                    QCOMPARE(presentation->currentWidget(), originalCentral);
                    auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock"));
                    auto* content = window.findChild<QWidget*>(QStringLiteral("agentWorkspaceDockContent"));
                    QVERIFY(sidebar && content && content->layout());
                    if (sidebarGuard) QCOMPARE(sidebar, sidebarGuard.data());
                    sidebarGuard = sidebar;
                    QCOMPARE(sidebar->widget(), content);
                    QCOMPARE(content->layout()->count(), 1);
                    QCOMPARE(content->layout()->itemAt(0)->widget(), static_cast<QWidget*>(agent));
                    QCOMPARE(agent->parentWidget(), content);
                    QTRY_VERIFY2_WITH_TIMEOUT(window.width() == width, qPrintable(layoutState(width, iteration)), 2000);
                    QTRY_VERIFY2_WITH_TIMEOUT(sidebar->isVisible(), qPrintable(layoutState(width, iteration)), 2000);
                    QTRY_VERIFY2_WITH_TIMEOUT(sidebar->width() >= 400 && sidebar->width() <= 440, qPrintable(layoutState(width, iteration)), 2000);
                    QTRY_VERIFY2_WITH_TIMEOUT(agent->width() >= 400 && agent->width() <= 440, qPrintable(layoutState(width, iteration)), 2000);
                    QVERIFY2(statePreserved(), "Sidebar duplicated a view or discarded conversation state");
                    if (width == 1600 && iteration == 0) QVERIFY(screenshot("sidebar-1600"));
                    if (width == 1920 && iteration == 0) QVERIFY(screenshot("sidebar-1920"));
                }
                agent->PresentationModeRequested(Mode::Detached);
                QCOMPARE(agent->Mode(), Mode::Detached);
                QVERIFY(agent->isWindow());
                QCOMPARE(presentation->currentWidget(), originalCentral);
                QCOMPARE(presentation->indexOf(agent), -1);
                const auto* detachedContent = window.findChild<QWidget*>(QStringLiteral("agentWorkspaceDockContent"));
                if (detachedContent) QCOMPARE(detachedContent->layout()->count(), 0);
                QVERIFY2(statePreserved(), "Detached mode replaced the editor or discarded conversation state");
                window.HideAgentWorkspace();
                QCOMPARE(presentation->currentWidget(), originalCentral);
                QVERIFY(agent->isHidden());
                QVERIFY2(statePreserved(), "Returning to the editor discarded the existing session");
            }
        }
        // 已显示的侧栏随宿主变窄自动回到整页；覆盖实际异步 resize eventFilter。
        for (const int narrowWidth : {1280, 1080}) {
            window.resize(1600, 900);
            window.PresentAgentWorkspace(static_cast<int>(Mode::Sidebar));
            QCOMPARE(agent->Mode(), Mode::Sidebar);
            QVERIFY(sidebarGuard && sidebarGuard->isVisible());
            window.resize(narrowWidth, 900);
            QTRY_COMPARE_WITH_TIMEOUT(agent->Mode(), Mode::Page, 2000);
            QTRY_VERIFY2_WITH_TIMEOUT(window.width() == narrowWidth, qPrintable(layoutState(narrowWidth, 0)), 2000);
            QVERIFY(sidebarGuard->isHidden());
            QCOMPARE(presentation->currentWidget(), static_cast<QWidget*>(agent));
            QVERIFY2(statePreserved(), "Resizing a visible sidebar discarded existing state");
            // 已窄的宿主显式请求侧栏时也不能撑宽窗口。
            window.PresentAgentWorkspace(static_cast<int>(Mode::Sidebar));
            QCOMPARE(agent->Mode(), Mode::Page);
            QVERIFY(agent->minimumWidth() <= 360);
            QCOMPARE(presentation->currentWidget(), static_cast<QWidget*>(agent));
            QVERIFY2(statePreserved(), "Narrow-host fallback discarded existing state");
            if (narrowWidth == 1080) QVERIFY(screenshot("page-1080-fallback"));
        }
        window.ActivateModule("objects"); // 普通模块导航回原 Dock。
        QCOMPARE(presentation->currentWidget(), originalCentral);
        QVERIFY2(statePreserved(), "Activating a module replaced the original editor host");
        window.ShowAgentWindow();
        window.Navigate({"triggers", "trigger", QString::number(triggerId), {{"stableIdentity", true}}});
        QCOMPARE(presentation->currentWidget(), originalCentral);
        QVERIFY2(statePreserved(), "Object navigation discarded conversation or undo history");
        QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
        // 析构时仍有未保存 T 草稿；不得写源图或留下第二个 Agent 视图。
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(agentGuard.isNull() && conversationGuard.isNull() && presentationGuard.isNull() && sidebarGuard.isNull());
    QCOMPARE(agentViewCount(), previousAgentViews);
    QCOMPARE(War3Map::Sha256File(source).value, sourceHash.value);
}

void NativeWorkbenchTests::StartupPageSharesEditorAndAssistantHost() {
    using Mode = War3Map::Agent::AgentWindow::PresentationMode;
    MainWindow window(MainWindow::StartupMode::StartupProbe);
    auto* pages = qobject_cast<QStackedWidget*>(window.centralWidget());
    QVERIFY(pages && pages == window.workspacePages_);
    auto* startup = window.findChild<QWidget*>(QStringLiteral("startupPage"));
    QVERIFY(startup && pages->currentWidget() == startup);
    auto* originalDock = window.Dock();
    QVERIFY(pages->indexOf(originalDock) >= 0);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("openMapAction")));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("createMapAction")));
    // 无效或尚未提供的模块不能把启动页变成空编辑区。
    for (const War3Map::Shell::ModuleId id : {"missing", "ai"}) {
        window.ActivateModule(id);
        QCOMPARE(pages->currentWidget(), startup);
        QVERIFY(!window.startupDismissed_);
        window.Navigate({id, {}, {}, {}});
        QCOMPARE(pages->currentWidget(), startup);
        QVERIFY(!window.startupDismissed_);
    }
    const auto captureDirectory = qEnvironmentVariable("FORGE_BRAND_CAPTURE_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        QJsonArray captures;
        for (const QSize physical : {QSize{1920, 1080}, QSize{2560, 1440}}) {
            const auto dpr = window.devicePixelRatioF();
            const QSize requested(qRound(physical.width() / dpr), qRound(physical.height() / dpr));
            window.resize(requested); window.show(); QTest::qWait(50);
            const auto image = window.grab();
            const auto name = QStringLiteral("startup-%1x%2-dpr%3.png")
                .arg(physical.width()).arg(physical.height()).arg(dpr);
            QVERIFY(image.save(QDir(captureDirectory).filePath(name)));
            captures.append(QJsonObject{{"file", name}, {"dpr", dpr},
                {"requested_physical_width", physical.width()}, {"requested_physical_height", physical.height()},
                {"actual_physical_width", image.width()}, {"actual_physical_height", image.height()},
                {"actual_logical_width", window.width()}, {"actual_logical_height", window.height()},
                {"fixture", "actual-main-window-startup-without-user-config"}});
        }
        QVERIFY(WriteText(QDir(captureDirectory).filePath(QStringLiteral("startup-geometry.json")),
            QJsonDocument(captures).toJson(QJsonDocument::Indented)));
    }
    window.resize(1920, 1080); window.show();
    window.SetupAgentWorkspace(false); // 临时测试身份，无账户、密钥或网络请求。
    for (const auto mode : {Mode::Page, Mode::Sidebar, Mode::Detached}) {
        window.PresentAgentWorkspace(static_cast<int>(mode));
        auto* agent = window.findChild<War3Map::Agent::AgentWindow*>();
        QVERIFY(agent);
        QCOMPARE(agent->Mode(), mode);
        QCOMPARE(window.centralWidget(), pages);
        QCOMPARE(window.Dock(), originalDock);
        if (mode == Mode::Page) QCOMPARE(pages->currentWidget(), agent);
        else QCOMPARE(pages->currentWidget(), startup);
        window.HideAgentWorkspace();
        QCOMPARE(pages->currentWidget(), startup);
    }
    auto* agent = window.findChild<War3Map::Agent::AgentWindow*>();
    auto* assistantAction = window.findChild<QAction*>(QStringLiteral("agentWorkspaceAction"));
    QVERIFY(agent && assistantAction);
    const bool statusWasHidden = window.statusBar()->isHidden();
    // 实际助手发出形态切换意图，中间不隐藏；始终复用同一视图与公共stack。
    window.ShowAgentWindow();
    for (const auto mode : {Mode::Page, Mode::Sidebar, Mode::Detached, Mode::Page}) {
        agent->PresentationModeRequested(mode);
        QCoreApplication::processEvents();
        QCOMPARE(agent->Mode(), mode);
        QCOMPARE(window.findChild<War3Map::Agent::AgentWindow*>(), agent);
        QCOMPARE(window.centralWidget(), pages);
        QCOMPARE(window.Dock(), originalDock);
        QCOMPARE(window.findChildren<War3Map::Agent::AgentWindow*>().size(), 1);
        QVERIFY(window.AgentWorkspaceVisible());
        QVERIFY(assistantAction->isChecked());
        QCOMPARE(window.statusBar()->isHidden(), mode == Mode::Page || statusWasHidden);
        QCOMPARE(pages->currentWidget(), mode == Mode::Page ? static_cast<QWidget*>(agent) : startup);
        const auto* sidebar = window.findChild<QDockWidget*>(QStringLiteral("agentWorkspaceDock"));
        QCOMPARE(sidebar && sidebar->isVisible(), mode == Mode::Sidebar);
    }
    window.HideAgentWorkspace();
    QCOMPARE(pages->currentWidget(), startup);
    QVERIFY(!assistantAction->isChecked());
    QCOMPARE(window.statusBar()->isHidden(), statusWasHidden);
    window.StopAgentWorkspace();
    QCOMPARE(window.centralWidget(), pages);
    QCOMPARE(pages->currentWidget(), startup);
    window.PlaceModule("triggers", War3Map::Shell::Placement::Main());
    QCOMPARE(pages->currentWidget(), originalDock);
    QVERIFY(window.startupDismissed_);
    window.ShowBaseWorkspace(); // 重新刷新空项目不得盖住用户已经打开的模块。
    QCOMPARE(pages->currentWidget(), originalDock);
    window.ActivateModule("objects");
    QCOMPARE(pages->currentWidget(), originalDock);
    // 两个新隔离宿主分别证明首次有效显示意图，不伪造startupDismissed状态。
    for (const bool placement : {true, false}) {
        MainWindow revealWindow(MainWindow::StartupMode::StartupProbe);
        revealWindow.resize(1920, 1080); revealWindow.show();
        auto* revealPages = qobject_cast<QStackedWidget*>(revealWindow.centralWidget());
        auto* revealStartup = revealWindow.findChild<QWidget*>(QStringLiteral("startupPage"));
        auto* revealAction = revealWindow.findChild<QAction*>(QStringLiteral("agentWorkspaceAction"));
        QVERIFY(revealPages && revealStartup && revealAction);
        QCOMPARE(revealPages->currentWidget(), revealStartup);
        QVERIFY(!revealWindow.startupDismissed_);
        const bool revealStatusWasHidden = revealWindow.statusBar()->isHidden();
        revealWindow.SetupAgentWorkspace(false);
        revealWindow.ShowAgentWindow();
        auto* revealAgent = revealWindow.findChild<War3Map::Agent::AgentWindow*>();
        QVERIFY(revealAgent && revealAction->isChecked());
        QCOMPARE(revealPages->currentWidget(), static_cast<QWidget*>(revealAgent));
        QVERIFY(revealWindow.statusBar()->isHidden());
        QVERIFY(revealWindow.CurrentModule().has_value());
        if (placement) {
            // 重复当前模块不发CurrentModuleChanged，必须靠显式reveal退出AI页。
            revealWindow.PlaceModule(*revealWindow.CurrentModule(), War3Map::Shell::Placement::Main());
        } else {
            revealWindow.ActivateModule("objects");
        }
        QCoreApplication::processEvents();
        QCOMPARE(revealWindow.centralWidget(), revealPages);
        QCOMPARE(revealPages->currentWidget(), static_cast<QWidget*>(revealWindow.Dock()));
        QVERIFY(revealWindow.startupDismissed_);
        QVERIFY(revealAgent->isHidden());
        QVERIFY(!revealWindow.AgentWorkspaceVisible());
        QVERIFY(!revealAction->isChecked());
        QCOMPARE(revealWindow.statusBar()->isHidden(), revealStatusWasHidden);
        QCOMPARE(revealWindow.findChildren<War3Map::Agent::AgentWindow*>().size(), 1);
    }
    // 战役现已提供；首次进入模块复用原 Dock，但不能悄悄创建空战役会话。
    for (const bool navigate : {false, true}) {
        MainWindow campaignWindow(MainWindow::StartupMode::StartupProbe);
        auto* campaignPages = qobject_cast<QStackedWidget*>(campaignWindow.centralWidget());
        auto* campaignStartup = campaignWindow.findChild<QWidget*>(QStringLiteral("startupPage"));
        auto* campaignDock = campaignWindow.Dock();
        QVERIFY(campaignPages && campaignStartup && campaignWindow.campaignModule_);
        QCOMPARE(campaignPages->currentWidget(), campaignStartup);
        QVERIFY(!campaignWindow.startupDismissed_);
        QVERIFY(!campaignWindow.campaignModule_->Session());
        if (navigate) campaignWindow.Navigate({"campaign", {}, {}, {}});
        else campaignWindow.ActivateModule("campaign");
        QCOMPARE(campaignWindow.centralWidget(), campaignPages);
        QCOMPARE(campaignWindow.Dock(), campaignDock);
        QCOMPARE(campaignPages->currentWidget(), static_cast<QWidget*>(campaignDock));
        QVERIFY(campaignWindow.CurrentModule() == "campaign");
        QVERIFY(campaignWindow.startupDismissed_);
        QVERIFY(!campaignWindow.campaignModule_->Session());
    }
}

void NativeWorkbenchTests::RecentMapsUserOriginAndPrivateRestore() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto source = NativePath(fixture.filePath(QStringLiteral("user-map.w3x")));
    const auto project = NativePath(fixture.filePath(QStringLiteral("user-map.forge")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    const auto sourceBytes = ReadFile(source);
    const auto storeDirectory = fixture.filePath(QStringLiteral("recent"));
    MainWindow window(MainWindow::StartupMode::Normal, nullptr, storeDirectory);
    QVERIFY(window.recentMapsStore_);
    const auto storePath = NativePath(window.recentMapsStore_->FilePath());
    QVERIFY(window.OpenMapArchivePath(source)); // 外部/验收入口保持原行为。
    QVERIFY(!std::filesystem::exists(storePath));
    QVERIFY(window.OpenUserMapArchivePath(source));
    const auto history = window.recentMapsStore_->Read();
    QVERIFY(history.succeeded);
    QCOMPARE(history.entries.size(), 1);
    QCOMPARE(history.entries.first().path, QFileInfo(QString::fromStdWString(source.wstring())).canonicalFilePath());
    const auto before = ReadFile(storePath);
    QVERIFY(!before.isEmpty());
    QVERIFY(!window.OpenUserMapArchivePath(NativePath(fixture.filePath(QStringLiteral("missing.w3x")))));
    QCOMPARE(ReadFile(storePath), before);
    QVERIFY(window.SaveEditorProjectToPath(project));
    MainWindow restored(MainWindow::StartupMode::Normal, nullptr, storeDirectory);
    QVERIFY(restored.OpenEditorProjectPath(project));
    QCOMPARE(ReadFile(storePath), before); // 工程恢复不登记 MRU。
    // 原地图仍与工程基线逐字节相同：直接打开原地图（保存写回原位置），而不是工程内私有基线。
    QCOMPARE(restored.project_->sourceArchivePath_, source);
    MainWindow probe(MainWindow::StartupMode::HeadlessProbe, nullptr, storeDirectory);
    QVERIFY(probe.OpenUserMapArchivePath(source));
    QCOMPARE(ReadFile(storePath), before); // Probe 即使显式 helper 也不登记。
    QObject cancellation;
    bool cancelled = false;
    QTimer::singleShot(0, &cancellation, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                cancelled = true;
                dialog->reject();
            }
        }
    });
    window.OpenMapArchive();
    QVERIFY(cancelled);
    QCOMPARE(ReadFile(storePath), before);
    QCOMPARE(ReadFile(source), sourceBytes);
}

void NativeWorkbenchTests::RecentMapsShareSurfacesAndPreserveAllPinned() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto captureDirectory = qEnvironmentVariable("FORGE_RECENT_MAPS_CAPTURE_DIR");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir::isAbsolutePath(captureDirectory));
        QVERIFY(QDir{}.mkpath(captureDirectory));
    }
    QJsonObject capture;
    const auto rectJson = [](const QRect& rect) {
        return QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()};
    };
    const auto captureWidget = [&](const QString& name, QWidget* widget, QScrollArea* scroll) {
        if (captureDirectory.isEmpty()) return true;
        const auto path = QDir(captureDirectory).filePath(name + QStringLiteral(".png"));
        if (QFileInfo::exists(path)) return false; // 每轮新目录，保留旧身份截图。
        const auto pixels = widget->grab();
        if (pixels.isNull() || !pixels.save(path, "PNG")) return false;
        QJsonObject geometry{{QStringLiteral("file"), path},
            {QStringLiteral("devicePixelRatio"), widget->devicePixelRatioF()},
            {QStringLiteral("logicalRect"), rectJson(widget->rect())},
            {QStringLiteral("globalRect"), rectJson(QRect(widget->mapToGlobal(QPoint{}), widget->size()))},
            {QStringLiteral("pixelWidth"), pixels.width()}, {QStringLiteral("pixelHeight"), pixels.height()},
            {QStringLiteral("fontFamily"), widget->font().family()}};
        if (scroll) {
            geometry.insert(QStringLiteral("viewportRect"), rectJson(scroll->viewport()->rect()));
            geometry.insert(QStringLiteral("horizontalMaximum"), scroll->horizontalScrollBar()->maximum());
            geometry.insert(QStringLiteral("verticalMaximum"), scroll->verticalScrollBar()->maximum());
            geometry.insert(QStringLiteral("verticalValue"), scroll->verticalScrollBar()->value());
        }
        capture.insert(name, geometry);
        return true;
    };
    const auto source = NativePath(fixture.filePath(QStringLiteral("surface.w3x")));
    QVERIFY(MakeArchive(source, 420).Succeeded());
    MainWindow window(MainWindow::StartupMode::Normal, nullptr, fixture.filePath(QStringLiteral("recent")));
    QVERIFY(window.startupRecentMaps_);
    QVERIFY(window.menuRecentMaps_);
    QVERIFY(window.OpenUserMapArchivePath(source));
    window.ShowRecentMapsManager();
    QVERIFY(window.managedRecentMaps_);
    const auto count = [](War3Map::Shell::RecentMapsList* list) {
        return list->findChildren<QToolButton*>(QStringLiteral("recentMapOpen")).size();
    };
    for (auto* list : {window.startupRecentMaps_.data(), window.menuRecentMaps_.data(), window.managedRecentMaps_.data()})
        QCOMPARE(count(list), 1);
    auto* pin = window.menuRecentMaps_->findChild<QToolButton*>(QStringLiteral("recentMapPin"));
    QVERIFY(pin);
    pin->click();
    const auto pinned = window.recentMapsStore_->Read();
    QVERIFY(pinned.succeeded && pinned.entries.first().pinned);
    for (auto* list : {window.startupRecentMaps_.data(), window.menuRecentMaps_.data(), window.managedRecentMaps_.data()}) {
        auto* currentPin = list->findChild<QToolButton*>(QStringLiteral("recentMapPin"));
        QVERIFY(currentPin);
        QCOMPARE(currentPin->text(), QStringLiteral("取消固定"));
    }
    const auto before = ReadFile(NativePath(window.recentMapsStore_->FilePath()));
    {
        QLockFile held(window.recentMapsStore_->LockPath());
        held.setStaleLockTime(0);
        QVERIFY(held.tryLock(0));
        auto* remove = window.managedRecentMaps_->findChild<QToolButton*>(QStringLiteral("recentMapRemove"));
        QVERIFY(remove);
        remove->click();
        QCOMPARE(ReadFile(NativePath(window.recentMapsStore_->FilePath())), before);
        QCOMPARE(count(window.managedRecentMaps_), 1); // GUI 0ms 锁失败保旧行并显示原因。
        QVERIFY(!window.recentMapsMessage_.isEmpty());
    }
    QVERIFY(QFile::remove(QString::fromStdWString(source.wstring())));
    window.RefreshRecentMaps();
    for (auto* list : {window.startupRecentMaps_.data(), window.menuRecentMaps_.data(), window.managedRecentMaps_.data()}) {
        auto* open = list->findChild<QToolButton*>(QStringLiteral("recentMapOpen"));
        QVERIFY(open && !open->isEnabled());
        QVERIFY(list->findChild<QToolButton*>(QStringLiteral("recentMapPin"))->isEnabled());
        QVERIFY(list->findChild<QToolButton*>(QStringLiteral("recentMapRemove"))->isEnabled());
    }
    window.managedRecentMaps_->findChild<QToolButton*>(QStringLiteral("recentMapRemove"))->click();
    for (auto* list : {window.startupRecentMaps_.data(), window.menuRecentMaps_.data(), window.managedRecentMaps_.data()})
        QCOMPARE(count(list), 0);
    window.ResetProjectState();
    for (int i = 0; i < 10; ++i) {
        const auto path = fixture.filePath(QStringLiteral("pinned-%1.w3x").arg(i));
        QVERIFY(WriteText(path, "owned history fixture"));
        QVERIFY(window.recentMapsStore_->RecordOpened(path).succeeded);
        QVERIFY(window.recentMapsStore_->SetPinned(path, true).succeeded);
    }
    const auto allPinned = ReadFile(NativePath(window.recentMapsStore_->FilePath()));
    const auto eleventh = NativePath(fixture.filePath(QStringLiteral("opened-but-not-recorded.w3x")));
    QVERIFY(MakeArchive(eleventh, 731).Succeeded());
    QVERIFY(window.OpenUserMapArchivePath(eleventh));
    QVERIFY(window.project_->sourceArchivePath_ == eleventh);
    QCOMPARE(ReadFile(NativePath(window.recentMapsStore_->FilePath())), allPinned);
    QCOMPARE(window.recentMapRows_.size(), 10);
    QVERIFY(window.recentMapsMessage_.contains(QStringLiteral("未加入")));
    window.recentMapsManager_->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    window.recentMapsMenu_->popup(QPoint(24, 24));
    QTest::qWait(10);
    auto* menuScroll = window.recentMapsMenu_->findChild<QScrollArea*>(QStringLiteral("recentMapsMenuScroll"));
    QVERIFY(menuScroll);
    QCOMPARE(count(window.menuRecentMaps_), 10);
    QVERIFY(menuScroll->verticalScrollBar()->maximum() > 0);
    QCOMPARE(menuScroll->horizontalScrollBar()->maximum(), 0);
    menuScroll->verticalScrollBar()->setValue(menuScroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    const auto rows = window.menuRecentMaps_->findChildren<QWidget*>(QStringLiteral("recentMapRow"));
    QCOMPARE(rows.size(), 10);
    QJsonObject lastControls;
    for (const auto& name : {QStringLiteral("recentMapPin"), QStringLiteral("recentMapRemove")}) {
        auto* button = rows.last()->findChild<QToolButton*>(name);
        QVERIFY(button);
        const QRect buttonRect(button->mapTo(menuScroll->viewport(), QPoint{}), button->size());
        QVERIFY2(menuScroll->viewport()->rect().contains(buttonRect), qPrintable(name + QStringLiteral(" 最后一条操作必须完整落入 viewport")));
        lastControls.insert(name, rectJson(buttonRect));
    }
    capture.insert(QStringLiteral("lastMenuControls"), lastControls);
    QVERIFY(window.recentMapsMenu_->screen());
    const auto availableHeight = window.recentMapsMenu_->screen()->availableGeometry().height();
    QVERIFY2(window.recentMapsMenu_->height() <= availableHeight, "十条最近地图菜单必须在当前屏幕高度内滚动");
    QVERIFY(menuScroll->height() <= std::max(80, std::min(560, availableHeight - 160)));
    QVERIFY(captureWidget(QStringLiteral("menu-10"), window.recentMapsMenu_, menuScroll));
    window.recentMapsMenu_->close();
    if (!captureDirectory.isEmpty()) {
        window.ResetProjectState();
        window.startupDismissed_ = false;
        window.ShowBaseWorkspace();
        window.resize(1280, 800);
        window.show();
        QTest::qWait(10);
        auto* startupScroll = window.startupPage_->findChild<QScrollArea*>(QStringLiteral("startupScroll"));
        QVERIFY(startupScroll);
        QCOMPARE(count(window.startupRecentMaps_), 10);
        QCOMPARE(window.startupPage_->findChildren<QScrollArea*>().size(), 1); // 原单层滚动。
        QVERIFY(captureWidget(QStringLiteral("startup-10"), window.startupPage_, startupScroll));
        window.ShowRecentMapsManager();
        QTest::qWait(10);
        auto* managerScroll = window.recentMapsManager_->findChild<QScrollArea*>();
        QVERIFY(managerScroll);
        QCOMPARE(count(window.managedRecentMaps_), 10);
        QVERIFY(captureWidget(QStringLiteral("manager-10"), window.recentMapsManager_, managerScroll));
        const auto geometryPath = QDir(captureDirectory).filePath(QStringLiteral("geometry.json"));
        QVERIFY(!QFileInfo::exists(geometryPath));
        capture.insert(QStringLiteral("storeIsIsolatedFixture"), true);
        capture.insert(QStringLiteral("historyCount"), 10);
        QVERIFY(WriteText(geometryPath, QJsonDocument(capture).toJson(QJsonDocument::Indented)));
        window.recentMapsManager_->close();
    }
}

void NativeWorkbenchTests::ArchiveWorkspaceOwnsOnlyDerivedDirectories() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto first = NativePath(fixture.filePath(QStringLiteral("first.w3x")));
    const auto second = NativePath(fixture.filePath(QStringLiteral("second.w3x")));
    const auto project = NativePath(fixture.filePath(QStringLiteral("owned.forge")));
    QVERIFY(MakeArchive(first, 420).Succeeded());
    QVERIFY(MakeArchive(second, 731).Succeeded());
    const auto sourceBytes = ReadFile(first);
    std::filesystem::path finalWorkspace;
    {
        MainWindow window(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(window.OpenMapArchivePath(first));
        QVERIFY(window.project_->mapWorkspace_ && window.project_->mapWorkspace_->isValid());
        const auto firstWorkspace = window.project_->projectRoot_;
        QVERIFY(!window.OpenWorkspacePath(firstWorkspace));
        QVERIFY(!window.OpenWorkspacePath(firstWorkspace / "table"));
        QVERIFY(window.project_->projectRoot_ == firstWorkspace);
        QVERIFY(std::filesystem::is_regular_file(firstWorkspace / "table" / "unit.ini"));
        QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 420);
        QVERIFY(window.OpenMapArchivePath(second));
        QVERIFY(!std::filesystem::exists(firstWorkspace));
        const auto secondWorkspace = window.project_->projectRoot_;
        QVERIFY(window.SaveEditorProjectToPath(project));
        // 原地图不在时恢复只能打开工程内私有基线；本用例检查这些私有派生目录的归属。
        QVERIFY(std::filesystem::remove(second));
        MainWindow restored(MainWindow::StartupMode::HeadlessProbe);
        QVERIFY(restored.OpenEditorProjectPath(project));
        QVERIFY(restored.project_->mapWorkspace_ && restored.project_->restoredFiles_);
        const auto restoredMap = restored.project_->projectRoot_;
        const auto restoredContainer = NativePath(restored.project_->restoredFiles_->path());
        QVERIFY(std::filesystem::is_regular_file(restoredMap / "table" / "unit.ini"));
        QVERIFY(std::filesystem::is_regular_file(restored.project_->sourceArchivePath_));
        QVERIFY(restoredMap != restoredContainer);
        const auto privateSource = restored.project_->sourceArchivePath_;
        const auto privateSourceBytes = ReadFile(privateSource);
        QVERIFY(!privateSourceBytes.isEmpty());
        QVERIFY(!restored.OpenWorkspacePath(restoredMap));
        QVERIFY(!restored.OpenWorkspacePath(restoredContainer));
        QVERIFY(!restored.OpenMapArchivePath(privateSource));
        QVERIFY(restored.project_->projectRoot_ == restoredMap);
        QVERIFY(restored.project_->sourceArchivePath_ == privateSource);
        QCOMPARE(ReadFile(privateSource), privateSourceBytes);
        QVERIFY(std::filesystem::is_regular_file(restoredMap / "table" / "unit.ini"));
        restored.ResetProjectState();
        QVERIFY(!std::filesystem::exists(restoredMap));
        QVERIFY(!std::filesystem::exists(restoredContainer));
        const auto authorDirectory = NativePath(fixture.filePath(QStringLiteral("author-workspace")));
        std::filesystem::create_directories(authorDirectory / "table");
        QVERIFY(WriteText(QString::fromStdWString((authorDirectory / "table" / "unit.ini").wstring()), "[hfoo]\n_parent=\"hfoo\"\n"));
        QVERIFY(window.OpenWorkspacePath(authorDirectory));
        QVERIFY(!window.project_->mapWorkspace_);
        QVERIFY(!std::filesystem::exists(secondWorkspace));
        window.ResetProjectState();
        QVERIFY(std::filesystem::is_regular_file(authorDirectory / "table" / "unit.ini"));
        QVERIFY(window.OpenMapArchivePath(first));
        finalWorkspace = window.project_->projectRoot_;
        QVERIFY(std::filesystem::exists(finalWorkspace));
    }
    QVERIFY(!std::filesystem::exists(finalWorkspace));
    QCOMPARE(ReadFile(first), sourceBytes);
}

void NativeWorkbenchTests::ArchiveWorkspaceCancellationAndFailureKeepActiveOwner() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto first = NativePath(fixture.filePath(QStringLiteral("active.w3x")));
    const auto second = NativePath(fixture.filePath(QStringLiteral("cancelled.w3x")));
    const auto invalid = NativePath(fixture.filePath(QStringLiteral("invalid.w3x")));
    QVERIFY(MakeArchive(first, 420).Succeeded());
    QVERIFY(MakeArchive(second, 731).Succeeded());
    QVERIFY(WriteText(QString::fromStdWString(invalid.wstring()), "not an MPQ"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(first));
    const auto active = window.project_->projectRoot_;
    const auto directories = [] {
        return QDir(QDir::tempPath()).entryList({QStringLiteral("War3MapEditor64-map-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    };
    const auto before = directories();
    QVERIFY(ChangeHealth(window, 777));
    QObject cancellation;
    bool cancelled = false;
    QTimer::singleShot(0, &cancellation, [&] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            cancelled = true;
            prompt->reject();
        }
    });
    QVERIFY(!window.OpenMapArchivePath(second));
    QVERIFY(cancelled);
    QCOMPARE(directories(), before); // 切换取消只回收本次新建目录。
    QVERIFY(window.project_->projectRoot_ == active);
    QVERIFY(std::filesystem::exists(active));
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 777);
    QVERIFY(!window.OpenMapArchivePath(invalid));
    QCOMPARE(directories(), before);
    QVERIFY(window.project_->projectRoot_ == active);
    window.ResetProjectState();
    QVERIFY(!std::filesystem::exists(active));
}

void NativeWorkbenchTests::ShellFitsCommonWindowSizes() {
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    auto* search = window.findChild<QLineEdit*>("globalSearch");
    QVERIFY(search);
    QToolButton* saveButton = nullptr;
    for (auto* button : window.Header()->findChildren<QToolButton*>("headerAction")) {
        if (button->defaultAction() == window.saveMapInPlaceAction_) saveButton = button;
    }
    QVERIFY(saveButton);
    QCOMPARE(saveButton->defaultAction(), window.saveMapInPlaceAction_);
    for (const QString& module : {QStringLiteral("triggers"), QStringLiteral("objects"),
                                  QStringLiteral("variables"), QStringLiteral("assets")}) {
        window.ActivateModule(module.toStdString());
        window.resize(1280, 800);
        window.show();
        QTest::qWait(10);
        const auto describe = [&window, &module]() {
            QString text = module + QStringLiteral(": window min %1").arg(window.minimumSizeHint().width());
            text += QStringLiteral(", header %1").arg(window.Header()->minimumSizeHint().width());
            text += QStringLiteral(", dock %1").arg(window.centralWidget()->minimumSizeHint().width());
            text += QStringLiteral(", status %1").arg(window.statusBar()->minimumSizeHint().width());
            return text;
        };
        qInfo("%s", qPrintable(describe()));
        QVERIFY2(window.minimumSizeHint().width() <= 1280, qPrintable(describe()));
        QVERIFY2(window.centralWidget()->width() <= window.width(), qPrintable(describe()));
        QVERIFY2(search->width() >= 130 && search->width() <= 230, qPrintable(describe()));
        QVERIFY2(saveButton->width() >= saveButton->sizeHint().width(), qPrintable(describe()));
    }
    window.ActivateModule("assets");
    window.resize(1080, 800);
    window.show();
    QTest::qWait(10);
    qInfo() << "1080 header width/min:" << window.Header()->width() << window.Header()->minimumSizeHint().width()
            << "search width:" << search->width();
    for (const QString& name : {QStringLiteral("projectIdentity"), QStringLiteral("moduleSwitcher")}) {
        if (auto* widget = window.findChild<QWidget*>(name)) qInfo() << name << "width/min:" << widget->width() << widget->minimumSizeHint().width();
    }
    for (auto* button : window.Header()->findChildren<QToolButton*>("headerAction")) {
        qInfo() << "action:" << button->defaultAction()->text() << "width/hint/minWidth/x/right:" << button->width() << button->sizeHint().width() << button->minimumWidth() << button->x() << button->geometry().right();
    }
    QCOMPARE(window.width(), 1080);
    QVERIFY(window.Header()->width() <= window.width());
    QVERIFY(search->width() >= 130 && search->width() <= 230);
    QVERIFY(saveButton->width() >= saveButton->sizeHint().width());

    auto* toolbar = window.Header()->findChild<War3Map::UiKit::ResponsiveToolBar*>("headerActions");
    QVERIFY(toolbar);
    auto* switcher = window.Header()->findChild<QWidget*>("moduleSwitcher");
    QVERIFY(switcher);
    const auto output = qEnvironmentVariable("AUD_SHELL_RESPONSIVE_CAPTURE_DIR");
    if (!output.isEmpty()) { QVERIFY(QDir::isAbsolutePath(output)); QVERIFY(QDir().mkpath(output)); }
    const auto previousTheme = window.themeManager_->CurrentThemeId();
    const auto restoreTheme = qScopeGuard([&] { window.themeManager_->ApplyTheme(previousTheme); });
    for (const QString& theme : {QStringLiteral("graphite"), QStringLiteral("paper")}) {
        window.themeManager_->ApplyTheme(theme);
        for (const auto* module : {"triggers", "objects", "variables", "sounds", "assets", "manager",
                 "map-settings", "entities", "campaign", "world"}) {
            window.ActivateModule(module);
            for (const int width : {1920, 1600, 1366, 1280, 1024}) {
                window.resize(width, 1080); QTest::qWait(10); toolbar->Refresh();
                // 校验实际生产窗口，不能由独立HeaderBar的宽度测试代替。
                QCOMPARE(window.width(), width);
                QVERIFY(window.minimumWidth() <= width);
                QVERIFY(window.Header()->width() <= width);
                QVERIFY(window.centralWidget()->width() <= width);
                for (const QWidget* parent : {static_cast<QWidget*>(window.Header()), static_cast<QWidget*>(toolbar), switcher}) {
                    QList<QRect> rectangles;
                    for (const auto* child : parent->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
                        if (child->isHidden() || child->isWindow()) continue;
                        const QRect rectangle(child->mapTo(parent, QPoint{}), child->size());
                        const auto description = QStringLiteral("%1/%2/%3 %4=%5x%6 child %7=(%8,%9 %10x%11)")
                            .arg(theme, QString::fromLatin1(module)).arg(width).arg(parent->objectName())
                            .arg(parent->width()).arg(parent->height()).arg(child->objectName())
                            .arg(rectangle.x()).arg(rectangle.y()).arg(rectangle.width()).arg(rectangle.height());
                        QVERIFY2(parent->rect().contains(rectangle), qPrintable(description));
                        for (const auto& previous : rectangles) QVERIFY(!previous.intersects(rectangle));
                        rectangles.push_back(rectangle);
                    }
                }
                for (const auto* button : window.Header()->findChildren<QToolButton*>("headerAction")) {
                    if (button->isHidden()) QVERIFY(toolbar->OverflowMenu()->actions().contains(button->defaultAction()));
                    else QVERIFY(button->width() >= button->sizeHint().width());
                }
                if (!output.isEmpty() && (std::string_view(module) == "triggers" || std::string_view(module) == "objects"
                        || std::string_view(module) == "world")) {
                    const auto stem = QStringLiteral("%1-%2-%3-dpr%4").arg(theme, QString::fromLatin1(module)).arg(width).arg(window.devicePixelRatioF());
                    const auto file = QDir(output).filePath(stem + QStringLiteral(".png"));
                    QVERIFY(!QFileInfo::exists(file));
                    const auto pixels = window.grab(); QVERIFY(!pixels.isNull()); QVERIFY(pixels.save(file));
                    QCOMPARE(pixels.width(), qRound(width * window.devicePixelRatioF()));
                    QCOMPARE(pixels.height(), qRound(window.height() * window.devicePixelRatioF()));
                    const QJsonObject receipt{{"scope", "production-main-window-headless-probe"}, {"map_loaded", false},
                        {"theme", theme}, {"module", module}, {"capture_method", "QWidget::grab"}, {"resampled", false},
                        {"logical_width", window.width()}, {"logical_height", window.height()}, {"dpr", window.devicePixelRatioF()},
                        {"physical_width", pixels.width()}, {"physical_height", pixels.height()}};
                    QVERIFY(WriteText(QDir(output).filePath(stem + QStringLiteral(".json")), QJsonDocument(receipt).toJson(QJsonDocument::Indented)));
                }
            }
        }
    }
}

// Qt GUI 默认日志可能没有 stdout；保留 Qt 原断言，同时逐失败发布有界阶段/原因。
#define REAL_VERIFY2(condition, message) do { \
    const bool observedPassed = static_cast<bool>(condition); \
    if (!observedPassed) ObserveFailure(receipt, QString::fromUtf8(message)); \
    QVERIFY2(observedPassed, message); \
} while (false)
#define REAL_VERIFY(condition) REAL_VERIFY2(condition, #condition)
#define REAL_COMPARE(actual, expected) do { \
    const auto observedActual = (actual); const auto observedExpected = (expected); \
    if (!(observedActual == observedExpected)) ObserveFailure(receipt, \
        QStringLiteral(#actual " == " #expected ": actual=") + ObservedValue(observedActual) \
            + QStringLiteral(", expected=") + ObservedValue(observedExpected)); \
    QCOMPARE(observedActual, observedExpected); \
} while (false)


void NativeWorkbenchTests::TriggerReplaceOwnedLargeMapMainHost256() {
    using namespace War3Map;
    using namespace War3Map::Tests::TriggerReplaceReal;
    using War3Map::Modules::TriggerReplaceDialog;
    if (!qApp->property("triggerReplaceMainHostExplicit").toBool()) QSKIP("Use the registered single-method real-host entry; ordinary NativeWorkbench remains isolated from real inputs.");
    const auto map_ = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP"), ui_ = qEnvironmentVariable("FORGE_YDWE_ROOT");
    if (map_.isEmpty() || ui_.isEmpty()) QSKIP("This actual MainWindow case requires explicit readonly large-map and catalog sources.");
    Receipt receipt;
    receipt.data.insert("evidence", "real MainWindow HeadlessProbe/command/Chrome/Shell actions and shared replacement service, CPU only");
    receipt.data.insert("candidate", "main-host09-actual-256");
    receipt.data.insert("host_scope", "Headless MainWindow; actual observer and recovery scheduling included; Normal recovery persistence excluded");
    receipt.data.insert("owned_temporary_member_projection_created", true);
    receipt.data.insert("cold_open_in_foreground_gate", false);
    receipt.data.insert("first_search_scope", "actual Main bound PrepareTriggerReplaceSources + Query + Populate + one Qt event drain");
    receipt.data.insert("snapshot_copy_scope", "separate public Strings Snapshot; not a decomposition of first Search");
    receipt.data.insert("rss_initial_bytes", Rss());
    const auto needle_ = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_QUERY_NEEDLE");
    const auto target_ = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_TARGET_VARIABLE");
    const auto expectedSha_ = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_MAP_SHA256");
    double limitMs_ = 1000;
    const auto requestedLimit = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_MAX_OPERATION_MS");
    if (!requestedLimit.isEmpty()) {
        bool ok = false; limitMs_ = requestedLimit.toDouble(&ok);
        REAL_VERIFY2(ok && std::isfinite(limitMs_) && limitMs_ > 0 && limitMs_ <= 1000, "An explicit latency gate may only tighten 1000ms.");
    }
    REAL_VERIFY2(qEnvironmentVariable("FORGE_TRIGGER_REPLACE_SOURCE_VARIABLE").isEmpty(), "The actual256 ParameterValue case does not filter one source variable.");
    REAL_VERIFY(needle_.isEmpty() || ExactNeedle(needle_.toUtf8().toStdString()));
    REAL_VERIFY(expectedSha_.isEmpty() || (expectedSha_.size() == 64 && std::all_of(expectedSha_.begin(), expectedSha_.end(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    })));
    receipt.data.insert("operation_limit_ms", limitMs_);
    receipt.data.insert("timing_gate", "maximum of actual Main command/query/GUIpreview/Apply/ShellUndoRedo plus event drain and heartbeat <=1000ms; owned-worker duration separate");
    REAL_VERIFY(qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"));
    REAL_VERIFY(QGuiApplication::platformName() == QStringLiteral("offscreen"));
    REAL_VERIFY(QSettings::defaultFormat() == QSettings::IniFormat);
    const QFileInfo source(map_), uiSource(ui_);
    REAL_VERIFY2(source.isAbsolute() && source.isFile() && source.size() >= 300ll * 1024 * 1024, "Explicit readonly owned map must be absolute and at least300MiB.");
    REAL_VERIFY2(uiSource.isAbsolute() && uiSource.isDir(), "Explicit readonly catalog root must be an existing absolute directory.");
    receipt.data.insert("map_bytes", QString::number(source.size()));
    receipt.data.insert("stage", "actual-main-construction-and-readonly-open");
    QElapsedTimer setup; setup.start();
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    auto& project = *window.project_;
    const auto noSideEffects = [&] {
        return window.worldModule_ && window.worldModule_->Viewport() == nullptr && !project.savingMap_
            && !window.writingRecovery_ && !window.recoveryWorker_ && !window.recoveryJob_
            && project.recoveryTimer_ && !project.recoveryTimer_->isActive() && !window.recentMapsStore_
            && !window.feedbackClient_ && !window.updateClient_;
    };
    bool sideEffectsClean = noSideEffects();
    REAL_VERIFY(sideEffectsClean);
    window.show(); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    REAL_VERIFY(noSideEffects());
    REAL_VERIFY(window.UseUiDataRoot(std::filesystem::path(ui_.toStdWString())));
    REAL_VERIFY(window.OpenMapArchivePath(std::filesystem::path(map_.toStdWString())));
    receipt.data.insert("actual_main_cold_open_ms_outside_ui_gate", Milliseconds(setup));
    REAL_VERIFY(noSideEffects());
    REAL_VERIFY(project.archiveSession_ && project.triggerDocument_ && project.uiCatalog_ && project.mapEdits_ && project.triggerUndoStack_);
    REAL_VERIFY(project.triggerDocument_->HasTrustedSourceSeal());
    REAL_VERIFY(project.editTrackingReady_ && !project.editTrackingFailed_ && !project.sourceChangeSuspected_ && !project.sourceChangeConfirmed_);
    REAL_VERIFY(window.CurrentModule() == std::optional<War3Map::Shell::ModuleId>{"triggers"});
    REAL_VERIFY(window.UndoGroup()->activeStack() == project.triggerUndoStack_);
    REAL_COMPARE(project.triggerUndoStack_->count(), 0);
    receipt.data.insert("map_sha256", Utf8Display(project.archiveSession_->SourceSha256()));
    if (!expectedSha_.isEmpty()) REAL_COMPARE(Utf8Display(project.archiveSession_->SourceSha256()), expectedSha_);
    const std::array<std::string, 2> triggerMembers{"war3map.wtg", "war3map.wct"};
    setup.restart();
    auto captured = project.archiveSession_->CaptureExactMembers(triggerMembers);
    receipt.data.insert("wtg_wct_source_capture_ms_outside_ui_gate", Milliseconds(setup));
    REAL_VERIFY2(captured.Succeeded() && captured.value && captured.value->size() == 2, qPrintable(FailureText(captured.diagnostics)));
    REAL_VERIFY((*captured.value)[0].present && (*captured.value)[1].present);
    receipt.data.insert("wtg_bytes", QString::number((*captured.value)[0].bytes.size()));
    receipt.data.insert("wct_bytes", QString::number((*captured.value)[1].bytes.size()));
    std::size_t hostObserverCalls = 0;
    QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &window, [&](int) { ++hostObserverCalls; });
    receipt.data.insert("host_observer", "actual pre-existing MainWindow indexChanged/Observe/Chrome/recovery; no extra fake observer or cost subtraction");
    const auto HostMeasure = [&]<class Callback>(Receipt& observed, const QString& stage, Callback&& callback) -> decltype(auto) {
        sideEffectsClean = sideEffectsClean && noSideEffects();
        if constexpr (std::is_void_v<std::invoke_result_t<Callback>>) {
            Measure(observed, stage, std::forward<Callback>(callback));
            sideEffectsClean = sideEffectsClean && noSideEffects();
        } else {
            auto result = Measure(observed, stage, std::forward<Callback>(callback));
            sideEffectsClean = sideEffectsClean && noSideEffects();
            return result;
        }
    };
    const auto mapRevision = project.mapEdits_->Revision(), mapSession = project.mapEdits_->SessionIdentity();
    const auto generation = project.editRevision_.Generation();
    const auto* documentOwner = project.triggerDocument_.get();
    const auto wtgSha = std::string(documentOwner->SourceWtgSha256()), wctSha = std::string(documentOwner->SourceWctSha256());
    const auto catalogSha = project.uiCatalog_->Sha256();
    const auto originalArguments = documentOwner->argumentNodes;
    setup.restart();
    receipt.data.insert("stage", "baseline-serialize");
    const auto original = Trigger::WtgWctCodec{}.Serialize(*documentOwner, *project.uiCatalog_);
    receipt.data.insert("baseline_serialize_ms_outside_ui_gate", Milliseconds(setup));
    if (!original.Succeeded() || !original.value) ObserveFailure(receipt, QStringLiteral("Baseline Serialize failed: ") + FailureText(original.diagnostics));
    REAL_VERIFY2(original.Succeeded() && original.value, qPrintable(FailureText(original.diagnostics)));
    receipt.data.insert("stage", "baseline-source-byte-equality");
    const bool baselineWtgExact = original.value->wtg == (*captured.value)[0].bytes;
    const bool baselineWctExact = original.value->wct == (*captured.value)[1].bytes;
    receipt.data.insert("baseline_wtg_exact", baselineWtgExact); receipt.data.insert("baseline_wct_exact", baselineWctExact);
    receipt.data.insert("baseline_encoded_wtg_bytes", QString::number(original.value->wtg.size()));
    receipt.data.insert("baseline_encoded_wct_bytes", QString::number(original.value->wct.size()));
    if (!baselineWtgExact || !baselineWctExact) ObserveFailure(receipt,
        QStringLiteral("Baseline encoded bytes differ from accepted source: WTG exact=%1, WCT exact=%2")
            .arg(baselineWtgExact ? QStringLiteral("true") : QStringLiteral("false"),
                baselineWctExact ? QStringLiteral("true") : QStringLiteral("false")));
    REAL_VERIFY(baselineWtgExact && baselineWctExact);
    receipt.data.insert("stage", "choose-metadata-source-target");
    QJsonObject choiceStatistics;
    setup.restart(); QString choiceReason;
    const auto preparationRevision = std::to_string(generation) + "/" + std::to_string(project.editRevision_.Revision())
        + "/" + std::to_string(mapSession) + "/" + std::to_string(mapRevision) + "/" + catalogSha;
    const auto selectedCase = Choose(*documentOwner, *project.uiCatalog_, preparationRevision, needle_.toUtf8().toStdString(),
        target_.toUtf8().toStdString(), choiceStatistics, choiceReason);
    receipt.data.insert("readonly_case_preparation_ms_outside_ui_gate", Milliseconds(setup));
    receipt.data.insert("readonly_case_preparation_in_foreground_gate", false);
    receipt.data.insert("choice_metadata", choiceStatistics);
    if (!selectedCase) ObserveFailure(receipt, choiceReason);
    REAL_VERIFY2(selectedCase.has_value(), qPrintable(choiceReason));
    const auto choice = *selectedCase;
    receipt.data.insert("selected_target_id", QString::number(choice.targetId));
    receipt.data.insert("selected_type", Utf8Display(choice.type));
    receipt.data.insert("query_needle", Utf8Display(choice.needle));
    receipt.data.insert("distinct_source_eca_owners", QString::number(choice.sourceEcas));
    REAL_VERIFY(choice.rows.size() == 256 && choice.sourceEcas >= 2);
    receipt.data.insert("actual_argument_records", QString::number(originalArguments.size()));

    auto& module = *window.triggersModule_; QWidget* page = module.Widget(); REAL_VERIFY(page);
    receipt.data.insert("rss_post_bind_bytes", Rss()); receipt.data.insert("stage", "actual-main-command-find-replace");
    for (int round = 0; round < 3; ++round) HostMeasure(receipt, QStringLiteral("main_command_show_find_replace"), [&] {
        window.ExecuteCommand("triggers.find-replace");
    });
    REAL_VERIFY(window.CurrentModule() == std::optional<War3Map::Shell::ModuleId>{"triggers"});
    REAL_VERIFY(noSideEffects());
    QPointer<TriggerReplaceDialog> dialog(dynamic_cast<TriggerReplaceDialog*>(page->findChild<QDialog*>(QStringLiteral("triggerReplaceDialog"))));
    REAL_VERIFY(dialog && dialog->isVisible() && !dialog->isModal());
    struct WorkerCleanup {
        QPointer<TriggerReplaceDialog> dialog;
        ~WorkerCleanup() {
            if (dialog) dialog->CancelPreview();
            // 异常/断言提前退出也请求阶段取消；不强杀线程。超时由独立测试进程的外层 deadline 处理。
            if (!QThreadPool::globalInstance()->waitForDone(5000)) {
                std::fputs("TRIGGER_REPLACE_REAL_CLEANUP owned worker still stopping after 5000ms\n", stdout); std::fflush(stdout);
            }
        }
    } workerCleanup{dialog};
    auto* scope = Control<QComboBox>(*dialog, "triggerReplaceScope");
    auto* mode = Control<QComboBox>(*dialog, "triggerReplaceMode");
    auto* match = Control<QComboBox>(*dialog, "triggerReplaceMatch");
    auto* query = Control<QLineEdit>(*dialog, "triggerReplaceQuery");
    auto* tree = Control<QTreeWidget>(*dialog, "triggerReplaceResults");
    auto* search = Control<QPushButton>(*dialog, "triggerReplaceSearch");
    auto* next = Control<QPushButton>(*dialog, "triggerReplaceNext");
    auto* status = Control<QLabel>(*dialog, "triggerReplaceStatus");
    REAL_VERIFY(scope && mode && match && query && tree && search && next && status);
    scope->setCurrentIndex(0); mode->setCurrentIndex(mode->findData(static_cast<int>(Services::TriggerReplaceMode::ParameterValue)));
    match->setCurrentIndex(match->findData(static_cast<int>(Services::TriggerReplaceMatch::Exact)));
    query->setText(Utf8Display(choice.needle));
    receipt.data.insert("stage", "first-search-preparation-query");
    HostMeasure(receipt, QStringLiteral("first_search_including_prepare"), [&] { search->click(); });
    REAL_VERIFY2(dialog && tree->topLevelItemCount() > 0, qPrintable(status->text()));
    receipt.data.insert("first_query_status", status->text());
    for (int round = 0; round < 3; ++round) HostMeasure(receipt, QStringLiteral("warm_search"), [&] { search->click(); });
    REAL_VERIFY2(tree->topLevelItemCount() > 0, qPrintable(status->text()));
    REAL_COMPARE(project.triggerUndoStack_->count(), 0); REAL_COMPARE(project.mapEdits_->Revision(), mapRevision);

    receipt.data.insert("stage", "separate-readonly-snapshot-copy");
    for (int round = 0; round < 3; ++round) {
        const auto snapshot = HostMeasure(receipt, QStringLiteral("separate_strings_snapshot_copy"), [&] {
            return project.mapEdits_->Snapshot(MapDataKind::Strings, Services::TriggerReplaceLimits{}.maxNameTextBytes);
        });
        REAL_VERIFY2(snapshot.Succeeded() && snapshot.value && snapshot.value->Strings(), qPrintable(FailureText(snapshot.diagnostics)));
        std::uint64_t bytes = 0;
        for (const auto& member : snapshot.value->Members()) for (const auto& raw : member.sources) bytes += raw.data.size();
        receipt.data.insert("separate_strings_snapshot_owned_raw_bytes", QString::number(bytes));
        receipt.data.insert("separate_strings_snapshot_entries", QString::number(snapshot.value->Strings()->entries.size()));
    }
    receipt.data.insert("rss_after_prepare_bytes", Rss());

    auto* filter = Control<QLineEdit>(*dialog, "triggerReplaceTargetFilter");
    auto* targetSearch = Control<QPushButton>(*dialog, "triggerReplaceTargetSearch");
    auto* targetNext = Control<QPushButton>(*dialog, "triggerReplaceTargetNext");
    auto* targets = Control<QComboBox>(*dialog, "triggerReplaceTarget");
    auto* targetStatus = Control<QLabel>(*dialog, "triggerReplaceTargetStatus");
    REAL_VERIFY(filter && targetSearch && targetNext && targets && targetStatus);
    filter->setText(Utf8Display(choice.targetName)); receipt.data.insert("stage", "actual-typed-target-options");
    for (int round = 0; round < 3; ++round) HostMeasure(receipt, QStringLiteral("typed_target_query"), [&] { targetSearch->click(); });
    const QString targetPrefix = Utf8Display(choice.targetName) + QStringLiteral(" · ");
    int targetIndex = -1;
    for (std::size_t targetPage = 0; targetPage < 256; ++targetPage) {
        for (int i = 1; i < targets->count(); ++i) if (targets->itemText(i).startsWith(targetPrefix)) {
            REAL_VERIFY2(targetIndex < 0, "The displayed exact target is ambiguous"); targetIndex = i;
        }
        if (targetIndex >= 0 || !targetNext->isEnabled()) break;
        HostMeasure(receipt, QStringLiteral("typed_target_next_page"), [&] { targetNext->click(); });
    }
    REAL_VERIFY2(targetIndex > 0, qPrintable(targetStatus->text())); targets->setCurrentIndex(targetIndex);
    receipt.data.insert("typed_target_actual_label", targets->currentText());
    REAL_COMPARE(project.triggerUndoStack_->count(), 0);

    receipt.data.insert("stage", "actual-cross-page-selection");
    std::set<StableId> selected;
    std::size_t pages = 0; QString selectionMismatch;
    while (selected.size() < 256 && pages < 128) {
        ++pages;
        HostMeasure(receipt, QStringLiteral("select_page_items"), [&] {
            for (int i = 0; i < tree->topLevelItemCount() && selected.size() < 256; ++i) {
                auto* item = tree->topLevelItem(i);
                if (!(item->flags() & Qt::ItemIsUserCheckable)) continue;
                const auto id = DisplayedArgumentId(item->text(1));
                if (!id || selected.contains(*id) || !choice.rows.contains(*id)) continue;
                const auto& before = choice.rows.at(*id);
                if (item->text(2) != Utf8Display(before.oldValue) || item->text(3) != Utf8Display(before.declaredType)) {
                    selectionMismatch = QStringLiteral("Actual UI row no longer matches qualified source for argument%1; old/type must remain exact.").arg(*id);
                    continue;
                }
                item->setCheckState(0, Qt::Checked); selected.insert(*id);
            }
        });
        if (selected.size() == 256 || !next->isEnabled()) break;
        HostMeasure(receipt, QStringLiteral("query_next_page"), [&] { next->click(); });
    }
    receipt.data.insert("actual_query_pages_used", QString::number(pages)); receipt.data.insert("selected_count", QString::number(selected.size()));
    REAL_VERIFY2(selectionMismatch.isEmpty(), qPrintable(selectionMismatch));
    REAL_VERIFY2(selected.size() == 256 && pages > 1, qPrintable(status->text()));
    for (const auto& [id, before] : choice.rows) { (void)before; REAL_VERIFY(selected.contains(id)); }
    auto* previewButton = Control<QPushButton>(*dialog, "triggerReplacePreview");
    auto* previewRows = Control<QTreeWidget>(*dialog, "triggerReplacePreviewRows");
    auto* apply = Control<QPushButton>(*dialog, "triggerReplaceApply");
    REAL_VERIFY(previewButton && previewRows && apply && previewButton->isEnabled());
    receipt.data.insert("stage", "shared-opaque-async-preview-256");
    QJsonArray asyncSamples;
    for (int round = 0; round < 3; ++round) {
        REAL_VERIFY(dialog && previewButton->isEnabled() && !dialog->PreviewPending());
        QElapsedTimer requestTimer; requestTimer.start();
        qint64 lastHeartbeat = 0, maximumGap = 0; std::size_t heartbeatCount = 0;
        const auto beat = [&] {
            const auto now = requestTimer.nsecsElapsed(); maximumGap = (std::max)(maximumGap, now - lastHeartbeat);
            lastHeartbeat = now; ++heartbeatCount;
        };
        QTimer heartbeat; heartbeat.setInterval(5); heartbeat.setTimerType(Qt::PreciseTimer);
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, beat); heartbeat.start();
        HostMeasure(receipt, QStringLiteral("preview_schedule_gui_256"), [&] { previewButton->click(); });
        const auto scheduledAt = requestTimer.nsecsElapsed();
        receipt.data.insert("stage", QStringLiteral("async-preview-wait-round-%1").arg(round + 1));
        while (dialog && dialog->PreviewPending() && requestTimer.elapsed() < 30000) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QTest::qWait(1);
            sideEffectsClean = sideEffectsClean && noSideEffects();
        }
        // 包括调度前沿、每次回调间隔、最终 adoption 后沿，不用平均心跳掩盖一次停顿。
        const auto completedAt = requestTimer.nsecsElapsed(); maximumGap = (std::max)(maximumGap, completedAt - lastHeartbeat); heartbeat.stop();
        QJsonObject sample{{"round", round + 1}, {"request_until_adoption_ms", static_cast<double>(completedAt) / 1000000.0},
            {"wait_after_gui_schedule_ms", static_cast<double>(completedAt - scheduledAt) / 1000000.0},
            {"heartbeat_count", QString::number(heartbeatCount)}, {"heartbeat_maximum_gap_ms", static_cast<double>(maximumGap) / 1000000.0}};
        if (dialog) if (const auto timing = dialog->LastPreviewTiming()) {
            sample.insert("gui_plan_ms", static_cast<double>(timing->planNanoseconds) / 1000000.0);
            sample.insert("gui_owned_clone_ms", static_cast<double>(timing->cloneNanoseconds) / 1000000.0);
            sample.insert("worker_actual_ms", timing->workerNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->workerNanoseconds.load()) / 1000000.0));
            sample.insert("worker_queue_ms", timing->queueNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->queueNanoseconds.load()) / 1000000.0));
            sample.insert("result_delivery_and_adoption_ms", timing->deliveryNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->deliveryNanoseconds.load()) / 1000000.0));
        }
        asyncSamples.push_back(sample); receipt.data.insert("async_preview_samples", asyncSamples);
        REAL_VERIFY2(dialog && !dialog->PreviewPending(), "Owned background preparation did not finish within the separate 30000ms test deadline");
        REAL_VERIFY2(previewRows->topLevelItemCount() == 256 && apply->isEnabled(), qPrintable(status->text()));
        const auto timing = dialog->LastPreviewTiming();
        REAL_VERIFY2(timing && timing->workerNanoseconds.load() >= 0 && timing->queueNanoseconds.load() >= 0 && timing->deliveryNanoseconds.load() >= 0,
            "Successful actual Module preview must have completed owned-worker timing; missing/cancelled/stale duration is Unknown");
        REAL_VERIFY(timing->planNanoseconds >= 0 && timing->cloneNanoseconds >= 0);
        receipt.timings[QStringLiteral("preview_plan_gui_256")].push_back(static_cast<double>(timing->planNanoseconds) / 1000000.0);
        receipt.timings[QStringLiteral("preview_owned_clone_gui_256")].push_back(static_cast<double>(timing->cloneNanoseconds) / 1000000.0);
        REAL_VERIFY2(heartbeatCount > 0, "The actual event-loop heartbeat did not run while the large-map preview was being prepared");
        REAL_VERIFY2(static_cast<double>(maximumGap) / 1000000.0 <= limitMs_, "Actual event-loop maximum gap exceeded the unchanged finite <=1000ms foreground gate");
        REAL_COMPARE(project.triggerUndoStack_->count(), 0); REAL_COMPARE(project.mapEdits_->Revision(), mapRevision);
    }
    REAL_VERIFY2(previewRows->topLevelItemCount() == 256 && apply->isEnabled(), qPrintable(status->text()));
    std::set<StableId> previewIds;
    const auto replacement = previewRows->topLevelItem(0)->text(2).toUtf8().toStdString();
    REAL_VERIFY(replacement == choice.targetName || replacement == "udg_" + choice.targetName);
    for (int i = 0; i < previewRows->topLevelItemCount(); ++i) {
        const auto* row = previewRows->topLevelItem(i); const auto id = DisplayedArgumentId(row->text(0));
        REAL_VERIFY(id && previewIds.insert(*id).second);
        REAL_VERIFY(choice.rows.contains(*id));
        REAL_COMPARE(row->text(1), Utf8Display(choice.rows.at(*id).oldValue)); REAL_COMPARE(row->text(2), Utf8Display(replacement));
        REAL_COMPARE(row->text(3), Utf8Display(choice.type));
    }
    REAL_VERIFY(previewIds == selected); REAL_COMPARE(project.triggerUndoStack_->count(), 0);
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
    receipt.data.insert("rss_after_preview_bytes", Rss());

    receipt.data.insert("stage", "real-one-snapshot-undo-publication");
    HostMeasure(receipt, QStringLiteral("apply_256_one_snapshot_undo"), [&] { apply->click(); });
    REAL_COMPARE(project.triggerUndoStack_->count(), 1); REAL_COMPARE(project.triggerUndoStack_->index(), 1);
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, true));
    REAL_VERIFY(project.triggerDocument_.get() == documentOwner && project.editRevision_.Generation() == generation);
    for (int round = 0; round < 3; ++round) {
        HostMeasure(receipt, QStringLiteral("undo_256"), [&] { window.UndoAction()->trigger(); });
        REAL_COMPARE(project.triggerUndoStack_->index(), 0);
        REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
        if (round == 0) {
            setup.restart(); const auto undone = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
            receipt.data.insert("undo_wire_verification_ms_outside_ui_gate", Milliseconds(setup));
            REAL_VERIFY2(undone.Succeeded() && undone.value, qPrintable(FailureText(undone.diagnostics)));
            REAL_VERIFY(undone.value->wtg == original.value->wtg && undone.value->wct == original.value->wct);
        }
        HostMeasure(receipt, QStringLiteral("redo_256"), [&] { window.RedoAction()->trigger(); });
        REAL_COMPARE(project.triggerUndoStack_->index(), 1); REAL_COMPARE(project.triggerUndoStack_->count(), 1);
        REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, true));
    }
    HostMeasure(receipt, QStringLiteral("final_undo_256"), [&] { window.UndoAction()->trigger(); });
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
    REAL_COMPARE(project.triggerUndoStack_->count(), 1); REAL_COMPARE(project.triggerUndoStack_->index(), 0);
    REAL_VERIFY(project.triggerUndoStack_->canRedo());
    REAL_COMPARE(project.mapEdits_->Revision(), mapRevision); REAL_COMPARE(project.mapEdits_->SessionIdentity(), mapSession);
    REAL_COMPARE(std::string(project.triggerDocument_->SourceWtgSha256()), wtgSha);
    REAL_COMPARE(std::string(project.triggerDocument_->SourceWctSha256()), wctSha);
    REAL_COMPARE(project.uiCatalog_->Sha256(), catalogSha);
    for (const auto* stack : project.objectUndoStacks_) if (stack) REAL_COMPARE(stack->count(), 0);
    receipt.data.insert("stage", "readonly-source-final-verification"); setup.restart();
    const auto unchanged = project.archiveSession_->VerifySourceUnchanged();
    receipt.data.insert("source_final_sha_verification_ms_outside_ui_gate", Milliseconds(setup));
    REAL_VERIFY2(unchanged.Succeeded(), qPrintable(FailureText(unchanged.diagnostics)));
    receipt.data.insert("source_unchanged", true); receipt.data.insert("wtg_wct_undo_byte_exact", true);
    receipt.data.insert("one_undo_command", true); receipt.data.insert("snapshot_probe_is_separate_measurement", true);
    receipt.data.insert("main_window_index_changed_calls", QString::number(hostObserverCalls)); REAL_COMPARE(hostObserverCalls, std::size_t{8});
    receipt.data.insert("stage", "maximum-latency-gate");
    for (const auto& [name, samples] : receipt.timings) {
        REAL_VERIFY(!samples.empty());
        REAL_VERIFY2(*std::max_element(samples.begin(), samples.end()) <= limitMs_,
            qPrintable(name + QStringLiteral(" exceeded the finite <=1000ms maximum foreground gate; report the slow stage without widening it")));
    }
    REAL_VERIFY(sideEffectsClean && noSideEffects());
    REAL_VERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    REAL_VERIFY(window.UndoGroup()->activeStack() == project.triggerUndoStack_);
    REAL_VERIFY(!window.UndoAction()->isEnabled() && window.RedoAction()->isEnabled());
    REAL_VERIFY(!project.triggerDirty_);
    setup.restart();
    const auto finalWire = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    receipt.data.insert("final_undo_wire_verification_ms_outside_ui_gate", Milliseconds(setup));
    REAL_VERIFY2(finalWire.Succeeded() && finalWire.value, qPrintable(FailureText(finalWire.diagnostics)));
    REAL_VERIFY(finalWire.value->wtg == (*captured.value)[0].bytes && finalWire.value->wct == (*captured.value)[1].bytes);
    receipt.data.insert("main_window_verified", true);
    receipt.data.insert("main_chrome_and_shell_actions_verified", true);
    receipt.data.insert("normal_recovery_write_verified", false);
    receipt.data.insert("world_viewport_created", false);
    receipt.data.insert("all_selected_and_unselected_arguments_preserved", true);
    receipt.data.insert("stage", "complete"); receipt.data.insert("passed", true);
}

// F1：真实主窗口在大图副本上把最大的 GUI 触发器转换为自定义代码，再撤销/重做三轮。
// 每步计入命令本身与随后一次事件处理（界面刷新、观察者），门限默认 200 ms；最后撤销须逐字节还原。
void NativeWorkbenchTests::TriggerConversionOwnedLargeMapMainHost() {
    using namespace War3Map;
    if (!qApp->property("triggerReplaceMainHostExplicit").toBool()) QSKIP("Use the registered single-method real-host entry.");
    const auto map = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP"), ui = qEnvironmentVariable("FORGE_YDWE_ROOT");
    if (map.isEmpty() || ui.isEmpty()) QSKIP("Requires explicit readonly large-map copy and catalog root.");
    double limitMs = 200;
    if (const auto requested = qEnvironmentVariable("FORGE_TRIGGER_CONVERSION_MAX_OPERATION_MS"); !requested.isEmpty()) {
        bool ok = false; limitMs = requested.toDouble(&ok);
        QVERIFY2(ok && limitMs > 0, "FORGE_TRIGGER_CONVERSION_MAX_OPERATION_MS must be a positive number.");
    }
    QVERIFY(QGuiApplication::platformName() == QStringLiteral("offscreen"));
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    auto& project = *window.project_;
    window.show(); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QVERIFY(window.UseUiDataRoot(std::filesystem::path(ui.toStdWString())));
    QVERIFY(window.OpenMapArchivePath(std::filesystem::path(map.toStdWString())));
    QVERIFY(project.triggerDocument_ && project.uiCatalog_ && project.triggerUndoStack_);
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    const Trigger::TriggerRecord* target = nullptr;
    for (const auto& trigger : project.triggerDocument_->triggers)
        if (trigger.type == 0 && trigger.customTextFlag == 0 && (!target || trigger.ecas.size() > target->ecas.size())) target = &trigger;
    QVERIFY(target && !target->ecas.empty());
    const auto id = target->id;
    auto* module = window.triggersModule_;
    module->SetConfirmHandler([](const QString&, const QString&, const QStringList&) { return true; });
    QVERIFY(module->Reveal({"triggers", "trigger", QString::number(id), {}}));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QStringList timings;
    double worst = 0;
    const auto measure = [&](const QString& name, auto&& action) {
        QElapsedTimer timer; timer.start();
        action();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const double ms = timer.nsecsElapsed() / 1.0e6;
        worst = std::max(worst, ms);
        timings << QStringLiteral("%1=%2ms").arg(name).arg(ms, 0, 'f', 1);
    };
    bool converted = false;
    measure(QStringLiteral("convert"), [&] { converted = module->ConvertCurrentTriggerToCustomCode(); });
    QVERIFY(converted);
    const auto* after = Trigger::TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, id);
    QVERIFY(after && after->customTextFlag == 1 && after->ecas.empty());
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    for (int round = 0; round < 3; ++round) {
        measure(QStringLiteral("undo%1").arg(round), [&] { project.triggerUndoStack_->undo(); });
        measure(QStringLiteral("redo%1").arg(round), [&] { project.triggerUndoStack_->redo(); });
    }
    measure(QStringLiteral("final-undo"), [&] { project.triggerUndoStack_->undo(); });
    qInfo().noquote() << "TRIGGER_CONVERSION_MAIN_HOST target-top-level-ecas=" << target->ecas.size() << timings.join(' ');
    for (int row = 0; row < project.diagnosticsModel_->rowCount(); ++row)
        for (int column = 0; column < project.diagnosticsModel_->columnCount(); ++column) {
            const auto* item = project.diagnosticsModel_->item(row, column);
            QVERIFY2(!item || !item->text().contains(QStringLiteral("CODE_EDIT")), qPrintable(item->text()));
        }
    const auto restored = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(restored.Succeeded() && restored.value);
    QVERIFY2(restored.value->wtg == original.value->wtg && restored.value->wct == original.value->wct,
        "Final undo did not restore the original WTG/WCT bytes.");
    if (qEnvironmentVariableIsSet("FORGE_TRIGGER_CONVERSION_PROFILE")) {
        // 诊断：逐个断开撤销栈的监听者，观察重做/撤销耗时变化。
        auto* stack = project.triggerUndoStack_;
        const std::vector<std::pair<QString, QObject*>> receivers{{"main-window", &window}, {"trigger-module", window.triggersModule_},
            {"variables-module", static_cast<QObject*>(window.variablesModule_)}, {"manager-module", static_cast<QObject*>(window.managerModule_)},
            {"world-module", static_cast<QObject*>(window.worldModule_)}, {"project", window.project_.get()}};
        const auto pair = [&](const QString& name) {
            timings.clear();
            measure(name + QStringLiteral("-redo"), [&] { stack->redo(); });
            measure(name + QStringLiteral("-undo"), [&] { stack->undo(); });
            qInfo().noquote() << "PROFILE" << timings.join(' ');
        };
        pair(QStringLiteral("all-connected"));
        for (const auto& [name, receiver] : receivers) {
            if (receiver) QObject::disconnect(stack, &QUndoStack::indexChanged, receiver, nullptr);
            pair(QStringLiteral("without-") + name);
        }
    }
    QVERIFY2(worst <= limitMs, qPrintable(QStringLiteral("Slowest step %1ms exceeds %2ms: %3")
        .arg(worst, 0, 'f', 1).arg(limitMs).arg(timings.join(' '))));
}
int main(int argc, char** argv) {
    const bool mainHostRequested = [&] {
        for (int index = 1; index < argc; ++index)
            if (QByteArray(argv[index]) == QByteArrayLiteral("TriggerReplaceOwnedLargeMapMainHost256")
                || QByteArray(argv[index]) == QByteArrayLiteral("TriggerConversionOwnedLargeMapMainHost")) return true;
        return false;
    }();
    // 只隔离这个显式真实用例；原 NativeWorkbench 默认 main/测试环境保持原行为。
    std::unique_ptr<QTemporaryDir> mainHostSettings;
    if (mainHostRequested) {
        if (qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP").isEmpty() || qEnvironmentVariable("FORGE_YDWE_ROOT").isEmpty()) {
            std::fputs("TRIGGER_REPLACE_MAIN_HOST_SKIP explicit readonly map and catalog ENV are required\n", stdout);
            return 77;
        }
        if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("offscreen")) {
            std::fputs("The actual MainWindow readonly case requires QT_QPA_PLATFORM=offscreen\n", stderr); return 2;
        }
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
        mainHostSettings = std::make_unique<QTemporaryDir>();
        if (!mainHostSettings->isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, mainHostSettings->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, mainHostSettings->path());
    }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    // 默认离屏；排查真实字体下的布局时可显式指定 QT_QPA_PLATFORM=windows。
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    if (mainHostRequested) {
        application.setProperty("triggerReplaceMainHostExplicit", true);
        const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
        if (!fonts.bundledUi || !fonts.bundledCode || !fonts.failedFiles.isEmpty()) {
            std::fputs("The actual MainWindow case requires deployed bundled UI/code fonts\n", stderr); return 2;
        }
        War3Map::UiKit::ApplyApplicationFont(fonts);
    }
    const auto originalTempRoot = QDir::tempPath();
    application.setProperty("nativeWorkbenchOriginalTempRoot", originalTempRoot);
    if (qEnvironmentVariableIsSet("FORGE_R08_SCREENSHOT_DIR")
        || qEnvironmentVariableIsSet("FORGE_R08_CHANGES_SCREENSHOT_DIR")
        || qEnvironmentVariableIsSet("FORGE_R08_WORLD_NATIVE_SCREENSHOT_DIR")
        || qEnvironmentVariableIsSet("FORGE_R08_WORLD_ANIMATION_EVIDENCE_DIR")
        || qEnvironmentVariableIsSet("FORGE_AI_WORKSPACE_CAPTURE_DIR")
        || qEnvironmentVariableIsSet("FORGE_WORLD_PLACEMENT_EVIDENCE_DIR")
        || qEnvironmentVariableIsSet("FORGE_BRAND_CAPTURE_DIR")
        || qEnvironmentVariableIsSet("FORGE_MCP_GUI_CAPTURE_DIR")
        || qEnvironmentVariableIsSet("FORGE_POPUP_CAPTURE_DIR")
        || qEnvironmentVariableIsSet("AUD_SHELL_RESPONSIVE_CAPTURE_DIR")
        || qEnvironmentVariableIsSet("FORGE_RECENT_MAPS_CAPTURE_DIR")) {
        const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
        if (!fonts.bundledUi || !fonts.bundledCode || !fonts.failedFiles.isEmpty()) {
            qCritical("Capture requires deployed bundled UI/code fonts; refusing unreadable evidence");
            return 2;
        }
        War3Map::UiKit::ApplyApplicationFont(fonts);
        const QFontMetrics metrics(QApplication::font());
        if (!metrics.inFont(QChar{u'地'}) || !metrics.inFont(QChar{u'A'})) return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("War3MapEditor64Tests"));
    // 每次运行独立的测试状态；避免读取旧 AppData 或与 MSIX 重定向的恢复目录混用。
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("NativeWorkbenchTests-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    // OpenMapArchivePath 的派生工作区也落入此独立测试目录，并在进程结束前清理。
    const auto extractionTemplate = qEnvironmentVariable("FORGE_RECOVERY_QUIT_LIFECYCLE_CHILD") == QStringLiteral("1")
        ? QDir(originalTempRoot).filePath(QStringLiteral("at-XXXXXX")) : QString{};
    QTemporaryDir extractedRoot(extractionTemplate);
    if (!extractedRoot.isValid()) {
        qCritical().noquote() << "NATIVE_WORKBENCH_TEMP_ROOT_FAILED"
            << "root=" << originalTempRoot << "template=" << extractionTemplate << "error=" << extractedRoot.errorString();
        return 1;
    }
    const QByteArray previousTmp = qgetenv("TMP");
    const QByteArray previousTemp = qgetenv("TEMP");
    qputenv("TMP", QFile::encodeName(extractedRoot.path()));
    qputenv("TEMP", QFile::encodeName(extractedRoot.path()));
    NativeWorkbenchTests tests;
    const int result = QTest::qExec(&tests, argc, argv);
    qputenv("TMP", previousTmp);
    qputenv("TEMP", previousTemp);
    return result;
}

#include "CampaignWorkbenchChecks.inl"
#include "NativeWorkbenchTests.moc"
