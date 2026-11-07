#pragma once
#include <cstdint>
#include <map>
#include <memory>

namespace Audastra::Runtime32::OwnLua {
// 池编号不持有agent引用。观察到创建/销毁时更换本地生命期，不能按数字复活旧userdata。
// 只用于已核对为非零编号、显式销毁的类型；外部创建和销毁不在此账本覆盖范围内。
class NativePoolLedger final {
public:
    struct Life { bool alive=true; };
    static constexpr std::size_t kMaximumLive=4096;
    ~NativePoolLedger(){Clear();}
    [[nodiscard]] bool CanCreate() const noexcept{return live_.size()<kMaximumLive;}
    [[nodiscard]] std::shared_ptr<Life> Created(std::uint32_t id) {
        if(!id || !CanCreate())return {};
        auto value=std::make_shared<Life>();
        if(const auto old=live_.find(id);old!=live_.end())old->second->alive=false;
        live_[id]=value;return value;
    }
    [[nodiscard]] std::shared_ptr<Life> Borrow(std::uint32_t id) const noexcept {
        const auto found=live_.find(id);return found==live_.end()?nullptr:found->second;
    }
    void Destroyed(std::uint32_t id) noexcept {
        const auto found=live_.find(id);
        if(found!=live_.end()){found->second->alive=false;live_.erase(found);}
    }
    void Clear() noexcept {for(auto& [id,life]:live_)life->alive=false;live_.clear();}
private:
    std::map<std::uint32_t,std::shared_ptr<Life>> live_;
};
} // namespace Audastra::Runtime32::OwnLua
