#include "MainWindow.h"
#include "TriggerModule.h"
#include "AssetOwnedPayload.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Runtime/LuaModuleNames.h"
#include "War3MapCore/Services/LuaTestMailboxSession.h"

#include <QJsonObject>

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

void MainWindow::SnapshotLuaFileBaseline() {
    luaFileBaseline_.clear(); luaFileQueue_.clear(); luaFileSequence_ = 0; luaFileLastMessage_.clear();
    for (const auto& entry : project_->AssetEntries()) {
        if (!IsLuaPath(entry.path) || entry.ambiguous) continue;
        const auto read = War3Map::QtProject::ReadOwnedAsset(*project_, entry.path);
        if (read.Succeeded()) luaFileBaseline_[War3Map::QtProject::EditorProject::AssetKey(entry.path)] = read.value->sha256;
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
        luaFileQueue_.clear(); state = "stopped"; message = tr("本次测试游戏已退出。");
    } else if (!luaFileQueue_.empty()) {
        // 已有文件在等待游戏确认：只推进队列，不接受新的发送。
        if (snapshot.state == Services::LuaTestReloadState::Complete && snapshot.sequence == luaFileSequence_) {
            const auto done = luaFileQueue_.front();
            if (snapshot.resultCode == Runtime::LuaReloadResult::Success) {
                luaFileBaseline_[War3Map::QtProject::EditorProject::AssetKey(done.path)] = done.sha256;
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
        if (!triggersModule_->CommitPendingEdits())
            return Failure("LUA_RELOAD_DRAFT_INVALID", tr("代码草稿未能提交，没有发送任何文件。"));
        std::vector<LuaFilePending> changed;
        for (const auto& entry : project_->AssetEntries()) {
            if (!IsLuaPath(entry.path)) continue;
            if (entry.ambiguous) return Failure("LUA_RELOAD_SOURCE_AMBIGUOUS", tr("%1 有多份来源，没有发送任何文件。").arg(entry.path));
            const auto read = War3Map::QtProject::ReadOwnedAsset(*project_, entry.path);
            if (!read.Succeeded()) {
                return Failure("LUA_RELOAD_READ", tr("无法读取 %1：%2").arg(entry.path,
                    read.diagnostics.empty() ? QString{} : QString::fromStdString(read.diagnostics.back().message)));
            }
            const auto key = War3Map::QtProject::EditorProject::AssetKey(entry.path);
            const auto baseline = luaFileBaseline_.find(key);
            if (baseline != luaFileBaseline_.end() && baseline->second == read.value->sha256) continue;
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
