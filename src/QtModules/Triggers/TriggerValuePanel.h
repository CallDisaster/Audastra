#pragma once

#include "TriggerFunctionList.h"

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"

#include <QFrame>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListWidget;
class QPushButton;
class QTabBar;
class QIcon;

// 取值面板的选择结果。valueKind：0 预设，1 变量，2 函数，3 直接值。
struct TriggerValueChoice {
    std::int32_t valueKind = 3;
    std::string value;
    std::optional<std::string> callFunction;
    // 新挂接函数自身的参数（按位置），例如 YDWE 局部变量读取的变量名。
    std::vector<std::pair<std::int32_t, std::string>> callArguments;
    // 非空：先新建该类型的全局变量 value，再作为参数使用（与参数修改同一次撤销）。
    std::optional<std::string> newVariableType;
    // 预放对象来自只读密封快照；提交前须重捕获并匹配，普通值保持为空。
    std::string mapObjectSnapshotIdentity;
};

struct TriggerValueObject {
    QString rawcode;
    QString name;
};

// 取值面板（对应原型 SharedValuePicker）：点击语句中的参数后贴在参数下方的非模态浮层。
// 期望类型由类型系统解析（YDWE 的 Null / typename / objecttype、原版 SetVariable），各页始终可用：
//   预设 / 变量（全局变量 + YDWE 局部变量 loc_ / loop_）/ 函数（含 AnyReturnType）/ 值或物体。
// 当前值独立显示于顶部；函数参数可以从该区编辑，也可直接换成其他来源。Enter 使用，Esc 关闭。
class TriggerValuePanel final : public QFrame {
public:
    using ObjectLister = std::function<std::vector<TriggerValueObject>(const std::string& uiType)>;

    TriggerValuePanel(const War3Map::Trigger::TriggerDocument& document,
        const War3Map::Trigger::UiCatalog& catalog,
        War3Map::StableId ecaId,
        std::size_t position,
        ObjectLister objects,
        TriggerFunctionList::CategoryIcon categoryIcon = {},
        QWidget* parent = nullptr,
        std::shared_ptr<const War3Map::Trigger::MapObjectReferenceSnapshot> mapObjects = {},
        QString mapObjectFailure = {});
    ~TriggerValuePanel() override;

    void SetChosen(std::function<void(const TriggerValueChoice&)> chosen) { chosen_ = std::move(chosen); }
    void SetReferencePresentation(const War3Map::Trigger::ReferencePresentation& shown, const QIcon& icon,
        std::optional<QString> stringBody = std::nullopt);
    // 参数由函数提供时：打开该函数调用自身的参数。
    void SetEditCall(std::function<void(War3Map::StableId callEcaId)> editCall) { editCall_ = std::move(editCall); }
    // 贴在 anchor（全局坐标）下方；下方空间不足时翻到上方。
    void ShowAt(const QRect& anchor);
    // 解析后的期望类型（Null 解析不了时仍为 "Null"）。
    [[nodiscard]] const std::string& ExpectedType() const noexcept { return type_; }
    // 应用当前选中项（Enter / 双击）；供测试直接调用。
    void Apply();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 记住用户拖动后的尺寸（本机设置），下次打开沿用。
    void hideEvent(QHideEvent* event) override;

private:
    enum class Page { Preset, Variable, Function, Value };
    void BuildPages();
    void SelectPage(Page page);
    void Rebuild();
    void RebuildVariables(const QString& query);
    void RebuildFunctions(const QString& query);
    void RebuildValues(const QString& query);
    void SelectCurrent();
    void Choose(const TriggerValueChoice& choice);
    [[nodiscard]] bool Numeric() const;
    [[nodiscard]] bool Boolean() const;
    [[nodiscard]] bool TextLike() const;

    const War3Map::Trigger::TriggerDocument& document_;
    const War3Map::Trigger::UiCatalog& catalog_;
    std::shared_ptr<const War3Map::Trigger::MapObjectReferenceSnapshot> mapObjects_;
    QString mapObjectFailure_;
    bool writableTarget_ = false;
    std::unique_ptr<War3Map::Trigger::TriggerTypeSystem> types_;
    TriggerFunctionList functions_;
    ObjectLister objects_;
    std::vector<TriggerValueObject> objectItems_;
    std::vector<War3Map::Trigger::LocalVariable> locals_;
    const War3Map::Trigger::EcaNode* eca_ = nullptr;
    std::string declared_; // 定义中的声明类型（scriptcode、AnyGlobal 等决定页签）
    std::string type_;     // 解析后的期望类型
    const War3Map::Trigger::ArgumentNode* argument_ = nullptr;
    const War3Map::Trigger::EcaNode* currentCall_ = nullptr;
    std::vector<Page> pages_;
    Page page_ = Page::Value;
    QTabBar* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPlainTextEdit* literalText_ = nullptr;
    std::optional<QString> originalStringBody_;
    QString initialStringInput_;
    QListWidget* list_ = nullptr;
    QLabel* note_ = nullptr;
    QPushButton* create_ = nullptr;
    QPushButton* editCallButton_ = nullptr;
    std::function<void(const TriggerValueChoice&)> chosen_;
    std::function<void(War3Map::StableId)> editCall_;
    // 面板底部“新建变量并使用”按钮当前的动作。
    std::function<void()> createAction_;
};
