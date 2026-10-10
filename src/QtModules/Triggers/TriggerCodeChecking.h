#pragma once
#include "Widgets/CodeViewTypes.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <QHash>

namespace War3Map::Trigger { class UiCatalog; }

namespace War3Map::Modules {
[[nodiscard]] UiKit::CodeCheckTask PrepareTriggerCodeCheck(const QString& text, std::uint64_t generation,
    const QString& source, UiKit::CodeLanguage language, Services::CodeCheckContext context = {});
[[nodiscard]] UiKit::CodeCheckResult PresentCodeCheckResult(const Services::CodeCheckResult& result, const QString& text, const QString& source, std::stop_token stop = {});
[[nodiscard]] UiKit::CodeCheckTask PrepareProjectCodeCheck(std::shared_ptr<const Services::CodeCheckProjectSnapshot> snapshot,
    std::shared_ptr<Services::CodeCheckProjectCache> cache, Trigger::CodeBlockTarget target,
    const QString& text, std::uint64_t generation, UiKit::CodeLanguage language);
// 未声明编码的旧生成脚本只读查看：保留有效 UTF8 标量，非法片段按 Core 旧编码策略推测。
[[nodiscard]] UiKit::CodeDecodedSource DecodeMapCodeForView(const QByteArray& bytes, std::stop_token stop = {});
[[nodiscard]] UiKit::CodeDecodedSource ShowMapCodeRawBytes(const QByteArray& bytes, std::stop_token stop = {});
// 函数库里各函数的作者说明（标题与提示），按脚本函数名索引，供代码悬停显示。
[[nodiscard]] QHash<QString, QString> UiFunctionNotes(const Trigger::UiCatalog& catalog);
[[nodiscard]] QVector<UiKit::CodeCompletionItem> CodeDeclarationsForView(const QString& text, const QString& source, UiKit::CodeLanguage language);
}
