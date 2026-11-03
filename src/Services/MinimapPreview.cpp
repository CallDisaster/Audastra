#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <array>

namespace War3Map::Services {
Result<MinimapPreview> DecodeMinimapPreview(const MinimapPreviewInput& input) {
    using Output = Result<MinimapPreview>;
    const auto& expected = input.sourceSha256;
    if (expected.size() != 64 || !std::all_of(expected.begin(), expected.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) return Output::Failure("Minimap.Preview", "MINIMAP_PREVIEW_IDENTITY", "须提供小写的完整源地图SHA-256。");
    MinimapPreview result; result.sourceSha256 = expected;
    if (!input.member) return Output::Success(std::move(result));
    result.present = true;
    if (input.member->size() > 4U * 1024U * 1024U) return Output::Failure("Minimap.Preview",
        "MINIMAP_PREVIEW_BUDGET", "已保存的小地图超过4MiB输入预算。");
    auto sha = Sha256(*input.member);
    if (!sha.Succeeded()) return {{}, std::move(sha.diagnostics)};
    result.memberSha256 = std::move(*sha.value);
    Image::BlpDecodeLimits decode; decode.maxDimension = 512;
    decode.maxInputBytes = 4U * 1024U * 1024U; decode.maxWorkingBytes = 64U * 1024U * 1024U;
    auto image = Image::DecodeBlpBudgeted(*input.member, decode);
    if (!image.Succeeded()) {
        auto failure = Output::Failure("Minimap.Preview", "MINIMAP_PREVIEW_INVALID",
            "已保存的小地图无法在预览预算内解码；没有使用空图或其它地图替代。");
        failure.diagnostics.insert(failure.diagnostics.end(), image.diagnostics.begin(), image.diagnostics.end());
        return failure;
    }
    result.image = std::move(image.value->image);
    auto rgbaSha = Sha256(result.image.rgba);
    if (!rgbaSha.Succeeded()) return {{}, std::move(rgbaSha.diagnostics)};
    result.rgbaSha256 = std::move(*rgbaSha.value);
    return Output::Success(std::move(result));
}

Result<MinimapPreview> CaptureMinimapPreview(const MapArchiveSession& source, std::string_view expected) {
    if (expected != source.SourceSha256()) return Result<MinimapPreview>::Failure("Minimap.Preview",
        "MINIMAP_PREVIEW_STALE", "小地图预览来源已变化，请重新取得地图身份。");
    const std::array<std::string, 1> names{"war3mapMap.blp"};
    ArchiveExactMemberLimits limits; limits.memberCount = 1;
    limits.memberBytes = limits.totalBytes = 4U * 1024U * 1024U;
    auto bytes = source.CaptureExactMembers(names, limits);
    if (!bytes.Succeeded()) return {{}, std::move(bytes.diagnostics)};
    MinimapPreviewInput input; input.sourceSha256 = std::string(expected);
    if (bytes.value->front().present) input.member = std::move(bytes.value->front().bytes);
    return DecodeMinimapPreview(input);
}

Result<MinimapPreview> OpenMinimapPreview(const std::filesystem::path& path, std::string_view expected) {
    // 只读专用打开保留完整来源证明，不要求本次读取具有地图写出资格。
    auto source = MapArchiveSession::OpenMinimapAudit(path);
    if (!source.Succeeded()) return {{}, std::move(source.diagnostics)};
    return CaptureMinimapPreview(*source.value, expected);
}

Status ExportMinimapPreviewPpm(const MinimapPreview& preview, const std::filesystem::path& output) {
    if (!preview.present || !preview.image.Valid() || preview.image.width > 512 || preview.image.height > 512) return Status::Failure("Minimap.Preview",
        "MINIMAP_PREVIEW_MISSING", "源地图没有可导出的小地图底图。");
    const auto header = "P6\n" + std::to_string(preview.image.width) + ' ' + std::to_string(preview.image.height) + "\n255\n";
    std::vector<std::byte> bytes; bytes.reserve(header.size() + preview.image.rgba.size() / 4 * 3);
    for (char c : header) bytes.push_back(static_cast<std::byte>(c));
    for (std::size_t i = 0; i < preview.image.rgba.size(); i += 4) {
        const auto alpha = std::to_integer<unsigned>(preview.image.rgba[i + 3]);
        for (std::size_t c = 0; c < 3; ++c) bytes.push_back(static_cast<std::byte>(
            (std::to_integer<unsigned>(preview.image.rgba[i + c]) * alpha + 127U) / 255U));
    }
    // CREATE_NEW与独占句柄保护作者文件；失败只删除本次新建的文件对象。
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Status::Failure("Minimap.Preview",
        "MINIMAP_PREVIEW_OUTPUT", "预览输出须为可写且尚不存在的新文件。");
    DWORD written = 0;
    const bool wrote = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    FILE_DISPOSITION_INFO discard{TRUE};
    const bool removed = !wrote && SetFileInformationByHandle(file, FileDispositionInfo, &discard, sizeof discard);
    CloseHandle(file);
    if (!wrote) return Status::Failure("Minimap.Preview", "MINIMAP_PREVIEW_OUTPUT",
        removed ? "预览副本未完整写出，已撤回本次新建文件。" : "预览副本未完整写出，无法撤回新建文件，请检查输出路径。");
    return Status::Ok();
}
} // namespace War3Map::Services
