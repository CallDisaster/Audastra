#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/ProjectSearch.h"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::QtApp {

// 带 project_identity / expected_revision 的工具 schema 增加 binding_token，并把身份字段（及 expected）移出必填；
// 移出的字段写入 tokenFields，由调用入口在未给令牌时检查。
QJsonObject WithBindingToken(QJsonObject schema, QStringList* tokenFields = nullptr);

// 嵌入 AI 和外部 MCP 共用的活动文档适配。借用唯一 GUI project，
// 不打开另一份 McpWorkspace；所有调用须在 GUI 线程串行。
class LiveEditorTools final : public QObject {
    Q_OBJECT
public:
    using ProjectSearchRevisionQuery = std::function<std::string()>;
    using ProjectSearchPageQuery = std::function<Result<Services::ProjectSearchPage>(
        std::string_view expectedContextRevision, std::string_view query,
        const Services::ProjectSearchOptions& options, std::size_t pageLimit,
        const Services::ProjectSearchCursor* cursor)>;
    explicit LiveEditorTools(QtProject::EditorProject& project, QObject* parent = nullptr);
    ~LiveEditorTools() override;
    [[nodiscard]] Mcp::McpServer& Server() noexcept;
    void SetSaveHandler(std::function<bool(const std::filesystem::path&, bool)> handler);
    void SetValidationSaveHandler(std::function<bool(const std::filesystem::path&)> handler);
    void SetBeforeWrite(std::function<bool()> handler);
    void SetLuaTestHandler(std::function<QJsonObject(bool submit)> handler);
    // map_test_start / map_test_stop / map_test_eval：参数已校验，返回工具结果。
    void SetMapTestHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler);
    void SetWorldViewportHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler);
    void SetGameUiHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler);
    // 可信窗口注入与 UI 相同的同步 owned-page 查询；不存借用 context 或跨调用 span。
    void SetProjectSearchQuery(ProjectSearchRevisionQuery revision, ProjectSearchPageQuery query);
    void SetPolicy(Mcp::ToolPolicy policy);
    void SetSkillRoot(const std::filesystem::path& root);
    [[nodiscard]] QJsonObject Invoke(const QString& name, const QJsonObject& arguments);
    [[nodiscard]] QJsonArray Tools();
    [[nodiscard]] QStringList RegisteredToolNames() const;

signals:
    void Activity(const QJsonObject& event);
    void Changed();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace War3Map::QtApp
