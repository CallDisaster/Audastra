"""P1 BOM 工具独立回归：使用目标 Lua 和原始 reader，仅创建原创临时夹具。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


TOOL = Path(__file__).with_name("p1-host-enable-utf8.py")
READER = Path("script/ydwe/ui-builder/old-reader.lua")
BOM = b"\xef\xbb\xbf"
LUA_FIXTURE = r'''
local factory = assert(loadfile(arg[1]))()
local function load_bytes(path)
    local stream = assert(io.open(path, 'rb'))
    local bytes = stream:read('*a')
    stream:close()
    return bytes
end
local object = factory(function(name)
    if name == [[ui\TriggerData.txt]] then
        return load_bytes(arg[2])
    end
    assert(name == [[ui\TriggerStrings.txt]])
    return load_bytes(arg[3])
end)
local mode = arg[4]
if mode == 'original-gap' then
    local item = object.ui.event.P1Event
    assert(item == nil or item.description == nil, 'original reader unexpectedly accepted BOM fixture')
elseif mode == 'event' then
    local item = assert(object.ui.event.P1Event, 'missing first event section')
    assert(item.title == 'Fixture event', 'event title changed')
    assert(item.description == 'Original description', 'event description changed')
    assert(item.category == 'TC_P1', 'event category changed')
    assert(#item.args == 0, 'zero-argument event changed')
elseif mode == 'interior' then
    local seen = {}
    object.parse = function(self, section, key, value)
        seen[#seen+1] = {section=section,key=key,value=value}
    end
    object:readfile(load_bytes(arg[5]))
    assert(#seen == 2, 'unexpected parse call count')
    assert(seen[1].section == 'TriggerEvents' and seen[1].key == 'P1A')
    assert(seen[1].value == string.char(239,187,191)..'inside', 'interior value BOM changed')
    assert(seen[2].section == 'TriggerEvents' and seen[2].key == 'P1B', 'later BOM header was normalized')
    assert(seen[2].value == 'tail', 'later value changed')
elseif mode == 'double-prefix' then
    local seen
    object.parse = function(self, section, key, value)
        seen = {section=section,key=key,value=value}
    end
    object:readfile(load_bytes(arg[5]))
    assert(seen and seen.section == nil and seen.key == 'P1A', 'more than the first BOM was stripped')
    assert(seen.value == 'tail')
else
    error('unknown fixture mode')
end
print('P1_REAL_LUA_READER_OK:'..mode)
'''


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class HostBomTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="p1-bom-fixture-", dir=self.parent))
        self.host = self.directory / "host"
        self.reader = self.host / READER
        self.reader.parent.mkdir(parents=True)
        self.reader.write_bytes(self.original_reader)
        self.marker = self.host / "p1-host-assembly.local.json"
        self.write_marker()
        self.backup = self.directory / "backup"
        self.receipt = self.directory / "receipt.local.json"
        self.runner = self.directory / "reader-fixture.lua"
        self.runner.write_text(LUA_FIXTURE, encoding="utf-8")
        self.data = self.directory / "TriggerData.fixture.txt"
        self.strings = self.directory / "TriggerStrings.fixture.txt"
        self.data.write_bytes(b"[TriggerEvents]\r\nP1Event=0\r\n_P1Event_Category=TC_P1\r\n")
        self.strings.write_bytes(b'[TriggerEventStrings]\r\nP1Event="Fixture event"\r\nP1Event="Original description"\r\n')

    def write_marker(self, **changes):
        marker = {"host": str(self.host), "overlay_files": 4, "source_unchanged": True,
                  "base_ui_preserved": True, "base_include_preserved": True,
                  "ydwe_started": False, "registry_modified": False}
        marker.update(changes)
        self.marker.write_text(json.dumps(marker), encoding="utf-8")

    def command(self, **changes):
        options = {"host": self.host, "expected-reader-sha256": sha(self.reader.read_bytes()),
                   "backup-directory": self.backup, "receipt": self.receipt}
        options.update(changes)
        argv = [self.python, "-B", str(TOOL)]
        for key, value in options.items():
            if value is True:
                argv.append("--" + key)
            else:
                argv.extend(("--" + key, str(value)))
        return subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", timeout=30,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def apply(self, **changes):
        result = self.command(**changes)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual(receipt["status"], "applied")
        self.assertEqual((self.backup / "old-reader.original.lua").read_bytes(), self.original_reader)
        self.assertEqual(receipt["reader_sha256_after"], sha(self.reader.read_bytes()))
        return receipt

    def lua_check(self, mode, payload=None):
        argv = [str(self.lua), "-E", str(self.runner), str(self.reader), str(self.data),
                str(self.strings), mode]
        if payload is not None:
            path = self.directory / (mode + ".fixture.txt")
            path.write_bytes(payload)
            argv.append(str(path))
        result = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", timeout=30,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("P1_REAL_LUA_READER_OK:" + mode, result.stdout)

    def assert_rejected_without_reader_write(self, **changes):
        before = self.reader.read_bytes()
        result = self.command(**changes)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.reader.read_bytes(), before)
        return result

    def test_real_reader_first_bom_section(self):
        self.data.write_bytes(BOM + self.data.read_bytes())
        self.strings.write_bytes(BOM + self.strings.read_bytes())
        self.lua_check("original-gap")
        self.apply()
        self.lua_check("event")

    def test_real_reader_without_bom_unchanged(self):
        self.lua_check("event")
        self.apply()
        self.lua_check("event")

    def test_only_initial_bom_removed_and_later_bytes_preserved(self):
        self.apply()
        self.lua_check("interior", BOM + b"[TriggerEvents]\r\nP1A=" + BOM +
                       b"inside\r\n" + BOM + b"[LaterHeader]\r\nP1B=tail\r\n")
        self.lua_check("double-prefix", BOM + BOM + b"[TriggerEvents]\nP1A=tail\n")

    def test_lf_source_retains_all_original_bytes(self):
        lf = self.original_reader.replace(b"\r\n", b"\n")
        self.reader.write_bytes(lf)
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        patched = self.reader.read_bytes()
        offset = receipt["insertion_offset"]
        inserted = receipt["inserted_ascii"].encode("ascii")
        self.assertEqual(patched[:offset] + patched[offset + len(inserted):], lf)
        self.lua_check("event")

    def test_wrong_expected_sha_refuses_all_outputs(self):
        self.assert_rejected_without_reader_write(**{"expected-reader-sha256": "0" * 64})
        self.assertFalse(self.backup.exists())
        self.assertFalse(self.receipt.exists())

    def test_repeated_repair_refuses_new_outputs(self):
        self.apply()
        second_backup = self.directory / "backup-second"
        second_receipt = self.directory / "receipt-second.local.json"
        self.assert_rejected_without_reader_write(**{"backup-directory": second_backup, "receipt": second_receipt})
        self.assertFalse(second_backup.exists())
        self.assertFalse(second_receipt.exists())

    def test_existing_backup_and_receipt_refused(self):
        self.backup.mkdir()
        self.assert_rejected_without_reader_write()
        self.assertFalse(self.receipt.exists())
        self.receipt.write_text("old evidence", encoding="utf-8")
        self.assert_rejected_without_reader_write(**{"backup-directory": self.directory / "another-backup"})
        self.assertEqual(self.receipt.read_text(encoding="utf-8"), "old evidence")

    def test_assembly_identity_mismatch(self):
        self.write_marker(host=str(self.directory / "different-host"))
        self.assert_rejected_without_reader_write()
        self.assertFalse(self.backup.exists())

    def test_missing_assembly_marker(self):
        self.marker.unlink()
        self.assert_rejected_without_reader_write()
        self.assertFalse(self.backup.exists())

    def test_reparse_host_refused(self):
        link = self.directory / "host-link"
        try:
            os.symlink(self.host, link, target_is_directory=True)
        except OSError as exc:
            self.fail(f"本环境无法完成 required reparse fixture: {exc}")
        self.assert_rejected_without_reader_write(host=link)
        self.assertFalse(self.backup.exists())

    def test_dry_run_creates_nothing(self):
        before = self.reader.read_bytes()
        result = self.command(**{"dry-run": True})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.reader.read_bytes(), before)
        self.assertFalse(self.backup.exists())
        self.assertFalse(self.receipt.exists())

    def test_overlay_mismatch_refuses_repair(self):
        reference = self.directory / "reference"
        for relative in ("ui", "compiler/include", "memory_hack", "AsphodelusUI"):
            for root in (self.host, reference):
                directory = root / relative
                directory.mkdir(parents=True)
                (directory / "fixture.txt").write_text("original overlay", encoding="utf-8")
        (self.host / "ui/fixture.txt").write_text("different overlay", encoding="utf-8")
        self.assert_rejected_without_reader_write(**{"overlay-reference": reference})
        self.assertFalse(self.backup.exists())


def main():
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua", help="显式 Lua；缺省读取 process FORGE_YDWE_ROOT/bin/lua.exe")
    parser.add_argument("--reader-source", help="显式原 reader；缺省读取 process FORGE_YDWE_ROOT")
    parser.add_argument("--temporary-parent", help="保留夹具的已有目录；缺省创建并清理自有临时目录")
    args = parser.parse_args()
    ydwe_environment = os.environ.get("FORGE_YDWE_ROOT")
    if (not args.lua or not args.reader_source) and not ydwe_environment:
        print("SKIP: FORGE_YDWE_ROOT is unset; supply --lua and --reader-source to run actual Lua tests.")
        return 77
    ydwe = Path(ydwe_environment) if ydwe_environment else None
    HostBomTests.lua = Path(args.lua or ydwe / "bin/lua.exe").resolve(strict=True)
    HostBomTests.original_reader = Path(args.reader_source or ydwe / READER).read_bytes()
    HostBomTests.python = sys.executable
    if args.temporary_parent:
        HostBomTests.parent = Path(args.temporary_parent).resolve(strict=True)
        result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False)
    else:
        with tempfile.TemporaryDirectory(prefix="p1-bom-tests-", dir=tempfile.gettempdir()) as owned:
            HostBomTests.parent = Path(owned).resolve(strict=True)
            result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
