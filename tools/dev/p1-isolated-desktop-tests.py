"""独立 Desktop 的原创 Python/手写 PE32 夹具；不运行 WE/游戏，不写注册表。"""
from __future__ import annotations

import argparse
import contextlib
import ctypes as C
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
import unittest


HERE = Path(__file__).resolve().parent


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


A = module("p1_isolated_desktop", HERE / "p1-isolated-desktop.py")
ROOT, HELPER, HELPER_SHA = None, None, None


def fixture_child(action, output):
    """只能由测试显式调用的原创 Python 入口，输出自身真实 Desktop/argv/env。"""
    win = A.Windows()
    user = win.u
    user.GetUserObjectInformationW.argtypes = [A.PTR, C.c_int, A.PTR, A.U32, C.POINTER(A.U32)]
    user.GetUserObjectInformationW.restype = C.c_int
    buf, needed = C.create_unicode_buffer(256), A.U32()
    tid = win.k.GetCurrentThreadId()
    handle = user.GetThreadDesktop(tid)
    win.check(user.GetUserObjectInformationW(handle, 2, buf, C.sizeof(buf), C.byref(needed)), "fixture Desktop")
    data = {"pid": os.getpid(), "tid": tid, "desktop": buf.value, "cwd": os.getcwd(),
            "argv": sys.argv, "environmentKeys": sorted(os.environ)}
    if action == "descendant":
        child = subprocess.Popen([sys.executable, "-B", str(Path(__file__).resolve()),
            "--fixture-child", "hang", "--child-output", str(Path(output).with_suffix(".descendant.json"))],
            close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW)
        data["descendantPid"] = child.pid
        # 给原创后代启动时间；Job 中仍存活时 adapter 必须拒签。
        time.sleep(0.3)
    with Path(output).open("x", encoding="utf-8") as stream:
        json.dump(data, stream)
    if action == "hang":
        time.sleep(120)
    return 37 if action == "nonzero" else 0


def fixture_pe(hang=False):
    """原创 x86 机器码：自身查询 Desktop/PID/TID，借 debug string 传回。"""
    section = bytearray(0x3000)
    base, rva = 0x400000, 0x1000
    iat, table, offset = {}, 0x600, 0x900
    imports = [("kernel32.dll", ["GetCurrentThreadId", "GetCurrentProcessId", "OutputDebugStringW", "ExitProcess", "Sleep"]),
               ("user32.dll", ["GetThreadDesktop", "GetUserObjectInformationW", "wsprintfW"])]
    for index, (dll, names) in enumerate(imports):
        dll_raw = dll.encode() + b"\0"
        dll_at = 0x800 + index * 32
        section[dll_at:dll_at + len(dll_raw)] = dll_raw
        struct.pack_into("<IIIII", section, 0x500 + index * 20, rva + table, 0, 0, rva + dll_at, rva + table)
        for name in names:
            raw = b"\0\0" + name.encode() + b"\0"
            section[offset:offset + len(raw)] = raw
            struct.pack_into("<I", section, table, rva + offset)
            iat[name] = base + rva + table
            table += 4
            offset += (len(raw) + 1) & ~1
        table += 4
    label, fmt, buffer, number, needed = [base + rva + n for n in (0xC00, 0xC80, 0x2000, 0x2400, 0x2600)]
    for at, text in ((0xC00, "P1ActualDesktop"), (0xC80, "P1Pid:%u;Tid:%u")):
        raw = (text + "\0").encode("utf-16-le")
        section[at:at + len(raw)] = raw
    code, failures = bytearray(), []

    def push(value):
        code.extend(b"\x68" + struct.pack("<I", value))

    def call(name):
        code.extend(b"\xff\x15" + struct.pack("<I", iat[name]))

    def check():
        code.extend(b"\x85\xc0\x0f\x84")
        failures.append(len(code))
        code.extend(b"\0" * 4)

    call("GetCurrentThreadId")
    code.extend(b"\x50")
    call("GetThreadDesktop")
    check()
    code.extend(b"\x89\xc3")  # ebx = borrowed Desktop handle
    for value in (needed, 512, buffer, 2):
        push(value)
    code.extend(b"\x53")
    call("GetUserObjectInformationW")
    check()
    push(label)
    call("OutputDebugStringW")
    push(buffer)
    call("OutputDebugStringW")
    call("GetCurrentThreadId")
    code.extend(b"\x50")
    call("GetCurrentProcessId")
    code.extend(b"\x50")
    push(fmt)
    push(number)
    call("wsprintfW")
    code.extend(b"\x83\xc4\x10")  # cdecl varargs
    push(number)
    call("OutputDebugStringW")
    if hang:
        push(0xFFFFFFFF)
        call("Sleep")
    push(7)
    call("ExitProcess")
    failure_at = len(code)
    push(91)
    call("ExitProcess")
    for at in failures:
        struct.pack_into("<i", code, at, failure_at - at - 4)
    section[:len(code)] = code
    dos = bytearray(0x80)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)
    coff = struct.pack("<HHIIIHH", 0x14C, 1, 0, 0, 0, 224, 0x0102)
    optional = bytearray(224)
    struct.pack_into("<H", optional, 0, 0x10B)
    struct.pack_into("<III", optional, 4, 0x3000, 0x3000, 0)
    struct.pack_into("<III", optional, 16, rva, rva, rva)
    struct.pack_into("<III", optional, 28, base, 0x1000, 0x200)
    struct.pack_into("<HHHHHH", optional, 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into("<II", optional, 56, 0x5000, 0x200)
    struct.pack_into("<HH", optional, 68, 2, 0)
    struct.pack_into("<IIIIII", optional, 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into("<II", optional, 104, rva + 0x500, 60)
    struct.pack_into("<II", optional, 192, rva + 0x600, table - 0x600)
    sh = struct.pack("<8sIIIIIIHHI", b".p1desk", 0x4000, rva, len(section), 0x200, 0, 0, 0, 0, 0xE0000060)
    return (bytes(dos) + b"PE\0\0" + coff + bytes(optional) + sh).ljust(0x200, b"\0") + bytes(section)


def explicit_env():
    return {key: os.environ[key] for key in ("SystemRoot", "WINDIR", "PATH", "TEMP", "TMP")}


class DesktopTests(unittest.TestCase):
    count = 0

    def case(self, action="normal"):
        DesktopTests.count += 1
        root = ROOT / f"case-{DesktopTests.count:02d}-{self._testMethodName}"
        root.mkdir()
        cwd = root / "工作目录 $quote` space"
        cwd.mkdir()
        entry = Path(__file__).resolve()
        argv = [sys.executable, "-B", str(entry), "--fixture-child", action,
                "--child-output", str(root / "child.local.json")]
        args = SimpleNamespace(helper=str(HELPER), helper_sha256=HELPER_SHA,
            entry_sha256=A.sha_bytes(entry.read_bytes()), cwd=str(cwd), env_json=json.dumps(explicit_env()),
            evidence=str(root / "desktop"), argv_json=json.dumps(argv), timeout_seconds=15)
        return args, root

    def completed(self, result, expected_exit=0):
        self.assertTrue(result["adapterCompleted"], result)
        self.assertEqual(result["actualExitCode"], expected_exit, result)
        self.assertEqual(result["returnCode"], expected_exit)
        self.assertEqual(result["errors"], [])
        self.assertTrue(result["collectorSignaledAfterJobClose"])
        self.assertEqual(result["inputDesktopBefore"], result["inputDesktopAfter"])
        self.assertNotEqual(result["desktop"]["name"], result["inputDesktopBefore"])
        self.assertEqual(result["collectorDesktop"]["name"], result["desktop"]["name"])
        self.assertTrue(all(result["handlesClosed"].values()))
        self.assertEqual(set(result["handlesClosed"]), {"thread", "process", "job", "desktop"})
        self.assertTrue(result["jobBeforeClose"]["enumerationComplete"])
        self.assertEqual(result["jobBeforeClose"]["unresolvedAssociatedPids"], [])
        self.assertFalse(result["nativeSaveValidated"])
        self.assertFalse(result["runtimeValidated"])
        self.assertFalse(result["allDescendantExitsProven"])

    def test_actual_python_desktop_cwd_argv(self):
        args, root = self.case()
        result = A.run(args)
        self.completed(result)
        child = json.loads((root / "child.local.json").read_text())
        self.assertEqual(child["desktop"], result["desktop"]["name"])
        self.assertEqual(child["pid"], result["process"]["pid"])
        self.assertEqual(child["tid"], result["process"]["primaryThreadId"])
        self.assertEqual(child["cwd"], args.cwd)
        self.assertEqual(child["argv"], json.loads(args.argv_json)[2:])

    def test_nonzero_actual_exit_propagation(self):
        args, _ = self.case("nonzero")
        self.completed(A.run(args), 37)

    def test_cli_actual_process_exit_propagation(self):
        args, root = self.case("nonzero")
        command = [sys.executable, "-B", str(HERE / "p1-isolated-desktop.py")]
        for key, value in vars(args).items():
            command.extend(["--" + key.replace("_", "-"), str(value)])
        actual = subprocess.run(command, capture_output=True, text=True, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW, close_fds=True)
        self.assertEqual(actual.returncode, 37, actual.stderr)
        result = json.loads((root / "desktop" / "summary.local.json").read_text())
        self.completed(result, 37)
        self.assertEqual(json.loads(actual.stdout)["returnCode"], 37)

    def test_timeout_own_handle_wait_close(self):
        args, _ = self.case("hang")
        args.timeout_seconds = 0.7
        result = A.run(args)
        self.assertFalse(result["adapterCompleted"])
        self.assertTrue(result["timeout"], result)
        self.assertEqual(result["returnCode"], 124)
        self.assertEqual(result["actualExitCode"], A.KILL_CODE)
        self.assertEqual(result["cleanupAction"], "TerminateOwnJob")
        self.assertTrue(result["collectorSignaledAfterJobClose"])
        self.assertTrue(all(result["handlesClosed"].values()))

    def test_unresolved_descendant_is_not_success(self):
        args, _ = self.case("descendant")
        result = A.run(args)
        self.assertFalse(result["adapterCompleted"], result)
        self.assertEqual(result["actualExitCode"], 0, result)
        self.assertTrue(result["jobBeforeClose"]["unresolvedAssociatedPids"])
        self.assertTrue(result["jobKillOnCloseContainment"])
        self.assertFalse(result["allDescendantExitsProven"])

    def test_job_assignment_failure_never_resumes(self):
        args, root = self.case()

        class FailAssign(A.Windows):
            def __init__(self):
                super().__init__()
                # 仅原创夹具注入自己持有对象，未改 OS API 或冻结 collector。
                self.k.AssignProcessToJobObject = lambda *_: 0

        result = A.run(args, windows_factory=FailAssign)
        self.assertFalse(result["adapterCompleted"])
        self.assertEqual(result["cleanupAction"], "TerminateOwnSuspendedProcess")
        self.assertTrue(result["collectorSignaledAfterJobClose"])
        self.assertFalse((root / "child.local.json").exists())

    def test_reject_old_evidence(self):
        args, _ = self.case()
        Path(args.evidence).mkdir()
        result = A.run(args)
        self.assertNotIn("process", result)
        self.assertFalse((Path(args.evidence) / "summary.local.json").exists())

    def test_reject_environment_secret_and_duplicate(self):
        args, _ = self.case()
        for additions in ({"P1_PRIVATE_TOKEN": "never-persist-sentinel-44d8"}, {"Path": "duplicate"}):
            args.env_json = json.dumps({**explicit_env(), **additions})
            result = A.run(args)
            self.assertNotIn("process", result)
            self.assertNotIn("never-persist-sentinel-44d8", json.dumps(result))

    def test_reject_entry_sha_and_nul(self):
        args, _ = self.case()
        args.entry_sha256 = "0" * 64
        self.assertNotIn("process", A.run(args))
        args.argv_json = json.dumps([sys.executable, "-B", "bad\0"])
        self.assertNotIn("process", A.run(args))

    def test_helper_hash_and_signature_fail_closed(self):
        args, root = self.case()
        args.helper_sha256 = "0" * 64
        self.assertNotIn("process", A.run(args))
        source = HELPER.read_text(encoding="utf-8-sig")
        changed = root / "helper-signature.py"
        changed.write_text(source.replace("def _close_desktop_handle(handle: int)",
            "def _close_desktop_handle(handle: int, extra=None)"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "参数契约"):
            A.load_helpers(changed, A.sha_bytes(changed.read_bytes()))

    def test_worker_rejects_request_hash_schema_and_entry(self):
        args, root = self.case()
        self.completed(A.run(args))
        original = json.loads((root / "desktop" / "worker-request.local.json").read_text())
        request = root / "tampered-request.local.json"
        request.write_text(json.dumps(original), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "请求 SHA"):
            A.worker(str(request), "0" * 64)
        for change, message in (({"extra": "reject"}, "schema"), ({"entrySha256": "0" * 64}, "entry SHA"),
                                ({"adapterSha256": "0" * 64}, "自身 SHA")):
            request.write_text(json.dumps({**original, **change}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, message):
                A.worker(str(request), A.sha_bytes(request.read_bytes()))

    def trace_case(self, *, hang=False):
        args, root = self.case()
        T = module("p1_loader_trace_fixture", HERE / "p1-loader-trace.py")
        game, host = root / "game", root / "host"
        game.mkdir()
        host.mkdir()
        (host / "bin").mkdir()
        image, config = game / "worldeditydwe.exe", game / "worldeditydwe.exe.config"
        image.write_bytes(fixture_pe(hang))
        config.write_bytes(T.N.EDITOR_LOADER_CONFIG)
        map_path = root / "原创非地图 $tick` space.w3x"
        map_path.write_bytes(b"P1 original synthetic non-map marker")
        prepared_path = root / "prepared.local.json"
        prepared_path.write_text(json.dumps({"schema": "p1-native-loader-v1", "status": "prepared",
            "sourceUnchanged": True, "gameRoot": str(game), "hostRoot": str(host),
            "loader": {"relativePath": image.name, **T.N.file_identity(image)},
            "config": {"relativePath": config.name, **T.N.file_identity(config)},
            "syntheticFixtureOnly": True}), encoding="utf-8")
        trace = HERE / "p1-loader-trace.py"
        argv = [sys.executable, "-B", str(trace)]
        for flag, path in (("image", image), ("config", config), ("prepared", prepared_path), ("map", map_path)):
            argv.extend(["--" + flag, str(path), "--" + flag + "-sha256", A.sha_bytes(path.read_bytes())])
        argv.extend(["--cwd", str(host), "--argv-json", json.dumps([str(image), "-loadfile", str(map_path)]),
            "--process-name", "ydwe", "--evidence", str(root / "trace"), "--timeout-seconds", "2"])
        args.argv_json, args.entry_sha256 = json.dumps(argv), A.sha_bytes(trace.read_bytes())
        args.timeout_seconds = 20
        return args, root

    def test_two_hops_frozen_collector_wow64_actual_desktop(self):
        args, root = self.trace_case()
        result = A.run(args)
        self.completed(result)
        trace = json.loads((root / "trace" / "summary.local.json").read_text())
        self.assertTrue(trace["traceCompleted"], trace)
        self.assertTrue(trace["evidenceComplete"], trace)
        self.assertEqual(trace["actualExitCode"], 7)
        self.assertEqual(trace["process"]["processMachine"], "0x14c")
        self.assertEqual(trace["process"]["nativeMachine"], "0x8664")
        self.assertEqual(trace["errors"], [])
        events = [json.loads(line) for line in (root / "trace" / "events.local.jsonl").read_text().splitlines()]
        texts = [row["debugString"].get("text") for row in events if row["event"] == "OUTPUT_DEBUG_STRING"]
        at = texts.index("P1ActualDesktop")
        self.assertEqual(texts[at + 1], result["desktop"]["name"])
        pid_text = next(text for text in texts if text and text.startswith("P1Pid:"))
        pid = int(pid_text.split(";")[0].split(":")[1])
        tid = int(pid_text.split(";")[1].split(":")[1])
        self.assertEqual(pid, trace["process"]["pid"])
        self.assertEqual(tid, next(row["tid"] for row in events if row["event"] == "CREATE_PROCESS"))
        self.assertNotEqual(pid, result["process"]["pid"])
        self.assertEqual(trace["jobBeforeClose"]["unresolvedAssociatedPids"], [])
        self.assertTrue(trace["ownTargetSignaledAfterJobClose"])
        self.assertFalse(trace["nativeSaveValidated"])

    def test_collector_timeout_drains_before_adapter_exit(self):
        args, root = self.trace_case(hang=True)
        result = A.run(args)
        self.completed(result, 2)
        self.assertFalse(result["timeout"])
        trace = json.loads((root / "trace" / "summary.local.json").read_text())
        self.assertTrue(trace["targetExitObserved"])
        self.assertTrue(trace["ownTargetSignaledAfterJobClose"])
        self.assertFalse(trace["evidenceComplete"])
        self.assertTrue(any("timeout" in value for value in trace["errors"]))

    def test_collector_requires_cleanup_margin(self):
        args, _ = self.trace_case()
        args.timeout_seconds = 2
        self.assertNotIn("process", A.run(args))

    def deadline_argv(self):
        args, _ = self.trace_case()
        argv = json.loads(args.argv_json)
        self.assertEqual(argv[-2:], ["--timeout-seconds", "2"])
        return args, argv[:-2]

    def reject_deadline_forms(self, cases):
        args, base = self.deadline_argv()
        collector = module("p1_deadline_parser_oracle", HERE / "p1-loader-trace.py")
        args.timeout_seconds = 90
        for flags, actual_timeout in cases:
            with self.subTest(flags=flags):
                # 原冻结 parser 确实接受这些形式；仅测解析，不运行真实/夹具目标。
                self.assertEqual(collector.parser().parse_args([*base[3:], *flags]).timeout_seconds, actual_timeout)
                args.argv_json = json.dumps([*base, *flags])
                with self.assertRaisesRegex(ValueError, "timeout"):
                    A.preflight(args)
        self.assertFalse(Path(args.evidence).exists(), "deadline 拒绝必须发生在新建回执/Desktop前")

    def test_collector_rejects_equals_and_mixed_timeout_forms(self):
        self.reject_deadline_forms([
            (["--timeout-seconds=500"], 500),
            (["--timeout-seconds=120"], 120),
            (["--timeout-seconds", "30", "--timeout-seconds=120"], 120),
            (["--timeout-seconds=120", "--timeout-seconds", "30"], 30),
        ])

    def test_collector_rejects_duplicate_timeout_even_same_value(self):
        self.reject_deadline_forms([
            (["--timeout-seconds", "30", "--timeout-seconds", "500"], 500),
            (["--timeout-seconds", "30", "--timeout-seconds", "120"], 120),
            (["--timeout-seconds", "30", "--timeout-seconds", "30"], 30),
        ])

    def test_collector_rejects_all_argparse_timeout_abbreviations(self):
        option = "--timeout-seconds"
        cases = [(["--timeout", "500"], 500), (["--timeout-s", "500"], 500),
                 (["--timeout=500"], 500), (["--timeout-s=500"], 500)]
        for length in range(3, len(option)):
            prefix = option[:length]
            cases.extend([([prefix, "120"], 120), ([prefix + "=120"], 120),
                          (["--timeout-seconds", "30", prefix, "120"], 120)])
        self.reject_deadline_forms(cases)

    def test_collector_actual_default_and_explicit_cleanup_boundaries(self):
        args, base = self.deadline_argv()
        for flags, actual_timeout in (([], 30), (["--timeout-seconds", "60"], 60),
                                     (["--timeout-seconds", "1.25e2"], 125)):
            with self.subTest(flags=flags):
                args.argv_json = json.dumps([*base, *flags])
                args.timeout_seconds = actual_timeout + 15
                A.preflight(args)
                args.timeout_seconds -= 0.001
                with self.assertRaisesRegex(ValueError, "15秒"):
                    A.preflight(args)
        for value in ("nan", "inf", "-inf"):
            args.argv_json = json.dumps([*base, "--timeout-seconds", value])
            args.timeout_seconds = 600
            with self.assertRaises(ValueError):
                A.preflight(args)

    def test_collector_timeout_text_inside_values_is_not_an_option(self):
        args, base = self.deadline_argv()
        nested = base.index("--argv-json") + 1
        base[nested] = json.dumps(["--timeout-seconds=500", "--timeout", "500"])
        args.argv_json = json.dumps(base)
        args.timeout_seconds = 45
        A.preflight(args)  # 只验parser/deadline，不以该无效目标argv运行collector。
        args.timeout_seconds = 44.999
        with self.assertRaisesRegex(ValueError, "15秒"):
            A.preflight(args)

    def test_descendant_policy_opt_in_is_exact_single_boolean(self):
        args, _ = self.case()
        base = [item for key, value in vars(args).items() for item in ("--" + key.replace("_", "-"), str(value))]
        self.assertFalse(A.parser().parse_args(base).unpackaged_descendants)
        self.assertTrue(A.parser().parse_args([*base, "--unpackaged-descendants"]).unpackaged_descendants)
        bad = [["--unpackaged-descendants", "--unpackaged-descendants"],
               ["--unpackaged-descendants=false"], ["--unpackaged-descendants", "1"]]
        bad.extend([["--unpackaged-descendants"[:n]] for n in range(3, len("--unpackaged-descendants"))])
        for flags in bad:
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                A.parser().parse_args([*base, *flags])

    def test_descendant_policy_invalid_types_stop_before_windows(self):
        for value in (1, "true", None):
            args, root = self.case()
            args.unpackaged_descendants = value
            called = []
            result = A.run(args, windows_factory=lambda: called.append(True))
            self.assertEqual(called, [])
            self.assertNotIn("process", result)
            self.assertFalse((root / "desktop").exists())

    def policy_factory(self, stage, calls):
        class ApiSpy(A.Windows):
            def __init__(self):
                super().__init__()
                initialize, update, delete, create = (self.k.InitializeProcThreadAttributeList,
                    self.k.UpdateProcThreadAttribute, self.k.DeleteProcThreadAttributeList, self.k.CreateProcessW)
                initialize.argtypes, initialize.restype = [A.PTR, A.U32, A.U32, C.POINTER(A.SIZE)], C.c_int
                update.argtypes, update.restype = [A.PTR, A.U32, A.SIZE, A.PTR, A.SIZE, A.PTR, A.PTR], C.c_int
                delete.argtypes, delete.restype = [A.PTR], None
                def init(pointer, count, flags, size):
                    calls.append("size" if pointer is None else "initialize")
                    if stage == ("size" if pointer is None else "initialize"):
                        C.set_last_error(5)
                        return 0
                    return initialize(pointer, count, flags, size)
                def set_attribute(pointer, flags, attribute, value, size, previous, returned):
                    calls.append(("update", attribute, C.cast(value, C.POINTER(A.U32)).contents.value))
                    if stage == "update":
                        C.set_last_error(5)
                        return 0
                    return update(pointer, flags, attribute, value, size, previous, returned)
                def free(pointer):
                    calls.append("delete")
                    delete(pointer)
                    if stage == "delete":
                        raise OSError("original fixture post-delete failure")
                def start(*values):
                    extended = C.cast(values[8], C.POINTER(A.STARTUPINFOEX)).contents
                    calls.append(("create", values[5], extended.startup.cb, extended.startup.desktop,
                                  bool(extended.attributes)))
                    if stage == "create":
                        C.set_last_error(5)
                        return 0
                    return create(*values)
                self.k.InitializeProcThreadAttributeList = init
                self.k.UpdateProcThreadAttribute = set_attribute
                self.k.DeleteProcThreadAttributeList = free
                self.k.CreateProcessW = start
        return ApiSpy

    def test_descendant_policy_attribute_failures_never_create(self):
        for stage in ("size", "initialize", "update"):
            args, root = self.case()
            args.unpackaged_descendants = True
            calls = []
            result = A.run(args, windows_factory=self.policy_factory(stage, calls))
            self.assertNotIn("process", result)
            self.assertFalse(result["adapterCompleted"])
            self.assertFalse(any(isinstance(row, tuple) and row[0] == "create" for row in calls))
            self.assertEqual(calls.count("delete"), 1 if stage == "update" else 0)
            self.assertFalse((root / "child.local.json").exists())
            self.assertTrue(all(result["handlesClosed"].values()))

    def test_descendant_policy_create_failure_releases_attributes(self):
        args, _ = self.case()
        args.unpackaged_descendants = True
        calls = []
        result = A.run(args, windows_factory=self.policy_factory("create", calls))
        self.assertFalse(result["adapterCompleted"])
        self.assertEqual(calls[-1], "delete")
        self.assertEqual(calls.count("delete"), 1)
        self.assertFalse(result["desktopAppPolicy"]["appliedToCollector"])
        self.assertTrue(result["desktopAppPolicy"]["attributesDeleted"])

    def test_descendant_policy_post_create_failure_reaps_suspended_original(self):
        args, root = self.case()
        args.unpackaged_descendants = True
        calls = []
        result = A.run(args, windows_factory=self.policy_factory("delete", calls))
        self.assertFalse(result["adapterCompleted"])
        self.assertEqual(result["cleanupAction"], "TerminateOwnSuspendedProcess")
        self.assertTrue(result["collectorSignaledAfterJobClose"])
        self.assertFalse((root / "child.local.json").exists())
        self.assertEqual(calls.count("delete"), 1)

    def test_descendant_policy_actual_wow64_target_no_package_same_desktop_job(self):
        args, root = self.trace_case()
        args.unpackaged_descendants = True
        args.argv_json = json.dumps([*json.loads(args.argv_json), "--require-unpackaged-target"])
        calls = []
        result = A.run(args, windows_factory=self.policy_factory(None, calls))
        self.completed(result)
        self.assertIn(("update", 0x20012, 1), calls)
        create = next(row for row in calls if isinstance(row, tuple) and row[0] == "create")
        self.assertEqual(create[1], 0x4 | 0x400 | 0x08000000 | 0x00080000)
        self.assertEqual(create[2:], (112, "WinSta0\\" + result["desktop"]["name"], True))
        self.assertEqual(calls[-1], "delete")
        policy = result["desktopAppPolicy"]
        self.assertTrue(all(policy[key] for key in ("requested", "attributesInitialized", "attributeConfigured",
                                                   "appliedToCollector", "attributesDeleted")))
        self.assertFalse(policy["jobBreakaway"])
        trace = json.loads((root / "trace" / "summary.local.json").read_text())
        identity = trace["targetPackageIdentity"]
        self.assertTrue(trace["requireUnpackagedTarget"])
        self.assertEqual(identity["state"], "unpackaged")
        self.assertEqual(identity["returnCodes"], [15700])
        self.assertIsNone(identity["packageFullName"])
        self.assertEqual(identity["processIdentity"], trace["process"])
        self.assertTrue(identity["originalProcessHandle"] and identity["beforeResume"])
        self.assertEqual(trace["actualExitCode"], 7)
        self.assertTrue(trace["traceCompleted"] and trace["evidenceComplete"])
        events = [json.loads(line) for line in (root / "trace" / "events.local.jsonl").read_text().splitlines()]
        texts = [row["debugString"].get("text") for row in events if row["event"] == "OUTPUT_DEBUG_STRING"]
        self.assertEqual(texts[texts.index("P1ActualDesktop") + 1], result["desktop"]["name"])
        self.assertEqual(trace["jobBeforeClose"]["unresolvedAssociatedPids"], [])
        self.assertTrue(trace["ownTargetSignaledAfterJobClose"])


def main():
    global ROOT, HELPER, HELPER_SHA
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output")
    p.add_argument("--helper")
    p.add_argument("--fixture-child", choices=("normal", "nonzero", "hang", "descendant"))
    p.add_argument("--child-output")
    args = p.parse_args()
    if args.fixture_child:
        return fixture_child(args.fixture_child, args.child_output)
    ROOT, HELPER = A.absolute_path(args.output, exists=False), A.absolute_path(args.helper)
    ROOT.mkdir()
    HELPER_SHA = A.sha_bytes(HELPER.read_bytes())
    started = time.monotonic()
    with (ROOT / "tests.local.log").open("x", encoding="utf-8") as stream:
        result = unittest.TextTestRunner(stream=stream, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DesktopTests))
    summary = {"testsRun": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
        "seconds": round(time.monotonic() - started, 3), "helperSha256": HELPER_SHA,
        "adapterSha256": A.sha_bytes((HERE / "p1-isolated-desktop.py").read_bytes()),
        "testsSha256": A.sha_bytes(Path(__file__).read_bytes()),
        "collectorSha256": A.sha_bytes((HERE / "p1-loader-trace.py").read_bytes()),
        "originalFixturesOnly": True, "registryGuardExercised": False,
        "nativeSaveValidated": False, "runtimeValidated": False}
    (ROOT / "summary.local.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
