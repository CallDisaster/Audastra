"""Generate author-facing JASS wrappers and the exact host contract from one manifest.

This tool never authorizes a DLL, executes it, or opens game/third-party code.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re

NAME = re.compile(r"[a-z][a-z0-9_-]{0,31}\Z", re.ASCII)
COMMAND = re.compile(r"[a-z][a-z0-9_-]*(?:\.[a-z][a-z0-9_-]*)+\Z", re.ASCII)
VERSION = re.compile(r"[A-Za-z0-9._+\-]{1,63}\Z", re.ASCII)
KINDS = {"v": "AJ_VOID", "i": "AJ_INT", "r": "AJ_REAL", "s": "AJ_TEXT", "b": "AJ_BOOL", "d": "AJ_HANDLE"}
JASS = {"v": "boolean", "i": "integer", "r": "real", "s": "string", "b": "boolean", "d": "integer"}
SUFFIX = {"v": "Void", "i": "Int", "r": "Real", "s": "Text", "b": "Bool", "d": "Handle"}


def unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate JSON key: {key}")
        out[key] = value
    return out


def fields(obj, expected, label):
    if not isinstance(obj, dict) or set(obj) != set(expected):
        raise ValueError(f"{label}: expected exactly {sorted(expected)}")


def wrapper_name(name):
    # Injective mapping: underscores, dots and dashes cannot collapse to the same symbol.
    return "AjM_" + name.replace("_", "_u").replace(".", "_d").replace("-", "_h")


def validate(data):
    fields(data, {"schema", "abi", "name", "version", "commands"}, "manifest")
    if data["schema"] != "audastra.japi.module.v2" or type(data["abi"]) is not int or data["abi"] != 0x20000:
        raise ValueError("unsupported schema/ABI; legacy WarVK manifests are not V2 manifests")
    if not isinstance(data["name"], str) or not NAME.fullmatch(data["name"]) or data["name"] in {"host", "module"}:
        raise ValueError("invalid/reserved module name")
    if not isinstance(data["version"], str) or not VERSION.fullmatch(data["version"]):
        raise ValueError("invalid version")
    if not isinstance(data["commands"], list) or len(data["commands"]) > 256:
        raise ValueError("commands must be an array of at most 256 entries")
    seen = set()
    for command in data["commands"]:
        fields(command, {"name", "arguments", "result", "domain"}, "command")
        name = command["name"]
        if not isinstance(name, str) or len(name) > 63 or not COMMAND.fullmatch(name) or not name.startswith(data["name"] + ".") or name in seen:
            raise ValueError("invalid/duplicate command name")
        seen.add(name)
        args = command["arguments"]
        if not isinstance(args, str) or len(args) > 16 or any(c not in "irsbd" for c in args):
            raise ValueError("invalid argument types")
        if not isinstance(command["result"], str) or command["result"] not in KINDS or command["domain"] not in ("sync", "local"):
            raise ValueError("invalid result/domain")
    return data


def read_manifest(path):
    if path.suffix.lower() != ".json":
        raise ValueError("manifest must be an explicit .json file")
    if path.stat().st_size > 256 * 1024:
        raise ValueError("manifest exceeds 256 KiB")
    return validate(json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object))


def generate(data):
    validate(data)
    jass = ["// Generated from audastra.japi.module.v2. Include AudastraJapi.j first.",
            "// Inspect AjLastStatus immediately after every value-returning wrapper."]
    cpp = ["// Generated metadata, NOT a trusted DLL authorization.", "#pragma once", '#include "aj/Host.h"',
           "namespace audastra::japi::generated {", f"inline ModuleContract {wrapper_name(data['name'])}() {{",
           f'    return {{"{data["name"]}", "{data["version"]}", {{']
    for c in data["commands"]:
        params = ", ".join(f"{JASS[t]} a{i}" for i, t in enumerate(c["arguments"])) or "nothing"
        args = " + ".join(f"AjArg{SUFFIX[t]}(a{i})" for i, t in enumerate(c["arguments"])) or '""'
        jass.extend([f"// Domain: {c['domain']}. Void wrappers return success, not a module result.",
                     f"function {wrapper_name(c['name'])} takes {params} returns {JASS[c['result']]}",
                     f'    return AjCall{SUFFIX[c["result"]]}("{c["name"]}", {args})', "endfunction", ""])
        cpp.append(f'        {{"{c["name"]}", "{c["arguments"]}", {KINDS[c["result"]]}, AJ_{c["domain"].upper()}_COMMAND}},')
    cpp.extend(["    }};", "}", "}", ""])
    return "\n".join(jass), "\n".join(cpp)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--dll", type=Path, help="optional built module for package digest; never executed")
    args = parser.parse_args()
    try:
        data = read_manifest(args.manifest)
        jass, cpp = generate(data)
        package = None
        if args.dll:
            if args.dll.suffix.lower() != ".dll" or args.dll.name != data["name"] + ".dll":
                raise ValueError("DLL name must match the module name")
            if args.dll.stat().st_size > 64 * 1024 * 1024:
                raise ValueError("DLL exceeds 64 MiB")
            package = {"schema": "audastra.japi.package.v2", "module": data,
                       "dll": args.dll.name, "sha256": hashlib.sha256(args.dll.read_bytes()).hexdigest()}
        outputs = {data["name"] + ".j": jass, data["name"] + ".contract.h": cpp,
                   data["name"] + ".manifest.json": json.dumps(data, ensure_ascii=False, indent=2) + "\n"}
        if package:
            outputs[data["name"] + ".package.json"] = json.dumps(package, ensure_ascii=False, indent=2) + "\n"
        # A dedicated generated directory is required; refuse overwrites and input/output collisions.
        args.out.mkdir(parents=True, exist_ok=True)
        if any((args.out / name).exists() for name in outputs):
            raise ValueError("output already exists; choose a new generated directory")
        for name, contents in outputs.items():
            with (args.out / name).open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(contents)
        print(f"generated {len(outputs)} files for {data['name']}; no DLL authorization created")
    except (ValueError, OSError) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    main()
