#pragma once

#include "War3MapCore/Feedback/Queue.h"

namespace War3Map::Feedback::Detail {

struct Entry {
    EntrySummary summary;
    std::string problemFingerprint;
    std::string confirmedPreviewSha256;
    std::optional<ReportData> report;
    bool deliveryInFlight = false;
};

struct Snapshot {
    std::uint64_t nextSequence = 1;
    std::optional<std::int64_t> lastSentUnixSeconds;
    std::vector<Entry> entries;
};

[[nodiscard]] EntrySummary Summarize(const Entry& entry);
[[nodiscard]] Status ValidateEntry(const Entry& entry, const PrivacyContext& context, const QueueLimits& limits);
[[nodiscard]] Result<std::string> EncodeSnapshot(const Snapshot& snapshot, const PrivacyContext& context, const QueueLimits& limits);
[[nodiscard]] Result<Snapshot> DecodeSnapshot(std::span<const std::byte> bytes, const PrivacyContext& context, const QueueLimits& limits);
[[nodiscard]] Result<Delivery> MakeDelivery(const Entry& entry, const PrivacyContext& context, const QueueLimits& limits);
[[nodiscard]] Result<std::vector<std::byte>> MakeReportZip(const Delivery& delivery);

class QueueStorage final {
public:
    [[nodiscard]] static Result<std::unique_ptr<QueueStorage>> Open(const std::filesystem::path& directory, std::size_t maxBytes);
    ~QueueStorage();
    QueueStorage(const QueueStorage&) = delete;
    QueueStorage& operator=(const QueueStorage&) = delete;
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> Load();
    [[nodiscard]] Status Save(std::span<const std::byte> bytes);
private:
    struct Impl;
    explicit QueueStorage(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// 与队列相同的单次原子文件写出；导出只接受不存在的目标，不替换已有文件。
[[nodiscard]] Status WriteNewExport(const std::filesystem::path& target, std::span<const std::byte> bytes);

} // namespace War3Map::Feedback::Detail
