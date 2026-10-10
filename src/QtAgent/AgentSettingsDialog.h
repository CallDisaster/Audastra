#pragma once

#include "AgentTypes.h"
#include "ApplicationSettingsTransfer.h"

#include <QDialog>

#include <functional>
#include <memory>

namespace War3Map::Agent {

class CredentialStore;

class AgentSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AgentSettingsDialog(const AgentSettings& settings, QWidget* parent = nullptr);
    ~AgentSettingsDialog() override;
    [[nodiscard]] AgentSettings Settings() const;
    void SetAvailableTools(const QStringList& tools);
    void SetMcpAccessToken(const QString& token);
    void SetCredentialStore(CredentialStore* store);
    // 界面字体是应用设置（不进 AI 配置文件）：空字符串表示默认微软雅黑。
    void SetUiFontFamily(const QString& family);
    [[nodiscard]] QString UiFontFamily() const;
    void SetApplicationPreferences(const ApplicationPreferences& application);
    [[nodiscard]] ApplicationPreferences ApplicationSettings() const;
    // 导入仅暂存；保存仍由现有commit回调统一处理。取消不触碰本机配置/凭据。
    [[nodiscard]] bool ImportSettingsData(const QByteArray& bytes, QString* error = nullptr);
    [[nodiscard]] QByteArray ExportSettingsData(QString* error = nullptr) const;
    // 根绑定 AgentSettingsStore::Save；提交失败仅清理本次新建凭据，旧配置和密钥均保留。
    void SetCommitHandler(std::function<bool(const AgentSettings&, QString*)> handler);
    // 宿主注入应用级服务控件；不进入 AI 配置或设置迁移，取消不自动提交。
    void AddGeneralSection(QWidget* section);
    void AddApplicationPage(const QString& label, QWidget* page);

public slots:
    void accept() override;
    void reject() override;

signals:
    void SettingsChanged(const War3Map::Agent::AgentSettings& settings);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Agent
