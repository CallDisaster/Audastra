"""独立 MCP HTTP 物编/共用事务/真实归档回读验收；只写新证据目录。

只证明此脚本实际执行的独立 HTTP 调用和候选归档回读，不等价于 GUI 助手调用、
原生编辑器或游戏验收。凭据只传给自有子进程，不写环境、请求头或 token 到证据。
"""

import argparse
import hashlib
import http.client
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import secrets
import struct
import subprocess
import threading
import time


KINDS = ("unit", "item", "ability", "buff", "destructable", "doodad", "upgrade")
OBJECT_MEMBERS = ("war3map.w3u", "war3map.w3t", "war3map.w3a", "war3map.w3h",
                  "war3map.w3b", "war3map.w3d", "war3map.w3q")
PROTECTED_MEMBERS = ("war3map.wtg", "war3map.wct", "tiara.ai", "tiara.blp")
OBJECT_TOOLS = {"objects_list", "objects_get", "objects_set_fields", "objects_create",
                "objects_delete", "objects_field_options"}
MAX_RESPONSE = 8 * 1024 * 1024
MAX_PAGES = 256
MAX_NATIVE_BYTES = 64 * 1024 * 1024
PERSIST_CREATED_NAME = "Audastra HTTP persisted E000 clone"
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    # 包括 .ai 在内的受保护文件仅流式哈希，不解析或输出其正文。
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def json_sha(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":")).encode("utf-8")).hexdigest()


def normalized_member(name):
    return name.replace("\\", "/").casefold()


def safe_member(name):
    require(isinstance(name, str) and name and "\0" not in name, "invalid MPQ member name")
    windows = PureWindowsPath(name)
    require(not windows.drive and not windows.root, "absolute MPQ member path refused")
    parts = name.replace("\\", "/").split("/")
    reserved = {"CON", "PRN", "AUX", "NUL"} | {f"{p}{i}" for p in ("COM", "LPT") for i in range(1, 10)}
    for part in parts:
        require(part not in ("", ".", "..") and not any(c in part for c in ':*?<>|"'),
                "unsafe MPQ member path refused")
        require(not part.endswith((".", " ")) and part.split(".")[0].upper() not in reserved,
                "Windows device or ambiguous MPQ path refused")
    return Path(*parts)


def inventory(root):
    result = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        require(path.resolve().is_relative_to(root), "workspace path escapes its root")
        require(not path.is_symlink(), "workspace symlink refused for this probe")
        if path.is_file():
            result.append({"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                           "sha256": sha(path)})
    return result


class Evidence:
    def __init__(self, output):
        self.output = output
        self.tokens = []
        self.lock = threading.Lock()
        self.log = (output / "transcript.jsonl").open("w", encoding="utf-8")
        self.receipt = {
            "schema": "audastra-mcp-object-http-probe-v1", "passed": False,
            "evidence_level": "independent-mcp-http-and-candidate-archive-readback",
            "gui_assistant_actual_calls_verified": False,
            "native_editor_verified": False, "game_runtime_verified": False,
            "protocol_version": "2025-03-26", "stage": "created", "checks": [],
            "servers": [], "cli_calls": [], "cleanup_errors": [],
        }

    def scrub(self, text):
        for token in self.tokens:
            text = text.replace(token, "[REDACTED]")
        return text

    def artifact(self, filename, value):
        text = self.scrub(json.dumps(value, ensure_ascii=False, indent=2))
        temporary = self.output / (filename + ".tmp")
        temporary.write_text(text + "\n", encoding="utf-8")
        # Windows 短暂读锁可能拒绝原子替换；只重试共享冲突，最长等待两秒。
        deadline = time.monotonic() + 2.0
        while True:
            try:
                temporary.replace(self.output / filename)
                break
            except OSError as error:
                if os.name != "nt" or getattr(error, "winerror", None) not in (5, 32, 33):
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise
                time.sleep(min(0.05, remaining))
                if time.monotonic() >= deadline:
                    raise

    def checkpoint(self, stage=None):
        if stage is not None:
            self.receipt["stage"] = stage
        self.artifact("receipt.json", self.receipt)

    def event(self, value):
        with self.lock:
            self.log.write(self.scrub(json.dumps(value, ensure_ascii=False)) + "\n")
            self.log.flush()

    def check(self, name, detail):
        self.receipt["checks"].append({"name": name, "passed": True, "detail": detail})
        self.checkpoint(name)


class HttpServer:
    def __init__(self, args, env, workspace, label, evidence):
        self.args, self.env, self.workspace = args, env, workspace
        self.label, self.evidence = label, evidence
        self.process = None
        self.endpoint = None
        self.port = None
        self.sequence = 0
        self.envelope = None
        self.token = secrets.token_urlsafe(48)
        evidence.tokens.append(self.token)
        self.ready = threading.Event()
        self.threads = []
        self.record = {"label": label, "workspace": str(workspace), "owned": True}
        evidence.receipt["servers"].append(self.record)

    def _drain(self, stream, channel):
        with (self.evidence.output / f"{self.label}.{channel}.log").open("w", encoding="utf-8") as log:
            while True:
                line = stream.readline(1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 1024 * 1024 or not line.endswith(b"\n"):
                    while line and not line.endswith(b"\n"):
                        line = stream.readline(1024 * 1024 + 1)
                    log.write("[omitted oversized or incomplete diagnostic line]\n")
                    log.flush()
                    continue
                text = self.evidence.scrub(line.decode("utf-8", errors="replace"))
                log.write(text)
                log.flush()
                match = re.search(r"MCP_HTTP_LISTENING: (http://127\.0\.0\.1:(\d+)/mcp)", text)
                if channel == "stderr" and match:
                    self.endpoint, self.port = match.group(1), int(match.group(2))
                    self.ready.set()

    def start(self):
        child_env = dict(self.env)
        child_env["FORGE_MCP_ACCESS_TOKEN"] = self.token
        command = [str(self.args.server), "--workspace", str(self.workspace), "--ui-data",
                   str(self.args.ydwe_root / "ui"), "--http-port", "0", "--http-token-env",
                   "FORGE_MCP_ACCESS_TOKEN"]
        self.record["command"] = command  # 仅环境变量名称，绝不包含值。
        self.process = subprocess.Popen(command, env=child_env, creationflags=FLAGS,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.record["pid"] = self.process.pid
        self.evidence.checkpoint(f"{self.label}-started")
        for channel in ("stdout", "stderr"):
            thread = threading.Thread(target=self._drain, args=(getattr(self.process, channel), channel), daemon=True)
            thread.start()
            self.threads.append(thread)
        deadline = time.monotonic() + self.args.start_timeout
        while not self.ready.wait(0.1):
            require(self.process.poll() is None, f"{self.label} server exited before HTTP listening")
            require(time.monotonic() < deadline, f"{self.label} HTTP startup timed out")
        require(0 < self.port <= 65535, "invalid loopback listening port")
        self.record["endpoint"] = self.endpoint
        initialized = self.request("initialize", {
            "protocolVersion": "2025-03-26", "capabilities": {},
            "clientInfo": {"name": "audastra-object-http-probe", "version": "1"},
        })
        require(initialized.get("protocolVersion") == "2025-03-26", "protocol negotiation changed unexpectedly")
        self.request("notifications/initialized", {}, notification=True)
        listed = self.request("tools/list", {})
        self.evidence.artifact(f"{self.label}.tools-list.json", listed)
        names = {tool.get("name") for tool in listed.get("tools", [])}
        required = OBJECT_TOOLS | {"get_revision", "preview_edit", "validate_edit", "rollback_edit", "save_map_edit_candidate"}
        require(required <= names, f"{self.label} required tools missing: {sorted(required - names)}")
        revision = self.tool("get_revision", {})
        require(revision.get("identity_checked") is True and revision.get("identity_current") is True,
                "actual workspace identity is not current")
        self.record["initial_revision"] = revision
        self.evidence.check(f"{self.label}-http-initialized-and-workspace-loaded", {"tool_count": len(names), "revision": revision})
        return self

    def request(self, method, params, notification=False, timeout=None, expected_rpc_error=None):
        if expected_rpc_error is not None:
            require(not notification and isinstance(expected_rpc_error, dict)
                    and set(expected_rpc_error) == {"code", "message"}
                    and type(expected_rpc_error["code"]) is int
                    and isinstance(expected_rpc_error["message"], str),
                    "expected RPC error must explicitly name one code/message for a request")
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notification:
            self.sequence += 1
            message["id"] = self.sequence
        self.evidence.event({"server": self.label, "request": message})
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout or self.args.request_timeout)
        started = time.monotonic()
        try:
            connection.request("POST", "/mcp", body=json.dumps(message, ensure_ascii=False).encode("utf-8"), headers={
                "Authorization": "Bearer " + self.token,
                "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                "MCP-Protocol-Version": "2025-03-26",
            })
            response = connection.getresponse()
            raw = response.read(MAX_RESPONSE + 1)
            require(len(raw) <= MAX_RESPONSE, "HTTP response exceeds 8 MiB budget")
            if notification:
                self.evidence.event({"server": self.label, "http_status": response.status,
                                     "notification": method, "seconds": time.monotonic() - started})
                require(response.status == 202 and not raw, "initialized notification must return empty HTTP 202")
                return None
            require(response.status == 200, f"HTTP status {response.status}: {raw[:4096].decode('utf-8', errors='replace')}")
            require(response.getheader("Content-Type", "").split(";")[0].strip() == "application/json",
                    "unexpected HTTP response content type")
            reply = json.loads(raw)
            self.evidence.event({"server": self.label, "http_status": response.status, "response": reply,
                                 "seconds": time.monotonic() - started})
            if expected_rpc_error is not None:
                require(isinstance(reply, dict) and set(reply) == {"jsonrpc", "id", "error"}
                        and reply["jsonrpc"] == "2.0" and type(reply["id"]) is int
                        and reply["id"] == message["id"] and isinstance(reply["error"], dict)
                        and set(reply["error"]) == {"code", "message"}
                        and type(reply["error"]["code"]) is int
                        and reply["error"] == expected_rpc_error,
                        "expected JSON-RPC error did not exactly match the response envelope")
                return reply
            require(isinstance(reply, dict) and reply.get("id") == message["id"] and "error" not in reply,
                    "JSON-RPC error or mismatched response id")
            require(isinstance(reply.get("result"), dict), "JSON-RPC result is not an object")
            return reply["result"]
        except Exception as error:
            self.evidence.event({"server": self.label, "transport_error": str(error), "method": method})
            raise
        finally:
            connection.close()

    def raw_tool(self, name, arguments, timeout=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments}, timeout=timeout)
        content = result.get("content")
        require(isinstance(content, list) and len(content) == 1 and content[0].get("type") == "text",
                f"{name} did not return exactly one text business result")
        detail = json.loads(content[0]["text"])
        require(isinstance(detail, dict), f"{name} business result is not an object")
        return result, detail

    def tool(self, name, arguments, timeout=None):
        result, detail = self.raw_tool(name, arguments, timeout)
        require(not result.get("isError") and detail.get("ok") is True, f"{name} business failure: {detail}")
        if detail.get("transaction_active") and isinstance(detail.get("transaction_revision"), dict):
            self.envelope = {"transaction_id": detail["transaction_id"], "expected_revision": detail["transaction_revision"]}
        return detail

    def rejection(self, name, arguments, code=None):
        result, detail = self.raw_tool(name, arguments)
        require(result.get("isError") is True and detail.get("ok") is False, f"{name} unexpectedly accepted rejection probe")
        if code:
            require(any(row.get("code") == code for row in detail.get("diagnostics", [])),
                    f"{name} rejected for an unexpected reason: {detail}")
        return detail

    def rollback(self):
        if self.envelope is not None:
            self.tool("rollback_edit", self.envelope, timeout=15)
            self.envelope = None

    def close(self):
        if self.process is None:
            return
        if self.process.poll() is None:
            try:
                self.rollback()
            except Exception as error:
                self.evidence.receipt["cleanup_errors"].append({"server": self.label, "rollback_error": str(error)})
            # HTTP 入口没有 shutdown RPC；终止的仅是本脚本创建、仍由 Popen 持有的 PID。
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
                self.record["owned_process_killed"] = True
            self.record["owned_process_terminated"] = True
        self.record["exit_code"] = self.process.returncode
        for thread in self.threads:
            thread.join(timeout=2)
        self.evidence.checkpoint()


def run_cli(args, env, evidence, *arguments):
    command = [str(args.cli), *(str(argument) for argument in arguments)]
    started = time.monotonic()
    done = subprocess.run(command, env=env, creationflags=FLAGS, capture_output=True,
                          timeout=args.cli_timeout)
    require(len(done.stdout) <= MAX_RESPONSE and len(done.stderr) <= MAX_RESPONSE, "CLI output budget exceeded")
    record = {"arguments": command[1:], "exit_code": done.returncode, "seconds": time.monotonic() - started}
    evidence.receipt["cli_calls"].append(record)
    if done.returncode != 0:
        record["stdout"] = done.stdout.decode("utf-8", errors="replace")
        record["stderr"] = done.stderr.decode("utf-8", errors="replace")
        evidence.checkpoint("cli-failed")
    require(done.returncode == 0, f"CLI failed: {record}")
    return done.stdout.decode("utf-8", errors="strict")


def list_members(args, env, evidence, archive, label):
    output = run_cli(args, env, evidence, "mpq-list", archive)
    lines = output.splitlines()
    require(lines, "empty MPQ listing")
    summary = re.fullmatch(r"total=(\d+) shown=(\d+)", lines[-1])
    require(summary is not None, "MPQ listing summary missing")
    names = lines[:-1]
    require(int(summary.group(1)) == int(summary.group(2)) == len(names), "MPQ enumeration count mismatch")
    require(0 < len(names) <= 16384, "MPQ member count outside probe budget")
    require(len({normalized_member(name) for name in names}) == len(names), "ambiguous MPQ member aliases")
    for name in names:
        safe_member(name)
    evidence.artifact(f"{label}.member-list.json", {"archive": str(archive), "enumeration": "existing-cli-mpq-list",
                       "member_count": len(names), "names": names, "anonymous_member_coverage_proven": False})
    return {normalized_member(name): name for name in names}


def extract_members(args, env, evidence, archive, members, destination, label):
    destination.mkdir(parents=True, exist_ok=False)
    manifest = []
    for name in members:
        relative = safe_member(name)
        target = destination / relative
        require(target.resolve().is_relative_to(destination.resolve()), "MPQ target escapes evidence root")
        target.parent.mkdir(parents=True, exist_ok=True)
        run_cli(args, env, evidence, "mpq-extract", archive, name, target)
        require(target.is_file(), "CLI extract produced no file")
        manifest.append({"member": name, "bytes": target.stat().st_size, "sha256": sha(target)})
    evidence.artifact(f"{label}.manifest.json", {"archive": str(archive), "workspace": str(destination),
                       "member_count": len(manifest), "members": manifest})
    evidence.check(f"{label}-cli-extracted", {"member_count": len(manifest), "manifest_sha256": json_sha(manifest)})
    return {normalized_member(row["member"]): row for row in manifest}


def list_objects(server, kind, scope="all"):
    items, first = [], None
    for page in range(MAX_PAGES):
        reply = server.tool("objects_list", {"kind": kind, "scope": scope, "page": {"offset": len(items), "limit": 128}})
        if first is None:
            first = reply
        require(reply.get("revision") == first.get("revision") and reply.get("project_identity") == first.get("project_identity"),
                "object list identity changed during pagination")
        rows = reply.get("items")
        require(isinstance(rows, list) and len(rows) <= 128, "object list page invalid")
        require(reply.get("offset") == len(items) and reply.get("total") == first.get("total"), "object list pagination changed")
        items.extend(rows)
        if not reply.get("truncated"):
            require(len(items) == reply.get("total"), "object list was not read completely")
            require(len({row["id"] for row in items}) == len(items), "duplicate object id in pagination")
            return items
        require(rows, "truncated object page made no progress")
    raise RuntimeError("object list exceeds bounded pagination budget")


def get_fields(server, object_id, fields=None):
    rows, first = [], None
    for page in range(16):
        arguments = {"kind": "unit", "id": object_id, "page": {"offset": len(rows), "limit": 128}}
        if fields is not None:
            arguments["fields"] = fields
        reply = server.tool("objects_get", arguments)
        if first is None:
            first = reply
        require(reply["revision"] == first["revision"] and reply["project_identity"] == first["project_identity"],
                "object fields identity changed during pagination")
        require(isinstance(reply.get("fields"), list) and len(reply["fields"]) <= 128, "object field page invalid")
        require(reply.get("offset") == len(rows) and reply.get("total") == first.get("total"),
                "object field pagination changed")
        rows.extend(reply["fields"])
        if not reply.get("truncated"):
            require(len(rows) == reply.get("total"), "object fields were not read completely")
            first["fields"] = rows
            return first
        require(reply["fields"], "truncated field page made no progress")
    raise RuntimeError("object fields exceed bounded pagination budget")


def address(field):
    result = {"field": field["field"]}
    for key in ("level", "variation"):
        if key in field:
            require(isinstance(field[key], int) and field[key] > 0, "getter emitted an invalid scalar zero address")
            result[key] = field[key]
    return result


def identity(reply):
    require(isinstance(reply.get("project_identity"), str) and isinstance(reply.get("revision"), str),
            "object query omitted string identity/revision")
    return {"project_identity": reply["project_identity"], "expected_revision": reply["revision"]}


def choose_field(server, units):
    custom = [row for row in units if row.get("source") == "custom"]
    require(custom, "no actual custom unit available; this probe will not guess or edit a standard object")
    for unit in custom[:128]:
        reply = get_fields(server, unit["id"], ["unam", "uhpm"])
        for field_id in ("unam", "uhpm"):
            for field in reply["fields"]:
                if field.get("field") != field_id or field.get("editable") is not True or field.get("ordered_list"):
                    continue
                old = field.get("value")
                if field_id == "unam" and isinstance(old, str):
                    new = old + " [夜间HTTP物编验证]"
                    if len(new.encode("utf-8")) > 65536:
                        continue
                elif field_id == "uhpm" and isinstance(old, int) and not isinstance(old, bool):
                    minimum = float(field.get("minimum") or -(2**31))
                    maximum = float(field.get("maximum") or 2**31 - 1)
                    require(math.isfinite(minimum) and math.isfinite(maximum), "HP field has non-finite metadata range")
                    candidates = [value for value in (old + 1, old - 1) if minimum <= value <= maximum and -(2**31) <= value < 2**31]
                    if not candidates:
                        continue
                    new = candidates[0]
                else:
                    continue
                address(field)
                return unit, field, new, get_fields(server, unit["id"])
    raise RuntimeError("bounded actual custom-unit scan found no editable name/HP field")


def choose_persistent_source(server, units):
    matches = [row for row in units if row.get("id") == "E000" and row.get("source") == "custom"]
    require(len(matches) == 1, "persistent clone gate requires the actual unique custom source E000")
    before = get_fields(server, "E000")
    health = [row for row in before["fields"] if row.get("field") == "uhpm"]
    names = [row for row in before["fields"] if row.get("field") == "unam"]
    require(len(health) == len(names) == 1 and health[0].get("editable") is True and
            names[0].get("editable") is True and isinstance(names[0].get("value"), str),
            "E000 must expose actual unique editable health and name fields")
    field = health[0]
    old = field.get("value")
    require(isinstance(old, int) and not isinstance(old, bool), "E000 health must be an actual integer value")
    minimum = float(field.get("minimum") or -(2**31))
    maximum = float(field.get("maximum") or 2**31 - 1)
    require(math.isfinite(minimum) and math.isfinite(maximum), "E000 health metadata range must be finite")
    candidates = [value for value in (old + 1, old - 1)
                  if minimum <= value <= maximum and -(2**31) <= value < 2**31]
    require(candidates, "E000 health has no legal non-no-op probe value")
    address(field)
    return matches[0], field, candidates[0], before


def read_native_units(path):
    # 本探针只读已由 CLI 提取的经典 v2 w3u；不读 WTG/WCT/AI/BLP 正文。
    require(0 < path.stat().st_size <= MAX_NATIVE_BYTES, "native unit member exceeds the 64 MiB probe budget")
    with path.open("rb") as stream:
        data = stream.read(MAX_NATIVE_BYTES + 1)
    require(len(data) <= MAX_NATIVE_BYTES, "native unit member grew beyond the probe budget")
    position, total_modifications = 0, 0

    def take(size):
        nonlocal position
        require(0 <= size <= len(data) - position, "truncated native unit record")
        result = data[position:position + size]
        position += size
        return result

    def integer():
        return struct.unpack("<i", take(4))[0]

    version = integer()
    require(version == 2, "persistent clone wire proof supports only actual classic v2 w3u")
    records = {}
    counts = {}
    for table in ("original", "custom"):
        count = integer()
        require(0 <= count <= 65536 and count <= (len(data) - position) // 12,
                "native unit object count exceeds the bounded remaining input")
        counts[table] = count
        for _ in range(count):
            start = position
            parent, new_id = take(4), take(4)
            require(b"\0" not in parent and (new_id == b"\0" * 4 if table == "original" else b"\0" not in new_id),
                    "native unit original/custom rawcode structure is invalid")
            effective = parent if table == "original" else new_id
            require(effective not in records, "native unit object ID is ambiguous")
            modifications = integer()
            require(0 <= modifications <= 65536 and modifications <= (len(data) - position) // 13,
                    "native unit modification count exceeds the bounded remaining input")
            total_modifications += modifications
            require(total_modifications <= 1000000, "native unit aggregate modification budget exceeded")
            fields, field_addresses = {}, set()
            for _ in range(modifications):
                field, wire_tag = take(4), integer()
                field_address = field.rstrip(b"\0")  # 与 native codec 一致，允许短字段的尾部 NUL 填充。
                require(field_address not in field_addresses, "native unit field address is ambiguous")
                field_addresses.add(field_address)
                require(wire_tag in (0, 1, 2, 3), "unknown native unit wire type cannot be safely parsed")
                if wire_tag == 3:
                    end = data.find(b"\0", position, min(len(data), position + 1024 * 1024 + 1))
                    require(end >= position, "native unit string is unterminated or exceeds the 1 MiB probe budget")
                    value = take(end - position + 1)
                else:
                    value = take(4)
                marker = take(4)
                require(marker in (b"\0" * 4, parent, new_id), "native unit field end marker is invalid")
                # Float32 的原始 bits 和真实 tag 单独保留，不使用 Python 数值推断 Real/Unreal。
                fields[field] = {"wire_tag": wire_tag, "value_bytes": value, "end_marker": marker}
            records[effective] = {"table": table, "parent": parent, "new_id": new_id,
                                  "fields": fields, "record_bytes": data[start:position]}
    require(position == len(data), "native unit member contains unsupported trailing bytes")
    return {"version": version, "counts": counts, "records": records,
            "sha256": hashlib.sha256(data).hexdigest()}


def verify_persisted_native_clone(source, candidate, clone_id, selected, new_value):
    original_id, clone_key = b"E000", clone_id.encode("ascii", errors="strict")
    old_records, new_records = source["records"], candidate["records"]
    require(original_id in old_records and clone_key not in old_records and len(clone_key) == 4,
            "persistent clone ID must be unique against the actual source native table")
    require(new_records.keys() == old_records.keys() | {clone_key},
            "candidate native unit ID set differs from source plus the one persistent clone")
    original, edited, clone = old_records[original_id], new_records[original_id], new_records[clone_key]
    require(original["table"] == edited["table"] == clone["table"] == "custom" and
            edited["parent"] == clone["parent"] == original["parent"] and edited["new_id"] == original["new_id"],
            "saved source/clone native ancestry changed")
    health = selected["field"].encode("ascii", errors="strict")
    require(health == b"uhpm" and not any(selected.get(key, 0) for key in ("level", "variation", "data_index")),
            "persistent clone gate requires the actual scalar health address")
    old_fields, edited_fields, clone_fields = original["fields"], edited["fields"], clone["fields"]
    require(edited_fields.keys() == old_fields.keys() | {health}, "saved E000 native field set changed beyond health")
    require([field for field in edited_fields if field != health] == [field for field in old_fields if field != health],
            "saved E000 reordered native fields outside health")
    require(all(edited_fields[field] == row for field, row in old_fields.items() if field != health),
            "saved E000 native value/type/marker changed outside health")
    require(edited_fields[health]["wire_tag"] == 0 and
            struct.unpack("<i", edited_fields[health]["value_bytes"])[0] == new_value,
            "saved E000 native health is not the requested actual Int32 value")
    if health in old_fields:
        require(edited_fields[health]["wire_tag"] == old_fields[health]["wire_tag"] and
                edited_fields[health]["end_marker"] == old_fields[health]["end_marker"] and
                list(edited_fields) == list(old_fields),
                "health edit altered the original native storage type or end marker")
    else:
        require(edited_fields[health]["end_marker"] == original_id and list(edited_fields) == list(old_fields) + [health],
                "new health field has an unexpected native marker or position")
    require(clone_fields.keys() == old_fields.keys() | {b"unam"}, "clone native fields differ beyond its explicit name")
    require([field for field in clone_fields if field != b"unam"] == [field for field in old_fields if field != b"unam"],
            "clone reordered native fields outside its explicit name")
    for field, row in old_fields.items():
        if field == b"unam":
            continue
        actual = clone_fields[field]
        expected_marker = clone_key if row["end_marker"] == original_id else row["end_marker"]
        require(actual["wire_tag"] == row["wire_tag"] and actual["value_bytes"] == row["value_bytes"] and
                actual["end_marker"] == expected_marker,
                f"clone failed to preserve actual source native wire storage for {field.hex()}")
    require(clone_fields[b"unam"]["wire_tag"] == 3 and
            clone_fields[b"unam"]["value_bytes"] == PERSIST_CREATED_NAME.encode("utf-8") + b"\0",
            "persisted clone native name is not the requested real string")
    if b"unam" in old_fields:
        name_marker = old_fields[b"unam"]["end_marker"]
        expected_name_marker = clone_key if name_marker == original_id else name_marker
        require(old_fields[b"unam"]["wire_tag"] == 3 and clone_fields[b"unam"]["end_marker"] == expected_name_marker and
                list(clone_fields) == list(old_fields),
                "explicit clone name edit altered its actual native storage type, marker or position")
    else:
        require(clone_fields[b"unam"]["end_marker"] == clone_key and list(clone_fields) == list(old_fields) + [b"unam"],
                "new clone name has an unexpected native marker or position")
    for object_id, record in old_records.items():
        if object_id != original_id:
            require(new_records[object_id]["record_bytes"] == record["record_bytes"],
                    "another source native unit record changed")
    wire_fields = []
    for field, expected_tag in ((b"udef", 2), (b"udup", 2), (b"usca", 1)):
        require(field in old_fields and old_fields[field]["wire_tag"] == expected_tag,
                f"actual E000 regression field {field.decode('ascii')} has not established its required native wire tag")
        actual = clone_fields[field]
        wire_fields.append({"field": field.decode("ascii"), "source_wire_tag": old_fields[field]["wire_tag"],
                            "clone_wire_tag": actual["wire_tag"], "source_value_bytes_hex": old_fields[field]["value_bytes"].hex(),
                            "clone_value_bytes_hex": actual["value_bytes"].hex()})
    return {"source_id": "E000", "clone_id": clone_id, "clone_parent_rawcode_hex": clone["parent"].hex(),
            "source_native_sha256": source["sha256"], "candidate_native_sha256": candidate["sha256"],
            "source_native_object_count": len(old_records), "candidate_native_object_count": len(new_records),
            "all_source_objects_except_E000_health_byte_preserved": True,
            "clone_all_source_fields_except_explicit_name_wire_preserved": True, "actual_wire_fields": wire_fields}


def field_key(field):
    return (field["field"], field.get("level", 0), field.get("variation", 0), field.get("data_index", 0))


def check_single_field(before, after, selected, expected):
    old = {field_key(row): row for row in before["fields"]}
    new = {field_key(row): row for row in after["fields"]}
    require(len(old) == len(before["fields"]) and len(new) == len(after["fields"]), "ambiguous duplicate field addresses")
    key = field_key(selected)
    require(old.keys() == new.keys() and key in new, "field set changed outside the selected address")
    require(new[key].get("value") == expected, "selected field readback value differs")
    require(all(old[item] == new[item] for item in old if item != key), "an unrelated object field changed")
    return new[key]


def create_delete_gate(server, evidence, unit, selected, before):
    parameters = {"kind": "unit", "base_id": unit["id"], **identity(before)}
    if selected["field"] == "unam":
        parameters["name"] = "夜间HTTP创建与删除门禁"
    created = server.tool("objects_create", parameters)
    require(created.get("changed") is True and created.get("saved") is False and created.get("stable_id"),
            "creation did not enter an unsaved stable object transaction")
    require(server.envelope is not None, "creation did not return the typed transaction envelope")
    created_id = created["id"]
    readback = get_fields(server, created_id)
    require(readback.get("id") == created_id and readback["fields"], "created object is not queryable")
    rejected = server.rejection("objects_delete", {"kind": "unit", "id": created_id, **identity(readback)})
    audit = rejected.get("reference_audit")
    require(isinstance(audit, dict) and audit.get("complete") is False and audit.get("coverage"),
            "delete refusal did not expose incomplete reference coverage")
    kept = get_fields(server, created_id)
    require(kept["fields"] == readback["fields"] and kept["revision"] == readback["revision"],
            "refused deletion changed the created object")
    evidence.check("separate-create-and-delete-gate", {"created": created, "delete_refusal": rejected})
    server.rollback()
    server.rejection("objects_get", {"kind": "unit", "id": created_id})
    restored = get_fields(server, unit["id"])
    require(restored["fields"] == before["fields"], "creation rollback changed the original unit")
    evidence.check("create-transaction-rolled-back", {"created_id_absent": created_id})
    return created_id, restored


def persist_created_gate(server, evidence, before, source_ids):
    created = server.tool("objects_create", {"kind": "unit", "base_id": "E000",
                                           "name": PERSIST_CREATED_NAME, **identity(before)})
    clone_id = created.get("id")
    require(isinstance(clone_id, str) and re.fullmatch(r"[ -~]{4}", clone_id) is not None and clone_id not in source_ids,
            "persistent creation did not allocate an actual unique four-byte ID")
    require(created.get("changed") is True and created.get("saved") is False and created.get("stable_id") and
            source_ids | {clone_id} == {row["id"] for row in list_objects(server, "unit")} and server.envelope is not None,
            "persistent clone did not enter the same typed transaction or changed another unit identity")
    readback = get_fields(server, clone_id)
    names = [row for row in readback["fields"] if row.get("field") == "unam"]
    require(readback.get("id") == clone_id and len(names) == 1 and names[0].get("value") == PERSIST_CREATED_NAME,
            "persistent clone name was not actually read back")
    source_readback = get_fields(server, "E000")
    require(source_readback["fields"] == before["fields"], "persistent creation changed original E000")
    evidence.artifact("persisted-clone.overlay.json", readback)
    evidence.check("persistent-clone-created-in-main-save-transaction", {"created": created,
                   "clone_id": clone_id, "actual_name": names[0], "original_E000_fields_unchanged": True})
    return {"id": clone_id, "name": PERSIST_CREATED_NAME, "created": created, "readback": readback}, source_readback


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "workspace", "ydwe-root", "war3-root", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--qt-bin", type=Path, help="仅向自有子进程 PATH 添加 Qt 运行库目录")
    parser.add_argument("--exercise-create-delete", action="store_true", help="在独立事务验证创建和删除拒绝，再回滚")
    parser.add_argument("--persist-created-object", action="store_true",
                        help="仅配合--exercise-create-delete：在主保存事务保留第二次自动ID的E000克隆，并核验实际native wire类型")
    parser.add_argument("--expected-source-sha256", help="可选的已授权源归档身份；不符时在启动服务前拒绝")
    parser.add_argument("--start-timeout", type=float, default=60)
    parser.add_argument("--request-timeout", type=float, default=60)
    parser.add_argument("--save-timeout", type=float, default=900)
    parser.add_argument("--cli-timeout", type=float, default=60)
    args = parser.parse_args()
    if args.persist_created_object and not args.exercise_create_delete:
        parser.error("--persist-created-object requires --exercise-create-delete")
    for name in ("start_timeout", "request_timeout", "save_timeout", "cli_timeout"):
        require(getattr(args, name) > 0, "timeouts must be positive")
    args.output = args.output.resolve()
    require(not args.output.is_relative_to(args.workspace.resolve()),
            "evidence output must be outside the input workspace")
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = Evidence(args.output)
    servers, source_before, workspace_before = [], None, None
    try:
        for name in ("server", "cli", "source", "workspace", "ydwe_root", "war3_root", "qt_bin"):
            value = getattr(args, name)
            if value is not None:
                setattr(args, name, value.resolve(strict=True))
        require(args.server.is_file() and args.cli.is_file() and args.source.is_file(), "binary/source path is not a file")
        require(args.source.suffix.lower() in (".w3x", ".w3m"), "source must be a map archive")
        require(args.workspace.is_dir() and (args.ydwe_root / "ui").is_dir() and args.war3_root.is_dir(),
                "workspace or trusted root/ui directory is missing")
        require(not args.output.is_relative_to(args.workspace), "evidence output must be outside the input workspace")
        source_before = sha(args.source)
        if args.expected_source_sha256 is not None:
            require(re.fullmatch(r"[0-9a-f]{64}", args.expected_source_sha256) is not None,
                    "expected source SHA-256 must be 64 lowercase hex digits")
            require(source_before == args.expected_source_sha256, "authorized source archive SHA-256 mismatch")
        workspace_before = inventory(args.workspace)
        evidence.receipt.update({"source": str(args.source), "workspace": str(args.workspace),
            "source_sha256_before": source_before, "server": str(args.server), "server_sha256": sha(args.server),
            "cli": str(args.cli), "cli_sha256": sha(args.cli), "ydwe_root": str(args.ydwe_root),
            "ui_data": str(args.ydwe_root / "ui"), "war3_root": str(args.war3_root),
            "exercise_create_delete": args.exercise_create_delete,
            "persist_created_object": args.persist_created_object,
            "expected_source_sha256": args.expected_source_sha256,
            "workspace_inventory_sha256_before": json_sha(workspace_before)})
        evidence.artifact("source-workspace.before.json", workspace_before)
        evidence.checkpoint("input-identities-captured")
        env = dict(os.environ)
        env.pop("FORGE_MCP_ACCESS_TOKEN", None)
        path_entries = [str(args.server.parent), str(args.cli.parent)]
        if args.qt_bin:
            require(args.qt_bin.is_dir(), "Qt runtime directory does not exist")
            path_entries.insert(0, str(args.qt_bin))
        env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
        env["FORGE_MCP_YDWE_ROOT"] = str(args.ydwe_root)
        env["FORGE_MCP_WAR3_ROOT"] = str(args.war3_root)

        source_members = list_members(args, env, evidence, args.source, "source")
        require(all(name in source_members for name in ("war3map.wtg", "war3map.wct", "war3map.w3u")),
                "actual archive lacks required trigger pair/custom-unit native member")
        audit_names = [source_members[name] for name in PROTECTED_MEMBERS + OBJECT_MEMBERS if name in source_members]
        source_audit = extract_members(args, env, evidence, args.source, audit_names,
                                       args.output / "source-member-hashes", "source-audit")
        source_native = None
        if args.persist_created_object:
            source_native = read_native_units(args.output / "source-member-hashes" /
                                             safe_member(source_members["war3map.w3u"]))
            require(source_native["sha256"] == source_audit["war3map.w3u"]["sha256"],
                    "native wire proof source differs from the actually extracted source SHA")
        source_server = HttpServer(args, env, args.workspace, "source", evidence)
        servers.append(source_server)
        source_server.start()
        all_objects = {kind: list_objects(source_server, kind) for kind in KINDS}
        evidence.check("seven-kinds-complete-paginated-list", {
            kind: {"total": len(items), "rows_sha256": json_sha(items)} for kind, items in all_objects.items()})
        selector = choose_persistent_source if args.persist_created_object else choose_field
        unit, selected, new_value, before = selector(source_server, all_objects["unit"])
        evidence.receipt["selected"] = {"kind": "unit", "id": unit["id"], "stable_id": unit.get("stable_id"),
            "field": selected, "before_value": selected["value"], "after_value": new_value}
        evidence.artifact("selected-object.before.json", before)
        evidence.checkpoint("actual-editable-custom-unit-field-selected")
        option_field = next((row for row in before["fields"] if row.get("editable") is True and
                             (row.get("type") in ("bool", "boolean") or row.get("ordered_list"))), selected)
        options = source_server.tool("objects_field_options", {"kind": "unit", "id": unit["id"],
                                     "field": option_field["field"], "page": {"offset": 0, "limit": 128}})
        require(isinstance(options.get("items"), list), "field options result has no actual option array")
        evidence.check("actual-field-options", {"field": option_field["field"], "result": options})

        created_id = None
        persisted = None
        if args.exercise_create_delete:
            created_id, before = create_delete_gate(source_server, evidence, unit, selected, before)
        valid_change = {**address(selected), "value": new_value}
        alternative = next((row for row in before["fields"] if row.get("editable") is True and
                            address(row) != address(selected) and isinstance(row.get("value"), (str, bool, int, float, list))), None)
        if alternative is not None:
            bad_value = "not-a-boolean-or-ordered-array" if isinstance(alternative["value"], (bool, list)) else True
            bad_tail = {**address(alternative), "value": bad_value}
            bad_tail_scope = "valid-prefix-and-distinct-actual-field-with-invalid-type"
        else:
            bad_tail = {**address(selected), "value": selected["value"]}
            bad_tail_scope = "valid-prefix-and-duplicate-exact-field-address"
        base_parameters = {"kind": "unit", "id": unit["id"], **identity(before)}
        rejected = source_server.rejection("objects_set_fields", {**base_parameters, "changes": [valid_change, bad_tail]})
        unchanged = get_fields(source_server, unit["id"])
        require(unchanged["fields"] == before["fields"] and unchanged["revision"] == before["revision"],
                "bad-last batch partially published or changed revision")
        evidence.check("bad-last-batch-atomic-refusal", {"scope": bad_tail_scope, "refusal": rejected,
                                                       "all_original_fields_unchanged": True})
        duplicate = source_server.rejection("objects_set_fields", {**base_parameters, "changes": [valid_change, valid_change]},
                                           "OBJECT_DUPLICATE_FIELD")
        unchanged_again = get_fields(source_server, unit["id"])
        require(unchanged_again["fields"] == before["fields"] and unchanged_again["revision"] == before["revision"],
                "duplicate-address batch changed the unit")
        evidence.check("duplicate-field-batch-refused", duplicate)

        if args.persist_created_object:
            persisted, before = persist_created_gate(source_server, evidence, before,
                                                     {row["id"] for row in all_objects["unit"]})
            base_parameters = {"kind": "unit", "id": unit["id"], **identity(before)}
            evidence.receipt["persisted_clone"] = {"id": persisted["id"], "name": persisted["name"],
                "temporary_probe_id_reused_after_rollback": persisted["id"] == created_id}

        edited = source_server.tool("objects_set_fields", {**base_parameters, "changes": [valid_change]})
        require(edited.get("changed") is True and edited.get("saved") is False and source_server.envelope is not None,
                "valid edit did not publish an unsaved typed transaction")
        changes = edited.get("changes", {}).get("items", [])
        require(len(changes) == 1 and changes[0].get("field") == selected["field"],
                "main save transaction contains more than one field change")
        after = get_fields(source_server, unit["id"])
        check_single_field(before, after, selected, new_value)
        evidence.check("one-legal-field-edited", edited)
        # 当前有成功编辑的 overlay；恢复旧值的合法前缀必须是实际变更，不能以 no-op 冒充原子性。
        require(alternative is not None, "edited-overlay atomicity needs a distinct actual editable tail field")
        require(selected["value"] != new_value, "restore prefix would be a no-op")
        restore_prefix = {**address(selected), "value": selected["value"]}
        overlay_rejected = source_server.rejection("objects_set_fields", {
            "kind": "unit", "id": unit["id"], **identity(after), "changes": [restore_prefix, bad_tail]})
        overlay_codes = {row.get("code") for row in overlay_rejected.get("diagnostics", [])}
        require(overlay_codes & {"OBJECT_BOOLEAN_REQUIRED", "OBJECT_ORDERED_LIST_REQUIRED",
                                "OBJECT_INTEGER_REQUIRED", "OBJECT_REAL_REQUIRED", "OBJECT_STRING_REQUIRED",
                                "OBJECT_REFERENCE_NOT_FOUND", "OBJECT_ENUM_INVALID"},
                "edited-overlay bad-tail probe did not reach actual field-value validation")
        require(not overlay_codes & {"OBJECT_REVISION_CONFLICT", "OBJECT_DUPLICATE_FIELD"},
                "edited-overlay bad-tail probe was blocked before field-value validation")
        after_overlay_refusal = get_fields(source_server, unit["id"])
        require(after_overlay_refusal["fields"] == after["fields"] and
                after_overlay_refusal["revision"] == after["revision"],
                "bad-last batch reverted or partially changed the already edited overlay")
        evidence.check("edited-overlay-non-no-op-prefix-bad-last-atomic-refusal", {
            "restore_prefix": restore_prefix, "distinct_bad_tail": bad_tail, "refusal": overlay_rejected,
            "preserved_edited_value": new_value, "preserved_object_revision": after["revision"],
            "all_current_fields_unchanged": True})
        stale = source_server.rejection("objects_set_fields", {**base_parameters,
                                       "changes": [{**address(selected), "value": selected["value"]}]},
                                       "OBJECT_REVISION_CONFLICT")
        after_stale = get_fields(source_server, unit["id"])
        require(after_stale["fields"] == after["fields"] and after_stale["revision"] == after["revision"],
                "stale revision changed a current field or revision")
        evidence.check("old-object-revision-refused-current-value-preserved", stale)
        if persisted is not None:
            preserved_clone = get_fields(source_server, persisted["id"])
            require(preserved_clone["fields"] == persisted["readback"]["fields"],
                    "health edit or refusal gates changed the persistent clone")
            persisted["readback"] = preserved_clone
        preview = source_server.tool("preview_edit", source_server.envelope)
        require(len(preview.get("changes", [])) == 1 and preview["changes"][0].get("kind") == "object",
                "preview includes members beyond the single object change")
        validated = source_server.tool("validate_edit", source_server.envelope)
        require(validated.get("satisfied") is True and validated.get("highest_proven_level", -1) >= 0,
                "latest transaction generation did not validate")
        evidence.check("shared-transaction-preview-and-validation", {"envelope": source_server.envelope,
                       "preview": preview, "validation": validated})
        candidate = args.output / ("mcp-object-candidate" + args.source.suffix.lower())
        require(not candidate.exists(), "candidate already exists")
        saved = source_server.tool("save_map_edit_candidate", {"source_map_path": str(args.source),
            "output_path": str(candidate), "expected_source_sha256": source_before,
            **source_server.envelope, "script_target": "source"}, timeout=args.save_timeout)
        require(candidate.is_file() and saved.get("candidate_kind") == "mpq-map-archive" and
                saved.get("source_sha256") == source_before and saved.get("candidate_sha256") == sha(candidate) and
                saved.get("script_rebuilt") is True and saved.get("native_editor_verified") is False and
                saved.get("game_runtime_verified") is False, "save receipt identities or acceptance claims are invalid")
        require(isinstance(saved.get("output_path"), str) and
                Path(saved["output_path"]).resolve(strict=True) == candidate.resolve(strict=True),
                "save receipt output_path does not identify the actual candidate")
        require(any(normalized_member(row.get("name", "")) == "war3map.w3u" and
                    row.get("original_sha256") == source_audit["war3map.w3u"]["sha256"] and
                    row.get("original_sha256") != row.get("saved_sha256") for row in saved.get("edited_members", [])),
                "save receipt did not bind the edited native unit member to its original SHA")
        evidence.receipt["saved"] = saved
        evidence.receipt["candidate"] = str(candidate)
        evidence.check("existing-save-service-produced-real-candidate", saved)
        source_server.rollback()
        rolled_back = get_fields(source_server, unit["id"])
        require(rolled_back["fields"] == before["fields"], "save transaction rollback did not restore source view")
        if persisted is not None:
            source_server.rejection("objects_get", {"kind": "unit", "id": persisted["id"]})
            require({row["id"] for row in list_objects(source_server, "unit")} ==
                    {row["id"] for row in all_objects["unit"]},
                    "main transaction rollback did not remove the persistent clone from the source view")
            evidence.check("persistent-clone-source-overlay-rolled-back-after-save", {"clone_id_absent": persisted["id"]})
        source_server.close()

        candidate_members = list_members(args, env, evidence, candidate, "candidate")
        candidate_workspace = args.output / "candidate-workspace"
        candidate_manifest = extract_members(args, env, evidence, candidate, list(candidate_members.values()),
                                             candidate_workspace, "candidate")
        unit_receipts = [row for row in saved.get("edited_members", [])
                         if normalized_member(row.get("name", "")) == "war3map.w3u"]
        require(len(unit_receipts) == 1 and "war3map.w3u" in candidate_manifest and
                unit_receipts[0].get("original_sha256") == source_audit["war3map.w3u"]["sha256"] and
                unit_receipts[0].get("saved_sha256") == candidate_manifest["war3map.w3u"]["sha256"],
                "exactly one native unit receipt must bind original and actual extracted candidate SHA")
        evidence.check("native-unit-receipt-bound-to-actual-extracted-member", unit_receipts[0])
        protected = {}
        for member in PROTECTED_MEMBERS:
            original, copied = source_audit.get(member), candidate_manifest.get(member)
            require((original is None) == (copied is None), f"protected member presence changed: {member}")
            require(original is None or original["sha256"] == copied["sha256"], f"protected member hash changed: {member}")
            protected[member] = {"present": original is not None, "source_sha256": original["sha256"] if original else None,
                                 "candidate_sha256": copied["sha256"] if copied else None, "body_inspected": False}
        for member in OBJECT_MEMBERS:
            if member == "war3map.w3u":
                require(member in candidate_manifest and source_audit[member]["sha256"] != candidate_manifest[member]["sha256"],
                        "selected native unit member did not change in candidate")
                continue
            original, copied = source_audit.get(member), candidate_manifest.get(member)
            require((original is None) == (copied is None) and (original is None or original["sha256"] == copied["sha256"]),
                    f"unmodified object category changed: {member}")
        evidence.check("protected-member-hashes-and-other-object-categories-preserved", protected)
        if persisted is not None:
            candidate_native = read_native_units(candidate_workspace / safe_member(candidate_members["war3map.w3u"]))
            require(candidate_native["sha256"] == candidate_manifest["war3map.w3u"]["sha256"],
                    "native wire proof candidate differs from the actually extracted candidate SHA")
            wire_proof = verify_persisted_native_clone(source_native, candidate_native, persisted["id"], selected, new_value)
            evidence.artifact("persisted-clone.native-wire-readback.json", wire_proof)
            evidence.check("persistent-clone-actual-native-wire-types-and-source-fields-preserved", wire_proof)
        candidate_server = HttpServer(args, env, candidate_workspace, "candidate", evidence)
        servers.append(candidate_server)
        candidate_server.start()
        reread = get_fields(candidate_server, unit["id"])
        readback_field = check_single_field(before, reread, selected, new_value)
        candidate_units = list_objects(candidate_server, "unit")
        expected_unit_ids = {row["id"] for row in all_objects["unit"]}
        if persisted is not None:
            expected_unit_ids.add(persisted["id"])
        require({row["id"] for row in candidate_units} == expected_unit_ids,
                "candidate leaked a creation or lost an object")
        require(candidate_server.record["initial_revision"]["revision"]["ui_catalog_sha256"] ==
                source_server.record["initial_revision"]["revision"]["ui_catalog_sha256"], "candidate reopened with another catalog")
        if created_id is not None and (persisted is None or created_id != persisted["id"]):
            candidate_server.rejection("objects_get", {"kind": "unit", "id": created_id})
        if persisted is not None:
            clone_reread = get_fields(candidate_server, persisted["id"])
            require(clone_reread["fields"] == persisted["readback"]["fields"],
                    "second actual HTTP server did not read back the saved clone fields")
            clone_names = [row for row in clone_reread["fields"] if row.get("field") == "unam"]
            require(len(clone_names) == 1 and clone_names[0].get("value") == PERSIST_CREATED_NAME,
                    "second actual HTTP server did not read back the saved clone name")
            evidence.artifact("persisted-clone.candidate-http-readback.json", clone_reread)
            evidence.check("second-http-server-read-back-persistent-clone", {"id": persisted["id"],
                "actual_name": clone_names[0], "exact_unit_id_set_is_source_plus_persistent_clone": True,
                "temporary_probe_id_reused_after_rollback": persisted["id"] == created_id})
        evidence.artifact("candidate-object.readback.json", reread)
        evidence.check("new-http-server-read-back-candidate-native-field", {
            "id": unit["id"], "field": readback_field, "candidate_sha256": sha(candidate),
            "unit_id_set_preserved": persisted is None, "original_unit_ids_preserved": True,
            "authorized_persistent_clone_id": persisted["id"] if persisted is not None else None,
            "created_probe_object_leaked": False})
        candidate_server.close()
        require(sha(args.source) == source_before, "source archive changed during HTTP probe")
        require(inventory(args.workspace) == workspace_before, "original expanded workspace changed during HTTP probe")
        require(not evidence.receipt["cleanup_errors"], "owned server transaction cleanup failed")
        evidence.receipt["passed"] = True
        evidence.checkpoint("passed-independent-http-and-archive-readback")
    except Exception as error:
        evidence.receipt["failed_stage"] = evidence.receipt["stage"]
        evidence.receipt["error"] = evidence.scrub(str(error))
        evidence.checkpoint("failed")
    finally:
        for server in reversed(servers):
            try:
                server.close()
            except Exception as error:
                evidence.receipt["cleanup_errors"].append({"server": server.label, "close_error": str(error)})
        try:
            if source_before is not None:
                source_after = sha(args.source)
                evidence.receipt["source_sha256_after"] = source_after
                evidence.receipt["source_unchanged"] = source_after == source_before
            if workspace_before is not None:
                after_inventory = inventory(args.workspace)
                evidence.artifact("source-workspace.after.json", after_inventory)
                evidence.receipt["workspace_inventory_sha256_after"] = json_sha(after_inventory)
                evidence.receipt["source_workspace_unchanged"] = after_inventory == workspace_before
        except Exception as error:
            evidence.receipt["cleanup_errors"].append({"identity_recheck_error": str(error)})
        if (evidence.receipt["cleanup_errors"] or evidence.receipt.get("source_unchanged") is False or
                evidence.receipt.get("source_workspace_unchanged") is False):
            evidence.receipt["passed"] = False
        evidence.checkpoint()
        evidence.log.close()
    print(json.dumps({"passed": evidence.receipt["passed"], "receipt": str(args.output / "receipt.json"),
                      "candidate": evidence.receipt.get("candidate"), "error": evidence.receipt.get("error")}, ensure_ascii=False))
    return 0 if evidence.receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
