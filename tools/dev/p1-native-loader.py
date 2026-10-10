"""为明确锁定的独立 P1 游戏/宿主准备原生 loader；不启动进程或写注册表。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import sys


LOADER_NAME = "worldeditydwe.exe"
CONFIG_NAME = LOADER_NAME + ".config"
# 与已审查 YDWEStartup 固定模板逐字节一致；不是从来源配置文件猜测。
EDITOR_LOADER_CONFIG = (
    b'<?xml version="1.0"?>\n'
    b'<configuration>\n'
    b'\t<runtime>\n'
    b'\t\t<loadFromRemoteSources enabled="true" />\n'
    b'\t</runtime>\n'
    b'</configuration>\n'
)
SENTINELS = ("war3.exe", "game.dll", "war3.mpq", "war3patch.mpq", "storm.dll")
PRELOAD_NAMES = ("YDDllFixer.dll", "vcruntime140.dll", "msvcp140.dll", "StormLib.dll",
                 "lua53.dll", "ydbase.dll", "LuaEngine.dll")
HOST_REQUIRED = ("bin/worldedit.exe", "bin/YDWEStartup.dll", "bin/ydwar3.dll",
                 "bin/modules/filesystem.dll", "script/common/config.lua",
                 "script/ydwe/ydwe_on_startup.lua", "script/ydwe/ui-builder/old-reader.lua",
                 *("bin/" + name for name in PRELOAD_NAMES))
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_FILES = 20000
MAX_DIRECTORIES = 10000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha_bytes(raw):
    return hashlib.sha256(raw).hexdigest()


def expected_sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None,
            "显式预期 SHA256 必须为 64 位十六进制")
    return value.lower()


def relative(value):
    require(isinstance(value, str) and value and "\x00" not in value, "相对路径为空或含 NUL")
    win = PureWindowsPath(value)
    parts = value.replace("\\", "/").split("/")
    require(not win.drive and not win.is_absolute() and not value.startswith("/"), "拒绝绝对成员路径")
    require(all(part not in ("", ".", "..") and not part.endswith((" ", "."))
                and not any(char in part for char in ':<>|?*') for part in parts),
            "拒绝越界、ADS、空成员或 Windows 别名路径")
    return Path(*parts)


def plain(value):
    text = os.fspath(value)
    require(text and "\x00" not in text and not text.startswith(("\\\\", "//")),
            "拒绝空路径、NUL、UNC 或设备路径")
    path = Path(text)
    require(path.is_absolute(), "必须显式提供绝对路径")
    require(all(part not in (".", "..") and not part.endswith((" ", "."))
                and not any(char in part for char in ':<>|?*') for part in path.parts[1:]),
            "拒绝越界、ADS 或 Windows 别名路径")
    for current in (path, *path.parents):
        if current.exists() or current.is_symlink():
            info = current.lstat()
            require(not current.is_symlink() and not (getattr(info, "st_file_attributes", 0) & 0x400),
                    f"拒绝链接或 reparse point: {current}")
            if current.is_file():
                require(info.st_nlink == 1, f"拒绝 hardlink 文件: {current}")
    return path.resolve()


def disjoint(left, right):
    return left != right and left not in right.parents and right not in left.parents


def stamp(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_nlink)


def file_identity(path):
    path = plain(path)
    require(path.is_file(), f"输入不是普通文件: {path}")
    before = path.stat()
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        require(stamp(os.fstat(stream.fileno())) == stamp(before), "输入打开时发生替换")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
        require(stamp(os.fstat(stream.fileno())) == stamp(before), "输入读取期间发生变化")
    require(stamp(plain(path).stat()) == stamp(before), "输入读取后发生替换")
    return {"bytes": before.st_size, "sha256": sha.hexdigest()}


def read_json(path):
    path = plain(path)
    require(path.is_file() and path.stat().st_size <= MAX_JSON_BYTES, "JSON 不存在或超过 16 MiB")
    identity = file_identity(path)
    with path.open("rb") as stream:
        raw = stream.read(MAX_JSON_BYTES + 1)
    require(len(raw) <= MAX_JSON_BYTES, "JSON 读取超过预算")
    require(len(raw) == identity["bytes"] and sha_bytes(raw) == identity["sha256"]
            and file_identity(path) == identity, "JSON 在读取期间变化")
    value = json.loads(raw.decode("utf-8-sig"))
    require(isinstance(value, dict), "JSON 顶层必须是对象")
    return value, identity


def locate(root, name):
    current = root
    for part in relative(name).parts:
        require(current.is_dir(), f"成员父目录不存在: {name}")
        matches = [entry for entry in current.iterdir() if entry.name.casefold() == part.casefold()]
        require(len(matches) == 1, f"成员缺失或大小写歧义: {name}")
        current = plain(matches[0])
    return current


def absent(root, name):
    require(not any(entry.name.casefold() == name.casefold() for entry in root.iterdir()),
            f"输出已存在或出现根目录 DLL shadow: {name}")


def inventory(root, root_name, excluded=None):
    files, directories = [], []
    for current, child_directories, child_files in os.walk(root, followlinks=False):
        directory = plain(current)
        child_directories[:] = sorted(child_directories, key=str.casefold)
        kept = []
        for name in child_directories:
            child = plain(directory / name)
            if excluded is not None and child == excluded:
                continue
            require(child.is_dir(), "清单中的目录不是普通目录")
            directories.append(child.relative_to(root).as_posix())
            require(len(directories) <= MAX_DIRECTORIES, "目录清单超过预算")
            kept.append(name)
        child_directories[:] = kept
        for name in sorted(child_files, key=str.casefold):
            file = plain(directory / name)
            row = {"root": root_name, "relativePath": file.relative_to(root).as_posix(), **file_identity(file)}
            files.append(row)
            require(len(files) <= MAX_FILES, "文件清单超过预算")
    require(len({row["relativePath"].casefold() for row in files}) == len(files), "清单文件大小写歧义")
    require(len({path.casefold() for path in directories}) == len(directories), "清单目录大小写歧义")
    return files, directories


def validate_game_manifest(game):
    manifest, identity = read_json(game / "p1-sandbox.json")
    require(manifest.get("schema") == "p1-game-sandbox-v1" and manifest.get("status") == "prepared"
            and manifest.get("source_unchanged") is True, "游戏沙盒未通过 prepare")
    require(plain(manifest.get("sandbox", "")) == game, "游戏 manifest sandbox 身份不符")
    source = plain(manifest.get("source", ""))
    require(disjoint(source, game), "游戏沙盒不能与原始安装互为父子目录")
    rows = manifest.get("files")
    require(isinstance(rows, list) and 0 < len(rows) <= MAX_FILES, "游戏 manifest files 无效")
    seen = set()
    for row in rows:
        require(isinstance(row, dict), "游戏成员必须为对象")
        name = relative(row.get("path")).as_posix()
        require(name.casefold() not in seen, "游戏 manifest 重复成员")
        seen.add(name.casefold())
        require(type(row.get("bytes")) is int and row["bytes"] >= 0, "游戏成员 size 无效")
        expected = {"bytes": row["bytes"], "sha256": expected_sha(row.get("sha256"))}
        require(file_identity(locate(game, name)) == expected, f"游戏来源身份变化: {name}")
    require(set(SENTINELS).issubset(seen), "游戏 manifest 缺少五个固定哨兵")
    for entry in game.iterdir():
        if entry.is_file() and entry.suffix.casefold() in (".exe", ".dll", ".asi", ".ocx"):
            require(entry.name.casefold() in seen, f"游戏根出现未记录可执行模块: {entry.name}")
    return identity


def collect(game_root, host_root, loader_reference, loader_sha):
    game, host, reference = plain(game_root), plain(host_root), plain(loader_reference)
    require(game.is_dir() and host.is_dir(), "游戏或宿主目录不存在")
    require(game != host and host not in game.parents, "宿主不能覆盖或包含游戏根")
    require(disjoint(reference, game) and disjoint(reference, host), "loader 原安装参考不能在自有游戏/宿主内")
    expected = expected_sha(loader_sha)
    game_manifest = validate_game_manifest(game)
    assembly, assembly_identity = read_json(host / "p1-host-assembly.local.json")
    require(plain(assembly.get("host", "")) == host, "assembly host 身份不符")
    require(all(assembly.get(flag) is True for flag in
                ("source_unchanged", "base_ui_preserved", "base_include_preserved")), "assembly 缺少完整复制证明")
    require(type(assembly.get("overlay_files")) is int and assembly["overlay_files"] > 0,
            "assembly overlay 数量无效")
    require(assembly.get("registry_modified") is False and assembly.get("ydwe_started") is False,
            "assembly 已运行或修改注册表；拒绝此次初始 loader 准备")
    for name in HOST_REQUIRED:
        require(locate(host, name).is_file(), f"宿主必需依赖缺失: {name}")
    for name in PRELOAD_NAMES:
        absent(game, name)
        absent(host, name)
    absent(game, LOADER_NAME)
    absent(game, CONFIG_NAME)
    own_loader = locate(host, "bin/worldedit.exe")
    loader_identity, reference_identity = file_identity(own_loader), file_identity(reference)
    require(loader_identity["sha256"] == expected and reference_identity == loader_identity,
            "OWN loader 与显式 SHA 锁定的原安装参考不逐字节相同")
    game_files, game_directories = inventory(game, "game", host if game in host.parents else None)
    host_files, host_directories = inventory(host, "host")
    require(len(game_files) + len(host_files) <= MAX_FILES, "合并文件清单超过预算")
    logs = host / "logs"
    if logs.exists():
        require(plain(logs).is_dir(), "已有 logs 不是普通目录")
    return {"schema": "p1-native-loader-input-v1", "gameRoot": str(game), "hostRoot": str(host),
            "expectedLoaderSha256": expected,
            "loaderReference": {"path": str(reference), **reference_identity},
            "loaderSource": {"root": "host", "relativePath": "bin/worldedit.exe", **loader_identity},
            "gameManifest": game_manifest, "hostAssembly": assembly_identity,
            "dependencies": game_files + host_files,
            "directories": {"game": game_directories, "host": host_directories},
            "logsPreviouslyExisted": logs.exists(),
            "launched": False, "registryWritten": False, "nativeSaveValidated": False}


def receipt_target(value, game, host, reference):
    target = plain(value)
    require(not target.exists() and target.parent.is_dir(), "receipt 必须为已有父目录下的独占新路径")
    require(disjoint(target, game) and disjoint(target, host) and target != reference,
            "receipt 不能写入游戏/宿主或覆盖来源")
    require(disjoint(target.parent, reference.parent), "receipt 不能写入原安装参考目录")
    return target


def save_receipt(stream, data):
    stream.seek(0)
    json.dump(data, stream, ensure_ascii=False, indent=2, allow_nan=False)
    stream.write("\n")
    stream.truncate()
    stream.flush()
    os.fsync(stream.fileno())


def inspect(args):
    snapshot = collect(args.game_root, args.host_root, args.loader_reference, args.expected_loader_sha256)
    output = receipt_target(args.receipt, plain(args.game_root), plain(args.host_root), plain(args.loader_reference))
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        save_receipt(stream, snapshot)
    return {"status": "inspected", "receipt": str(output), "sha256": file_identity(output)["sha256"],
            "files": len(snapshot["dependencies"]), "launched": False}


def prepare(args):
    game, host, reference = plain(args.game_root), plain(args.host_root), plain(args.loader_reference)
    input_path = plain(args.input_inventory)
    require(disjoint(input_path, game) and disjoint(input_path, host), "input inventory 必须在游戏/宿主外")
    previous, input_identity = read_json(input_path)
    require(input_identity["sha256"] == expected_sha(args.expected_input_sha256), "input inventory 显式 SHA 不符")
    current = collect(game, host, reference, args.expected_loader_sha256)
    require(previous == current, "锁定 input inventory 与实际完整文件/目录清单不同")
    output = receipt_target(args.receipt, game, host, reference)
    require(output != input_path, "最终 receipt 不能覆盖 input inventory")
    result = {"schema": "p1-native-loader-v1", "status": "preparing", "gameRoot": str(game), "hostRoot": str(host),
              "loader": {"relativePath": LOADER_NAME, "bytes": current["loaderSource"]["bytes"],
                         "sha256": current["expectedLoaderSha256"]},
              "config": {"relativePath": CONFIG_NAME, "bytes": len(EDITOR_LOADER_CONFIG),
                         "sha256": sha_bytes(EDITOR_LOADER_CONFIG)},
              "dependencies": current["dependencies"], "directories": current["directories"],
              "loaderReference": current["loaderReference"],
              "inputInventory": {"path": str(input_path), **input_identity},
              "gameManifest": current["gameManifest"], "hostAssembly": current["hostAssembly"],
              "hostLogs": {"path": str(host / "logs"), "created": False},
              "sourceUnchanged": False, "launched": False, "registryWritten": False, "nativeSaveValidated": False,
              "createdFiles": [], "limitations": ["preparation-only", "no-runtime-path-proof", "no-process-isolation-proof"]}
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        save_receipt(stream, result)
        try:
            # 新输出全部 CreateNew；失败保留独占文件和同一回执，不清理、不覆写重试。
            loader = plain(game / LOADER_NAME)
            with locate(host, "bin/worldedit.exe").open("rb") as incoming, loader.open("xb") as outgoing:
                result["createdFiles"].append(str(loader))
                shutil.copyfileobj(incoming, outgoing, length=1024 * 1024)
                outgoing.flush()
                os.fsync(outgoing.fileno())
            require(file_identity(loader) == {key: result["loader"][key] for key in ("bytes", "sha256")},
                    "新 loader 副本字节不一致")
            config = plain(game / CONFIG_NAME)
            with config.open("xb") as outgoing:
                result["createdFiles"].append(str(config))
                outgoing.write(EDITOR_LOADER_CONFIG)
                outgoing.flush()
                os.fsync(outgoing.fileno())
            require(file_identity(config) == {key: result["config"][key] for key in ("bytes", "sha256")},
                    "新 config 固定模板字节不一致")
            if not current["logsPreviouslyExisted"]:
                plain(host / "logs").mkdir()
                result["hostLogs"]["created"] = True
            require(file_identity(reference) == {key: current["loaderReference"][key] for key in ("bytes", "sha256")},
                    "原安装 loader 在准备期间变化")
            require(read_json(input_path)[1] == input_identity, "input inventory 在准备期间变化")
            after_game, after_game_directories = inventory(game, "game", host if game in host.parents else None)
            after_host, after_host_directories = inventory(host, "host")
            added = [{"root": "game", **result[key]} for key in ("loader", "config")]
            expected_files = sorted(current["dependencies"] + added, key=lambda row: (row["root"], row["relativePath"]))
            require(sorted(after_game + after_host, key=lambda row: (row["root"], row["relativePath"])) == expected_files,
                    "准备期间游戏/宿主出现未知更改，原输入未通过前后 SHA 校验")
            expected_host_directories = current["directories"]["host"] + ([] if current["logsPreviouslyExisted"] else ["logs"])
            require(set(after_game_directories) == set(current["directories"]["game"])
                    and set(after_host_directories) == set(expected_host_directories), "准备期间出现未知目录变化")
            result.update(status="prepared", sourceUnchanged=True, dependencies=after_game + after_host,
                          directories={"game": after_game_directories, "host": after_host_directories})
            save_receipt(stream, result)
        except Exception as error:
            result.update(status="failed", errorType=type(error).__name__, error=str(error))
            save_receipt(stream, result)
            raise
    return {"status": result["status"], "receipt": str(output), "sha256": file_identity(output)["sha256"],
            "files": len(result["dependencies"]), "launched": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "prepare"):
        child = commands.add_parser(name)
        for option in ("game-root", "host-root", "loader-reference", "expected-loader-sha256", "receipt"):
            child.add_argument("--" + option, required=True)
        if name == "prepare":
            child.add_argument("--input-inventory", required=True)
            child.add_argument("--expected-input-sha256", required=True)
    args = parser.parse_args()
    try:
        result = inspect(args) if args.command == "inspect" else prepare(args)
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except Exception as error:
        print(json.dumps({"status": "failed", "errorType": type(error).__name__, "error": str(error)},
                         ensure_ascii=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
