"""P1 验收脚本共用的文件身份、参数路径和只运行自有命令工具。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # 回执不覆盖已有证据；失败后保留现场，换一个新目录重试。
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def read_json(path):
    with Path(path).open("r", encoding="utf-8-sig") as stream:
        return json.load(stream)


def relative_path(value):
    require(isinstance(value, str) and value and "\x00" not in value, "相对路径不能为空")
    win = PureWindowsPath(value)
    parts = value.replace("\\", "/").split("/")
    require(not win.is_absolute() and not win.drive and not value.startswith("/"), "拒绝绝对路径")
    require(all(part not in ("", ".", "..") and ":" not in part for part in parts), "拒绝越界、空路径段或 ADS")
    return Path(*parts)


def plain_path(path):
    path = Path(path).absolute()
    # Windows junction 与 symlink 都不能把读取/写入引出选定目录。
    for ancestor in (path, *path.parents):
        if ancestor.exists() or ancestor.is_symlink():
            info = ancestor.lstat()
            require(not ancestor.is_symlink() and not (getattr(info, "st_file_attributes", 0) & 0x400),
                    f"拒绝链接或 reparse point: {ancestor}")
    return path.resolve()


def separate_paths(source, target):
    source, target = plain_path(source), plain_path(target)
    require(source != target and source not in target.parents and target not in source.parents,
            "输出必须与只读来源分离，不能互为祖先目录")
    return source, target


class CommandCaptureError(ValueError):
    """保留失败子进程的原字节路径与严格解码诊断；不能据此提升验收状态。"""

    def __init__(self, message, diagnostics):
        self.diagnostics = diagnostics
        super().__init__(message + ": " + json.dumps(diagnostics, ensure_ascii=True, allow_nan=False))


def command_failure(argv, cwd, code, stdout, stderr, reason, decode_errors, evidence_root):
    # 每个失败独占新目录，只保存本次捕获；不会向来源或现有回执写入。
    parent = plain_path(evidence_root if evidence_root is not None else tempfile.gettempdir())
    require(parent.is_dir(), "命令诊断父目录须为现有普通目录")
    directory = Path(tempfile.mkdtemp(prefix="forge-p1-command-failure-", dir=parent))
    directory = plain_path(directory)
    diagnostics = {"schema": "p1-command-capture-failure-v1", "argv": list(argv),
                   "cwd": str(Path(cwd).absolute()) if cwd is not None else str(Path.cwd()),
                   "exit_code": code, "reason": reason, "decode_errors": decode_errors,
                   "directory": str(directory), "streams": {}}
    for name, content in (("stdout", stdout), ("stderr", stderr)):
        row = {"captured": isinstance(content, bytes)}
        if isinstance(content, bytes):
            path = directory / (name + ".bin")
            with path.open("xb") as stream:
                stream.write(content)
            row.update(path=str(path), bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
        diagnostics["streams"][name] = row
    write_json(directory / "failure.local.json", diagnostics)
    return CommandCaptureError(f"命令失败 exit={code} {reason}", diagnostics)


def run_command(argv, *, timeout=120, cwd=None, evidence_root=None):
    require(isinstance(argv, list) and argv and all(isinstance(v, str) and v for v in argv), "命令必须为非空 argv 数组")
    # 不使用 shell、不枚举/关闭外部进程；超时只由 subprocess 回收本次启动的进程。
    # Windows Python 重定向默认可能是 GBK；仅约束子进程标准I/O，不改文件读写编码。
    environment = os.environ.copy()
    environment["PYTHONIOENCODING"] = "utf-8:strict"
    try:
        result = subprocess.run(argv, cwd=cwd, timeout=timeout, capture_output=True,
                                text=False, env=environment, shell=False)
    except subprocess.TimeoutExpired as error:
        raise command_failure(argv, cwd, None, error.stdout, error.stderr,
                              "timeout", [], evidence_root) from error
    decoded = {}
    decode_errors = []
    for name in ("stdout", "stderr"):
        content = getattr(result, name)
        if not isinstance(content, bytes):
            decode_errors.append({"stream": name, "reason": "capture did not return bytes"})
            continue
        try:
            decoded[name] = content.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            decode_errors.append({"stream": name, "encoding": "utf-8", "start": error.start,
                                  "end": error.end, "reason": error.reason,
                                  "window_hex": content[max(0, error.start - 16):error.end + 16].hex()})
    if decode_errors:
        raise command_failure(argv, cwd, result.returncode, result.stdout, result.stderr,
                              "strict UTF-8 stream decode failed", decode_errors, evidence_root)
    if result.returncode != 0:
        raise command_failure(argv, cwd, result.returncode, result.stdout, result.stderr,
                              "nonzero exit: " + decoded["stderr"][-4000:], [], evidence_root)
    return decoded["stdout"]


def command_json(argv, **kwargs):
    value = json.loads(run_command(argv, **kwargs))
    require(isinstance(value, dict), "命令 stdout 必须是一个 JSON 对象")
    return value
