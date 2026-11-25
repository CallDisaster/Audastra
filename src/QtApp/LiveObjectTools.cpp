#include "LiveObjectTools.h"
#include "EditorProject.h"
#include "ObjectContextProvider.h"
#include "ObjectPlanCommit.h"
#include "McpObjectTools.h"
#include "McpToolInternal.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Workspace.h"

#include <array>
#include <memory>
#include <optional>

namespace War3Map::QtApp {
namespace {
using namespace ObjectData;
using QtProject::EditorProject;

class LiveObjectHost {
public:
    LiveObjectHost(EditorProject& project, QJsonObject identity) : project_(project), identity_(std::move(identity)),
        context_(QtProject::MakeObjectContextProvider(project)) {}
    Mcp::ObjectToolHost Host() {
        return {
            [this] {
                return QJsonObject{{"project_identity", identity_.value("project_identity")},
                    {"revision", QString::number(project_.editRevision_.Revision())}};
            },
            [this](ObjectType type) { return Context(type); },
            [this](const Services::ObjectCommandPlan& plan) { return Commit(plan); },
            [this](const ObjectKey& key) { return project_.AssessDeletion(key); }
        };
    }
    Result<Services::ObjectCommandContext> Context(ObjectType type) {
        return context_(type);
    }
private:
    Status Commit(const Services::ObjectCommandPlan& plan) {
        // 先按查询时的工程修订拒绝过期上下文，再交给共用提交（核对当前文档并入该分类撤销栈）。
        const auto current = Context(plan.subject.type);
        if (!current.Succeeded()) return {current.diagnostics};
        if (!current.value->document || current.value->document->DocumentFingerprint() != plan.before.DocumentFingerprint())
            return Status::Failure("Object.Live", "OBJECT_PLAN_STALE", "命令不属于当前物编文档；地图未修改。");
        return QtProject::CommitObjectCommandPlan(project_, plan);
    }
    EditorProject& project_;
    QJsonObject identity_;
    QtProject::ObjectContextProvider context_;
};
} // namespace

LiveObjectContextProvider MakeLiveObjectContextProvider(EditorProject& project, const QJsonObject& currentIdentity) {
    Q_UNUSED(currentIdentity);
    return QtProject::MakeObjectContextProvider(project);
}

QJsonObject InvokeLiveObjectTool(EditorProject& project, const QString& name, const QJsonObject& arguments,
    const QJsonObject& currentIdentity) {
    LiveObjectHost host(project, currentIdentity);
    return Mcp::InvokeObjectTool(name, arguments, host.Host());
}

} // namespace War3Map::QtApp
