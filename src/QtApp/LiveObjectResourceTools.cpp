#include "LiveObjectResourceTools.h"
#include "McpObjectResourceTools.h"
#include "McpToolInternal.h"
#include "EditorProject.h"
#include <QThread>
#include <exception>

namespace War3Map::QtApp {
LiveObjectResourceTools::LiveObjectResourceTools(QtProject::EditorProject& project, QObject* parent)
    : QObject(parent), project_(project), factory_(MakeObjectResourcePickerFactory(project)) {}
LiveObjectResourceTools::CatalogResult LiveObjectResourceTools::Load(const Modules::ObjectResourcePickerServices& services,
    Services::ObjectResourceOrigin origin) {
    const auto index = static_cast<std::size_t>(origin);
    if (catalogs_[index]) return *catalogs_[index];
    if (!workers_[index]) {
        auto result = std::make_shared<CatalogResult>();
        auto* worker = QThread::create([load = services.load, result, origin] {
            try { *result = load(origin, {}); }
            catch (const std::exception& error) { *result = CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_INDEX_FAILED", error.what()); }
        });
        workers_[index] = worker;
        connect(worker, &QThread::finished, this, [this, worker, result, index, revision = services.revision] {
            if (workers_[index] != worker) return;
            workers_[index] = nullptr;
            if (revision_ == revision) catalogs_[index] = std::move(*result);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }
    return CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_INDEX_PENDING", "正在后台建立资源目录；用同一资源身份重试查询。");
}
Result<Services::ObjectResourceInfo> LiveObjectResourceTools::Inspect(const Modules::ObjectResourcePickerServices& services,
    std::string_view value, Services::ObjectResourceKind kind) {
    using Return = Result<Services::ObjectResourceInfo>;
    auto key = std::pair{kind, std::string(value)};
    if (const auto found = inspected_.find(key); found != inspected_.end()) return found->second;
    if (!services.inspect) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_INSPECT_UNAVAILABLE", "来源读取接口未接通。");
    if (!inspectWorker_) {
        auto result = std::make_shared<Return>();
        auto* worker = QThread::create([inspect = services.inspect, ownedValue = key.second, kind, result] {
            try { *result = inspect(ownedValue, kind); }
            catch (const std::exception& error) { *result = Return::Failure("Object.Resource", "OBJECT_RESOURCE_INSPECT_FAILED", error.what()); }
        });
        inspectWorker_ = worker;
        connect(worker, &QThread::finished, this, [this, worker, result, key = std::move(key), revision = services.revision] {
            if (inspectWorker_ != worker) return;
            inspectWorker_ = nullptr;
            if (revision_ != revision) return;
            if (inspected_.size() >= 256) inspected_.clear();
            inspected_.insert_or_assign(key, std::move(*result));
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
    }
    return Return::Failure("Object.Resource", "OBJECT_RESOURCE_INSPECT_PENDING", "正在后台读取资源信息；用同一资源身份重试查询。");
}
QJsonObject LiveObjectResourceTools::Invoke(const QString& name, const QJsonObject& arguments,
    const std::function<QJsonObject()>& identity) {
    const auto checked = project_.CheckProjectSources();
    if (!checked.Succeeded()) return Mcp::Failure(checked);
    const auto services = factory_();
    if (services.revision.empty() || !services.isCurrent || !services.isCurrent())
        return Mcp::Failure("OBJECT_RESOURCE_HOST_MISSING", "活动工程没有可信资源来源。");
    if (revision_ != services.revision) {
        revision_ = services.revision; catalogs_ = {}; inspected_.clear();
        // 老任务仅持有冻结输入，完成后丢弃。每来源同时只保留一个后台任务。
    }
    Mcp::ObjectResourceToolHost host;
    host.identity = [&] {
        auto state = identity(); state.insert("resource_revision", QString::fromStdString(factory_().revision)); return state;
    };
    host.load = [&](auto origin) { return Load(services, origin); };
    host.inspect = [&](auto value, auto kind) { return Inspect(services, value, kind); };
    host.field = [&](auto type, auto id) -> Result<Services::ObjectResourceField> {
        const auto* definition = project_.objectFieldSchema_ ? project_.objectFieldSchema_->FindNative(type, id) : nullptr;
        const auto field = definition ? Services::DescribeObjectResourceField(*definition) : std::optional<Services::ObjectResourceField>{};
        if (!field) return Result<Services::ObjectResourceField>::Failure("Object.Resource", "OBJECT_RESOURCE_FIELD_UNSUPPORTED", "字段不存在或不是资源字段。");
        return Result<Services::ObjectResourceField>::Success(*field);
    };
    auto result = Mcp::InvokeObjectResourceTool(name, arguments, host);
    if (!services.isCurrent()) return Mcp::Failure("OBJECT_RESOURCE_HOST_CHANGED", "查询期间资源来源已变化。");
    return result;
}
}
