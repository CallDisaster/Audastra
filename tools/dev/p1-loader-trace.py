"""只跟踪明确 SHA 绑定、由本工具新建的 OWN PE32 进程；不 attach 作者进程。"""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


_spec = importlib.util.spec_from_file_location("p1_native_loader", Path(__file__).with_name("p1-native-loader.py"))
N = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(N)
U16, U32, U64, PTR = C.c_uint16, C.c_uint32, C.c_uint64, C.c_void_p
SIZE = C.c_size_t
MAX_FILE_BYTES = 128 * 1024 * 1024
MAX_MAP_BYTES = 512 * 1024 * 1024
MAP_HASH_CHUNK_BYTES = 1024 * 1024
MAX_REMOTE_BYTES = 2 * 1024 * 1024
MAX_EVENTS = 20000
MAX_DEBUG_BYTES = 4 * 1024 * 1024
KILL_CODE = 0xE0500001


class FILETIME(C.Structure):
    _fields_ = [("low", U32), ("high", U32)]


class FILEINFO(C.Structure):
    _fields_ = [("attributes", U32), ("created", FILETIME), ("accessed", FILETIME),
                ("written", FILETIME), ("volume", U32), ("sizeHigh", U32),
                ("sizeLow", U32), ("links", U32), ("indexHigh", U32), ("indexLow", U32)]


class STARTUPINFO(C.Structure):
    _fields_ = [("cb", U32), ("reserved", C.c_wchar_p), ("desktop", C.c_wchar_p),
                ("title", C.c_wchar_p), ("x", U32), ("y", U32), ("xsize", U32),
                ("ysize", U32), ("xchars", U32), ("ychars", U32), ("fill", U32),
                ("flags", U32), ("show", U16), ("reservedSize", U16),
                ("reservedBytes", PTR), ("stdin", PTR), ("stdout", PTR), ("stderr", PTR)]


class PROCESSINFO(C.Structure):
    _fields_ = [("process", PTR), ("thread", PTR), ("pid", U32), ("tid", U32)]


class EXCEPTIONRECORD(C.Structure):
    _fields_ = [("code", U32), ("flags", U32), ("record", PTR), ("address", PTR),
                ("parameterCount", U32), ("parameters", SIZE * 15)]


class EXCEPTIONINFO(C.Structure):
    _fields_ = [("record", EXCEPTIONRECORD), ("firstChance", U32)]


class CREATEPROCESS(C.Structure):
    _fields_ = [("file", PTR), ("process", PTR), ("thread", PTR), ("base", PTR),
                ("debugOffset", U32), ("debugSize", U32), ("tls", PTR),
                ("start", PTR), ("imageName", PTR), ("unicode", U16)]


class CREATETHREAD(C.Structure):
    _fields_ = [("thread", PTR), ("tls", PTR), ("start", PTR)]


class LOADDLL(C.Structure):
    _fields_ = [("file", PTR), ("base", PTR), ("debugOffset", U32),
                ("debugSize", U32), ("imageName", PTR), ("unicode", U16)]


class DEBUGSTRING(C.Structure):
    _fields_ = [("address", PTR), ("unicode", U16), ("length", U16)]


class DEBUGUNION(C.Union):
    _fields_ = [("exception", EXCEPTIONINFO), ("createProcess", CREATEPROCESS),
                ("createThread", CREATETHREAD), ("loadDll", LOADDLL),
                ("unloadBase", PTR), ("debugString", DEBUGSTRING),
                ("exitCode", U32), ("rip", U32 * 2)]


class DEBUGEVENT(C.Structure):
    _fields_ = [("code", U32), ("pid", U32), ("tid", U32), ("info", DEBUGUNION)]


class BASICINFO(C.Structure):
    _fields_ = [("reserved1", PTR), ("peb", PTR), ("reserved2", PTR * 2),
                ("pid", SIZE), ("reserved3", PTR)]


class MEMORYINFO(C.Structure):
    _fields_ = [("base", PTR), ("allocationBase", PTR), ("allocationProtect", U32),
                ("partition", U16), ("regionSize", SIZE), ("state", U32),
                ("protect", U32), ("type", U32)]


class JOBBASIC(C.Structure):
    _fields_ = [("processTime", C.c_int64), ("jobTime", C.c_int64), ("flags", U32),
                ("minWorkingSet", SIZE), ("maxWorkingSet", SIZE), ("activeLimit", U32),
                ("affinity", SIZE), ("priority", U32), ("scheduling", U32)]


class JOBEXT(C.Structure):
    _fields_ = [("basic", JOBBASIC), ("io", U64 * 6), ("processMemory", SIZE),
                ("jobMemory", SIZE), ("peakProcessMemory", SIZE), ("peakJobMemory", SIZE)]


def ft(value):
    return (value.high << 32) | value.low


def canonical(value):
    return os.path.normcase(os.path.normpath(value))


def native_path(value):
    N.require(value.startswith("\\\\?\\") and not value.startswith("\\\\?\\UNC\\"),
              "实际文件句柄路径不是本地绝对 DOS 路径")
    return value[4:]


class Windows:
    def __init__(self):
        N.require(sys.platform == "win32" and C.sizeof(PTR) == 8,
                  "collector 只支持 Windows AMD64 Python，目标须为 PE32 I386")
        N.require(C.sizeof(DEBUGEVENT) == 176 and DEBUGEVENT.info.offset == 16 and
                  C.sizeof(CREATEPROCESS) == 72 and C.sizeof(LOADDLL) == 40 and
                  C.sizeof(JOBEXT) == 144 and C.sizeof(MEMORYINFO) == 48,
                  "ctypes 与已核验的 native AMD64 SDK 布局不符")
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.n = C.WinDLL("ntdll", use_last_error=True)
        signatures = {
            "CloseHandle": ([PTR], C.c_int),
            "CreateFileW": ([C.c_wchar_p, U32, U32, PTR, U32, U32, PTR], PTR),
            "GetFileInformationByHandle": ([PTR, C.POINTER(FILEINFO)], C.c_int),
            "GetFinalPathNameByHandleW": ([PTR, C.c_wchar_p, U32, U32], U32),
            "GetFileSizeEx": ([PTR, C.POINTER(C.c_int64)], C.c_int),
            "SetFilePointerEx": ([PTR, C.c_int64, C.POINTER(C.c_int64), U32], C.c_int),
            "ReadFile": ([PTR, PTR, U32, C.POINTER(U32), PTR], C.c_int),
            "CreateProcessW": ([C.c_wchar_p, C.c_wchar_p, PTR, PTR, C.c_int, U32,
                                 PTR, C.c_wchar_p, C.POINTER(STARTUPINFO), C.POINTER(PROCESSINFO)], C.c_int),
            "CreateJobObjectW": ([PTR, C.c_wchar_p], PTR),
            "SetInformationJobObject": ([PTR, C.c_int, PTR, U32], C.c_int),
            "QueryInformationJobObject": ([PTR, C.c_int, PTR, U32, C.POINTER(U32)], C.c_int),
            "AssignProcessToJobObject": ([PTR, PTR], C.c_int),
            "TerminateJobObject": ([PTR, U32], C.c_int),
            "TerminateProcess": ([PTR, U32], C.c_int),
            "ResumeThread": ([PTR], U32),
            "GetProcessId": ([PTR], U32),
            "GetProcessTimes": ([PTR, C.POINTER(FILETIME), C.POINTER(FILETIME),
                                  C.POINTER(FILETIME), C.POINTER(FILETIME)], C.c_int),
            "QueryFullProcessImageNameW": ([PTR, U32, C.c_wchar_p, C.POINTER(U32)], C.c_int),
            "IsWow64Process2": ([PTR, C.POINTER(U16), C.POINTER(U16)], C.c_int),
            "WaitForDebugEventEx": ([C.POINTER(DEBUGEVENT), U32], C.c_int),
            "ContinueDebugEvent": ([U32, U32, U32], C.c_int),
            "ReadProcessMemory": ([PTR, PTR, PTR, SIZE, C.POINTER(SIZE)], C.c_int),
            "WriteProcessMemory": ([PTR, PTR, PTR, SIZE, C.POINTER(SIZE)], C.c_int),
            "VirtualQueryEx": ([PTR, PTR, C.POINTER(MEMORYINFO), SIZE], SIZE),
            "SetErrorMode": ([U32], U32),
            "WaitForSingleObject": ([PTR, U32], U32),
            "GetSystemDirectoryW": ([C.c_wchar_p, U32], U32),
            "GetSystemWow64DirectoryW": ([C.c_wchar_p, U32], U32),
        }
        for name, (args, result) in signatures.items():
            api = getattr(self.k, name)
            api.argtypes, api.restype = args, result
        self.n.NtQueryInformationProcess.argtypes = [PTR, U32, PTR, U32, C.POINTER(U32)]
        self.n.NtQueryInformationProcess.restype = C.c_int32
        self.system_directories = {}
        for kind, function in (("native64", self.k.GetSystemDirectoryW), ("wow64", self.k.GetSystemWow64DirectoryW)):
            # 32768 WCHAR 恰超过 UNICODE_STRING 16-bit 字节预算；本机 Wow64
            # API 在该容量返回 required count 但 buffer 为空。使用 <=65534 bytes。
            buffer = C.create_unicode_buffer(32767)
            count = function(buffer, len(buffer))
            N.require(0 < count < len(buffer) and count == len(buffer.value), "无法确定 Windows 实际系统目录")
            self.system_directories[kind] = buffer.value

    def check(self, result, label):
        if not result:
            raise OSError(C.get_last_error(), label)
        return result

    def close(self, handle):
        if handle and handle != C.c_void_p(-1).value:
            self.check(self.k.CloseHandle(handle), "CloseHandle")

    def package_identity(self, process):
        """用CreateProcess原hProcess在Resume前查询；仅15700证明没有package身份。"""
        row = {"api": "GetPackageFullName", "state": "unknown", "packageFullName": None,
               "returnCodes": [], "originalProcessHandle": True, "beforeResume": True}
        try:
            api = self.k.GetPackageFullName
            api.argtypes, api.restype = [PTR, C.POINTER(U32), C.c_wchar_p], C.c_int32
            length = U32()
            code = int(api(process, C.byref(length), None))
            row["returnCodes"].append(code)
            if code == 15700:  # APPMODEL_ERROR_NO_PACKAGE；不是GetLastError。
                row["state"] = "unpackaged"
            elif code == 122 and 0 < length.value <= 4096:
                capacity = length.value
                name = C.create_unicode_buffer(capacity)
                code = int(api(process, C.byref(length), name))
                row["returnCodes"].append(code)
                if code == 0 and 1 < length.value <= capacity and len(name.value) + 1 == length.value:
                    row.update(state="packaged", packageFullName=name.value)
                else:
                    row["error"] = "package身份回读失败或返回长度不一致"
            else:
                row["error"] = "package身份API错误或容量超限"
        except Exception as error:
            row["error"] = type(error).__name__ + ": " + str(error)
        return row

    def file_info(self, handle):
        info = FILEINFO()
        self.check(self.k.GetFileInformationByHandle(handle, C.byref(info)), "file information")
        return {"volume": info.volume, "fileId": (info.indexHigh << 32) | info.indexLow,
                "bytes": (info.sizeHigh << 32) | info.sizeLow, "links": info.links,
                "attributes": info.attributes, "writtenFiletime": ft(info.written)}

    def file_path(self, handle):
        buf = C.create_unicode_buffer(32768)
        count = self.k.GetFinalPathNameByHandleW(handle, buf, len(buf), 0)
        N.require(0 < count < len(buf), "无法读取完整实际文件句柄路径")
        return native_path(buf.value)

    def file_bytes(self, handle, limit=MAX_FILE_BYTES):
        size = C.c_int64()
        self.check(self.k.GetFileSizeEx(handle, C.byref(size)), "file size")
        N.require(0 <= size.value <= limit, "文件超过捕获预算")
        self.check(self.k.SetFilePointerEx(handle, 0, None, 0), "rewind file")
        pieces, remaining = [], size.value
        while remaining:
            amount = min(1024 * 1024, remaining)
            buffer, read = C.create_string_buffer(amount), U32()
            self.check(self.k.ReadFile(handle, buffer, amount, C.byref(read), None), "ReadFile")
            N.require(0 < read.value <= amount, "读取文件提前结束")
            pieces.append(buffer.raw[:read.value])
            remaining -= read.value
        return b"".join(pieces)

    def observed_file(self, handle):
        N.require(handle, "debug event 没有提供文件句柄；实际路径 unknown")
        before = self.file_info(handle)
        path, data = self.file_path(handle), self.file_bytes(handle)
        N.require(before == self.file_info(handle), "实际文件在句柄读取期间发生变化")
        result = {"path": path, **before, "sha256": N.sha_bytes(data), "machine": None}
        if len(data) >= 64 and data[:2] == b"MZ":
            off = struct.unpack_from("<I", data, 0x3C)[0]
            if off + 6 <= len(data) and data[off:off + 4] == b"PE\0\0":
                result["machine"] = hex(struct.unpack_from("<H", data, off + 4)[0])
                if off + 84 <= len(data):
                    result["imageSize"] = struct.unpack_from("<I", data, off + 80)[0]
        return result

    def map_digest(self, handle):
        """地图只计算身份；同一锁定句柄按固定块读取，不缓存完整原始字节。"""
        size = C.c_int64()
        self.check(self.k.GetFileSizeEx(handle, C.byref(size)), "map file size")
        N.require(0 <= size.value <= MAX_MAP_BYTES, "地图超过流式身份预算")
        self.check(self.k.SetFilePointerEx(handle, 0, None, 0), "rewind map file")
        buffer = C.create_string_buffer(MAP_HASH_CHUNK_BYTES)
        view = memoryview(buffer).cast("B")
        digest, total = hashlib.sha256(), 0
        while total < size.value:
            amount, read = min(MAP_HASH_CHUNK_BYTES, size.value - total), U32()
            self.check(self.k.ReadFile(handle, buffer, amount, C.byref(read), None), "ReadFile map")
            N.require(0 < read.value <= amount, "读取地图提前结束")
            digest.update(view[:read.value])
            total += read.value
        return {"bytes": total, "sha256": digest.hexdigest()}

    def read_memory(self, process, address, length):
        N.require(address and 0 <= length <= MAX_REMOTE_BYTES, "远程指针为空或读取超过预算")
        if not length:
            return b""
        buffer, read = C.create_string_buffer(length), SIZE()
        self.check(self.k.ReadProcessMemory(process, address, buffer, length, C.byref(read)), "ReadProcessMemory")
        N.require(read.value == length, "远程读取不完整")
        return buffer.raw

    def pointer(self, process, address, width=8):
        return int.from_bytes(self.read_memory(process, address, width), "little")

    def process_identity(self, process, pid, image):
        N.require(self.k.GetProcessId(process) == pid, "新建 process handle 的 PID 不符")
        buf, length = C.create_unicode_buffer(32768), U32(32768)
        self.check(self.k.QueryFullProcessImageNameW(process, 0, buf, C.byref(length)), "process image path")
        N.require(canonical(buf.value) == canonical(str(image)), "实际 process image 路径不符")
        times = [FILETIME() for _ in range(4)]
        self.check(self.k.GetProcessTimes(process, *(C.byref(v) for v in times)), "process creation time")
        pm, nm = U16(), U16()
        self.check(self.k.IsWow64Process2(process, C.byref(pm), C.byref(nm)), "process architecture")
        N.require(pm.value == 0x14C and nm.value == 0x8664, "目标不是 AMD64 上的 OWN WOW64 I386")
        return {"pid": pid, "imagePath": buf.value, "creationFiletime": ft(times[0]),
                "processMachine": hex(pm.value), "nativeMachine": hex(nm.value)}


class Locks:
    """保留目录祖先及绑定输入句柄，拒绝 reparse/hardlink 和运行期间替换。"""
    def __init__(self, win):
        self.win, self.handles, self.directories = win, [], set()

    def directory(self, path):
        path = N.plain(path)
        for current in reversed((path, *path.parents)):
            key = canonical(str(current))
            if key in self.directories:
                continue
            h = self.win.k.CreateFileW(str(current), 0x80, 3, None, 3, 0x02200000, None)
            N.require(h != C.c_void_p(-1).value, "不能锁定普通目录祖先")
            self.handles.append(h)
            info = self.win.file_info(h)
            N.require(info["attributes"] & 0x10 and not info["attributes"] & 0x400,
                      "目录祖先是 reparse 或不是目录")
            N.require(canonical(self.win.file_path(h)) == key, "目录祖先实际句柄路径不符")
            self.directories.add(key)

    def _open_file(self, path):
        path = N.plain(path)
        self.directory(path.parent)
        h = self.win.k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x00200000, None)
        N.require(h != C.c_void_p(-1).value, "不能锁定输入文件")
        self.handles.append(h)
        before = self.win.file_info(h)
        N.require(not before["attributes"] & 0x410 and before["links"] == 1, "输入是目录、reparse 或 hardlink")
        N.require(canonical(self.win.file_path(h)) == canonical(str(path)), "输入实际句柄路径不符")
        return path, h, before

    def file(self, path, expected):
        path, h, before = self._open_file(path)
        raw = self.win.file_bytes(h, N.MAX_JSON_BYTES if path.suffix.lower() == ".json" else MAX_FILE_BYTES)
        N.require(before == self.win.file_info(h) and N.sha_bytes(raw) == N.expected_sha(expected),
                  "输入 SHA 或运行前身份不符")
        return path, raw, {"path": str(path), **before, "sha256": N.sha_bytes(raw)}

    def map_file(self, path, expected):
        path, h, before = self._open_file(path)
        digest = self.win.map_digest(h)
        N.require(before == self.win.file_info(h) and before["bytes"] == digest["bytes"] and
                  digest["sha256"] == N.expected_sha(expected), "地图 SHA 或运行前身份不符")
        return path, {"path": str(path), **before, "sha256": digest["sha256"]}

    def close(self):
        errors = []
        for h in reversed(self.handles):
            try:
                self.win.close(h)
            except Exception as error:
                errors.append(str(error))
        self.handles.clear()
        if errors:
            raise OSError("input locks close errors: " + "; ".join(errors))


class Evidence:
    def __init__(self, path):
        self.path = path
        path.mkdir()
        N.plain(path)
        self.events = (path / "events.local.jsonl").open("xb")
        self.count, self.debug_bytes = 0, 0

    def json(self, name, value):
        with (self.path / name).open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=True, indent=2, allow_nan=False)
            stream.write("\n")

    def raw(self, name, raw):
        with (self.path / name).open("xb") as stream:
            stream.write(raw)
        return {"path": str(self.path / name), "bytes": len(raw), "sha256": N.sha_bytes(raw)}

    def event(self, row):
        N.require(self.count < MAX_EVENTS, "debug event 数超过预算")
        self.events.write((json.dumps(row, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8"))
        self.events.flush()
        self.count += 1


def preflight(args, win, locks):
    N.require(type(getattr(args, "require_unpackaged_target", False)) is bool,
              "require-unpackaged-target必须显式布尔值")
    image, image_raw, image_id = locks.file(args.image, args.image_sha256)
    config, config_raw, config_id = locks.file(args.config, args.config_sha256)
    prepared_path, prepared_raw, prepared_id = locks.file(args.prepared, args.prepared_sha256)
    map_path, map_id = locks.map_file(args.map, args.map_sha256)
    prepared = json.loads(prepared_raw.decode("utf-8-sig"))
    N.require(isinstance(prepared, dict) and prepared.get("schema") == "p1-native-loader-v1" and
              prepared.get("status") == "prepared" and prepared.get("sourceUnchanged") is True,
              "prepared receipt 未签收 prepared/sourceUnchanged")
    game, host, cwd = N.plain(prepared["gameRoot"]), N.plain(prepared["hostRoot"]), N.plain(args.cwd)
    N.require(game.is_dir() and host.is_dir() and cwd == host and game != host and host not in game.parents,
              "game/host/cwd 身份不符")
    locks.directory(cwd)
    prefix = N.plain(host / "bin")
    N.require(prefix.is_dir(), "host/bin PATH 前缀不存在")
    locks.directory(prefix)
    for path, identity, key in ((image, image_id, "loader"), (config, config_id, "config")):
        bound = prepared.get(key, {})
        N.require(isinstance(bound, dict) and path == game / N.relative(bound.get("relativePath")) and
                  identity["bytes"] == bound.get("bytes") and identity["sha256"] == N.expected_sha(bound.get("sha256")),
                  "实际 image/config 不匹配 prepared receipt")
    N.require(config.name == image.name + ".config" and config_raw == N.EDITOR_LOADER_CONFIG,
              "config 不是已审查的固定 loader 配置")
    N.require(len(image_raw) >= 64 and image_raw[:2] == b"MZ", "目标不是 PE")
    off = struct.unpack_from("<I", image_raw, 0x3C)[0]
    N.require(off + 26 <= len(image_raw) and image_raw[off:off + 4] == b"PE\0\0" and
              struct.unpack_from("<H", image_raw, off + 4)[0] == 0x14C and
              struct.unpack_from("<H", image_raw, off + 24)[0] == 0x10B, "目标不是 PE32 I386")
    argv = json.loads(args.argv_json)
    N.require(isinstance(argv, list) and len(argv) == 3 and all(isinstance(v, str) and v and "\x00" not in v for v in argv)
              and argv == [str(image), "-loadfile", str(map_path)], "argv 必须精确为绑定 image/-loadfile/绝对候选图")
    N.require(args.process_name == "ydwe", "process-name 必须明确为 ydwe")
    N.require(1 <= args.timeout_seconds <= 300, "timeout 必须在 1..300 秒")
    evidence = N.plain(args.evidence)
    N.require(not evidence.exists() and evidence.parent.is_dir(), "证据目录必须不存在且父目录普通存在")
    N.require(N.disjoint(evidence, game) and N.disjoint(evidence, host) and
              all(N.disjoint(evidence, p) for p in (prepared_path, map_path)), "证据输出不能位于绑定目标或输入目录")
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.lower() in ("path", "ydwe-process-name"):
            del environment[key]
    environment["PATH"] = str(prefix) + os.pathsep + os.environ.get("PATH", "")
    environment["ydwe-process-name"] = "ydwe"
    commandline = subprocess.list2cmdline(argv)
    N.require(len(commandline) < 32767 and all("\x00" not in k + v for k, v in environment.items()), "环境或 argv 含 NUL/超限")
    return {"image": image, "cwd": cwd, "argv": argv, "commandline": commandline,
            "environment": environment, "evidence": evidence, "bindings": {
                "image": image_id, "config": config_id, "prepared": prepared_id, "map": map_id,
                "cwd": str(cwd), "argv": argv, "pathPrefix": str(prefix), "processName": "ydwe",
                "dependenciesInventory": "root guard owns complete before/after inventories; collector does not repeat them"}}


def unicode_field(win, process, descriptor, evidence, name, width=8):
    raw = win.read_memory(process, descriptor, 16 if width == 8 else 8)
    length, maximum = struct.unpack_from("<HH", raw)
    address = int.from_bytes(raw[8:16] if width == 8 else raw[4:8], "little")
    N.require(length % 2 == 0 and length <= maximum <= 65534, "UNICODE_STRING 长度不符")
    content = win.read_memory(process, address, length) if length else b""
    return content.decode("utf-16-le", "strict"), evidence.raw(name + ".utf16le.bin", content)


def selected_environment(win, process, address, evidence):
    pieces, total, end = [], 0, None
    # 按可读提交区域采集有界环境块，只保存明确选中的两个键；不落盘完整环境。
    while total < MAX_REMOTE_BYTES:
        region = MEMORYINFO()
        N.require(win.k.VirtualQueryEx(process, address + total, C.byref(region), C.sizeof(region)) == C.sizeof(region),
                  "无法确定实际环境的可读内存区域")
        N.require(region.state == 0x1000 and not region.protect & 0x101, "实际环境内存不可读")
        amount = min(4096, MAX_REMOTE_BYTES - total, int(region.base) + region.regionSize - address - total)
        N.require(amount > 0, "环境区域无进展")
        pieces.append(win.read_memory(process, address + total, amount))
        total += amount
        blob = b"".join(pieces)
        # UTF-16 对齐，避免在 ASCII 字符高位与 NUL 之间误判。
        end = next((i for i in range(max(0, total - amount - 4) // 2 * 2, len(blob) - 3, 2)
                    if blob[i:i + 4] == b"\x00\x00\x00\x00"), None)
        if end is not None:
            break
    N.require(end is not None, "实际环境缺少双 NUL 或超过 2 MiB")
    selected = {}
    for entry in blob[:end].decode("utf-16-le", "strict").split("\x00"):
        if "=" in entry and not entry.startswith("="):
            key, value = entry.split("=", 1)
            if key.lower() in ("path", "ydwe-process-name"):
                N.require(key.lower() not in selected, "实际环境选定键重复")
                raw = entry.encode("utf-16-le")
                selected[key.lower()] = {"value": value,
                    "raw": evidence.raw("actual-env-" + key.lower() + ".utf16le.bin", raw)}
    return selected


def actual_parameters(win, process, pid, base, launch, evidence):
    basic, returned = BASICINFO(), U32()
    status = win.n.NtQueryInformationProcess(process, 0, C.byref(basic), C.sizeof(basic), C.byref(returned))
    N.require(status == 0 and returned.value == C.sizeof(basic) and basic.pid == pid and basic.peb,
              "实际 native ProcessBasicInformation/PID 不符")
    peb = int(basic.peb)
    N.require(win.pointer(process, peb + 0x10) == base and win.read_memory(process, peb + 2, 1) == b"\x01",
              "native PEB image base/debugged 标志不符")
    params = win.pointer(process, peb + 0x20)
    N.require(params, "实际 native PEB ProcessParameters 为空")
    image, image_raw = unicode_field(win, process, params + 0x60, evidence, "actual-image")
    commandline, command_raw = unicode_field(win, process, params + 0x70, evidence, "actual-commandline")
    cwd, cwd_raw = unicode_field(win, process, params + 0x38, evidence, "actual-cwd")
    environment = selected_environment(win, process, win.pointer(process, params + 0x80), evidence)
    N.require(canonical(image) == canonical(str(launch["image"])) and commandline == launch["commandline"] and
              canonical(cwd) == canonical(str(launch["cwd"])), "实际 native PEB image/argv/cwd 不符")
    N.require(environment.get("path", {}).get("value") == launch["environment"]["PATH"] and
              environment.get("ydwe-process-name", {}).get("value") == "ydwe", "实际 child PATH/process-name 不符")
    return {"nativePeb": peb, "parameters": params, "imagePath": image, "commandline": commandline,
            "cwd": cwd, "selectedEnvironment": environment,
            "raw": {"image": image_raw, "commandline": command_raw, "cwd": cwd_raw},
            "layout": "native AMD64 PEB/RTL parameters; SDK image/command fields plus internal cwd/environment offsets; validated against exact expected values, may change by OS"}


def owned_loader_snaps(win, process, pid, base, actual, result, evidence):
    """唯一可选目标写入；未公开的偏移须 explicit opt-in 且在 OWN 首次执行前逐项核验。"""
    wow, returned = SIZE(), U32()
    status = win.n.NtQueryInformationProcess(process, 26, C.byref(wow), C.sizeof(wow), C.byref(returned))
    N.require(status == 0 and returned.value == C.sizeof(wow) and 0 < wow.value <= 0xFFFFFFFF,
              "无法取得并约束 internal WOW64 PEB32 候选")
    # 官方 class26 只承诺 WOW64 非零；把返回值当 PEB32 是本机内部布局假设。
    peb32 = wow.value
    N.require(win.pointer(process, peb32 + 8, 4) == base and win.read_memory(process, peb32 + 2, 1) == b"\x01",
              "internal PEB32 候选 image base/debugged 交叉核验失败")
    p32 = win.pointer(process, peb32 + 0x10, 4)
    N.require(p32 and win.pointer(process, actual["nativePeb"] + 0x20) == actual["parameters"] and
              win.k.GetProcessId(process) == pid, "OWN PEB process 身份失效")
    p32_image, _ = unicode_field(win, process, p32 + 0x38, evidence, "snaps-wow32-image", 4)
    p32_command, _ = unicode_field(win, process, p32 + 0x40, evidence, "snaps-wow32-commandline", 4)
    N.require(canonical(p32_image) == canonical(actual["imagePath"]) and p32_command == actual["commandline"],
              "internal PEB32 parameters 与已验证 native 身份不符")
    channels = [("native64", actual["nativePeb"] + 0xBC), ("wow64-internal32", peb32 + 0x68)]
    rows = [{"kind": kind, "address": address, "before": win.read_memory(process, address, 4)}
            for kind, address in channels]
    result.update(flag="FLG_SHOW_LDR_SNAPS (0x2)",
                  layout="internal NtGlobalFlag offsets; not a stable public SDK contract", channels=[])
    evidence.json("loader-snaps-before.local.json", {"pid": pid, "beforeFirstUserExecution": True,
        "channels": [{"kind": r["kind"], "address": r["address"], "beforeHex": r["before"].hex()} for r in rows]})
    for row in rows:
        before = int.from_bytes(row["before"], "little")
        after = before | 2
        record = {"kind": row["kind"], "address": row["address"], "before": before,
                  "requestedAfter": after, "beforeHex": row["before"].hex(), "writeAttempted": True}
        result["channels"].append(record)
        buffer, written = C.create_string_buffer(struct.pack("<I", after)), SIZE()
        win.check(win.k.WriteProcessMemory(process, row["address"], buffer, 4, C.byref(written)), "OWN loader snaps write")
        raw_after = win.read_memory(process, row["address"], 4)
        N.require(written.value == 4 and raw_after == struct.pack("<I", after), "OWN loader snaps 回读不符")
        record.update(after=after, readbackHex=raw_after.hex(), verified=True)
    result["applied"] = True
    return result


def capture_string(win, process, value, evidence):
    count = int(value.length)
    N.require(0 < count <= 32768 and evidence.debug_bytes + count * 3 <= MAX_DEBUG_BYTES,
              "debug string 空长度、单条或总字节超过预算")
    reported = win.read_memory(process, value.address, count)
    row = {"unicode": bool(value.unicode), "reportedLength": count,
           "reportedRaw": evidence.raw(f"debug-{evidence.count:05}-reported.bin", reported)}
    evidence.debug_bytes += len(reported)
    if value.unicode:
        # 文档称 length 为 bytes，部分系统报告 UTF16 code units。保留报告长度的原 bytes，
        # 仅为有界 UTF16 文本额外读取到 2*length；不抹掉这项文档/本机约定差异。
        raw = reported
        end = next((i for i in range(0, len(raw) - 1, 2) if raw[i:i + 2] == b"\x00\x00"), None)
        extended = end is None
        if extended:
            raw = win.read_memory(process, value.address, count * 2)
        end = next((i for i in range(0, len(raw) - 1, 2) if raw[i:i + 2] == b"\x00\x00"), None)
        N.require(end is not None, "Unicode debug string 有界读取内没有终止符")
        raw = raw[:end + 2]
        evidence.debug_bytes += len(raw)
        N.require(evidence.debug_bytes <= MAX_DEBUG_BYTES, "debug string 总字节超过预算")
        row.update(boundedRaw=evidence.raw(f"debug-{evidence.count:05}-utf16.bin", raw),
                   boundedReadLimit=count * 2 if extended else count, extensionNeeded=extended,
                   terminatedBytes=len(raw), text=raw[:-2].decode("utf-16-le", "strict"))
    else:
        row.update(text=None, encoding="ANSI original bytes retained; no locale/UTF8 guess")
    return row


def run_trace(args):
    win, evidence = Windows(), None
    locks, pi, job = Locks(win), PROCESSINFO(), None
    pending, exited, assigned = None, False, False
    result = {"schema": "p1-owned-loader-debug-trace-v1", "traceCompleted": False,
              "nativeSaveValidated": False, "runtimeValidated": False, "errors": [],
              "startedUnixNs": time.time_ns(), "actualExitCode": None,
              "loaderSnaps": {"requested": bool(args.owned_loader_snaps), "applied": False},
              "os": list(sys.getwindowsversion()), "debugOnlyThisProcess": True,
              "attachUsed": False, "memoryWritesAllowed": bool(args.owned_loader_snaps)}
    result["requireUnpackagedTarget"] = getattr(args, "require_unpackaged_target", False)
    try:
        launch = preflight(args, win, locks)
        evidence = Evidence(launch["evidence"])
        locks.directory(evidence.path)
        evidence.json("inputs.local.json", {"schema": "p1-owned-trace-inputs-v1", **launch["bindings"],
                      "collector": N.file_identity(Path(__file__)), "timeoutSeconds": args.timeout_seconds,
                      "helper": N.file_identity(Path(N.__file__)), "systemDirectories": win.system_directories,
                      "ownedLoaderSnapsOptIn": args.owned_loader_snaps,
                      "requireUnpackagedTarget": result["requireUnpackagedTarget"],
                      "errorMode": "child inherits process-only SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX; collector previous mode restored immediately"})
        job = win.check(win.k.CreateJobObjectW(None, None), "create OWN job")
        limit = JOBEXT()
        limit.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        win.check(win.k.SetInformationJobObject(job, 9, C.byref(limit), C.sizeof(limit)), "job kill-on-close")
        si = STARTUPINFO()
        si.cb, si.flags, si.show = C.sizeof(si), 1, 0  # STARTF_USESHOWWINDOW, SW_HIDE
        command = C.create_unicode_buffer(launch["commandline"])
        env_text = "\x00".join(k + "=" + v for k, v in sorted(launch["environment"].items(), key=lambda kv: kv[0].casefold())) + "\x00\x00"
        env_buffer = C.create_unicode_buffer(env_text)
        previous_mode = win.k.SetErrorMode(0x8003)
        try:
            win.check(win.k.CreateProcessW(str(launch["image"]), command, None, None, False,
                        0x2 | 0x4 | 0x400, env_buffer, str(launch["cwd"]), C.byref(si), C.byref(pi)), "create OWN debug target")
        finally:
            win.k.SetErrorMode(previous_mode)
        result["process"] = win.process_identity(pi.process, pi.pid, launch["image"])
        win.check(win.k.AssignProcessToJobObject(job, pi.process), "assign OWN suspended process to job")
        assigned = True
        result["targetPackageIdentity"] = win.package_identity(pi.process)
        result["targetPackageIdentity"]["processIdentity"] = result["process"]
        evidence.json("target-package.local.json", result["targetPackageIdentity"])
        if result["requireUnpackagedTarget"]:
            N.require(result["targetPackageIdentity"]["state"] == "unpackaged"
                      and result["targetPackageIdentity"]["returnCodes"] == [15700],
                      "目标不是已证明的unpackaged进程；拒绝Resume并清理OWN Job")
        N.require(win.k.ResumeThread(pi.thread) == 1, "OWN primary suspend count 不符")
        deadline, kill_deadline, killing = time.monotonic() + args.timeout_seconds, None, False
        startup_breakpoint_codes, create_seen, modules = set(), False, []
        names = {1: "EXCEPTION", 2: "CREATE_THREAD", 3: "CREATE_PROCESS", 4: "EXIT_THREAD",
                 5: "EXIT_PROCESS", 6: "LOAD_DLL", 7: "UNLOAD_DLL", 8: "OUTPUT_DEBUG_STRING", 9: "RIP"}
        while not exited:
            if time.monotonic() >= deadline and not killing:
                result["errors"].append("timeout: terminated only the assigned OWN job")
                win.check(win.k.TerminateJobObject(job, KILL_CODE), "terminate OWN timed-out job")
                killing, kill_deadline = True, time.monotonic() + 5
            if killing and time.monotonic() >= kill_deadline:
                raise TimeoutError("OWN job termination did not yield EXIT_PROCESS within drain budget")
            event = DEBUGEVENT()  # 清零 padding，保存本次内核填充的原结构 bytes。
            if not win.k.WaitForDebugEventEx(C.byref(event), 100):
                error = C.get_last_error()
                if error == 121:  # ERROR_SEM_TIMEOUT
                    continue
                raise OSError(error, "WaitForDebugEventEx")
            continuation = 0x80010001 if event.code == 1 else 0x00010002
            pending = (event.pid, event.tid, continuation, event.code)
            N.require(event.pid == pi.pid, "debug event 不是唯一 OWN target PID")
            row = {"sequence": evidence.count, "event": names.get(event.code, "UNKNOWN"),
                   "pid": event.pid, "tid": event.tid, "unixNs": time.time_ns(),
                   "rawEventHex": bytes(event).hex()}
            if event.code == 3:
                N.require(not create_seen and event.tid == pi.tid, "CREATE_PROCESS 重复或初始 TID 不符")
                create_seen = True
                image_file = event.info.createProcess.file
                try:
                    row["image"] = win.observed_file(image_file)
                    N.require(row["image"]["sha256"] == launch["bindings"]["image"]["sha256"] and
                              canonical(row["image"]["path"]) == canonical(str(launch["image"])), "CREATE image handle SHA/path 不符")
                finally:
                    win.close(image_file)
                N.require(win.process_identity(event.info.createProcess.process, pi.pid, launch["image"]) == result["process"],
                          "CREATE event process handle creation 身份不符")
                actual = actual_parameters(win, pi.process, pi.pid, int(event.info.createProcess.base), launch, evidence)
                modules.append({"base": int(event.info.createProcess.base), **row["image"]})
                result["actualParameters"] = actual
                evidence.json("actual-parameters.local.json", actual)
                if args.owned_loader_snaps:
                    owned_loader_snaps(win, pi.process, pi.pid, int(event.info.createProcess.base), actual,
                                       result["loaderSnaps"], evidence)
                row["beforeFirstUserExecution"] = True
            elif event.code == 6:
                hfile = event.info.loadDll.file
                row["base"] = int(event.info.loadDll.base or 0)
                try:
                    row["image"] = win.observed_file(hfile)
                    modules.append({"base": row["base"], **row["image"]})
                except (OSError, ValueError) as error:
                    row["image"] = {"path": None, "unknown": True, "error": str(error)}
                    result["errors"].append("LOAD_DLL identity unknown: " + str(error))
                finally:
                    win.close(hfile)
            elif event.code == 8:
                try:
                    row["debugString"] = capture_string(win, pi.process, event.info.debugString, evidence)
                except (OSError, ValueError, UnicodeError) as error:
                    row["debugString"] = {"captureError": str(error)}
                    result["errors"].append("debug string capture: " + str(error))
            elif event.code == 1:
                rec = event.info.exception.record
                N.require(rec.parameterCount <= 15, "异常参数数超过 SDK 界限")
                row["exception"] = {"code": rec.code, "hex": f"0x{rec.code:08X}",
                    "address": int(rec.address or 0), "flags": rec.flags,
                    "firstChance": bool(event.info.exception.firstChance),
                    "parameters": list(rec.parameters)[:rec.parameterCount]}
                containing = next((m for m in modules if m["base"] <= int(rec.address or 0) <
                                   m["base"] + m.get("imageSize", 0)), None)
                row["exception"]["observedAddressModule"] = containing
                # WOW64 可给 native/WX86 两种启动断点。仅接受已加载的系统启动模块内
                # 每种码第一次 first-chance；目标 image 内 int3 与后续 DebugBreak 不吞。
                system_directory = win.system_directories["native64" if rec.code == 0x80000003 else "wow64"]
                startup_order = ((rec.code == 0x80000003 and not startup_breakpoint_codes) or
                                 (rec.code == 0x4000001F and startup_breakpoint_codes == {0x80000003}))
                if (rec.code in (0x80000003, 0x4000001F) and event.info.exception.firstChance and event.tid == pi.tid and
                        startup_order and containing and
                        canonical(str(Path(containing["path"]).parent)) == canonical(system_directory) and
                        containing["machine"] == ("0x8664" if rec.code == 0x80000003 else "0x14c") and
                        Path(containing["path"]).name.lower() in ("ntdll.dll", "wow64.dll")):
                    startup_breakpoint_codes.add(rec.code)
                    continuation = 0x00010002
                    row["startupBreakpointCandidateHandled"] = True
                    row["startupBreakpointRule"] = "initial TID + observed system directory/architecture + first code; local fixture verified; OS startup behavior may differ"
            elif event.code == 5:
                code = event.info.exitCode
                result.update(actualExitCode=code if code < 0x80000000 else code - 0x100000000,
                              actualExitHex=f"0x{code:08X}")
                row["exitCode"] = code
            elif event.code == 7:
                row["base"] = int(event.info.unloadBase or 0)
                modules[:] = [m for m in modules if m["base"] != row["base"]]
            elif event.code == 9:
                row["rip"] = {"error": event.info.rip[0], "type": event.info.rip[1]}
            evidence.event(row)
            pending = (event.pid, event.tid, continuation, event.code)
            win.check(win.k.ContinueDebugEvent(*pending[:3]), "ContinueDebugEvent")
            if event.code == 5:
                exited = True  # EXIT 只有成功 Continue 后才算完成回收。
            pending = None
        result["traceCompleted"] = create_seen and not killing
    except Exception as error:
        result["errors"].append(type(error).__name__ + ": " + str(error))
        if pi.process and not exited:
            if assigned:
                terminated = win.k.TerminateJobObject(job, KILL_CODE)
            else:
                terminated = win.k.TerminateProcess(pi.process, KILL_CODE)
            if not terminated:
                result["errors"].append("OWN failure termination failed: " + str(C.get_last_error()))
        if pending:
            if win.k.ContinueDebugEvent(*pending[:3]):
                if pending[3] == 5:
                    exited = True
            else:
                result["errors"].append("OWN pending Continue retry failed: " + str(C.get_last_error()))
            pending = None
        # 不允许收集错误留下自己的 debuggee 暂停；失败回收亦只消费 OWN PID 事件。
        if pi.process and not exited:
            end = time.monotonic() + 5
            while time.monotonic() < end:
                event = DEBUGEVENT()
                if not win.k.WaitForDebugEventEx(C.byref(event), 100):
                    wait_error = C.get_last_error()
                    if wait_error == 121:
                        continue
                    result["errors"].append("OWN drain Wait failed: " + str(wait_error))
                    break
                if event.pid != pi.pid:
                    result["errors"].append("unexpected PID during OWN drain")
                    break
                # 回收不能依赖已耗尽预算/失败磁盘的证据写入，逐事件 finally Continue。
                try:
                    if event.code in (3, 6):
                        win.close(event.info.createProcess.file if event.code == 3 else event.info.loadDll.file)
                    if event.code == 5:
                        code = event.info.exitCode
                        result.update(actualExitCode=code if code < 0x80000000 else code - 0x100000000,
                                      actualExitHex=f"0x{code:08X}")
                    if evidence:
                        evidence.event({"sequence": evidence.count, "event": "FAILURE_DRAIN", "code": event.code,
                                        "pid": event.pid, "tid": event.tid, "rawEventHex": bytes(event).hex()})
                except Exception as drain_error:
                    result["errors"].append("failure drain capture: " + str(drain_error))
                finally:
                    continued = win.k.ContinueDebugEvent(event.pid, event.tid, 0x80010001 if event.code == 1 else 0x00010002)
                    if continued and event.code == 5:
                        exited = True
                    if not continued:
                        result["errors"].append("failure drain Continue failed: " + str(C.get_last_error()))
                if exited or not continued:
                    break
    finally:
        result["targetExitObserved"] = exited
        if pi.process and exited:
            # Continue EXIT 后还存在 kernel shutdown 窗口；先等自有 PI handle signaled，
            # 再查询 Job，不能把这个窗口的 root PID 误判为仍活跃后代。
            result["targetSignaledBeforeJobQuery"] = win.k.WaitForSingleObject(pi.process, 5000) == 0
            if not result["targetSignaledBeforeJobQuery"]:
                result["errors"].append("OWN target not signaled after EXIT Continue")
        if job:
            # Job 只容纳本工具新建的 target 与其自然子孙；不 attach 或枚举作者进程。
            ids, returned = C.create_string_buffer(8192), U32()
            if win.k.QueryInformationJobObject(job, 3, ids, len(ids), C.byref(returned)):
                assigned_count, list_count = struct.unpack_from("<II", ids.raw)
                bounded = min(list_count, (len(ids) - 8) // 8)
                complete = (list_count == assigned_count and list_count <= (len(ids) - 8) // 8 and
                            returned.value >= 8 + list_count * 8)
                associated = list(struct.unpack_from("<" + "Q" * bounded, ids.raw, 8)) if bounded else []
                root_signaled = result.get("targetSignaledBeforeJobQuery", False)
                unresolved = [p for p in associated if p != pi.pid or not root_signaled]
                # PID list 是 associated processes，不是活跃状态 oracle。仅排除已
                # 通过同一 PI handle EXIT Continue + signaled 证明终止的 OWN root。
                result["jobBeforeClose"] = {"assigned": assigned_count, "listed": list_count, "associatedPids": associated,
                    "knownSignaledRootExcluded": root_signaled, "unresolvedAssociatedPids": unresolved,
                    "enumerationComplete": complete,
                    "onlyRootDebugged": True, "jobCloseKillsRemainingOwnDescendants": True}
                if not complete:
                    result["errors"].append("OWN job final PID enumeration incomplete")
                if unresolved:
                    result["errors"].append("OWN job has unresolved associated target/descendants; last job handle close contains them")
            else:
                result["errors"].append("OWN job final PID query failed: " + str(C.get_last_error()))
            try:
                win.close(job)
            except Exception as close_error:
                result["errors"].append("job close: " + str(close_error))
        if pi.process:
            result["ownTargetSignaledAfterJobClose"] = win.k.WaitForSingleObject(pi.process, 1000) == 0
            if not result["ownTargetSignaledAfterJobClose"]:
                result["errors"].append("OWN target not signaled after job close")
        for handle in (pi.thread, pi.process):
            try:
                win.close(handle)
            except Exception as close_error:
                result["errors"].append("OWN PI handle close: " + str(close_error))
        try:
            locks.close()
        except Exception as lock_error:
            result["errors"].append(str(lock_error))
        result.update(endedUnixNs=time.time_ns(), eventCount=evidence.count if evidence else 0,
                      evidenceComplete=bool(evidence) and exited and not result["errors"])
        if evidence:
            evidence.events.close()
            evidence.json("summary.local.json", result)
    return result


def parser():
    class Parser(argparse.ArgumentParser):
        def _get_option_tuples(self, option_string):
            return [item for item in super()._get_option_tuples(option_string)
                    if item[0].dest != "require_unpackaged_target" or option_string == "--require-unpackaged-target"]
    class OptInOnce(argparse.Action):
        def __call__(self, parser, namespace, values, option_string=None):
            if getattr(namespace, self.dest, False):
                raise argparse.ArgumentError(self, "显式opt-in不得重复")
            setattr(namespace, self.dest, True)
    p = Parser(description=__doc__)
    for field in ("image", "image-sha256", "config", "config-sha256", "prepared", "prepared-sha256",
                  "cwd", "map", "map-sha256", "argv-json", "process-name", "evidence"):
        p.add_argument("--" + field, required=True)
    p.add_argument("--timeout-seconds", type=float, default=30)
    p.add_argument("--owned-loader-snaps", action="store_true",
                   help="Explicit opt-in to OWN pre-execution internal PEB NtGlobalFlag writes; requires root review before real loader")
    p.add_argument("--require-unpackaged-target", action=OptInOnce, nargs=0, default=False,
                   help="Require same OWN suspended target GetPackageFullName=15700 before Resume")
    return p


def main():
    result = run_trace(parser().parse_args())
    print(json.dumps({key: result.get(key) for key in ("traceCompleted", "evidenceComplete", "actualExitCode", "actualExitHex", "errors")}, ensure_ascii=True))
    return 0 if result["traceCompleted"] and result["evidenceComplete"] else 2


if __name__ == "__main__":
    sys.exit(main())
