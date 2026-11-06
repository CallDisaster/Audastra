#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace War3Map::Modules {
namespace {
constexpr const char* kClipboardType = "application/x-audastra-ui-frame+json";
QString Problem(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? QObject::tr("界面复制失败") : QString::fromStdString(diagnostics.front().message);
}
} // namespace

bool GameUiModule::CopySelectedDescriptionFrame(QString* error) {
    QString id;
    const auto* frame = SelectedDescriptionFrame(&id);
    const auto* description = Description(id);
    if (!frame || !description) { if (error) *error = tr("先选中自己的界面框架"); return false; }
    const auto copied = GameUi::CopyUiFrame(*description, frame->id);
    if (!copied.value) { if (error) *error = Problem(copied.diagnostics); return false; }
    auto* mime = new QMimeData;
    const QByteArray bytes(copied.value->data(), static_cast<qsizetype>(copied.value->size()));
    mime->setData(kClipboardType, bytes);
    mime->setText(QString::fromUtf8(bytes));
    QGuiApplication::clipboard()->setMimeData(mime);
    return true;
}

bool GameUiModule::PasteDescriptionFrame(QString* error) {
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(kClipboardType)) return fail(tr("剪贴板里没有界面框架"));
    const QByteArray bytes = mime->data(kClipboardType);
    if (bytes.size() > 2 * 1024 * 1024) return fail(tr("界面剪贴板超过2 MiB"));
    const auto json = QJsonDocument::fromJson(bytes);
    if (!json.isObject()) return fail(tr("界面剪贴板不是有效的描述"));
    QString id, parent;
    if (tree_ && selected_ < tree_->Nodes().size() && tree_->Nodes()[selected_].uiFrame) {
        const auto& link = *tree_->Nodes()[selected_].uiFrame;
        id = QString::fromStdString(link.description); parent = QString::fromStdString(link.frame);
    } else if (Descriptions().size() == 1) id = QString::fromStdString(Descriptions().front().description.id);
    if (id.isEmpty()) return fail(tr("先选中要粘贴到的界面描述或父框架"));
    QJsonObject operation{{"op", "paste_frame"}, {"clipboard", json.object()}};
    if (!parent.isEmpty()) operation.insert("parent", parent);
    if (!EditDescription(id, QString::fromUtf8(QJsonDocument(QJsonArray{operation}).toJson(QJsonDocument::Compact)), tr("粘贴界面框架"), error)) return false;
    const auto* updated = Description(id);
    if (updated) {
        const auto* owner = parent.isEmpty() ? nullptr : GameUi::FindUiFrame(*updated, parent.toStdString());
        const auto& children = owner ? owner->children : updated->frames;
        if (!children.empty()) SelectFrameWhenReady(QString::fromStdString(children.back().id));
    }
    return true;
}
} // namespace War3Map::Modules
