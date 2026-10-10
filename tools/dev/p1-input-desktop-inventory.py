"""native08 的只读清单回执；复用 native-loader 的普通路径/原子文件身份/完整清单。"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepared", required=True)
    parser.add_argument("--prepared-sha256", required=True)
    parser.add_argument("--native-loader-sha256", required=True)
    parser.add_argument("--receipt", required=True)
    args = parser.parse_args()
    loader_path = Path(__file__).with_name("p1-native-loader.py")
    import hashlib
    if hashlib.sha256(loader_path.read_bytes()).hexdigest() != args.native_loader_sha256.lower():
        raise ValueError("native-loader 公共清单实现 SHA 不符")
    spec = importlib.util.spec_from_file_location("p1_native08_inventory_common", loader_path)
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    prepared, identity = common.read_json(common.plain(args.prepared))
    common.require(identity["sha256"] == common.expected_sha(args.prepared_sha256), "prepared SHA 不符")
    common.require(prepared.get("schema") == "p1-native-loader-v1" and prepared.get("status") == "prepared",
                   "prepared schema/status 不符")
    game, host = common.plain(prepared["gameRoot"]), common.plain(prepared["hostRoot"])
    game_files, game_dirs = common.inventory(game, "game", host if game in host.parents else None)
    host_files, host_dirs = common.inventory(host, "host")
    files = game_files + host_files
    expected = {(row["root"], row["relativePath"]): row for row in prepared["dependencies"]}
    actual = {(row["root"], row["relativePath"]): row for row in files}
    common.require(len(expected) == len(prepared["dependencies"]) and len(actual) == len(files),
                   "inventory 重复成员")
    added = [actual[key] for key in sorted(actual.keys() - expected.keys())]
    removed = [expected[key] for key in sorted(expected.keys() - actual.keys())]
    changed = [{"before": expected[key], "after": actual[key]}
               for key in sorted(actual.keys() & expected.keys()) if actual[key] != expected[key]]
    directories = {"game": game_dirs, "host": host_dirs}
    dir_delta = {scope: {"added": sorted(set(directories[scope]) - set(prepared["directories"][scope])),
                         "removed": sorted(set(prepared["directories"][scope]) - set(directories[scope]))}
                 for scope in directories}
    matches = not any((added, removed, changed)) and all(not any(row.values()) for row in dir_delta.values())
    output = common.plain(args.receipt)
    common.require(not output.exists() and output.parent.is_dir() and
                   common.disjoint(output, game) and common.disjoint(output, host), "新回执路径合同不符")
    result = {"schema": "p1-input-desktop-inventory-v1", "matchesPrepared": matches,
              "gameRoot": str(game), "hostRoot": str(host), "preparedIdentity": identity,
              "dependencies": files, "directories": directories,
              "delta": {"added": added, "removed": removed, "changed": changed, "directories": dir_delta},
              "launched": False, "registryWritten": False}
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        common.save_receipt(stream, result)
    print(json.dumps({"receipt": str(output), "matchesPrepared": matches, "files": len(files),
                      "added": len(added), "removed": len(removed), "changed": len(changed)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
