#pragma once

#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/Text/TextUtil.h"

#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 界面核心测试共用的检查函数。
namespace War3Map::GameUi::Testing {

inline void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

inline bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

inline bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

inline const FrameNode& NodeNamed(const FrameTree& tree, std::string_view name) {
    const auto id = tree.FindByName(name);
    if (!id) throw std::runtime_error("node missing: " + std::string(name));
    return tree.Nodes()[*id];
}

inline void RequireRect(const FrameNode& node, float left, float bottom, float right, float top, const char* reason) {
    Require(node.layout == LayoutState::Placed && Near(node.rect.left, left) && Near(node.rect.bottom, bottom) &&
            Near(node.rect.right, right) && Near(node.rect.top, top),
        reason);
}

// 内存中的“归档”：路径不区分大小写，供 FrameLibrary 读取。
inline FrameFileReader MemoryReader(const std::map<std::string, std::string>& files) {
    return [files](const std::string& path) -> Result<std::vector<std::byte>> {
        std::string key = Text::LowerAscii(path);
        for (char& c : key) {
            if (c == '/') c = '\\';
        }
        const auto found = files.find(key);
        if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "MISSING", path);
        std::vector<std::byte> bytes(found->second.size());
        std::memcpy(bytes.data(), found->second.data(), bytes.size());
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    };
}

inline const ScriptFrame& FrameCreatedAt(const ScriptFramePlan& plan, std::uint32_t line) {
    for (const auto& frame : plan.frames) {
        if (frame.created.line == line) return frame;
    }
    throw std::runtime_error("script frame missing at line " + std::to_string(line));
}

} // namespace War3Map::GameUi::Testing
