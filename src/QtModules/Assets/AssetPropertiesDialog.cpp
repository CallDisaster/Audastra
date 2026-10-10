#include "AssetPropertiesDialog.h"

#include "AssetsDetail.h"
#include "EditorProject.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace War3Map::Modules {

namespace {

constexpr char kStandardFolder[] = "war3mapImported\\";

} // namespace

AssetPropertiesDialog::AssetPropertiesDialog(const QtProject::AssetEntry& entry,
    std::function<QString(const QString&)> check, const QString& disabledIconState, QWidget* parent, bool directorySource)
    : QDialog(parent), original_(AssetsDetail::Normalized(entry.path)), fileName_(AssetsDetail::FileName(entry.path)),
      check_(std::move(check)) {
    setObjectName("assetPropertiesDialog");
    setWindowTitle(tr("导入文件属性"));
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->addRow(tr("文件名："), new QLabel(fileName_, this));
    form->addRow(tr("类型："), new QLabel(AssetsDetail::TypeText(fileName_), this));
    form->addRow(tr("大小："), new QLabel(AssetsDetail::SizeText(entry.size), this));
    auto* source = new QLabel(entry.ownedPayload ? tr("工程草稿 · 内容保存在工程中") : entry.Pending() ? tr("待导入 · %1").arg(entry.diskPath)
        : directorySource ? tr("目录快照（原路径 %1）").arg(entry.originalPath)
        : entry.originalPath == entry.path ? tr("地图归档") : tr("地图归档（原路径 %1，保存时移动）").arg(entry.originalPath), this);
    source->setObjectName("assetSource");
    source->setWordWrap(true);
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("来源："), source);
    if (!disabledIconState.isEmpty()) {
        auto* icon = new QLabel(disabledIconState, this);
        icon->setObjectName("assetDisabledIconState");
        icon->setWordWrap(true);
        form->addRow(tr("暂停图标："), icon);
    }
    layout->addLayout(form);

    // 原版规则：未勾选自定义路径时文件放在 war3mapImported\ 下。
    custom_ = new QCheckBox(tr("使用自定义路径"), this);
    custom_->setObjectName("assetCustomPath");
    const bool standard = original_.compare(QString::fromLatin1(kStandardFolder) + fileName_, Qt::CaseInsensitive) == 0;
    custom_->setChecked(!standard);
    layout->addWidget(custom_);
    path_ = new QLineEdit(original_, this);
    path_->setObjectName("assetPathEdit");
    path_->setEnabled(custom_->isChecked());
    path_->setToolTip(tr("归档内的完整路径，文件夹用 \\ 分隔。例如 ReplaceableTextures\\CommandButtons\\BTNHero.blp"));
    layout->addWidget(path_);
    problem_ = new QLabel(this);
    problem_->setObjectName("assetPathProblem");
    problem_->setWordWrap(true);
    problem_->setStyleSheet(QStringLiteral("color: #e5534b;"));
    layout->addWidget(problem_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons_);

    connect(custom_, &QCheckBox::toggled, this, [this](bool checked) {
        path_->setEnabled(checked);
        if (!checked) path_->setText(QString::fromLatin1(kStandardFolder) + fileName_);
        Validate();
    });
    connect(path_, &QLineEdit::textChanged, this, &AssetPropertiesDialog::Validate);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    Validate();
}

QString AssetPropertiesDialog::Path() const {
    return AssetsDetail::Normalized(path_->text().trimmed());
}

void AssetPropertiesDialog::Validate() {
    const QString path = Path();
    const QString problem = path.compare(original_, Qt::CaseSensitive) == 0 ? QString{} : check_(path);
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}

} // namespace War3Map::Modules
