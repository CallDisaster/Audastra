"""参数化盘点、只读复制和显式启动独立 Warcraft 沙盒；不证明游戏进图。"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

_spec = importlib.util.spec_from_file_location("p1_support", Path(__file__).with_name("p1-support.py"))
support = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(support)

# 经典 1.27a 的基础文件清单；扩展/JAPI/插件只能由调用者的清单明确加入。
DEFAULT_PROFILE = {
    "required": ["war3.exe", "Game.dll", "Storm.dll", "War3.mpq", "War3x.mpq", "War3xlocal.mpq", "War3Patch.mpq",
                 "Mss32.dll", "ijl15.dll", "msvcr120.dll"],
    "optional": ["Warcraft III.exe", "blizzard.ax", "Movies", "War3Local.mpq"],
}


def profile_paths(profile):
    support.require(isinstance(profile, dict), "清单必须是 JSON 对象")
    rows, seen = [], set()
    for kind in ("required", "optional"):
        values = profile.get(kind, [])
        support.require(isinstance(values, list), f"{kind} 必须为数组")
        for value in values:
            relative = support.relative_path(value)
            key = relative.as_posix().casefold()
            support.require(key not in seen, f"清单重复路径: {value}")
            support.require(not any(key.startswith(old + "/") or old.startswith(key + "/") for old in seen),
                            f"清单父子路径重复: {value}")
            seen.add(key)
            rows.append((relative, kind))
    support.require(any(kind == "required" for _, kind in rows), "清单至少要有一个必需文件")
    return rows


def locate(root, relative):
    # 允许 Linux 夹具验证 Windows 文件名大小写语义；每一级都拒绝歧义。
    current = root
    for part in relative.parts:
        if not current.is_dir():
            return None
        matches = [p for p in current.iterdir() if p.name.casefold() == part.casefold()]
        support.require(len(matches) <= 1, f"来源大小写歧义: {relative}")
        if not matches:
            return None
        current = support.plain_path(matches[0])
    return current


def inventory(source, profile, *, hashes=False):
    source = support.plain_path(source)
    support.require(source.is_dir(), "来源目录不存在")
    rows = []
    for relative, kind in profile_paths(profile):
        found = locate(source, relative)
        row = {"path": relative.as_posix(), "required": kind == "required", "present": found is not None}
        if found:
            files = [found] if found.is_file() else sorted(found.rglob("*"))
            size, count = 0, 0
            identities = []
            for file in files:
                support.plain_path(file)
                if file.is_file():
                    size += file.stat().st_size
                    count += 1
                    if hashes:
                        identities.append({"path": file.relative_to(source).as_posix(), "bytes": file.stat().st_size,
                                           "sha256": support.sha256(file)})
            row.update(bytes=size, file_count=count, kind="file" if found.is_file() else "directory")
            if hashes:
                row["files"] = identities
        rows.append(row)
    return {"schema": "p1-game-inventory-v1", "source": str(source), "members": rows,
            "missing_required": [row["path"] for row in rows if row["required"] and not row["present"]],
            "runtime_validated": False, "japi_chain_validated": False}


def prepare(source, sandbox, profile):
    source, sandbox = support.separate_paths(source, sandbox)
    support.require(not sandbox.exists(), "沙盒目录必须不存在，拒绝覆盖或复用")
    before = inventory(source, profile, hashes=True)
    support.require(not before["missing_required"], f"缺必需文件: {before['missing_required']}")
    identities = [entry for member in before["members"] for entry in member.get("files", [])]
    support.require(len({entry["path"].casefold() for entry in identities}) == len(identities),
                    "复制清单包含 Windows 大小写冲突")
    sandbox.mkdir(parents=True)
    copied = []
    try:
        for identity in identities:
            relative = support.relative_path(identity["path"])
            origin, output = source / relative, sandbox / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            with origin.open("rb") as incoming, output.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing, length=1024 * 1024)
            support.require(output.stat().st_size == identity["bytes"] and support.sha256(output) == identity["sha256"],
                            f"沙盒副本身份不一致: {relative}")
            copied.append(identity)
        for identity in identities:
            support.require(support.sha256(source / support.relative_path(identity["path"])) == identity["sha256"],
                            "来源在复制期间变化；沙盒未通过")
        receipt = {"schema": "p1-game-sandbox-v1", "status": "prepared", "source": str(source), "sandbox": str(sandbox),
                   "files": copied, "missing_optional": [m["path"] for m in before["members"] if not m["present"]],
                   "profile": profile, "source_unchanged": True, "runtime_validated": False,
                   "japi_chain_validated": False, "author_confirmation_required": True}
    except Exception as error:
        support.write_json(sandbox / "p1-sandbox-failed.json", {"status": "failed", "reason": str(error), "copied": copied})
        raise
    support.write_json(sandbox / "p1-sandbox.json", receipt)
    return receipt


def launch(sandbox, map_path, launcher_relative, arguments):
    sandbox = support.plain_path(sandbox)
    receipt = support.read_json(sandbox / "p1-sandbox.json")
    support.require(receipt.get("status") == "prepared" and receipt.get("schema") == "p1-game-sandbox-v1", "沙盒没有通过准备")
    support.require(support.plain_path(receipt["sandbox"]) == sandbox, "沙盒回执目录不符")
    copied = {entry["path"].casefold() for entry in receipt["files"]}
    for file in sandbox.rglob("*"):
        support.plain_path(file)
        if file.is_file() and file.suffix.lower() in (".exe", ".dll", ".asi"):
            support.require(file.relative_to(sandbox).as_posix().casefold() in copied,
                            f"沙盒出现未记录可执行模块: {file.name}")
    for entry in receipt["files"]:
        file = support.plain_path(sandbox / support.relative_path(entry["path"]))
        support.require(file.is_file() and support.sha256(file) == entry["sha256"], f"沙盒文件变化: {entry['path']}")
    launcher_name = support.relative_path(launcher_relative).as_posix()
    support.require(launcher_name.casefold() in {row["path"].casefold() for row in receipt["files"]}, "启动器未包含在复制清单中")
    launcher = locate(sandbox, support.relative_path(launcher_relative))
    original = support.plain_path(map_path)
    support.require(original.is_file() and original.suffix.lower() in (".w3x", ".w3m"), "地图必须是现有 w3x/w3m")
    support.require(isinstance(arguments, list) and all(isinstance(x, str) and "\x00" not in x for x in arguments), "启动参数必须为字符串数组")
    support.require(any("{map}" in arg for arg in arguments), "启动参数必须包含 {map}")
    launch_dir = sandbox / "Maps" / "P1"
    support.plain_path(launch_dir)
    launch_dir.mkdir(parents=True, exist_ok=True)
    map_sha = support.sha256(original)
    output = launch_dir / (map_sha + original.suffix.lower())
    support.plain_path(output)
    if output.exists():
        support.require(support.sha256(output) == map_sha, "沙盒内地图身份不同")
    else:
        with original.open("rb") as incoming, output.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
    support.require(support.sha256(original) == map_sha and support.sha256(output) == map_sha, "地图复制期间变化")
    argv = [str(launcher), *[arg.replace("{map}", str(output)) for arg in arguments]]
    # 显式 launch 才启动；不等待、操作窗口或关闭任何进程。
    process = subprocess.Popen(argv, cwd=sandbox, shell=False, stdin=subprocess.DEVNULL)
    result = {"schema": "p1-game-launch-v1", "pid": process.pid, "argv": argv, "map_sha256": map_sha,
              "runtime_validated": False, "japi_chain_validated": False, "author_confirmation_required": True}
    support.write_json(sandbox / f"p1-launch-{process.pid}.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for action in ("inspect", "prepare"):
        child = sub.add_parser(action)
        child.add_argument("--source", required=True, type=Path)
        child.add_argument("--profile", type=Path, help="JSON required/optional 相对路径清单")
        if action == "inspect":
            child.add_argument("--report", required=True, type=Path)
            child.add_argument("--hashes", action="store_true")
        else:
            child.add_argument("--sandbox", required=True, type=Path)
    child = sub.add_parser("launch")
    child.add_argument("--sandbox", required=True, type=Path)
    child.add_argument("--map", required=True, type=Path)
    child.add_argument("--launcher-relative", default="war3.exe")
    child.add_argument("--arguments-json", required=True, help='例: ["-window","-loadfile","{map}"]；JAPI需要明确的加载链')
    args = parser.parse_args()
    if args.command == "inspect":
        result = inventory(args.source, support.read_json(args.profile) if args.profile else DEFAULT_PROFILE, hashes=args.hashes)
        support.write_json(args.report, result)
        print(json.dumps({"missing_required": result["missing_required"], "report": str(args.report)}, ensure_ascii=False))
        return 1 if result["missing_required"] else 0
    if args.command == "prepare":
        result = prepare(args.source, args.sandbox, support.read_json(args.profile) if args.profile else DEFAULT_PROFILE)
        print(json.dumps({"status": result["status"], "files": len(result["files"]), "sandbox": str(args.sandbox)}, ensure_ascii=False))
    else:
        result = launch(args.sandbox, args.map, args.launcher_relative, json.loads(args.arguments_json))
        print(json.dumps({"pid": result["pid"], "runtime_validated": False}))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"P1 sandbox: {error}", file=sys.stderr)
        sys.exit(2)
