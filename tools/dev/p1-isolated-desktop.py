"""P1 独立 Desktop 薄宿主；只运行显式绑定的 Python 子命令，不证明原版保存。"""
from __future__ import annotations

import argparse
import ast
import contextlib
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime
import hashlib
import io
import json
import math
import os
from pathlib import Path
import runpy
import stat
import struct
import subprocess
import sys
import threading
import time
from types import SimpleNamespace
import uuid


U32, PTR, SIZE = C.c_uint32, C.c_void_p, C.c_size_t
KILL_CODE = 0xE0501001
HELPERS = frozenset(("_native_handle_value", "_now_compact", "_desktop_access_mask",
    "_desktop_name_from_handle", "_query_input_desktop_name", "_create_isolated_desktop",
    "_close_desktop_handle", "_build_environment_block"))
DESKTOP_CONSTANTS = {"DESKTOP_READOBJECTS": 1, "DESKTOP_CREATEWINDOW": 2,
                     "DESKTOP_ENUMERATE": 0x40, "DESKTOP_WRITEOBJECTS": 0x80}
ENV_REQUIRED = {"systemroot", "windir", "path", "temp", "tmp"}
ENV_OPTIONAL = {"comspec", "pathext", "systemdrive", "userprofile", "localappdata",
                "appdata", "programdata", "pythonutf8", "pythondontwritebytecode"}
REQUEST_FIELDS = {"schema", "argv", "entrySha256", "adapterSha256", "helper", "helperSha256",
                  "cwd", "environmentSha256", "environmentKeys", "desktop", "witness", "physicalArgv",
                  "unpackagedDescendants"}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha_bytes(value):
    return hashlib.sha256(value).hexdigest()


def environment_sha(env):
    return sha_bytes(json.dumps({k.casefold(): v for k, v in env.items()}, sort_keys=True).encode())


def absolute_path(value, *, exists=True):
    path = Path(value)
    require(path.is_absolute() and "\x00" not in str(value), "路径必须是无 NUL 的绝对路径")
    require(not str(path).startswith(("\\\\", "//")) and ":" not in str(path)[2:], "拒绝 UNC、设备路径和 ADS")
    for part in [path, *path.parents]:
        if part.exists():
            require(not (part.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT), "拒绝 reparse 路径")
    if exists:
        require(path.exists(), "路径不存在: " + str(path))
    return path.resolve(strict=exists)


def load_helpers(path, expected_sha):
    """只复用锁定源码的函数定义，绝不 import MCP 宿主或执行其顶层初始化。"""
    source = path.read_bytes()
    require(sha_bytes(source) == expected_sha.lower(), "Desktop helper SHA 不符")
    tree = ast.parse(source.decode("utf-8-sig"), filename=str(path))
    selected = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in HELPERS]
    require(len(selected) == len(HELPERS) and {n.name for n in selected} == HELPERS, "helper 定义缺失或重名")
    require(all(not node.decorator_list for node in selected), "helper 不允许装饰器副作用")
    parameters = {"_native_handle_value": ["value"], "_now_compact": [], "_desktop_access_mask": [],
        "_desktop_name_from_handle": ["handle"], "_query_input_desktop_name": [],
        "_create_isolated_desktop": ["name"], "_close_desktop_handle": ["handle"],
        "_build_environment_block": ["env"]}
    for node in selected:
        args = node.args
        require([a.arg for a in args.args] == parameters[node.name] and not any((args.posonlyargs,
            args.kwonlyargs, args.vararg, args.kwarg, args.defaults, args.kw_defaults)),
            "helper 参数契约变更: " + node.name)
    # 这些值与锁定 helper 的常量逐一核对，而不是执行其它模块级表达式。
    for name, value in DESKTOP_CONSTANTS.items():
        nodes = [n for n in tree.body if isinstance(n, ast.Assign) and
                 any(isinstance(t, ast.Name) and t.id == name for t in n.targets)]
        require(len(nodes) == 1 and isinstance(nodes[0].value, ast.Constant) and
                nodes[0].value.value == value, "helper Desktop 常量变更")
    module = ast.Module(body=[ast.ImportFrom(module="__future__",
        names=[ast.alias(name="annotations")], level=0), *selected], type_ignores=[])
    scope = {"ctypes": C, "wintypes": W, "threading": threading, "datetime": datetime,
             **DESKTOP_CONSTANTS}
    exec(compile(ast.fix_missing_locations(module), str(path), "exec"), scope)
    return SimpleNamespace(**{name: scope[name] for name in HELPERS})


class STARTUPINFO(C.Structure):
    _fields_ = [("cb", U32), ("reserved", C.c_wchar_p), ("desktop", C.c_wchar_p),
        ("title", C.c_wchar_p), ("x", U32), ("y", U32), ("xSize", U32), ("ySize", U32),
        ("xChars", U32), ("yChars", U32), ("fill", U32), ("flags", U32),
        ("show", C.c_uint16), ("reservedSize", C.c_uint16), ("reservedBytes", PTR),
        ("stdin", PTR), ("stdout", PTR), ("stderr", PTR)]


class PROCESSINFO(C.Structure):
    _fields_ = [("process", PTR), ("thread", PTR), ("pid", U32), ("tid", U32)]


class STARTUPINFOEX(C.Structure):
    _fields_ = [("startup", STARTUPINFO), ("attributes", PTR)]


@contextlib.contextmanager
def desktop_app_policy(win, startup, enabled, receipt):
    """只作用于新建collector的后代；不设置Job breakaway，不修改宿主包或机器策略。"""
    if not enabled:
        yield C.byref(startup), 0
        return
    require(C.sizeof(STARTUPINFOEX) == 112, "STARTUPINFOEX SDK布局不符")
    for name, types, result in (
        ("InitializeProcThreadAttributeList", [PTR, U32, U32, C.POINTER(SIZE)], C.c_int),
        ("UpdateProcThreadAttribute", [PTR, U32, SIZE, PTR, SIZE, PTR, PTR], C.c_int),
        ("DeleteProcThreadAttributeList", [PTR], None)):
        api = getattr(win.k, name)
        api.argtypes, api.restype = types, result
    size = SIZE()
    C.set_last_error(0)
    sized = win.k.InitializeProcThreadAttributeList(None, 1, 0, C.byref(size))
    require(not sized and C.get_last_error() == 122 and 0 < size.value <= 1024 * 1024,
            "desktop-app attribute容量查询失败")
    storage = C.create_string_buffer(size.value)
    win.check(win.k.InitializeProcThreadAttributeList(storage, 1, 0, C.byref(size)), "初始化desktop-app attribute")
    receipt["attributesInitialized"] = True
    try:
        # SDK: ProcThreadAttributeDesktopAppPolicy=18 | INPUT=0x20000；只允许ENABLE_PROCESS_TREE=1。
        policy = U32(1)
        win.check(win.k.UpdateProcThreadAttribute(storage, 0, 0x20012, C.byref(policy), C.sizeof(policy), None, None),
                  "设置desktop-app后代策略")
        receipt["attributeConfigured"] = True
        extended = STARTUPINFOEX()
        extended.startup = startup
        extended.startup.cb = C.sizeof(extended)
        extended.attributes = C.cast(storage, PTR)
        # extended/storage/policy均保持存活，直到CreateProcess返回，再删除attribute list。
        yield C.cast(C.byref(extended), C.POINTER(STARTUPINFO)), 0x00080000
    finally:
        win.k.DeleteProcThreadAttributeList(storage)
        receipt["attributesDeleted"] = True


class JOBBASIC(C.Structure):
    _fields_ = [("processTime", C.c_int64), ("jobTime", C.c_int64), ("flags", U32),
        ("minSet", SIZE), ("maxSet", SIZE), ("activeLimit", U32), ("affinity", SIZE),
        ("priority", U32), ("scheduling", U32)]


class JOBEXT(C.Structure):
    _fields_ = [("basic", JOBBASIC), ("io", C.c_uint64 * 6), ("processMemory", SIZE),
        ("jobMemory", SIZE), ("peakProcessMemory", SIZE), ("peakJobMemory", SIZE)]


class Windows:
    def __init__(self):
        require(sys.platform == "win32" and C.sizeof(PTR) == 8, "只支持 Windows AMD64 Python")
        require(C.sizeof(STARTUPINFO) == 104 and C.sizeof(JOBEXT) == 144, "Win32 结构布局不符")
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.u = C.WinDLL("user32", use_last_error=True)
        signatures = {
            "CreateProcessW": ([C.c_wchar_p, C.c_wchar_p, PTR, PTR, C.c_int, U32,
                PTR, C.c_wchar_p, C.POINTER(STARTUPINFO), C.POINTER(PROCESSINFO)], C.c_int),
            "CreateJobObjectW": ([PTR, C.c_wchar_p], PTR),
            "SetInformationJobObject": ([PTR, C.c_int, PTR, U32], C.c_int),
            "AssignProcessToJobObject": ([PTR, PTR], C.c_int),
            "QueryInformationJobObject": ([PTR, C.c_int, PTR, U32, C.POINTER(U32)], C.c_int),
            "TerminateJobObject": ([PTR, U32], C.c_int), "TerminateProcess": ([PTR, U32], C.c_int),
            "WaitForSingleObject": ([PTR, U32], U32), "GetExitCodeProcess": ([PTR, C.POINTER(U32)], C.c_int),
            "CloseHandle": ([PTR], C.c_int), "ResumeThread": ([PTR], U32),
            "GetProcessId": ([PTR], U32), "GetCurrentThreadId": ([], U32),
            "QueryFullProcessImageNameW": ([PTR, U32, C.c_wchar_p, C.POINTER(U32)], C.c_int),
            "GetProcessTimes": ([PTR, PTR, PTR, PTR, PTR], C.c_int),
            "IsWow64Process2": ([PTR, C.POINTER(C.c_uint16), C.POINTER(C.c_uint16)], C.c_int),
            "SetErrorMode": ([U32], U32),
            "OpenProcess": ([U32, C.c_int, U32], PTR),
            "IsProcessInJob": ([PTR, PTR, C.POINTER(C.c_int)], C.c_int),
        }
        for name, (args, result) in signatures.items():
            api = getattr(self.k, name)
            api.argtypes, api.restype = args, result
        self.u.GetThreadDesktop.argtypes, self.u.GetThreadDesktop.restype = [U32], PTR
        self.u.GetProcessWindowStation.argtypes, self.u.GetProcessWindowStation.restype = [], PTR
        self.u.OpenDesktopW.argtypes, self.u.OpenDesktopW.restype = [C.c_wchar_p, U32, C.c_int, U32], PTR
        self.u.CloseDesktop.argtypes, self.u.CloseDesktop.restype = [PTR], C.c_int

    @staticmethod
    def check(ok, label):
        if not ok:
            raise OSError(C.get_last_error(), label)
        return ok

    def thread_desktop(self, helpers, tid):
        handle = self.check(self.u.GetThreadDesktop(tid), "GetThreadDesktop")
        # GetThreadDesktop 返回借用句柄，不由调用者 CloseDesktop。
        name = helpers._desktop_name_from_handle(handle)
        require(name, "无法读取实际 thread Desktop 名称")
        return {"threadId": tid, "handle": int(handle), "name": name, "borrowedHandle": True}

    def identity(self, pi, image):
        require(self.k.GetProcessId(pi.process) == pi.pid, "原 hProcess PID 不符")
        path, length = C.create_unicode_buffer(32768), U32(32768)
        self.check(self.k.QueryFullProcessImageNameW(pi.process, 0, path, C.byref(length)), "实际 Python 路径")
        require(os.path.normcase(path.value) == os.path.normcase(str(image)), "实际 Python image 不符")
        times = [C.c_uint64() for _ in range(4)]
        self.check(self.k.GetProcessTimes(pi.process, *[C.byref(v) for v in times]), "创建时间")
        machine, native = C.c_uint16(), C.c_uint16()
        self.check(self.k.IsWow64Process2(pi.process, C.byref(machine), C.byref(native)), "Python 架构")
        require(machine.value == 0 and native.value == 0x8664, "collector 必须使用 AMD64 Python")
        return {"pid": int(pi.pid), "primaryThreadId": int(pi.tid), "creationFileTime": times[0].value,
                "image": path.value, "machine": "AMD64", "retainedOriginalHandle": True}

    def job_state(self, job, root_pid, root_signaled):
        data, needed = C.create_string_buffer(32768), U32()
        self.check(self.k.QueryInformationJobObject(job, 3, data, len(data), C.byref(needed)), "OWN Job PID 枚举")
        assigned, listed = struct.unpack_from("<II", data.raw)
        complete = assigned == listed and listed <= (len(data) - 8) // 8 and needed.value >= 8 + listed * 8
        bounded = min(listed, (len(data) - 8) // 8)
        pids = list(struct.unpack_from("<" + "Q" * bounded, data.raw, 8)) if bounded else []
        unresolved = [pid for pid in pids if pid != root_pid or not root_signaled]
        return {"assigned": assigned, "listed": listed, "associatedPids": pids,
                "enumerationComplete": complete, "unresolvedAssociatedPids": unresolved,
                "rootSignaledExcluded": root_signaled, "descendantExitIndividuallyProven": False}

    def associated_identity(self, job, pid):
        """只诊断本 Job 关联项；PID 重用时不接受身份，更不通过该 PID 终止。"""
        row, handle = {"pid": pid}, self.k.OpenProcess(0x1000 | 0x100000, False, pid)
        if not handle:
            row["identity"] = "unavailable"
            return row
        try:
            member = C.c_int()
            self.check(self.k.IsProcessInJob(handle, job, C.byref(member)), "关联项 Job membership")
            require(member.value, "关联 PID 已不属于 OWN Job，不接受回收利用的 PID")
            buf, length = C.create_unicode_buffer(32768), U32(32768)
            self.check(self.k.QueryFullProcessImageNameW(handle, 0, buf, C.byref(length)), "关联项实际 image")
            times = [C.c_uint64() for _ in range(4)]
            self.check(self.k.GetProcessTimes(handle, *[C.byref(v) for v in times]), "关联项创建时间")
            row.update(image=buf.value, creationFileTime=times[0].value,
                       signaledAtQuery=self.k.WaitForSingleObject(handle, 0) == 0, ownJobMember=True)
        except Exception as error:
            row["error"] = str(error)
        finally:
            row["queryHandleClosed"] = bool(self.k.CloseHandle(handle))
        return row


def environment(value):
    env = json.loads(value)
    require(isinstance(env, dict), "env-json 必须是对象")
    require(all(isinstance(k, str) and isinstance(v, str) and k and "=" not in k and
                "\x00" not in k + v for k, v in env.items()), "环境键值格式非法")
    keys = {key.casefold() for key in env}
    require(len(keys) == len(env), "环境键不得大小写重名")
    require(ENV_REQUIRED <= keys <= ENV_REQUIRED | ENV_OPTIONAL, "只接受显式必要环境键白名单")
    for key, value in env.items():
        if key.casefold() in {"pythonutf8", "pythondontwritebytecode"}:
            require(value == "1", "Python 开关只允许1")
        if key.casefold() in {"systemroot", "windir", "temp", "tmp"}:
            require(absolute_path(value).is_dir(), "环境目录不存在")
    require(sum(len(k) + len(v) + 2 for k, v in env.items()) < 32767, "环境块超限")
    return env


def collector_timeout(source, arguments):
    """只接受无歧义 timeout 拼写，实际值和默认值交冻结 collector 原 parser。"""
    option = "--timeout-seconds"
    timeout_tokens = [token for token in arguments if token.startswith("--") and
        len(token.split("=", 1)[0]) > 2 and option.startswith(token.split("=", 1)[0])]
    require(all(token == option for token in timeout_tokens),
            "collector timeout 仅允许规范分离式 --timeout-seconds <value>，拒绝等号和缩写")
    require(len(timeout_tokens) <= 1, "collector timeout 不能重复指定")
    # 只执行已核 SHA 源码中的无参 parser 定义，不 import collector/native-loader 顶层。
    tree = ast.parse(source.decode("utf-8-sig"))
    definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "parser"]
    require(len(definitions) == 1, "collector parser 定义缺失或重名")
    node = definitions[0]
    require(not node.decorator_list and not any((node.args.posonlyargs, node.args.args,
        node.args.kwonlyargs, node.args.vararg, node.args.kwarg, node.args.defaults,
        node.args.kw_defaults)), "collector parser 无参契约变更")
    scope = {"argparse": argparse, "__doc__": "SHA-bound collector parser"}
    exec(compile(ast.Module(body=[node], type_ignores=[]), "<SHA-bound collector parser>", "exec"), scope)
    # parser 错误不打印可能含输入数据的整条 argv；拒绝发生在新建 Desktop/进程之前。
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            parsed = scope["parser"]().parse_args(arguments)
        except SystemExit as error:
            raise ValueError("collector 参数未通过冻结 parser") from error
    return parsed.timeout_seconds


def preflight(args):
    require(type(getattr(args, "unpackaged_descendants", False)) is bool, "unpackaged-descendants必须显式布尔值")
    require(math.isfinite(args.timeout_seconds) and 0.1 <= args.timeout_seconds <= 600, "超时必须在0.1至600秒")
    argv = json.loads(args.argv_json)
    require(isinstance(argv, list) and len(argv) >= 3 and all(isinstance(v, str) and "\x00" not in v for v in argv), "argv 必须是字符串数组")
    require(argv[1] == "-B", "只允许显式 Python -B script 入口")
    image, entry = absolute_path(argv[0]), absolute_path(argv[2])
    require(image.is_file() and entry.is_file() and entry.suffix.lower() == ".py", "Python/script 路径无效")
    entry_source = entry.read_bytes()
    require(sha_bytes(entry_source) == args.entry_sha256.lower(), "入口脚本 SHA 不符")
    command = subprocess.list2cmdline(argv)
    require(len(command) < 32767, "命令行超限")
    if entry.name == "p1-loader-trace.py":
        # collector 的内部期限后还需要 Continue EXIT、Job 回读及落盘裕量。
        trace_timeout = collector_timeout(entry_source, argv[3:])
        require(math.isfinite(trace_timeout) and args.timeout_seconds >= trace_timeout + 15,
                "Desktop 外层期限须至少比 collector timeout 多15秒")
    cwd, helper = absolute_path(args.cwd), absolute_path(args.helper)
    require(cwd.is_dir() and helper.is_file(), "cwd/helper 类型不符")
    env = environment(args.env_json)
    evidence = absolute_path(args.evidence, exists=False)
    require(not evidence.exists() and evidence.parent.is_dir(), "回执目录必须全新且父目录存在")
    return argv, image, entry, command, cwd, helper, env, evidence


def worker(request_path, request_sha):
    """与 collector 同一 PID/主线程；只在实际 Desktop witness 后进入冻结脚本。"""
    path = absolute_path(request_path)
    require(path.is_file() and path.stat().st_nlink == 1 and path.stat().st_size < 131072,
            "worker 请求必须是新的有界普通文件")
    raw = path.read_bytes()
    require(sha_bytes(raw) == request_sha, "worker 请求 SHA 不符")
    request = json.loads(raw)
    require(isinstance(request, dict) and set(request) == REQUEST_FIELDS and
            request["schema"] == "p1-desktop-worker-v1", "worker schema 不符")
    require(type(request["unpackagedDescendants"]) is bool, "worker后代策略必须为布尔值")
    require(sha_bytes(Path(__file__).read_bytes()) == request["adapterSha256"], "worker 自身 SHA 不符")
    argv = request["argv"]
    require(isinstance(argv, list) and len(argv) >= 3 and argv[1] == "-B" and
        all(isinstance(v, str) and "\x00" not in v for v in argv), "worker logical argv 非法")
    require(os.path.normcase(argv[0]) == os.path.normcase(sys.executable), "worker Python image 参数不符")
    entry = absolute_path(argv[2])
    require(entry.is_file() and sha_bytes(entry.read_bytes()) == request["entrySha256"], "worker entry SHA 不符")
    win = Windows()
    helpers = load_helpers(absolute_path(request["helper"]), request["helperSha256"])
    actual = win.thread_desktop(helpers, win.k.GetCurrentThreadId())
    require(actual["name"] == request["desktop"], "worker 实际 Desktop 不符，拒绝进入 collector")
    require(os.path.normcase(os.getcwd()) == os.path.normcase(request["cwd"]), "worker 实际 cwd 不符")
    keys = request["environmentKeys"]
    require(isinstance(keys, list) and len(keys) == len(set(k.casefold() for k in keys)) and
        ENV_REQUIRED <= {k.casefold() for k in keys} <= ENV_REQUIRED | ENV_OPTIONAL, "worker 环境键白名单不符")
    selected = {key: os.environ[key] for key in keys}
    require(environment_sha(selected) == request["environmentSha256"], "worker 选定实际环境不符")
    physical = request["physicalArgv"]
    require(physical == [sys.executable, "-B", str(Path(__file__).resolve()), "--worker-request", str(path)],
            "worker physical argv 绑定不符")
    # request SHA 不能递归包含自身；最后的哈希参数单独加在已绑定 argv 之后。
    expected_physical = [*physical, "--request-sha256", request_sha]
    require(sys.argv == expected_physical[2:], "worker 实际 Python argv 不符")
    win.k.GetCommandLineW.argtypes, win.k.GetCommandLineW.restype = [], C.c_wchar_p
    actual_command = win.k.GetCommandLineW()
    require(actual_command == subprocess.list2cmdline(expected_physical), "worker Win32 物理命令行不符")
    witness = absolute_path(request["witness"], exists=False)
    require(witness.parent == path.parent and not witness.exists(), "worker witness 必须是同目录新文件")
    data = {"schema": "p1-desktop-worker-witness-v1", "pid": os.getpid(), "desktop": actual,
        "cwd": os.getcwd(), "logicalArgv": argv, "physicalCommandline": actual_command,
        "requestSha256": request_sha, "entrySha256": request["entrySha256"],
        "adapterSha256": request["adapterSha256"], "environmentKeys": keys,
        "unpackagedDescendantsRequested": request["unpackagedDescendants"],
        "environmentSha256": environment_sha(selected), "sameProcessMainThread": True,
        "witnessBeforeEntry": True, "observedUnixNs": time.time_ns()}
    with witness.open("x", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=True, indent=2)
    sys.argv = argv[2:]
    sys.path[0] = str(entry.parent)
    # SystemExit 原样传播；不另启第三个 Python，不替换 collector 的 Windows API。
    runpy.run_path(str(entry), run_name="__main__")
    return 0


def run(args, *, windows_factory=Windows):
    result = {"schema": "p1-isolated-desktop-v1", "startedUnixNs": time.time_ns(),
        "adapterPid": os.getpid(), "nativeSaveValidated": False, "runtimeValidated": False,
        "errors": [], "adapterCompleted": False, "actualExitCode": None,
        "timeout": False, "handlesClosed": {}, "allDescendantExitsProven": False}
    policy = result["desktopAppPolicy"] = {"requested": getattr(args, "unpackaged_descendants", False),
        "attribute": "PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY", "value": 1,
        "scope": "collector-descendants", "jobBreakaway": False,
        "attributesInitialized": False, "attributeConfigured": False,
        "appliedToCollector": False, "attributesDeleted": False}
    evidence, win, helpers, job, desktop = None, None, None, None, None
    pi, assigned, input_before, request_sha = PROCESSINFO(), False, None, None
    try:
        argv, image, entry, command, cwd, helper, env, evidence_path = preflight(args)
        evidence_path.mkdir()  # 不覆写旧回执；抢占失败也不改旧目录。
        evidence = evidence_path
        helpers = load_helpers(helper, args.helper_sha256)
        win = windows_factory()
        input_before = helpers._query_input_desktop_name()
        require(input_before, "无法证明当前 input Desktop")
        station = helpers._desktop_name_from_handle(win.u.GetProcessWindowStation())
        require(station.casefold() == "winsta0", "adapter 只支持当前 WinSta0")
        result.update(inputDesktopBefore=input_before, windowStation=station,
            parentDesktop=win.thread_desktop(helpers, win.k.GetCurrentThreadId()),
            logicalArgv=argv, cwd=str(cwd), environmentKeys=sorted(env),
            environmentSha256=environment_sha(env),
            helper={"path": str(helper), "sha256": args.helper_sha256.lower(),
                    "selectedDefinitions": sorted(HELPERS), "moduleTopLevelExecuted": False},
            entrySha256=args.entry_sha256.lower(), pythonSha256=sha_bytes(image.read_bytes()))
        name = "P1Owned_" + uuid.uuid4().hex
        prior = win.u.OpenDesktopW(name, 0, False, 1)
        if prior:
            win.check(win.u.CloseDesktop(prior), "关闭碰撞查询句柄")
            raise ValueError("随机 Desktop 已存在，拒绝复用")
        require(C.get_last_error() == 2, "无法证明随机 Desktop 不存在")
        creation = helpers._create_isolated_desktop(name)
        require(creation.get("ok"), "Desktop 创建失败: " + str(creation))
        desktop = creation["handle"]
        result["desktop"] = {"name": name, "handle": desktop, "creation": creation}
        require(helpers._desktop_name_from_handle(desktop) == name, "实际创建 Desktop 名称不符")
        request_path, witness_path = evidence / "worker-request.local.json", evidence / "worker-witness.local.json"
        physical = [str(image), "-B", str(Path(__file__).resolve()), "--worker-request", str(request_path)]
        request = {"schema": "p1-desktop-worker-v1", "argv": argv, "entrySha256": args.entry_sha256.lower(),
            "adapterSha256": sha_bytes(Path(__file__).read_bytes()), "helper": str(helper),
            "helperSha256": args.helper_sha256.lower(), "cwd": str(cwd), "environmentSha256": environment_sha(env),
            "environmentKeys": sorted(env), "desktop": name, "witness": str(witness_path), "physicalArgv": physical,
            "unpackagedDescendants": policy["requested"]}
        request_raw = json.dumps(request, ensure_ascii=True, sort_keys=True).encode("utf-8")
        request_sha = sha_bytes(request_raw)
        with request_path.open("xb") as stream:
            stream.write(request_raw)
        physical.extend(["--request-sha256", request_sha])
        command = subprocess.list2cmdline(physical)
        require(len(command) < 32767, "worker 物理命令行超限")
        result.update(physicalArgv=physical, requestSha256=request_sha)
        job = win.check(win.k.CreateJobObjectW(None, None), "创建 OWN Job")
        limits = JOBEXT()
        limits.basic.flags = 0x2000
        win.check(win.k.SetInformationJobObject(job, 9, C.byref(limits), C.sizeof(limits)), "OWN Job kill-on-close")
        si = STARTUPINFO()
        si.cb, si.desktop, si.flags, si.show = C.sizeof(si), "WinSta0\\" + name, 1, 0
        cmd, env_block = C.create_unicode_buffer(command), helpers._build_environment_block(env)
        with desktop_app_policy(win, si, policy["requested"], policy) as (startup, extra_flags):
            previous_mode = win.k.SetErrorMode(0x8003)
            try:
                win.check(win.k.CreateProcessW(str(image), cmd, None, None, False,
                    0x4 | 0x400 | 0x08000000 | extra_flags, env_block, str(cwd), startup, C.byref(pi)), "创建 OWN Python")
                policy["appliedToCollector"] = policy["requested"]
            finally:
                win.k.SetErrorMode(previous_mode)
        result["process"] = win.identity(pi, image)
        result["launch"] = {"explicitDesktop": si.desktop, "showWindow": "SW_HIDE",
            "inheritHandles": False, "createSuspended": True, "createNoWindow": True}
        win.check(win.k.AssignProcessToJobObject(job, pi.process), "挂起 OWN Python 加入 Job")
        assigned = True
        require(helpers._query_input_desktop_name() == input_before, "启动前 input Desktop 已变化")
        require(win.k.ResumeThread(pi.thread) == 1, "OWN Python suspend count 不符")
        waited = win.k.WaitForSingleObject(pi.process, int(args.timeout_seconds * 1000))
        if waited == 258:
            result["timeout"] = True
            raise TimeoutError("OWN Python 外层超时")
        require(waited == 0, "OWN Python wait 失败")
        result["collectorSignaled"] = True
        code = U32()
        win.check(win.k.GetExitCodeProcess(pi.process, C.byref(code)), "OWN Python 实际 exit")
        result["actualExitCode"], result["actualExitHex"] = code.value, f"0x{code.value:08X}"
    except Exception as error:
        result["errors"].append(type(error).__name__ + ": " + str(error))
    finally:
        if win:
            if pi.process and win.k.WaitForSingleObject(pi.process, 0) != 0:
                try:
                    if assigned:
                        win.check(win.k.TerminateJobObject(job, KILL_CODE), "终止 OWN Job")
                        result["cleanupAction"] = "TerminateOwnJob"
                    else:
                        win.check(win.k.TerminateProcess(pi.process, KILL_CODE), "终止 OWN 挂起原handle")
                        result["cleanupAction"] = "TerminateOwnSuspendedProcess"
                    require(win.k.WaitForSingleObject(pi.process, 5000) == 0, "清理后 OWN Python 未 signaled")
                except Exception as error:
                    result["errors"].append("cleanup: " + str(error))
            signaled = bool(pi.process and win.k.WaitForSingleObject(pi.process, 0) == 0)
            result["collectorSignaled"] = signaled
            if pi.process and signaled and result["actualExitCode"] is None:
                code = U32()
                if win.k.GetExitCodeProcess(pi.process, C.byref(code)):
                    result["actualExitCode"], result["actualExitHex"] = code.value, f"0x{code.value:08X}"
            if job:
                try:
                    state = win.job_state(job, pi.pid, signaled)
                    if state["enumerationComplete"] and state["unresolvedAssociatedPids"]:
                        result["jobFirstQuery"] = state
                        result["associatedIdentityAtDrain"] = [win.associated_identity(job, pid)
                            for pid in state["unresolvedAssociatedPids"][:16]]
                        require(all(row.get("queryHandleClosed", True) for row in result["associatedIdentityAtDrain"]),
                                "关联项只读诊断句柄关闭失败")
                        drain_until = time.monotonic() + 2
                        while state["enumerationComplete"] and state["unresolvedAssociatedPids"] and time.monotonic() < drain_until:
                            time.sleep(0.05)
                            state = win.job_state(job, pi.pid, signaled)
                    result["jobBeforeClose"] = state
                    require(state["enumerationComplete"] and not state["unresolvedAssociatedPids"],
                        "OWN Job 仍有未证明退出的关联 PID，close仅是 containment")
                except Exception as error:
                    result["errors"].append("job: " + str(error))
                result["handlesClosed"]["job"] = bool(win.k.CloseHandle(job))
                result["jobKillOnCloseContainment"] = result["handlesClosed"]["job"]
            if pi.process:
                result["collectorSignaledAfterJobClose"] = win.k.WaitForSingleObject(pi.process, 1000) == 0
            for name, handle in (("thread", pi.thread), ("process", pi.process)):
                if handle:
                    result["handlesClosed"][name] = bool(win.k.CloseHandle(handle))
        if helpers and desktop:
            result["handlesClosed"]["desktop"] = helpers._close_desktop_handle(desktop)
        if helpers and input_before:
            result["inputDesktopAfter"] = helpers._query_input_desktop_name()
            if result["inputDesktopAfter"] != input_before:
                result["errors"].append("input Desktop 在运行期间变化；未尝试切回")
        if evidence and pi.process and result.get("cleanupAction") != "TerminateOwnSuspendedProcess":
            try:
                witness_path = evidence / "worker-witness.local.json"
                require(witness_path.is_file() and witness_path.stat().st_size < 131072, "缺少有界 worker witness")
                witness = json.loads(witness_path.read_text(encoding="utf-8"))
                require(witness["pid"] == pi.pid and witness["desktop"]["threadId"] == pi.tid and
                    witness["desktop"]["name"] == result["desktop"]["name"] and
                    witness["requestSha256"] == request_sha and witness["logicalArgv"] == result["logicalArgv"] and
                    witness["environmentSha256"] == result["environmentSha256"] and
                    witness["unpackagedDescendantsRequested"] == policy["requested"] and
                    witness["entrySha256"] == args.entry_sha256.lower() and
                    witness["physicalCommandline"] == subprocess.list2cmdline(result["physicalArgv"]) and
                    witness["sameProcessMainThread"] and witness["witnessBeforeEntry"], "worker witness 绑定不符")
                result["collectorDesktop"] = witness["desktop"]
                result["workerWitness"] = witness
            except Exception as error:
                result["errors"].append("worker witness: " + str(error))
        if any(not value for value in result["handlesClosed"].values()):
            result["errors"].append("至少一个自有句柄关闭失败")
        result["adapterCompleted"] = bool(pi.process and result.get("collectorSignaledAfterJobClose") and
            not result["errors"] and result["actualExitCode"] is not None)
        result["returnCode"] = result["actualExitCode"] if result["adapterCompleted"] else 124 if result["timeout"] else 2
        result["endedUnixNs"] = time.time_ns()
        if evidence:
            try:
                with (evidence / "summary.local.json").open("x", encoding="utf-8") as stream:
                    json.dump(result, stream, ensure_ascii=True, indent=2)
            except Exception as error:
                result["errors"].append("summary write: " + str(error))
                result["adapterCompleted"], result["returnCode"] = False, 2
    return result


def parser():
    class Parser(argparse.ArgumentParser):
        def _get_option_tuples(self, option_string):
            return [item for item in super()._get_option_tuples(option_string)
                    if item[0].dest != "unpackaged_descendants" or option_string == "--unpackaged-descendants"]
    class OptInOnce(argparse.Action):
        def __call__(self, parser, namespace, values, option_string=None):
            if getattr(namespace, self.dest, False):
                raise argparse.ArgumentError(self, "显式opt-in不得重复")
            setattr(namespace, self.dest, True)
    p = Parser(description=__doc__)
    for name in ("helper", "helper-sha256", "entry-sha256", "cwd", "env-json", "evidence", "argv-json"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--timeout-seconds", type=float, default=90)
    p.add_argument("--unpackaged-descendants", action=OptInOnce, nargs=0, default=False,
                   help="Explicit desktop-app policy=1 for collector descendants; preserves OWN Job/Desktop")
    return p


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--worker-request" and sys.argv[3] == "--request-sha256":
        return worker(sys.argv[2], sys.argv[4])
    result = run(parser().parse_args())
    print(json.dumps({key: result.get(key) for key in ("adapterCompleted", "actualExitCode", "returnCode", "errors")}))
    return result["returnCode"]


if __name__ == "__main__":
    sys.exit(main())
