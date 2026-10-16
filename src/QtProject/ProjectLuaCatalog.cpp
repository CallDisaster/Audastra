#include "EditorProject.h"
#include "AssetOwnedPayload.h"
#include "ProjectLuaModules.h"

#include <QStringDecoder>
#include <algorithm>

#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaApiCatalog.h"
#include "War3MapCore/Services/LuaRuntimeCatalog.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

namespace War3Map::QtProject {

std::shared_ptr<const Lua::ApiCatalog> EditorProject::LuaRuntimeCatalog() {
    if (luaRuntimeScriptsRoot_ != war3GameRoot_) {
        luaRuntimeScriptsRoot_ = war3GameRoot_;
        luaRuntimeScripts_.reset();
        luaRuntimeCatalog_.reset();
        if (!war3GameRoot_.empty()) {
            auto scripts = Services::ReadGameLuaRuntimeScripts(war3GameRoot_);
            if (scripts.Succeeded() && scripts.value)
                luaRuntimeScripts_ = std::make_shared<const Services::LuaRuntimeScripts>(std::move(*scripts.value));
        }
    }
    // 地图变量经 jass.globals 读取，名字是 udg_ 前缀的 JASS 全局。
    std::vector<std::pair<std::string, std::string>> globals;
    std::string key;
    if (triggerDocument_) {
        for (const auto& variable : triggerDocument_->variables) {
            auto type = variable.typeName.text + (variable.isArray ? " array" : "");
            key += variable.name.text + ':' + type + '\n';
            globals.emplace_back("udg_" + variable.name.text, std::move(type));
        }
    }
    if (!luaRuntimeCatalog_ || key != luaRuntimeCatalogKey_) {
        luaRuntimeCatalog_ = Services::BuildLuaRuntimeCatalog(luaRuntimeScripts_.get(), globals);
        luaRuntimeCatalogKey_ = std::move(key);
    }
    return luaRuntimeCatalog_;
}

std::string LuaModuleName(const QString& path) {
    if (!path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) return {};
    auto name = path.left(path.size() - 4);
    name.replace('\\', '.').replace('/', '.');
    return name.toStdString();
}

std::shared_ptr<const LuaModuleTexts> CaptureLuaModules(const EditorProject& project, const QString& current) {
    constexpr std::size_t kTotalBytes = 16 * 1024 * 1024;
    auto modules = std::make_shared<LuaModuleTexts>();
    std::size_t total = 0;
    for (const auto& entry : project.AssetEntries()) {
        if (entry.ambiguous || EditorProject::AssetKey(entry.path) == EditorProject::AssetKey(current)) continue;
        auto name = LuaModuleName(entry.path);
        if (name.empty()) continue;
        const auto read = ReadOwnedAsset(project, entry.path);
        if (!read.Succeeded() || !read.value->bytes || total + read.value->bytes->size() > kTotalBytes) continue;
        total += read.value->bytes->size();
        const auto& bytes = *read.value->bytes;
        modules->emplace(std::move(name), std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    return modules;
}

std::vector<LuaScriptEntry> ListLuaScripts(const EditorProject& project) {
    std::vector<LuaScriptEntry> scripts;
    for (const auto& entry : project.AssetEntries()) {
        if (!entry.path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) continue;
        scripts.push_back({entry.path, entry.ambiguous ? QObject::tr("来源歧义") : entry.runtimeLuaDraft ? QObject::tr("工程草稿")
            : entry.Pending() ? QObject::tr("待导入") : QObject::tr("原内容"), entry.size});
    }
    std::sort(scripts.begin(), scripts.end(), [](const auto& a, const auto& b) {
        const auto folded = a.path.compare(b.path, Qt::CaseInsensitive);
        return folded < 0 || (folded == 0 && a.path.compare(b.path, Qt::CaseSensitive) < 0);
    });
    return scripts;
}

Result<LuaScriptText> ReadLuaScript(const EditorProject& project, const QString& path) {
    auto read = ReadOwnedAsset(project, path);
    if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
    const auto& bytes = *read.value->bytes;
    const QByteArray raw(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    LuaScriptText result{decoder.decode(raw), read.value->sha256};
    if (decoder.hasError()) return Result<LuaScriptText>::Failure("Lua", "LUA_SCRIPT_NOT_UTF8", "Lua 文件不是 UTF-8 编码。");
    if (result.sha256.empty()) {
        auto hash = Sha256(bytes);
        if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
        result.sha256 = std::move(*hash.value);
    }
    return Result<LuaScriptText>::Success(std::move(result));
}

QString WriteLuaScript(EditorProject& project, const QString& path, QString text,
    const std::string& expectedSha256, const QString& label) {
    if (!path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) return QObject::tr("只能写入 .lua 文件。");
    const auto entries = project.AssetEntries();
    const auto existing = std::find_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == EditorProject::AssetKey(path); });
    if (existing != entries.end()) {
        if (existing->ambiguous) return QObject::tr("该路径有多份来源，未写入。");
        const auto current = ReadOwnedAsset(project, existing->path);
        if (!current.Succeeded()) return QString::fromUtf8(current.diagnostics.empty() ? std::string("读取原文件失败。") : current.diagnostics.front().message);
        const auto& old = *current.value->bytes;
        if (!expectedSha256.empty()) {
            auto hash = Sha256(old);
            if (!hash.Succeeded() || *hash.value != expectedSha256) return QObject::tr("文件已被修改，请重新读取后再写入。");
        }
        const QByteArray oldBytes(reinterpret_cast<const char*>(old.data()), static_cast<qsizetype>(old.size()));
        if (oldBytes.contains("\r\n") && !QByteArray(oldBytes).replace("\r\n", "").contains('\n')) {
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")); text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        }
    } else if (!expectedSha256.empty()) return QObject::tr("文件不存在，无法按指定版本替换。");
    else if (project.directorySnapshot_ && !project.archiveSession_) return QObject::tr("目录扫描不完整，不能新建。");
    const auto utf8 = text.toUtf8();
    const auto* start = reinterpret_cast<const std::byte*>(utf8.constData());
    auto bytes = std::make_shared<const std::vector<std::byte>>(start, start + utf8.size());
    const auto target = existing != entries.end() ? existing->path : path;
    return project.RecordAssetEdit(label, [&](AssetState& candidate) { return PutOwnedAsset(project, candidate, target, bytes); });
}

} // namespace War3Map::QtProject
