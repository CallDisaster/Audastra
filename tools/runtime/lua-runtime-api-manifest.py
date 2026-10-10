"""从真实隔离 IPC 观察生成只读本机接口清单，或核对现有清单是否仍匹配。

缺证据、来源变化、假 fixture、未知/不完整枚举都不能变成完整语义检查成功。
该工具不加载 DLL、不启动游戏、不调用 API，不复制用户运行库。
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re
import sys

sys.dont_write_bytecode = True
EXPECTED_MODULES = {"ydbase.dll", "lua53.dll", "ydwar3.dll", "yd_loader.dll", "yd_jass_api.dll", "yd_lua_engine.dll"}
MAX_JSON = 4 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    path = Path(path)
    for member in (path, *path.parents):
        if member.exists() or member.is_symlink():
            info = member.lstat()
            require(not member.is_symlink() and not getattr(info, "st_file_attributes", 0) & 0x400, "Source is a reparse point")
    require(path.is_file() and path.stat().st_nlink == 1 and path.stat().st_size <= 64 * 1024 * 1024, "Source file type/budget mismatch")
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def strict_json(text):
    require(len(text.encode("utf-8")) <= MAX_JSON, "JSON budget exceeded")
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON key")
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))


def read_json(path):
    path = Path(path)
    require(path.stat().st_size <= MAX_JSON, "JSON file budget exceeded")
    return strict_json(path.read_text(encoding="utf-8-sig"))


def snapshot_sources(modules):
    require(isinstance(modules, list) and len(modules) == 6, "Exactly six captured runtime sources required")
    seen, sources = set(), []
    for row in modules:
        require(isinstance(row, dict) and row.get("name") in EXPECTED_MODULES and row["name"] not in seen, "Runtime module list invalid")
        seen.add(row["name"])
        expected, actual = row.get("expectedSha256"), row.get("sha256")
        require(isinstance(expected, str) and re.fullmatch("[0-9a-f]{64}", expected) and expected == actual, "Expected/actually loaded runtime SHA differ")
        require(row.get("loaded") is True, "Runtime module was not actually loaded")
        if row["name"] in ("yd_jass_api.dll", "yd_lua_engine.dll"):
            require(row.get("initializeCalled") is True, "Runtime Initialize was not observed")
        require(digest(row["path"]) == actual, "Runtime source is missing or stale: " + row["name"])
        sources.append({"name": row["name"], "path": row["path"], "sha256": actual})
    require(seen == EXPECTED_MODULES, "Runtime module coverage incomplete")
    return sources


def blocked_write_proof(probe, isolation, runtime_sources):
    records, count = probe.get("blockedWrites"), probe.get("blockedWriteCount")
    require(isinstance(records, list) and type(count) is int and 0 <= count <= 32 and count == len(records)
            and probe.get("blockedWriteOverflow") is False and type(probe.get("fatalWriteCount")) is int and probe["fatalWriteCount"] == 0,
            "Blocked write ledger missing, fatal or overflowed")
    require(type(probe.get("writeDenied")) is bool and probe["writeDenied"] == (count > 0), "Blocked write flag/count differ")
    configured = probe.get("nonfatalBlockedPaths")
    require(isinstance(configured, list) and len(configured) <= 4 and all(isinstance(path, str) and path for path in configured)
            and len({str(PureWindowsPath(path)).casefold() for path in configured}) == len(configured)
            and configured == isolation.get("nonfatalBlockedPaths"), "Nonfatal blocked paths differ from bounded unique explicit same-run input")
    for row in records:
        require(isinstance(row, dict) and row.get("path") in configured
                and row.get("nonfatal") is True and row.get("ready") is True
                and type(row.get("ntStatus")) is int and row["ntStatus"] == 0xC0000022
                and row.get("originalNtDispatched") is False,
                "Write rejection was not an exact configured pre-dispatch ACCESS_DENIED")
    for path in configured:
        # 此路径只允许拒写后继续，绝不应授权作者安装中的任何文件。
        candidate = PureWindowsPath(path)
        require(candidate.is_absolute() and not path.startswith("\\\\")
                and all(part not in (".", "..") and not part.endswith((".", " ")) and ":" not in part for part in candidate.parts[1:]),
                "Nonfatal blocked path alias invalid")
        for source in runtime_sources:
            actual = PureWindowsPath(source["path"])
            runtime_root = actual.parent
            if actual.parent.name.casefold() == "bin": runtime_root = actual.parent.parent
            elif actual.parent.name.casefold() == "warcraft3" and actual.parent.parent.name.casefold() == "plugin": runtime_root = actual.parent.parent.parent
            require(candidate != runtime_root and runtime_root not in candidate.parents, "Author runtime write must remain fatal")
    return {"configuredNonfatalBlockedPaths": configured, "writeDenied": probe["writeDenied"],
            "blockedWriteCount": count, "blockedWrites": records,
            "scope": "Recorded NT open requests rejected before original dispatch; no complete OS sandbox claim"}


def graphics_backend_proof(launch, isolation):
    """绑定实际启动请求；没有后端实测，不能从参数或导入 DLL 推断实际 renderer。"""
    require(launch.get("processCreated") is True and type(launch.get("gameProcessId")) is int
            and launch["gameProcessId"] > 0, "Original Game process creation identity missing")
    argv = isolation.get("argv")
    require(isinstance(argv, list) and len(argv) >= 5 and len(argv) % 2 == 1
            and all(type(value) is str and value for value in argv)
            and not argv[0].startswith("--"), "Physical launcher argv shape missing")
    pairs = list(zip(argv[1::2], argv[2::2]))
    require(all(key.startswith("--") and len(key) > 2 for key, value in pairs), "Physical launcher argv pairs invalid")

    def single(key, required):
        values = [value for option, value in pairs if option == key]
        require(len(values) == 1 if required else len(values) <= 1, "Missing/duplicate physical launcher option: " + key)
        return values[0] if values else None

    game, map_name = single("--game", True), single("--map", True)
    renderer = single("--probe-renderer", False)
    require(renderer is None or renderer == "opengl", "Unsupported explicit probe renderer")
    game_path, map_path = PureWindowsPath(game), PureWindowsPath(map_name)
    require(game_path.is_absolute() and not game.startswith("\\\\") and game_path.name.casefold() == "war3.exe",
            "Physical Game executable path invalid")
    require(not map_path.is_absolute() and not map_path.drive and not map_path.root
            and len(map_name) < 54 and len(map_path.parts) >= 3 and map_path.parts[0].casefold() == "maps"
            and map_path.suffix in (".w3x", ".w3m")
            and all(32 <= ord(value) <= 126 and value not in ':"<>|?*' for value in map_name)
            and all(part not in (".", "..") and not part.endswith((".", " ")) for part in re.split(r"[\\/]", map_name)),
            "Physical relative probe map path invalid")
    requested = "opengl" if renderer is not None else "game-default"
    require(launch.get("probeGraphicsBackendRequested") == requested
            and isolation.get("probeGraphicsBackendRequested") == requested, "Same-run graphics request differs")
    require(launch.get("gameplayVerified") is False and isolation.get("gameplayVerified") is False,
            "Metadata observation falsely claimed gameplay verification")
    expected = [game, "-window", *(["-opengl"] if renderer is not None else []), "-loadfile", map_name]
    native = launch.get("gameArgv")
    require(isinstance(native, list) and all(type(value) is str for value in native) and native == expected,
            "Actual constructed Game argv differs from same-run physical request")
    return {"requested": requested, "actualBackend": "unknown", "actualBackendVerified": False,
            "gameArgv": expected, "argvSameRunBound": True, "backendSpecificAbsenceNotProven": True}


def bridge_desktop_proof(launch, isolation, nonce):
    name = "AudastraRuntimeProbe-" + nonce
    require(isolation.get("desktop", {}).get("name") == name, "Created Desktop identity missing")
    mode = isolation.get("desktopWitnessMode")
    require(mode in ("none", "hidden-window"), "Unknown/missing explicit Desktop witness mode")
    pid, tid = isolation.get("bridgePid"), isolation.get("bridgeThreadId")
    require(type(pid) is int and pid > 0 and type(tid) is int and tid > 0, "Original bridge PID/TID missing")
    proof, query = isolation.get("bridgeDesktop", {}), isolation.get("bridgeThreadDesktopQuery", {})
    require(isinstance(proof, dict) and proof.get("name") == name and type(proof.get("threadId")) is int and proof["threadId"] == tid
            and isinstance(query, dict) and type(query.get("threadId")) is int and query["threadId"] == tid and type(query.get("returnedNull")) is bool
            and type(query.get("lastError")) is int and 0 <= query["lastError"] <= 0xFFFFFFFF,
            "Desktop observation/query is not bound to original bridge thread")
    if query["returnedNull"]:
        require("name" not in query, "NULL Desktop query fabricated a Desktop name")
    else:
        require(query.get("name") == name, "Direct Desktop query contradicts expected Desktop")
    argv = isolation.get("argv")
    require(isinstance(argv, list) and all(isinstance(value, str) for value in argv), "Physical launcher argv missing")
    positions = [index for index, value in enumerate(argv) if value == "--probe-desktop-witness"]
    if mode == "none":
        require(not positions and "desktopWitness" not in launch and proof.get("proofKind") == "thread-desktop"
                and not query["returnedNull"] and proof.get("borrowedHandle") is True
                and type(proof.get("handle")) is int and proof["handle"] > 0, "Actual direct Desktop proof missing/unsolicited witness")
        return {**proof, "mode": mode, "query": query, "bridgePid": pid}
    require(len(positions) == 1 and positions[0] + 1 < len(argv) and argv[positions[0] + 1] == "hidden-window"
            and proof.get("proofKind") == "hidden-window" and "handle" not in proof and "borrowedHandle" not in proof,
            "Hidden Desktop witness not explicitly requested or fabricated borrowed handle")
    native = launch.get("desktopWitness")
    require(isinstance(native, dict) and native.get("requested") is True and native.get("mode") == "hidden-window"
            and native.get("proofKind") == "hidden-window" and native.get("nonce") == nonce
            and native.get("created") is True and native.get("destroyed") is True and native.get("unregistered") is True
            and type(native.get("errorWin32")) is int and native["errorWin32"] == 0, "Native witness creation/cleanup incomplete")
    require(proof.get("className") == "AudastraProbeDesktopWitness-" + nonce
            and type(proof.get("hwnd")) is int and proof["hwnd"] > 0
            and type(proof.get("ownerPid")) is int and proof["ownerPid"] == pid
            and type(proof.get("ownerTid")) is int and proof["ownerTid"] == tid,
            "Hidden HWND class/PID/TID does not identify original bridge")
    require(type(proof.get("style")) is int and 0 < proof["style"] <= 0xFFFFFFFF
            and proof["style"] & 0x80000000 and not proof["style"] & 0x50000000
            and type(proof.get("exStyle")) is int and proof["exStyle"] == 0x08000080 and proof.get("visible") is False,
            "Hidden witness is visible, child or activates/toolwindow styles differ")
    for field in ("originalProcessAlive", "originalThreadAlive", "originalProcessInOwnedJob", "observationRepeated", "enumerationComplete"):
        require(proof.get(field) is True, "External witness proof failed: " + field)
    require(type(proof.get("windowCount")) is int and 1 <= proof["windowCount"] <= 64
            and type(proof.get("originalProcessCreationFileTime")) is int and proof["originalProcessCreationFileTime"] > 0,
            "Incomplete/overflowed Desktop enumeration or original process identity missing")
    for field in ("className", "hwnd", "threadId", "style", "exStyle", "visible"):
        require(type(native.get(field)) is type(proof[field]) and native[field] == proof[field], "Native/external HWND pair differs: " + field)
    require(type(native.get("bridgePid")) is int and native["bridgePid"] == pid, "Native witness belongs to another bridge")
    return {**proof, "mode": mode, "query": query, "bridgePid": pid, "nativeWitness": native}


def build_manifest(preparation, launch, isolation, common_path):
    require(preparation.get("schema") == "audastra-lua-api-probe-preparation-v1"
            and preparation.get("authorScriptsIncluded") is False, "Original map preparation missing")
    nonce = preparation.get("nonce")
    require(isinstance(nonce, str) and re.fullmatch("[0-9a-f]{32}", nonce), "Nonce invalid")
    require(isolation.get("schema") == "audastra-isolated-lua-api-probe-v1" and isolation.get("succeeded") is True
            and isolation.get("nonce") == nonce and isolation.get("bridgeExitCode") == 0, "Isolated run not confirmed")
    require(isolation.get("inputDesktopBefore") == isolation.get("inputDesktopAfter") and isolation.get("inputDesktopBefore"), "Input Desktop changed/unknown")
    graphics_proof = graphics_backend_proof(launch, isolation)
    desktop_proof = bridge_desktop_proof(launch, isolation, nonce)
    require(isolation.get("jobBeforeClose", {}).get("enumerationComplete") is True
            and isolation.get("jobBeforeClose", {}).get("unresolvedAssociatedPids") == [], "Owned Job not terminal")
    require(isolation.get("desktopClosed") is True and isolation.get("jobClosed") is True
            and isolation.get("threadClosed") is True and isolation.get("bridgeHandleClosed") is True
            and not isolation.get("errors"), "Isolation handles/cleanup failed")
    require(isolation.get("luaEnvironmentOverridesAbsent") is True, "Inherited Lua source environment was not excluded")
    require(launch.get("schema") == "audastra-warcraft-launch-v1" and launch.get("bridgeExitCode") == 0
            and launch.get("hookInstalled") is True and launch.get("gameObserved") is True
            and launch.get("runtimeInitialized") is True and launch.get("errorStage") == "lua-api-probe-captured", "Actual runtime initialization/probe capture missing")
    probe = launch.get("luaApiProbe", {})
    require(probe.get("enabled") is True and probe.get("nonce") == nonce and probe.get("captured") is True
            and probe.get("writeGuardInstalled") is True and probe.get("observerInstalled") is True, "Readonly observation proof missing")
    require(probe.get("mapSha256") == preparation.get("mapSha256") and probe.get("scriptSha256") == preparation.get("luaSha256"), "Owned map/probe bindings differ")
    require(digest(preparation["mapPath"]) == preparation["mapSha256"] and digest(preparation["luaPath"]) == preparation["luaSha256"], "Original probe source changed")
    require(digest(common_path) == preparation.get("commonSha256"), "common.j signature source changed")
    sources = snapshot_sources(launch.get("modules"))
    write_proof = blocked_write_proof(probe, isolation, sources)
    engine = next(row for row in sources if row["name"] == "yd_lua_engine.dll")
    require(engine["sha256"] == preparation.get("runtimeEngineSha256"), "Binary candidate discovery used another engine")
    require(launch.get("gameDllSha256") == launch.get("expectedGameDllSha256")
            and digest(launch["gameDllPath"]) == launch["gameDllSha256"], "Actual Game.dll source changed")
    observed = strict_json(probe.get("payloadJson", ""))
    require(observed.get("schema") == "audastra-lua-api-observation-v1" and observed.get("nonce") == nonce
            and observed.get("completed") is True and observed.get("fixture") is not True
            and observed.get("gameApiFunctionsInvokedByProbe") == 0 and observed.get("runtimePreloadOnly") is True,
            "No genuine completed metadata observation")
    signatures = {}
    for signature in preparation.get("commonSignatures", []):
        require(isinstance(signature, dict) and isinstance(signature.get("name"), str)
                and signature["name"] not in signatures and type(signature.get("parameterCount")) is int
                and len(signature.get("parameters", [])) == signature["parameterCount"], "Actual common.j signature index invalid")
        signatures[signature["name"]] = signature
    modules, seen, field_count = [], set(), 0
    require(isinstance(observed.get("modules"), list) and 0 < len(observed["modules"]) <= 64, "Runtime module observation budget/coverage invalid")
    for source in observed["modules"]:
        name = source.get("name")
        require(isinstance(name, str) and re.fullmatch(r"jass\.[A-Za-z0-9_.]+", name) and name not in seen, "Observed module identity invalid")
        require(type(source.get("available")) is bool and source.get("enumerationComplete") is False, "Partial observation falsely claimed complete coverage")
        seen.add(name)
        module = {"name": name, "observedAvailable": source["available"], "absenceProven": False,
                  "enumerationComplete": False, "reason": source.get("reason", "Partial runtime observation"), "fields": []}
        names = set()
        for field in source.get("fields", []):
            require(isinstance(field, dict) and isinstance(field.get("name"), str)
                    and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", field["name"]) and field["name"] not in names
                    and field.get("kind") in ("function", "table", "string", "number", "boolean", "userdata", "thread", "nil"), "Observed field invalid")
            field_count += 1
            require(field_count <= 24000 and len(field["name"]) <= 256, "Observed fields budget exceeded")
            names.add(field["name"])
            row = {"name": field["name"], "kind": field["kind"], "origin": field.get("origin", "unknown"), "signature": None}
            if name == "jass.common" and row["kind"] == "function" and row["name"] in signatures:
                row["signature"] = {**signatures[row["name"]], "source": "actual-common.j-declaration",
                                    "sourceSha256": preparation["commonSha256"]}
            module["fields"].append(row)
        require(source["available"] or not module["fields"], "Unavailable module fabricated fields")
        modules.append(module)
    return {"schema": "audastra-lua-runtime-api-manifest-v1", "scope": "actual-runtime-observed-partial",
            "fullyEnumerated": False, "unknownDoesNotMeanMissing": True, "nonce": nonce,
            "runtimeSources": sources, "gameSource": {"path": launch["gameDllPath"], "sha256": launch["gameDllSha256"]},
            "commonSource": {"path": str(common_path), "sha256": preparation["commonSha256"]},
            "probeMapSha256": preparation["mapSha256"], "probeScriptSha256": preparation["luaSha256"],
            "moduleCount": len(modules), "fieldCount": field_count, "modules": modules,
            "writeContainment": write_proof,
            "desktopProof": desktop_proof,
            "graphicsBackend": graphics_proof, "gameplayVerified": False,
            "semanticChecksComplete": False, "savePermissionGranted": False,
            "reason": "Only positively observed fields and common.j signatures; missing globals, lazy fields and other signatures remain unknown"}


def verify_manifest(manifest):
    require(manifest.get("schema") == "audastra-lua-runtime-api-manifest-v1"
            and manifest.get("scope") == "actual-runtime-observed-partial" and manifest.get("fullyEnumerated") is False
            and manifest.get("unknownDoesNotMeanMissing") is True and manifest.get("savePermissionGranted") is False
            and manifest.get("gameplayVerified") is False,
            "Manifest schema/scope invalid")
    sources = manifest.get("runtimeSources")
    require(isinstance(sources, list) and len(sources) == 6 and {row.get("name") for row in sources} == EXPECTED_MODULES, "Manifest sources incomplete")
    for source in [*sources, manifest["gameSource"], manifest["commonSource"]]:
        require(digest(source["path"]) == source["sha256"], "Manifest source is missing or stale: " + str(source["path"]))
    evidence = manifest.get("evidenceSources")
    require(isinstance(evidence, list) and len(evidence) == 3
            and {source.get("role") for source in evidence} == {"preparation", "launcher-receipt", "isolation-receipt"},
            "Evidence sources incomplete")
    receipts = {}
    for source in evidence:
        path = Path(source["path"])
        require(path.is_absolute() and path.is_file() and digest(path) == source.get("sha256"), "Evidence source is missing or stale")
        receipts[source["role"]] = read_json(path)
    by_role = {source["role"]: source for source in evidence}
    isolation = receipts["isolation-receipt"]
    require(by_role["preparation"]["sha256"] == isolation.get("preparationSha256")
            and by_role["launcher-receipt"]["sha256"] == isolation.get("launcherReceiptSha256"), "Receipt source SHA binding mismatch")
    # 与生成使用同一个接纳入口；不可把任意改写的字段/签名再次签为已观察。
    canonical = build_manifest(receipts["preparation"], receipts["launcher-receipt"], isolation, manifest["commonSource"]["path"])
    # Python 的值比较把 False/0、True/1 和 1/1.0 混同；JSON 型别属于冻结证据合同。
    actual_text = json.dumps({key: value for key, value in manifest.items() if key != "evidenceSources"},
                             sort_keys=True, allow_nan=False, separators=(",", ":"))
    canonical_text = json.dumps(canonical, sort_keys=True, allow_nan=False, separators=(",", ":"))
    require(actual_text == canonical_text,
            "Manifest observed fields/signatures differ from frozen evidence")
    for source in evidence:
        require(digest(source["path"]) == source["sha256"], "Evidence changed during verification")
    return {"status": "verified-partial", "moduleCount": manifest["moduleCount"], "fieldCount": manifest["fieldCount"],
            "semanticChecksComplete": False, "unknownDoesNotMeanMissing": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    operations = parser.add_subparsers(dest="operation", required=True)
    generate = operations.add_parser("generate")
    for name in ("preparation", "launcher-receipt", "isolation-receipt", "common", "output"):
        generate.add_argument("--" + name, required=True, type=Path)
    verify = operations.add_parser("verify")
    verify.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.operation == "verify":
            result = verify_manifest(read_json(args.manifest))
        else:
            preparation, launch, isolation = (read_json(value) for value in (args.preparation, args.launcher_receipt, args.isolation_receipt))
            require(digest(args.preparation) == isolation.get("preparationSha256")
                    and digest(args.launcher_receipt) == isolation.get("launcherReceiptSha256"), "Receipt source SHA binding mismatch")
            result = build_manifest(preparation, launch, isolation, args.common)
            result["evidenceSources"] = [{"role": role, "path": str(path.absolute()), "sha256": digest(path)}
                                         for role, path in zip(("preparation", "launcher-receipt", "isolation-receipt"),
                                                               (args.preparation, args.launcher_receipt, args.isolation_receipt))]
            verify_manifest(result)
            with args.output.open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
                stream.write("\n")
            result = {"status": "generated-partial", "path": str(args.output), "sha256": digest(args.output),
                      "moduleCount": result["moduleCount"], "fieldCount": result["fieldCount"]}
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except Exception as error:
        print(json.dumps({"status": "fallback-syntax-only", "semanticChecksComplete": False,
                          "message": "目前只检查语法；可信运行库接口清单缺失、过期或证据不足。", "reason": str(error)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
