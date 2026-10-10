#pragma once

#include <QMimeData>
#include <QObject>
#include <QStandardItemModel>
#include <QString>
#include <QStringList>

// 资源模块内部的路径与列表工具。归档路径统一用反斜杠显示与比较（大小写不敏感）。
namespace War3Map::Modules::AssetsDetail {

inline constexpr int kPathRole = Qt::UserRole + 1;  // 文件或文件夹的归档路径
inline constexpr int kKindRole = Qt::UserRole + 2;  // 0 文件夹，1 文件
inline constexpr int kSortRole = Qt::UserRole + 3;
inline constexpr char kMime[] = "application/x-war3map-asset-paths";

inline QString Normalized(const QString& path) {
    return QString(path).replace(QLatin1Char('/'), QLatin1Char('\\'));
}

inline QString ParentFolder(const QString& path) {
    const QString normalized = Normalized(path);
    const qsizetype slash = normalized.lastIndexOf(QLatin1Char('\\'));
    return slash < 0 ? QString{} : normalized.left(slash);
}

inline QString FileName(const QString& path) {
    return Normalized(path).section(QLatin1Char('\\'), -1);
}

inline QString JoinPath(const QString& folder, const QString& name) {
    return folder.isEmpty() ? name : folder + QLatin1Char('\\') + name;
}

inline bool SameFolder(const QString& left, const QString& right) {
    return Normalized(left).compare(Normalized(right), Qt::CaseInsensitive) == 0;
}

// folder 是否在 root 之内（或就是 root）。
inline bool InFolder(const QString& path, const QString& root) {
    if (root.isEmpty()) return true;
    const QString normalized = Normalized(path);
    return normalized.startsWith(Normalized(root) + QLatin1Char('\\'), Qt::CaseInsensitive) || SameFolder(normalized, root);
}

inline QString TypeText(const QString& name) {
    const QString ext = name.section(QLatin1Char('.'), -1).toLower();
    if (!name.contains(QLatin1Char('.'))) return QObject::tr("文件");
    if (ext == QLatin1String("blp")) return QObject::tr("BLP 纹理");
    if (ext == QLatin1String("tga") || ext == QLatin1String("dds") || ext == QLatin1String("png")) return QObject::tr("%1 图像").arg(ext.toUpper());
    if (ext == QLatin1String("mdx") || ext == QLatin1String("mdl")) return QObject::tr("%1 模型").arg(ext.toUpper());
    if (ext == QLatin1String("wav") || ext == QLatin1String("mp3") || ext == QLatin1String("flac")) return QObject::tr("%1 音频").arg(ext.toUpper());
    if (ext == QLatin1String("j") || ext == QLatin1String("lua") || ext == QLatin1String("ai")) return QObject::tr("脚本");
    if (ext == QLatin1String("txt") || ext == QLatin1String("slk") || ext == QLatin1String("fdf") || ext == QLatin1String("toc")) return QObject::tr("%1 文本").arg(ext.toUpper());
    return QObject::tr("%1 文件").arg(ext.toUpper());
}

inline QString SizeText(qint64 size) {
    if (size < 1024) return QObject::tr("%1 字节").arg(size);
    if (size < 1024 * 1024) return QObject::tr("%1 KB").arg((size + 1023) / 1024);
    return QObject::tr("%1 MB").arg(static_cast<double>(size) / (1024.0 * 1024.0), 0, 'f', 1);
}

// 列表与文件夹树都可以拖出：拖动数据是选中项的归档路径（文件夹前缀 "folder:"）。
class AssetItemModel final : public QStandardItemModel {
public:
    using QStandardItemModel::QStandardItemModel;
    [[nodiscard]] QStringList mimeTypes() const override { return {QString::fromLatin1(kMime)}; }
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QStringList paths;
        for (const QModelIndex& index : indexes) {
            if (index.column() != 0) continue;
            const QString path = index.data(kPathRole).toString();
            if (index.data(kKindRole).toInt() == 0) {
                if (!path.isEmpty()) paths << QStringLiteral("folder:") + path;
            } else {
                paths << path;
            }
        }
        auto* data = new QMimeData;
        data->setData(QString::fromLatin1(kMime), paths.join(QLatin1Char('\n')).toUtf8());
        return data;
    }
};

} // namespace War3Map::Modules::AssetsDetail
