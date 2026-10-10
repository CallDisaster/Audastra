#include "Fonts/FontLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QSet>
#include <QStandardPaths>

namespace War3Map::UiKit {
namespace {

template <std::size_t N>
QStringList AvailableFamilies(const std::array<std::string_view, N>& candidates) {
    QSet<QString> installed;
    for (const QString& family : QFontDatabase::families()) {
        installed.insert(family.toCaseFolded());
    }
    QStringList result;
    for (const auto candidate : candidates) {
        const QString family = QString::fromUtf8(candidate.data(), static_cast<qsizetype>(candidate.size()));
        // CSS 通用族名只对 Web 有意义。
        if (family == QLatin1String("sans-serif") || family == QLatin1String("serif") ||
            family == QLatin1String("monospace")) {
            continue;
        }
        if (installed.contains(family.toCaseFolded())) {
            result.push_back(family);
        }
    }
    return result;
}

template <std::size_t N>
bool IsFirstCandidate(const QStringList& families, const std::array<std::string_view, N>& candidates) {
    return !families.isEmpty() &&
        families.front() == QString::fromUtf8(candidates.front().data(),
            static_cast<qsizetype>(candidates.front().size()));
}

QFont BaseFont(const QStringList& families, int pixelSize) {
    QFont font;
    if (!families.isEmpty()) {
        font.setFamilies(families);
    }
    font.setPixelSize(pixelSize);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

} // namespace

QString DefaultFontDirectory() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fonts"));
}

FontSetup LoadFonts(const QString& directory) {
    FontSetup setup;
    const QDir dir(directory);
    if (!directory.isEmpty() && dir.exists()) {
        const auto files = dir.entryInfoList(
            {QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files, QDir::Name);
        for (const auto& file : files) {
            const int id = QFontDatabase::addApplicationFont(file.absoluteFilePath());
            (id < 0 ? setup.failedFiles : setup.loadedFiles).push_back(file.fileName());
        }
    }
    // 默认界面字体是系统的微软雅黑。正常 Windows 平台已能找到；离屏平台只认 Qt 自带字体目录，
    // 这里按需登记同一组系统字体文件，保证截图与测试看到的就是实际界面字体。
    if (!QFontDatabase::families().contains(QStringLiteral("Microsoft YaHei UI"))) {
        const QDir system(QStandardPaths::writableLocation(QStandardPaths::FontsLocation));
        for (const auto* name : {"msyh.ttc", "msyhbd.ttc"}) {
            const QString path = system.filePath(QString::fromLatin1(name));
            if (QFileInfo::exists(path) && QFontDatabase::addApplicationFont(path) >= 0) setup.loadedFiles.push_back(QString::fromLatin1(name));
        }
    }
    setup.uiFamilies = AvailableFamilies(Tokens::kUiFontFamilies);
    setup.codeFamilies = AvailableFamilies(Tokens::kCodeFontFamilies);
    setup.bundledUi = IsFirstCandidate(setup.uiFamilies, Tokens::kUiFontFamilies);
    setup.bundledCode = IsFirstCandidate(setup.codeFamilies, Tokens::kCodeFontFamilies);
    return setup;
}

QFont UiFont(const FontSetup& setup, int pixelSize, int weight) {
    QFont font = BaseFont(setup.uiFamilies, pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QFont CodeFont(const FontSetup& setup, int pixelSize) {
    QFont font = BaseFont(setup.codeFamilies, pixelSize);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QFont MonoFont(int pixelSize) {
    QStringList families;
    for (const auto family : Tokens::kCodeFontFamilies)
        families << QString::fromUtf8(family.data(), static_cast<qsizetype>(family.size()));
    QFont font;
    font.setFamilies(families);
    font.setPixelSize(pixelSize);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

namespace {
FontSetup& AppliedSetup() {
    static FontSetup setup;
    return setup;
}
} // namespace

void ApplyApplicationFont(const FontSetup& setup, const QString& preferredFamily) {
    AppliedSetup() = setup;
    QFont font = UiFont(setup);
    // 用户在设置里选的字体放在最前，令牌候选作为缺字回退。
    if (!preferredFamily.isEmpty() && QFontDatabase::families().contains(preferredFamily)) {
        QStringList families = setup.uiFamilies;
        families.removeAll(preferredFamily);
        families.prepend(preferredFamily);
        font.setFamilies(families);
    }
    QGuiApplication::setFont(font);
}

void ApplyPreferredUiFamily(const QString& preferredFamily) {
    ApplyApplicationFont(AppliedSetup(), preferredFamily);
}

QStringList SelectableUiFamilies() {
    QStringList families;
    for (const QString& family : QFontDatabase::families(QFontDatabase::SimplifiedChinese)) {
        // 竖排变体（@开头）和私有字体不出现在选择列表里。
        if (family.startsWith(QLatin1Char('@')) || QFontDatabase::isPrivateFamily(family) || families.contains(family)) continue;
        families.push_back(family);
    }
    return families;
}

} // namespace War3Map::UiKit
