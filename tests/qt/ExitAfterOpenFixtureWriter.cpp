#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ByteVector = std::vector<std::byte>;

class BinaryWriter final {
public:
    void U16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
        bytes_.push_back(static_cast<std::byte>(value >> 8U));
    }

    void F32(float value) { I32(std::bit_cast<std::int32_t>(value)); }

    void I32(std::int32_t value) {
        const std::uint32_t raw = static_cast<std::uint32_t>(value);
        bytes_.push_back(static_cast<std::byte>(raw));
        bytes_.push_back(static_cast<std::byte>(raw >> 8U));
        bytes_.push_back(static_cast<std::byte>(raw >> 16U));
        bytes_.push_back(static_cast<std::byte>(raw >> 24U));
    }

    void Ascii(std::string_view value) {
        bytes_.insert(
            bytes_.end(),
            reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size()));
    }

    void CString(std::string_view value) {
        Ascii(value);
        bytes_.push_back(std::byte{0});
    }

    void SizedText(std::string_view value) {
        if (value.empty()) {
            I32(0);
            return;
        }
        I32(static_cast<std::int32_t>(value.size() + 1));
        Ascii(value);
        bytes_.push_back(std::byte{0});
    }

    [[nodiscard]] ByteVector Take() {
        return std::move(bytes_);
    }

private:
    ByteVector bytes_;
};

void WriteBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("无法创建合成测试文件");
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error("无法完整写入合成测试文件");
    }
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    WriteBytes(
        path,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()),
            text.size()));
}

[[nodiscard]] ByteVector MakeWtg(bool customText) {
    BinaryWriter writer;
    writer.Ascii("WTG!");
    writer.I32(7);

    writer.I32(1);       // 分类数量。
    writer.I32(1);       // 分类 ID。
    writer.CString("Synthetic");
    writer.I32(0);       // 不是注释分类。

    writer.I32(2);       // WTG v7 变量头。
    writer.I32(0);       // 变量数量。
    writer.I32(1);       // 触发器数量。

    writer.CString("Synthetic trigger");
    writer.CString("Redistributable process-test fixture");
    writer.I32(0);       // 普通触发器。
    writer.I32(1);       // 启用。
    writer.I32(customText ? 1 : 0); // 自定义文本只使用 WCT，不伪造 ECA。
    writer.I32(0);       // 初始开启。
    writer.I32(0);       // 不在初始化时直接运行。
    writer.I32(1);       // 所属分类 ID。
    writer.I32(customText ? 0 : 1); // 根 ECA 数量。

    if (customText) {
        return writer.Take();
    }

    writer.I32(2);       // Action。
    writer.CString("DoNothing");
    writer.I32(1);       // ECA 启用。
    writer.I32(0);       // 子 ECA 数量；DoNothing 没有参数。
    return writer.Take();
}

[[nodiscard]] ByteVector MakeWct(bool customText) {
    BinaryWriter writer;
    writer.I32(1);
    writer.CString("Synthetic fixture");
    writer.SizedText("");
    writer.I32(1);
    writer.SizedText(customText
        ? "function SyntheticRecovered takes nothing returns nothing\nendfunction\n" : "");
    return writer.Take();
}

[[nodiscard]] ByteVector MakeTerrain() {
    BinaryWriter writer;
    writer.Ascii("W3E!"); writer.I32(11); writer.Ascii("L"); writer.I32(0);
    writer.I32(1); writer.Ascii("Ldrt");
    writer.I32(1); writer.Ascii("CLdi");
    writer.I32(9); writer.I32(9); writer.F32(-512); writer.F32(-512);
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) {
            writer.U16(static_cast<std::uint16_t>(8192 + x * 16 + y * 8));
            writer.U16(8192);
            writer.Ascii(std::string_view("\0\0\2", 3));
        }
    }
    return writer.Take();
}

void WriteWorkspace(const std::filesystem::path& root, bool customText, bool worldTerrain) {
    // UI Catalog 完全位于临时工作区，测试不会退回仓库 data/ui 或用户安装目录。
    std::filesystem::remove_all(root);
    WriteText(root / "ui" / "config", "base\r\n");
    WriteText(
        root / "ui" / "base" / "define.txt",
        "[TriggerTypes]\r\n"
        "SyntheticInteger=0,0,1,Synthetic,integer\r\n");
    WriteText(
        root / "ui" / "base" / "event.txt",
        "[SyntheticEvent]\r\n"
        "title=\"Synthetic event\"\r\n");
    WriteText(
        root / "ui" / "base" / "condition.txt",
        "[SyntheticCondition]\r\n"
        "title=\"Synthetic condition\"\r\n");
    WriteText(
        root / "ui" / "base" / "action.txt",
        "[DoNothing]\r\n"
        "title=\"Do nothing\"\r\n"
        "description=\"Do nothing\"\r\n");
    WriteText(
        root / "ui" / "base" / "call.txt",
        "[SyntheticCall]\r\n"
        "title=\"Synthetic call\"\r\n"
        "returns=integer\r\n");

    WriteBytes(root / "map" / "war3map.wtg", MakeWtg(customText));
    WriteBytes(root / "map" / "war3map.wct", MakeWct(customText));
    // 原创纯地形，不依赖游戏、YDWE 或模型文件；缺少贴图时使用显式高度色预览。
    if (worldTerrain) WriteBytes(root / "map" / "war3map.w3e", MakeTerrain());
    WriteText(
        root / "table" / "unit.ini",
        "[H000]\r\n"
        "_parent = \"hfoo\"\r\n"
        "name = \"Synthetic Unit\"\r\n"
        "hitPoints = 420\r\n");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2 && argc != 3) {
        std::cerr << "用法: war3-map-exit-after-open-fixture-writer <workspace>\n";
        return 2;
    }
    try {
        const std::string_view workspacePath(argv[1]);
        WriteWorkspace(std::filesystem::path(std::u8string(workspacePath.begin(), workspacePath.end())),
            argc == 3 && std::string_view(argv[2]) == "--custom-text",
            argc == 3 && std::string_view(argv[2]) == "--world-terrain");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "生成合成工作区失败: " << error.what() << '\n';
        return 1;
    }
}
