#!/usr/bin/env python3
"""Exercise the real UI connection functions against a controlled transport.

Only the small JASS subset used by these functions is translated. Unsupported
syntax is an error, not silently ignored. This is not a JASS VM, protocol parser
test, or game-runtime acceptance test; request/response transport is substituted.
No AI files are opened, and no directories are enumerated.
"""

from __future__ import annotations

import argparse
import ast
from collections import deque
from pathlib import Path
import re
import unittest


UI_FUNCTIONS = ("Connect", "Check", "AudastraUIIsReady", "AudastraUILastError")
JAPI_FUNCTIONS = ("AjInitialize", "AjModuleLoad")
UI_GLOBALS = ("PREFIX", "connectStatus", "failedStatus", "failedRequest")
JAPI_GLOBALS = ("AjReady", "AjLastStatus", "AjNextRequest", "AjCapabilities", "AjLastInteger")
STUB_FUNCTIONS = ("AjRequest", "AjCallVoid", "AjCallBool", "AjCallText", "AjArgText", "AjArgInt", "I2S")
TYPES = {"boolean": "False", "integer": "0", "string": '""'}


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
        if isinstance(node, ast.Constant) and type(node.value) not in (str, int, bool):
            raise ValueError(f"unsupported literal: {text}")
        if isinstance(node, ast.Name) and node.id not in names | calls | {"true", "false"}:
            raise ValueError(f"unknown identifier {node.id}")
        if isinstance(node, ast.Call) and (
                not isinstance(node.func, ast.Name) or node.func.id not in calls or node.keywords):
            raise ValueError(f"unsupported call: {text}")

    class Booleans(ast.NodeTransformer):
        def visit_Name(self, node: ast.Name) -> ast.AST:
            if node.id in ("true", "false"):
                return ast.copy_location(ast.Constant(node.id == "true"), node)
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
            rf"^\s*(?:private\s+)?(?:constant\s+)?(integer|boolean|string)\s+{name}\s*=\s*(.+)$",
            source, re.MULTILINE)
        if len(declarations) != 1:
            raise ValueError(f"expected exactly one initialized global {name}")
        kind, raw = declarations[0]
        value = ast.literal_eval(expression(without_comment(raw), set(), set()))
        expected = {"integer": int, "boolean": bool, "string": str}[kind]
        if type(value) is not expected:
            raise ValueError(f"wrong global initializer type for {name}")
        result[name] = value
    return result


def translate_function(source: str, name: str, globals_: set[str]) -> str:
    header, lines = extract_function(source, name)
    declaration = re.fullmatch(
        rf"(?:private\s+)?function\s+{name}\s+takes\s+(.+)\s+returns\s+(boolean|string)", header)
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
    callable_names = set(UI_FUNCTIONS + JAPI_FUNCTIONS + STUB_FUNCTIONS)
    output = [f"def {name}({', '.join(arguments)}):", "    global " + ", ".join(sorted(globals_))]
    branches: list[bool] = []
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
            branches.append(False)
        elif line.startswith("elseif ") and line.endswith(" then"):
            if not branches or branches[-1]:
                raise ValueError(f"unexpected elseif in {name}")
            branches.pop()
            emit("elif " + expr(line[7:-5]) + ":")
            branches.append(False)
        elif line == "else":
            if not branches or branches[-1]:
                raise ValueError(f"unexpected else in {name}")
            branches.pop()
            emit("else:")
            branches.append(True)
        elif line == "endif":
            if not branches:
                raise ValueError(f"unexpected endif in {name}")
            branches.pop()
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

    def __init__(self, state: dict, *, hello=(), modules=(), ready=(), errors=(), hello_default=0):
        self.state = state
        self.outcomes = {"host.hello": deque(hello), "module.load": deque(modules),
                         "audastraui.ready": deque(ready), "audastraui.error": deque(errors)}
        self.defaults = {"host.hello": hello_default, "module.load": 0,
                         "audastraui.ready": (0, True), "audastraui.error": (0, "")}
        self.calls: list[tuple[str, str, str]] = []
        self.text = ""

    def request(self, command: str, arguments: str, expected_kind: str) -> bool:
        expected = {"host.hello": ("", "i"), "module.load": (";s:audastraui", "v"),
                    "audastraui.ready": ("", "b")}
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
                  "AjArgInt": lambda number: ";i:" + str(number), "I2S": str})
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
        with self.assertRaisesRegex(ValueError, "unsupported statement"):
            runtime(source, self.japi_source)


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
    except (OSError, ValueError) as error:
        parser.error(str(error))
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ConnectTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
