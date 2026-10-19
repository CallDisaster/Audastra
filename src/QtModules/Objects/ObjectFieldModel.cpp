#include "ObjectModels.h"
#include "ObjectModelsDetail.h"

#include <exception>
#include <utility>

using namespace War3Map::ObjectData;
using namespace ObjectModelsDetail;

// 地图覆盖字段模型：显示、编辑、重置与撤销。

ObjectFieldModel::ObjectFieldModel(QObject* parent) : QAbstractTableModel(parent) {}
void ObjectFieldModel::SetColourPreviewFactory(ColourPreviewFactory factory) {
    colourPreviewFactory_ = std::move(factory);
    if (rowCount()) emit dataChanged(index(0, 3), index(rowCount() - 1, 3), {Qt::ToolTipRole});
}
War3Map::UiKit::WarcraftTextSourceResolver ObjectFieldModel::PrepareColourPreview() const {
    // 用户打开一个编辑器时才显式准备；普通 data()/刷新从不建立 WTS 快照。
    const auto factory = colourPreviewFactory_;
    try { return factory ? factory() : War3Map::UiKit::WarcraftTextSourceResolver{}; }
    catch (const std::exception&) {
        return [](const QString&) { return War3Map::UiKit::WarcraftTextSource{{},
            QObject::tr("当前 WTS 预览准备失败；原字段值保持不变。"), War3Map::UiKit::WarcraftTextSourceState::Unknown}; };
    }
}
void ObjectFieldModel::SetValuePresentationResolver(ValuePresentationResolver resolver) {
    valuePresentation_ = std::move(resolver);
    if (rowCount()) emit dataChanged(index(0, 2), index(rowCount() - 1, 3), {Qt::DisplayRole});
}
QString ObjectFieldModel::PresentedValue(const ObjectFieldDefinition* definition, const QString& value) const {
    return !showRawFields_ && valuePresentation_ ? valuePresentation_(definition, value) : value;
}

QString ObjectFieldLabel(const ObjectFieldDefinition* definition, bool raw) {
    if (!definition) return {};
    return ToQString(raw || definition->displayName.empty() ? definition->fieldId : definition->displayName);
}

QString ObjectFieldCategory(const ObjectFieldDefinition* definition) {
    if (!definition) return QObject::tr("未分类");
    const auto& category = definition->categoryDisplayName.empty() ? definition->category : definition->categoryDisplayName;
    return category.empty() ? QObject::tr("未分类") : ToQString(category);
}

void ObjectFieldModel::SetDocument(LniObjectDocument* document, const ObjectKey* object) {
    document_ = document;
    object_ = object ? std::optional<ObjectKey>{*object} : std::nullopt;
    RebuildRows();
}

void ObjectFieldModel::SetEditCallback(std::function<void()> callback) { editCallback_ = std::move(callback); }
void ObjectFieldModel::SetFieldFilter(const QString& filter) { fieldFilter_ = filter.trimmed(); RebuildRows(); }
void ObjectFieldModel::SetContext(const StandardObjectCatalog* catalog, const ObjectFieldSchema* schema) {
    catalog_ = catalog;
    schema_ = schema;
    RebuildRows();
}
void ObjectFieldModel::SetShowRawFields(bool raw) { showRawFields_ = raw; RebuildRows(); }
void ObjectFieldModel::SetCategoryFilter(const QString& category) { categoryFilter_ = category; RebuildRows(); }
void ObjectFieldModel::SetUndoStack(QUndoStack* stack) { undoStack_ = stack; }
const ObjectRecord* ObjectFieldModel::CurrentObject() const {
    return document_ && object_ ? document_->FindObject(*object_) : nullptr;
}

void ObjectFieldModel::RebuildRows() {
    beginResetModel();
    rows_.clear();
    visible_.clear();
    inheritanceIssue_.clear();
    const ObjectRecord* object = CurrentObject();
    if (object) {
        const ObjectInheritanceChain inheritance = ResolveObjectInheritance(*document_, object->key, catalog_);
        inheritanceIssue_ = inheritance.reason;
        const StandardObjectEntry* base = inheritance.standardBase;
        const std::string baseId = base ? base->key.rawcode : object->key.rawcode;
        ObjectFieldContext context;
        context.rawcode = baseId;
        if (base) {
            context.baseCode = std::string(base->Field("code"));
            if (context.baseCode.empty()) context.baseCode = baseId;
            const auto flag = [base](std::string_view key) -> std::optional<bool> {
                const auto value = base->Field(key);
                if (value == "1") return true;
                if (value == "0") return false;
                return {};
            };
            context.hero = flag("hero");
            context.building = flag("isbldg");
            if (object->key.type == ObjectType::Unit) {
                context.hero = base->category == "hero";
                context.building = base->category == "building";
            }
            context.item = object->key.type == ObjectType::Item;
            context.creep = flag("creep");
        }
        const auto makeMapRow = [&](const ObjectModification& modification, const ObjectRecord& owner) {
            FieldRow row;
            row.effective = modification;
            row.canonicalKey = modification.key;
            row.overridden = owner.key == object->key;
            row.valueSource = tr("地图对象 %1").arg(ToQString(owner.key.rawcode));
            if (schema_) {
                auto match = schema_->MatchNativeField(object->key.type, modification.key, context);
                if (!match.definition) match = schema_->MatchSourceField(object->key.type, modification.key.fieldId, context);
                row.definition = match.definition;
                if (match.definition) row.canonicalKey = match.modification;
                row.editable = match.safeToEdit;
                row.reason = ToQString(match.reason);
                if (row.editable && !SchemaValue(row.definition, ToQString(ValueToDisplayString(modification.value)))) {
                    row.editable = false;
                    row.reason = tr("字段值无法按原版元数据类型解析");
                }
            } else {
                // 无游戏资料仍允许已知原生四字节字段的既有值编辑，不创建猜测映射。
                row.editable = modification.key.fieldId.size() == 4;
                row.reason = tr("未加载字段元数据");
            }
            row.editable = row.editable && !modification.valueWasOpaque &&
                !std::holds_alternative<UnknownBytesValue>(modification.value) && document_->IsStructurallySafe();
            if (!row.overridden && row.definition && row.editable) row.effective.key = row.canonicalKey;
            return row;
        };
        // 保留全部子对象修改的源键与次序；新的继承覆盖使用已确认的原生地址。
        for (const auto& modification : object->modifications) {
            rows_.push_back(makeMapRow(modification, *object));
        }
        if (base && schema_) {
            // 标准目录是只读字符串快照；只有可靠的 metadata 地址参与继承编辑。
            for (const auto& [sourceKey, sourceValue] : base->fields) {
                const QStringList components = SplitComponents(ToQString(sourceValue));
                std::vector<ObjectFieldMatch> matches;
                auto match = schema_->MatchSourceField(object->key.type, sourceKey, context);
                if (match.definition && match.safeToEdit) matches.push_back(match);
                else {
                    for (qsizetype part = 0; part < components.size(); ++part) {
                        auto indexed = schema_->MatchSourceField(object->key.type, sourceKey, context, static_cast<std::uint32_t>(part));
                        if (indexed.definition && indexed.safeToEdit) matches.push_back(std::move(indexed));
                    }
                }
                for (const auto& field : matches) {
                    QString raw = ToQString(sourceValue);
                    if (field.sourceIndex) {
                        if (*field.sourceIndex >= static_cast<std::uint32_t>(components.size())) continue;
                        raw = components.at(static_cast<qsizetype>(*field.sourceIndex));
                    } else if (ToQString(field.definition->sourceTable).compare(QStringLiteral("profile"), Qt::CaseInsensitive) == 0 && !components.isEmpty()) {
                        raw = components.join(QChar(','));
                    }
                    auto baseline = SchemaValue(field.definition, raw);
                    if (!baseline) continue;
                    auto existing = std::find_if(rows_.begin(), rows_.end(), [&field](const FieldRow& row) {
                        return row.canonicalKey == field.modification;
                    });
                    if (existing != rows_.end()) {
                        if (existing->baseline && *existing->baseline != *baseline) {
                            existing->editable = false;
                            existing->reason = tr("多个原版源字段映射到同一地址且值不一致");
                            continue;
                        }
                        existing->baseline = std::move(baseline);
                        existing->baselineSource = tr("基础对象 %1").arg(ToQString(base->key.rawcode));
                        continue;
                    }
                    FieldRow row;
                    row.canonicalKey = field.modification;
                    row.effective.key = field.modification;
                    row.effective.sourceKey = sourceKey;
                    row.effective.value = *baseline;
                    row.definition = field.definition;
                    row.baseline = std::move(baseline);
                    row.valueSource = row.baselineSource = tr("基础对象 %1").arg(ToQString(base->key.rawcode));
                    row.editable = document_->IsStructurallySafe();
                    rows_.push_back(std::move(row));
                }
            }
        }
        // 远父先合并，近父后覆盖；子对象已有字段只更新其重置目标，不覆盖子值。
        for (std::size_t position = inheritance.records.size(); position > 1; --position) {
            const ObjectRecord& parent = *inheritance.records[position - 1];
            std::vector<ModificationKey> seen;
            for (const auto& modification : parent.modifications) {
                FieldRow parentRow = makeMapRow(modification, parent);
                const bool duplicate = std::find(seen.begin(), seen.end(), parentRow.canonicalKey) != seen.end();
                seen.push_back(parentRow.canonicalKey);
                auto existing = std::find_if(rows_.begin(), rows_.end(), [&](const FieldRow& row) {
                    return row.canonicalKey == parentRow.canonicalKey;
                });
                if (duplicate) {
                    if (existing != rows_.end()) {
                        existing->editable = false;
                        existing->baseline.reset();
                        existing->reason = tr("父对象 %1 有多个字段映射到同一地址").arg(ToQString(parent.key.rawcode));
                    }
                    continue;
                }
                parentRow.baseline = parentRow.effective.value;
                parentRow.baselineSource = parentRow.valueSource;
                if (existing == rows_.end()) rows_.push_back(std::move(parentRow));
                else if (existing->overridden) {
                    existing->baseline = parentRow.effective.value;
                    existing->baselineSource = parentRow.valueSource;
                    // 近父的明确值遮蔽远父的歧义；仍保留子字段自身的类型/地址限制。
                    const FieldRow ownRow = makeMapRow(existing->effective, *object);
                    existing->editable = ownRow.editable && parentRow.editable;
                    existing->reason = ownRow.reason;
                    if (!parentRow.editable) {
                        existing->reason = tr("父对象 %1 的覆盖无法安全解析：%2")
                            .arg(ToQString(parent.key.rawcode), parentRow.reason);
                    }
                } else *existing = std::move(parentRow);
            }
        }
        if (base && !base->nativeOverridesSupported) {
            for (auto& row : rows_) {
                row.editable = false;
                row.reason = tr("基础对象使用地图或战役 SLK/profile；当前只支持读取，尚未支持此来源的原生覆盖写回。");
            }
        }
        if (!inheritance.IsComplete()) {
            for (auto& row : rows_) {
                // 无目录的旧独立LNI文档仍可编辑自身已知值；未提供任何继承值。
                // 已加载目录后的缺失/循环，以及无目录下的多层继承均禁止修改。
                const bool ownValueWithoutCatalog = inheritance.catalogUnavailable &&
                    inheritance.records.size() == 1 && !schema_ && row.overridden &&
                    (!object->parentRawcode || object->parentRawcode->empty() || *object->parentRawcode == object->key.rawcode);
                if (!ownValueWithoutCatalog) row.editable = false;
                row.reason = inheritance.reason;
            }
        }
        std::stable_sort(rows_.begin() + static_cast<std::ptrdiff_t>(object->modifications.size()), rows_.end(), [](const FieldRow& left, const FieldRow& right) {
            const QString a = ObjectFieldCategory(left.definition) + ObjectFieldLabel(left.definition) + IndexText(left.canonicalKey);
            const QString b = ObjectFieldCategory(right.definition) + ObjectFieldLabel(right.definition) + IndexText(right.canonicalKey);
            return a < b;
        });
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            for (std::size_t j = i + 1; j < object->modifications.size(); ++j) {
                if (rows_[i].canonicalKey == rows_[j].canonicalKey) {
                    rows_[i].editable = rows_[j].editable = false;
                    rows_[i].reason = rows_[j].reason = tr("多个地图字段映射到同一原生地址");
                }
            }
        }
        // 同一字段的多个源映射不能静默合并；显示已有记录，新增覆盖由唯一canonical地址定位。
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const FieldRow& row = rows_[i];
            const QString category = ObjectFieldCategory(row.definition);
            const QString text = ObjectFieldLabel(row.definition) + QChar(' ') +
                ToQString(row.effective.key.fieldId) + QChar(' ') + ToQString(row.effective.sourceKey) +
                QChar(' ') + ToQString(ValueToDisplayString(row.effective.value)) + QChar(' ') + category;
            if ((!categoryFilter_.isEmpty() && category != categoryFilter_) ||
                (!fieldFilter_.isEmpty() && !text.contains(fieldFilter_, Qt::CaseInsensitive))) continue;
            visible_.push_back(i);
        }
    }
    endResetModel();
}

const ObjectFieldModel::FieldRow* ObjectFieldModel::RowAt(int row) const {
    return row >= 0 && static_cast<std::size_t>(row) < visible_.size() ? &rows_[visible_[static_cast<std::size_t>(row)]] : nullptr;
}

QStringList ObjectFieldModel::Categories() const {
    QStringList result;
    for (const auto& row : rows_) {
        const QString category = ObjectFieldCategory(row.definition);
        if (!result.contains(category)) result.append(category);
    }
    result.sort();
    return result;
}
int ObjectFieldModel::FieldCount() const { return static_cast<int>(rows_.size()); }
int ObjectFieldModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(visible_.size()); }
int ObjectFieldModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 5; }
const ObjectModification* ObjectFieldModel::ModificationAt(int row) const {
    const auto* field = RowAt(row);
    return field ? &field->effective : nullptr;
}
const ObjectFieldDefinition* ObjectFieldModel::DefinitionAt(int row) const {
    const auto* field = RowAt(row);
    return field ? field->definition : nullptr;
}
std::optional<std::string> ObjectFieldModel::ReferencedTextAt(int row) const {
    const auto* field = RowAt(row);
    if (!field || !stringReference_ || !std::holds_alternative<std::string>(field->effective.value)) return std::nullopt;
    return stringReference_(std::get<std::string>(field->effective.value));
}

std::optional<Value> ObjectFieldModel::EditorValueAt(int row) const {
    const auto* field = RowAt(row);
    if (!field) return {};
    if (field->definition) {
        auto typed = SchemaValue(field->definition, ToQString(ValueToDisplayString(field->effective.value)));
        if (typed) {
            if (IsBoolean(field->definition)) return Value{std::get<std::int32_t>(*typed) != 0};
            return typed;
        }
    }
    return field->effective.value;
}

QVariant ObjectFieldModel::data(const QModelIndex& index, int role) const {
    const auto* field = RowAt(index.row());
    if (!field || !index.isValid()) return {};
    const QString raw = ToQString(field->effective.key.fieldId);
    const QString label = field->definition ? ObjectFieldLabel(field->definition, showRawFields_) : raw;
    if (role == Qt::UserRole) return raw;
    if (role == Qt::ForegroundRole) {
        if (!field->editable) return QBrush(QColor(QStringLiteral("#8996a5")));
        if (field->overridden) return QBrush(QColor(0xd8, 0x6c, 0xd8)); // 与原版一致：修改过的字段为品红色
        return {};
    }
    if (role == Qt::ToolTipRole) {
        QString tooltip = tr("%1\n字段: %2\n索引: %3\n继承值: %4\n当前来源: %5\n继承来源: %6\n%7%8")
            .arg(label, raw, IndexText(field->canonicalKey),
                field->baseline ? ToQString(ValueToDisplayString(*field->baseline)) : tr("未解析"),
                field->valueSource,
                field->baselineSource.isEmpty() ? tr("未解析") : field->baselineSource,
                field->overridden ? tr("已修改") : tr("默认"),
                field->editable ? QString{} : QChar('\n') + (field->reason.isEmpty() ? tr("只读") : field->reason));
        if (IsMultilineTextField(field->definition, EditorValueAt(index.row()))) {
            const auto referenced = ReferencedTextAt(index.row());
            tooltip += tr("\n\n完整内容:\n%1").arg(ToQString(referenced ? *referenced : ValueToDisplayString(field->effective.value)));
        }
        return tooltip;
    }
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    switch (index.column()) {
    case 0:
        return showRawFields_ ? raw : ObjectFieldCategory(field->definition) + QStringLiteral(" — ") + label
            + (IndexText(field->canonicalKey) == QStringLiteral("—") ? QString{} : QStringLiteral(" · ") + IndexText(field->canonicalKey));
    case 1: return IndexText(field->canonicalKey);
    case 2: {
        const auto value = field->baseline ? ToQString(ValueToDisplayString(*field->baseline)) : ToQString(field->effective.originalValueToken);
        return role == Qt::DisplayRole ? PresentedValue(field->definition, value) : value;
    }
    case 3:
        if (role == Qt::DisplayRole && IsBoolean(field->definition)) {
            const auto value = EditorValueAt(index.row());
            if (value && std::holds_alternative<bool>(*value)) return std::get<bool>(*value) ? tr("是") : tr("否");
        }
        if (role == Qt::DisplayRole && IsMultilineTextField(field->definition, EditorValueAt(index.row()))) {
            QString preview = PresentedValue(field->definition, ToQString(ValueToDisplayString(field->effective.value)));
            return preview.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
                .replace(QChar('\n'), QStringLiteral("  ↵  "));
        }
        return role == Qt::DisplayRole ? PresentedValue(field->definition, ToQString(ValueToDisplayString(field->effective.value)))
            : ToQString(ValueToDisplayString(field->effective.value));
    case 4: return !field->editable ? tr("只读") : field->overridden ? tr("已修改") : tr("默认");
    default: return {};
    }
}

QVariant ObjectFieldModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QAbstractTableModel::headerData(section, orientation, role);
    static const QStringList headers{tr("字段"), tr("索引"), tr("继承值"), tr("值"), tr("状态")};
    return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
}

Qt::ItemFlags ObjectFieldModel::flags(const QModelIndex& index) const {
    auto result = QAbstractTableModel::flags(index);
    const auto* field = RowAt(index.row());
    if (field && field->editable && index.column() == 3) result |= Qt::ItemIsEditable;
    return result;
}

void ObjectFieldModel::CommitSnapshot(LniObjectDocument next, const QString& label) {
    const QPointer<ObjectFieldModel> guard(this);
    LniObjectDocument* target = document_;
    auto apply = [guard, target](const LniObjectDocument& snapshot) {
        if (!guard) return;
        *target = snapshot; // 指针稳定：视图/保存基线不悬空。宿主须在销毁文档前清对应撤销栈。
        if (guard->document_ == target) guard->RebuildRows();
        if (guard->editCallback_) guard->editCallback_();
    };
    if (undoStack_) undoStack_->push(new ObjectSnapshotCommand(*document_, std::move(next), std::move(apply), label));
    else apply(next);
}

bool ObjectFieldModel::setData(const QModelIndex& index, const QVariant& input, int role) {
    if (role == Qt::EditRole && writeGate_ && !writeGate_()) return false;
    const auto* field = RowAt(index.row());
    if (role != Qt::EditRole || index.column() != 3 || !field || !field->editable || !document_ || !object_) return false;
    // 编辑框里是引用的正文；原样提交表示没有修改，保留 TRIGSTR 引用。
    if (const auto referenced = ReferencedTextAt(index.row());
        referenced && input.metaType().id() == QMetaType::QString && input.toString().toStdString() == *referenced) return true;
    QVariant value = input;
    if (field->definition && !field->definition->referenceTypes.empty()) {
        const QStringList ids = input.metaType().id() == QMetaType::QStringList ? input.toStringList()
            : input.toString().isEmpty() ? QStringList{} : input.toString().split(QChar(','), Qt::KeepEmptyParts);
        // 既有未知ID保留；新增只能是四字节ASCII标识，不去重、不改变次序。
        const QStringList oldIds = ReferenceIds(field->effective.value);
        if (ids == oldIds) return true;
        for (const auto& id : ids) if (!ValidRawcode(id) && !oldIds.contains(id)) return false;
        if (!field->definition->isList && ids.size() > 1) return false;
        value = ids.join(QChar(','));
    }
    auto converted = field->definition ? SchemaValue(field->definition, value) : ConvertEditedValue(field->effective.value, value);
    if (!converted) return false;
    if (field->definition && (std::holds_alternative<std::int32_t>(*converted) || std::holds_alternative<float>(*converted))) {
        const double numeric = std::holds_alternative<float>(*converted) ? std::get<float>(*converted) : std::get<std::int32_t>(*converted);
        bool valid = false;
        const double minimum = ToQString(field->definition->minimum).toDouble(&valid);
        if (valid && numeric < minimum) return false;
        const double maximum = ToQString(field->definition->maximum).toDouble(&valid);
        if (valid && numeric > maximum) return false;
    }
    // 既有bool文档保留值类型；native bool是整数。
    if (std::holds_alternative<bool>(field->effective.value) && IsBoolean(field->definition))
        converted = Value{std::get<std::int32_t>(*converted) != 0};
    if (ValueToDisplayString(*converted) == ValueToDisplayString(field->effective.value)) return true;
    LniObjectDocument next = *document_;
    const auto status = next.SetModification(*object_, field->effective.key, *converted);
    if (!status.Succeeded()) return false;
    CommitSnapshot(std::move(next), tr("修改 %1").arg(field->definition ? ObjectFieldLabel(field->definition) : ToQString(field->effective.key.fieldId)));
    return true;
}

bool ObjectFieldModel::CanReset(int row) const {
    const auto* field = RowAt(row);
    return field && field->overridden && field->editable && field->baseline.has_value();
}
bool ObjectFieldModel::ResetField(int row) {
    if (writeGate_ && !writeGate_()) return false;
    if (!CanReset(row) || !document_ || !object_) return false;
    const auto* field = RowAt(row);
    LniObjectDocument next = *document_;
    if (!next.RemoveModification(*object_, field->effective.key).Succeeded()) return false;
    CommitSnapshot(std::move(next), tr("恢复继承值"));
    return true;
}

