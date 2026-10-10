"""在显式沙盒/Job/独立 Desktop 中运行一次原创 Lua 元数据探针。

复用既有 P1 的 Win32 结构、Job 查询与锁定 Desktop helper；不调用 AutoTest MCP
顶层、不切换 input Desktop、不连接/停止已有进程、不复制用户运行库。
"""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
MODULES = ("bin/ydbase.dll", "bin/lua53.dll", "bin/ydwar3.dll", "plugin/warcraft3/yd_loader.dll",
           "plugin/warcraft3/yd_jass_api.dll", "plugin/warcraft3/yd_lua_engine.dll")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def plain(value, exists=True):
    path = Path(value)
    require(path.is_absolute() and not str(path).startswith(("\\\\", "//")) and "\x00" not in str(path), "绝对普通路径必需")
    win = PureWindowsPath(str(path))
    require(all(part not in (".", "..") and not part.endswith((".", " ")) and ":" not in part for part in win.parts[1:]), "拒绝路径别名/ADS")
    for parent in (path, *path.parents):
        if parent.exists() or parent.is_symlink():
            info = parent.lstat()
            require(not parent.is_symlink() and not getattr(info, "st_file_attributes", 0) & 0x400, "拒绝 reparse 路径")
            if parent.is_file():
                require(info.st_nlink == 1, "拒绝 hardlink 文件")
    return path.resolve(strict=exists)


def read_json(path, budget=2 * 1024 * 1024):
    require(path.stat().st_size <= budget, "JSON 超预算")
    def pairs(items):
        value = {}
        for key, item in items:
            require(key not in value, "JSON 重复字段")
            value[key] = item
        return value
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError("非有限 JSON")))


def write_new(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def locked_adapter(path, expected):
    require(sha(path) == expected, "既有 Win32 adapter SHA 不符")
    spec = importlib.util.spec_from_file_location("audastra_probe_isolated_adapter", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(sha(path) == expected, "adapter 在载入期间变化")
    return module


def bind_witness_apis(win):
    callback = C.WINFUNCTYPE(C.c_int, C.c_void_p, C.c_ssize_t, use_last_error=True)
    calls = {"EnumDesktopWindows": ([C.c_void_p, callback, C.c_ssize_t], C.c_int),
             "GetWindowThreadProcessId": ([C.c_void_p, C.POINTER(C.c_ulong)], C.c_ulong),
             "GetClassNameW": ([C.c_void_p, C.c_wchar_p, C.c_int], C.c_int),
             "GetWindowLongPtrW": ([C.c_void_p, C.c_int], C.c_ssize_t),
             "IsWindowVisible": ([C.c_void_p], C.c_int)}
    for name, (parameters, result) in calls.items():
        api = getattr(win.u, name); api.argtypes, api.restype = parameters, result
    return callback


def observed_initial_enum_pending(enumerated, error, callback_count, row_count, callback_failed):
    # 实测尚无证据状态；不解释为 Win32 文档承诺的空桌面，也不授予任何 proof。
    return (all(type(value) is int for value in (enumerated, error, callback_count, row_count))
            and enumerated == 0 and error == 0 and callback_count == 0 and row_count == 0
            and callback_failed is False)


def observe_hidden_witness(win, callback_type, desktop, job, process, name, nonce, creation_time, pending_state=None):
    def original_alive():
        require(win.k.WaitForSingleObject(process.process, 0) == 258, "见证前后原 bridge 进程必须仍运行")
        require(win.k.WaitForSingleObject(process.thread, 0) == 258, "见证前后原 bridge 主线程必须仍运行")
        member = C.c_int()
        win.check(win.k.IsProcessInJob(process.process, job, C.byref(member)), "见证核对原句柄 exact Job")
        require(member.value != 0, "原 bridge 句柄不属于本次确切 Job")

    def row(hwnd):
        pid = C.c_ulong()
        C.set_last_error(0)
        tid = win.u.GetWindowThreadProcessId(hwnd, C.byref(pid)); error = C.get_last_error()
        require(tid != 0, "见证 HWND owner 查询失败: " + str(error))
        text = C.create_unicode_buffer(256)
        C.set_last_error(0)
        count = win.u.GetClassNameW(hwnd, text, len(text)); error = C.get_last_error()
        require(0 < count < len(text) - 1, "见证 HWND class 查询失败/截断: " + str(error))
        values = []
        for index in (-16, -20):
            C.set_last_error(0)
            value = win.u.GetWindowLongPtrW(hwnd, index); error = C.get_last_error()
            require(value != 0 or error == 0, "见证 HWND style 查询失败: " + str(error))
            values.append(value & 0xFFFFFFFF)
        return {"hwnd": int(hwnd), "className": text.value, "ownerPid": int(pid.value), "ownerTid": int(tid),
                "style": values[0], "exStyle": values[1], "visible": bool(win.u.IsWindowVisible(hwnd))}

    original_alive()
    rows, errors = [], []
    callback_count = 0
    @callback_type
    def observe(hwnd, _):
        nonlocal callback_count
        callback_count += 1
        try:
            require(len(rows) < 64, "自有 Desktop 顶层窗口超过64项预算")
            rows.append(row(hwnd)); return 1
        except Exception as error:
            errors.append(str(error)); C.set_last_error(1237); return 0
    C.set_last_error(0)
    enumerated = win.u.EnumDesktopWindows(desktop, observe, 0); error = C.get_last_error()
    if observed_initial_enum_pending(enumerated, error, callback_count, len(rows), bool(errors)):
        original_alive()
        if pending_state is not None:
            pending_state["zeroNoCallbackCount"] += 1
            pending_state["lastReturn"], pending_state["lastError"] = enumerated, error
        return None
    require(not errors and enumerated, "自有 Desktop 枚举失败: " + "; ".join(errors[:1]) + " Win32=" + str(error))
    expected = "AudastraProbeDesktopWitness-" + nonce
    matches = [value for value in rows if value["className"] == expected]
    require(len(matches) <= 1, "nonce class 窗口不唯一")
    original_alive()
    if not matches:
        return None
    first = matches[0]
    require(first["ownerPid"] == process.pid and first["ownerTid"] == process.tid, "nonce class 窗口不是原 bridge 主线程")
    require(first["style"] & 0x80000000 and not first["style"] & 0x50000000
            and first["exStyle"] == 0x08000080 and first["visible"] is False, "见证窗口必须为不激活/不显示的顶层窗口")
    require(row(first["hwnd"]) == first, "见证窗口在二次 owner/class/style 核对中变化/消失")
    original_alive()
    return {**first, "proofKind": "hidden-window", "name": name, "threadId": int(process.tid),
            "originalProcessAlive": True, "originalThreadAlive": True, "originalProcessInOwnedJob": True,
            "observationRepeated": True, "enumerationComplete": True, "windowCount": len(rows),
            "originalProcessCreationFileTime": creation_time}


def verify_witness_pair(raw, isolation):
    if isolation["desktopWitnessMode"] == "none":
        require("desktopWitness" not in raw, "默认模式出现未请求的窗口见证")
        return
    evidence = raw.get("desktopWitness", {})
    observed = isolation.get("bridgeDesktop", {})
    require(evidence.get("requested") is True and evidence.get("created") is True
            and evidence.get("destroyed") is True and evidence.get("unregistered") is True
            and evidence.get("mode") == "hidden-window" and evidence.get("proofKind") == "hidden-window"
            and evidence.get("nonce") == isolation["nonce"] and evidence.get("errorWin32") == 0,
            "原生见证创建/清理/nonce 未确认")
    for key in ("hwnd", "className", "threadId", "style", "exStyle", "visible"):
        require(type(evidence.get(key)) is type(observed.get(key)) and evidence.get(key) == observed.get(key), "原生与外部窗口见证不一致: " + key)
    require(type(evidence.get("bridgePid")) is int and evidence["bridgePid"] == isolation["bridgePid"], "原生与原 bridge PID 不一致")


def run(args):
    require(sys.platform == "win32", "仅 Windows 运行")
    adapter = plain(args.adapter)
    native = locked_adapter(adapter, args.adapter_sha256)
    helpers = native.load_helpers(plain(args.desktop_helper), args.desktop_helper_sha256)
    preparation_path = plain(args.preparation)
    preparation = read_json(preparation_path)
    require(preparation.get("schema") == "audastra-lua-api-probe-preparation-v1"
            and preparation.get("authorScriptsIncluded") is False, "只接受原创准备回执")
    nonce = preparation["nonce"]
    require(re.fullmatch("[0-9a-f]{32}", nonce) is not None, "nonce 无效")
    sandbox = plain(args.sandbox_root)
    require(sandbox.is_dir() and sandbox.name == "War3_AutoTestSandbox", "必须显式选择已有 War3_AutoTestSandbox")
    runtime = plain(args.ydwe_root)
    require(runtime != sandbox and runtime not in sandbox.parents and sandbox not in runtime.parents, "运行库与沙盒必须分离")
    executable, bootstrap = plain(args.launcher), plain(args.bootstrap)
    require(sha(executable) == args.launcher_sha256 and sha(bootstrap) == args.bootstrap_sha256, "自有 bridge/bootstrap SHA 不符")
    require(plain(preparation["mapPath"]).stat().st_size < 2 * 1024 * 1024, "只允许原创微型 probe 地图")
    require(sha(preparation["mapPath"]) == preparation["mapSha256"] and sha(preparation["luaPath"]) == preparation["luaSha256"], "原创 map/Lua 来源变化")
    before = {relative: sha(plain(runtime / relative)) for relative in MODULES}
    require(before["plugin/warcraft3/yd_lua_engine.dll"] == preparation["runtimeEngineSha256"], "候选源与当前运行库不一致")
    # 参数拒绝在任何本次目录/副本写入之前完成。
    nonfatal_paths = []
    require(len(args.nonfatal_blocked_path) <= 4, "非致命拒写精确路径最多4条")
    for value in args.nonfatal_blocked_path:
        nonfatal = plain(value, exists=False)
        require(runtime != nonfatal and runtime not in nonfatal.parents
                and sandbox != nonfatal and sandbox not in nonfatal.parents, "非致命拒写不能指向用户运行库或沙盒")
        canonical = str(nonfatal).casefold()
        require(canonical not in nonfatal_paths, "重复非致命拒写路径")
        nonfatal_paths.append(canonical)
    evidence = plain(args.output_root, exists=False)
    require(not evidence.exists(), "回执目录必须不存在")
    require(runtime != evidence and runtime not in evidence.parents, "回执不能写入用户安装")
    evidence.mkdir()
    map_parent = sandbox / "Maps" / "ARProbe" / nonce[:8]
    require(plain(map_parent, exists=False).parent == sandbox / "Maps" / "ARProbe", "映射副本目录越界")
    map_parent.parent.mkdir(exist_ok=True)
    require(not map_parent.exists(), "nonce 沙盒副本目录已存在")
    map_parent.mkdir()
    candidate = map_parent / "p.w3x"
    with plain(preparation["mapPath"]).open("rb") as source, candidate.open("xb") as target:
        shutil.copyfileobj(source, target, 1024 * 1024)
    require(sha(candidate) == preparation["mapSha256"], "沙盒副本 SHA 不符")
    relative = str(PureWindowsPath("Maps", "ARProbe", nonce[:8], "p.w3x"))
    require(len(relative) < 54, "经典 Maps 相对路径超预算")
    receipt = evidence / "launcher.local.json"
    name = "AudastraRuntimeProbe-" + nonce
    argv = [str(executable), "--game", str(sandbox / "war3.exe"), "--map", relative,
            "--runtime", str(runtime), "--runtime-layout", "ydwe-tree", "--bootstrap", str(bootstrap),
            "--receipt", str(receipt), "--lua-api-probe", nonce, "--probe-map-sha256", preparation["mapSha256"],
            "--probe-script", str(plain(preparation["luaPath"])), "--probe-script-sha256", preparation["luaSha256"],
            "--desktop", "WinSta0\\" + name, "--probe-timeout-ms", str(args.probe_timeout_ms)]
    for canonical in nonfatal_paths:
        argv.extend(("--probe-nonfatal-blocked-path", canonical))
    if args.probe_desktop_witness:
        argv.extend(("--probe-desktop-witness", args.probe_desktop_witness))
    if args.probe_window_diagnostics:
        argv.extend(("--probe-window-diagnostics", args.probe_window_diagnostics))
    if args.probe_renderer:
        argv.extend(("--probe-renderer", args.probe_renderer))
    result = {"schema": "audastra-isolated-lua-api-probe-v1", "nonce": nonce, "argv": argv,
              "preparationSha256": sha(preparation_path), "adapterSha256": args.adapter_sha256,
              "desktopHelperSha256": args.desktop_helper_sha256, "runtimeSha256Before": before,
              "sandboxCopy": str(candidate), "sandboxCopySha256": preparation["mapSha256"], "errors": []}
    result["nonfatalBlockedPaths"] = nonfatal_paths
    result["desktopWitnessMode"] = args.probe_desktop_witness or "none"
    result["windowDiagnosticsMode"] = args.probe_window_diagnostics or "none"
    result["probeGraphicsBackendRequested"] = args.probe_renderer or "game-default"
    result["gameplayVerified"] = False
    win = native.Windows()
    old_error = win.k.SetErrorMode(0x8007)
    desktop, job, assigned = None, None, False
    process = native.PROCESSINFO()
    input_before = helpers._query_input_desktop_name()
    result["inputDesktopBefore"] = input_before
    started = time.monotonic()
    try:
        creation = helpers._create_isolated_desktop(name)
        desktop = creation["handle"]
        require(helpers._desktop_name_from_handle(desktop) == name, "实际独立 Desktop 名称不符")
        result["desktop"] = {"name": name, "handle": desktop}
        job = win.check(win.k.CreateJobObjectW(None, None), "创建本次 Job")
        limits = native.JOBEXT(); limits.basic.flags = 0x2000
        win.check(win.k.SetInformationJobObject(job, 9, C.byref(limits), C.sizeof(limits)), "设置本次 Job kill-on-close")
        environment = os.environ.copy()
        environment = {key: value for key, value in environment.items()
                       if not key.upper().startswith(("AUDASTRA_TEST_", "LUA_INIT", "LUA_PATH", "LUA_CPATH"))
                       and key.casefold() != "ydwe-process-name"}
        result["luaEnvironmentOverridesAbsent"] = not any(key.upper().startswith(("LUA_INIT", "LUA_PATH", "LUA_CPATH")) for key in environment)
        environment["TEMP"], environment["TMP"] = str(map_parent), str(map_parent)
        block = helpers._build_environment_block(environment)
        startup = native.STARTUPINFO()
        startup.cb, startup.desktop, startup.flags, startup.show = C.sizeof(startup), "WinSta0\\" + name, 1, 0
        command = C.create_unicode_buffer(subprocess.list2cmdline(argv))
        policy = {"requested": True}
        # 桌面 app 的子进程策略仅作用于这个新建 Job；没有 breakaway 或机器设置。
        with native.desktop_app_policy(win, startup, True, policy) as (start, extra):
            win.check(win.k.CreateProcessW(str(executable), command, None, None, False,
                      0x4 | 0x400 | 0x08000000 | 0x4000 | extra, block, str(sandbox), start, C.byref(process)), "创建隔离自有 bridge")
        result["desktopAppPolicy"] = policy
        win.check(win.k.AssignProcessToJobObject(job, process.process), "纳入本次 Job")
        assigned = True
        result["bridgePid"] = process.pid
        result["bridgeThreadId"] = process.tid
        times = [C.c_uint64() for _ in range(4)]
        win.check(win.k.GetProcessTimes(process.process, *[C.byref(value) for value in times]), "读取原 bridge 创建时间")
        creation_time = times[0].value
        require(creation_time > 0, "原 bridge 创建时间缺失")
        callback_type = bind_witness_apis(win) if args.probe_desktop_witness else None
        require(helpers._query_input_desktop_name() == input_before, "恢复前 input Desktop 变化")
        require(win.k.ResumeThread(process.thread) != 0xFFFFFFFF, "恢复自有 bridge 失败")
        # 此环境实测挂起/短命 bridge 的外部查询为 NULL；具体原因尚未确定。
        # Microsoft 文档说明 thread 创建时即关联 Desktop；不得把 NULL 当成功证明。
        # 先恢复已经归入本次 Job 的 bridge；它自己在创建游戏前严格验证当前 Desktop/Job。
        # 外部观察仍须得到实际借用 Desktop，不能用启动参数或 Default 代替。
        desktop_deadline = time.monotonic() + 5
        result["bridgeDesktopQueryZeroResults"] = 0
        result["bridgeDesktopCheckedAfterResume"] = True
        if args.probe_desktop_witness:
            result["bridgeDesktopEnumerationPending"] = {"zeroNoCallbackCount": 0}
        while True:
            C.set_last_error(0)
            actual = win.u.GetThreadDesktop(process.tid)
            query_error = C.get_last_error()
            query = {"threadId": int(process.tid), "returnedNull": not bool(actual), "lastError": query_error}
            if actual:
                actual_name = helpers._desktop_name_from_handle(actual)
                require(actual_name == name, "bridge 实际 Desktop 不符")
                query["name"] = actual_name
            else:
                result["bridgeDesktopQueryZeroResults"] += 1
                result["bridgeDesktopLastQueryError"] = query_error
            result["bridgeThreadDesktopQuery"] = query
            if args.probe_desktop_witness:
                witnessed = observe_hidden_witness(win, callback_type, desktop, job, process, name, nonce, creation_time,
                                                    result["bridgeDesktopEnumerationPending"])
                if witnessed:
                    result["bridgeDesktop"] = witnessed
                    break
            elif actual:
                result["bridgeDesktop"] = {"proofKind": "thread-desktop", "threadId": process.tid, "handle": int(actual),
                                           "name": actual_name, "borrowedHandle": True}
                break
            require(win.k.WaitForSingleObject(process.process, 0) == 258, "bridge 在实际 Desktop 证明形成前退出；不回退到 Default")
            require(time.monotonic() < desktop_deadline, "bridge 实际 Desktop 证明超时；不回退到 Default")
            time.sleep(0.02)
        require(helpers._query_input_desktop_name() == input_before, "恢复后 input Desktop 变化")
        deadline = time.monotonic() + args.probe_timeout_ms / 1000 + 25
        while win.k.WaitForSingleObject(process.process, 100) == 258:
            require(time.monotonic() < deadline, "自有 bridge 外层期限超时")
        code = native.U32()
        win.check(win.k.GetExitCodeProcess(process.process, C.byref(code)), "读取 bridge exit")
        result["bridgeExitCode"] = code.value
        require(receipt.exists(), "缺 launcher 原始回执")
        result["launcherReceiptSha256"] = sha(receipt)
        raw = read_json(receipt)
        require(raw.get("luaApiProbe", {}).get("nonce") == nonce, "回执 nonce 绑定失败")
        verify_witness_pair(raw, result)
        require(code.value == 0 and raw.get("luaApiProbe", {}).get("captured") is True, "实际运行未形成 Lua API 观察；保留具体 errorStage")
    except Exception as error:
        result["errors"].append(str(error))
    finally:
        # 一项清理失败仍继续关闭其余本次句柄；不能因查询异常跳过 kill-on-close。
        def cleanup(label, operation):
            try:
                return operation()
            except Exception as error:
                result["errors"].append(label + ": " + str(error))
                return None

        def drain_owned_job():
            signaled = bool(process.process and win.k.WaitForSingleObject(process.process, 0) == 0)
            if process.process and not signaled:
                if assigned:
                    win.check(win.k.TerminateJobObject(job, 0xE0501001), "仅结束本次 Job")
                else:
                    win.check(win.k.TerminateProcess(process.process, 0xE0501001), "仅结束本次挂起 bridge")
                win.k.WaitForSingleObject(process.process, 5000)
            if not job:
                return
            state = win.job_state(job, process.pid, bool(process.process and win.k.WaitForSingleObject(process.process, 0) == 0))
            # bridge 已退出而自己的 game 仍存活，也只能结束这个独立 Job。
            if state["unresolvedAssociatedPids"]:
                win.check(win.k.TerminateJobObject(job, 0xE0501001), "结束本次残留 Job 子进程")
            end = time.monotonic() + 2
            while state["unresolvedAssociatedPids"] and time.monotonic() < end:
                time.sleep(0.05)
                state = win.job_state(job, process.pid, bool(process.process and win.k.WaitForSingleObject(process.process, 0) == 0))
            result["jobBeforeClose"] = state
            require(state["enumerationComplete"] and not state["unresolvedAssociatedPids"], "本次 Job 未全部退出；close 只作 containment")

        cleanup("owned Job drain", drain_owned_job)
        if job:
            result["jobClosed"] = cleanup("Job handle close", lambda: bool(win.check(win.k.CloseHandle(job), "CloseHandle(Job)")))
        for key, handle in (("threadClosed", process.thread), ("bridgeHandleClosed", process.process)):
            if handle:
                result[key] = cleanup(key, lambda handle=handle: bool(win.check(win.k.CloseHandle(handle), "CloseHandle")))
        if desktop:
            result["desktopClosed"] = cleanup("Desktop handle close", lambda: helpers._close_desktop_handle(desktop))
            if result["desktopClosed"] is not True:
                result["errors"].append("本次 Desktop 句柄未确认关闭")
        result["inputDesktopAfter"] = cleanup("input Desktop query", helpers._query_input_desktop_name)
        if result["inputDesktopAfter"] != input_before:
            result["errors"].append("运行期间 input Desktop 变化；未切换或尝试切回")
        result["runtimeSha256After"] = cleanup("readonly runtime identity", lambda: {relative: sha(plain(runtime / relative)) for relative in MODULES})
        if result["runtimeSha256After"] != before:
            result["errors"].append("本机只读运行库身份变化")
        result["seconds"] = time.monotonic() - started
        # 仅在 Job 已无活进程时逐项删本轮已知地图/日志；保留其他失败现场，不递归删除。
        def remove_owned_artifacts():
            owned = {candidate: preparation["mapSha256"]}
            log = map_parent / "audastra-runtime-probe-war3.log"
            if log.exists():
                result["sandboxLog"] = {"path": str(log), "sha256": sha(log), "bytes": log.stat().st_size}
                owned[log] = result["sandboxLog"]["sha256"]
                shutil.copyfile(log, evidence / "sandbox-runtime-war3.log")
            for file, expected in owned.items():
                require(plain(file).parent == map_parent and sha(file) == expected, "清理文件归属/SHA变化")
                file.unlink()
            if not any(map_parent.iterdir()):
                map_parent.rmdir()
                if not any(map_parent.parent.iterdir()):
                    map_parent.parent.rmdir()
            else:
                result["retainedSandboxFiles"] = [file.name for file in map_parent.iterdir()]
        if result.get("jobBeforeClose", {}).get("enumerationComplete") is True and result["jobBeforeClose"].get("unresolvedAssociatedPids") == []:
            cleanup("owned sandbox artifacts", remove_owned_artifacts)
        result["succeeded"] = not result["errors"]
        cleanup("restore own error mode", lambda: win.k.SetErrorMode(old_error))
        result["succeeded"] = not result["errors"]
        write_new(evidence / "isolation.local.json", result)
    print(json.dumps({"succeeded": result["succeeded"], "receipt": str(evidence / "isolation.local.json"), "errors": result["errors"]}, ensure_ascii=False))
    return 0 if result["succeeded"] else 1


class SingleProbeOption(argparse.Action):
    """只约束显式 probe 选项；重复参数在 run() 和任何文件写入前拒绝。"""

    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(str(option_string) + " 不能重复")
        setattr(namespace, self.dest, values)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    for name in ("adapter", "desktop-helper", "preparation", "sandbox-root", "ydwe-root", "launcher", "bootstrap", "output-root"):
        result.add_argument("--" + name, required=True, type=Path)
    for name in ("adapter-sha256", "desktop-helper-sha256", "launcher-sha256", "bootstrap-sha256"):
        result.add_argument("--" + name, required=True)
    result.add_argument("--probe-timeout-ms", type=int, default=45000)
    result.add_argument("--nonfatal-blocked-path", type=Path, action="append", default=[],
                        help="仅此精确路径的写请求仍拒绝，但允许观察继续；不授予写权限")
    result.add_argument("--probe-desktop-witness", choices=("hidden-window",),
                        help="仅原创metadata probe显式创建隐藏顶层见证；默认仍要求直接Desktop查询")
    result.add_argument("--probe-window-diagnostics", choices=("bounded",), action=SingleProbeOption,
                        help="仅自有Game窗口两次有界只读诊断；不构成Desktop、进图或Lua捕获证明")
    result.add_argument("--probe-renderer", choices=("opengl",), action=SingleProbeOption,
                        help="仅metadata probe显式请求-opengl；实际后端保持未知，不构成D3D9进图验收")
    return result


if __name__ == "__main__":
    arguments = parser().parse_args()
    require(1000 <= arguments.probe_timeout_ms <= 120000, "探针期限必须1–120秒")
    raise SystemExit(run(arguments))
