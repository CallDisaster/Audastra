#include "WorldMinimapMarkerDialog.h"
#include "EditorProject.h"

#include <QCoreApplication>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringListModel>
#include <QVBoxLayout>

namespace War3Map::Modules {
void OpenMinimapMarkerDialog(QtProject::EditorProject& project, QWidget* parent) {
    using namespace Services;
    QDialog dialog(parent);
    const auto tr = [](const char* text) { return QCoreApplication::translate("MinimapMarkerDialog", text); };
    dialog.setWindowTitle(tr("小地图图标"));
    dialog.resize(490, 450);
    auto* layout = new QVBoxLayout(&dialog);
    auto* message = new QLabel(&dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto* list = new QListView(&dialog);
    auto* model = new QStringListModel(list);
    list->setModel(model);
    list->setUniformItemSizes(true);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(list);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* type = new QSpinBox(&dialog); type->setRange(0, 2);
    auto* x = new QSpinBox(&dialog); x->setRange(0, 255);
    auto* y = new QSpinBox(&dialog); y->setRange(0, 255);
    auto* color = new QLineEdit(QStringLiteral("ffffffff"), &dialog); color->setMaxLength(8);
    form->addRow(tr("类型（0–2）"), type);
    form->addRow(tr("X"), x);
    form->addRow(tr("Y"), y);
    form->addRow(tr("BGRA（十六进制）"), color);
    auto* buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    const auto button = [&](const char* text) {
        auto* control = new QPushButton(tr(text), &dialog);
        buttons->addWidget(control);
        return control;
    };
    auto* add = button("增加"); auto* move = button("移动所选"); auto* remove = button("删除所选");
    auto* close = button("关闭");
    std::optional<MinimapMarkerView> view;
    const auto report = [&](const auto& diagnostics) {
        project.AddDiagnostics(diagnostics);
        message->setText(tr("无法编辑图标。请查看问题列表。"));
    };
    const auto show = [&] {
        auto query = project.QueryMinimapMarkers();
        view.reset();
        QStringList rows;
        if (!query.Succeeded() || !query.value) report(query.diagnostics);
        else {
            view = std::move(*query.value);
            message->setText(view->legacy ? tr("保留旧图标，未识别其生成来源。") : QString{});
            rows.reserve(static_cast<qsizetype>(view->records.size()));
            for (std::size_t i = 0; i < view->records.size(); ++i) {
                const auto& value = view->records[i];
                QString bgra;
                for (const auto byte : value.colorBgra) bgra += QStringLiteral("%1").arg(unsigned(byte), 2, 16, QLatin1Char('0'));
                rows.push_back(QStringLiteral("%1 · (%2, %3) · %4 · %5")
                    .arg(value.typeRaw).arg(value.positionRaw[0]).arg(value.positionRaw[1]).arg(bgra)
                    .arg(view->origins[i] == MinimapMarkerOrigin::Generated ? tr("自动") :
                        view->origins[i] == MinimapMarkerOrigin::Author ? tr("手动") : tr("旧图标")));
            }
        }
        model->setStringList(rows);
    };
    const auto apply = [&](MinimapMarkerOperation operation) {
        if (!view) return;
        const auto index = list->currentIndex();
        if (operation != MinimapMarkerOperation::Add && !index.isValid()) {
            message->setText(tr("请先选择一个图标。"));
            return;
        }
        MinimapMarkerEdit edit{operation, index.isValid() ? std::size_t(index.row()) : 0,
            {type->value(), {x->value(), y->value()}, {}}};
        if (operation == MinimapMarkerOperation::Add) {
            const auto hex = color->text();
            if (hex.size() != 8) { message->setText(tr("颜色需要 8 个十六进制数字。")); return; }
            for (int i = 0; i < 4; ++i) {
                bool okay = false;
                const auto value = hex.mid(i * 2, 2).toUInt(&okay, 16);
                if (!okay || value > 255) { message->setText(tr("颜色需要 8 个十六进制数字。")); return; }
                edit.value.colorBgra[i] = static_cast<std::uint8_t>(value);
            }
        }
        const auto status = project.EditMinimapMarkers(view->revision, std::span(&edit, 1));
        if (!status.Succeeded()) { report(status.diagnostics); return; }
        show();
    };
    QObject::connect(list, &QListView::clicked, &dialog, [&](const QModelIndex& row) {
        if (!view || !row.isValid()) return;
        const auto& record = view->records.at(row.row());
        type->setValue(record.typeRaw);
        x->setValue(record.positionRaw[0]); y->setValue(record.positionRaw[1]);
    });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] { apply(MinimapMarkerOperation::Add); });
    QObject::connect(move, &QPushButton::clicked, &dialog, [&] { apply(MinimapMarkerOperation::Move); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { apply(MinimapMarkerOperation::Remove); });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    show();
    dialog.exec();
}
} // namespace War3Map::Modules
