#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include <stdexcept>
#include <string_view>
#include <utility>

namespace War3Map::Render::Scan::PreviewRead {
// 资源层失败经过异常边界时保留整组诊断；what() 仍供枚举/宿主错误码使用。
class Failure final : public std::runtime_error {
public:
    Failure(std::vector<Diagnostic> values, std::string_view context, std::string_view fallback)
        : std::runtime_error(std::string(context) + (values.empty() ? std::string(fallback) : values.front().code)),
          diagnostics(std::move(values)) {
        if (diagnostics.empty()) diagnostics.push_back({DiagnosticSeverity::Error, "PreviewScan", std::string(fallback),
            "资源操作失败且未提供详细诊断。", std::nullopt});
    }
    std::vector<Diagnostic> diagnostics;
};
template<class T> T Take(Result<T> result, std::string_view context = {}) {
    if (!result.Succeeded()) throw Failure(std::move(result.diagnostics), context, "SCAN_READ_FAILED");
    return std::move(*result.value);
}
inline void Take(Status status) {
    if (!status.Succeeded()) throw Failure(std::move(status.diagnostics), {}, "SCAN_SOURCE_FAILED");
}
template<class Read> Result<AnimationResource> Resource(std::string_view path, Read&& read) {
    try { return Result<AnimationResource>::Success(std::forward<Read>(read)()); }
    catch (const Failure& failure) {
        auto diagnostics = failure.diagnostics;
        for (auto& diagnostic : diagnostics) diagnostic.message += " 请求资源：" + std::string(path);
        return {std::nullopt, std::move(diagnostics)};
    } catch (const std::exception& error) {
        return Result<AnimationResource>::Failure("PreviewScan", error.what(),
            "只读资源取得失败。请求资源：" + std::string(path));
    }
}
} // namespace War3Map::Render::Scan::PreviewRead
