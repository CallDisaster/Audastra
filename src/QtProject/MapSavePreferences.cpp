#include "MapSavePreferences.h"
#include "../QtCommon/MapBackupPaths.h"

#include <QSettings>
#include <QStandardPaths>

namespace War3Map::QtProject {

Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions() {
    const QSettings settings;
    return ReadMapInPlaceSaveOptions(settings,
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions(
    const QSettings& settings, const QString& genericDataRoot) {
    return QtCommon::ReadMapInPlaceSaveOptions(settings, genericDataRoot);
}

} // namespace War3Map::QtProject
