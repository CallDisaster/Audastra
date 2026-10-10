#pragma once

#include "AgentTypes.h"

namespace War3Map::Agent {

// 设置与凭据分开存储；普通 JSON 只有本机凭据引用。
class AgentSettingsStore final {
public:
    explicit AgentSettingsStore(QString filePath = {});

    [[nodiscard]] AgentSettings Load(QString* error = nullptr) const;
    [[nodiscard]] bool Save(const AgentSettings& settings, QString* error = nullptr) const;
    [[nodiscard]] QString FilePath() const;
    [[nodiscard]] static bool Validate(const AgentSettings& settings, QString* error = nullptr);

private:
    QString filePath_;
};

} // namespace War3Map::Agent
