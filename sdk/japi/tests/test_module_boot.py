#!/usr/bin/env python3
"""Execute the SDK's bounded module state machine with controlled natives only."""
import sys
from pathlib import Path
import unittest
from test_ui_connect import initial_globals, translate_function, JAPI_GLOBALS, Transport, read_jass

SOURCE = Path(__file__).resolve().parents[1] / "jass" / "AudastraJapi.j"
FUNCTIONS = ("AjFail", "AjInitialize", "AjStopRuntimeModule", "AjRuntimeModuleLoaded",
             "AjPollRuntimeModule", "AjStartRuntimeModule")
GLOBALS = JAPI_GLOBALS + ("AjRuntimeModuleAttempts", "AjRuntimeModuleTimer")

class ModuleBoot(unittest.TestCase):
    def make(self, loaded=True, hello=(), identity=(0, "expected")):
        source = read_jass(SOURCE)
        state = initial_globals(source, GLOBALS)
        state["AjRuntimeModuleExpected"] = True
        state["AjExpectedRuntimeIdentity"] = "expected"
        events = []
        transport = Transport(state, hello=hello, identity_default=identity)
        state.update({"AjRequest": transport.request, "CreateTimer": lambda: object(),
                      "TimerStart": lambda *a: events.append(("timer", a[1:3])),
                      "PauseTimer": lambda t: events.append(("pause",)),
                      "DestroyTimer": lambda t: events.append(("destroy",))})
        def load(name):
            self.assertEqual(name, "AudastraRuntimeModuleLoad")
            events.append(("load",))
            if loaded is not None:
                state["AjRuntimeModuleLoaded"](loaded)
        state["ExecuteFunc"] = load
        calls = FUNCTIONS + ("CreateTimer", "TimerStart", "PauseTimer", "DestroyTimer", "ExecuteFunc")
        for name in FUNCTIONS:
            exec(translate_function(source, name, set(GLOBALS), calls), state)
        return state, events, transport

    def test_delayed_bridge_and_single_load_per_vm(self):
        state, events, wire = self.make(hello=(8, 16, 0))
        self.assertFalse(state["AjInitialize"]())
        self.assertEqual(wire.calls, [])
        state["AjStartRuntimeModule"]()
        self.assertEqual(events, [("timer", (0.2, True))])
        for _ in range(3): state["AjPollRuntimeModule"]()
        self.assertEqual(state["AjRuntimeModuleState"], 3)
        self.assertTrue(state["AjInitialize"]())
        self.assertEqual(events.count(("load",)), 1)
        for _ in range(200):
            state["AjStartRuntimeModule"]()
            state["AjPollRuntimeModule"]()
        self.assertEqual(events.count(("load",)), 1)
        self.assertEqual(events.count(("destroy",)), 1)

    def test_bounded_timeout_and_no_restart(self):
        state, events, wire = self.make(hello=(8,) * 150)
        state["AjStartRuntimeModule"]()
        for _ in range(150): state["AjPollRuntimeModule"]()
        self.assertEqual(state["AjRuntimeModuleState"], -2)
        self.assertEqual(state["AjRuntimeModuleAttempts"], 150)
        self.assertIsNone(state["AjRuntimeModuleTimer"])
        self.assertFalse(state["AjInitialize"]())
        state["AjStartRuntimeModule"]()
        state["AjPollRuntimeModule"]()
        self.assertEqual(len(wire.calls), 150)
        self.assertEqual(events.count(("load",)), 1)

    def test_load_false_or_missing_shim_stops(self):
        for loaded in (False, None):
            state, events, wire = self.make(loaded=loaded)
            state["AjStartRuntimeModule"]()
            for _ in range(150): state["AjPollRuntimeModule"]()
            self.assertEqual(state["AjRuntimeModuleState"], -1)
            self.assertEqual(state["AjRuntimeModuleLastStatus"], 13)
            self.assertEqual(events.count(("load",)), 1)
            self.assertEqual(wire.calls, [])

    def test_identity_mismatch_is_terminal(self):
        state, events, wire = self.make(identity=(0, "previous-process-content"))
        state["AjStartRuntimeModule"]()
        state["AjPollRuntimeModule"]()
        self.assertEqual(state["AjRuntimeModuleState"], -3)
        self.assertFalse(state["AjReady"])
        self.assertFalse(state["AjInitialize"]())
        self.assertEqual(state["AjLastStatus"], 14)
        self.assertIsNone(state["AjRuntimeModuleTimer"])

    def test_ai_product_never_schedules_module(self):
        state, events, wire = self.make()
        state["AjRuntimeModuleExpected"] = False
        state["AjStartRuntimeModule"]()
        self.assertEqual(events, [])
        self.assertTrue(state["AjInitialize"]())

    def test_late_loaded_notification_cannot_reopen_terminal_state(self):
        state, events, wire = self.make(loaded=False)
        state["AjStartRuntimeModule"]()
        state["AjPollRuntimeModule"]()
        state["AjRuntimeModuleLoaded"](True)
        self.assertEqual(state["AjRuntimeModuleState"], -1)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
