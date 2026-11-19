#pragma once

#include "AgentTypes.h"

namespace War3Map::Agent {

// 设置与凭据分开存储；普通 JSON 只有本机凭据引用。
class AgentSettingsStore final {
public:
    explicit AgentSettingsStore(QString filePath = {});

    // error 非空表示整份设置无法读取（返回默认值，原文件保留）；
    // warning 非空表示设置已读取，但部分外部 MCP 条目不符合规则被忽略。
    [[nodiscard]] AgentSettings Load(QString* error = nullptr, QString* warning = nullptr) const;
    [[nodiscard]] bool Save(const AgentSettings& settings, QString* error = nullptr) const;
    [[nodiscard]] QString FilePath() const;
    [[nodiscard]] static bool Validate(const AgentSettings& settings, QString* error = nullptr);

private:
    QString filePath_;
};

} // namespace War3Map::Agent
