#pragma once

#include "War3MapCore/Feedback/Queue.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <miniz.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace FeedbackFixtures {

inline std::size_t checks = 0;
inline void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class T> inline T Take(War3Map::Result<T> result, const char* message) {
    if (!result.Succeeded()) {
        std::string detail(message);
        for (const auto& diagnostic : result.diagnostics)
            detail += " | " + diagnostic.code + ": " + diagnostic.message;
        Require(false, detail.c_str());
    }
    Require(result.Succeeded(), message);
    return std::move(*result.value);
}
inline void NoPrivate(std::string_view text, std::initializer_list<std::string_view> values) {
    for (auto value : values) {
        // values仅是这些原创测试中的固定字符串；禁止输出被测的完整报告/日志。
        const auto message = "private text must not survive (fixture=" + std::string(value) + ")";
        Require(text.find(value) == std::string_view::npos, message.c_str());
    }
}
inline std::string Read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
inline void Write(const std::filesystem::path& file, std::string_view text) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    Require(static_cast<bool>(stream), "write owned fixture file");
}
inline std::filesystem::path ResolveDirectory(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "open existing temp directory");
    const auto capacity = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring text(capacity + 1, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, text.data(), static_cast<DWORD>(text.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    Require(capacity > 0 && length > 0 && length < text.size(), "resolve real long temp directory");
    text.resize(length);
    Require(!text.starts_with(L"\\\\?\\UNC\\"), "fixtures require local temp directory");
    if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    return std::filesystem::path(text).lexically_normal();
}

class Temp final {
public:
    Temp() : parent_(ResolveDirectory(std::filesystem::temp_directory_path())) {
        static std::atomic<std::uint64_t> sequence{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = parent_ / (L"audastra-feedback-test-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)));
            if (CreateDirectoryW(path_.c_str(), nullptr)) return;
            Require(GetLastError() == ERROR_ALREADY_EXISTS, "create exclusive fixture directory");
        }
        throw std::runtime_error("cannot create owned fixture directory");
    }
    ~Temp() {
        // 只递归清理由本夹具独占创建、已解析真实绝对父目录下的明确子目录。
        if (path_.is_absolute() && path_.parent_path() == parent_ && path_.filename().native().starts_with(L"audastra-feedback-test-")) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    Temp(const Temp&) = delete;
    Temp& operator=(const Temp&) = delete;
    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path parent_;
    std::filesystem::path path_;
};

inline War3Map::Feedback::PrivacyContext Context() {
    using namespace War3Map::Feedback;
    return {{{PathRole::Game, R"(C:\Games\SyntheticWarcraft)"}, {PathRole::Ydwe, R"(D:\工具\SyntheticYDWE)"},
        {PathRole::User, R"(C:\Users\Synthetic User)"}}, {"原创测试 地图.w3x"}, {"original-secret-value-123"}};
}
inline War3Map::Feedback::DraftInput AgentDraft() {
    using namespace War3Map::Feedback;
    DraftInput draft;
    draft.source = ReportSource::Agent;
    draft.agentIssue = AgentIssue::InternalToolError;
    draft.application = {"1.2.3", "0123456789abcdef0123456789abcdef01234567", "Windows original fixture", "Synthetic GPU"};
    draft.errorCode = "ORIGINAL_INTERNAL_ERROR";
    draft.callSite = "RenderService::Capture";
    draft.toolName = "world.capture";
    draft.diagnostic = "Original internal fixture error";
    return draft;
}
inline War3Map::Feedback::DraftInput UserDraft() {
    auto draft = AgentDraft();
    draft.source = War3Map::Feedback::ReportSource::User;
    draft.agentIssue = War3Map::Feedback::AgentIssue::Unspecified;
    draft.userDescription = "原创建议：希望添加可选快捷键。";
    return draft;
}

class FakeTransport final : public War3Map::Feedback::IReportTransport {
public:
    bool configured = true;
    std::size_t calls = 0;
    bool throwOnSend = false;
    War3Map::Feedback::TransportResult response{true, false, {}};
    std::optional<War3Map::Feedback::Delivery> last;
    [[nodiscard]] bool Configured() const noexcept override { return configured; }
    [[nodiscard]] War3Map::Feedback::TransportResult Send(const War3Map::Feedback::Delivery& delivery) override {
        ++calls;
        last = delivery;
        if (throwOnSend) throw std::runtime_error("original fake transport exception");
        return response;
    }
};

inline void Big32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<std::byte>((value >> shift) & 255));
}
inline void Chunk(std::vector<std::byte>& out, std::string_view type, std::span<const std::byte> data) {
    Big32(out, static_cast<std::uint32_t>(data.size()));
    const auto start = out.size();
    for (char c : type) out.push_back(static_cast<std::byte>(c));
    out.insert(out.end(), data.begin(), data.end());
    Big32(out, static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(out.data() + start),
        static_cast<mz_ulong>(data.size() + 4))));
}
inline std::vector<std::byte> Png(bool addPrivateText = false) {
    std::vector<std::byte> result{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
    std::vector<std::byte> header;
    Big32(header, 1); Big32(header, 1);
    for (unsigned value : {8U, 6U, 0U, 0U, 0U}) header.push_back(static_cast<std::byte>(value));
    Chunk(result, "IHDR", header);
    if (addPrivateText) {
        const std::string text = std::string("Comment\0", 8) + "original-secret-value-123 C:\\Users\\Synthetic User\\private.png";
        Chunk(result, "tEXt", std::as_bytes(std::span(text.data(), text.size())));
    }
    const std::array<unsigned char, 5> pixel{0, 50, 60, 70, 255};
    mz_ulong length = mz_compressBound(static_cast<mz_ulong>(pixel.size()));
    std::vector<std::byte> compressed(length);
    Require(mz_compress2(reinterpret_cast<unsigned char*>(compressed.data()), &length, pixel.data(), static_cast<mz_ulong>(pixel.size()), MZ_BEST_SPEED) == MZ_OK, "compress original fixture pixel");
    compressed.resize(length);
    Chunk(result, "IDAT", compressed);
    Chunk(result, "IEND", {});
    return result;
}

} // namespace FeedbackFixtures
