#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Game/GameDataSource.h"
#include <Windows.h>
#include <map>
#include <utility>

namespace War3Map::Render {
namespace {
Status SourceFailure() {
    return Status::Failure("Render.Animation", "WORLD_ASSET_SOURCE_CHANGED",
        "动画资源来源已变化或无法取得只读租约；旧缓存不再作为当前来源，请重新加载。");
}
struct FileLease {
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info{};
    bool missing = false;
    explicit FileLease(const std::filesystem::path& path, bool allowMissing) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            missing = allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        } else if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }
    ~FileLease() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    FileLease(const FileLease&) = delete;
    FileLease& operator=(const FileLease&) = delete;
    bool Valid() const noexcept { return file != INVALID_HANDLE_VALUE || missing; }
};
bool Same(const FileLease& a, const FileLease& b) {
    if (!a.Valid() || !b.Valid() || a.missing != b.missing) return false;
    if (a.missing) return true;
    return a.info.dwVolumeSerialNumber == b.info.dwVolumeSerialNumber &&
        a.info.nFileIndexHigh == b.info.nFileIndexHigh && a.info.nFileIndexLow == b.info.nFileIndexLow &&
        a.info.nFileSizeHigh == b.info.nFileSizeHigh && a.info.nFileSizeLow == b.info.nFileSizeLow &&
        CompareFileTime(&a.info.ftLastWriteTime, &b.info.ftLastWriteTime) == 0;
}
} // namespace
struct AnimationSourceGuard::State {
    std::map<std::filesystem::path, std::unique_ptr<FileLease>> leases;
};
AnimationSourceGuard::AnimationSourceGuard() : state_(std::make_unique<State>()) {}
AnimationSourceGuard::~AnimationSourceGuard() = default;
Status AnimationSourceGuard::Watch(const std::filesystem::path& path, bool allowMissing) {
    if (const auto found = state_->leases.find(path); found != state_->leases.end()) {
        const FileLease current(path, allowMissing);
        return Same(*found->second, current) ? Status::Ok() : SourceFailure();
    }
    if (state_->leases.size() >= 4096) return Status::Failure("Render.Animation", "WORLD_ASSET_GUARD_BUDGET",
        "动画来源文件租约超过 4096 项，未继续无界缓存。");
    auto lease = std::make_unique<FileLease>(path, allowMissing);
    if (!lease->Valid()) return SourceFailure();
    state_->leases.emplace(path, std::move(lease));
    return Status::Ok();
}
Status AnimationSourceGuard::WatchGameRoot(const std::filesystem::path& root) {
    // 核心 MPQ 名单复用 GameDataSource；不在此复制另一套 patch/local/x/base 名单。
    const auto discovered = Game::GameDataSource::Validate(root);
    if (discovered.archives.empty()) return SourceFailure();
    for (const auto& archive : discovered.archives) {
        auto status = Watch(root / archive.fileName, true);
        if (!status.Succeeded()) return status;
    }
    return Verify();
}
Status AnimationSourceGuard::Verify() const {
    for (const auto& [path, lease] : state_->leases) {
        const FileLease current(path, true);
        if (!Same(*lease, current)) return SourceFailure();
    }
    return Status::Ok();
}
} // namespace War3Map::Render
