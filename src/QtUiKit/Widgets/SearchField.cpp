#include "Widgets/SearchField.h"

#include "Icons/SvgIcons.h"

#include <QEvent>
#include <QLabel>
#include <QLineEdit>

namespace War3Map::UiKit {
namespace {

constexpr int kIconSize = 14;
constexpr int kIconLeft = 10;

class SearchIconPlacer final : public QObject {
public:
    SearchIconPlacer(QLineEdit* edit, QLabel* icon) : QObject(edit), edit_(edit), icon_(icon) {}

    void Place() {
        // 以输入框整体高度居中（不受内边距和缩放取整影响），左侧固定留白。
        icon_->setGeometry(kIconLeft, (edit_->height() - kIconSize) / 2, kIconSize, kIconSize);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == edit_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) Place();
        if (watched == edit_ && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) Refresh();
        return false;
    }

public:
    void Refresh() { icon_->setPixmap(SvgIcon(QStringLiteral("lucide/search")).pixmap(kIconSize, kIconSize)); }

private:
    QLineEdit* edit_;
    QLabel* icon_;
};

} // namespace

void InstallSearchIcon(QLineEdit* edit) {
    if (edit == nullptr || edit->findChild<QLabel*>(QStringLiteral("searchFieldIcon"), Qt::FindDirectChildrenOnly)) return;
    auto* icon = new QLabel(edit);
    icon->setObjectName(QStringLiteral("searchFieldIcon"));
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setAlignment(Qt::AlignCenter);
    auto* placer = new SearchIconPlacer(edit, icon);
    placer->Refresh();
    edit->installEventFilter(placer);
    // 文本从图标右侧开始；样式表的左内边距之外再让出图标宽度。
    edit->setTextMargins(kIconSize + 8, 0, 0, 0);
    placer->Place();
}

} // namespace War3Map::UiKit
