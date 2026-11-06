#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace War3Map::DiagnosticTrace {
using Observer = std::function<void(std::string_view)>;
inline std::atomic<std::shared_ptr<const Observer>> observer;
inline void Configure(Observer sink) { observer.store(std::make_shared<const Observer>(std::move(sink))); }
inline void Event(std::string_view name) noexcept {
    if (const auto sink = observer.load()) try { (*sink)(name); } catch (...) {}
}
class Scope {
public:
    explicit Scope(std::string_view name) noexcept : sink_(observer.load()), name_(name) { Emit("begin/"); }
    ~Scope() { Emit("end/"); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    void Emit(std::string_view prefix) noexcept {
        if (sink_) try { (*sink_)(std::string(prefix) + std::string(name_)); } catch (...) {}
    }
    std::shared_ptr<const Observer> sink_;
    std::string_view name_;
};
} // namespace War3Map::DiagnosticTrace
