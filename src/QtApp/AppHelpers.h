#pragma once

// QtApp 组装层内部的小工具（旧主窗口拆分后多个文件共用）。
#include "EditorProject.h"

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QFile>
#include <QLabel>
#include <QString>

#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace AppDetail {

inline bool HasUiCatalogConfig(const std::filesystem::path& candidate) {
    return War3Map::QtProject::EditorProject::HasUiCatalogConfig(candidate);
}

inline std::filesystem::path NormalizeUiDataRoot(const std::filesystem::path& candidate) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(candidate, error);
    return (error ? candidate : absolute).lexically_normal();
}

inline constexpr std::array kObjectTypes{
    War3Map::ObjectData::ObjectType::Unit,
    War3Map::ObjectData::ObjectType::Ability,
    War3Map::ObjectData::ObjectType::Item,
    War3Map::ObjectData::ObjectType::Buff,
    War3Map::ObjectData::ObjectType::Upgrade,
    War3Map::ObjectData::ObjectType::Doodad,
    War3Map::ObjectData::ObjectType::Destructable,
};

inline std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
    return {begin, begin + bytes.size()};
}

inline QString Utf8(const std::string& value) {
    return QString::fromUtf8(value);
}

inline QString DiagnosticLevel(War3Map::DiagnosticSeverity severity) {
    return War3Map::QtProject::EditorProject::DiagnosticLevel(severity);
}

} // namespace AppDetail
