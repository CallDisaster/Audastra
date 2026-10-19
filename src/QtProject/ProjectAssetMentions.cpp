#include "EditorProject.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <array>
#include <optional>

// 删除资源前的路径查找：逐个来源读入、查找后即释放，不把整张地图同时放进内存。
// 查找规则由 Services::FindArchivePathMentions 统一提供，这里只负责给出“当前内容”。
namespace War3Map::QtProject {
namespace {

constexpr std::uint64_t kMaxScanBytes = 64ull * 1024 * 1024;

bool ScannableAsset(const QString& key) {
    for (const auto* extension : {".mdx", ".mdl", ".j", ".lua", ".txt", ".fdf", ".toc", ".slk", ".ini"})
        if (key.endsWith(QLatin1String(extension))) return true;
    return false;
}

} // namespace

EditorProject::AssetMentionScan EditorProject::FindAssetMentions(const QStringList& paths) const {
    AssetMentionScan scan;
    scan.mentions.resize(static_cast<std::size_t>(paths.size()));
    std::vector<std::string> targets;
    for (const auto& path : paths) targets.push_back(path.toStdString());
    const auto scanBytes = [&](std::string name, std::vector<std::byte> bytes) {
        const std::array<MapArchiveMember, 1> member{MapArchiveMember{std::move(name), std::move(bytes)}};
        auto found = Services::FindArchivePathMentions(member, targets);
        for (std::size_t index = 0; index < found.size(); ++index)
            for (auto& mention : found[index]) scan.mentions[index].push_back(std::move(mention));
        ++scan.scannedMembers;
    };
    // 地图自带的成员：缺失是正常的（例如没有 war3mapSkin.w3u），只有脚本必须至少读到一份。
    const auto scanSource = [&](const std::string& name) {
        auto bytes = ReadMapMember(name);
        if (!bytes.Succeeded() || !bytes.value) return false;
        scanBytes(name, std::move(*bytes.value));
        return true;
    };

    bool script = false;
    for (const auto* name : {"war3map.j", "scripts\\war3map.j", "war3map.lua"}) script = scanSource(name) || script;
    if (!script) scan.unread << QStringLiteral("war3map.j");
    for (const auto* name : {"war3map.w3i", "war3map.wts", "war3mapMisc.txt", "war3mapSkin.txt", "war3mapExtra.txt",
             "war3mapSkin.w3u", "war3mapSkin.w3t", "war3mapSkin.w3b", "war3mapSkin.w3d", "war3mapSkin.w3a",
             "war3mapSkin.w3h", "war3mapSkin.w3q"})
        scanSource(name);

    // 触发器：有未保存修改时按当前文档编码（与修改指纹同一写法，只读文本），否则读地图里的原文件。
    if (triggerDirty_ && triggerDocument_) {
        auto encoded = Trigger::WtgWctCodec{}.EncodeForIdentity(*triggerDocument_);
        if (encoded.Succeeded() && encoded.value) {
            scanBytes("war3map.wtg", std::move(encoded.value->wtg));
            scanBytes("war3map.wct", std::move(encoded.value->wct));
        } else {
            scan.unread << QStringLiteral("war3map.wtg") << QStringLiteral("war3map.wct");
        }
    } else {
        scanSource("war3map.wtg");
        scanSource("war3map.wct");
    }

    // 物编：有修改的类型按当前文档渲染为 LNI 文本，其余读地图里的 native 文件。
    for (std::size_t index = 0; index < objectDocuments_.size(); ++index) {
        const auto& document = objectDocuments_[index];
        if (!document) continue;
        const std::string name(ObjectData::NativeFileName(document->Type()));
        if (!objectDirty_[index]) {
            scanSource(name);
            continue;
        }
        auto text = ObjectData::LniObjectCodec(document->Type()).Serialize(*document);
        if (text.Succeeded() && text.value) scanBytes(name, std::move(*text.value));
        else scan.unread << QString::fromStdString(name);
    }

    // 资源：当前路径（含改名、待导入与 Lua 草稿），只读可能写路径的类型。
    for (const auto& entry : AssetEntries()) {
        const auto key = AssetKey(entry.path);
        if (!ScannableAsset(key)) continue;
        std::optional<std::vector<std::byte>> bytes;
        if (entry.ownedPayload) bytes = *entry.ownedPayload;
        else if (!entry.diskPath.isEmpty()) {
            auto read = IO::ReadFileBytes(std::filesystem::path(entry.diskPath.toStdWString()), kMaxScanBytes);
            if (read.Succeeded() && read.value) bytes = std::move(*read.value);
        } else if (!entry.originalPath.isEmpty() && !entry.ambiguous) {
            auto read = ReadMapMember(entry.originalPath.toStdString());
            if (read.Succeeded() && read.value) bytes = std::move(*read.value);
        }
        if (bytes) scanBytes(entry.path.toStdString(), std::move(*bytes));
        else scan.unread << entry.path;
    }
    return scan;
}

} // namespace War3Map::QtProject
