"""原创清单合同回归；不加载 DLL、不执行 Lua/Warcraft，不冒充运行时观察。"""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).with_name("lua-runtime-api-manifest.py")
SPEC = importlib.util.spec_from_file_location("runtime_manifest", SCRIPT)
API = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(API)
DELETE = object()


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="audastra-runtime-inventory-test-")
        self.root = Path(self.temporary.name)
        self.nonce = "abcdef0123456789abcdef0123456789"
        def file(name, value):
            path = self.root / name
            path.write_text(value, encoding="utf-8")
            return path
        self.common = file("common.j", "native ActualApi takes integer value returns boolean\n")
        self.game = file("Game.dll", "original readonly fake Game source")
        self.executable = file("war3.exe", "original owned fake Game executable; never executed")
        self.launcher = file("original-launcher.exe", "original owned fake bridge; never executed")
        self.map_relative = "Maps\\OriginalOwned\\probe.w3x"
        self.base_launcher_args = [str(self.launcher), "--game", str(self.executable), "--map", self.map_relative]
        self.map = file("probe.w3x", "original owned fake map")
        self.lua = file("probe.lua", "original owned fake metadata code")
        self.modules = []
        for name in sorted(API.EXPECTED_MODULES):
            path = file(name, "original synthetic source " + name)
            digest = API.digest(path)
            self.modules.append({"name": name, "path": str(path), "expectedSha256": digest, "sha256": digest,
                                 "loaded": True, "initializeCalled": name in ("yd_jass_api.dll", "yd_lua_engine.dll")})
        self.preparation = {"schema": "audastra-lua-api-probe-preparation-v1", "nonce": self.nonce,
                            "authorScriptsIncluded": False, "mapPath": str(self.map), "mapSha256": API.digest(self.map),
                            "luaPath": str(self.lua), "luaSha256": API.digest(self.lua), "commonSha256": API.digest(self.common),
                            "runtimeEngineSha256": next(row["sha256"] for row in self.modules if row["name"] == "yd_lua_engine.dll"),
                            "commonSignatures": [{"name": "ActualApi", "parameterCount": 1, "parameters": ["integer"], "returns": "boolean"}]}
        self.observed = {"schema": "audastra-lua-api-observation-v1", "nonce": self.nonce, "completed": True,
                         "gameApiFunctionsInvokedByProbe": 0, "runtimePreloadOnly": True, "modules": [
                             {"name": "jass.common", "available": True, "enumerationComplete": False,
                              "fields": [{"name": "ActualApi", "kind": "function", "origin": "common-declaration-runtime-lookup"}]},
                             {"name": "jass.lazy", "available": True, "enumerationComplete": False, "fields": []},
                             {"name": "jass.candidate", "available": False, "enumerationComplete": False, "reason": "failed readonly lookup", "fields": []}]}
        self.launch = {"schema": "audastra-warcraft-launch-v1", "bridgeExitCode": 0, "hookInstalled": True,
                       "processCreated": True, "gameProcessId": 333,
                       "gameplayVerified": False, "probeGraphicsBackendRequested": "game-default",
                       "gameArgv": [str(self.executable), "-window", "-loadfile", self.map_relative],
                       "gameObserved": True, "runtimeInitialized": True, "errorStage": "lua-api-probe-captured",
                       "gameDllPath": str(self.game), "gameDllSha256": API.digest(self.game), "expectedGameDllSha256": API.digest(self.game),
                       "modules": self.modules, "luaApiProbe": {"enabled": True, "nonce": self.nonce, "captured": True,
                       "writeGuardInstalled": True, "observerInstalled": True, "writeDenied": False,
                       "nonfatalBlockedPaths": [], "blockedWriteCount": 0, "fatalWriteCount": 0,
                       "blockedWriteOverflow": False, "blockedWrites": [],
                       "mapSha256": self.preparation["mapSha256"], "scriptSha256": self.preparation["luaSha256"]}}
        self.isolation = {"schema": "audastra-isolated-lua-api-probe-v1", "succeeded": True, "nonce": self.nonce,
                          "bridgeExitCode": 0, "inputDesktopBefore": "Default", "inputDesktopAfter": "Default",
                          "desktop": {"name": "AudastraRuntimeProbe-" + self.nonce},
                          "desktopWitnessMode": "none", "bridgePid": 111, "bridgeThreadId": 222,
                          "argv": self.base_launcher_args.copy(), "gameplayVerified": False,
                          "probeGraphicsBackendRequested": "game-default",
                          "bridgeThreadDesktopQuery": {"threadId": 222, "returnedNull": False, "lastError": 0,
                                                       "name": "AudastraRuntimeProbe-" + self.nonce},
                          "bridgeDesktop": {"proofKind": "thread-desktop", "name": "AudastraRuntimeProbe-" + self.nonce,
                                            "threadId": 222, "handle": 333, "borrowedHandle": True},
                          "jobBeforeClose": {"enumerationComplete": True, "unresolvedAssociatedPids": []},
                          "desktopClosed": True, "jobClosed": True, "threadClosed": True, "bridgeHandleClosed": True,
                          "luaEnvironmentOverridesAbsent": True, "nonfatalBlockedPaths": [], "errors": []}

    def tearDown(self):
        self.assertEqual(self.root.resolve().parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith("audastra-runtime-inventory-test-"))
        self.temporary.cleanup()

    def build(self):
        self.launch["luaApiProbe"]["payloadJson"] = json.dumps(self.observed)
        result = API.build_manifest(self.preparation, self.launch, self.isolation, self.common)
        self.capture_number = getattr(self, "capture_number", 0) + 1
        prefix = str(self.capture_number) + "-"
        preparation, launch, isolation = (self.root / (prefix + name + ".json") for name in ("preparation", "launch", "isolation"))
        preparation.write_text(json.dumps(self.preparation), encoding="utf-8")
        launch.write_text(json.dumps(self.launch), encoding="utf-8")
        self.isolation["preparationSha256"] = API.digest(preparation)
        self.isolation["launcherReceiptSha256"] = API.digest(launch)
        isolation.write_text(json.dumps(self.isolation), encoding="utf-8")
        result["evidenceSources"] = [{"role": role, "path": str(path), "sha256": API.digest(path)}
                                    for role, path in zip(("preparation", "launcher-receipt", "isolation-receipt"), (preparation, launch, isolation))]
        return result

    def seed_hidden_desktop_witness(self):
        # 原创 JSON 夹具仅验证接纳合同，不创建窗口或冒充实际 Desktop 观察。
        name = "AudastraRuntimeProbe-" + self.nonce
        class_name = "AudastraProbeDesktopWitness-" + self.nonce
        self.isolation.update(desktopWitnessMode="hidden-window", argv=self.base_launcher_args + ["--probe-desktop-witness", "hidden-window"],
            bridgeThreadDesktopQuery={"threadId": 222, "returnedNull": True, "lastError": 5},
            bridgeDesktop={"proofKind": "hidden-window", "name": name, "threadId": 222,
                "hwnd": 444, "className": class_name, "ownerPid": 111, "ownerTid": 222,
                "style": 0x80000000, "exStyle": 0x08000080, "visible": False,
                "originalProcessAlive": True, "originalThreadAlive": True, "originalProcessInOwnedJob": True,
                "observationRepeated": True, "enumerationComplete": True, "windowCount": 1,
                "originalProcessCreationFileTime": 555})
        self.launch["desktopWitness"] = {"requested": True, "mode": "hidden-window", "proofKind": "hidden-window",
            "nonce": self.nonce, "className": class_name, "hwnd": 444, "bridgePid": 111, "threadId": 222,
            "style": 0x80000000, "exStyle": 0x08000080, "visible": False,
            "created": True, "destroyed": True, "unregistered": True, "errorWin32": 0}

    def assert_receipt_rejections(self, cases):
        # 每个负例从独立快照开始，不能由前一个错误残留制造假通过。
        baseline = copy.deepcopy((self.launch, self.isolation))
        for label, changes in cases:
            with self.subTest(case=label):
                self.launch, self.isolation = copy.deepcopy(baseline)
                documents = {"launch": self.launch, "isolation": self.isolation}
                for path, value in changes:
                    parent = documents[path[0]]
                    for key in path[1:-1]:
                        parent = parent[key]
                    if value is DELETE:
                        del parent[path[-1]]
                    else:
                        parent[path[-1]] = value
                with self.assertRaises(ValueError):
                    self.build()
        self.launch, self.isolation = baseline

    def test_direct_thread_desktop_requires_original_identity_and_explicit_query(self):
        manifest = self.build()
        self.assertEqual(manifest["desktopProof"]["proofKind"], "thread-desktop")
        self.assertNotIn("desktopWitness", self.launch)
        cases = [
            ("default-unknown-kind", [(('isolation', 'bridgeDesktop', 'proofKind'), 'invented-proof')]),
            ("default-hidden-kind", [(('isolation', 'bridgeDesktop', 'proofKind'), 'hidden-window')]),
            ("default-missing-kind", [(('isolation', 'bridgeDesktop', 'proofKind'), DELETE)]),
            ("default-unsupported-mode", [(('isolation', 'desktopWitnessMode'), 'invented-mode')]),
            ("default-hidden-mode", [(('isolation', 'desktopWitnessMode'), 'hidden-window')]),
            ("default-missing-mode", [(('isolation', 'desktopWitnessMode'), DELETE)]),
            ("default-absent-bridge", [(('isolation', 'bridgeDesktop'), DELETE)]),
            ("default-wrong-desktop", [(('isolation', 'bridgeDesktop', 'name'), 'Default')]),
            ("default-wrong-thread", [(('isolation', 'bridgeDesktop', 'threadId'), 223)]),
            ("default-zero-pid", [(('isolation', 'bridgePid'), 0)]),
            ("default-zero-tid", [(('isolation', 'bridgeThreadId'), 0)]),
            ("default-bool-pid", [(('isolation', 'bridgePid'), True)]),
            ("default-bool-tid-pair", [(('isolation', 'bridgeThreadId'), 1),
                                       (('isolation', 'bridgeDesktop', 'threadId'), True),
                                       (('isolation', 'bridgeThreadDesktopQuery', 'threadId'), True)]),
            ("default-unborrowed-handle", [(('isolation', 'bridgeDesktop', 'borrowedHandle'), False)]),
            ("default-zero-handle", [(('isolation', 'bridgeDesktop', 'handle'), 0)]),
            ("default-bool-handle", [(('isolation', 'bridgeDesktop', 'handle'), True)]),
            ("default-missing-query", [(('isolation', 'bridgeThreadDesktopQuery'), DELETE)]),
            ("default-query-wrong-tid", [(('isolation', 'bridgeThreadDesktopQuery', 'threadId'), 223)]),
            ("default-query-wrong-name", [(('isolation', 'bridgeThreadDesktopQuery', 'name'), 'Default')]),
            ("default-null-query", [(('isolation', 'bridgeThreadDesktopQuery', 'returnedNull'), True),
                                     (('isolation', 'bridgeThreadDesktopQuery', 'name'), DELETE)]),
            ("default-negative-query-error", [(('isolation', 'bridgeThreadDesktopQuery', 'lastError'), -1)]),
            ("default-query-error-overflow", [(('isolation', 'bridgeThreadDesktopQuery', 'lastError'), 0x100000000)]),
            ("default-unrequested-argv", [(('isolation', 'argv'), self.base_launcher_args + ['--probe-desktop-witness', 'hidden-window'])]),
            ("default-unrequested-raw", [(('launch', 'desktopWitness'), {'requested': False})]),
        ]
        self.assert_receipt_rejections(cases)

    def test_hidden_window_witness_keeps_null_diagnostic_and_partial_scope(self):
        self.seed_rejected_nonfatal_write()
        self.seed_hidden_desktop_witness()
        manifest = self.build()
        self.assertEqual(manifest["desktopProof"]["proofKind"], "hidden-window")
        self.assertTrue(self.isolation["bridgeThreadDesktopQuery"]["returnedNull"])
        self.assertEqual(self.isolation["bridgeThreadDesktopQuery"]["lastError"], 5)
        self.assertTrue(manifest["writeContainment"]["writeDenied"])
        self.assertFalse(manifest["fullyEnumerated"])
        self.assertFalse(manifest["semanticChecksComplete"])
        self.assertFalse(manifest["savePermissionGranted"])
        self.assertTrue(manifest["unknownDoesNotMeanMissing"])
        self.assertTrue(all(row["absenceProven"] is False for row in manifest["modules"]))
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")
        # 正确的非 NULL 原查询仍可作为诊断保留；不能让见证遮盖错误 Desktop。
        self.isolation["bridgeThreadDesktopQuery"].update(returnedNull=False, lastError=0,
            name="AudastraRuntimeProbe-" + self.nonce)
        self.assertEqual(API.verify_manifest(self.build())["status"], "verified-partial")
        # Windows 可补非显示 style 位；必须与同次 raw 精确一致。
        self.isolation["bridgeDesktop"]["style"] = self.launch["desktopWitness"]["style"] = 0x84000000
        self.assertEqual(API.verify_manifest(self.build())["status"], "verified-partial")

    def test_hidden_window_witness_rejects_invalid_binding_visibility_and_cleanup(self):
        self.seed_hidden_desktop_witness()
        cases = [
            ("hidden-unknown-kind", [(('isolation', 'bridgeDesktop', 'proofKind'), 'invented-proof')]),
            ("hidden-thread-kind", [(('isolation', 'bridgeDesktop', 'proofKind'), 'thread-desktop')]),
            ("hidden-none-mode", [(('isolation', 'desktopWitnessMode'), 'none')]),
            ("hidden-raw-absent", [(('launch', 'desktopWitness'), DELETE)]),
            ("hidden-not-requested", [(('launch', 'desktopWitness', 'requested'), False)]),
            ("hidden-raw-wrong-mode", [(('launch', 'desktopWitness', 'mode'), 'none')]),
            ("hidden-raw-wrong-kind", [(('launch', 'desktopWitness', 'proofKind'), 'thread-desktop')]),
            ("hidden-raw-wrong-nonce", [(('launch', 'desktopWitness', 'nonce'), '0' * 32)]),
            ("hidden-raw-wrong-class", [(('launch', 'desktopWitness', 'className'), 'wrong-class')]),
            ("hidden-raw-wrong-hwnd", [(('launch', 'desktopWitness', 'hwnd'), 445)]),
            ("hidden-zero-hwnd", [(('launch', 'desktopWitness', 'hwnd'), 0), (('isolation', 'bridgeDesktop', 'hwnd'), 0)]),
            ("hidden-bool-hwnd", [(('launch', 'desktopWitness', 'hwnd'), True), (('isolation', 'bridgeDesktop', 'hwnd'), True)]),
            ("hidden-raw-wrong-pid", [(('launch', 'desktopWitness', 'bridgePid'), 112)]),
            ("hidden-raw-wrong-tid", [(('launch', 'desktopWitness', 'threadId'), 223)]),
            ("hidden-not-created", [(('launch', 'desktopWitness', 'created'), False)]),
            ("hidden-not-destroyed", [(('launch', 'desktopWitness', 'destroyed'), False)]),
            ("hidden-not-unregistered", [(('launch', 'desktopWitness', 'unregistered'), False)]),
            ("hidden-cleanup-error", [(('launch', 'desktopWitness', 'errorWin32'), 5)]),
            ("hidden-cleanup-error-missing", [(('launch', 'desktopWitness', 'errorWin32'), DELETE)]),
            ("hidden-owner-wrong-pid", [(('isolation', 'bridgeDesktop', 'ownerPid'), 112)]),
            ("hidden-owner-wrong-tid", [(('isolation', 'bridgeDesktop', 'ownerTid'), 223)]),
            ("hidden-wrong-thread", [(('isolation', 'bridgeDesktop', 'threadId'), 223)]),
            ("hidden-bool-tid-pair", [(('isolation', 'bridgeThreadId'), 1),
                                      (('isolation', 'bridgeDesktop', 'threadId'), True),
                                      (('isolation', 'bridgeDesktop', 'ownerTid'), 1),
                                      (('isolation', 'bridgeThreadDesktopQuery', 'threadId'), True),
                                      (('launch', 'desktopWitness', 'threadId'), True)]),
            ("hidden-wrong-class", [(('isolation', 'bridgeDesktop', 'className'), 'wrong-class')]),
            ("hidden-wrong-desktop", [(('isolation', 'bridgeDesktop', 'name'), 'Default')]),
            ("hidden-visible", [(('launch', 'desktopWitness', 'visible'), True), (('isolation', 'bridgeDesktop', 'visible'), True)]),
            ("hidden-raw-visible", [(('launch', 'desktopWitness', 'visible'), True)]),
            ("hidden-dead-process", [(('isolation', 'bridgeDesktop', 'originalProcessAlive'), False)]),
            ("hidden-dead-thread", [(('isolation', 'bridgeDesktop', 'originalThreadAlive'), False)]),
            ("hidden-wrong-job", [(('isolation', 'bridgeDesktop', 'originalProcessInOwnedJob'), False)]),
            ("hidden-no-repeat", [(('isolation', 'bridgeDesktop', 'observationRepeated'), False)]),
            ("hidden-enum-failed", [(('isolation', 'bridgeDesktop', 'enumerationComplete'), False)]),
            ("hidden-window-absent", [(('isolation', 'bridgeDesktop', 'windowCount'), 0)]),
            ("hidden-enum-overflow", [(('isolation', 'bridgeDesktop', 'windowCount'), 65)]),
            ("hidden-zero-creation-time", [(('isolation', 'bridgeDesktop', 'originalProcessCreationFileTime'), 0)]),
            ("hidden-missing-hwnd", [(('isolation', 'bridgeDesktop', 'hwnd'), DELETE)]),
            ("hidden-fabricated-query-handle", [(('isolation', 'bridgeDesktop', 'handle'), 333)]),
            ("hidden-fabricated-borrowed-handle", [(('isolation', 'bridgeDesktop', 'borrowedHandle'), False)]),
            ("hidden-missing-original-pid", [(('isolation', 'bridgePid'), DELETE)]),
            ("hidden-missing-query", [(('isolation', 'bridgeThreadDesktopQuery'), DELETE)]),
            ("hidden-query-wrong-tid", [(('isolation', 'bridgeThreadDesktopQuery', 'threadId'), 223)]),
            ("hidden-query-wrong-desktop", [(('isolation', 'bridgeThreadDesktopQuery', 'returnedNull'), False),
                                           (('isolation', 'bridgeThreadDesktopQuery', 'name'), 'Default')]),
            ("hidden-null-query-fake-name", [(('isolation', 'bridgeThreadDesktopQuery', 'name'), 'AudastraRuntimeProbe-' + self.nonce)]),
            ("hidden-negative-query-error", [(('isolation', 'bridgeThreadDesktopQuery', 'lastError'), -1)]),
            ("hidden-query-error-overflow", [(('isolation', 'bridgeThreadDesktopQuery', 'lastError'), 0x100000000)]),
            ("hidden-query-not-boolean", [(('isolation', 'bridgeThreadDesktopQuery', 'returnedNull'), 'true')]),
            ("hidden-argv-absent", [(('isolation', 'argv'), self.base_launcher_args.copy())]),
            ("hidden-argv-duplicate", [(('isolation', 'argv'), self.base_launcher_args + ['--probe-desktop-witness', 'hidden-window'] * 2)]),
            ("hidden-argv-wrong-mode", [(('isolation', 'argv'), self.base_launcher_args + ['--probe-desktop-witness', 'invented-mode'])]),
            ("hidden-visible-style", [(('launch', 'desktopWitness', 'style'), 0x90000000), (('isolation', 'bridgeDesktop', 'style'), 0x90000000)]),
            ("hidden-child-style", [(('launch', 'desktopWitness', 'style'), 0xC0000000), (('isolation', 'bridgeDesktop', 'style'), 0xC0000000)]),
            ("hidden-no-popup-style", [(('launch', 'desktopWitness', 'style'), 0x04000000), (('isolation', 'bridgeDesktop', 'style'), 0x04000000)]),
            ("hidden-style-pair-mismatch", [(('launch', 'desktopWitness', 'style'), 0x84000000)]),
            ("hidden-style-outside-dword", [(('launch', 'desktopWitness', 'style'), 0x180000000), (('isolation', 'bridgeDesktop', 'style'), 0x180000000)]),
            ("hidden-no-noactivate", [(('launch', 'desktopWitness', 'exStyle'), 0x80), (('isolation', 'bridgeDesktop', 'exStyle'), 0x80)]),
            ("hidden-no-toolwindow", [(('launch', 'desktopWitness', 'exStyle'), 0x08000000), (('isolation', 'bridgeDesktop', 'exStyle'), 0x08000000)]),
            ("hidden-topmost", [(('launch', 'desktopWitness', 'exStyle'), 0x08000088), (('isolation', 'bridgeDesktop', 'exStyle'), 0x08000088)]),
            ("hidden-exstyle-pair-mismatch", [(('launch', 'desktopWitness', 'exStyle'), 0x08000088)]),
        ]
        self.assert_receipt_rejections(cases)

    def test_frozen_desktop_proof_and_permission_boundaries_cannot_be_rewritten(self):
        self.seed_hidden_desktop_witness()
        manifest = self.build()
        for change in ("proof-kind", "proof-claim", "proof-delete", "proof-empty", "semantic-checks", "save-permission", "absence"):
            with self.subTest(change=change):
                changed = copy.deepcopy(manifest)
                if change == "proof-kind": changed["desktopProof"]["proofKind"] = "thread-desktop"
                elif change == "proof-claim": changed["desktopProof"]["allGameThreadsVerified"] = True
                elif change == "proof-delete": del changed["desktopProof"]
                elif change == "proof-empty": changed["desktopProof"] = {}
                elif change == "semantic-checks": changed["semanticChecksComplete"] = True
                elif change == "save-permission": changed["savePermissionGranted"] = True
                else: changed["modules"][0]["absenceProven"] = True
                with self.assertRaises(ValueError): API.verify_manifest(changed)

    def seed_opengl_request(self):
        self.isolation["argv"] += ["--probe-renderer", "opengl"]
        self.launch["probeGraphicsBackendRequested"] = self.isolation["probeGraphicsBackendRequested"] = "opengl"
        self.launch["gameArgv"] = [str(self.executable), "-window", "-opengl", "-loadfile", self.map_relative]

    def test_graphics_requests_bind_game_argv_without_verifying_backend_or_gameplay(self):
        # 原创 argv/回执夹具不启动 fake executable，也不把请求当实际后端。
        expected = {"requested": "game-default", "actualBackend": "unknown", "actualBackendVerified": False,
                    "gameArgv": [str(self.executable), "-window", "-loadfile", self.map_relative],
                    "argvSameRunBound": True, "backendSpecificAbsenceNotProven": True}
        manifest = self.build()
        self.assertEqual(manifest["graphicsBackend"], expected)
        self.assertNotIn("--probe-renderer", self.isolation["argv"])
        self.assertNotIn("-opengl", manifest["graphicsBackend"]["gameArgv"])
        self.assertIs(manifest["gameplayVerified"], False)
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")
        self.seed_hidden_desktop_witness()
        self.seed_opengl_request()
        expected.update(requested="opengl", gameArgv=self.launch["gameArgv"].copy())
        manifest = self.build()
        self.assertEqual(manifest["graphicsBackend"], expected)
        self.assertIs(manifest["gameplayVerified"], False)
        self.assertIs(manifest["fullyEnumerated"], False)
        self.assertIs(manifest["semanticChecksComplete"], False)
        self.assertIs(manifest["savePermissionGranted"], False)
        self.assertIs(manifest["unknownDoesNotMeanMissing"], True)
        self.assertTrue(all(row["absenceProven"] is False for row in manifest["modules"]))
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")

    def test_graphics_request_receipts_reject_unpaired_or_invalid_game_tokens(self):
        self.seed_hidden_desktop_witness()
        self.seed_opengl_request()
        physical = self.isolation["argv"].copy()
        game_tokens = self.launch["gameArgv"].copy()
        witness = ["--probe-desktop-witness", "hidden-window"]
        renderer = ["--probe-renderer", "opengl"]
        def launch_args(executable, relative):
            return [str(self.launcher), "--game", executable, "--map", relative] + witness + renderer
        cases = [
            ("graphics-game-process-created-missing", [(('launch', 'processCreated'), DELETE)]),
            ("graphics-game-process-not-created", [(('launch', 'processCreated'), False)]),
            ("graphics-game-process-created-zero", [(('launch', 'processCreated'), 0)]),
            ("graphics-game-process-created-numeric-true", [(('launch', 'processCreated'), 1)]),
            ("graphics-game-pid-missing", [(('launch', 'gameProcessId'), DELETE)]),
            ("graphics-game-pid-zero", [(('launch', 'gameProcessId'), 0)]),
            ("graphics-game-pid-negative", [(('launch', 'gameProcessId'), -1)]),
            ("graphics-game-pid-bool-true", [(('launch', 'gameProcessId'), True)]),
            ("graphics-game-pid-bool-false", [(('launch', 'gameProcessId'), False)]),
            ("graphics-game-pid-float", [(('launch', 'gameProcessId'), 333.0)]),
            ("graphics-raw-unknown-request", [(('launch', 'probeGraphicsBackendRequested'), 'vulkan')]),
            ("graphics-isolation-unknown-request", [(('isolation', 'probeGraphicsBackendRequested'), 'd3d9')]),
            ("graphics-raw-request-missing", [(('launch', 'probeGraphicsBackendRequested'), DELETE)]),
            ("graphics-isolation-request-missing", [(('isolation', 'probeGraphicsBackendRequested'), DELETE)]),
            ("graphics-raw-request-bool", [(('launch', 'probeGraphicsBackendRequested'), False)]),
            ("graphics-isolation-request-bool", [(('isolation', 'probeGraphicsBackendRequested'), False)]),
            ("graphics-raw-request-mismatch", [(('launch', 'probeGraphicsBackendRequested'), 'game-default')]),
            ("graphics-isolation-request-mismatch", [(('isolation', 'probeGraphicsBackendRequested'), 'game-default')]),
            ("graphics-raw-gameplay-claimed", [(('launch', 'gameplayVerified'), True)]),
            ("graphics-isolation-gameplay-claimed", [(('isolation', 'gameplayVerified'), True)]),
            ("graphics-raw-gameplay-numeric", [(('launch', 'gameplayVerified'), 0)]),
            ("graphics-isolation-gameplay-numeric", [(('isolation', 'gameplayVerified'), 0)]),
            ("graphics-raw-gameplay-missing", [(('launch', 'gameplayVerified'), DELETE)]),
            ("graphics-isolation-gameplay-missing", [(('isolation', 'gameplayVerified'), DELETE)]),
            ("graphics-physical-argv-missing", [(('isolation', 'argv'), DELETE)]),
            ("graphics-physical-argv-empty", [(('isolation', 'argv'), [])]),
            ("graphics-physical-launcher-missing", [(('isolation', 'argv'), physical[1:])]),
            ("graphics-physical-dangling-key", [(('isolation', 'argv'), physical + ['--unused'])]),
            ("graphics-physical-value-not-string", [(('isolation', 'argv'), physical[:-1] + [42])]),
            ("graphics-physical-key-not-option", [(('isolation', 'argv'), [physical[0], 'game', *physical[2:]])]),
            ("graphics-game-key-absent", [(('isolation', 'argv'), [str(self.launcher), '--map', self.map_relative] + witness + renderer)]),
            ("graphics-map-key-absent", [(('isolation', 'argv'), [str(self.launcher), '--game', str(self.executable)] + witness + renderer)]),
            ("graphics-game-key-duplicate", [(('isolation', 'argv'), physical + ['--game', str(self.executable)])]),
            ("graphics-map-key-duplicate", [(('isolation', 'argv'), physical + ['--map', self.map_relative])]),
            ("graphics-game-value-empty", [(('isolation', 'argv'), launch_args('', self.map_relative))]),
            ("graphics-game-value-wrong-exe", [(('isolation', 'argv'), launch_args(str(self.game), self.map_relative)),
                                              (('launch', 'gameArgv'), [str(self.game), *game_tokens[1:]])]),
            ("graphics-game-value-relative", [(('isolation', 'argv'), launch_args('war3.exe', self.map_relative)),
                                             (('launch', 'gameArgv'), ['war3.exe', *game_tokens[1:]])]),
            ("graphics-map-value-empty", [(('isolation', 'argv'), launch_args(str(self.executable), ''))]),
            ("graphics-map-value-absolute", [(('isolation', 'argv'), launch_args(str(self.executable), str(self.map))),
                                             (('launch', 'gameArgv'), game_tokens[:-1] + [str(self.map)])]),
            ("graphics-map-value-traversal", [(('isolation', 'argv'), launch_args(str(self.executable), 'Maps\\..\\probe.w3x')),
                                              (('launch', 'gameArgv'), game_tokens[:-1] + ['Maps\\..\\probe.w3x'])]),
            ("graphics-map-value-wrong-extension", [(('isolation', 'argv'), launch_args(str(self.executable), 'Maps\\OriginalOwned\\probe.txt')),
                                                    (('launch', 'gameArgv'), game_tokens[:-1] + ['Maps\\OriginalOwned\\probe.txt'])]),
            ("graphics-renderer-value-missing", [(('isolation', 'argv'), self.base_launcher_args + witness + ['--probe-renderer'])]),
            ("graphics-renderer-duplicate", [(('isolation', 'argv'), physical + renderer)]),
            ("graphics-renderer-invalid", [(('isolation', 'argv'), physical[:-1] + ['d3d9'])]),
            ("graphics-renderer-request-without-flag", [(('isolation', 'argv'), self.base_launcher_args + witness)]),
            ("graphics-game-argv-missing", [(('launch', 'gameArgv'), DELETE)]),
            ("graphics-game-argv-not-list", [(('launch', 'gameArgv'), ' '.join(game_tokens))]),
            ("graphics-game-argv-wrong-exe", [(('launch', 'gameArgv'), [str(self.game), *game_tokens[1:]])]),
            ("graphics-game-argv-wrong-map", [(('launch', 'gameArgv'), game_tokens[:-1] + ['Maps\\OriginalOwned\\other.w3x'])]),
            ("graphics-game-argv-opengl-missing", [(('launch', 'gameArgv'), [str(self.executable), '-window', '-loadfile', self.map_relative])]),
            ("graphics-game-argv-opengl-duplicate", [(('launch', 'gameArgv'), game_tokens[:3] + ['-opengl'] + game_tokens[3:])]),
            ("graphics-game-argv-extra-token", [(('launch', 'gameArgv'), game_tokens + ['-swtnl'])]),
            ("graphics-game-argv-wrong-order", [(('launch', 'gameArgv'), [str(self.executable), '-opengl', '-window', '-loadfile', self.map_relative])]),
            ("graphics-game-argv-value-not-string", [(('launch', 'gameArgv'), game_tokens[:-1] + [False])]),
        ]
        self.assert_receipt_rejections(cases)
        self.launch["probeGraphicsBackendRequested"] = self.isolation["probeGraphicsBackendRequested"] = "game-default"
        self.isolation["argv"] = self.base_launcher_args + witness
        self.launch["gameArgv"] = [str(self.executable), "-window", "-loadfile", self.map_relative]
        self.assert_receipt_rejections([
            ("graphics-default-silent-opengl", [(('launch', 'gameArgv'), game_tokens)]),
            ("graphics-default-silent-renderer-flag", [(('isolation', 'argv'), physical)]),
        ])

    def test_frozen_graphics_requests_and_resigned_receipts_cannot_change_claims(self):
        self.seed_hidden_desktop_witness()
        self.seed_opengl_request()
        manifest = self.build()
        replacements = [
            ("requested", ("graphicsBackend", "requested"), "game-default"),
            ("actual-backend", ("graphicsBackend", "actualBackend"), "opengl"),
            ("actual-backend-verified", ("graphicsBackend", "actualBackendVerified"), True),
            ("actual-backend-numeric-false", ("graphicsBackend", "actualBackendVerified"), 0),
            ("game-argv", ("graphicsBackend", "gameArgv"), self.launch["gameArgv"] + ["-swtnl"]),
            ("same-run", ("graphicsBackend", "argvSameRunBound"), False),
            ("same-run-numeric-true", ("graphicsBackend", "argvSameRunBound"), 1),
            ("backend-absence", ("graphicsBackend", "backendSpecificAbsenceNotProven"), False),
            ("backend-absence-numeric-true", ("graphicsBackend", "backendSpecificAbsenceNotProven"), 1),
            ("module-count-float", ("moduleCount",), float(manifest["moduleCount"])),
            ("gameplay", ("gameplayVerified",), True),
            ("semantic", ("semanticChecksComplete",), True),
            ("save", ("savePermissionGranted",), True),
            ("unknown", ("unknownDoesNotMeanMissing",), False),
            ("graphics-delete", ("graphicsBackend",), DELETE),
            ("graphics-empty", ("graphicsBackend",), {}),
        ]
        for label, keys, value in replacements:
            with self.subTest(canonical=label):
                changed = copy.deepcopy(manifest)
                parent = changed
                for key in keys[:-1]: parent = parent[key]
                if value is DELETE: del parent[keys[-1]]
                else: parent[keys[-1]] = value
                with self.assertRaises(ValueError): API.verify_manifest(changed)
        # 重算本测试自有 receipt SHA 后仍须重建 pair/canonical，不能只靠旧 SHA 拒绝。
        for label in ("launcher-request", "launcher-game-argv", "isolation-request", "isolation-physical-argv"):
            with self.subTest(receipt=label):
                changed = self.build()
                roles = {row["role"]: Path(row["path"]) for row in changed["evidenceSources"]}
                raw = API.read_json(roles["launcher-receipt"])
                isolated = API.read_json(roles["isolation-receipt"])
                if label == "launcher-request": raw["probeGraphicsBackendRequested"] = "game-default"
                elif label == "launcher-game-argv": raw["gameArgv"] = [str(self.executable), "-window", "-loadfile", self.map_relative]
                elif label == "isolation-request": isolated["probeGraphicsBackendRequested"] = "game-default"
                else: isolated["argv"] = self.base_launcher_args + ["--probe-desktop-witness", "hidden-window"]
                roles["launcher-receipt"].write_text(json.dumps(raw), encoding="utf-8")
                isolated["launcherReceiptSha256"] = API.digest(roles["launcher-receipt"])
                roles["isolation-receipt"].write_text(json.dumps(isolated), encoding="utf-8")
                for row in changed["evidenceSources"]: row["sha256"] = API.digest(roles[row["role"]])
                with self.assertRaises(ValueError): API.verify_manifest(changed)

    def test_positive_fields_have_actual_common_signature_and_unknown_is_preserved(self):
        manifest = self.build()
        self.assertFalse(manifest["fullyEnumerated"])
        self.assertTrue(manifest["unknownDoesNotMeanMissing"])
        self.assertEqual(manifest["modules"][0]["fields"][0]["signature"]["parameterCount"], 1)
        self.assertIsNone(manifest["modules"][1].get("signature"))
        self.assertFalse(manifest["modules"][2]["absenceProven"])
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")

    def test_actual_source_change_invalidates_previous_manifest(self):
        manifest = self.build()
        Path(manifest["runtimeSources"][0]["path"]).write_bytes(b"changed source")
        with self.assertRaisesRegex(ValueError, "missing or stale"):
            API.verify_manifest(manifest)

    def seed_rejected_nonfatal_write(self):
        # 路径来自本测试显式输入；不是任意真实机器目录。
        path = "Z:\\OriginalOwnedProfileFixture\\profile.bin"
        self.isolation["nonfatalBlockedPaths"] = [path]
        self.launch["luaApiProbe"].update(nonfatalBlockedPaths=[path], writeDenied=True, blockedWriteCount=1,
            blockedWrites=[{"path": path, "nonfatal": True, "ready": True, "ntStatus": 0xC0000022, "originalNtDispatched": False}])

    def test_exact_rejected_write_keeps_truthful_flag_and_valid_partial(self):
        self.seed_rejected_nonfatal_write()
        manifest = self.build()
        self.assertTrue(manifest["writeContainment"]["writeDenied"])
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")

    def test_blocked_ledger_missing_tampered_fatal_and_overflow_are_rejected(self):
        import copy
        self.seed_rejected_nonfatal_write()
        original = copy.deepcopy(self.launch["luaApiProbe"])
        for field, value in (("blockedWriteCount", 33), ("fatalWriteCount", 1), ("blockedWriteOverflow", True),
                             ("writeDenied", False), ("nonfatalBlockedPaths", ["different"]), ("blockedWrites", [])):
            self.launch["luaApiProbe"] = copy.deepcopy(original)
            self.launch["luaApiProbe"][field] = value
            with self.assertRaises(ValueError): self.build()
        for field, value in (("path", "different"), ("nonfatal", False), ("ready", False), ("ntStatus", 0), ("originalNtDispatched", True)):
            self.launch["luaApiProbe"] = copy.deepcopy(original)
            self.launch["luaApiProbe"]["blockedWrites"][0][field] = value
            with self.assertRaises(ValueError): self.build()

    def test_rejected_write_cannot_be_deleted_from_a_frozen_manifest(self):
        self.seed_rejected_nonfatal_write()
        manifest = self.build()
        manifest["writeContainment"]["blockedWrites"] = []
        with self.assertRaisesRegex(ValueError, "differ from frozen evidence"): API.verify_manifest(manifest)

    def test_explicit_path_cannot_turn_author_runtime_write_nonfatal(self):
        self.seed_rejected_nonfatal_write()
        path = str(Path(self.modules[0]["path"]).parent / "forbidden-author.txt")
        self.isolation["nonfatalBlockedPaths"] = [path]
        self.launch["luaApiProbe"]["nonfatalBlockedPaths"] = [path]
        self.launch["luaApiProbe"]["blockedWrites"][0]["path"] = path
        with self.assertRaisesRegex(ValueError, "Author runtime"): self.build()

    def test_two_exact_rejections_and_unknown_third_path(self):
        import copy
        self.seed_rejected_nonfatal_write()
        second = "Z:\\OriginalOwnedProfileFixture\\second.bin"
        self.isolation["nonfatalBlockedPaths"].append(second)
        self.launch["luaApiProbe"]["nonfatalBlockedPaths"].append(second)
        row = copy.deepcopy(self.launch["luaApiProbe"]["blockedWrites"][0]); row["path"] = second
        self.launch["luaApiProbe"]["blockedWrites"].append(row); self.launch["luaApiProbe"]["blockedWriteCount"] = 2
        self.assertEqual(API.verify_manifest(self.build())["status"], "verified-partial")
        self.launch["luaApiProbe"]["blockedWrites"][1]["path"] = "Z:\\OriginalOwnedProfileFixture\\unknown-third.bin"
        with self.assertRaises(ValueError): self.build()

    def test_duplicate_overcapacity_and_alias_configuration_are_rejected(self):
        self.seed_rejected_nonfatal_write()
        first = self.isolation["nonfatalBlockedPaths"][0]
        for paths in ([first, first.upper()], [first + str(index) for index in range(5)], ["relative.bin"], ["Z:\\unsafe:stream"]):
            self.isolation["nonfatalBlockedPaths"] = paths
            self.launch["luaApiProbe"]["nonfatalBlockedPaths"] = paths
            self.launch["luaApiProbe"]["blockedWrites"][0]["path"] = paths[0]
            with self.assertRaises(ValueError): self.build()

    def test_missing_source_is_never_a_complete_inventory(self):
        manifest = self.build()
        Path(manifest["runtimeSources"][0]["path"]).unlink()
        with self.assertRaises(ValueError):
            API.verify_manifest(manifest)

    def test_fixture_capture_is_rejected_as_real_evidence(self):
        self.observed["fixture"] = True
        with self.assertRaisesRegex(ValueError, "genuine"):
            self.build()

    def test_observed_fields_and_signatures_cannot_be_rewritten_after_generation(self):
        manifest = self.build()
        for name in ("field", "signature"):
            changed = json.loads(json.dumps(manifest))
            field = changed["modules"][0]["fields"][0]
            if name == "field":
                field["name"] = "InventedApi"
            else:
                field["signature"]["parameterCount"] = 99
            with self.assertRaisesRegex(ValueError, "differ from frozen evidence"):
                API.verify_manifest(changed)

    def test_initial_zero_enum_pending_is_not_a_desktop_proof(self):
        # 只调用纯判定函数；不绑定 Win32 API、不枚举窗口、不启动任何实例。
        runner_path = Path(__file__).with_name("run-lua-api-probe.py")
        runner_spec = importlib.util.spec_from_file_location("runtime_probe_pending_contract", runner_path)
        runner = importlib.util.module_from_spec(runner_spec)
        runner_spec.loader.exec_module(runner)
        pending = runner.observed_initial_enum_pending
        baseline = (0, 0, 0, 0, False)
        self.assertIs(pending(*baseline), True)
        rejected = [
            ("complete-enumeration", (1, 0, 0, 0, False)),
            ("api-error", (0, 5, 0, 0, False)),
            ("callback-entered", (0, 0, 1, 0, False)),
            ("partial-row", (0, 0, 0, 1, False)),
        ]
        for index in range(4):
            for value in (False, None, 0.0, "0", -1):
                changed = list(baseline)
                changed[index] = value
                rejected.append(("numeric-type-" + str(index) + "-" + repr(value), tuple(changed)))
        for value in (True, None, 0, "false", []):
            rejected.append(("callback-failure-" + repr(value), (0, 0, 0, 0, value)))
        for label, observation in rejected:
            with self.subTest(predicate=label):
                self.assertIs(pending(*observation), False)

        self.seed_hidden_desktop_witness()
        # 初始 FALSE/error0/无回调只能保留为未形成证明的诊断。
        self.isolation["bridgeDesktopEnumerationPending"] = {
            "zeroNoCallbackCount": 2, "lastReturn": 0, "lastError": 0}
        self.assert_receipt_rejections([
            ("pending-no-final-proof", [(('isolation', 'bridgeDesktop'), DELETE)]),
            ("pending-diagnostic-as-proof", [(('isolation', 'bridgeDesktop'), {
                "proofKind": "pending", "zeroNoCallbackCount": 2, "lastReturn": 0, "lastError": 0})]),
            ("pending-incomplete-observation", [
                (('isolation', 'bridgeDesktop', 'enumerationComplete'), False),
                (('isolation', 'bridgeDesktop', 'windowCount'), 0)]),
            ("pending-empty-complete-claim", [(('isolation', 'bridgeDesktop', 'windowCount'), 0)]),
        ])
        # 后来真正完整的观察可被接纳；此前诊断不能扩大检查覆盖或写回权限。
        manifest = self.build()
        self.assertIs(manifest["desktopProof"]["enumerationComplete"], True)
        self.assertEqual(manifest["desktopProof"]["windowCount"], 1)
        self.assertIs(manifest["fullyEnumerated"], False)
        self.assertIs(manifest["semanticChecksComplete"], False)
        self.assertIs(manifest["savePermissionGranted"], False)
        self.assertIs(manifest["unknownDoesNotMeanMissing"], True)
        self.assertTrue(all(row["absenceProven"] is False for row in manifest["modules"]))
        self.assertEqual(API.verify_manifest(manifest)["status"], "verified-partial")

    def test_frozen_receipts_must_exist_and_remain_unchanged(self):
        manifest = self.build()
        evidence = Path(manifest["evidenceSources"][0]["path"])
        original = evidence.read_bytes()
        evidence.write_bytes(b"changed receipt")
        with self.assertRaisesRegex(ValueError, "Evidence source is missing or stale"):
            API.verify_manifest(manifest)
        evidence.write_bytes(original)
        evidence.unlink()
        with self.assertRaisesRegex(ValueError, "Evidence source is missing or stale"):
            API.verify_manifest(manifest)

    def test_require_failure_does_not_prove_absent_module(self):
        manifest = self.build()
        row = manifest["modules"][2]
        self.assertFalse(row["observedAvailable"])
        self.assertFalse(row["absenceProven"])
        self.assertFalse(row["enumerationComplete"])

    def test_nonce_write_guard_and_job_are_required(self):
        self.observed["runtimePreloadOnly"] = False
        with self.assertRaisesRegex(ValueError, "genuine"):
            self.build()
        self.observed["runtimePreloadOnly"] = True
        for change in ("nonce", "writeGuardInstalled", "writeDenied"):
            old = self.launch["luaApiProbe"][change]
            self.launch["luaApiProbe"][change] = "different" if change == "nonce" else not old
            with self.assertRaises(ValueError):
                self.build()
            self.launch["luaApiProbe"][change] = old
        self.isolation["jobBeforeClose"]["unresolvedAssociatedPids"] = [123]
        with self.assertRaisesRegex(ValueError, "terminal"):
            self.build()
        self.isolation["jobBeforeClose"]["unresolvedAssociatedPids"] = []
        for field in ("threadClosed", "bridgeHandleClosed", "luaEnvironmentOverridesAbsent"):
            self.isolation[field] = False
            with self.assertRaises(ValueError):
                self.build()
            self.isolation[field] = True

    def test_full_coverage_and_duplicate_keys_are_rejected(self):
        self.observed["modules"][0]["enumerationComplete"] = True
        with self.assertRaisesRegex(ValueError, "complete"):
            self.build()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            API.strict_json('{"nonce":1,"nonce":2}')

    def test_cli_missing_manifest_reports_truthful_syntax_fallback(self):
        completed = subprocess.run([sys.executable, "-B", str(SCRIPT), "verify", "--manifest", str(self.root / "missing.json")],
                                   capture_output=True, text=True, encoding="utf-8", timeout=10,
                                   env={**os.environ, "PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1"},
                                   creationflags=0x08000000 if sys.platform == "win32" else 0)
        self.assertEqual(completed.returncode, 1)
        result = json.loads(completed.stdout)
        self.assertEqual(result["status"], "fallback-syntax-only")
        self.assertFalse(result["semanticChecksComplete"])
        self.assertIn("目前只检查语法", result["message"])


if __name__ == "__main__":
    unittest.main()
