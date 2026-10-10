#include "FeedbackInternal.h"

#include <miniz.h>

#include <cstring>
#include <memory>

namespace War3Map::Feedback::Detail {

Result<std::vector<std::byte>> MakeReportZip(const Delivery& delivery) {
    auto fail = [] { return Result<std::vector<std::byte>>::Failure("Feedback.Export", "FEEDBACK_EXPORT_ZIP_FAILED", "报告 ZIP 生成失败，没有输出部分包。"); };
    if (delivery.reportJson.empty() || delivery.reportJson.size() > 4 * 1024 * 1024
        || (delivery.cleanedLogs && delivery.cleanedLogs->size() > 1024 * 1024)
        || delivery.userReviewedScreenshotPng.size() > 16 * 1024 * 1024) return fail();
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0)) return fail();
    struct End {
        mz_zip_archive* archive;
        ~End() { mz_zip_writer_end(archive); }
    } end{&archive};
    if (!mz_zip_writer_add_mem(&archive, "report.json", delivery.reportJson.data(), delivery.reportJson.size(), MZ_BEST_SPEED)) return fail();
    if (delivery.cleanedLogs && !mz_zip_writer_add_mem(&archive, "logs.txt", delivery.cleanedLogs->data(), delivery.cleanedLogs->size(), MZ_BEST_SPEED)) return fail();
    if (!delivery.userReviewedScreenshotPng.empty() && !mz_zip_writer_add_mem(&archive, "screenshot.png", delivery.userReviewedScreenshotPng.data(), delivery.userReviewedScreenshotPng.size(), MZ_BEST_SPEED)) return fail();
    void* heap = nullptr;
    std::size_t bytes = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &heap, &bytes)) return fail();
    struct Free { void operator()(void* memory) const { mz_free(memory); } };
    std::unique_ptr<void, Free> owned(heap);
    if (heap == nullptr || bytes == 0 || bytes > 24 * 1024 * 1024) return fail();
    std::vector<std::byte> result(bytes);
    std::memcpy(result.data(), heap, bytes);
    return Result<std::vector<std::byte>>::Success(std::move(result));
}

} // namespace War3Map::Feedback::Detail
