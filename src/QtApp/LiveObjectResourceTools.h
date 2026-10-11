#pragma once
#include "ObjectResourcePickerAdapter.h"
#include <QObject>
#include <QJsonObject>
#include <array>

namespace War3Map::QtApp {
class LiveObjectResourceTools final : public QObject {
public:
    explicit LiveObjectResourceTools(QtProject::EditorProject& project, QObject* parent = nullptr);
    [[nodiscard]] QJsonObject Invoke(const QString& name, const QJsonObject& arguments,
        const std::function<QJsonObject()>& identity);
private:
    using CatalogResult = Result<std::shared_ptr<const Services::ObjectResourceCatalog>>;
    CatalogResult Load(const Modules::ObjectResourcePickerServices& services, Services::ObjectResourceOrigin origin);
    QtProject::EditorProject& project_;
    Modules::ObjectResourcePickerFactory factory_;
    std::string revision_;
    std::array<std::optional<CatalogResult>, 3> catalogs_;
    // 冷目录索引在后台构建；查询返回明确pending，Agent用同一身份重试。
    std::array<QPointer<QThread>, 3> workers_;
};
}
