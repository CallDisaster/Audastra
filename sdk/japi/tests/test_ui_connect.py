#!/usr/bin/env python3
"""Exercise UI connection and scalar grammar source with controlled natives.

Only the small JASS subset used by these functions is translated. Unsupported
syntax is an error, not silently ignored. This is not a JASS VM or game-runtime
acceptance test; native calls and request/response transport are substituted.
No AI files are opened, and no directories are enumerated.
"""

from __future__ import annotations

import argparse
import ast
from collections import deque
from pathlib import Path
import re
import random
import unittest


UI_FUNCTIONS = ("Connect", "Check", "AudastraUIIsReady", "AudastraUILastError")
JAPI_FUNCTIONS = ("AjInitialize", "AjModuleLoad", "AjFail")
UI_GLOBALS = ("PREFIX", "connectStatus", "failedStatus", "failedRequest")
JAPI_GLOBALS = ("AjReady", "AjLastStatus", "AjNextRequest", "AjCapabilities", "AjLastInteger",
                "AjExpectedRuntimeIdentity", "AjLastText", "AjLastKind", "AjLastReal")
STUB_FUNCTIONS = ("AjRequest", "AjCallVoid", "AjCallBool", "AjCallText", "AjArgText", "AjArgInt", "I2S",
                  "AudastraUIDataErrors")
TYPES = {"boolean": "False", "integer": "0", "string": '""', "real": "0.0"}


def without_comment(line: str) -> str:
    quoted = False
    escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
        elif quoted and char == "\\":
            escaped = True
        elif char == '"':
            quoted = not quoted
        elif not quoted and line[index:index + 2] == "//":
            return line[:index].strip()
    if quoted:
        raise ValueError("unterminated JASS string")
    return line.strip()


def expression(text: str, names: set[str], calls: set[str]) -> str:
    """Whitelist the expression AST; never execute arbitrary source expressions."""
    tree = ast.parse(text, mode="eval")
    allowed = (ast.Expression, ast.Constant, ast.Name, ast.Load, ast.Call,
               ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not, ast.USub,
               ast.BinOp, ast.Add, ast.Compare, ast.Eq, ast.NotEq, ast.Lt,
               ast.LtE, ast.Gt, ast.GtE)
    for node in ast.walk(tree):
        if not isinstance(node, allowed):
            raise ValueError(f"unsupported expression: {text}")
        if isinstance(node, ast.Constant) and type(node.value) not in (str, int, bool, float):
            raise ValueError(f"unsupported literal: {text}")
        if isinstance(node, ast.Name) and node.id not in names | calls | {"true", "false", "null"}:
            raise ValueError(f"unknown identifier {node.id}")
        if isinstance(node, ast.Call) and (
                not isinstance(node.func, ast.Name) or node.func.id not in calls or node.keywords):
            raise ValueError(f"unsupported call: {text}")

    class Booleans(ast.NodeTransformer):
        def visit_Name(self, node: ast.Name) -> ast.AST:
            if node.id in ("true", "false", "null"):
                return ast.copy_location(ast.Constant(None if node.id == "null" else node.id == "true"), node)
            return node

    return ast.unparse(ast.fix_missing_locations(Booleans().visit(tree)))


def extract_function(source: str, name: str) -> tuple[str, list[str]]:
    lines = source.splitlines()
    starts = [index for index, line in enumerate(lines) if re.match(
        rf"\s*(?:private\s+)?function\s+{re.escape(name)}\s+takes\s+", line)]
    if len(starts) != 1:
        raise ValueError(f"expected exactly one function {name}")
    start = starts[0]
    for end in range(start + 1, len(lines)):
        if without_comment(lines[end]) == "endfunction":
            return without_comment(lines[start]), lines[start + 1:end]
    raise ValueError(f"missing endfunction for {name}")


def initial_globals(source: str, names: tuple[str, ...]) -> dict[str, object]:
    result = {}
    for name in names:
        declarations = re.findall(
            rf"^\s*(?:private\s+)?(?:constant\s+)?(integer|boolean|string|real|timer)\s+{name}\s*=\s*(.+)$",
            source, re.MULTILINE)
        if len(declarations) != 1:
            raise ValueError(f"expected exactly one initialized global {name}")
        kind, raw = declarations[0]
        value = ast.literal_eval(expression(without_comment(raw), set(), set()))
        expected = {"integer": int, "boolean": bool, "string": str, "real": float, "timer": type(None)}[kind]
        if type(value) is not expected:
            raise ValueError(f"wrong global initializer type for {name}")
        result[name] = value
    return result


def translate_function(source: str, name: str, globals_: set[str], extra_calls=()) -> str:
    header, lines = extract_function(source, name)
    declaration = re.fullmatch(
        rf"(?:private\s+)?function\s+{name}\s+takes\s+(.+)\s+returns\s+(boolean|string|integer|nothing)", header)
    if not declaration:
        raise ValueError(f"unsupported function declaration: {header}")
    arguments = []
    if declaration[1] != "nothing":
        for part in declaration[1].split(","):
            match = re.fullmatch(r"\s*(boolean|integer|string)\s+([A-Za-z][A-Za-z0-9_]*)\s*", part)
            if not match:
                raise ValueError(f"unsupported parameter: {part}")
            arguments.append(match[2])
    local_names = set(arguments)
    callable_names = set(UI_FUNCTIONS + JAPI_FUNCTIONS + STUB_FUNCTIONS) | set(extra_calls)
    output = [f"def {name}({', '.join(arguments)}):"]
    if globals_:
        output.append("    global " + ", ".join(sorted(globals_)))
    branches: list[str] = []
    statements_started = False

    def emit(text: str) -> None:
        output.append("    " * (len(branches) + 1) + text)

    def expr(text: str) -> str:
        return expression(text, globals_ | local_names, callable_names)

    for raw in lines:
        line = without_comment(raw)
        if not line:
            continue
        local = re.fullmatch(r"local (boolean|integer|string) ([A-Za-z][A-Za-z0-9_]*)(?: = (.+))?", line)
        if local:
            if statements_started or local[2] in globals_ | local_names | callable_names:
                raise ValueError(f"invalid local declaration in {name}: {line}")
            local_names.add(local[2])
            emit(f"{local[2]} = {expr(local[3]) if local[3] else TYPES[local[1]]}")
            continue
        statements_started = True
        if line.startswith("if ") and line.endswith(" then"):
            emit("if " + expr(line[3:-5]) + ":")
            branches.append("if")
        elif line.startswith("elseif ") and line.endswith(" then"):
            if not branches or branches[-1] != "if":
                raise ValueError(f"unexpected elseif in {name}")
            branches.pop()
            emit("elif " + expr(line[7:-5]) + ":")
            branches.append("if")
        elif line == "else":
            if not branches or branches[-1] != "if":
                raise ValueError(f"unexpected else in {name}")
            branches.pop()
            emit("else:")
            branches.append("else")
        elif line == "endif":
            if not branches or branches[-1] not in ("if", "else"):
                raise ValueError(f"unexpected endif in {name}")
            branches.pop()
        elif line == "loop":
            emit("while True:")
            branches.append("loop")
        elif line.startswith("exitwhen "):
            if "loop" not in branches:
                raise ValueError(f"exitwhen outside loop in {name}")
            emit("if " + expr(line[9:]) + ": break")
        elif line == "endloop":
            if not branches or branches[-1] != "loop":
                raise ValueError(f"unexpected endloop in {name}")
            branches.pop()
        elif line == "return":
            emit("return")
        elif line.startswith("call "):
            call = re.sub(r"\bfunction\s+([A-Za-z][A-Za-z0-9_]*)", r"\1", line[5:])
            if not isinstance(ast.parse(call, mode="eval").body, ast.Call):
                raise ValueError(f"unsupported call statement: {line}")
            emit(expr(call))
        elif line.startswith("return "):
            emit("return " + expr(line[7:]))
        elif line.startswith("set "):
            assignment = re.fullmatch(r"set ([A-Za-z][A-Za-z0-9_]*) = (.+)", line)
            if not assignment or assignment[1] not in (globals_ | local_names) - {"PREFIX"}:
                raise ValueError(f"unsupported assignment in {name}: {line}")
            emit(assignment[1] + " = " + expr(assignment[2]))
        else:
            raise ValueError(f"unsupported statement in {name}: {line}")
    if branches:
        raise ValueError(f"unclosed if in {name}")
    return "\n".join(output) + "\n"


class Transport:
    """Deterministic request outcomes; no native bridge or protocol parsing."""

    def __init__(self, state: dict, *, hello=(), modules=(), ready=(), errors=(), hello_default=0,
                 identities=(), identity_default=(0, "")):
        self.state = state
        self.outcomes = {"host.hello": deque(hello), "module.load": deque(modules),
                         "audastraui.ready": deque(ready), "audastraui.error": deque(errors),
                         "host.runtimeidentity": deque(identities)}
        self.defaults = {"host.hello": hello_default, "module.load": 0,
                         "audastraui.ready": (0, True), "audastraui.error": (0, ""),
                         "host.runtimeidentity": identity_default}
        self.calls: list[tuple[str, str, str]] = []
        self.text = ""

    def request(self, command: str, arguments: str, expected_kind: str) -> bool:
        expected = {"host.hello": ("", "i"), "module.load": (";s:audastraui", "v"),
                    "audastraui.ready": ("", "b"), "host.runtimeidentity": ("", "s")}
        if command == "audastraui.error":
            if not re.fullmatch(r";i:[0-9]+", arguments) or expected_kind != "s":
                raise AssertionError("unexpected error-query arguments")
        elif command not in expected or (arguments, expected_kind) != expected[command]:
            raise AssertionError(f"unexpected request: {(command, arguments, expected_kind)}")
        self.calls.append((command, arguments, expected_kind))
        self.state["AjNextRequest"] += 1
        queue = self.outcomes[command]
        result = queue.popleft() if queue else self.defaults[command]
        status, value = result if isinstance(result, tuple) else (result, 3 if command == "host.hello" else 0)
        self.state["AjLastStatus"] = status
        self.state["AjLastInteger"] = int(value) if status == 0 and expected_kind in ("i", "b") else 0
        self.text = value if status == 0 and expected_kind == "s" else ""
        self.state["AjLastText"] = self.text
        return status == 0

    def call_bool(self, command: str, arguments: str) -> bool:
        return self.request(command, arguments, "b") and self.state["AjLastInteger"] != 0

    def call_text(self, command: str, arguments: str) -> str:
        return self.text if self.request(command, arguments, "s") else ""

    def count(self, command: str) -> int:
        return sum(call[0] == command for call in self.calls)


def runtime(ui_source: str, japi_source: str, **outcomes) -> tuple[dict, Transport]:
    state = initial_globals(ui_source, UI_GLOBALS) | initial_globals(japi_source, JAPI_GLOBALS)
    globals_ = set(state)
    transport = Transport(state, **outcomes)
    state.update({"__builtins__": {}, "AjRequest": transport.request,
                  "AjCallVoid": lambda command, args: transport.request(command, args, "v"),
                  "AjCallBool": transport.call_bool, "AjCallText": transport.call_text,
                  "AjArgText": lambda text: ";s:" + text,
                  "AjArgInt": lambda number: ";i:" + str(number), "I2S": str,
                  # 这些用例从不绑定界面数据面（dataStatus 保持 -1），库里的数据面错误查询此时返回空、不发请求。
                  "AudastraUIDataErrors": lambda: ""})
    for source, names in ((ui_source, UI_FUNCTIONS), (japi_source, JAPI_FUNCTIONS)):
        for name in names:
            translated = translate_function(source, name, globals_)
            exec(compile(translated, f"<translated {name}>", "exec"), state)
    return state, transport


class ConnectTests(unittest.TestCase):
    ui_source = ""
    japi_source = ""

    def make(self, **outcomes):
        return runtime(self.ui_source, self.japi_source, **outcomes)

    def test_hello_failure_recovers_on_next_call(self):
        state, transport = self.make(hello=(16, 0))
        self.assertFalse(state["AudastraUIIsReady"]())
        self.assertFalse(state["AjReady"])
        self.assertEqual(state["connectStatus"], 16)
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(transport.count("host.hello"), 2)
        self.assertEqual(transport.count("module.load"), 1)
        self.assertEqual(state["AjLastStatus"], 0)
        self.assertEqual(state["connectStatus"], 0)

    def test_matching_identity_is_required_and_then_cached(self):
        state, transport = self.make(identity_default=(0, "a" * 64))
        state["AjExpectedRuntimeIdentity"] = "a" * 64
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(transport.count("host.runtimeidentity"), 1)
        self.assertEqual(transport.count("module.load"), 1)
        self.assertTrue(state["AjReady"])

    def test_wrong_or_missing_identity_never_loads_ui(self):
        for identity in ((0, "b" * 64), (0, ""), (16, "")):
            with self.subTest(identity=identity):
                state, transport = self.make(identity_default=identity)
                state["AjExpectedRuntimeIdentity"] = "a" * 64
                self.assertEqual([state["AudastraUIIsReady"]() for _ in range(150)], [False] * 150)
                self.assertFalse(state["AjReady"])
                self.assertEqual(transport.count("module.load"), 0)
                self.assertEqual(transport.count("host.runtimeidentity"), 150)
                self.assertEqual(state["AjLastStatus"], 14 if identity[0] == 0 else identity[0])

    def test_identity_failure_can_recover_at_attempt_150(self):
        state, transport = self.make(identities=[(0, "b" * 64)] * 149 + [(0, "a" * 64)])
        state["AjExpectedRuntimeIdentity"] = "a" * 64
        self.assertEqual([state["AudastraUIIsReady"]() for _ in range(150)], [False] * 149 + [True])
        self.assertEqual(transport.count("host.hello"), 150)
        self.assertEqual(transport.count("module.load"), 1)
        self.assertTrue(state["AjReady"])

    def test_attempt_150_can_still_recover(self):
        state, transport = self.make(hello=([16] * 149 + [0]))
        self.assertEqual([state["AudastraUIIsReady"]() for _ in range(150)], [False] * 149 + [True])
        self.assertEqual(transport.count("host.hello"), 150)
        self.assertEqual(transport.count("module.load"), 1)

    def test_module_failure_retries_without_repeating_successful_hello(self):
        state, transport = self.make(modules=(8, 0))
        self.assertFalse(state["AudastraUIIsReady"]())
        self.assertTrue(state["AjReady"])
        self.assertEqual(state["connectStatus"], 8)
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(transport.count("host.hello"), 1)
        self.assertEqual(transport.count("module.load"), 2)
        self.assertEqual(state["AjLastStatus"], 0)

    def test_absent_runtime_fails_closed_for_all_150_attempts(self):
        state, transport = self.make(hello_default=16)
        self.assertEqual([state["AudastraUIIsReady"]() for _ in range(150)], [False] * 150)
        self.assertEqual(transport.count("host.hello"), 150)
        self.assertEqual(len(transport.calls), 150)
        self.assertEqual(state["connectStatus"], 16)

    def test_success_is_cached_but_readiness_is_polled(self):
        state, transport = self.make()
        self.assertTrue(all(state["AudastraUIIsReady"]() for _ in range(10)))
        self.assertEqual(transport.count("host.hello"), 1)
        self.assertEqual(transport.count("module.load"), 1)
        self.assertEqual(transport.count("audastraui.ready"), 10)

    def test_readiness_can_recover_without_reloading_module(self):
        state, transport = self.make(ready=((0, False), (0, True)))
        self.assertFalse(state["AudastraUIIsReady"]())
        self.assertEqual(state["connectStatus"], 0)
        self.assertEqual(state["failedStatus"], 8)
        self.assertEqual(state["failedRequest"], 3)
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(transport.count("module.load"), 1)
        self.assertEqual(transport.count("audastraui.ready"), 2)

    def test_connection_and_readiness_failures_retain_diagnostics(self):
        state, transport = self.make(hello=(16, 0), ready=((8, False),),
                                     errors=((0, "UI backend unavailable"), (8, "")))
        self.assertFalse(state["AudastraUIIsReady"]())
        self.assertEqual(state["AudastraUILastError"](), "AudastraUI 运行库未连接（JAPI 状态 16）")
        self.assertFalse(state["AudastraUIIsReady"]())
        self.assertEqual(state["failedStatus"], 8)
        request = state["failedRequest"]
        self.assertEqual(state["AudastraUILastError"](), "UI backend unavailable")
        self.assertEqual(transport.calls[-1], ("audastraui.error", f";i:{request}", "s"))
        self.assertEqual(state["AudastraUILastError"](), "AudastraUI 调用失败（JAPI 状态 8）")
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(state["AjLastStatus"], 0)
        self.assertEqual(state["connectStatus"], 0)

    def test_error_query_does_not_retry_failed_connection(self):
        state, transport = self.make(hello=(16, 0))
        self.assertFalse(state["AudastraUIIsReady"]())
        previous_calls = list(transport.calls)
        self.assertEqual(state["AudastraUILastError"](), "AudastraUI 运行库未连接（JAPI 状态 16）")
        self.assertEqual(transport.calls, previous_calls)
        self.assertEqual(state["connectStatus"], 16)
        self.assertEqual(state["AjLastStatus"], 16)
        self.assertFalse(state["AjReady"])
        self.assertTrue(state["AudastraUIIsReady"]())
        self.assertEqual(transport.count("host.hello"), 2)
        self.assertEqual(state["AudastraUILastError"](), "")

    def test_error_query_before_connection_does_not_start_handshake(self):
        state, transport = self.make()
        self.assertEqual(state["AudastraUILastError"](), "AudastraUI 运行库未连接（JAPI 状态 -1）")
        self.assertEqual(transport.calls, [])
        self.assertEqual(state["connectStatus"], -1)
        self.assertFalse(state["AjReady"])

    def test_cached_connect_does_not_erase_existing_call_error(self):
        state, transport = self.make()
        self.assertTrue(state["Connect"]())
        previous_calls = list(transport.calls)
        state["AjLastStatus"] = 5
        self.assertTrue(state["Connect"]())
        self.assertEqual(state["AjLastStatus"], 5)
        self.assertEqual(transport.calls, previous_calls)

    def test_old_failure_cache_mutation_is_detected(self):
        header, body = extract_function(self.ui_source, "Connect")
        original = "\n".join([header, *body, "endfunction"])
        mutated, count = re.subn(r"if\s+connectStatus\s*!=\s*0\s+then", "if connectStatus < 0 then", original)
        self.assertEqual(count, 1, "mutation must replace the actual success-cache condition exactly once")
        start = self.ui_source.index(header)
        end = self.ui_source.index("endfunction", start) + len("endfunction")
        source = self.ui_source[:start] + mutated + self.ui_source[end:]
        state, transport = runtime(source, self.japi_source, hello=(16, 0))
        self.assertFalse(state["AudastraUIIsReady"]())
        with self.assertRaises(AssertionError):
            self.assertTrue(state["AudastraUIIsReady"](), "old condition must fail the recovery assertion")
        self.assertEqual(transport.count("host.hello"), 1)

    def test_unsupported_statements_are_rejected(self):
        source = self.ui_source.replace("return connectStatus == 0", "call UnsupportedNative()\nreturn connectStatus == 0", 1)
        self.assertNotEqual(source, self.ui_source)
        with self.assertRaisesRegex(ValueError, "unsupported (statement|call)"):
            runtime(source, self.japi_source)


class RealSyntaxTests(unittest.TestCase):
    """Execute the actual source grammar with substituted string/integer natives.

    These are ASCII grammar checks, not VM performance or R2SW precision proof.
    The single-character canonical roundtrip is also exercised by a game probe.
    """
    japi_source = ""

    def setUp(self):
        def s2i(text):
            match = re.match(r"\s*[+-]?[0-9]+", text)
            return int(match[0]) if match else 0

        self.state = {"__builtins__": {}, "StringLength": len, "SubString": lambda s, a, b: s[max(a, 0):max(b, 0)],
                      "S2I": s2i, "I2S": str}
        names = ("AjCanonicalInt", "AjFind", "AjRealSyntax", "StringLength", "SubString", "S2I", "I2S")
        for name in names[:3]:
            code = translate_function(self.japi_source, name, set(), names)
            exec(compile(code, f"<translated {name}>", "exec"), self.state)

    @staticmethod
    def reference(text):
        if not 1 <= len(text) <= 32:
            return False
        match = re.fullmatch(r"-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE]([+-]?[0-9]{1,2}))?", text)
        return match is not None and (match[1] is None or -46 <= int(match[1]) <= 46)

    def test_decimal_and_exponent_boundaries(self):
        samples = ("0", "-0", "01", ".5", "-.5", "1.", "1e46", "1e-46", "1E+46", "1e00",
                   "1e47", "1e-47", "1e046", "1e+", "1..0", ".", "-", "+1", " 1", "1 ",
                   "nan", "inf", "1;0", "١", "１", "1e١", "0" * 32, "0" * 33, "")
        for text in samples:
            with self.subTest(text=text):
                self.assertEqual(self.state["AjRealSyntax"](text), self.reference(text))

    def test_differential_grammar(self):
        random_ = random.Random(270528)
        for _ in range(4000):
            text = "".join(random_.choices("0123456789eE+-.; x", k=random_.randrange(34)))
            self.assertEqual(self.state["AjRealSyntax"](text), self.reference(text), repr(text))

    def test_every_single_character_byte(self):
        for value in range(256):
            ch = chr(value)
            self.assertEqual(self.state["AjCanonicalInt"](ch), ch in "0123456789", repr(ch))


class DiagnosticTests(unittest.TestCase):
    ui_source = ""
    japi_source = ""

    def make(self, **outcomes):
        state, transport = runtime(self.ui_source, self.japi_source, **outcomes)
        globals_ = set(initial_globals(self.japi_source, JAPI_GLOBALS))
        diagnostics = initial_globals(self.japi_source, ("AjTestDiagnosticStarted", "AjTestDiagnosticAttempts", "AjTestDiagnosticTimer"))
        state.update(diagnostics)
        globals_.update(diagnostics)
        starts, stops, messages = [], [], []
        state.update({"CreateTimer": lambda: 1,
                      "TimerStart": lambda timer, period, repeating, callback: starts.append((timer, period, repeating, callback)),
                      "PauseTimer": lambda timer: stops.append(("pause", timer)),
                      "DestroyTimer": lambda timer: stops.append(("destroy", timer)),
                      "GetLocalPlayer": lambda: 0,
                      "DisplayTimedTextToPlayer": lambda player, x, y, seconds, text: messages.append((player, seconds, text))})
        names = ("AjStopTestDiagnostics", "AjPollTestDiagnostics", "AjStartTestDiagnostics", "CreateTimer", "TimerStart",
                 "PauseTimer", "DestroyTimer", "GetLocalPlayer", "DisplayTimedTextToPlayer")
        for name in names[:3]:
            source = translate_function(self.japi_source, name, globals_, names)
            exec(compile(source, f"<translated {name}>", "exec"), state)
        return state, transport, starts, stops, messages

    def test_formal_client_does_not_schedule_or_display(self):
        _, transport, starts, stops, messages = self.make(hello_default=16)
        self.assertEqual((transport.calls, starts, stops, messages), ([], [], [], []))

    def test_150_failures_display_once_and_destroy(self):
        state, transport, starts, stops, messages = self.make(hello_default=16)
        state["AjStartTestDiagnostics"]()
        state["AjStartTestDiagnostics"]()
        self.assertEqual(len(starts), 1)
        self.assertEqual(starts[0][:3], (1, 0.2, True))
        callback = starts[0][3]
        for _ in range(149):
            callback()
        self.assertEqual((stops, messages), ([], []))
        callback()
        self.assertEqual(transport.count("host.hello"), 150)
        self.assertEqual(stops, [("pause", 1), ("destroy", 1)])
        self.assertEqual(len(messages), 1)
        self.assertIn("16", messages[0][2])
        self.assertIsNone(state["AjTestDiagnosticTimer"])
        state["AjStartTestDiagnostics"]()
        self.assertEqual(len(starts), 1)
        callback()
        self.assertEqual(len(messages), 1)
        self.assertEqual(stops, [("pause", 1), ("destroy", 1)])

    def test_success_at_first_or_last_attempt_is_silent(self):
        for attempt in (1, 150):
            with self.subTest(attempt=attempt):
                state, transport, starts, stops, messages = self.make(hello=[16]*(attempt-1)+[0])
                state["AjStartTestDiagnostics"]()
                for _ in range(attempt):
                    starts[0][3]()
                self.assertEqual(transport.count("host.hello"), attempt)
                self.assertEqual(stops, [("pause", 1), ("destroy", 1)])
                self.assertEqual(messages, [])

    def test_identity_mismatch_is_a_bounded_failure(self):
        state, _, starts, stops, messages = self.make(identity_default=(0, "b"*64))
        state["AjExpectedRuntimeIdentity"] = "a"*64
        state["AjStartTestDiagnostics"]()
        for _ in range(150):
            starts[0][3]()
        self.assertEqual(len(messages), 1)
        self.assertIn("14", messages[0][2])
        self.assertEqual(stops, [("pause", 1), ("destroy", 1)])


def read_jass(path: Path) -> str:
    # Check before and after resolution so a .j symlink cannot redirect to .ai.
    if path.suffix.lower() != ".j":
        raise ValueError("only explicit .j source paths are accepted; AI files are forbidden")
    resolved = path.resolve(strict=True)
    if resolved.suffix.lower() != ".j" or not resolved.is_file():
        raise ValueError("resolved source must be a .j file; AI files are forbidden")
    return resolved.read_text(encoding="utf-8-sig")


def main() -> int:
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ui", nargs="?", type=Path, default=root / "assets/ui-library/AudastraUI/jass/AudastraUI.j")
    parser.add_argument("japi", nargs="?", type=Path, default=root / "sdk/japi/jass/AudastraJapi.j")
    args = parser.parse_args()
    try:
        ConnectTests.ui_source = read_jass(args.ui)
        ConnectTests.japi_source = read_jass(args.japi)
        RealSyntaxTests.japi_source = ConnectTests.japi_source
        DiagnosticTests.ui_source = ConnectTests.ui_source
        DiagnosticTests.japi_source = ConnectTests.japi_source
    except (OSError, ValueError) as error:
        parser.error(str(error))
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ConnectTests)
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(RealSyntaxTests))
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(DiagnosticTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
