#include "ObjectModule.h"
#include "Widgets/ObjectTreeBrowser.h"

#include "EditorProject.h"
#include "ObjectModels.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "Icons/IconProvider.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QTableView>
#include <QTabWidget>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <optional>

namespace War3Map::Modules {

using namespace ObjectData;

namespace {
QString U8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// 文档地址稳定；创建与撤销均使用完整的行/对象快照，保留未识别内容。
class CreateObjectCommand final : public QUndoCommand {
public:
    CreateObjectCommand(LniObjectDocument* document, LniObjectDocument next,
        QString title, std::function<void()> refresh)
        : QUndoCommand(std::move(title)), document_(document), before_(*document),
          after_(std::move(next)), refresh_(std::move(refresh)) {}
    void undo() override { *document_ = before_; refresh_(); }
    void redo() override { *document_ = after_; refresh_(); }
private:
    LniObjectDocument* document_;
    LniObjectDocument before_, after_;
    std::function<void()> refresh_;
};
}

bool ObjectModule::EnsureObjectDocument(int index) {
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size())) return false;
    const auto fail = [this](const QString& message) {
        ShowStatus(message, 7000);
        project_->AddDiagnostic(tr("警告"), tr("物体"), tr("OBJECT_DOCUMENT_UNAVAILABLE"), message);
        return false;
    };
    if (project_->objectDocuments_[index]) {
        if (!project_->objectDocuments_[index]->IsStructurallySafe()) return fail(tr("此类物编结构无法安全编辑"));
        if (!project_->sourceArchivePath_.empty() && (!project_->nativeObjectDocuments_[index] || !project_->nativeObjectDocuments_[index]->IsEditable()))
            return fail(tr("此类物编没有可编辑的原生基线，不能以空文件代替"));
        return true;
    }
    if (project_->sourceArchivePath_.empty()) return fail(tr("请先打开地图，再创建此类物体"));
    auto opened = War3Map::Mpq::MpqArchive::OpenReadOnly(project_->sourceArchivePath_);
    if (!opened.Succeeded() || !opened.value) return fail(tr("无法读取当前地图归档"));
    const auto type = static_cast<ObjectType>(index);
    if (opened.value->HasFile(NativeFileName(type)))
        return fail(tr("地图中已有此类物编文件，但未能完整载入；原文件保持不变"));
    const auto inventory = opened.value->ListFilesStrict();
    if (!inventory.Succeeded()) return fail(tr("无法确认地图成员完整性，暂不能新增物编文件"));
    auto native = NativeObjectDocument::CreateEmpty(type, 2);
    auto lni = LniObjectCodec(type).Parse({});
    if (!native.Succeeded() || !native.value || !lni.Succeeded()) return fail(tr("无法创建经典物编文档"));
    auto* document = dynamic_cast<LniObjectDocument*>(lni.document.get());
    if (!document) return fail(tr("物编文档类型错误"));
    project_->objectDocuments_[index] = std::make_unique<LniObjectDocument>(std::move(*document));
    project_->nativeObjectDocuments_[index] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
    // 只分配内存；用户产生修改且另存成功前，不写入任何地图成员。
    return true;
}

void ObjectModule::SelectMapObject(int index, const QString& rawcode) {
    if (currentObjectType_ != index) objectCategories_->setCurrentRow(index);
    if (currentObjectType_ != index) ShowObjectType(index);
    objectSourceTabs_->setCurrentIndex(1);
    mapObjectSearch_->clear();
    for (int row = 0; row < objectFilter_->rowCount(); ++row) {
        const auto candidate = objectFilter_->index(row, 0);
        if (candidate.data(Qt::UserRole).toString() == rawcode) {
            objectListView_->setCurrentIndex(candidate);
            objectListView_->scrollTo(candidate);
            ShowSelectedObject();
            return;
        }
    }
    objectListView_->setCurrentIndex({});
    ShowSelectedObject();
}

void ObjectModule::RefreshObjectDocument(int index, const QString& preferredRawcode) {
    project_->objectDirty_[index] = true;
    project_->ObserveObjectEdit(index);
    if (currentObjectType_ == index) {
        objectFieldModel_->SetDocument(nullptr, nullptr);
        objectListModel_->SetDocument(project_->objectDocuments_[index].get());
    }
    UpdateObjectReferenceEntries();
    if (currentObjectType_ == index) SelectMapObject(index, preferredRawcode);
    UpdateObjectFieldControls();
    emit DocumentEdited(index);
    emit DocumentsChanged();
}

void ObjectModule::CommitObjectCreation(int index, LniObjectDocument next,
    const QString& rawcode, const QString& label) {
    project_->objectUndoStacks_[index]->push(new CreateObjectCommand(project_->objectDocuments_[index].get(),
        std::move(next), label, [this, index, rawcode]() { RefreshObjectDocument(index, rawcode); }));
}

QString ObjectModule::SuggestObjectId(int index, const QString& baseRawcode) const {
    const auto type = static_cast<ObjectType>(index);
    const QChar prefix = baseRawcode.isEmpty() ? QChar('X') : baseRawcode.front();
    for (int counter = 0; counter < 46656; ++counter) {
        const auto candidate = prefix + QString::number(counter, 36).toUpper().rightJustified(3, QChar('0'));
        if (project_->standardCatalog_ && project_->standardCatalog_->Find(type, candidate.toStdString())) continue;
        if (project_->objectDocuments_[index] && project_->objectDocuments_[index]->FindObject({type, candidate.toStdString()})) continue;
        return candidate;
    }
    return {};
}

bool ObjectModule::CreateObjectFromBase(int index, const QString& baseRawcode,
    const QString& rawcode, const QString& name) {
    if (index < 0 || index >= 7 || !project_->standardCatalog_) return false;
    const auto type = static_cast<ObjectType>(index);
    const auto* base = project_->standardCatalog_->Find(type, baseRawcode.toStdString());
    if (!base || project_->standardCatalog_->Find(type, rawcode.toStdString()) || !EnsureObjectDocument(index)) return false;
    auto next = *project_->objectDocuments_[index];
    const auto added = next.AddObject(rawcode.toStdString(), baseRawcode.toStdString());
    if (!added.Succeeded()) return false;
    if (!name.trimmed().isEmpty()) {
        if (!project_->objectFieldSchema_) return false;
        ObjectFieldContext context;
        context.rawcode = base->key.rawcode;
        context.baseCode = std::string(base->Field("code"));
        context.item = type == ObjectType::Item;
        context.hero = base->category == "hero";
        context.building = base->category == "building";
        auto match = project_->objectFieldSchema_->MatchSourceField(type, "name", context);
        if (!match.definition) match = project_->objectFieldSchema_->MatchSourceField(type, "name", context, 0u);
        if (!match.definition || !match.safeToEdit || match.definition->nativeValueType != NativeValueType::String) return false;
        if (!next.SetModification({type, rawcode.toStdString()}, match.modification, name.trimmed().toStdString()).Succeeded()) return false;
    }
    CommitObjectCreation(index, std::move(next), rawcode, tr("创建 %1").arg(rawcode));
    SelectMapObject(index, rawcode);
    ShowStatus(tr("已创建 %1 · %2").arg(name.isEmpty() ? U8(base->displayName) : name, rawcode), 5000);
    return true;
}

bool ObjectModule::CloneCustomObject(int index, const QString& sourceRawcode,
    const QString& newRawcode) {
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size()) || !project_->standardCatalog_)
        return false;
    if (newRawcode.size() != 4 || !newRawcode.contains(QRegularExpression(QStringLiteral("^[A-Za-z0-9]{4}$"))))
        return false;
    const auto type = static_cast<ObjectType>(index);
    if (project_->standardCatalog_->Find(type, newRawcode.toStdString()) || !EnsureObjectDocument(index))
        return false;
    const ObjectKey source{type, sourceRawcode.toStdString()};
    const auto* original = project_->objectDocuments_[index]->FindObject(source);
    if (!original || !original->parentRawcode) return false;
    auto next = *project_->objectDocuments_[index];
    const auto cloned = next.CloneCustomObject(source, newRawcode.toStdString());
    if (!cloned.Succeeded()) {
        for (const auto& diagnostic : cloned.diagnostics)
            project_->AddDiagnostic(tr("错误"), tr("物体"), U8(diagnostic.code), U8(diagnostic.message));
        return false;
    }
    CommitObjectCreation(index, std::move(next), newRawcode, tr("复制 %1 为 %2").arg(sourceRawcode, newRawcode));
    SelectMapObject(index, newRawcode);
    ShowStatus(tr("已复制 %1 → %2").arg(sourceRawcode, newRawcode), 5000);
    return true;
}

void ObjectModule::DuplicateSelectedCustomObject() {
    const int index = currentObjectType_;
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size())
        || !objectListView_ || !objectFilter_ || !project_->objectDocuments_[index]) return;
    const QModelIndex selected = objectListView_->currentIndex();
    if (!selected.isValid()) return;
    const auto* object = objectListModel_->ObjectAt(objectFilter_->mapToSource(selected).row());
    if (!object || !object->parentRawcode) return;
    const QString sourceRawcode = U8(object->key.rawcode);
    bool accepted = false;
    const QString newRawcode = QInputDialog::getText(page_, tr("复制自定义物体"),
        tr("新对象 ID"), QLineEdit::Normal, SuggestObjectId(index, sourceRawcode), &accepted).trimmed();
    if (!accepted) return;
    if (!CloneCustomObject(index, sourceRawcode, newRawcode))
        ShowStatus(tr("复制失败 · 检查四位 ID 是否重复，或查看诊断"), 7000);
}

void ObjectModule::EditSelectedStandardObject() {
    const auto selected = standardObjectBrowser_->CurrentEntry();
    if (!selected || !project_->standardCatalog_) return;
    const auto type = static_cast<ObjectType>(currentObjectType_);
    if (!project_->standardCatalog_->Find(type, selected->id.toStdString()) || !EnsureObjectDocument(currentObjectType_)) return;
    if (!project_->objectDocuments_[currentObjectType_]->FindObject({type, selected->id.toStdString()})) {
        auto next = *project_->objectDocuments_[currentObjectType_];
        if (!next.AddOriginalObject(selected->id.toStdString()).Succeeded()) return;
        CommitObjectCreation(currentObjectType_, std::move(next), selected->id,
            tr("编辑标准对象 %1").arg(selected->name));
    }
    SelectMapObject(currentObjectType_, selected->id);
}

bool ObjectModule::CommitStandardFieldValue(const QString& rawcode, const QString& sourceField,
    const QString& value) {
    const int index = currentObjectType_;
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size()) ||
        !project_->standardCatalog_ || !project_->objectFieldSchema_ || project_->projectRoot_.empty()) return false;
    const auto type = static_cast<ObjectType>(index);
    const auto* base = project_->standardCatalog_->Find(type, rawcode.toStdString());
    if (!base) return false;
    const ObjectKey key{type, rawcode.toStdString()};
    const auto* current = project_->objectDocuments_[index].get();
    if (current && !current->IsStructurallySafe()) return false;
    if (const auto* record = current ? current->FindObject(key) : nullptr;
        record && record->parentRawcode) return false;

    // 先在临时文档中使用地图字段模型的类型、范围和原生地址校验。
    // 只有真正改变字段后才分配原生基线并提交覆盖，取消或无效输入不留空对象。
    std::optional<LniObjectDocument> candidate;
    if (current) candidate.emplace(*current);
    else {
        auto parsed = LniObjectCodec(type).Parse({});
        auto* empty = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
        if (!empty || !empty->IsStructurallySafe()) return false;
        candidate.emplace(*empty);
    }
    if (!candidate->FindObject(key) && !candidate->AddOriginalObject(key.rawcode).Succeeded()) return false;

    ObjectFieldContext context;
    context.rawcode = base->key.rawcode;
    context.baseCode = std::string(base->Field("code"));
    if (context.baseCode.empty()) context.baseCode = base->key.rawcode;
    context.hero = base->category == "hero";
    context.building = base->category == "building";
    context.item = type == ObjectType::Item;
    const auto match = project_->objectFieldSchema_->MatchSourceField(type, sourceField.toStdString(), context);
    if (!match.definition || !match.safeToEdit) return false;

    ObjectFieldModel trial;
    trial.SetContext(project_->standardCatalog_.get(), project_->objectFieldSchema_.get());
    trial.SetDocument(&*candidate, &key);
    int fieldRow = -1;
    for (int row = 0; row < trial.rowCount(); ++row) {
        const auto* modification = trial.ModificationAt(row);
        if (modification && modification->key == match.modification) {
            if (fieldRow >= 0) return false; // 多个源字段指向同一地址时拒绝猜测。
            fieldRow = row;
        }
    }
    if (fieldRow < 0 || !(trial.flags(trial.index(fieldRow, 3)) & Qt::ItemIsEditable)) return false;
    const auto before = candidate->DocumentFingerprint();
    QString editedValue = value;
    for (const auto& choice : match.definition->choices) {
        if (value == U8(choice.displayName)) {
            editedValue = U8(choice.value);
            break;
        }
    }
    if (!trial.setData(trial.index(fieldRow, 3), editedValue, Qt::EditRole) ||
        candidate->DocumentFingerprint() == before) return false;
    if (!EnsureObjectDocument(index)) return false;
    // 确认真实文档在校验期间没有被换掉；最终只产生当前分类的一条撤销记录。
    if (current && current != project_->objectDocuments_[index].get()) return false;
    CommitObjectCreation(index, std::move(*candidate), rawcode,
        tr("修改标准对象 %1 的 %2").arg(rawcode, U8(match.definition->displayName)));
    ShowStatus(tr("已为 %1 创建地图字段覆盖 · 原版数据保持只读").arg(rawcode), 5000);
    return true;
}

void ObjectModule::CreateNewCustomObject() {
    const int typeAtOpen = currentObjectType_;
    if (!project_->standardCatalog_ || !EnsureObjectDocument(typeAtOpen)) return;
    auto* documentAtOpen = project_->objectDocuments_[typeAtOpen].get();
    if (!newObjectPicker_) newObjectPicker_ = new War3Map::QtApp::ObjectPickerPopup(project_->iconProvider_.get(), page_);
    newObjectPicker_->disconnect(this);
    QVector<War3Map::QtApp::ObjectViewEntry> bases;
    for (const auto& base : project_->standardCatalog_->Entries()) {
        if (base.key.type != static_cast<ObjectType>(typeAtOpen)) continue;
        War3Map::QtApp::ObjectViewEntry entry;
        entry.kind = U8(ToString(base.key.type)); entry.id = U8(base.key.rawcode);
        entry.name = U8(base.displayName); entry.iconPath = U8(base.iconPath);
        entry.category = U8(base.category); entry.race = U8(base.race);
        bases.push_back(std::move(entry));
    }
    newObjectPicker_->SetEntries(bases);
    newObjectPicker_->SetKindFilter(U8(ToString(static_cast<ObjectType>(typeAtOpen))));
    newObjectPicker_->SetTitle(tr("选择基础对象"));
    connect(newObjectPicker_, &War3Map::QtApp::ObjectPickerPopup::EntryChosen, this,
        [this, documentAtOpen, typeAtOpen](const War3Map::QtApp::ObjectViewEntry& entry) {
            if (currentObjectType_ != typeAtOpen || project_->objectDocuments_[typeAtOpen].get() != documentAtOpen) return;
            QDialog dialog(page_);
            dialog.setWindowTitle(tr("新建自定义物体"));
            dialog.setMinimumWidth(390);
            auto* layout = new QVBoxLayout(&dialog);
            auto* heading = new QLabel(tr("%1 · %2").arg(entry.name, entry.id));
            heading->setTextFormat(Qt::PlainText);
            heading->setObjectName("workspaceTitle");
            layout->addWidget(heading);
            auto* form = new QFormLayout;
            auto* name = new QLineEdit(entry.name);
            name->setObjectName("newObjectName");
            auto* rawcode = new QLineEdit(SuggestObjectId(typeAtOpen, entry.id));
            rawcode->setObjectName("newObjectId");
            rawcode->setMaxLength(4);
            rawcode->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9]{4}")), rawcode));
            form->addRow(tr("名称"), name);
            form->addRow(tr("对象 ID"), rawcode);
            layout->addLayout(form);
            auto* error = new QLabel;
            error->setTextFormat(Qt::PlainText);
            error->setWordWrap(true);
            layout->addWidget(error);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            buttons->button(QDialogButtonBox::Ok)->setText(tr("创建"));
            buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this]() {
                if (project_->objectDocuments_[typeAtOpen].get() != documentAtOpen) { dialog.reject(); return; }
                if (!rawcode->hasAcceptableInput() || name->text().trimmed().isEmpty()) {
                    error->setText(tr("填写名称和四位字母数字 ID")); return;
                }
                if (!CreateObjectFromBase(typeAtOpen, entry.id, rawcode->text(), name->text())) {
                    error->setText(tr("无法创建：检查 ID 是否重复，以及名称字段定义是否可用")); return;
                }
                dialog.accept();
            });
            name->selectAll(); name->setFocus();
            dialog.exec();
        });
    newObjectPicker_->OpenAt(objectTree_);
}

} // namespace War3Map::Modules
