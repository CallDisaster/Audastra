"""P3 经典 DOO 完整字节 oracle；复用既有 CLI 事务、脚本与保存链。

inspect/compare 只读参数化 DOO 文件，save 只向新证据目录生成独立候选。
不解释 rawcode 前缀，不读取 tiara 正文，不宣称原版真保存或游戏通过。
子进程超时只停止等待，保留 PID、现场和日志，不 kill、不自动清理。
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from mcp_object_http_probe import normalized_member, safe_member


_support_spec = importlib.util.spec_from_file_location(
    "p3_existing_path_support", Path(__file__).with_name("p1-support.py"))
if _support_spec is None or _support_spec.loader is None:
    raise RuntimeError("无法加载既有 P1 路径/文件身份工具")
_support = importlib.util.module_from_spec(_support_spec)
_support_spec.loader.exec_module(_support)
require, plain_path = _support.require, _support.plain_path
sha256, write_json = _support.sha256, _support.write_json

DOO_MEMBER = "war3map.doo"
MAX_WIRE_BYTES, MAX_PLACEMENTS, MAX_NESTED = 64 * 1024 * 1024, 100000, 1000000
MAX_OUTPUT_BYTES, MAX_MEMBERS = 8 * 1024 * 1024, 16384
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
METADATA_NAMES = {"(listfile)", "(attributes)", "(signature)"}
OPAQUE_PROTECTED = ("war3map.wtg", "war3map.wct", "tiara.ai", "tiara.blp")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def f32(value):
    require(type(value) in (int, float) and math.isfinite(value), "浮点输入必须有限")
    try:
        result = struct.unpack("<f", struct.pack("<f", value))[0]
    except (OverflowError, struct.error) as error:
        raise ValueError("float32 输入溢出") from error
    require(math.isfinite(result), "float32 结果不是有限值")
    return result


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


class DoodadReader:
    """原创按字段流读取；保留记录切片，期望值不调用 C++ reader/writer。"""

    def __init__(self, data):
        require(isinstance(data, bytes) and 24 <= len(data) <= MAX_WIRE_BYTES,
                "DOO 成员超出 24 字节至 64 MiB 的独立预算")
        self.data, self.offset, self.placements, self.nested = data, 0, 0, 0

    def take(self, count):
        require(0 <= count <= len(self.data) - self.offset,
                f"DOO 字段截断，offset={self.offset} need={count}")
        start = self.offset
        self.offset += count
        return self.data[start:self.offset]

    def integer(self):
        return struct.unpack("<i", self.take(4))[0]

    def floating(self):
        value = struct.unpack("<f", self.take(4))[0]
        require(math.isfinite(value), f"DOO 含非有限浮点，offset={self.offset - 4}")
        return value

    def count(self, minimum_width, nested=False):
        count = self.integer()
        require(0 <= count <= (len(self.data) - self.offset) // minimum_width,
                "DOO 数量负值或无法由剩余完整字段承载")
        consumed = self.nested if nested else self.placements
        maximum = MAX_NESTED if nested else MAX_PLACEMENTS
        require(count <= maximum - consumed, "DOO 全局普通/特殊或嵌套记录预算超限")
        if nested:
            self.nested += count
        else:
            self.placements += count
        return count


def parse_layout(data, skin=False):
    reader = DoodadReader(data)
    require(reader.take(4) == b"W3do" and reader.integer() == 8 and reader.integer() == 11,
            "仅接受 W3do v8/subversion11；未知版本不能猜布局")
    count = reader.count(54 if skin else 50)
    rows = []
    for index in range(count):
        start = reader.offset
        row = {"index": index, "start": start, "rawcode": reader.take(4), "variation": reader.integer()}
        row["position"] = [reader.floating() for _ in range(3)]
        row["facing"] = reader.floating()
        row["scale"] = [reader.floating() for _ in range(3)]
        if skin:
            row["skin"] = reader.take(4)
        row["flags"] = reader.take(1)[0]
        require(row["flags"] < 0x20, "DOO flags 表明未知/错位布局")
        row["life"] = reader.take(1)[0]
        row["dropped_item_table"] = reader.integer()
        row["item_sets"] = []
        for _ in range(reader.count(4, nested=True)):
            choices = [(reader.take(4), reader.integer()) for _ in range(reader.count(8, nested=True))]
            row["item_sets"].append(choices)
        row["creation_offset"] = reader.offset
        row["creation_id"] = reader.integer()
        row["end"], row["bytes"] = reader.offset, data[start:reader.offset]
        rows.append(row)
    tail_start = reader.offset
    require(reader.integer() == 0, "特殊地物版本未确认，拒绝保存 oracle")
    special_count = reader.count(16)
    special = []
    for index in range(special_count):
        start = reader.offset
        record = {"index": index, "rawcode": reader.take(4), "variation": reader.integer(),
                  "position_grid": [reader.integer(), reader.integer()]}
        record["bytes"] = data[start:reader.offset]
        special.append(record)
    require(reader.offset == len(data), "DOO 后有未知尾部；不能丢弃或猜解释")
    return {"data": data, "sha256": digest(data), "rows": rows, "count": count,
            "special": special, "tail_start": tail_start, "tail": data[tail_start:],
            "nested_records": reader.nested, "layout": "skin" if skin else "classic"}


def read_doodads_bytes(data):
    classic = parse_layout(data)
    require(classic["count"] > 0, "无普通记录时布局未定，不提供可编辑 oracle")
    try:
        parse_layout(data, skin=True)
    except ValueError:
        return classic
    raise ValueError("classic 与 SkinId 都可完整读至 EOF；独立 oracle 拒绝歧义")


def read_doodads(path):
    path = plain_path(path)
    require(path.is_file() and 24 <= path.stat().st_size <= MAX_WIRE_BYTES, "DOO 输入必须为预算内普通文件")
    return read_doodads_bytes(path.read_bytes())


def selected_row(source, index):
    require(type(index) is int and 0 <= index < source["count"], "选中行必须为普通 DOO 记录")
    row = source["rows"][index]
    require(all(abs(value) <= 1000000 for value in row["position"]), "选中坐标超出已有摆放编辑范围")
    return row


def rotation_radians(degrees):
    # CLI 的公开角度是 float32 度；wire 为 [0, 2π) float32 弧度。
    value = f32(degrees)
    require(0 <= value < 360, "绝对朝向必须为 [0,360) 有限度数")
    radians, turn = f32(value * math.pi / 180), f32(2 * math.pi)
    require(radians < turn, "float32 度到弧度舍入到整周，既有服务会拒绝")
    return f32(math.fmod(f32(f32(math.fmod(radians, turn)) + turn), turn))


def expected_rotation_bytes(source, index, degrees):
    row = selected_row(source, index)
    expected = bytearray(source["data"])
    struct.pack_into("<f", expected, row["start"] + 20, rotation_radians(degrees))
    return bytes(expected)


def expected_copy_bytes(source, index, offset):
    row = selected_row(source, index)
    require(len(offset) == 3 and all(abs(f32(value)) <= 1000000 for value in offset), "复制偏移不是有效 XYZ")
    require(all(abs(value) <= 1000 for value in row["scale"]), "复制源缩放超出已有安全范围")
    require(source["count"] + len(source["special"]) < MAX_PLACEMENTS, "复制后的普通及特殊总数超限")
    creation_id = max(-1, *(entry["creation_id"] for entry in source["rows"])) + 1
    require(creation_id <= 2**31 - 1, "新 creationId 已耗尽")
    clone = bytearray(row["bytes"])
    for axis, delta in enumerate(offset):
        value = f32(row["position"][axis] + f32(delta))
        require(abs(value) <= 1000000, "复制后的坐标越界")
        struct.pack_into("<f", clone, 8 + axis * 4, value)
    require(row["creation_offset"] == row["end"] - 4, "creationId 必须为完整普通记录尾")
    struct.pack_into("<i", clone, len(clone) - 4, creation_id)
    expected = bytearray(source["data"])
    struct.pack_into("<i", expected, 12, source["count"] + 1)
    expected[source["tail_start"]:source["tail_start"]] = clone
    require(len(expected) <= MAX_WIRE_BYTES, "复制 DOO 字节预算超限")
    # 再独立解析新增完整流，同时核全局嵌套预算和布局没有变为歧义。
    read_doodads_bytes(bytes(expected))
    return bytes(expected)


def wire_summary(wire, index=None):
    result = {"layout": wire["layout"], "bytes": len(wire["data"]), "sha256": wire["sha256"],
              "normal_count": wire["count"], "special_count": len(wire["special"]),
              "nested_record_count": wire["nested_records"], "special_tail_sha256": digest(wire["tail"])}
    if index is not None:
        row = selected_row(wire, index)
        result["selected"] = {"index": index, "rawcode_hex": row["rawcode"].hex(), "creation_id": row["creation_id"],
            "variation": row["variation"], "position_world": row["position"], "position_world_raw_u32": [bits(v) for v in row["position"]],
            "facing_radians": row["facing"], "facing_raw_u32": bits(row["facing"]), "scale": row["scale"],
            "flags": row["flags"], "life": row["life"], "dropped_item_table": row["dropped_item_table"],
            "item_set_count": len(row["item_sets"]),
            "item_set_sizes_prefix": [len(item_set) for item_set in row["item_sets"][:128]],
            "item_set_sizes_truncated": len(row["item_sets"]) > 128,
            "drop_payload_sha256": digest(row["bytes"][38:-4]), "record_sha256": digest(row["bytes"])}
    return result


def compare_wire(source, candidate, operation, index, degrees=None, offset=None):
    expected = (expected_rotation_bytes(source, index, degrees) if operation == "rotate"
                else expected_copy_bytes(source, index, offset))
    require(candidate["data"] == expected, "完整候选 DOO 不等于独立 oracle；禁止只核 XYZ")
    require(candidate["tail"] == source["tail"], "完整特殊网格尾发生变化")
    unchanged = [row["index"] for row in source["rows"] if operation == "copy" or row["index"] != index]
    require(all(source["rows"][row]["bytes"] == candidate["rows"][row]["bytes"] for row in unchanged),
            "未编辑普通记录切片发生变化")
    result = {"operation": operation, "source": wire_summary(source, index), "candidate": wire_summary(candidate, index),
              "full_member_equals_independent_expected": True, "special_tail_exact": True,
              "native_member_changed": candidate["data"] != source["data"],
              "unmodified_normal_record_count": len(unchanged), "all_unmodified_record_slices_exact": True}
    if operation == "copy":
        result["copy_offset"] = list(offset)
        result["appended"] = wire_summary(candidate, source["count"])["selected"]
        result["cloned_drop_sets_and_all_non_xyz_id_bytes_exact"] = True
    else:
        result["degrees"] = degrees
        result["only_selected_facing_window_may_differ"] = True
    return result


def file_identity(path):
    path = plain_path(path)
    require(path.is_file(), f"输入不是普通文件: {path}")
    info = path.stat()
    return {"path": str(path), "bytes": info.st_size, "sha256": sha256(path),
            "device": info.st_dev, "file_id": info.st_ino, "mtime_ns": info.st_mtime_ns}


def strict_json(text):
    def reject_constant(value):
        raise ValueError(f"JSON 不允许非有限值: {value}")

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "JSON 不能有重复字段")
            result[key] = value
        return result

    return json.loads(text, parse_constant=reject_constant, object_pairs_hook=unique_object)


class Probe:
    def __init__(self, args):
        self.args = args
        self.output = plain_path(args.output_dir)
        require(not self.output.exists(), "证据目录必须为不存在的新路径；失败现场不能覆盖")
        self.input_identities = []
        for name in ("source", "candidate", "doo", "cli"):
            value = getattr(args, name, None)
            if value is None:
                continue
            path, output = _support.separate_paths(value, self.output)
            require(output == self.output, "输出身份解析不一致")
            setattr(args, name, path)
            self.input_identities.append(file_identity(path))
        if args.mode == "save":
            require(SHA_PATTERN.fullmatch(args.expected_source_sha256) is not None, "需要明确的来源 lowercase SHA-256")
            require(sha256(args.source) == args.expected_source_sha256, "来源归档与期望 SHA 不一致")
            for name in ("ydwe_root", "war3_root"):
                path, output = _support.separate_paths(getattr(args, name), self.output)
                require(path.is_dir() and output == self.output, "工具链路径须为普通目录且与输出分离")
                setattr(args, name, path)
        self.output.mkdir(parents=True, exist_ok=False)
        self.live_processes = []
        self.receipt = {"schema": "p3-doodad-save-probe/v1", "mode": args.mode, "passed": False,
            "started_utc": datetime.now(timezone.utc).isoformat(), "output_dir": str(self.output),
            "oracle_file_sha256": sha256(plain_path(__file__)), "inputs_before": self.input_identities,
            "cli_calls": [], "source_and_input_files_unchanged": False,
            "native_editor_verified": False, "game_runtime_verified": False, "cleanup_attempted": False}
        write_json(self.output / "owned-scene.local.json", {"schema": "p3-owned-evidence/v1",
            "output_dir": str(self.output), "parent_pid": os.getpid(), "inputs_before": self.input_identities,
            "source_access": "readonly", "existing_paths_overwritten": False})

    def artifact(self, name, value):
        require(re.fullmatch(r"[A-Za-z0-9_.-]+", name) is not None, "回执名必须为单个安全路径段")
        write_json(self.output / name, value)

    def run_cli(self, label, *arguments, timeout=None):
        require(re.fullmatch(r"[a-z0-9-]+", label) is not None, "CLI 日志标签无效")
        command = [str(self.args.cli), *(str(value) for value in arguments)]
        stdout_path, stderr_path = self.output / (label + ".stdout.log"), self.output / (label + ".stderr.log")
        record = {"label": label, "argv": command, "started_utc": datetime.now(timezone.utc).isoformat(),
                  "pid": None, "exit_code": None, "timed_out": False, "kill_attempted": False}
        self.receipt["cli_calls"].append(record)
        started = time.monotonic()
        process = None
        try:
            environment = os.environ.copy()
            environment["PYTHONIOENCODING"] = "utf-8:strict"
            with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                process = subprocess.Popen(command, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL,
                    env=environment, shell=False,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                record["pid"] = process.pid
                try:
                    record["exit_code"] = process.wait(timeout=timeout or self.args.cli_timeout)
                except subprocess.TimeoutExpired as error:
                    record["timed_out"] = True
                    self.live_processes.append(process)
                    raise ValueError(f"CLI 超时；保留自然运行 PID={process.pid}，不得称候选通过: {label}") from error
            require(stdout_path.stat().st_size <= MAX_OUTPUT_BYTES and stderr_path.stat().st_size <= MAX_OUTPUT_BYTES,
                    "CLI 日志超出 8 MiB 输出读取预算；原日志保留")
            require(record["exit_code"] == 0, f"CLI actual_exit={record['exit_code']}；见 {label} 的原日志")
            return stdout_path.read_bytes().decode("utf-8", errors="strict")
        finally:
            record["seconds"] = time.monotonic() - started
            record["still_running_at_return"] = process is not None and process.poll() is None
            record["streams"] = {}
            for name, path in (("stdout", stdout_path), ("stderr", stderr_path)):
                record["streams"][name] = {"path": str(path), "bytes_at_return": path.stat().st_size if path.exists() else None,
                    "sha256": sha256(path) if path.exists() and not record["still_running_at_return"] else None}
            self.artifact(label + ".command.local.json", record)

    def verify_inputs(self):
        after = []
        for before in self.input_identities:
            current = file_identity(before["path"])
            after.append(current)
            require(current == before, f"来源或输入文件身份变化: {before['path']}")
        self.receipt["inputs_after"] = after
        self.receipt["source_and_input_files_unchanged"] = True


def list_members(probe, archive, label):
    output = probe.run_cli(label + "-members", "mpq-list", archive)
    lines = output.splitlines()
    require(lines, "MPQ 列表为空")
    summary = re.fullmatch(r"total=(\d+) shown=(\d+)", lines[-1])
    require(summary is not None and int(summary.group(1)) == int(summary.group(2)) == len(lines) - 1,
            "MPQ 列表未完整枚举")
    names = lines[:-1]
    require(0 < len(names) <= MAX_MEMBERS, "MPQ 具名成员预算超限")
    require(len({normalized_member(name) for name in names}) == len(names), "MPQ 成员名称歧义")
    for name in names:
        safe_member(name)
    members = {normalized_member(name): name for name in names}
    probe.artifact(label + ".member-list.local.json", {"archive": str(archive), "names": names,
        "metadata_names": [name for name in names if normalized_member(name) in METADATA_NAMES]})
    return members


def archive_coverage(probe, archive, expected_sha, label):
    output = probe.run_cli(label + "-coverage", "archive-session-inspect", archive)
    lines = output.splitlines()
    source_shas = [line.split("=", 1)[1] for line in lines if line.startswith("sourceSha256=")]
    opaque_counts = [line.split("=", 1)[1] for line in lines if line.startswith("opaqueMemberCount=")]
    require(source_shas == [expected_sha] and len(opaque_counts) == 1 and opaque_counts[0].isdigit(),
            "归档覆盖回执没有绑定当前来源 SHA 或 opaque 计数")
    opaque = int(opaque_counts[0])
    verify_append = bool(getattr(probe.args, "verify_opaque_append", False))
    require(opaque == 0 or verify_append, "存在匿名归档成员；具名哈希不能代替其完整独立保护证明")
    result = {"source_sha256": expected_sha, "opaque_member_count": opaque, "named_member_coverage_checked": True}
    if verify_append:
        result["independent_raw_append_proof_required"] = True
    return result


def member_identities(probe, archive, expected_sha, names, label):
    # 已有精确捕获 API：只取 presence/size/SHA，不提取或解释 tiara 字节。
    wanted = {key: value for key, value in names.items() if key not in METADATA_NAMES}
    for name in OPAQUE_PROTECTED:
        wanted.setdefault(normalized_member(name), name)
    ordered = [wanted[key] for key in sorted(wanted)]
    result = {}
    batch_size = probe.args.identity_batch_size
    for start in range(0, len(ordered), batch_size):
        batch = ordered[start:start + batch_size]
        arguments = ["archive-session-inspect", archive, "--expected-source-sha256", expected_sha]
        for name in batch:
            arguments.extend(("--member", name))
        reply = strict_json(probe.run_cli(f"{label}-identities-{start // batch_size:04d}", *arguments))
        require(isinstance(reply, dict) and reply.get("source_sha256") == expected_sha
                and isinstance(reply.get("members"), list) and len(reply["members"]) == len(batch),
                "精确成员身份没有绑定同一来源与全部请求")
        for name, row in zip(batch, reply["members"]):
            require(row.get("name") == name and row.get("state") in ("present", "missing"), "成员身份名称/状态不一致")
            key = normalized_member(name)
            if row["state"] == "present":
                require(isinstance(row.get("bytes"), str) and row["bytes"].isdigit()
                        and isinstance(row.get("sha256"), str) and SHA_PATTERN.fullmatch(row["sha256"]) is not None,
                        "存在成员缺少有效完整大小/SHA")
            else:
                require(key not in names and row.get("bytes") is None and row.get("sha256") is None,
                        "具名清单成员被报告 missing，或 missing 填入伪造身份")
            require(key not in result, "精确身份不能重复")
            result[key] = row
    probe.artifact(label + ".member-identities.local.json", {"archive": str(archive), "source_sha256": expected_sha,
        "members": list(result.values()), "tiara_content_parsed": False,
        "reserved_metadata_excluded": sorted(METADATA_NAMES), "capture_api_byte_budget": 64 * 1024 * 1024})
    return result


def extract_doo(probe, archive, identities, label):
    identity = identities.get(DOO_MEMBER)
    require(identity is not None and identity["state"] == "present", "归档必须含普通 DOO 成员")
    require(int(identity["bytes"]) <= MAX_WIRE_BYTES, "归档 DOO 超过独立预算")
    target = probe.output / (label + ".doo")
    require(not target.exists(), "DOO 提取目标必须新建")
    probe.run_cli(label + "-doo-extract", "mpq-extract", archive, identity["name"], target)
    wire = read_doodads(target)
    require(wire["sha256"] == identity["sha256"] and len(wire["data"]) == int(identity["bytes"]),
            "提取 DOO 与精确归档身份不一致")
    return wire


def bind_selected_query(probe, archive, expected_sha, source):
    native = selected_row(source, probe.args.row)
    reply = strict_json(probe.run_cli("source-selected-query", "map-data-query", archive, expected_sha,
                                    "doodads", probe.args.row, 1))
    require(isinstance(reply, dict) and reply.get("ok") is True
            and isinstance(reply.get("binding"), dict) and reply["binding"].get("source_sha256") == expected_sha,
            "公共查询没有绑定本次来源 SHA")
    member = reply.get("member")
    require(isinstance(member, dict) and member.get("state") == "ready" and member.get("kind") == "doodads"
            and normalized_member(member.get("member_name", "")) == DOO_MEMBER
            and member.get("member_sha256") == source["sha256"]
            and member.get("record_count") == str(source["count"] + len(source["special"])),
            "公共查询的完整 DOO 身份/普通特殊总数不一致")
    require(isinstance(reply.get("rows"), list) and len(reply["rows"]) == 1, "公共查询必须返回一条绑定的普通行")
    row = reply["rows"][0]
    expected_key = "doodad:" + native["rawcode"].hex() + ":" + str(native["creation_id"])
    require(row.get("form") == "normal-doodad" and row.get("source_index") == str(probe.args.row)
            and row.get("rawcode_hex") == native["rawcode"].hex() and row.get("creation_number") == native["creation_id"]
            and row.get("row_key") == expected_key
            and row.get("position_world_raw_u32") == [bits(value) for value in native["position"]],
            "公共行键/rawcode/creationId/XYZ bits 与独立 DOO 不一致")
    probe.artifact("source.selected-query.local.json", reply)
    return row


def save_candidate(probe):
    args = probe.args
    before_sha = args.expected_source_sha256
    source_coverage = archive_coverage(probe, args.source, before_sha, "source")
    source_names = list_members(probe, args.source, "source")
    source_identities = member_identities(probe, args.source, before_sha, source_names, "source")
    source_wire = extract_doo(probe, args.source, source_identities, "source")
    row = bind_selected_query(probe, args.source, before_sha, source_wire)
    expected = (expected_rotation_bytes(source_wire, args.row, args.degrees) if args.operation == "rotate"
                else expected_copy_bytes(source_wire, args.row, args.offset))
    require(expected != source_wire["data"], "请求没有净变更，不把无操作当保存验收")
    append_oracle, append_intent = None, None
    if args.verify_opaque_append:
        # 权限来自保存前的固定名字/源身份，不从候选发生变化的块倒推权限。
        oracle_path = Path(__file__).with_name("mpq_append_oracle.py")
        spec = importlib.util.spec_from_file_location("p3_independent_mpq_append", oracle_path)
        require(spec is not None and spec.loader is not None, "无法加载独立 raw append oracle")
        append_oracle = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(append_oracle)
        original_named = set(source_names) - METADATA_NAMES
        scripts = {"war3map.j", "war3map.lua", "scripts/war3map.j", "scripts/war3map.lua"} & original_named
        require(len(scripts) == 1 and next(iter(scripts)).endswith(".j"), "opaque append 只支持一个经典主 JASS")
        script_name = next(iter(scripts))
        optional = []
        for suffix in ("w3u", "w3a", "w3t", "w3h", "w3q", "w3d", "w3b"):
            key = "war3map." + suffix
            if key in original_named:
                identity = source_identities[key]
                optional.append({"name": identity["name"], "bytes": int(identity["bytes"]),
                    "sha256": identity["sha256"], "source_sha256": before_sha})
        append_intent = {"schema": "mpq-append-intent/v1", "source_sha256": before_sha,
            "inventory": {"schema": "mpq-append-inventory/v1", "source_sha256": before_sha, "names": list(source_names.values())},
            "business_names": [DOO_MEMBER, source_names[script_name]],
            "source_doo": {"bytes": len(source_wire["data"]), "sha256": source_wire["sha256"]},
            "expected_doo": {"bytes": len(expected), "sha256": digest(expected)}, "optional_source_equal": optional}
        probe.artifact("opaque-append-intent.local.json", append_intent)
        preflight = append_oracle.preflight_intent(args.source, append_intent)
        require(preflight["source"]["anonymous_block_count"] == source_coverage["opaque_member_count"],
                "独立索引/具名库存的匿名计数与源覆盖回执不一致")
        probe.artifact("opaque-append-preflight.local.json", preflight)
        append_intent_sha = append_oracle.canonical_digest(append_intent)
    candidate = probe.output / "candidate.w3x"
    arguments = ["map-edit-save", args.source, before_sha, "-", args.ydwe_root, args.war3_root, candidate]
    locator = ["doodads", args.row, row["row_key"], source_wire["sha256"]]
    if args.operation == "rotate":
        arguments.extend(("--rotate", *locator, format(args.degrees, ".17g")))
    else:
        arguments.extend(("--copy-source", *locator, "--copy-offset", *(format(value, ".17g") for value in args.offset)))
    for layer in args.layer:
        arguments.extend(("--layer", layer))
    probe.run_cli("shared-map-edit-save", *arguments, timeout=args.save_timeout)
    require(candidate.is_file(), "保存 CLI 成功但未生成实际候选")
    candidate_identity = file_identity(candidate)
    after_sha = candidate_identity["sha256"]
    candidate_coverage = archive_coverage(probe, candidate, after_sha, "candidate")
    candidate_names = list_members(probe, candidate, "candidate")
    original_named = set(source_names) - METADATA_NAMES
    saved_named = set(candidate_names) - METADATA_NAMES
    require(original_named == saved_named, "同一 DOO 保存不能新增或删除具名非元数据成员")
    candidate_identities = member_identities(probe, candidate, after_sha, candidate_names, "candidate")
    candidate_wire = extract_doo(probe, candidate, candidate_identities, "candidate")
    wire_proof = compare_wire(source_wire, candidate_wire, args.operation, args.row, args.degrees, args.offset)
    script_names = {"war3map.j", "war3map.lua", "scripts/war3map.j", "scripts/war3map.lua"}
    present_scripts = script_names & original_named
    require(len(present_scripts) == 1 and next(iter(present_scripts)).endswith(".j"),
            "本批只接受一个明确的经典 JASS 主脚本；Lua 或多主脚本另行验收")
    allowed = {DOO_MEMBER, *present_scripts}
    changed = []
    for key in sorted(source_identities):
        before, after = source_identities[key], candidate_identities[key]
        require(before["name"] == after["name"], "成员名大小写/路径发生未请求变化")
        if before != after:
            changed.append(key)
        if key not in allowed:
            require(before == after, f"未编辑成员完整 presence/size/SHA 变化: {key}")
    require(DOO_MEMBER in changed, "实际候选没有 DOO 净变更")
    if append_oracle is not None:
        require(source_coverage["opaque_member_count"] == candidate_coverage["opaque_member_count"], "候选 anonymous 计数改变")
        require(append_oracle.canonical_digest(append_intent) == append_intent_sha
                and strict_json((probe.output / "opaque-append-intent.local.json").read_text(encoding="utf-8")) == append_intent,
                "保存前冻结的 append intent 发生漂移")
        script_identity = candidate_identities[next(iter(present_scripts))]
        plan = {"schema": "mpq-append-bound-plan/v1", "intent": append_intent,
            "intent_sha256": append_intent_sha, "candidate_sha256": after_sha,
            "jass_witness": {"name": script_identity["name"], "bytes": int(script_identity["bytes"]),
                "sha256": script_identity["sha256"], "candidate_sha256": after_sha, "origin": "product-candidate-decoded-identity"}}
        probe.artifact("opaque-append-bound-plan.local.json", plan)
        try:
            raw_proof = append_oracle.prove_append(args.source, candidate, plan)
        except append_oracle.OracleError as error:
            probe.artifact("opaque-append-proof.local.json", {"schema": "mpq-append-proof/v1", "passed": False,
                "error": error.receipt(), "tiara_content_parsed": False})
            raise
        require(raw_proof["candidate"]["anonymous_block_count"] == candidate_coverage["opaque_member_count"],
                "独立候选索引/具名库存的匿名计数与覆盖回执不一致")
        probe.artifact("opaque-append-proof.local.json", raw_proof)
        probe.receipt["independent_raw_append_proof"] = {"passed": raw_proof["passed"],
            "path": str(probe.output / "opaque-append-proof.local.json"),
            "sha256": sha256(probe.output / "opaque-append-proof.local.json"), "intent_sha256": append_intent_sha,
            "oracle_file_sha256": raw_proof["oracle_file_sha256"],
            "protected_anonymous_block_count": raw_proof["source"]["anonymous_block_count"]}
    require(file_identity(candidate) == candidate_identity, "独立候选回读期间文件身份变化")
    probe.receipt.update(evidence_level="shared-cli-save-and-independent-doo-full-byte-archive-readback",
        source_coverage=source_coverage, candidate_coverage=candidate_coverage,
        candidate=candidate_identity, wire_proof=wire_proof, changed_named_members=changed,
        allowed_changed_named_members=sorted(allowed), unmodified_member_identities_exact=True,
        unmodified_named_member_count=len(original_named - allowed),
        protected_opaque_members={name: {"before": source_identities[normalized_member(name)],
            "after": candidate_identities[normalized_member(name)]} for name in OPAQUE_PROTECTED},
        tiara_content_parsed=False, reserved_metadata_comparison="excluded-by-existing-archive-save-contract")
    if append_oracle is not None:
        probe.receipt.update(evidence_level="shared-cli-save-independent-doo-full-bytes-and-raw-mpq-append",
            reserved_metadata_comparison="raw-block-metadata-and-complete-original-slices-preserved")


def self_check(probe):
    # 原创非空 little-endian 字段流，不读地图或调用产品/编译器。
    first = struct.pack("<4si3ff3fBBii", b"A001", 2, 96, -64, -0.0, 0.25, 1.25, 0.75, 2.25, 2, 77, 17, 2)
    first += struct.pack("<i4si4siii", 2, b"I001", 65, b"I002", 35, 0, 41)
    second = struct.pack("<4si3ff3fBBiii", b"A002", 1, -32, 48, 12, 0, 1, 1, 1, 0, 100, -1, 0, 55)
    tail = struct.pack("<ii4siii", 0, 1, b"\x00\xff\x01\x02", 3, -5, 8)
    data = struct.pack("<4siii", b"W3do", 8, 11, 2) + first + second + tail
    source = read_doodads_bytes(data)
    require(source["count"] == 2 and len(source["special"]) == 1 and source["nested_records"] == 4
            and source["rows"][0]["item_sets"] == [[(b"I001", 65), (b"I002", 35)], []]
            and bits(source["rows"][0]["position"][2]) == 0x80000000
            and source["special"][0]["rawcode"] == b"\x00\xff\x01\x02", "原创完整字段/负零/嵌套/特殊尾不匹配")
    copied = read_doodads_bytes(expected_copy_bytes(source, 0, [32, -16, 4]))
    copy = compare_wire(source, copied, "copy", 0, offset=[32, -16, 4])
    require(copied["rows"][-1]["creation_id"] == 56 and copied["rows"][-1]["position"] == [128, -80, 4]
            and copied["rows"][-1]["item_sets"] == source["rows"][0]["item_sets"], "原创复制语义不匹配")
    rotated = read_doodads_bytes(expected_rotation_bytes(source, 0, 90))
    rotation = compare_wire(source, rotated, "rotate", 0, degrees=90)
    rejected = []

    def refusal(label, action):
        try:
            action()
        except ValueError as error:
            rejected.append({"case": label, "reason": str(error)})
            return
        raise ValueError("独立 reader/oracle 本应拒绝: " + label)

    refusal("unknown-tail", lambda: read_doodads_bytes(data + b"\0"))
    refusal("truncated-tail", lambda: read_doodads_bytes(data[:-1]))
    refusal("empty-undetermined-layout", lambda: read_doodads_bytes(struct.pack("<4siiiii", b"W3do", 8, 11, 0, 0, 0)))
    changed = bytearray(data)
    struct.pack_into("<i", changed, 12, -1)
    refusal("negative-normal-count", lambda: read_doodads_bytes(bytes(changed)))
    changed = bytearray(data)
    struct.pack_into("<I", changed, 24, 0x7fc00000)
    refusal("nonfinite-position", lambda: read_doodads_bytes(bytes(changed)))
    changed = bytearray(data)
    changed[52] = 0x20
    refusal("unknown-flags-layout", lambda: read_doodads_bytes(bytes(changed)))
    changed = bytearray(data)
    struct.pack_into("<i", changed, source["tail_start"], 1)
    refusal("unknown-special-version", lambda: read_doodads_bytes(bytes(changed)))
    changed = bytearray(data)
    struct.pack_into("<i", changed, 58, -1)
    refusal("negative-item-set-count", lambda: read_doodads_bytes(bytes(changed)))
    skin = data[:52] + b"skin" + data[52:]
    # 两普通行都添加 skin 字段；第二记录的位置因此再偏移四字节。
    second_skin_position = source["rows"][1]["start"] + 4 + 36
    skin = skin[:second_skin_position] + b"skin" + skin[second_skin_position:]
    require(parse_layout(skin, skin=True)["count"] == 2, "原创 skin 对照本身必须完整有效")
    refusal("skin-layout-refused", lambda: read_doodads_bytes(skin))
    ambiguous = bytearray(struct.pack("<4siii", b"W3do", 8, 11, 4) + bytes(4 * 54 + 8))
    struct.pack_into("<i", ambiguous, 220, 1)
    require(parse_layout(bytes(ambiguous))["count"] == parse_layout(bytes(ambiguous), skin=True)["count"] == 4,
            "原创歧义对照必须让两布局都完整读取")
    refusal("both-layouts-ambiguous", lambda: read_doodads_bytes(bytes(ambiguous)))
    altered = bytearray(rotated["data"])
    struct.pack_into("<f", altered, source["rows"][1]["start"] + 8, -31)
    refusal("unselected-record-drift", lambda: compare_wire(source, read_doodads_bytes(bytes(altered)), "rotate", 0, degrees=90))
    altered = bytearray(copied["data"])
    struct.pack_into("<i", altered, copied["tail_start"] + 16, -4)
    refusal("special-tail-drift", lambda: compare_wire(source, read_doodads_bytes(bytes(altered)), "copy", 0, offset=[32, -16, 4]))
    altered = bytearray(copied["data"])
    struct.pack_into("<i", altered, copied["rows"][-1]["start"] + 54, 66)
    refusal("copied-drop-choice-drift", lambda: compare_wire(source, read_doodads_bytes(bytes(altered)), "copy", 0, offset=[32, -16, 4]))
    refusal("rotation-outside-contract", lambda: expected_rotation_bytes(source, 0, 360))
    refusal("copy-nonfinite-offset", lambda: expected_copy_bytes(source, 0, [0, 0, float("inf")]))
    refusal("special-is-not-normal-row", lambda: expected_copy_bytes(source, 2, [0, 0, 0]))
    probe.receipt.update(evidence_level="original-python-doo-oracle-self-check-only", wire=wire_summary(source),
        copy_proof=copy, rotation_proof=rotation, expected_refusals=rejected,
        no_map_or_product_process_executed=True)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    for mode in ("inspect", "compare", "save", "self-check"):
        command = subparsers.add_parser(mode)
        command.add_argument("--output-dir", required=True, type=Path)
        if mode == "inspect":
            command.add_argument("--doo", required=True, type=Path)
            command.add_argument("--row", type=int)
        if mode in ("compare", "save"):
            command.add_argument("--source", required=True, type=Path)
            command.add_argument("--row", required=True, type=int)
            command.add_argument("--operation", required=True, choices=("rotate", "copy"))
            command.add_argument("--degrees", type=float)
            command.add_argument("--offset", nargs=3, type=float)
        if mode == "compare":
            command.add_argument("--candidate", required=True, type=Path)
        if mode == "save":
            command.add_argument("--cli", required=True, type=Path)
            command.add_argument("--expected-source-sha256", required=True)
            command.add_argument("--ydwe-root", required=True, type=Path)
            command.add_argument("--war3-root", required=True, type=Path)
            command.add_argument("--layer", action="append", default=[])
            command.add_argument("--cli-timeout", type=float, default=120)
            command.add_argument("--save-timeout", type=float, default=900)
            command.add_argument("--identity-batch-size", type=int, default=32)
            command.add_argument("--verify-opaque-append", action="store_true",
                help="显式启用独立 classic v0 raw append 证明；默认仍拒绝 anonymous")
    args = parser.parse_args()
    if args.mode in ("compare", "save"):
        require(args.row >= 0, "行索引不能为负")
        if args.operation == "rotate":
            require(args.degrees is not None and args.offset is None, "旋转只接受 --degrees")
            rotation_radians(args.degrees)
        else:
            require(args.offset is not None and args.degrees is None, "复制只接受 --offset XYZ")
            require(all(abs(f32(value)) <= 1000000 for value in args.offset), "复制偏移越界")
    if args.mode == "save":
        require(0 < args.cli_timeout <= 3600 and 0 < args.save_timeout <= 3600, "超时须在 0..3600 秒内")
        require(1 <= args.identity_batch_size <= 128, "精确身份批量须为 1..128")
        require(len(args.layer) <= 32 and len(set(args.layer)) == len(args.layer)
                and all(value and "\0" not in value for value in args.layer), "工具链层参数无效/重复/超限")
    return args


def main():
    args = arguments()
    probe = Probe(args)
    failure = None
    try:
        if args.mode == "inspect":
            probe.receipt.update(evidence_level="independent-classic-doo-read-only", wire=wire_summary(read_doodads(args.doo), args.row))
        elif args.mode == "compare":
            proof = compare_wire(read_doodads(args.source), read_doodads(args.candidate), args.operation, args.row, args.degrees, args.offset)
            probe.receipt.update(evidence_level="independent-classic-doo-full-byte-comparison", wire_proof=proof)
        elif args.mode == "save":
            save_candidate(probe)
        else:
            self_check(probe)
    except Exception as error:
        failure = {"type": type(error).__name__, "message": str(error)}
    try:
        probe.verify_inputs()
    except Exception as error:
        failure = {"type": type(error).__name__, "message": str(error), "prior_failure": failure}
    probe.receipt.update(passed=failure is None, failure=failure, ended_utc=datetime.now(timezone.utc).isoformat(),
                         live_process_pids=[process.pid for process in probe.live_processes if process.poll() is None])
    probe.artifact("receipt.local.json", probe.receipt)
    print(json.dumps({"passed": probe.receipt["passed"], "receipt": str(probe.output / "receipt.local.json"),
        "receipt_sha256": sha256(probe.output / "receipt.local.json"), "mode": args.mode,
        "source_and_input_files_unchanged": probe.receipt["source_and_input_files_unchanged"],
        "live_process_pids": probe.receipt["live_process_pids"], "failure": failure}, ensure_ascii=True, allow_nan=False))
    return 0 if probe.receipt["passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError) as error:
        print(json.dumps({"passed": False, "argument_or_setup_failure": type(error).__name__, "message": str(error)},
                         ensure_ascii=True, allow_nan=False))
        raise SystemExit(2) from error
