"""原创手写 PE32 夹具验证；不运行真实 loader、WE、Warcraft 或已安装 debugger。"""
from __future__ import annotations

import argparse
import contextlib
import ctypes as C
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import time
import tracemalloc
import unittest
from types import SimpleNamespace


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("p1_loader_trace", HERE / "p1-loader-trace.py")
T = importlib.util.module_from_spec(spec)
spec.loader.exec_module(T)
ROOT = None


def fixture_pe(mode="normal"):
    """独立机器码及 PE headers，不复制任何安装二进制或使用真实 editor entry。"""
    section = bytearray(0x2000)
    base, rva = 0x400000, 0x1000
    names = ["OutputDebugStringW", "OutputDebugStringA", "GetCommandLineW", "GetCurrentDirectoryW",
             "GetEnvironmentVariableW", "LoadLibraryW", "FreeLibrary", "ExitProcess", "Sleep", "RaiseException"]
    if mode == "missing":
        extra = True
    else:
        extra = False
    descriptor, table, name_offset = 0x400, 0x500, 0x600
    iat = {}
    for index, name in enumerate(names):
        raw = b"\0\0" + name.encode("ascii") + b"\0"
        section[name_offset:name_offset + len(raw)] = raw
        struct.pack_into("<I", section, table + index * 4, rva + name_offset)
        iat[name] = base + rva + table + index * 4
        name_offset += (len(raw) + 1) & ~1
    dll_offset = 0x780
    section[dll_offset:dll_offset + 13] = b"kernel32.dll\0"
    struct.pack_into("<IIIII", section, descriptor, rva + table, 0, 0, rva + dll_offset, rva + table)
    if extra:
        missing_name = b"p1-owned-definitely-missing-44d8.dll\0"
        section[0x800:0x800 + len(missing_name)] = missing_name
        section[0x850:0x850 + len(b"\0\0MissingEntry\0")] = b"\0\0MissingEntry\0"
        struct.pack_into("<I", section, 0x890, rva + 0x850)
        struct.pack_into("<IIIII", section, descriptor + 20, rva + 0x890, 0, 0, rva + 0x800, rva + 0x890)
    strings = {"unicode": "P1 原创 PE32 fixture\0".encode("utf-16-le"),
               "ansi": b"P1-ANSI:\x80\xfe\xff\0", "version": "version.dll\0".encode("utf-16-le"),
               "name": "ydwe-process-name\0".encode("utf-16-le")}
    pointers, at = {}, 0x900
    for name, raw in strings.items():
        section[at:at + len(raw)] = raw
        pointers[name] = base + rva + at
        at += (len(raw) + 1) & ~1
    code = bytearray()

    def push(value):
        code.extend(b"\x68" + struct.pack("<I", value))

    def call(name):
        code.extend(b"\xff\x15" + struct.pack("<I", iat[name]))

    push(pointers["unicode"])
    call("OutputDebugStringW")
    push(pointers["ansi"])
    call("OutputDebugStringA")
    call("GetCommandLineW")
    code.extend(b"\x50")
    call("OutputDebugStringW")
    buffer = base + rva + 0x3000
    push(buffer)
    push(512)
    call("GetCurrentDirectoryW")
    push(buffer)
    call("OutputDebugStringW")
    push(512)
    push(buffer)
    push(pointers["name"])
    call("GetEnvironmentVariableW")
    push(buffer)
    call("OutputDebugStringW")
    push(pointers["version"])
    call("LoadLibraryW")
    code.extend(b"\x50")
    call("FreeLibrary")
    if mode == "hang":
        push(0xFFFFFFFF)
        call("Sleep")
    elif mode == "exception":
        for value in (0, 0, 0, 0xE0424242):
            push(value)
        call("RaiseException")
    elif mode == "breakpoint":
        code.extend(b"\xcc")
    push(7)
    call("ExitProcess")
    section[:len(code)] = code
    dos = bytearray(0x80)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)
    coff = struct.pack("<HHIIIHH", 0x14C, 1, 0, 0, 0, 224, 0x0102)
    optional = bytearray(224)
    struct.pack_into("<H", optional, 0, 0x10B)
    struct.pack_into("<III", optional, 4, 0x2000, 0x2000, 0)
    struct.pack_into("<III", optional, 16, rva, rva, rva)
    struct.pack_into("<III", optional, 28, base, 0x1000, 0x200)
    struct.pack_into("<HHHHHH", optional, 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into("<II", optional, 56, 0x6000, 0x200)
    struct.pack_into("<HH", optional, 68, 2, 0)  # GUI fixture, no unrelated console.
    struct.pack_into("<IIIIII", optional, 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into("<II", optional, 104, rva + descriptor, 60 if extra else 40)
    struct.pack_into("<II", optional, 192, rva + table, (len(names) + 1) * 4)
    sh = struct.pack("<8sIIIIIIHHI", b".fixture", 0x5000, rva, len(section), 0x200, 0, 0, 0, 0, 0xE0000060)
    headers = bytes(dos) + b"PE\0\0" + coff + bytes(optional) + sh
    return headers.ljust(0x200, b"\0") + bytes(section)


def synthetic_map(path, size):
    """原创重复字节和非整块尾部；生成时独立计算期望 SHA，不读取作者地图。"""
    block = bytes(range(256)) * 4096
    digest, remaining = hashlib.sha256(), size
    with path.open("wb") as stream:
        while remaining:
            piece = block[:min(len(block), remaining)]
            stream.write(piece)
            digest.update(piece)
            remaining -= len(piece)
    return digest.hexdigest()


class TraceTests(unittest.TestCase):
    count = 0

    def case(self, mode="normal"):
        TraceTests.count += 1
        root = ROOT / f"case-{TraceTests.count:02d}-{self._testMethodName}"
        root.mkdir()
        game, host = root / "game", root / "host 工作目录"
        game.mkdir()
        host.mkdir()
        (host / "bin").mkdir()
        image = game / "worldeditydwe.exe"
        image.write_bytes(fixture_pe(mode))
        config = game / "worldeditydwe.exe.config"
        config.write_bytes(T.N.EDITOR_LOADER_CONFIG)
        map_path = root / "地图 $tick` space.w3x"
        map_path.write_bytes(b"P1 original synthetic map marker; never a Warcraft map")
        prepared_path = root / "prepared.local.json"
        prepared = {"schema": "p1-native-loader-v1", "status": "prepared", "sourceUnchanged": True,
                    "gameRoot": str(game), "hostRoot": str(host),
                    "loader": {"relativePath": image.name, **T.N.file_identity(image)},
                    "config": {"relativePath": config.name, **T.N.file_identity(config)},
                    "syntheticFixtureOnly": True}
        prepared_path.write_text(json.dumps(prepared, ensure_ascii=True), encoding="utf-8")
        args = SimpleNamespace(image=str(image), image_sha256=T.N.file_identity(image)["sha256"],
                    config=str(config), config_sha256=T.N.file_identity(config)["sha256"],
                    prepared=str(prepared_path), prepared_sha256=T.N.file_identity(prepared_path)["sha256"],
                    cwd=str(host), map=str(map_path), map_sha256=T.N.file_identity(map_path)["sha256"],
                    argv_json=json.dumps([str(image), "-loadfile", str(map_path)]), process_name="ydwe",
                    evidence=str(root / "trace"), timeout_seconds=3, owned_loader_snaps=False)
        return args, root

    def successful(self, args, expected_code=7):
        result = T.run_trace(args)
        self.assertTrue(result["traceCompleted"], result["errors"])
        self.assertTrue(result["evidenceComplete"], result["errors"])
        self.assertTrue(result["targetExitObserved"], result)
        self.assertTrue(result["ownTargetSignaledAfterJobClose"], result)
        self.assertTrue(result["jobBeforeClose"]["enumerationComplete"])
        self.assertEqual(result["jobBeforeClose"]["unresolvedAssociatedPids"], [])
        self.assertEqual(result["actualExitCode"], expected_code)
        self.assertFalse(result["nativeSaveValidated"])
        self.assertFalse(result["runtimeValidated"])
        return result, [json.loads(line) for line in (Path(args.evidence) / "events.local.jsonl").read_text().splitlines()]

    def rejected(self, args):
        result = T.run_trace(args)
        self.assertFalse(result["traceCompleted"])
        self.assertNotIn("process", result, "preflight 拒绝必须在实际新建进程前")
        self.assertTrue(result["errors"])
        return result

    def test_native_struct_layout(self):
        self.assertEqual(C.sizeof(T.DEBUGEVENT), 176)
        self.assertEqual(T.DEBUGEVENT.info.offset, 16)
        self.assertEqual(C.sizeof(T.EXCEPTIONRECORD), 152)
        self.assertEqual(C.sizeof(T.CREATEPROCESS), 72)
        self.assertEqual(C.sizeof(T.LOADDLL), 40)
        self.assertEqual(C.sizeof(T.JOBEXT), 144)

    def test_actual_identity_environment_and_raw_unicode(self):
        args, root = self.case()
        key, secret = "P1_TRACE_TEST_PRIVATE", "do-not-persist-this-sentinel-901733"
        before = os.environ.get(key)
        os.environ[key] = secret
        try:
            result, events = self.successful(args)
        finally:
            if before is None:
                del os.environ[key]
            else:
                os.environ[key] = before
        self.assertEqual(events[0]["event"], "CREATE_PROCESS")
        self.assertTrue(events[0]["beforeFirstUserExecution"])
        self.assertEqual(result["actualParameters"]["cwd"].rstrip("\\"), args.cwd)
        self.assertEqual(result["actualParameters"]["commandline"], T.subprocess.list2cmdline(json.loads(args.argv_json)))
        self.assertEqual(set(result["actualParameters"]["selectedEnvironment"]), {"path", "ydwe-process-name"})
        texts = [e["debugString"].get("text") for e in events if e["event"] == "OUTPUT_DEBUG_STRING"]
        self.assertIn("P1 原创 PE32 fixture", texts)
        self.assertIn("ydwe", texts)
        ansi = next(e["debugString"] for e in events if e["event"] == "OUTPUT_DEBUG_STRING" and not e["debugString"].get("unicode"))
        self.assertEqual(Path(ansi["reportedRaw"]["path"]).read_bytes(), b"P1-ANSI:\x80\xfe\xff\0")
        for e in events:
            self.assertEqual(len(bytes.fromhex(e["rawEventHex"])), 176)
        startup = [e for e in events if e.get("startupBreakpointCandidateHandled")]
        self.assertEqual([e["exception"]["code"] for e in startup], [0x80000003, 0x4000001F])
        self.assertTrue(all(e["tid"] == events[0]["tid"] for e in startup))
        for path in Path(args.evidence).iterdir():
            self.assertNotIn(secret.encode(), path.read_bytes())
            self.assertNotIn(secret.encode("utf-16-le"), path.read_bytes())
        self.assertFalse(result["memoryWritesAllowed"])
        self.assertFalse(result["loaderSnaps"]["requested"])

    def test_real_loaded_module_paths_and_hashes(self):
        args, _ = self.case()
        result, events = self.successful(args)
        images = [e["image"] for e in events if e["event"] == "LOAD_DLL"]
        version = next(i for i in images if Path(i["path"]).name.lower() == "version.dll")
        self.assertEqual(version["machine"], "0x14c")
        self.assertEqual(version["sha256"], T.N.file_identity(version["path"])["sha256"] if os.stat(version["path"]).st_nlink == 1 else T.N.sha_bytes(Path(version["path"]).read_bytes()))
        self.assertEqual(result["process"]["processMachine"], "0x14c")

    def test_unhandled_exception_preserves_both_chances_and_actual_exit(self):
        args, _ = self.case("exception")
        _, events = self.successful(args, 0xE0424242 - 0x100000000)
        exceptions = [e["exception"] for e in events if e["event"] == "EXCEPTION" and e["exception"]["code"] == 0xE0424242]
        self.assertEqual([e["firstChance"] for e in exceptions], [True, False])

    def test_user_image_breakpoint_is_not_swallowed(self):
        args, _ = self.case("breakpoint")
        _, events = self.successful(args, 0x4000001F)
        user = [e for e in events if e["event"] == "EXCEPTION" and e["exception"]["code"] == 0x4000001F and
                Path(e["exception"].get("observedAddressModule", {}).get("path", "")).name == "worldeditydwe.exe"]
        self.assertEqual([e["exception"]["firstChance"] for e in user], [True, False])
        self.assertTrue(all(not e.get("startupBreakpointCandidateHandled") for e in user))

    def test_missing_import_actual_status_without_native_success(self):
        args, _ = self.case("missing")
        result, _ = self.successful(args, 0xC0000135 - 0x100000000)
        self.assertEqual(result["actualExitHex"], "0xC0000135")

    def test_owned_loader_snaps_opt_in_readback_only_on_original_fixture(self):
        args, _ = self.case()
        args.owned_loader_snaps = True
        result, events = self.successful(args)
        self.assertTrue(result["loaderSnaps"]["applied"])
        channels = result["loaderSnaps"]["channels"]
        self.assertEqual([r["kind"] for r in channels], ["native64", "wow64-internal32"])
        self.assertTrue(all(r["verified"] and r["after"] == r["before"] | 2 for r in channels))
        self.assertTrue(any(e["event"] == "OUTPUT_DEBUG_STRING" for e in events))

    def test_loader_snaps_missing_dll_name_is_actual_debug_output(self):
        args, _ = self.case("missing")
        args.owned_loader_snaps = True
        result, events = self.successful(args, 0xC0000135 - 0x100000000)
        raw = b"".join(Path(e["debugString"]["reportedRaw"]["path"]).read_bytes() for e in events
                       if e["event"] == "OUTPUT_DEBUG_STRING")
        name = b"p1-owned-definitely-missing-44d8.dll"
        self.assertTrue(name in raw or name.decode().encode("utf-16-le") in raw)
        self.assertTrue(result["loaderSnaps"]["applied"])

    def test_timeout_kills_only_owned_job_and_continues_exit(self):
        args, _ = self.case("hang")
        args.timeout_seconds = 1
        start = time.monotonic()
        result = T.run_trace(args)
        self.assertLess(time.monotonic() - start, 8)
        self.assertFalse(result["traceCompleted"])
        self.assertEqual(result["actualExitCode"], T.KILL_CODE - 0x100000000)
        self.assertTrue(result["targetExitObserved"])
        self.assertTrue(result["ownTargetSignaledAfterJobClose"])

    def test_event_budget_failure_still_drains_and_kills_own_target(self):
        args, _ = self.case("hang")
        before, T.MAX_EVENTS = T.MAX_EVENTS, 1
        try:
            result = T.run_trace(args)
        finally:
            T.MAX_EVENTS = before
        self.assertFalse(result["traceCompleted"])
        self.assertTrue(result["targetExitObserved"], result)
        self.assertTrue(result["ownTargetSignaledAfterJobClose"])

    def test_evidence_writer_failure_still_drains(self):
        args, _ = self.case("hang")
        original = T.Evidence.event
        def fail(_self, _row):
            raise OSError("original fixture simulated evidence disk failure")
        T.Evidence.event = fail
        try:
            result = T.run_trace(args)
        finally:
            T.Evidence.event = original
        self.assertFalse(result["traceCompleted"])
        self.assertTrue(result["targetExitObserved"], result)
        self.assertTrue(result["ownTargetSignaledAfterJobClose"])

    def test_failed_job_assignment_never_runs_original_fixture_code(self):
        args, _ = self.case()
        original = T.Windows
        class RefusedAssignment(original):
            def __init__(self):
                super().__init__()
                def refuse(_job, _process):
                    C.set_last_error(5)
                    return 0
                self.k.AssignProcessToJobObject = refuse
        T.Windows = RefusedAssignment
        try:
            result = T.run_trace(args)
        finally:
            T.Windows = original
        self.assertFalse(result["traceCompleted"])
        self.assertTrue(result["targetExitObserved"], result["errors"])
        self.assertTrue(result["ownTargetSignaledAfterJobClose"])
        rows = [json.loads(line) for line in (Path(args.evidence) / "events.local.jsonl").read_text().splitlines()]
        self.assertTrue(all(r["event"] == "FAILURE_DRAIN" for r in rows))
        self.assertTrue(all(r["code"] != 8 for r in rows), "Resume 前 Job 失败不得执行fixture的OutputDebugString")

    def test_image_sha_rejected_before_create(self):
        args, _ = self.case()
        args.image_sha256 = "0" * 64
        self.rejected(args)

    def test_prepared_sha_rejected_before_create(self):
        args, _ = self.case()
        args.prepared_sha256 = "0" * 64
        self.rejected(args)

    def test_config_sha_and_fixed_template_enforced(self):
        args, _ = self.case()
        Path(args.config).write_bytes(b"different config")
        self.rejected(args)

    def test_map_sha_rejected_before_create(self):
        args, _ = self.case()
        args.map_sha256 = "0" * 64
        self.rejected(args)

    def test_large_map_identity_streams_without_raw_and_keeps_lock(self):
        args, root = self.case()
        size = 145081468  # 真实失败的大小，内容全部为原创合成字节。
        args.map_sha256 = synthetic_map(Path(args.map), size)
        win = T.Windows()
        locks = T.Locks(win)
        original_read, original_bytes = win.k.ReadFile, win.file_bytes
        reads, handles = [], set()
        map_key = T.canonical(args.map)

        def stream_only(handle, limit=T.MAX_FILE_BYTES):
            self.assertNotEqual(T.canonical(win.file_path(handle)), map_key,
                                "地图不得进入缓存完整 raw 的捕获路径")
            return original_bytes(handle, limit)

        def tracked_read(handle, buffer, amount, read, overlapped):
            result = original_read(handle, buffer, amount, read, overlapped)
            if T.canonical(win.file_path(handle)) == map_key:
                reads.append((amount, C.cast(read, C.POINTER(T.U32)).contents.value))
                handles.add(handle)
            return result

        win.file_bytes, win.k.ReadFile = stream_only, tracked_read
        try:
            tracemalloc.start()
            try:
                launch = T.preflight(args, win, locks)
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
            identity = launch["bindings"]["map"]
            self.assertEqual(identity["bytes"], size)
            self.assertEqual(identity["sha256"], args.map_sha256)
            self.assertEqual(identity["path"], args.map)
            self.assertEqual(set(identity), {"path", "volume", "fileId", "bytes", "links",
                                             "attributes", "writtenFiletime", "sha256"})
            self.assertEqual(sum(actual for _, actual in reads), size)
            self.assertLessEqual(max(requested for requested, _ in reads), 1024 * 1024)
            self.assertEqual(len(handles), 1, "完整 SHA 必须来自同一持有句柄")
            self.assertLess(peak, 8 * 1024 * 1024, "145 MB 地图身份不得积累完整 raw")
            with self.assertRaises(OSError):
                with Path(args.map).open("wb"):
                    pass
            replacement = root / "original-replacement.w3x"
            replacement.write_bytes(b"original synthetic replacement")
            with self.assertRaises(OSError):
                os.replace(replacement, args.map)
            self.assertEqual(Path(args.map).stat().st_size, size)
            self.assertFalse(Path(args.evidence).exists(), "本项仅 preflight，不创建或启动目标")
            receipt = {"syntheticFixtureOnly": True, "realLoaderStarted": False,
                       "identity": identity, "readCalls": len(reads), "handleCount": len(handles),
                       "maxReadRequestBytes": max(requested for requested, _ in reads),
                       "totalReadBytes": sum(actual for _, actual in reads), "pythonPeakBytes": peak,
                       "rawCapturePathUsed": False, "writeAndReplaceRefused": True}
            (root / "map-identity.local.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        finally:
            win.k.ReadFile, win.file_bytes = original_read, original_bytes
            locks.close()
        with Path(args.map).open("ab"):
            pass  # 释放同一组锁后可重新取得写句柄；没有改地图字节。

    def test_map_over_512_mib_rejected_without_reading_content(self):
        args, _ = self.case()
        with Path(args.map).open("wb") as stream:
            stream.truncate(512 * 1024 * 1024 + 1)
        win, locks = T.Windows(), None
        original_read = win.k.ReadFile

        def no_content_read(*_args):
            self.fail("超地图预算必须先拒绝，不能读取内容")

        win.k.ReadFile = no_content_read
        try:
            locks = T.Locks(win)
            with self.assertRaisesRegex(ValueError, "地图超过流式身份预算"):
                locks.map_file(args.map, args.map_sha256)
        finally:
            win.k.ReadFile = original_read
            if locks:
                locks.close()
        result = self.rejected(args)
        self.assertTrue(any("地图超过流式身份预算" in error for error in result["errors"]))

    def test_large_map_wrong_sha_rejected_before_create(self):
        args, _ = self.case()
        expected = synthetic_map(Path(args.map), 145081468)
        args.map_sha256 = ("1" if expected[0] == "0" else "0") + expected[1:]
        result = self.rejected(args)
        self.assertTrue(any("地图 SHA 或运行前身份不符" in error for error in result["errors"]))

    def test_map_identity_change_during_digest_is_rejected(self):
        args, _ = self.case()
        win, locks = T.Windows(), None
        original_info = win.file_info
        map_reads = 0

        def changed_info(handle):
            nonlocal map_reads
            info = original_info(handle)
            if T.canonical(win.file_path(handle)) == T.canonical(args.map):
                map_reads += 1
                if map_reads > 1:
                    info["writtenFiletime"] += 1
            return info

        win.file_info = changed_info
        try:
            locks = T.Locks(win)
            with self.assertRaisesRegex(ValueError, "地图 SHA 或运行前身份不符"):
                locks.map_file(args.map, args.map_sha256)
            self.assertEqual(map_reads, 2)
        finally:
            if locks:
                locks.close()

    def test_map_short_read_is_rejected(self):
        args, _ = self.case()
        win, locks = T.Windows(), None
        original_read = win.k.ReadFile

        def zero_progress(_handle, _buffer, _amount, read, _overlapped):
            C.cast(read, C.POINTER(T.U32)).contents.value = 0
            return 1

        win.k.ReadFile = zero_progress
        try:
            locks = T.Locks(win)
            with self.assertRaisesRegex(ValueError, "读取地图提前结束"):
                locks.map_file(args.map, args.map_sha256)
        finally:
            win.k.ReadFile = original_read
            if locks:
                locks.close()

    def test_map_hardlink_and_reparse_inputs_still_rejected(self):
        for kind in ("hardlink", "reparse"):
            with self.subTest(kind=kind):
                args, root = self.case()
                alias = root / "original-map-alias.w3x"
                if kind == "hardlink":
                    os.link(args.map, alias)
                else:
                    os.symlink(args.map, alias)
                    args.map = str(alias)
                    args.argv_json = json.dumps([args.image, "-loadfile", args.map])
                self.rejected(args)

    def test_non_map_capture_budgets_remain_128_and_16_mib(self):
        for kind, extension, limit in (("image", ".exe", 128 * 1024 * 1024),
                                       ("config", ".config", 128 * 1024 * 1024),
                                       ("json", ".json", 16 * 1024 * 1024),
                                       ("observed", ".dll", 128 * 1024 * 1024)):
            with self.subTest(kind=kind):
                _, root = self.case()
                path = root / ("original-budget" + extension)
                with path.open("wb") as stream:
                    if extension in (".exe", ".dll"):
                        stream.write(fixture_pe())
                    stream.truncate(limit + 1)
                win = T.Windows()
                locks = T.Locks(win)
                try:
                    if kind == "observed":
                        handle = win.k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x00200000, None)
                        self.assertNotEqual(handle, C.c_void_p(-1).value)
                        locks.handles.append(handle)
                        with self.assertRaisesRegex(ValueError, "文件超过捕获预算"):
                            win.observed_file(handle)
                    else:
                        with self.assertRaisesRegex(ValueError, "文件超过捕获预算"):
                            locks.file(path, "0" * 64)
                finally:
                    locks.close()

    def test_argv_cannot_select_another_target_or_extra_switch(self):
        args, _ = self.case()
        args.argv_json = json.dumps([args.image, "-loadfile", args.map, "-extra"])
        self.rejected(args)

    def test_cwd_must_be_bound_host(self):
        args, root = self.case()
        args.cwd = str(root)
        self.rejected(args)

    def test_evidence_directory_cannot_be_reused_or_inside_target(self):
        args, _ = self.case()
        Path(args.evidence).mkdir()
        self.rejected(args)
        args.evidence = str(Path(args.cwd) / "trace")
        self.rejected(args)

    def test_hardlink_input_rejected(self):
        args, root = self.case()
        os.link(args.image, root / "image-alias.exe")
        self.rejected(args)

    def test_directory_reparse_rejected(self):
        args, root = self.case()
        link = root / "alias"
        os.symlink(Path(args.image).parent, link, target_is_directory=True)
        args.image = str(link / "worldeditydwe.exe")
        self.rejected(args)

    def test_bound_input_lock_prevents_write_or_replace(self):
        args, _ = self.case()
        win, locks = T.Windows(), None
        try:
            locks = T.Locks(win)
            launch = T.preflight(args, win, locks)
            with self.assertRaises(OSError):
                Path(args.image).write_bytes(b"changed")
            with self.assertRaises(OSError):
                Path(args.config).unlink()
            self.assertEqual(T.N.sha_bytes(Path(args.image).read_bytes()), args.image_sha256)
            self.assertEqual(launch["bindings"]["map"]["sha256"], args.map_sha256)
        finally:
            if locks:
                locks.close()

    def test_parent_environment_and_error_mode_preserved(self):
        args, _ = self.case()
        before = dict(os.environ)
        win = T.Windows()
        error_mode = win.k.SetErrorMode(0)
        win.k.SetErrorMode(error_mode)
        self.successful(args)
        self.assertEqual(dict(os.environ), before)
        after_mode = win.k.SetErrorMode(error_mode)
        self.assertEqual(after_mode, error_mode)

    def test_package_gate_opt_in_exact_once_and_boolean(self):
        args, _ = self.case()
        base = [item for key, value in vars(args).items() if key != "owned_loader_snaps"
                for item in ("--" + key.replace("_", "-"), str(value))]
        self.assertFalse(T.parser().parse_args(base).require_unpackaged_target)
        self.assertTrue(T.parser().parse_args([*base, "--require-unpackaged-target"]).require_unpackaged_target)
        bad = [["--require-unpackaged-target", "--require-unpackaged-target"],
               ["--require-unpackaged-target=true"], ["--require-unpackaged-target", "1"]]
        bad.extend([["--require-unpackaged-target"[:n]] for n in range(3, len("--require-unpackaged-target"))])
        for flags in bad:
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                T.parser().parse_args([*base, *flags])

    def test_package_gate_invalid_type_rejected_before_create(self):
        for value in (1, "true", None):
            args, _ = self.case()
            args.require_unpackaged_target = value
            self.rejected(args)

    def fake_package_api(self, mode, calls):
        def api(handle, length, output):
            calls.append(handle)
            count = C.cast(length, C.POINTER(T.U32))
            if mode == "unpackaged":
                return 15700
            if mode == "error":
                return 5
            if output is None:
                count.contents.value = 4097 if mode == "oversized" else len("P1.Original.Package_1.0_x86_fixture") + 1
                return 122
            if mode == "changed":
                return 15700
            output.value = "P1.Original.Package_1.0_x86_fixture"
            return 0
        return api

    def test_package_identity_api_handles_packaged_unpacked_unknown_bounded(self):
        for mode, expected, codes in (("unpackaged", "unpackaged", [15700]),
            ("packaged", "packaged", [122, 0]), ("error", "unknown", [5]),
            ("oversized", "unknown", [122]), ("changed", "unknown", [122, 15700])):
            calls = []
            win = T.Windows.__new__(T.Windows)
            win.k = SimpleNamespace(GetPackageFullName=self.fake_package_api(mode, calls))
            identity = win.package_identity(0x123456789ABC)
            self.assertEqual(identity["state"], expected)
            self.assertEqual(identity["returnCodes"], codes)
            self.assertEqual(calls, [0x123456789ABC] * len(codes))
            self.assertEqual(identity["packageFullName"], "P1.Original.Package_1.0_x86_fixture" if expected == "packaged" else None)
        win.k = SimpleNamespace()
        identity = win.package_identity(0x123456789ABC)
        self.assertEqual(identity["state"], "unknown")
        self.assertEqual(identity["returnCodes"], [])
        self.assertIn("error", identity)

    def test_package_identity_is_recorded_from_actual_suspended_target(self):
        args, root = self.case()
        result, _ = self.successful(args)
        identity = json.loads((root / "trace" / "target-package.local.json").read_text())
        self.assertEqual(identity, result["targetPackageIdentity"])
        self.assertEqual(identity["processIdentity"], result["process"])
        self.assertIn(identity["state"], ("packaged", "unpackaged"))
        self.assertTrue(identity["beforeResume"] and identity["originalProcessHandle"])
        self.assertFalse(result["requireUnpackagedTarget"])

    def test_package_gate_rejects_identity_and_api_error_before_resume(self):
        original = T.Windows
        for mode in ("packaged", "error", "missing"):
            args, root = self.case()
            args.require_unpackaged_target = True
            calls, resumed = [], []
            fake = self.fake_package_api(mode, calls)
            class RefusedPackage(original):
                def __init__(self):
                    super().__init__()
                    self.k.GetPackageFullName = fake
                    resume = self.k.ResumeThread
                    def resumed_thread(handle):
                        resumed.append(handle)
                        return resume(handle)
                    self.k.ResumeThread = resumed_thread
                def package_identity(self, process):
                    if mode == "missing":
                        # 模拟API不存在；查询方法必须把它留为unknown，不能默认为unpackaged。
                        def unavailable(*_args):
                            raise AttributeError("original fixture unavailable GetPackageFullName")
                        self.k.GetPackageFullName = unavailable
                    return super().package_identity(process)
            T.Windows = RefusedPackage
            try:
                result = T.run_trace(args)
            finally:
                T.Windows = original
            self.assertFalse(result["traceCompleted"])
            self.assertTrue(result["targetExitObserved"] and result["ownTargetSignaledAfterJobClose"], result)
            self.assertEqual(resumed, [])
            self.assertEqual(result["targetPackageIdentity"]["state"], "packaged" if mode == "packaged" else "unknown")
            self.assertTrue((root / "trace" / "target-package.local.json").is_file())
            events = [json.loads(line) for line in (root / "trace" / "events.local.jsonl").read_text().splitlines()]
            self.assertTrue(all(row["event"] == "FAILURE_DRAIN" and row["code"] != 8 for row in events))
            self.assertEqual(result["jobBeforeClose"]["unresolvedAssociatedPids"], [])


def main():
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-directory", required=True)
    args = parser.parse_args()
    ROOT = T.N.plain(args.evidence_directory)
    T.N.require(not ROOT.exists() and ROOT.parent.is_dir(), "原创测试证据目录必须全新")
    ROOT.mkdir()
    started = time.monotonic()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TraceTests))
    summary = {"schema": "p1-loader-trace-original-tests-v1", "tests": result.testsRun,
               "failures": len(result.failures), "errors": len(result.errors),
               "elapsedMs": round((time.monotonic() - started) * 1000), "syntheticFixtureOnly": True,
               "realLoaderStarted": False, "registryWritten": False, "debuggerInvoked": False,
               "sourceFiles": {name: T.N.file_identity(HERE / name) for name in ("p1-loader-trace.py", "p1-loader-trace-tests.py")}}
    with (ROOT / "summary.local.json").open("x", encoding="utf-8") as stream:
        json.dump(summary, stream, ensure_ascii=True, indent=2)
    print(json.dumps(summary, ensure_ascii=True))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
