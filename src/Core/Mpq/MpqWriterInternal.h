#pragma once

#include "War3MapCore/Mpq/MpqWriter.h"

namespace War3Map { class ProjectSaveControl; }

namespace War3Map::Mpq {

// 与写出器共用的纯输入校验；不压缩或创建文件，不证明压缩后布局与写出成功。
[[nodiscard]] Status ValidateMpqArchiveInputs(
    std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader = {},
    std::uint16_t sectorShift = kMpqWriterSectorShift);

// 调用方必须传入以 CREATE_NEW 打开的 Windows 文件句柄，并在调用期间保持独占所有权。
[[nodiscard]] Status WriteMpqArchiveToHandle(
    void* outputHandle,
    std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader = {},
    std::uint16_t sectorShift = kMpqWriterSectorShift,
    const ProjectSaveControl* control = nullptr);

} // namespace War3Map::Mpq
