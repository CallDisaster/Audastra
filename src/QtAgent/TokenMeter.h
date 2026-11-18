#pragma once

#include "AgentTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringView>

namespace War3Map::Agent {

// CJK 与 JSON Schema 感知的分项 Token 计量器（对齐 DSH dsh-token-meter）：
// 既可在尚未发出模型请求时给出准确的上下文构成估算，也可结合提供商实际返回的 usage 做等比校准。
class TokenMeter final {
public:
    [[nodiscard]] static qint64 EstimateText(QStringView text) noexcept;
    [[nodiscard]] static qint64 EstimateJson(const QJsonValue& value);
    [[nodiscard]] static qint64 EstimateTool(const QJsonObject& tool);
    [[nodiscard]] static qint64 EstimateMessage(const QJsonObject& message);

    // 结合真实请求总输入 token（若已知 > 0）对各输入构成项做比例投影，使分项之和与真实上下文一致。
    [[nodiscard]] static ContextCompositionSnapshot ProjectToActualInput(
        const ContextCompositionSnapshot& raw, qint64 actualPromptTokens);
};

} // namespace War3Map::Agent
