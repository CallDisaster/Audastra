#include "CampaignModule.h"
#include "EditorProject.h"
#include "War3MapCore/Hash.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Modules {
namespace {
QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
}
void CampaignModule::AddMap(bool replace) {
    if (!session_) return;
    const auto source = QFileDialog::getOpenFileName(page_, tr("选择章节地图"), {}, tr("Warcraft III 地图 (*.w3x *.w3m)"));
    if (source.isEmpty()) return;
    Services::CampaignCommand command;
    command.operation = replace ? Services::CampaignOperation::ReplaceMap : Services::CampaignOperation::AddMap;
    command.id = SelectedId(false); command.mapSource = std::filesystem::path(source.toStdWString());
    if (!replace) {
        bool accepted = false;
        const auto path = QInputDialog::getText(page_, tr("战役内地图路径"), tr("相对路径（英文字母、数字）"), QLineEdit::Normal,
            QFileInfo(source).fileName(), &accepted);
        if (!accepted) return;
        command.path = path.toUtf8().toStdString();
    }
    (void)ApplyCommand(command);
}
void CampaignModule::EditButton(bool create) {
    if (!session_) return;
    Services::CampaignCommand command;
    command.operation = create ? Services::CampaignOperation::AddButton : Services::CampaignOperation::UpdateButton;
    command.id = SelectedId(true);
    if (!create) {
        const auto& rows = session_->Info().buttons;
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == command.id; });
        if (found == rows.end()) return;
        command.button = *found;
    }
    QDialog dialog(page_); dialog.setWindowTitle(create ? tr("新增菜单按钮") : tr("编辑菜单按钮")); dialog.setMinimumWidth(480);
    auto* layout = new QVBoxLayout(&dialog); auto* form = new QFormLayout;
    auto* chapter = new QLineEdit(Text(command.button.chapterTitle), &dialog); chapter->setMaxLength(65536);
    auto* title = new QLineEdit(Text(command.button.mapTitle), &dialog); title->setMaxLength(65536);
    auto* map = new QComboBox(&dialog);
    for (const auto& entry : session_->Info().maps) map->addItem(Text(entry.path), Text(entry.path));
    const auto index = map->findData(Text(command.button.mapPath)); if (index >= 0) map->setCurrentIndex(index);
    auto* visible = new QCheckBox(tr("首次进入战役时显示"), &dialog); visible->setChecked(create || command.button.visible != 0);
    form->addRow(tr("章节标题"), chapter); form->addRow(tr("关卡标题"), title); form->addRow(tr("目标地图"), map); form->addRow(visible);
    layout->addLayout(form); auto* controls = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(controls);
    connect(controls, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(controls, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    command.button.chapterTitle = chapter->text().toUtf8().toStdString(); command.button.mapTitle = title->text().toUtf8().toStdString();
    command.button.mapPath = map->currentData().toString().toUtf8().toStdString();
    if (create || visible->isChecked() != (command.button.visible != 0)) command.button.visible = visible->isChecked() ? 1 : 0;
    (void)ApplyCommand(command);
}
void CampaignModule::RemoveSelection(bool menu) {
    Services::CampaignCommand command;
    command.operation = menu ? Services::CampaignOperation::RemoveButton : Services::CampaignOperation::RemoveMap;
    command.id = SelectedId(menu); if (command.id) (void)ApplyCommand(command);
}
void CampaignModule::MoveSelection(bool menu, int delta) {
    const auto table = menu ? buttons_ : maps_; if (!table || table->currentRow() < 0) return;
    const auto target = table->currentRow() + delta; if (target < 0 || target >= table->rowCount()) return;
    Services::CampaignCommand command;
    command.operation = menu ? Services::CampaignOperation::MoveButton : Services::CampaignOperation::MoveMap;
    command.id = SelectedId(menu); command.position = static_cast<std::size_t>(target);
    if (ApplyCommand(command)) table->selectRow(target);
}
void CampaignModule::ExportSelection() {
    if (!session_) return; const auto id = SelectedId(false);
    const auto& rows = session_->Info().maps;
    const auto found = std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
    if (found == rows.end()) return;
    const auto target = QFileDialog::getSaveFileName(page_, tr("导出章节地图"), Text(found->path), tr("Warcraft III 地图 (*.w3x *.w3m)"));
    if (target.isEmpty()) return;
    const auto exported = session_->ExportMember(displayedRevision_, found->path, std::filesystem::path(target.toStdWString()));
    if (!exported.Succeeded()) (void)Failed(exported.diagnostics); else Notice(tr("章节已导出：%1").arg(target));
}
bool CampaignModule::OpenChapter(StableId id) {
    if (!session_ || !FlushDraft()) return false;
    if (HasUncommittedChapter()) { Notice(tr("上一章节的修改尚未更新到战役，请先保存章节并点击“更新已打开章节”。")); return false; }
    const auto& rows = session_->Info().maps;
    const auto found = std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
    if (found == rows.end()) { Notice(tr("请选择一个章节地图。")); return false; }
    if (!chapterFiles_) chapterFiles_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/Audastra-CampaignChapters-XXXXXX");
    if (!chapterFiles_->isValid()) { Notice(tr("无法建立章节临时目录。")); return false; }
    const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces) + (Text(found->path).endsWith(".w3m", Qt::CaseInsensitive) ? ".w3m" : ".w3x");
    const auto output = std::filesystem::path(chapterFiles_->path().toStdWString()) / name.toStdWString();
    const auto exported = session_->ExportMember(displayedRevision_, found->path, output);
    if (!exported.Succeeded()) return Failed(exported.diagnostics);
    const auto hash = Sha256File(output);
    if (!hash.Succeeded() || !hash.value) return Failed(hash.diagnostics);
    emit ChapterOpenRequested(QString::fromStdWString(output.wstring()));
    if (mainProject_.sourceArchivePath_ != output) { Notice(tr("章节打开已取消；当前章节保持不变。")); return false; }
    openedChapterFile_ = output; openedChapterId_ = id;
    openedChapterSourceSha_ = *hash.value; chapterSavedPending_ = false;
    Notice(tr("章节副本已打开。保存章节后，回到战役点击“更新已打开章节”，再保存战役。"));
    return true;
}
bool CampaignModule::UpdateOpenedChapter() {
    if (!session_ || !openedChapterId_ || openedChapterFile_.empty()) { Notice(tr("本战役尚未打开章节副本。")); return false; }
    if (mainProject_.sourceArchivePath_ == openedChapterFile_ && mainProject_.HasUnsavedMapChanges()) {
        Notice(tr("请先保存当前章节地图，再更新到战役。")); return false;
    }
    const auto& maps = session_->Info().maps;
    const auto map = std::find_if(maps.begin(), maps.end(), [this](const auto& item) { return item.id == openedChapterId_; });
    if (map == maps.end()) { Notice(tr("此章节已从战役移除，副本保留；请使用“添加地图”重新导入。")); return false; }
    const auto source = session_->ReadMember(session_->Revision(), map->path);
    if (!source.Succeeded() || !source.value) return Failed(source.diagnostics);
    const auto sourceHash = Sha256(*source.value);
    if (!sourceHash.Succeeded() || !sourceHash.value) return Failed(sourceHash.diagnostics);
    if (*sourceHash.value != openedChapterSourceSha_) { Notice(tr("战役内章节已在其他入口替换，拒绝用旧章节副本覆盖；请导出副本后重新比较。")); return false; }
    const auto updatedHash = Sha256File(openedChapterFile_);
    if (!updatedHash.Succeeded() || !updatedHash.value) return Failed(updatedHash.diagnostics);
    Services::CampaignCommand command; command.operation = Services::CampaignOperation::ReplaceMap;
    command.id = openedChapterId_; command.mapSource = openedChapterFile_;
    if (!ApplyCommand(command)) return false;
    openedChapterSourceSha_ = *updatedHash.value; chapterSavedPending_ = false;
    Notice(tr("章节已更新到战役，下一次保存战役将包含此章节。")); emit Changed(); return true;
}
void CampaignModule::NoteChapterSaved(const std::filesystem::path& path) {
    if (openedChapterId_ && path == openedChapterFile_) {
        chapterSavedPending_ = true; emit Changed();
        Notice(tr("章节副本已保存，但尚未更新到战役。请点击“更新已打开章节”，然后保存战役。"));
    }
}
bool CampaignModule::HasUncommittedChapter() const {
    return openedChapterId_ && (chapterSavedPending_ || (mainProject_.sourceArchivePath_ == openedChapterFile_ && mainProject_.HasUnsavedMapChanges()));
}
} // namespace War3Map::Modules
