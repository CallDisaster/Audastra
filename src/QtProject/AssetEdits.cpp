#include "EditorProject.h"
#include "Icons/IconProvider.h"

#include <QSet>
#include <QHash>

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Workspace.h"

#include <algorithm>

// 资源路径编辑：地图已有成员的改名 / 移动 / 删除只记录在 assetEdits_，保存另存副本时才改写归档与导入清单；
// 待导入文件直接改它的归档路径。所有检查与 war3map.imp 的路径规则一致（相对 ASCII 路径）。
namespace War3Map::QtProject {
namespace {

// 地图自身的数据文件（根目录的 war3map* / war3campaign*、脚本、自动清单）由编辑器管理，不能当作资源改动。
bool IsMapDataPath(const QString& key) {
    if (key.startsWith(QLatin1Char('(')) || key == QLatin1String("scripts\\war3map.j") ||
        key == QLatin1String("scripts\\war3map.lua")) {
        return true;
    }
    return !key.contains(QLatin1Char('\\')) &&
        (key.startsWith(QLatin1String("war3map")) || key.startsWith(QLatin1String("war3campaign")));
}

} // namespace

QString EditorProject::AssetKey(const QString& path) {
    return QString(path).replace(QLatin1Char('/'), QLatin1Char('\\')).toLower();
}

QString EditorProject::CheckAssetPath(const QString& path) {
    if (path.trimmed().isEmpty()) return tr("路径不能为空。");
    if (path.endsWith(QLatin1Char('\\')) || path.endsWith(QLatin1Char('/'))) return tr("路径必须以文件名结尾。");
    for (const QChar character : path) {
        if (character.unicode() >= 0x7F) {
            return tr("路径只能使用英文字母、数字与常见符号（魔兽的导入清单不支持中文等非 ASCII 字符）。");
        }
    }
    if (!Resources::CanonicalImportPath(path.toStdString()).Succeeded()) {
        return tr("路径含有非法字符（: \" < > | ? *）、设备名、空的文件夹名或以点 / 空格结尾的名称。");
    }
    if (IsMapDataPath(AssetKey(path))) return tr("这是地图自身的数据文件，由编辑器管理，不能用作资源路径。");
    return {};
}

std::vector<AssetEntry> EditorProject::AssetEntries(std::size_t maxEntries) const {
    return AssetEntries(CaptureAssets(), maxEntries);
}

std::vector<AssetEntry> EditorProject::AssetEntries(const AssetState& state, std::size_t maxEntries) const {
    std::vector<AssetEntry> entries;
    if (!maxEntries) return entries;
    QHash<QString, std::size_t> rows;
    QHash<QString, const AssetEdit*> edits;
    QSet<QString> duplicateEdits;
    for (const auto& edit : state.edits) {
        const auto key = AssetKey(edit.from);
        if (edits.contains(key)) duplicateEdits.insert(key);
        else edits.insert(key, &edit);
    }
    if (archiveSession_) {
        for (const auto& member : archiveSession_->Members()) {
            const QString name = QString::fromUtf8(member.name);
            if (IsMapDataPath(AssetKey(name))) continue;
            QString path = name;
            if (const auto edit = edits.constFind(AssetKey(name)); edit != edits.cend()) {
                if ((*edit)->to.isEmpty()) continue;
                path = (*edit)->to;
            }
            const auto key = AssetKey(path);
            if (const auto row = rows.constFind(key); row != rows.cend()) {
                entries[*row].ambiguous = true;
            } else {
                rows.insert(key, entries.size());
                entries.push_back({path, name, static_cast<qint64>(member.data.size()), {}, duplicateEdits.contains(AssetKey(name))});
            }
        }
    } else if (directorySnapshot_) {
        // 目录来源保持真实相对路径；map/resource 前缀不是通用资源路径别名。
        // Snapshot 的枚举可能跳过无权限项，这里只提供已捕获成员的正证据。
        QSet<QString> managedObjects{"table\\w3i.ini"};
        for (std::size_t type = 0; type < kObjectTypeCount; ++type) {
            const auto nativeName = ObjectData::NativeFileName(static_cast<ObjectData::ObjectType>(type));
            const auto lniName = ObjectData::LniFileName(static_cast<ObjectData::ObjectType>(type));
            const auto native = AssetKey(QString::fromUtf8(nativeName.data(), static_cast<qsizetype>(nativeName.size())));
            const auto lni = AssetKey(QString::fromUtf8(lniName.data(), static_cast<qsizetype>(lniName.size())));
            managedObjects.insert(native); managedObjects.insert("map\\" + native);
            managedObjects.insert(lni); managedObjects.insert("table\\" + lni);
        }
        for (const auto& member : directorySnapshot_->Snapshot().files) {
            const auto name = QString::fromStdWString(member.relativePath.wstring()).replace('/', '\\');
            const auto sourceKey = AssetKey(name);
            if (IsMapDataPath(sourceKey) || (sourceKey.startsWith("map\\") && IsMapDataPath(sourceKey.mid(4)))) continue;
            if (managedObjects.contains(sourceKey)) continue;
            QString path = name;
            if (const auto edit = edits.constFind(sourceKey); edit != edits.cend()) {
                if ((*edit)->to.isEmpty()) continue;
                path = (*edit)->to;
            }
            const auto key = AssetKey(path);
            if (const auto row = rows.constFind(key); row != rows.cend()) entries[*row].ambiguous = true;
            else {
                rows.insert(key, entries.size());
                entries.push_back({path, name, static_cast<qint64>(member.size), {}, duplicateEdits.contains(sourceKey)});
            }
        }
    }
    for (const auto& pending : state.imports) {
        const auto key = AssetKey(pending.archivePath);
        if (const auto row = rows.constFind(key); row != rows.cend()) {
            auto& entry = entries[*row];
            if (entry.Pending()) entry.ambiguous = true;
            else {
                entry.path = pending.archivePath; entry.diskPath = pending.diskPath; entry.size = pending.size;
                entry.ownedPayload = pending.ownedPayload; entry.runtimeLuaDraft = pending.runtimeLuaDraft;
            }
        } else {
            rows.insert(key, entries.size());
            entries.push_back({pending.archivePath, {}, pending.size, pending.diskPath, false, pending.ownedPayload, pending.runtimeLuaDraft});
        }
    }
    // 截断的是有效覆盖序列，不能在旧归档前缀提前返回并漏掉待导入覆盖层。
    if (entries.size() > maxEntries) entries.resize(maxEntries);
    return entries;
}

QString EditorProject::MoveAsset(const QString& from, const QString& to) {
    auto candidate = CaptureAssets();
    const auto problem = MoveAsset(candidate, from, to);
    if (problem.isEmpty() && candidate != CaptureAssets()) PublishAssetState(std::move(candidate));
    return problem;
}

QString EditorProject::MoveAsset(AssetState& state, const QString& from, const QString& to) const {
    const QString problem = CheckAssetPath(to);
    if (!problem.isEmpty()) return problem;
    const QString fromKey = AssetKey(from);
    const QString toKey = AssetKey(to);
    const auto entries = AssetEntries(state);
    const auto source = std::find_if(entries.begin(), entries.end(), [&](const AssetEntry& entry) { return AssetKey(entry.path) == fromKey; });
    if (source == entries.end()) return tr("找不到资源：%1").arg(from);
    if (source->ambiguous) return tr("资源路径具有多份来源，不能选择其中一份改名：%1").arg(from);
    if (fromKey == toKey && from == to) return {};
    // 只改大小写时不算重名。
    if (fromKey != toKey && std::any_of(entries.begin(), entries.end(), [&](const AssetEntry& entry) { return AssetKey(entry.path) == toKey; })) {
        return tr("已有同名资源：%1").arg(to);
    }
    if (source->Pending()) {
        for (auto& pending : state.imports) {
            if (AssetKey(pending.archivePath) == fromKey) {
                pending.archivePath = to;
                if (pending.ownedPayload && to.endsWith(".lua", Qt::CaseInsensitive)) pending.runtimeLuaDraft = true;
            }
        }
        // 移动的是当前可见的新内容。旧成员不能在原路径重新出现，也不能再复制到目标覆盖新内容。
        if (!source->originalPath.isEmpty()) {
            const auto edit = std::find_if(state.edits.begin(), state.edits.end(),
                [&](const AssetEdit& value) { return AssetKey(value.from) == AssetKey(source->originalPath); });
            if (edit == state.edits.end()) state.edits.push_back({source->originalPath, {}});
            else edit->to.clear();
        }
    } else {
        const auto edit = std::find_if(state.edits.begin(), state.edits.end(),
            [&](const AssetEdit& value) { return AssetKey(value.from) == AssetKey(source->originalPath); });
        if (source->originalPath == to) {
            if (edit != state.edits.end()) state.edits.erase(edit); // 移回原处
        } else if (edit != state.edits.end()) {
            edit->to = to;
        } else {
            state.edits.push_back({source->originalPath, to});
        }
    }
    // 待导入内容回到原成员位置时直接覆盖；不同时提交同路径的 removal 与 replacement。
    std::erase_if(state.edits, [&](const AssetEdit& edit) {
        return edit.to.isEmpty() && std::any_of(state.imports.begin(), state.imports.end(),
            [&](const PendingImport& pending) { return AssetKey(pending.archivePath) == AssetKey(edit.from); });
    });
    return {};
}

QString EditorProject::RemoveAsset(const QString& path) {
    auto candidate = CaptureAssets();
    const auto problem = RemoveAsset(candidate, path);
    if (problem.isEmpty()) PublishAssetState(std::move(candidate));
    return problem;
}

QString EditorProject::RemoveAsset(AssetState& state, const QString& path) const {
    const QString key = AssetKey(path);
    const auto entries = AssetEntries(state);
    const auto source = std::find_if(entries.begin(), entries.end(), [&](const AssetEntry& entry) { return AssetKey(entry.path) == key; });
    if (source == entries.end()) return tr("找不到资源：%1").arg(path);
    if (source->ambiguous) return tr("资源路径具有多份来源，不能选择其中一份删除：%1").arg(path);
    if (source->Pending()) {
        std::erase_if(state.imports, [&](const PendingImport& pending) { return AssetKey(pending.archivePath) == key; });
    }
    if (!source->originalPath.isEmpty()) {
        const auto edit = std::find_if(state.edits.begin(), state.edits.end(),
            [&](const AssetEdit& value) { return AssetKey(value.from) == AssetKey(source->originalPath); });
        if (edit != state.edits.end()) edit->to.clear();
        else state.edits.push_back({source->originalPath, {}});
    }
    return {};
}

QString EditorProject::DisabledIconPath(const QString& iconPath) {
    const QString name = QString(iconPath).replace(QLatin1Char('/'), QLatin1Char('\\')).section(QLatin1Char('\\'), -1);
    const QString upper = name.toUpper();
    if (!upper.endsWith(QLatin1String(".BLP")) && !upper.endsWith(QLatin1String(".TGA"))) return {};
    if (!upper.startsWith(QLatin1String("BTN")) && !upper.startsWith(QLatin1String("PAS"))) return {};
    return QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\DIS") + name;
}

QStringList EditorProject::MissingDisabledIcons() const {
    const auto entries = AssetEntries();
    QSet<QString> keys;
    for (const auto& entry : entries) keys.insert(AssetKey(entry.path));
    QStringList missing;
    for (const auto& entry : entries) {
        const QString disabled = DisabledIconPath(entry.path);
        if (disabled.isEmpty() || keys.contains(AssetKey(disabled))) continue;
        if (iconProvider_ && iconProvider_->HasGameFile(disabled)) continue;
        missing << entry.path;
    }
    return missing;
}

AssetState EditorProject::CaptureAssets() const {
    return {pendingImports_, assetEdits_};
}

void EditorProject::RestoreAssets(const AssetState& state) {
    PublishAssetState(state);
}

void EditorProject::PublishAssetState(AssetState state) {
    pendingImports_.swap(state.imports);
    assetEdits_.swap(state.edits);
    NotifyImportsChanged(true);
}

} // namespace War3Map::QtProject
