"""只为明确身份的独立 P1 宿主启用旧 UI reader 文件首 UTF-8 BOM 兼容。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys


READER = Path("script/ydwe/ui-builder/old-reader.lua")
ASSEMBLY = "p1-host-assembly.local.json"
OVERLAYS = ("ui", "compiler/include", "memory_hack", "AsphodelusUI")
FUNCTION = b"function mt:readfile(buf)"
BOM_GUARD = b'\tif buf:sub(1, 3) == "\\239\\187\\191" then'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plain_path(value):
    text = os.fspath(value)
    require(text and "\x00" not in text, "路径不能为空或包含 NUL")
    require(not text.startswith(("\\\\", "//")), "拒绝 UNC 或设备路径")
    path = Path(text)
    require(path.is_absolute(), "必须提供绝对路径")
    require(all(":" not in part for part in path.parts[1:]), "拒绝 ADS 路径")
    for node in (path, *path.parents):
        if node.exists() or node.is_symlink():
            info = node.lstat()
            require(not node.is_symlink() and not (getattr(info, "st_file_attributes", 0) & 0x400),
                    f"拒绝链接或 reparse point: {node}")
    return path.resolve()


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def file_digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def patched_bytes(original):
    require(BOM_GUARD not in original, "reader 已包含本次 BOM guard，拒绝重复修复")
    require(original.count(FUNCTION) == 1, "readfile 入口必须恰好存在一次")
    entry = original.index(FUNCTION) + len(FUNCTION)
    if original[entry:entry + 2] == b"\r\n":
        newline = b"\r\n"
    else:
        require(original[entry:entry + 1] == b"\n", "readfile 入口行格式不匹配")
        newline = b"\n"
    # 保留原 assert 及其失败行为；guard 位于读取首 section 之前。
    assertion = b"\tlocal f = assert(buf)" + newline
    start = entry + len(newline)
    require(original[start:start + len(assertion)] == assertion, "原 readfile assert 入口不匹配")
    position = start + len(assertion)
    addition = newline.join((BOM_GUARD, b"\t\tbuf = buf:sub(4)", b"\tend", b""))
    patched = original[:position] + addition + original[position:]
    require(patched[:position] + patched[position + len(addition):] == original,
            "修复必须只插入 BOM guard，保留所有原字节")
    return patched, position, addition


def overlay_manifest(root):
    result = {}
    for relative in OVERLAYS:
        directory = plain_path(root / relative)
        require(directory.is_dir(), f"overlay 目录缺失: {relative}")
        for current, directories, files in os.walk(directory, followlinks=False):
            for name in directories:
                plain_path(Path(current) / name)
            for name in files:
                path = plain_path(Path(current) / name)
                require(path.is_file(), "overlay 必须是普通文件")
                result[path.relative_to(root).as_posix()] = file_digest(path)
    return dict(sorted(result.items()))


def manifest_digest(manifest):
    return digest(json.dumps(manifest, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")).encode("utf-8"))


def write_receipt(stream, receipt):
    stream.seek(0)
    json.dump(receipt, stream, ensure_ascii=False, indent=2, allow_nan=False)
    stream.write("\n")
    stream.truncate()
    stream.flush()
    os.fsync(stream.fileno())


def enable(args):
    host = plain_path(args.host)
    require(host.is_dir(), "宿主目录不存在")
    marker_path = plain_path(host / ASSEMBLY)
    require(marker_path.is_file(), "缺少独立宿主 assembly 身份标志")
    marker_raw = marker_path.read_bytes()
    marker = json.loads(marker_raw.decode("utf-8-sig"))
    require(isinstance(marker, dict), "assembly 标志必须是 JSON 对象")
    require(plain_path(marker.get("host", "")) == host, "assembly host 身份不匹配")
    for flag in ("source_unchanged", "base_ui_preserved", "base_include_preserved"):
        require(marker.get(flag) is True, f"assembly 标志未证明 {flag}")
    require(type(marker.get("overlay_files")) is int and marker["overlay_files"] > 0,
            "assembly 缺少正整数 overlay_files")
    require(marker.get("ydwe_started") is False and marker.get("registry_modified") is False,
            "assembly 已启动或修改注册表，拒绝本次未启动宿主修复")
    expected = args.expected_reader_sha256.lower()
    require(re.fullmatch(r"[0-9a-f]{64}", expected) is not None, "expected 原 reader SHA 无效")
    reader = plain_path(host / READER)
    require(reader.is_file(), "原 reader 不存在")
    original = reader.read_bytes()
    patched, position, addition = patched_bytes(original)
    require(digest(original) == expected, "原 reader SHA 与 expected 不匹配")
    backup = plain_path(args.backup_directory)
    receipt_path = plain_path(args.receipt)
    require(not backup.exists() and not receipt_path.exists(), "backup 与 receipt 必须独占新路径")
    require(backup != receipt_path and backup not in reader.parents and reader not in backup.parents,
            "backup 不能覆盖 reader 或其祖先")
    require(receipt_path != reader and receipt_path != marker_path, "receipt 不能覆盖宿主输入")
    require(host not in backup.parents and host not in receipt_path.parents,
            "backup 与 receipt 必须在宿主外，宿主内只允许修改 reader")
    require(backup.drive.casefold() == reader.drive.casefold(), "backup 与 reader 必须同卷，以便原子替换")
    protected = None
    reference = None
    if args.overlay_reference:
        reference = plain_path(args.overlay_reference)
        require(reference != host, "overlay reference 必须是独立只读来源")
        require(reference not in backup.parents and reference not in receipt_path.parents,
                "backup 与 receipt 不能写入只读 overlay 来源")
        source_before = overlay_manifest(reference)
        host_before = overlay_manifest(host)
        require(len(source_before) == marker["overlay_files"], "overlay 来源文件数量与 assembly 不匹配")
        require(host_before == source_before, "宿主 overlay 与只读来源逐文件 SHA 不一致")
        protected = {"file_count": len(source_before), "source_root": str(reference),
                     "before_manifest_sha256": manifest_digest(source_before), "files": source_before}
    receipt = {"schema_version": 1, "operation": "owned-host-old-reader-initial-utf8-bom",
               "status": "prepared", "host": str(host), "reader": str(reader),
               "assembly_sha256": digest(marker_raw), "reader_sha256_before": digest(original),
               "reader_sha256_after": digest(patched), "original_bytes": len(original),
               "patched_bytes": len(patched), "insertion_offset": position,
               "inserted_ascii": addition.decode("ascii"), "backup_directory": str(backup),
               "receipt": str(receipt_path), "protected_overlays": protected,
               "ydwe_or_we_started": False, "game_started": False, "hkcu_writes_requested": False}
    if args.dry_run:
        receipt["status"] = "dry-run"
        return receipt
    # 独占输出先于 reader 替换；任何失败保留新现场，不覆盖旧证据。
    backup.parent.mkdir(parents=True, exist_ok=True)
    plain_path(backup.parent)
    backup.mkdir()
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    plain_path(receipt_path.parent)
    with receipt_path.open("x", encoding="utf-8", newline="\n") as stream:
        write_receipt(stream, receipt)
        try:
            with (backup / "old-reader.original.lua").open("xb") as output:
                output.write(original)
                output.flush()
                os.fsync(output.fileno())
            staged = backup / "old-reader.patched.lua"
            with staged.open("xb") as output:
                output.write(patched)
                output.flush()
                os.fsync(output.fileno())
            require(plain_path(reader).read_bytes() == original, "reader 在准备后发生变化")
            require(marker_path.read_bytes() == marker_raw, "assembly 身份在准备后发生变化")
            os.replace(staged, reader)
            require(reader.read_bytes() == patched, "reader 替换后字节验证失败")
            if reference is not None:
                source_after = overlay_manifest(reference)
                host_after = overlay_manifest(host)
                require(source_after == source_before and host_after == host_before,
                        "overlay 在 reader 修复期间发生变化")
                protected.update(after_manifest_sha256=manifest_digest(source_after),
                                 source_unchanged=True, host_unchanged=True)
            receipt["status"] = "applied"
            write_receipt(stream, receipt)
        except Exception as exc:
            receipt.update(status="failed", error_type=type(exc).__name__, error=str(exc))
            write_receipt(stream, receipt)
            raise
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--expected-reader-sha256", required=True)
    parser.add_argument("--backup-directory", required=True)
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--overlay-reference", help="只读 adapter 根；核对 assembly 全部 overlay 文件")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        receipt = enable(args)
        summary = {key: receipt[key] for key in ("status", "reader_sha256_before", "reader_sha256_after",
                                               "backup_directory", "receipt")}
        if receipt["protected_overlays"] is not None:
            summary["protected_overlay_file_count"] = receipt["protected_overlays"]["file_count"]
        print(json.dumps(summary, ensure_ascii=True, indent=2))
        return 0
    except Exception as exc:
        print(json.dumps({"status": "failed", "error_type": type(exc).__name__, "error": str(exc)},
                         ensure_ascii=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
