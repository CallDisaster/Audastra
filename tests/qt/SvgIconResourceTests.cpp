#include "Icons/SvgIcons.h"

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QSvgRenderer>
#include <QTest>
#include <QToolButton>

using namespace War3Map::UiKit;

class SvgIconResourceTests final : public QObject {
    Q_OBJECT
private slots:
    void EveryBundledResourceHasValidSvgAndVisiblePixels() {
        QVERIFY(HasSvgIcon(QStringLiteral("lucide/file-code-2")));
        QDirIterator files(QStringLiteral(":/war3map/icons"), {QStringLiteral("*.svg")}, QDir::Files,
            QDirIterator::Subdirectories);
        int count = 0;
        while (files.hasNext()) {
            const auto path = files.next();
            QFile file(path); QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            const auto source = file.readAll();
            QVERIFY2(QSvgRenderer(source).isValid(), qPrintable(path));
            const auto name = path.mid(QStringLiteral(":/war3map/icons/").size()).chopped(4);
            const auto icon = SvgIcon(name); QVERIFY2(!icon.isNull(), qPrintable(name));
            const auto image = icon.pixmap(24, 24).toImage();
            bool visible = false;
            for (int y = 0; y < image.height() && !visible; ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha()) { visible = true; break; }
            QVERIFY2(visible, qPrintable(name));
            ++count;
        }
        QVERIFY(count > 0);
    }
    void MissingIconRetainsVisibleAccessibleAction() {
        QToolButton button;
        SetToolButtonSvgIcon(button, QStringLiteral("test-fixture/missing-icon"), QStringLiteral("新建自定义代码"));
        QVERIFY(button.icon().isNull()); QCOMPARE(button.toolButtonStyle(), Qt::ToolButtonTextOnly);
        QCOMPARE(button.text(), QStringLiteral("新建自定义代码")); QCOMPARE(button.accessibleName(), button.text());
        QVERIFY(button.sizeHint().width() >= button.fontMetrics().horizontalAdvance(button.text()));
        SetToolButtonSvgIcon(button, QStringLiteral("lucide/file-code-2"), button.text());
        QVERIFY(!button.icon().isNull()); QCOMPARE(button.toolButtonStyle(), Qt::ToolButtonIconOnly);
        QCOMPARE(button.accessibleName(), QStringLiteral("新建自定义代码"));
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    SvgIconResourceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "SvgIconResourceTests.moc"
