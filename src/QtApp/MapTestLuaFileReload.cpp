#include "MainWindow.h"
#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
#include "AssetOwnedPayload.h"
#include "ProjectLuaModules.h"
#include "ProjectLuaFolder.h"
#include "../QtCommon/LuaReloadButtonState.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Runtime/LuaModuleNames.h"
#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "War3MapCore/HLua/Project.h"
#include "War3MapCore/HLua/ProjectConfiguration.h"
#include <QSettings>

#include <QJsonObject>
#include <QAction>
#include <QStyle>
#include <QToolButton>
#include <set>

// 作者手写 Lua 的测试热重载（YDWE 约定：脚本里 Cheat("exec-lua: \"main\"") 加载地图内 .lua）。
// 测试启动时记录每个 .lua 文件的 SHA；热重载只发送之后改过并已应用的文件，按 “module:<模块名>”
// 逐个送进游戏，游戏确认一个再发下一个。发送前在编辑器完成语法检查，有错误时一个也不发。
namespace {
namespace Services = War3Map::Services;
namespace Runtime = Audastra::Runtime32;

QJsonObject Failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"code", code}, {"message", message}};
}
bool IsLuaPath(const QString& path) { return path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive); }
// 文件路径 → 模块名：lua\main.lua → lua.main；要求与运行时 require 的成员规则一致。
std::string ModuleName(const QString& path) {
    auto module = path.left(path.size() - 4).toStdString();
    for (auto& c : module) if (c == '\\' || c == '/') c = '.';
    const auto member = Runtime::LuaModuleMember(module);
    auto canonical = path.toStdString();
    for (auto& c : canonical) if (c == '/') c = '\\';
    return !member.empty() && QString::fromStdString(member).compare(QString::fromStdString(canonical), Qt::CaseInsensitive) == 0
        ? module : std::string{};
}
}

void MainWindow::SnapshotLuaFileBaseline(const std::filesystem::path& frozenMap) {
    luaFileBaseline_.clear(); luaFileQueue_.clear(); luaFileSequence_ = 0; luaFileLastMessage_.clear();
    luaFileReloadStopped_ = false; luaFileAutoReloadPending_ = false; luaFileDiffRevision_.reset(); luaFileChangedPaths_.clear();
    if (project_->LuaFolderSource().Linked() && !frozenMap.empty()) {
        const auto map = War3Map::Mpq::MpqArchive::OpenReadOnly(frozenMap);
        const auto files = map.Succeeded() ? map.value->ListFilesPreservingOpaque()
            : War3Map::Result<std::vector<War3Map::Mpq::MpqStrictFileInfo>>{std::nullopt, map.diagnostics};
        if (files.Succeeded()) for (const auto& file : *files.value) {
            if (!QString::fromStdString(file.name).endsWith(".lua", Qt::CaseInsensitive)
                || War3Map::Resources::IsEditorManagedMemberPath(file.name)) continue;
            if(HasHLuaProject() && !hluaAuthorPaths_.contains(War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(file.name)).toStdString()))continue;
            const auto bytes = map.value->ReadFileBounded(file.name, War3Map::QtProject::kOwnedLuaAssetBytes);
            if (!bytes.Succeeded()) continue;
            const auto hash = War3Map::Sha256(*bytes.value);
            if (hash.Succeeded()) luaFileBaseline_[War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(file.name))] = *hash.value;
        }
        return;
    }
    for (const auto& entry : War3Map::QtProject::ListLuaScripts(*project_)) {
        if (!IsLuaPath(entry.path)) continue;
        const auto read = War3Map::QtProject::ReadOwnedAsset(*project_, entry.path);
        if (read.Succeeded()) luaFileBaseline_[War3Map::QtProject::EditorProject::AssetKey(entry.path)] = read.value->sha256;
    }
}

bool MainWindow::LuaFileDiffersFromBaseline(const QString& path, const std::string& sha256) const {
    const auto baseline = luaFileBaseline_.find(War3Map::QtProject::EditorProject::AssetKey(path));
    return baseline == luaFileBaseline_.end() || baseline->second != sha256;
}

void MainWindow::RememberReloadedLuaFile(const QString& path, const std::string& sha256) {
    luaFileBaseline_[War3Map::QtProject::EditorProject::AssetKey(path)] = sha256;
    luaFileDiffRevision_.reset();
}

void MainWindow::UpdateLuaReloadPending() {
    if (!luaReloadAction_) return;
    if(!luaFileReload_ || luaFileReloadStopped_ || !LuaTestSessionCurrent())hluaRebuildRequired_=false;
    QStringList paths;
    if (luaFileReload_ && !luaFileReloadStopped_ && luaRuntimeMailbox_ && LuaTestSessionCurrent()) {
        const std::array<std::uint64_t, 2> revision{project_->editRevision_.Generation(), project_->editRevision_.Revision()};
        if (luaFileDiffRevision_ != revision) {
            luaFileChangedPaths_.clear();
            hluaRebuildRequired_=false;
            std::set<std::string> hluaPaths;
            if(HasHLuaProject()) {
                const auto source=War3Map::HLua::HLuaProjectScanner::Capture(project_->LuaFolderSource().Root());
                const auto plan=source.Succeeded()?War3Map::HLua::PrepareReload(*source.value,hluaBuiltHslkSha256_)
                    : War3Map::Result<War3Map::HLua::ReloadPlan>{std::nullopt,source.diagnostics};
                hluaRebuildRequired_=!plan.Succeeded() || plan.value->rebuildRequired;
                if(plan.Succeeded())for(const auto& file:plan.value->authorModules)hluaPaths.insert(
                    War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(file.path)).toStdString());
            }
            std::set<QString> present;
            for (const auto& entry : War3Map::QtProject::ListLuaScripts(*project_)) {
                if (!IsLuaPath(entry.path)) continue;
                if(HasHLuaProject() && !hluaPaths.contains(War3Map::QtProject::EditorProject::AssetKey(entry.path).toStdString()))continue;
                present.insert(War3Map::QtProject::EditorProject::AssetKey(entry.path));
                const auto read = War3Map::QtProject::ReadOwnedAsset(*project_, entry.path);
                if (!read.Succeeded() || LuaFileDiffersFromBaseline(entry.path, read.value->sha256))
                    luaFileChangedPaths_.push_back(entry.path);
            }
            for (const auto& [path, sha] : luaFileBaseline_) if (Services::IsAuthorLuaMember(path.toStdString()) && !present.contains(path))
                luaFileChangedPaths_.push_back(path);
            luaFileDiffRevision_ = revision;
        }
        paths = luaFileChangedPaths_;
        if (const auto* scripts = triggersModule_->LuaScripts(); scripts && scripts->HasPendingDraft()
            && !paths.contains(scripts->SelectedPath())) paths.push_back(scripts->SelectedPath());
    }
    const bool pending = !paths.empty() || hluaRebuildRequired_;
    if(HasHLuaProject())luaReloadAction_->setText(hluaRebuildRequired_?tr("热重载 Lua（需重新构建）"):tr("热重载 Lua"));
    luaReloadAction_->setProperty("luaReloadPending", pending);
    const bool rebuild=HasHLuaProject() && hluaRebuildRequired_;
    auto tooltip = rebuild?tr("hslk 已改变，请重新构建并启动测试；未发送模块。"):tr("热重载当前 Lua 测试");
    if (!rebuild && pending) tooltip += tr("\n待发送：%1").arg(paths.mid(0, 16).join(QStringLiteral("、")));
    if (paths.size() > 16) tooltip += tr("等 %1 个文件").arg(paths.size());
    luaReloadAction_->setToolTip(tooltip);
    for (auto* object : luaReloadAction_->associatedObjects()) {
        auto* button = qobject_cast<QToolButton*>(object);
        if(button)War3Map::QtCommon::ApplyLuaReloadButtonState(*button,luaReloadAction_->text(),pending,rebuild);
    }
}

QJsonObject MainWindow::LuaFileReload(bool submit) {
    if (!luaRuntimeMailbox_) return Failure("LUA_RELOAD_NO_SESSION", tr("请先启动使用 Audastra 自带 Lua 环境的地图测试。"));
    const auto query = Services::QueryLuaTestReload(*luaRuntimeMailbox_, luaFileQueue_.empty() ? 0 : luaFileSequence_);
    if (!query.Succeeded() || !query.value) {
        const auto& last = query.diagnostics;
        return Failure(last.empty() ? "LUA_RELOAD_INCOMPLETE" : QString::fromStdString(last.back().code),
            last.empty() ? tr("无法读取游戏中的热重载状态。") : QString::fromStdString(last.back().message));
    }
    const auto& snapshot = *query.value;
    QString state = "ready";
    QString message = luaFileLastMessage_;
    const bool stopped = snapshot.state == Services::LuaTestReloadState::Stopped
        || (!snapshot.processActive && snapshot.state != Services::LuaTestReloadState::Starting);
    const auto send = [&]() -> bool {
        const auto& next = luaFileQueue_.front();
        const auto sent = Services::SubmitLuaTestReload(*luaRuntimeMailbox_, {"module:" + next.module, next.source});
        if (!sent.Succeeded() || !sent.value) {
            luaFileQueue_.clear();
            state = "failed";
            message = sent.diagnostics.empty() ? tr("热重载请求没有送达游戏。") : QString::fromStdString(sent.diagnostics.back().message);
            return false;
        }
        luaFileSequence_ = *sent.value;
        state = "pending"; message = tr("正在把 %1 送进游戏…").arg(next.path);
        return true;
    };
    if (stopped) {
        luaFileReloadStopped_ = true;
        luaFileAutoReloadPending_ = false;
        luaFileQueue_.clear(); state = "stopped"; message = tr("本次测试游戏已退出。");
    } else if (!luaFileQueue_.empty()) {
        // 已有文件在等待游戏确认：只推进队列，不接受新的发送。
        if (snapshot.state == Services::LuaTestReloadState::Complete && snapshot.sequence == luaFileSequence_) {
            const auto done = luaFileQueue_.front();
            if (snapshot.resultCode == Runtime::LuaReloadResult::Success) {
                RememberReloadedLuaFile(done.path, done.sha256);
                luaFileQueue_.erase(luaFileQueue_.begin());
                state = "succeeded"; message = tr("已热重载 %1").arg(done.path);
                if (!luaFileQueue_.empty()) (void)send();
            } else {
                // 游戏拒绝或执行出错：后续文件不再发送，原逻辑保持；错误文本含文件与行。
                luaFileQueue_.clear(); state = "failed";
                message = tr("%1 热重载失败：%2").arg(done.path, QString::fromStdString(snapshot.message));
            }
        } else {
            state = "pending"; message = tr("正在把 %1 送进游戏…").arg(luaFileQueue_.front().path);
        }
        if (submit && state == "pending") return Failure("LUA_RELOAD_PENDING", tr("上一份 Lua 文件尚未收到游戏确认。"));
    } else if (submit) {
        if (snapshot.state == Services::LuaTestReloadState::Starting)
            return Failure("LUA_RELOAD_NOT_READY", tr("游戏中的 Lua 环境还在初始化，请进入地图后再热重载。"));
        if (project_->LuaFolderSource().Linked()) {
            project_->LuaFolderSource().Refresh();
            const auto& folder = project_->LuaFolderSource();
            if (!folder.Active() && !folder.Missing()) {
                const auto& diagnostics = folder.Diagnostics();
                return Failure("LUA_RELOAD_READ", diagnostics.empty() ? tr("外部Lua来源不可用。") : QString::fromStdString(diagnostics.front().message));
            }
            if (triggersModule_->LuaScripts()->HasPendingDraft())
                return Failure("LUA_RELOAD_DRAFT_INVALID", tr("编辑器仍有未应用草稿，请先处理草稿或外部冲突。"));
        }
        if (!triggersModule_->CommitPendingEdits())
            return Failure("LUA_RELOAD_DRAFT_INVALID", tr("代码草稿未能提交，没有发送任何文件。"));
        std::vector<LuaFilePending> changed;
        auto available = War3Map::QtProject::ListLuaScripts(*project_);
        if(HasHLuaProject()) {
            QSettings settings;
            const auto location=War3Map::HLua::ResolveProjectFramework(project_->LuaFolderSource().Root(),{},
                {std::filesystem::path(settings.value("hlua/frameworkRoot").toString().toStdWString()),{},{}});
            const auto framework=location.Succeeded()?War3Map::HLua::HLuaFrameworkLocator::Capture(*location.value)
                :War3Map::Result<War3Map::HLua::FrameworkSnapshot>{std::nullopt,location.diagnostics};
            if(!framework.Succeeded() || framework.value->sha256!=hluaBuiltFrameworkSha256_)
                return Failure("HLUA_REBUILD_REQUIRED",tr("h-lua 框架目录或内容已改变，需重新构建测试图；未发送模块。"));
            const auto source=War3Map::HLua::HLuaProjectScanner::Capture(project_->LuaFolderSource().Root());
            if(!source.Succeeded())return Failure("HLUA_RELOAD_SOURCE",tr("无法冻结作者模块，未发送任何文件。"));
            const auto plan=War3Map::HLua::PrepareReload(*source.value,hluaBuiltHslkSha256_);
            if(!plan.Succeeded() || plan.value->rebuildRequired)
                return Failure("HLUA_REBUILD_REQUIRED",tr("hslk 已改变或构建身份不完整，需重新构建测试图；未发送模块。"));
            std::set<QString> allowed;
            for(const auto& file:plan.value->authorModules)allowed.insert(War3Map::QtProject::EditorProject::AssetKey(QString::fromStdString(file.path)));
            std::erase_if(available,[&](const auto& entry){return !allowed.contains(War3Map::QtProject::EditorProject::AssetKey(entry.path));});
        }
        for (const auto& [path, sha] : luaFileBaseline_) if (Services::IsAuthorLuaMember(path.toStdString())
            && std::none_of(available.begin(), available.end(), [&](const auto& entry) {
                return War3Map::QtProject::EditorProject::AssetKey(entry.path) == path;
            })) return Failure("LUA_RELOAD_FILE_DELETED", tr("%1 已删除；请保存并重新测试，以清除游戏中的旧模块。").arg(path));
        for (const auto& entry : available) {
            if (!IsLuaPath(entry.path)) continue;
            const auto read = War3Map::QtProject::ReadOwnedAsset(*project_, entry.path);
            if (!read.Succeeded()) {
                return Failure("LUA_RELOAD_READ", tr("无法读取 %1：%2").arg(entry.path,
                    read.diagnostics.empty() ? QString{} : QString::fromStdString(read.diagnostics.back().message)));
            }
            if (!LuaFileDiffersFromBaseline(entry.path, read.value->sha256)) continue;
            LuaFilePending file{entry.path, ModuleName(entry.path), {}, read.value->sha256};
            if (file.module.empty())
                return Failure("LUA_RELOAD_MODULE_NAME", tr("%1 的路径不能作为 Lua 模块名（只能用字母、数字、中文等，目录用 \\ 分隔）。").arg(entry.path));
            file.source.assign(reinterpret_cast<const char*>(read.value->bytes->data()), read.value->bytes->size());
            if (file.source.empty() || file.source.size() >= Runtime::kLuaReloadSourceCapacity)
                return Failure("LUA_RELOAD_SIZE", tr("%1 为空或超过 1 MiB，不能热重载。").arg(entry.path));
            if (!Runtime::LuaMailboxUtf8(file.source))
                return Failure("LUA_RELOAD_ENCODING", tr("%1 不是 UTF-8 文本，自带 Lua 环境不能读取。").arg(entry.path));
            const auto checked = War3Map::Lua::CheckSyntax(file.source, "@" + entry.path.toStdString());
            if (!checked.Succeeded()) {
                const auto line = checked.diagnostics.empty() ? 0u : checked.diagnostics.front().line;
                const auto text = checked.diagnostics.empty() ? std::string("语法检查未完成。") : checked.diagnostics.front().message;
                return Failure("LUA_RELOAD_SYNTAX", tr("%1 第 %2 行有语法错误，没有发送任何文件：%3")
                    .arg(entry.path).arg(line).arg(QString::fromStdString(text)));
            }
            changed.push_back(std::move(file));
        }
        if (changed.empty()) {
            state = "no_changes"; message = tr("测试开始后 Lua 文件没有改动（修改后请先“应用”）。");
        } else {
            luaFileQueue_ = std::move(changed);
            (void)send();
        }
    }
    if (!submit && luaFileAutoReloadPending_ && luaFileQueue_.empty() && !stopped
        && snapshot.state != Services::LuaTestReloadState::Starting) {
        luaFileAutoReloadPending_ = false;
        if (luaAutoReloadAction_ && luaAutoReloadAction_->isChecked() && !triggersModule_->LuaScripts()->HasPendingDraft())
            return LuaFileReload(true);
    }
    luaFileLastMessage_ = message;
    QJsonObject result{{"ok", state != "failed" || !submit}, {"state", state},
        {"sequence", QString::number(luaFileSequence_)}, {"message", message}, {"seconds", 0.0}};
    if (state == "failed" && submit) result.insert("code", "LUA_RELOAD_FAILED");
    result.insert("runtime_diagnostic_sequence", QString::number(snapshot.runtimeDiagnosticSequence));
    result.insert("runtime_diagnostic_code", static_cast<int>(snapshot.runtimeDiagnosticCode));
    result.insert("runtime_message", QString::fromStdString(snapshot.runtimeMessage));
    result.insert("runtime_diagnostic_truncated", snapshot.runtimeDiagnosticTruncated);
    return result;
}
