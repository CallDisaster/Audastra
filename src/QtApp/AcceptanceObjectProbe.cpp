#include "MainWindow.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <QStandardItemModel>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 无界面验收：七类物编各改一个名称和一个确切的复杂字段，单位另改一个数值字段；
// 走界面同一另存路径（含脚本/模板重建），再从候选归档回读核对。
namespace {
using namespace War3Map::ObjectData;

// 各类的名称字段（与原版 metadata 的 ID 一致）。
constexpr std::array<const char*, 7> kNameFields{"unam", "anam", "unam", "fnam", "gnam", "dnam", "bnam"};

struct PlannedEdit {
    ObjectType type;
    ObjectKey object;
    ModificationKey field;
    Value value;
    bool exactRoundTrip = false;
};

struct RequiredComplexField {
    ObjectType type;
    std::string rawcode;
    std::optional<std::string> parent;
    ModificationKey field;
    Value expected;
    Value replacement;
};

const ObjectModification* FindModification(const ObjectRecord& record, const ModificationKey& key) {
    for (const auto& modification : record.modifications)
        if (modification.key == key) return &modification;
    return nullptr;
}

std::string FieldLabel(const ModificationKey& field) {
    std::string result = field.fieldId;
    if (!field.level && !field.variation && !field.dataIndex) return result;
    result += '[';
    if (field.variation) result += "variation=" + std::to_string(*field.variation);
    else if (field.level) result += "level=" + std::to_string(*field.level);
    if (field.dataIndex) {
        if (field.level || field.variation) result += ',';
        result += "data=" + std::to_string(*field.dataIndex);
    }
    result += ']';
    return result;
}

// 取第一个已有该字段覆盖的自定义对象；复用其完整键（含等级/变体），不猜测字段形状。
std::optional<PlannedEdit> PlanField(const LniObjectDocument& document, std::string_view fieldId, bool numeric) {
    for (const auto& record : document.Objects()) {
        if (!record.parentRawcode) continue;
        for (const auto& modification : record.modifications) {
            if (modification.key.fieldId != fieldId || modification.valueWasOpaque) continue;
            // 名称可能是 TRIGSTR 引用；写入可在游戏里直接辨认的字面名称。
            if (!numeric && std::holds_alternative<std::string>(modification.value))
                return PlannedEdit{document.Type(), record.key, modification.key,
                    Value{"FORGE " + record.key.rawcode}};
            if (numeric && std::holds_alternative<std::int32_t>(modification.value))
                return PlannedEdit{document.Type(), record.key, modification.key,
                    Value{std::get<std::int32_t>(modification.value) + 1}};
        }
    }
    return std::nullopt;
}
} // namespace

bool MainWindow::ProbeObjectEditAndSave(const std::filesystem::path& outputPath, QStringList& log) {
    if (outputPath.empty() || !project_->archiveSession_) {
        log << QStringLiteral("object-probe: 缺少源归档或输出路径");
        return false;
    }
    std::vector<PlannedEdit> edits;
    for (std::size_t index = 0; index < kNameFields.size(); ++index) {
        const auto& document = project_->objectDocuments_[index];
        if (!document) {
            log << QStringLiteral("object-probe: 类别 %1 没有物编文档，跳过").arg(index);
            continue;
        }
        if (auto name = PlanField(*document, kNameFields[index], false)) edits.push_back(std::move(*name));
        else log << QStringLiteral("object-probe: 类别 %1 没有带名称覆盖的自定义对象，跳过").arg(index);
        if (index == 0) {
            if (auto life = PlanField(*document, "uhpm", true)) edits.push_back(std::move(*life));
        }
    }
    // WP2 的冻结作者图样本：检查精确对象、父对象、字段键和值后才动内存状态。
    // 缺项说明样本已变化，不能退回到某个“相似对象”而冒称完成分类验收。
    const std::array required{
        RequiredComplexField{ObjectType::Unit, "u03G", std::string{"uarb"},
            ModificationKey{"uabi", {}, {}, {}},
            Value{std::string{"ACrk,A0RZ,A0RX,A0S3,A0RR,A0RL,A0RM,A0RP,A0SA,S007,A0RK,A0RK,A0RJ"}},
            Value{std::string{"A0RZ,ACrk,A0RX,A0S3,A0RR,A0RL,A0RM,A0RP,A0SA,S007,A0RK,A0RK,A0RJ"}}},
        RequiredComplexField{ObjectType::Ability, "A03Y", std::string{"ANcl"},
            ModificationKey{"Ncl1", 1u, 1u, {}}, Value{0.5f}, Value{0.6f}},
        RequiredComplexField{ObjectType::Item, "I002", std::string{"ratf"},
            ModificationKey{"iabi", {}, {}, {}}, Value{std::string{"A0W5,A0IW,A0IW,A00L"}},
            Value{std::string{"A00L,A0IW,A0IW,A0W5"}}},
        RequiredComplexField{ObjectType::Buff, "B044", std::string{"Bblo"},
            ModificationKey{"fta0", {}, {}, {}}, Value{std::string{"weapon,rear,left"}},
            Value{std::string{"weapon,rear,right"}}},
        RequiredComplexField{ObjectType::Upgrade, "R009", std::string{"Rhlh"},
            ModificationKey{"gnam", 2u, 0u, {}}, Value{std::string{"TRIGSTR_11305"}},
            Value{std::string{"FORGE R009 L2"}}},
        RequiredComplexField{ObjectType::Doodad, "D031", std::string{"YOgr"},
            // LNI 的 [0,0,1] 含占位 level=0；native 回灌会将它还原为 variation=1。
            ModificationKey{"dvr1", 0u, 0u, 1u}, Value{std::int32_t{50}}, Value{std::int32_t{70}}},
        RequiredComplexField{ObjectType::Destructable, "ITtg", {},
            // 来源 native 值是 5.0f；LNI 解析把无小数的文本 5 归为 Int32。
            ModificationKey{"bmas", {}, {}, {}}, Value{std::int32_t{5}}, Value{5.2f}},
    };
    for (const auto& sample : required) {
        const auto index = static_cast<std::size_t>(sample.type);
        const auto& document = project_->objectDocuments_[index];
        const ObjectKey key{sample.type, sample.rawcode};
        const auto* record = document ? document->FindObject(key) : nullptr;
        const auto* modification = record ? FindModification(*record, sample.field) : nullptr;
        if (!record || record->parentRawcode != sample.parent || !modification
            || modification->valueWasOpaque || modification->value != sample.expected) {
            const QString actual = !record ? QStringLiteral("对象不存在")
                : record->parentRawcode != sample.parent
                    ? QStringLiteral("parent=%1").arg(QString::fromStdString(record->parentRawcode.value_or("<original>")))
                : !modification ? QStringLiteral("字段不存在")
                : modification->valueWasOpaque ? QStringLiteral("字段为不透明值")
                : QStringLiteral("actual=%1, value-type=%2, expected-type=%3")
                    .arg(QString::fromUtf8(ValueToDisplayString(modification->value)))
                    .arg(modification->value.index()).arg(sample.expected.index());
            log << QStringLiteral("object-probe: 必需复杂字段缺失或原值变化：%1 %2.%3（%4）；未写出候选")
                .arg(QString::fromUtf8(std::string(ToString(sample.type))),
                    QString::fromStdString(sample.rawcode),
                    QString::fromStdString(FieldLabel(sample.field)), actual);
            return false;
        }
        edits.push_back({sample.type, key, sample.field, sample.replacement, true});
    }
    if (edits.empty()) {
        log << QStringLiteral("object-probe: 没有可改的自定义对象");
        return false;
    }
    for (const auto& edit : edits) {
        const auto index = static_cast<std::size_t>(edit.type);
        LniObjectDocument next = *project_->objectDocuments_[index];
        const auto status = next.SetModification(edit.object, edit.field, edit.value);
        if (!status.Succeeded()) {
            log << QStringLiteral("object-probe: 修改 %1.%2 失败").arg(QString::fromStdString(edit.object.rawcode),
                QString::fromStdString(edit.field.fieldId));
            for (const auto& diagnostic : status.diagnostics) log << QString::fromUtf8(diagnostic.message);
            return false;
        }
        *project_->objectDocuments_[index] = std::move(next);
        project_->objectDirty_[index] = true;
        project_->ObserveObjectEdit(static_cast<int>(index));
        log << QStringLiteral("object-probe: 修改 %1 %2.%3 = %4").arg(QString::fromUtf8(std::string(ToString(edit.type))),
            QString::fromStdString(edit.object.rawcode), QString::fromStdString(FieldLabel(edit.field)),
            QString::fromUtf8(ValueToDisplayString(edit.value)));
    }
    const int before = project_->diagnosticsModel_->rowCount();
    const bool saved = SaveMapArchiveToPath(outputPath);
    for (int row = before; row < project_->diagnosticsModel_->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < project_->diagnosticsModel_->columnCount(); ++column)
            cells << project_->diagnosticsModel_->item(row, column)->text();
        log << cells.join(QStringLiteral(" | ")).left(600);
    }
    log << QStringLiteral("object-probe: saved=%1").arg(saved ? QStringLiteral("true") : QStringLiteral("false"));
    if (!saved) return false;

    // 从候选归档独立回读：native → LNI → 查找对象与字段，值必须与编辑一致。
    const auto candidate = War3Map::MapArchiveSession::Open(outputPath);
    if (!candidate.Succeeded() || !candidate.value) {
        log << QStringLiteral("object-probe: 候选无法重开");
        return false;
    }
    bool allMatch = true;
    for (const auto& edit : edits) {
        const auto member = candidate.value->ReadMember(std::string(NativeFileName(edit.type)));
        bool matches = false;
        if (member.Succeeded() && member.value) {
            const auto lni = ConvertNativeObjectDataToLni(edit.type, *member.value);
            if (lni.Succeeded()) {
                auto parsed = LniObjectCodec(edit.type).Parse(*lni.value);
                const auto* document = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
                const auto* record = document ? document->FindObject(edit.object) : nullptr;
                const auto* modification = record ? FindModification(*record, edit.field) : nullptr;
                matches = modification && (edit.exactRoundTrip
                    ? modification->value == edit.value
                    : ValueToDisplayString(modification->value) == ValueToDisplayString(edit.value));
            }
        }
        allMatch = allMatch && matches;
        log << QStringLiteral("object-probe: 回读 %1 %2.%3 %4").arg(QString::fromUtf8(std::string(ToString(edit.type))),
            QString::fromStdString(edit.object.rawcode), QString::fromStdString(FieldLabel(edit.field)),
            matches ? QStringLiteral("一致") : QStringLiteral("不一致"));
    }
    log << QStringLiteral("object-probe: edits=%1 reopened=%2").arg(edits.size())
        .arg(allMatch ? QStringLiteral("true") : QStringLiteral("false"));
    return allMatch;
}
