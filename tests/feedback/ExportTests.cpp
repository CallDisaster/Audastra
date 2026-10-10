#include "Fixtures.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <set>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace FeedbackFixtures;

namespace {

class ZipRead final {
public:
    explicit ZipRead(std::string bytes) : bytes_(std::move(bytes)) {
        Require(mz_zip_reader_init_mem(&archive_, bytes_.data(), bytes_.size(), 0) != 0, "independently reopen exported ZIP");
    }
    ~ZipRead() { mz_zip_reader_end(&archive_); }
    [[nodiscard]] std::set<std::string> Names() const {
        std::set<std::string> names;
        const auto count = mz_zip_reader_get_num_files(const_cast<mz_zip_archive*>(&archive_));
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            Require(mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&archive_), i, &stat) != 0, "read ZIP member stat");
            names.insert(stat.m_filename);
        }
        Require(names.size() == count, "ZIP has no duplicate members");
        return names;
    }
    [[nodiscard]] std::string Member(const char* name) {
        std::size_t size = 0;
        void* heap = mz_zip_reader_extract_file_to_heap(&archive_, name, &size, 0);
        Require(heap != nullptr, "read ZIP member bytes");
        const std::string result(static_cast<const char*>(heap), size);
        mz_free(heap);
        return result;
    }
private:
    std::string bytes_;
    mz_zip_archive archive_{};
};

} // namespace

int main() {
    try {
        Temp temp;
        auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open export fixture queue");
        auto draft = UserDraft();
        draft.userDescription = R"(原创建议，涉及 C:\Users\Synthetic User\notes.txt 及 原创测试 地图.w3x)";
        draft.includeLogs = true;
        draft.recentLogs = R"(log file=\\private-server\中文目录\secret.log
apiKey="private-log-key"
url=https://user:password@example.invalid/v1?token=private-log-token#private-fragment
)";
        draft.includeConversation = true;
        draft.conversationSnippet = R"(用户明确选中的原创建议片段，路径 D:\工具\SyntheticYDWE\bin\private.dll)";
        draft.includeScreenshot = true;
        draft.screenshot.png = Png(true);
        const auto screenshot = Take(PrepareScreenshot(draft.screenshot.png), "process final explicit PNG");
        draft.screenshot.userReviewedSha256 = screenshot.sha256;
        const auto created = Take(queue->Draft(draft, 6000), "draft explicit attachments");
        const auto preview = Take(queue->GetPreview(created.entry.id), "full export preview");
        Require(!preview.consent && !preview.confirmed && preview.requiresScreenshotPreview, "export preview default consent and final PNG notice");
        Require(preview.delivery.userReviewedScreenshotPng == screenshot.png, "preview contains actual final PNG");
        NoPrivate(preview.delivery.reportJson, {"Synthetic User", "notes.txt", "原创测试 地图.w3x", "private-server", "private-log-key", "private-log-token", "private-fragment", "SyntheticYDWE", "private.dll"});
        const auto destination = temp.Path() / L"原创报告.zip";
        const auto exported = Take(queue->Export(created.entry.id, destination), "export without consent or endpoint");
        Require(exported.bytes == Read(destination).size() && exported.sha256 == Take(Sha256File(destination), "independent exported package hash"), "exported path bytes and digest reflect file");
        const auto originalFile = Read(destination);
        ZipRead zip(originalFile);
        Require(zip.Names() == std::set<std::string>{"report.json", "logs.txt", "screenshot.png"}, "exact original ZIP member allowlist");
        const auto report = zip.Member("report.json");
        Require(report == preview.delivery.reportJson, "ZIP report is full preview payload");
        const auto json = nlohmann::json::parse(report);
        Require(json.at("conversationSnippet").is_string() && json.at("screenshot").at("sha256") == screenshot.sha256, "explicit conversation and screenshot binding included");
        Require(json.at("screenshot").at("pixelPrivacy") == "截图像素由用户审核，未自动脱敏", "report makes no pixel redaction claim");
        Require(zip.Member("logs.txt") == *preview.delivery.cleanedLogs, "ZIP logs match sanitized preview");
        const auto pixels = zip.Member("screenshot.png");
        Require(pixels == std::string(reinterpret_cast<const char*>(screenshot.png.data()), screenshot.png.size()), "ZIP final screenshot is exact previewed bytes");
        NoPrivate(pixels, {"original-secret-value-123", "Synthetic User"});
        Require(!queue->Export(created.entry.id, destination).Succeeded() && Read(destination) == originalFile, "export never overwrites existing user file");
        Require(queue->ConfirmPreview(created.entry.id, preview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(), "explicit user confirms complete attachments then consents");
        FakeTransport transport;
        Require(Take(queue->TrySend(created.entry.id, preview.delivery.previewSha256, 6001, transport), "send exact reviewed export fixture payload").state == AttemptState::Sent, "reviewed attachments sent through fake interface");
        Require(transport.calls == 1 && transport.last->reportJson == report && transport.last->userReviewedScreenshotPng == screenshot.png, "transport receives exact sanitized reviewed payload only");
        const auto after = Read(temp.Path() / L"queue" / L"queue.json");
        NoPrivate(after, {"原创建议", "原创建议片段", "pngHex", "private-log-key", "private-log-token"});
        Require(!queue->Export(created.entry.id, temp.Path() / L"after-send.zip").Succeeded(), "removed sent payload cannot be reexported implicitly");

        auto plain = AgentDraft();
        const auto plainRecord = Take(queue->Draft(plain, 7000), "draft report with default exclusions");
        const auto plainExport = Take(queue->Export(plainRecord.entry.id, temp.Path() / L"plain.zip"), "export plain report");
        ZipRead plainZip(Read(plainExport.path));
        Require(plainZip.Names() == std::set<std::string>{"report.json"}, "default package excludes map trigger logs conversation screenshot");
        const auto plainJson = nlohmann::json::parse(plainZip.Member("report.json"));
        Require(!plainJson.contains("mapContent") && !plainJson.contains("triggerContent") && plainJson.at("logs").is_null()
            && plainJson.at("conversationSnippet").is_null() && plainJson.at("screenshot").is_null(), "export whitelist contains no unselected content");
        std::cout << "feedback export checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "feedback export failure: " << error.what() << '\n';
        return 1;
    }
}
