#!/usr/bin/env python3
"""在独占原创 TEMP 中验证真实更新打包脚本；保留夹具和运行证据。"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
from shutil import which
import subprocess
import sys
import tempfile
import zipfile


FILE_ATTRIBUTE_HIDDEN = 0x2
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF
VERSION = "0.1.0"
COMMIT = "b" * 40
RELEASE_NOTES = "原创隐藏文件打包回归"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_identity(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": sha256(data)}


class WindowsAttributes:
    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.GetFileAttributesW.argtypes = [wintypes.LPCWSTR]
        self.kernel.GetFileAttributesW.restype = wintypes.DWORD
        self.kernel.SetFileAttributesW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
        self.kernel.SetFileAttributesW.restype = wintypes.BOOL

    def get(self, path):
        value = self.kernel.GetFileAttributesW(str(path))
        if value == INVALID_FILE_ATTRIBUTES:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    def hide(self, path):
        if not self.kernel.SetFileAttributesW(str(path), self.get(path) | FILE_ATTRIBUTE_HIDDEN):
            raise ctypes.WinError(ctypes.get_last_error())


def snapshot_tree(root, attributes):
    """Python 遍历包含隐藏条目；同时保留原字节与所有原生属性。"""
    rows = {}
    contents = {}

    def visit(path):
        native = attributes.get(path)
        if native & FILE_ATTRIBUTE_REPARSE_POINT:
            raise RuntimeError("原创夹具意外含重解析点：" + str(path))
        relative = path.relative_to(root).as_posix()
        row = {"attributes": native}
        if native & FILE_ATTRIBUTE_DIRECTORY:
            row["kind"] = "directory"
            with os.scandir(path) as entries:
                children = sorted((Path(entry.path) for entry in entries), key=lambda child: child.name)
            for child in children:
                visit(child)
        else:
            data = path.read_bytes()
            row.update(kind="file", bytes=len(data), sha256=sha256(data))
            contents[relative] = data
        rows[relative] = row

    visit(root)
    return rows, contents


class PackageTests:
    def __init__(self, package_script, powershell):
        self.package_script = package_script
        self.powershell = powershell
        self.attributes = WindowsAttributes()
        self.root = Path(tempfile.mkdtemp(prefix="AudastraPackageUpdateTests-")).resolve()
        self.receipt = {
            "format": "audastra-package-update-regression",
            "started_utc": utc_now(),
            "fixture_only": True,
            "fixture_directory": str(self.root),
            "fixture_retained": True,
            "official_release": False,
            "signature_tested": False,
            "package_script": file_identity(package_script),
            "test_source": file_identity(Path(__file__).resolve()),
            "powershell": str(powershell),
            "cases": [],
            "checks": [],
        }
        self.write_json(self.root / "fixture-owner.local.json", {
            "fixture_only": True, "owner_pid": os.getpid(), "created_utc": utc_now(),
            "directory": str(self.root),
        })

    def owned(self, path):
        resolved = path.resolve()
        if resolved == self.root or self.root not in resolved.parents:
            raise RuntimeError("写入目标越出独占 TEMP：" + str(path))
        return resolved

    def write_new(self, path, data):
        with self.owned(path).open("xb") as stream:
            stream.write(data)

    def write_json(self, path, value):
        self.write_new(path, (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))

    def check(self, name, passed):
        self.receipt["checks"].append({"name": name, "passed": bool(passed)})

    def make_portable(self, case, hidden_file=False, hidden_directory=False, declare_hidden=False):
        portable = self.owned(case / "portable")
        portable.mkdir()
        payloads = {
            "bin/app-test.txt": b"original synthetic program fixture\r\n",
            "licenses/original-test.txt": b"original test only\n",
        }
        hidden_path = None
        if hidden_file:
            hidden_path = "original-hidden.txt"
            payloads[hidden_path] = b"original hidden file fixture\x00\xff\n"
        if hidden_directory:
            hidden_path = "original-hidden-directory/private-payload.txt"
            payloads[hidden_path] = b"original hidden directory fixture\n"
        for relative, data in payloads.items():
            target = self.owned(portable / relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            self.write_new(target, data)
        if hidden_file:
            self.attributes.hide(portable / hidden_path)
        if hidden_directory:
            self.attributes.hide(portable / "original-hidden-directory")
        declared = [
            {"path": relative, "bytes": len(data), "sha256": sha256(data)}
            for relative, data in sorted(payloads.items())
            if relative != hidden_path or declare_hidden
        ]
        self.write_json(portable / "manifest.json", {
            "format": 1, "source_clean": True, "version": VERSION,
            "source_commit": COMMIT, "files": declared,
        })
        return portable

    def verify_package(self, case, output, source_contents):
        manifest_path = output / "update-manifest.json"
        archive_path = output / ("Audastra-" + VERSION + "-windows-x64-update.zip")
        self.check(case + ": output files", manifest_path.is_file() and archive_path.is_file())
        if not manifest_path.is_file() or not archive_path.is_file():
            return
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        expected_fields = {
            "format": "audastra-update", "schema": 1, "product": "Audastra",
            "platform": "windows-x64", "version": VERSION,
            "minimum_direct_upgrade": VERSION, "source_commit": COMMIT,
            "release_notes": RELEASE_NOTES,
        }
        self.check(case + ": candidate identity", all(manifest.get(key) == value for key, value in expected_fields.items()))
        files = manifest["files"]
        paths = [entry["path"] for entry in files]
        self.check(case + ": exact unique allowlist", len(paths) == len(set(paths)) and set(paths) == set(source_contents))
        by_path = {entry["path"]: entry for entry in files}
        archive_bytes = archive_path.read_bytes()
        self.check(case + ": whole ZIP bytes and SHA", manifest["zip"] == {
            "filename": archive_path.name, "bytes": len(archive_bytes), "sha256": sha256(archive_bytes),
        })
        with zipfile.ZipFile(archive_path) as archive:
            names = archive.namelist()
            self.check(case + ": exact unique ZIP members", len(names) == len(set(names)) and set(names) == set(source_contents))
            for relative, original in sorted(source_contents.items()):
                entry = by_path.get(relative)
                self.check(case + ": allowlist identity " + relative, entry == {
                    "path": relative, "bytes": len(original), "sha256": sha256(original),
                })
                decoded = archive.read(relative)
                self.check(case + ": ZIP original bytes " + relative, decoded == original)
                self.check(case + ": ZIP bytes and SHA " + relative, entry is not None
                           and len(decoded) == entry["bytes"] and sha256(decoded) == entry["sha256"])
        self.check(case + ": unsigned output only", {path.name for path in output.iterdir()} == {
            manifest_path.name, archive_path.name,
        })

    def run_case(self, name, hidden_file=False, hidden_directory=False, declare_hidden=False, existing_output=False):
        case = self.owned(self.root / name)
        case.mkdir()
        record = {"name": name, "started_utc": utc_now()}
        self.receipt["cases"].append(record)
        portable = self.make_portable(case, hidden_file, hidden_directory, declare_hidden)
        output = self.owned(case / "output")
        before = snapshot_tree(portable, self.attributes)
        self.write_json(case / "source-before.local.json", before[0])
        if hidden_file or hidden_directory:
            hidden = portable / ("original-hidden.txt" if hidden_file else "original-hidden-directory")
            self.check(name + ": native hidden attribute", bool(self.attributes.get(hidden) & FILE_ATTRIBUTE_HIDDEN))
        output_before = None
        if existing_output:
            output.mkdir()
            self.write_new(output / "original-output.txt", b"preserve existing output bytes\x00\xff\r\n")
            self.write_new(output / "hidden-original-output.txt", b"preserve original hidden output\n")
            self.attributes.hide(output / "hidden-original-output.txt")
            self.owned(output / "original-empty-directory").mkdir()
            output_before = snapshot_tree(output, self.attributes)
            self.write_json(case / "output-before.local.json", output_before[0])
        expected_exit = 0 if declare_hidden else 1
        argv = [
            str(self.powershell), "-NoProfile", "-NonInteractive", "-File", str(self.package_script),
            "-PortableDirectory", str(portable), "-OutputDirectory", str(output),
            "-MinimumDirectUpgrade", VERSION, "-ReleaseNotes", RELEASE_NOTES,
        ]
        try:
            # 不设置会自动终止进程的 timeout；父集成者统一安排执行窗口。
            completed = subprocess.run(argv, cwd=case, shell=False, capture_output=True)
            self.write_new(case / "package.stdout.log", completed.stdout)
            self.write_new(case / "package.stderr.log", completed.stderr)
            record.update(actual_exit=completed.returncode, expected_exit=expected_exit)
            self.check(name + ": script exit", completed.returncode == expected_exit)
            if existing_output:
                output_after = snapshot_tree(output, self.attributes)
                self.write_json(case / "output-after.local.json", output_after[0])
                self.check(name + ": existing output bytes and all attributes preserved", output_after == output_before)
            elif declare_hidden:
                self.verify_package(name, output, before[1])
            else:
                self.check(name + ": rejected before output creation", not os.path.lexists(output))
        except Exception as error:
            record["error"] = type(error).__name__ + ": " + str(error)
            self.check(name + ": execution completed without exception", False)
        finally:
            try:
                after = snapshot_tree(portable, self.attributes)
                self.write_json(case / "source-after.local.json", after[0])
                self.check(name + ": source bytes and all attributes unchanged", after == before)
                record["source_unchanged"] = after == before
            except Exception as error:
                record["source_snapshot_error"] = type(error).__name__ + ": " + str(error)
                self.check(name + ": source snapshot available", False)
            record["finished_utc"] = utc_now()

    def run(self):
        try:
            self.run_case("undeclared-hidden-file", hidden_file=True)
            self.run_case("undeclared-hidden-directory", hidden_directory=True)
            self.run_case("declared-hidden-file", hidden_file=True, declare_hidden=True)
            self.run_case("existing-output", existing_output=True)
            self.check("package script bytes unchanged", file_identity(self.package_script) == self.receipt["package_script"])
        except Exception as error:
            self.receipt["error"] = type(error).__name__ + ": " + str(error)
            self.check("fixture setup and four cases completed", False)
        self.receipt["finished_utc"] = utc_now()
        self.receipt["check_count"] = len(self.receipt["checks"])
        self.receipt["failure_count"] = sum(not check["passed"] for check in self.receipt["checks"])
        self.receipt["passed"] = self.receipt["failure_count"] == 0 and len(self.receipt["cases"]) == 4
        receipt_path = self.root / "receipt.local.json"
        self.write_json(receipt_path, self.receipt)
        print(json.dumps({
            "checks": self.receipt["check_count"], "failures": self.receipt["failure_count"],
            "passed": self.receipt["passed"], "fixture_retained": True, "receipt": str(receipt_path),
        }, ensure_ascii=False))
        return 0 if self.receipt["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-script", type=Path, default=Path(__file__).resolve().parents[2] / "tools/dev/package-update.ps1")
    parser.add_argument("--powershell", default="pwsh")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("此回归需要 Windows 原生隐藏属性和 PowerShell 7")
    package_script = args.package_script.resolve(strict=True)
    executable = which(args.powershell)
    if executable is None:
        parser.error("找不到指定 PowerShell：" + args.powershell)
    powershell = Path(executable).resolve(strict=True)
    return PackageTests(package_script, powershell).run()


if __name__ == "__main__":
    sys.exit(main())
