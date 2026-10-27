#pragma once

#include "ObjectNames.h"
#include "Widgets/WarcraftTextPreview.h"
#include "War3MapCore/ObjectData/LevelAutofill.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "Widgets/ObjectCatalogWidgets.h"
#include "Widgets/ObjectResourcePicker.h"

#include <QAbstractTableModel>
#include <QIcon>
#include <QPointer>

#include <QtWidgets/QStyledItemDelegate>

#include <functional>
#include <optional>
#include <unordered_set>

class QUndoStack;
[[nodiscard]] QString ObjectFieldLabel(const War3Map::ObjectData::ObjectFieldDefinition* definition, bool raw = false);
[[nodiscard]] QString ObjectFieldCategory(const War3Map::ObjectData::ObjectFieldDefinition* definition);

class ObjectListModel final : public QAbstractTableModel {
public:
    explicit ObjectListModel(QObject* parent = nullptr);

    void SetDocument(const War3Map::ObjectData::LniObjectDocument* document);
    void MarkNewObject(const std::string& rawcode);
    using PresentationResolver = std::function<std::pair<QString, QIcon>(const War3Map::ObjectData::ObjectRecord&)>;
    void SetPresentationResolver(PresentationResolver resolver);
    void RefreshPresentation();
    [[nodiscard]] const War3Map::ObjectData::ObjectRecord* ObjectAt(int row) const;
    [[nodiscard]] bool IsNewObject(const std::string& rawcode) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;

private:
    const War3Map::ObjectData::LniObjectDocument* document_ = nullptr;
    std::unordered_set<std::string> newObjects_;
    PresentationResolver presentationResolver_;
};

class ObjectFieldModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    using ValuePresentationResolver = std::function<QString(const War3Map::ObjectData::ObjectFieldDefinition*, const QString&)>;
    void SetValuePresentationResolver(ValuePresentationResolver resolver);
    [[nodiscard]] QString PresentedValue(const War3Map::ObjectData::ObjectFieldDefinition* definition, const QString& value) const;
    // 文本字段的值是字符串表引用时，编辑框显示引用的正文；未改动则保留原引用，改动后写入新文字。
    using StringReferenceResolver = std::function<std::optional<std::string>(std::string_view)>;
    void SetStringReferenceResolver(StringReferenceResolver resolver) { stringReference_ = std::move(resolver); }
    [[nodiscard]] std::optional<std::string> ReferencedTextAt(int row) const;
    using ColourPreviewFactory = std::function<War3Map::UiKit::WarcraftTextSourceResolver()>;
    void SetColourPreviewFactory(ColourPreviewFactory factory);
    [[nodiscard]] War3Map::UiKit::WarcraftTextSourceResolver PrepareColourPreview() const;
    explicit ObjectFieldModel(QObject* parent = nullptr);

    void SetDocument(
        War3Map::ObjectData::LniObjectDocument* document,
        const War3Map::ObjectData::ObjectKey* object);
    void SetEditCallback(std::function<void()> callback);
    // 宿主在每次发布前复核有效目录；无宿主的离线模型只执行字段与结构校验。
    void SetWriteGate(std::function<bool()> gate) { writeGate_ = std::move(gate); }
    void SetFieldFilter(const QString& filter);
    void SetContext(const War3Map::ObjectData::StandardObjectCatalog* catalog,
        const War3Map::ObjectData::ObjectFieldSchema* schema);
    void SetShowRawFields(bool raw);
    void SetCategoryFilter(const QString& category);
    void SetUndoStack(QUndoStack* stack);
    [[nodiscard]] QStringList Categories() const;
    [[nodiscard]] int FieldCount() const;
    [[nodiscard]] QString InheritanceIssue() const { return inheritanceIssue_; }
    [[nodiscard]] bool CanReset(int row) const;
    bool ResetField(int row);
    [[nodiscard]] const War3Map::ObjectData::ObjectFieldDefinition* DefinitionAt(int row) const;
    [[nodiscard]] std::optional<War3Map::ObjectData::Value> EditorValueAt(int row) const;

    // 自动填充等级：同一字段（字段编号、数据列、变体相同）在当前对象上的各等级，不受搜索/分类过滤影响。
    // 不是多等级字段或字段不能按数值/文本填充时无类型。
    [[nodiscard]] std::optional<War3Map::ObjectData::LevelAutofillKind> LevelAutofillKindAt(int row) const;
    [[nodiscard]] std::vector<std::pair<std::uint32_t, QString>> LevelValuesOf(int row) const;
    // 一次撤销写入多个等级；任一等级只读、缺失或值不符合字段类型与范围时整批不写，返回原因（成功为空）。
    QString SetLevelValues(int row, const std::vector<std::pair<std::uint32_t, QString>>& values, const QString& label);

    // 获取指定模型行对应的修改记录（过滤模式下自动映射）。
    [[nodiscard]] const War3Map::ObjectData::ObjectModification* ModificationAt(
        int row) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

private:
    struct FieldRow {
        War3Map::ObjectData::ObjectModification effective;
        War3Map::ObjectData::ModificationKey canonicalKey;
        const War3Map::ObjectData::ObjectFieldDefinition* definition = nullptr;
        std::optional<War3Map::ObjectData::Value> baseline;
        QString valueSource;
        QString baselineSource;
        bool overridden = false;
        bool editable = false;
        QString reason;
    };
    [[nodiscard]] const War3Map::ObjectData::ObjectRecord* CurrentObject() const;
    void RebuildRows();
    void CommitSnapshot(War3Map::ObjectData::LniObjectDocument next, const QString& label);
    [[nodiscard]] const FieldRow* RowAt(int row) const;
    [[nodiscard]] std::vector<std::size_t> LevelFamily(const FieldRow& field) const;
    // 编辑文本按字段元数据转换并检查最小/最大值；setData 与多等级写入共用。
    [[nodiscard]] std::optional<War3Map::ObjectData::Value> ConvertedValue(const FieldRow& field, const QVariant& input) const;

    War3Map::ObjectData::LniObjectDocument* document_ = nullptr;
    std::optional<War3Map::ObjectData::ObjectKey> object_;
    std::function<void()> editCallback_;
    std::function<bool()> writeGate_;
    QString fieldFilter_;
    QString categoryFilter_;
    QString inheritanceIssue_;
    bool showRawFields_ = false;
    ValuePresentationResolver valuePresentation_;
    ColourPreviewFactory colourPreviewFactory_;
    StringReferenceResolver stringReference_;
    const War3Map::ObjectData::StandardObjectCatalog* catalog_ = nullptr;
    const War3Map::ObjectData::ObjectFieldSchema* schema_ = nullptr;
    QPointer<QUndoStack> undoStack_;
    std::vector<FieldRow> rows_;
    std::vector<std::size_t> visible_;
};

// 按字段值类型提供合适编辑器（int→SpinBox, float→DoubleSpinBox, bool→CheckBox, 其余→LineEdit）。
class ObjectFieldDelegate final : public QStyledItemDelegate {
public:
    explicit ObjectFieldDelegate(QObject* parent = nullptr);
    void SetReferenceContext(War3Map::QtApp::IconProvider* icons,
        const QVector<War3Map::QtApp::ObjectViewEntry>& entries);
    void SetResourcePickerFactory(War3Map::Modules::ObjectResourcePickerFactory factory) { resourcePickerFactory_ = std::move(factory); }

    [[nodiscard]] QWidget* createEditor(
        QWidget* parent,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(
        QWidget* editor,
        QAbstractItemModel* model,
        const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
        const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    War3Map::QtApp::IconProvider* icons_ = nullptr;
    QVector<War3Map::QtApp::ObjectViewEntry> entries_;
    QPointer<QWidget> referenceEditor_;
    War3Map::Modules::ObjectResourcePickerFactory resourcePickerFactory_;
};
