#pragma once

#include "War3MapCore/Services/ObjectResourceQuery.h"

#include <QFrame>
#include <QPointer>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

namespace War3Map::Modules {
struct ObjectResourcePreviewHandle {
    QWidget* widget = nullptr;
    std::function<void(const QString&, Services::ObjectResourceKind)> show;
};
struct ObjectResourcePickerServices {
    std::string revision;
    // 捕获时已冻结输入；后台线程不能读取可变工程或 QObject。
    std::function<Result<std::shared_ptr<const Services::ObjectResourceCatalog>>(
        Services::ObjectResourceOrigin, const std::function<bool(std::size_t)>&)> load;
    std::function<bool()> isCurrent;
    std::function<Result<Services::ObjectResourceInfo>(std::string_view, Services::ObjectResourceKind)> inspect;
    std::function<ObjectResourcePreviewHandle(QWidget*)> preview;
};
using ObjectResourcePickerFactory = std::function<ObjectResourcePickerServices()>;

class ObjectResourcePicker final : public QFrame {
    Q_OBJECT
public:
    ObjectResourcePicker(Services::ObjectResourceField field, QString original,
        ObjectResourcePickerServices services, QWidget* parent = nullptr);
    ~ObjectResourcePicker() override;
    // Apply 返回 false 时保留草稿并显示原因；唯一写入路径由原字段命令提供。
    void SetCommit(std::function<bool(const QString&)> commit);
    [[nodiscard]] QString Value() const;
signals:
    void ValueCommitted(const QString& value);
private:
    void SelectSource();
    void StartLoad(Services::ObjectResourceOrigin origin);
    void Requery();
    void AppendPage();
    void Choose(QTreeWidgetItem* item, bool activate);
    void ShowPreview(const QString& value);
    void AddOrReplace();
    void MoveSelected(int delta);
    void Apply();
    Services::ObjectResourceField field_;
    QString original_;
    QStringList originalItems_;
    ObjectResourcePickerServices services_;
    std::function<bool(const QString&)> commit_;
    ObjectResourcePreviewHandle preview_;
    QTabWidget* tabs_ = nullptr;
    QComboBox* browse_ = nullptr;
    QComboBox* category_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLineEdit* path_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* location_ = nullptr;
    QListWidget* selected_ = nullptr;
    QTreeWidget* candidates_ = nullptr;
    QPushButton* more_ = nullptr;
    std::shared_ptr<const Services::ObjectResourceCatalog> catalogs_[3];
    std::shared_ptr<const Services::ObjectResourceCatalog> catalog_;
    std::optional<Services::ObjectResourceCursor> cursor_;
    Services::ObjectResourceOrigin origin_ = Services::ObjectResourceOrigin::Object;
    QString directory_;
    QPointer<QThread> worker_;
    std::shared_ptr<std::atomic_bool> alive_;
    std::uint64_t request_ = 0;
    bool replacing_ = false;
};
} // namespace War3Map::Modules
