from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPOSITORY_ROOT / "tools" / "warvk_codegen" / "generate_warvk.py"
WARNING = "仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。"
EXPECTED_PATHS = {
    "jass/WarVK.j",
    "catalog/mapui/ui/config",
    "catalog/mapui/ui/warvk/define.txt",
    "catalog/mapui/ui/warvk/action.txt",
    "catalog/mapui/ui/warvk/call.txt",
    "catalog/legacy/config",
    "catalog/legacy/warvk/ui/TriggerData.txt",
    "catalog/legacy/warvk/ui/TriggerStrings.txt",
    "protocol/warvk-v1-vectors.json",
    "warvk-handoff.json",
}
STATIC_VERIFIER = os.environ.get("WARVK_GENERATED_STATIC_VERIFIER")


def synthetic_manifest() -> dict:
    return {
        "schema": {"name": "warvk-japi-manifest", "version": 1},
        "apiVersion": "WarVK synthetic 1.0",
        "protocolVersion": 1,
        "provenance": {
            "method": "clean-room",
            "allowedEvidence": ["synthetic project-owned fixture", "stock native ABI"],
            "restrictedImplementationConsulted": False,
            "runtimeEffectClaimed": False,
        },
        "distribution": {
            "status": "blocked",
            "reason": "synthetic static fixture",
            "deploymentAuthorized": False,
            "mapArchiveIntegrationAuthorized": False,
        },
        "protocol": {
            "prefix": "warvk:",
            "canonicalPrefix": "warvk:v1;",
            "canonicalGrammar": "warvk:v1;<command>[;<typed-arg>...]",
            "legacyGrammar": "warvk:<command>[;<typed-arg>...]",
            "legacyParseOnly": True,
            "separator": ";",
            "typeSeparator": ":",
            "encoding": "ASCII",
            "maxBytes": 512,
            "maxArgs": 16,
            "integerPolicy": (
                "strict signed int32 decimal; no whitespace, sign-only token, or overflow"),
            "realPolicy": (
                "strict decimal, optional exponent, finite and representable; reject NaN, Inf, "
                "whitespace, and overflow"),
            "nonWarvkPolicy": "forward_original_unconditionally",
            "profileMismatchPolicy": "fail_closed_no_address_scan",
            "typedArgs": {
                "b": {"type": "bool", "grammar": "b:0|b:1"},
                "i": {"type": "i32", "grammar": "i:<strict-int32-decimal>"},
                "d": {"type": "id", "grammar": "d:<positive-int32-decimal>"},
                "r": {"type": "real", "grammar": "r:<finite-decimal>"},
            },
            "carriers": {
                "preloader": {
                    "stockNative": "Preloader", "abi": "(S)V", "role": "setter_or_command"},
                "hotkey": {
                    "stockNative": "GetLocalizedHotkey", "abi": "(S)I",
                    "role": "bool_i32_or_id_return"},
                "localized_string": {
                    "stockNative": "GetLocalizedString", "abi": "(S)S",
                    "role": "string_or_textual_real_return"},
            },
        },
        "publicApiPolicy": {
            "jassNamePattern": "^WarVK[A-Za-z0-9]+$",
            "allowedInputTypes": ["bool", "i32", "id", "real"],
            "allowedResultTypes": ["void", "bool", "i32", "id", "real", "string"],
            "prohibitedPublicTypes": [
                "raw_pointer", "address", "vulkan_handle", "framebuffer",
                "native_callback", "native_event", "c_ui_callback", "raw_image_handle",
                "war3_handle",
            ],
            "localVisualWarningZh": WARNING,
            "managedIdPolicy": (
                "Only positive WarVK-managed int32 ids are accepted; Warcraft handles and "
                "addresses are forbidden."),
        },
        "featureStatus": "declared_backend_required",
        "categories": [
            {"name": "system", "status": "internal_dispatch", "featureBit": -1},
            {"name": "pointLight", "status": "declared_backend_required", "featureBit": 0},
        ],
        "errorCodes": [
            {"value": 0, "name": "None", "stableText": "no error"},
            {"value": 1, "name": "InvalidArgument", "stableText": "invalid argument"},
        ],
        "gameDllProfile": {
            "profileId": "synthetic-game-profile",
            "identity": {
                "sha256": "11" * 32,
                "fileVersion": "synthetic",
                "productVersion": "synthetic",
                "fileLength": 1,
                "machine": "0x014C",
                "optionalHeaderMagic": "0x010B",
                "timeDateStamp": "0x1",
                "sizeOfImage": "0x1",
                "sectionCount": 1,
                "entryPointRva": "0x1",
                "preferredImageBase": "0x1",
                "checksum": "0x1",
            },
            "verificationPolicy": "synthetic exact identity; fail closed without scanning",
            "registrationTargetRva": "0x1",
            "carriers": [
                {
                    "name": "GetLocalizedString", "abi": "(S)S",
                    "implementationRva": "0x1", "implementationSize": "0x4",
                    "registrationRva": "0x2", "nameRva": "0x3", "signatureRva": "0x4",
                    "prologueBytes": "55 8B EC 90", "prologueMask": "FF FF FF FF",
                    "relocations": [],
                },
                {
                    "name": "GetLocalizedHotkey", "abi": "(S)I",
                    "implementationRva": "0x5", "implementationSize": "0x4",
                    "registrationRva": "0x6", "nameRva": "0x7", "signatureRva": "0x8",
                    "prologueBytes": "55 8B EC 90", "prologueMask": "FF FF FF FF",
                    "relocations": [],
                },
                {
                    "name": "Preloader", "abi": "(S)V",
                    "implementationRva": "0x9", "implementationSize": "0x4",
                    "registrationRva": "0xA", "nameRva": "0xB", "signatureRva": "0xC",
                    "prologueBytes": "55 8B EC 90", "prologueMask": "FF FF FF FF",
                    "relocations": [],
                },
            ],
            "helpers": [],
            "registrationEvidence": {
                "addressing": "module_base_plus_rva",
                "layout": "synthetic exact layout",
                "scanAllowed": False,
            },
        },
        "commands": [
            {
                "id": 0,
                "name": "system.version",
                "jassName": "WarVKGetVersion",
                "category": "system",
                "carrier": "localized_string",
                "args": [],
                "result": "string",
                "backendRequired": False,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "获取版本",
                "descriptionZh": WARNING + "返回版本文本。",
            },
            {
                "id": 1,
                "name": "system.protocolVersion",
                "jassName": "WarVKGetProtocolVersion",
                "category": "system",
                "carrier": "hotkey",
                "args": [],
                "result": "i32",
                "backendRequired": False,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "获取协议版本",
                "descriptionZh": WARNING + "返回协议版本。",
            },
            {
                "id": 2,
                "name": "system.ready",
                "jassName": "WarVKIsReady",
                "category": "system",
                "carrier": "hotkey",
                "args": [],
                "result": "bool",
                "backendRequired": False,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "查询就绪",
                "descriptionZh": WARNING + "返回本地就绪状态。",
            },
            {
                "id": 3,
                "name": "pointLight.create",
                "jassName": "WarVKCreatePointLight",
                "category": "pointLight",
                "carrier": "hotkey",
                "args": [{"name": "radius", "type": "real"}],
                "result": "id",
                "backendRequired": True,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "创建点光",
                "descriptionZh": WARNING + "以半径 ${radius} 创建本地点光。",
            },
            {
                "id": 4,
                "name": "pointLight.alive",
                "jassName": "WarVKIsPointLightAlive",
                "category": "pointLight",
                "carrier": "hotkey",
                "args": [{"name": "lightId", "type": "id"}],
                "result": "bool",
                "backendRequired": True,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "查询点光",
                "descriptionZh": WARNING + "查询点光 ${lightId}。",
            },
            {
                "id": 5,
                "name": "system.metric",
                "jassName": "WarVKGetMetric",
                "category": "system",
                "carrier": "localized_string",
                "args": [],
                "result": "real",
                "backendRequired": True,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "查询指标",
                "descriptionZh": WARNING + "返回本地指标。",
            },
            {
                "id": 6,
                "name": "pointLight.configure",
                "jassName": "WarVKConfigurePointLight",
                "category": "pointLight",
                "carrier": "preloader",
                "args": [
                    {"name": "enabled", "type": "bool"},
                    {"name": "quality", "type": "i32"},
                    {"name": "lightId", "type": "id"},
                    {"name": "radius", "type": "real"},
                ],
                "result": "void",
                "backendRequired": True,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "配置点光",
                "descriptionZh": (
                    WARNING + "设置开关 ${enabled}、质量 ${quality}、点光 ${lightId}、半径 ${radius}。"),
            },
            {
                "id": 7,
                "name": "system.clear",
                "jassName": "WarVKClearState",
                "category": "system",
                "carrier": "preloader",
                "args": [],
                "result": "void",
                "backendRequired": False,
                "localVisual": True,
                "querySyncWarning": True,
                "titleZh": "清除状态",
                "descriptionZh": WARNING + "清除本地状态。",
            },
        ],
    }


class GeneratorFixture:
    def __init__(self, root: Path, manifest: dict | None = None) -> None:
        self.root = root
        self.manifest_path = root / "synthetic-warvk-v1.json"
        self.output = root / "generated"
        self.write(manifest or synthetic_manifest())

    def write(self, manifest: dict) -> str:
        payload = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        self.manifest_path.write_bytes(payload)
        return hashlib.sha256(payload).hexdigest()

    def write_raw(self, payload: bytes) -> str:
        self.manifest_path.write_bytes(payload)
        return hashlib.sha256(payload).hexdigest()

    def run(self, expected_sha256: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(GENERATOR),
                "--manifest", str(self.manifest_path),
                "--expected-manifest-sha256", expected_sha256,
                "--output", str(self.output),
                *extra,
            ],
            text=True,
            capture_output=True,
            check=False,
        )


class WarVKCodegenTests(unittest.TestCase):
    def test_fixed_output_set_and_static_safety_contract(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            fixture = GeneratorFixture(Path(temporary))
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            result = fixture.run(digest.upper())
            self.assertEqual(result.returncode, 0, result.stderr)

            if STATIC_VERIFIER:
                verification = subprocess.run(
                    [
                        STATIC_VERIFIER,
                        str(fixture.output / "catalog/mapui"),
                        str(fixture.output / "catalog/legacy"),
                        str(fixture.output / "jass/WarVK.j"),
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(
                    verification.returncode,
                    0,
                    "generated Catalog/JASS static verifier failed:\n"
                    + verification.stdout + verification.stderr,
                )

            paths = {
                path.relative_to(fixture.output).as_posix()
                for path in fixture.output.rglob("*") if path.is_file()
            }
            self.assertEqual(paths, EXPECTED_PATHS)
            self.assertEqual((fixture.output / "catalog/mapui/ui/config").read_bytes(), b"warvk\n")
            self.assertEqual((fixture.output / "catalog/legacy/config").read_bytes(), b"warvk\n")

            jass = (fixture.output / "jass/WarVK.j").read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"warvk:(?!v1;)", jass))
            self.assertIn('Preloader("warvk:v1;pointLight.configure"', jass)
            self.assertIn('GetLocalizedHotkey("warvk:v1;pointLight.create"', jass)
            self.assertIn('S2R(GetLocalizedString("warvk:v1;system.metric"', jass)
            for function_name in re.findall(r"(?m)^function\s+([A-Za-z0-9_]+)", jass):
                self.assertRegex(function_name, r"^WarVK[A-Za-z0-9]+$")
            for command in synthetic_manifest()["commands"]:
                function_start = jass.index("function " + command["jassName"] + " ")
                function_end = jass.index("endfunction", function_start)
                self.assertIn(WARNING, jass[function_start:function_end])

            action = (fixture.output / "catalog/mapui/ui/warvk/action.txt").read_text(encoding="utf-8")
            call = (fixture.output / "catalog/mapui/ui/warvk/call.txt").read_text(encoding="utf-8")
            define = (fixture.output / "catalog/mapui/ui/warvk/define.txt").read_text(encoding="utf-8")
            self.assertNotIn("//", action + call + define)
            self.assertIn("TC_WARVK_SYSTEM=", define)
            self.assertIn("TC_WARVK_POINTLIGHT=", define)
            self.assertIn("category=TC_WARVK_POINTLIGHT", action)
            self.assertIn("type=boolean", action)
            self.assertIn("type=integer", action)
            self.assertIn("type=real", action)
            clear_section = action.split("[WarVKClearState]", 1)[1]
            self.assertIn("[[.args]]\ntype=nothing", clear_section.replace("\r\n", "\n"))
            version_section = call.split("[WarVKGetVersion]", 1)[1].split("\n[WarVK", 1)[0]
            self.assertIn("[[.args]]\ntype=nothing", version_section.replace("\r\n", "\n"))
            self.assertEqual(action.count(WARNING), 2)
            self.assertEqual(call.count(WARNING), 6)

            trigger_data = (
                fixture.output / "catalog/legacy/warvk/ui/TriggerData.txt").read_text(encoding="utf-8")
            self.assertIn("WarVKClearState=0,nothing", trigger_data)
            self.assertIn("WarVKGetVersion=0,0,string,nothing", trigger_data)
            self.assertIn("WarVKConfigurePointLight=0,boolean,integer,integer,real", trigger_data)
            self.assertIn("_WarVKCreatePointLight_Category=TC_WARVK_POINTLIGHT", trigger_data)

            trigger_strings = (
                fixture.output / "catalog/legacy/warvk/ui/TriggerStrings.txt").read_text(encoding="utf-8")
            for command in synthetic_manifest()["commands"]:
                matching = [
                    line for line in trigger_strings.splitlines()
                    if line.startswith(command["jassName"] + "=")
                ]
                self.assertEqual(len(matching), 2)
                self.assertTrue(matching[1].startswith(command["jassName"] + "=,"))
            configure_description = [
                line for line in trigger_strings.splitlines()
                if line.startswith("WarVKConfigurePointLight=,")
            ][0]
            self.assertLess(configure_description.index("~enabled"), configure_description.index("~quality"))
            self.assertLess(configure_description.index("~quality"), configure_description.index("~lightId"))
            self.assertLess(configure_description.index("~lightId"), configure_description.index("~radius"))

            vectors = json.loads(
                (fixture.output / "protocol/warvk-v1-vectors.json").read_text(encoding="utf-8"))
            self.assertEqual(len(vectors["commandVectors"]), 8)
            self.assertTrue(all(
                vector["payload"].startswith("warvk:v1;") and vector["generationAllowed"]
                for vector in vectors["commandVectors"]))
            legacy_case = next(
                value for value in vectors["payloadCases"]
                if value["name"] == "legacy-compatible-parse-only")
            self.assertTrue(legacy_case["payload"].startswith("warvk:"))
            self.assertFalse(legacy_case["payload"].startswith("warvk:v1;"))
            self.assertFalse(legacy_case["generationAllowed"])
            max_args_case = next(
                value for value in vectors["payloadCases"]
                if value["name"] == "maximum-argument-envelope")
            self.assertEqual(max_args_case["payloadArguments"], 16)
            self.assertEqual(max_args_case["validationScope"], "envelope")
            max_bytes_case = next(
                value for value in vectors["payloadCases"]
                if value["name"] == "maximum-byte-envelope")
            too_long_case = next(
                value for value in vectors["payloadCases"]
                if value["name"] == "payload-too-long")
            self.assertEqual(max_bytes_case["payloadBytes"], 512)
            self.assertEqual(too_long_case["payloadBytes"], 513)
            self.assertEqual(max_bytes_case["validationScope"], "envelope")
            self.assertTrue(any(
                value["name"] == "carrier-mismatch"
                for value in vectors["payloadCases"]))

            handoff = json.loads(
                (fixture.output / "warvk-handoff.json").read_text(encoding="utf-8"))
            self.assertFalse(handoff["catalog"]["targetCatalogBound"])
            self.assertIsNone(handoff["catalog"]["targetCatalogBaselineSha256"])
            self.assertIsNone(handoff["catalog"]["mergedCatalogSha256"])
            self.assertFalse(handoff["catalog"]["mergedCatalogValidated"])
            self.assertEqual(handoff["integration"]["status"], "blocked")
            self.assertTrue(all(value is False for value in handoff["claims"].values()))
            self.assertEqual(handoff["generator"]["version"], "1.0.0")
            self.assertEqual(
                handoff["manifest"]["bytes"], fixture.manifest_path.stat().st_size)
            self.assertTrue(handoff["constraints"]["localVisualOnly"])
            self.assertTrue(
                handoff["constraints"]["queryResultsForbiddenInMultiplayerSyncBranches"])
            self.assertTrue(
                handoff["constraints"]["candidateCompleteOnlyWhenHandoffPresentAndVerified"])
            self.assertTrue(handoff["constraints"]["unexpectedFilesMustBeRejected"])
            self.assertEqual(
                handoff["publication"]["mode"], "windows_handle_bound_commit_marker")
            self.assertTrue(handoff["publication"]["targetNoReplace"])
            self.assertTrue(handoff["publication"]["directoryIdentityLeased"])
            self.assertTrue(handoff["publication"]["payloadsReadBackBeforeCommit"])
            self.assertTrue(handoff["publication"]["fixedOutputSetVerifiedBeforeCommit"])
            self.assertEqual(handoff["publication"]["completionMarker"], "warvk-handoff.json")
            self.assertTrue(
                handoff["publication"]["completionMarkerReservedExclusiveAtBegin"])
            self.assertEqual(
                {artifact["path"] for artifact in handoff["artifacts"]},
                EXPECTED_PATHS - {"warvk-handoff.json"})
            for artifact in handoff["artifacts"]:
                actual = (fixture.output / artifact["path"]).read_bytes()
                self.assertEqual(artifact["bytes"], len(actual), artifact["path"])
                self.assertEqual(
                    artifact["sha256"], hashlib.sha256(actual).hexdigest(), artifact["path"])

    def test_catalog_sha_binds_only_the_external_baseline(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            fixture = GeneratorFixture(Path(temporary))
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            baseline = "ab" * 32
            result = fixture.run(digest, "--catalog-sha256", baseline.upper())
            self.assertEqual(result.returncode, 0, result.stderr)
            handoff = json.loads(
                (fixture.output / "warvk-handoff.json").read_text(encoding="utf-8"))
            self.assertTrue(handoff["catalog"]["targetCatalogBound"])
            self.assertEqual(handoff["catalog"]["targetCatalogBaselineSha256"], baseline)
            self.assertIsNone(handoff["catalog"]["mergedCatalogSha256"])
            self.assertFalse(handoff["catalog"]["mergedCatalogValidated"])
            self.assertEqual(handoff["integration"]["status"], "blocked")
            self.assertTrue(all(value is False for value in handoff["claims"].values()))

    def test_sha_mismatch_fails_without_creating_output(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            fixture = GeneratorFixture(Path(temporary))
            result = fixture.run("00" * 32)
            self.assertEqual(result.returncode, 2)
            self.assertIn("SHA-256 mismatch", result.stderr)
            self.assertFalse(fixture.output.exists())

    def test_existing_output_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            fixture = GeneratorFixture(Path(temporary))
            fixture.output.mkdir()
            sentinel = fixture.output / "user-file.txt"
            sentinel.write_text("preserve", encoding="utf-8")
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("overwrite is forbidden", result.stderr)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")
            self.assertEqual(list(fixture.output.iterdir()), [sentinel])

    @unittest.skipUnless(os.name == "nt", "Windows handle-bound publication contract")
    def test_publish_race_preserves_external_target(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            output = root / "generated"
            module = runpy.run_path(str(GENERATOR))
            publish = module["publish_outputs"]
            error_type = module["CodegenError"]
            sentinel = b"external-target"
            real_lease = module["_lease_output_parent"]
            manifest = synthetic_manifest()
            model = module["_parse_manifest"](manifest)
            artifacts = module["build_outputs"](
                model, "synthetic.json", "11" * 32, 1, None)

            def inject_target(api: object, parent: Path) -> object:
                output.mkdir()
                (output / "sentinel.bin").write_bytes(sentinel)
                return real_lease(api, parent)

            with mock.patch.dict(
                    publish.__globals__, {"_lease_output_parent": inject_target}):
                with self.assertRaisesRegex(error_type, "overwrite is forbidden"):
                    publish(output, artifacts)

            self.assertEqual((output / "sentinel.bin").read_bytes(), sentinel)
            self.assertEqual(list(output.iterdir()), [output / "sentinel.bin"])
            self.assertEqual(list(root.glob(".generated.warvk-stage-*")), [])

    @unittest.skipUnless(os.name == "nt", "Windows handle-bound publication contract")
    def test_publication_holds_parent_identity_and_payload_bytes_through_commit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            output = root / "generated"
            moved = root.parent / (root.name + "-moved")
            module = runpy.run_path(str(GENERATOR))
            publish = module["publish_outputs"]
            manifest = synthetic_manifest()
            model = module["_parse_manifest"](manifest)
            artifacts = module["build_outputs"](
                model, "synthetic.json", "22" * 32, 1, None)
            real_verify = module["_verify_handoff_inventory"]
            parent_rename_denied = False
            payload_write_denied = False
            marker_read_denied = False

            def probe(handoff: bytes, measured: dict[str, bytes]) -> None:
                nonlocal parent_rename_denied, payload_write_denied, marker_read_denied
                try:
                    os.rename(root, moved)
                except OSError:
                    parent_rename_denied = True
                try:
                    with (output / "jass/WarVK.j").open("wb"):
                        pass
                except OSError:
                    payload_write_denied = True
                try:
                    (output / "warvk-handoff.json").read_bytes()
                except OSError:
                    marker_read_denied = True
                real_verify(handoff, measured)

            with mock.patch.dict(
                    publish.__globals__, {"_verify_handoff_inventory": probe}):
                publish(output, artifacts)

            self.assertTrue(parent_rename_denied)
            self.assertTrue(payload_write_denied)
            self.assertTrue(marker_read_denied)
            self.assertFalse(moved.exists())
            self.assertTrue((output / "warvk-handoff.json").is_file())

    @unittest.skipUnless(os.name == "nt", "Windows handle-bound publication contract")
    def test_failed_commit_marker_validation_leaves_no_valid_handoff(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            output = Path(temporary) / "generated"
            module = runpy.run_path(str(GENERATOR))
            publish = module["publish_outputs"]
            error_type = module["CodegenError"]
            model = module["_parse_manifest"](synthetic_manifest())
            artifacts = module["build_outputs"](
                model, "synthetic.json", "33" * 32, 1, None)

            def reject(_handoff: bytes, _measured: dict[str, bytes]) -> None:
                raise error_type("synthetic inventory rejection")

            with mock.patch.dict(
                    publish.__globals__, {"_verify_handoff_inventory": reject}):
                with self.assertRaisesRegex(error_type, "incomplete candidate preserved"):
                    publish(output, artifacts)

            self.assertTrue(output.is_dir())
            self.assertEqual((output / "warvk-handoff.json").read_bytes(), b"")
            with self.assertRaises(json.JSONDecodeError):
                json.loads((output / "warvk-handoff.json").read_text(encoding="utf-8"))

    @unittest.skipUnless(os.name == "nt", "Windows handle-bound publication contract")
    def test_unexpected_output_entry_blocks_completion_marker(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            output = Path(temporary) / "generated"
            module = runpy.run_path(str(GENERATOR))
            publish = module["publish_outputs"]
            error_type = module["CodegenError"]
            real_verify = module["_verify_handoff_inventory"]
            model = module["_parse_manifest"](synthetic_manifest())
            artifacts = module["build_outputs"](
                model, "synthetic.json", "44" * 32, 1, None)

            def inject_extra(handoff: bytes, measured: dict[str, bytes]) -> None:
                (output / "unexpected.bin").write_bytes(b"external")
                real_verify(handoff, measured)

            with mock.patch.dict(
                    publish.__globals__, {"_verify_handoff_inventory": inject_extra}):
                with self.assertRaisesRegex(error_type, "unexpected entries"):
                    publish(output, artifacts)

            self.assertEqual((output / "warvk-handoff.json").read_bytes(), b"")
            self.assertEqual((output / "unexpected.bin").read_bytes(), b"external")

    def test_placeholder_order_and_unversioned_generation_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            manifest = synthetic_manifest()
            manifest["commands"][6]["descriptionZh"] = (
                WARNING + "错误次序 ${quality} ${enabled} ${lightId} ${radius}。")
            fixture = GeneratorFixture(root, manifest)
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("placeholders must occur exactly once", result.stderr)
            self.assertFalse(fixture.output.exists())

            unversioned = copy.deepcopy(synthetic_manifest())
            unversioned["protocol"]["legacyParseOnly"] = False
            digest = fixture.write(unversioned)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("must be parse-only", result.stderr)
            self.assertFalse(fixture.output.exists())

    def test_unknown_fields_and_nonstandard_json_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            manifest = synthetic_manifest()
            manifest["commands"][0]["opaqueHandle"] = 1
            fixture = GeneratorFixture(root, manifest)
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("unknown=opaqueHandle", result.stderr)
            self.assertFalse(fixture.output.exists())

            digest = fixture.write_raw(b'{"schema":{},"schema":{}}\n')
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("duplicate JSON key", result.stderr)
            self.assertFalse(fixture.output.exists())

            digest = fixture.write_raw(b'{"schema":NaN}\n')
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("non-standard JSON constant", result.stderr)
            self.assertFalse(fixture.output.exists())

    def test_deep_json_and_invalid_unicode_never_escape_as_tracebacks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            fixture = GeneratorFixture(root)
            deeply_nested = b'{"value":' + b"[" * 10000 + b"0" + b"]" * 10000 + b"}\n"
            digest = fixture.write_raw(deeply_nested)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("manifest JSON is invalid", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse(fixture.output.exists())

            overlong_integer = b'{"value":' + b"1" * 5000 + b"}\n"
            digest = fixture.write_raw(overlong_integer)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("overlong JSON integer", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse(fixture.output.exists())

            invalid_unicode = synthetic_manifest()
            invalid_unicode["commands"][0]["titleZh"] = "\ud800"
            payload = (json.dumps(invalid_unicode, ensure_ascii=True) + "\n").encode("ascii")
            digest = fixture.write_raw(payload)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("invalid Unicode", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse(fixture.output.exists())

            invalid_evidence = synthetic_manifest()
            invalid_evidence["provenance"]["allowedEvidence"] = ["\ud800"]
            payload = (json.dumps(invalid_evidence, ensure_ascii=True) + "\n").encode("ascii")
            digest = fixture.write_raw(payload)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("invalid Unicode", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse(fixture.output.exists())

    def test_fixed_abi_profile_and_contiguous_ids_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            cases: list[tuple[str, dict, str]] = []

            wrong_abi = synthetic_manifest()
            wrong_abi["protocol"]["carriers"]["hotkey"]["abi"] = "(S)S"
            cases.append(("abi", wrong_abi, "required stock ABI"))

            scan_allowed = synthetic_manifest()
            scan_allowed["gameDllProfile"]["registrationEvidence"]["scanAllowed"] = True
            cases.append(("scan", scan_allowed, "forbid replacement address scanning"))

            short_implementation = synthetic_manifest()
            short_implementation["gameDllProfile"]["carriers"][0]["implementationSize"] = "0x1"
            cases.append((
                "short-implementation", short_implementation,
                "smaller than its prologue evidence"))

            id_gap = synthetic_manifest()
            id_gap["commands"][3]["id"] = 9
            cases.append(("ids", id_gap, "contiguous from zero"))

            error_gap = synthetic_manifest()
            error_gap["errorCodes"][1]["value"] = 3
            cases.append(("error-ids", error_gap, "errorCodes values must be contiguous"))

            unknown_profile = synthetic_manifest()
            unknown_profile["gameDllProfile"]["registrationEvidence"]["guessAddress"] = True
            cases.append(("profile-fields", unknown_profile, "unknown=guessAddress"))

            primitive_argument = synthetic_manifest()
            primitive_argument["commands"][3]["args"][0]["name"] = "integer"
            primitive_argument["commands"][3]["descriptionZh"] = (
                WARNING + "以半径 ${integer} 创建本地点光。")
            cases.append(("primitive-argument", primitive_argument, "unsafe or duplicated"))

            debug_argument = synthetic_manifest()
            debug_argument["commands"][3]["args"][0]["name"] = "debug"
            debug_argument["commands"][3]["descriptionZh"] = (
                WARNING + "以半径 ${debug} 创建本地点光。")
            cases.append(("debug-argument", debug_argument, "unsafe or duplicated"))

            for name, manifest, message in cases:
                with self.subTest(name=name):
                    case_root = root / name
                    case_root.mkdir()
                    fixture = GeneratorFixture(case_root, manifest)
                    digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
                    result = fixture.run(digest)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn(message, result.stderr)
                    self.assertFalse(fixture.output.exists())

    def test_generation_is_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            first_root = root / "first"
            second_root = root / "second"
            first_root.mkdir()
            second_root.mkdir()
            first = GeneratorFixture(first_root)
            second = GeneratorFixture(second_root)
            first_digest = hashlib.sha256(first.manifest_path.read_bytes()).hexdigest()
            second_digest = hashlib.sha256(second.manifest_path.read_bytes()).hexdigest()
            self.assertEqual(first_digest, second_digest)
            self.assertEqual(first.run(first_digest).returncode, 0)
            self.assertEqual(second.run(second_digest).returncode, 0)
            for relative in EXPECTED_PATHS:
                self.assertEqual(
                    (first.output / relative).read_bytes(),
                    (second.output / relative).read_bytes(),
                    relative)

    def test_invalid_catalog_sha_and_manifest_size_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="warvk-codegen-test-") as temporary:
            root = Path(temporary)
            fixture = GeneratorFixture(root)
            digest = hashlib.sha256(fixture.manifest_path.read_bytes()).hexdigest()
            result = fixture.run(digest, "--catalog-sha256", "not-a-sha")
            self.assertEqual(result.returncode, 2)
            self.assertIn("64 hexadecimal", result.stderr)
            self.assertFalse(fixture.output.exists())

            oversized = b" " * (4 * 1024 * 1024 + 1)
            digest = fixture.write_raw(oversized)
            result = fixture.run(digest)
            self.assertEqual(result.returncode, 2)
            self.assertIn("input safety limit", result.stderr)
            self.assertFalse(fixture.output.exists())


if __name__ == "__main__":
    unittest.main()
