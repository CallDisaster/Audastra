#include "ProjectFileIO.h"

#include <QFile>

namespace War3Map::QtProject {

std::optional<std::vector<std::byte>> ReadProjectFileBytes(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 512ll * 1024 * 1024) return {};
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError || data.size() != file.size()) return {};
    const auto* start = reinterpret_cast<const std::byte*>(data.constData());
    return std::vector<std::byte>(start, start + data.size());
}

} // namespace War3Map::QtProject
