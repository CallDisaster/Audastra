#pragma once
#include <QEventLoop>
#include <QThread>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace War3Map::QtCommon {
namespace OwnedWorkDetail { inline thread_local bool asynchronous = false; }

// 只选择调度方式，不授予来源或修订资格。默认同步，UI宿主显式开启。
class OwnedWorkScope {
public:
    explicit OwnedWorkScope(bool asynchronous) noexcept
        : previous_(std::exchange(OwnedWorkDetail::asynchronous, asynchronous)) {}
    ~OwnedWorkScope() { OwnedWorkDetail::asynchronous = previous_; }
    OwnedWorkScope(const OwnedWorkScope&) = delete;
    OwnedWorkScope& operator=(const OwnedWorkScope&) = delete;
private:
    bool previous_;
};

// 闭包须拥有输入，不能捕获控件或工程可变文档。借用的输出槽仅在join后由UI访问。
// 调用方负责整个嵌套事件循环的忙门，以及装回结果前的来源/修订核对。
template<class Function>
auto RunOwnedWork(Function function, bool eligible = true) -> std::invoke_result_t<Function> {
    using Result = std::invoke_result_t<Function>;
    static_assert(!std::is_void_v<Result> && !std::is_reference_v<Result>);
    if (!eligible || !OwnedWorkDetail::asynchronous) return std::invoke(std::move(function));
    std::optional<Result> result;
    std::exception_ptr failure;
    QEventLoop loop;
    std::unique_ptr<QThread> worker(QThread::create(
        [function = std::move(function), &result, &failure]() mutable {
            try { result.emplace(std::invoke(std::move(function))); }
            catch (...) { failure = std::current_exception(); }
        }));
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start(QThread::LowPriority);
    loop.exec();
    worker->wait();
    if (failure) std::rethrow_exception(failure);
    return std::move(*result);
}
} // namespace War3Map::QtCommon
