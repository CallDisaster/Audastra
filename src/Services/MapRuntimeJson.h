#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace War3Map::Services::detail {
// 所有运行库元数据共用预算和重复键策略；不对不透明成员调用。
inline nlohmann::json ParseRuntimeJson(std::span<const std::byte> bytes, std::size_t maximumBytes) {
    using Json = nlohmann::json;
    if (bytes.empty() || bytes.size() > maximumBytes) throw std::runtime_error("json budget");
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 16) throw std::runtime_error("json depth");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key
            && (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second))
            throw std::runtime_error("duplicate key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    const auto* first = reinterpret_cast<const char*>(bytes.data());
    return Json::parse(first, first + bytes.size(), callback);
}
} // namespace War3Map::Services::detail
