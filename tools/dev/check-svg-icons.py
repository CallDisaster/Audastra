#!/usr/bin/env python3
"""扫描产品C++的SVG名称（含间接包装/表格），逐一核对资源；只读，不需要Qt。"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from itertools import islice
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


@dataclass(frozen=True)
class Token:
    value: str
    line: int
    string: bool = False


def tokens(source: str) -> list[Token]:
    """保留行号并跳过注释；不能把注释里的旧资源名称当成产品引用。"""
    result: list[Token] = []
    at, line = 0, 1
    while at < len(source):
        if source[at].isspace():
            line += source[at] == "\n"
            at += 1
        elif source.startswith("//", at):
            end = source.find("\n", at)
            at = len(source) if end < 0 else end
        elif source.startswith("/*", at):
            end = source.find("*/", at + 2)
            end = len(source) if end < 0 else end + 2
            line += source[at:end].count("\n")
            at = end
        elif source.startswith('R"', at):
            opening = source.find("(", at + 2, at + 19)
            if opening < 0:
                result.append(Token(source[at], line)); at += 1; continue
            delimiter = source[at + 2:opening]
            closing = source.find(")" + delimiter + '"', opening + 1)
            end = len(source) if closing < 0 else closing + len(delimiter) + 2
            result.append(Token(source[opening + 1:closing], line, True))
            line += source[at:end].count("\n")
            at = end
        elif source[at] in '\"\'':
            quote, start, start_line = source[at], at + 1, line
            at += 1
            while at < len(source):
                if source[at] == "\\":
                    at += 2
                elif source[at] == quote:
                    break
                else:
                    at += 1
            body = source[start:at]
            result.append(Token(body, start_line, quote == '"'))
            line += body.count("\n")
            at += 1
        elif source[at].isalpha() or source[at] == "_":
            start = at
            while at < len(source) and (source[at].isalnum() or source[at] == "_"):
                at += 1
            result.append(Token(source[start:at], line))
        else:
            result.append(Token(source[at], line)); at += 1
    return result


def references(source: str, namespaces: set[str], dynamic_lines: list[int] | None = None) -> list[tuple[str, int]]:
    parsed = tokens(source)
    found: set[tuple[str, int]] = set()
    # 所有资源命名空间字面量都检查，包含Icon(name)、按钮工厂及数组中的间接引用。
    for token in parsed:
        if token.string and token.value.partition("/")[0] in namespaces:
            found.add((token.value, token.line))
    # 直接SvgIcon第一参数中的字面量也检查，包含未知命名空间和QString包装/三元表达式。
    for index, token in enumerate(parsed):
        if token.value != "SvgIcon" or index + 1 == len(parsed) or parsed[index + 1].value != "(":
            continue
        if index + 2 < len(parsed) and parsed[index + 2].value == "const":
            continue  # 函数声明/定义不是一次资源请求。
        depth = 0
        has_literal = False
        for argument in islice(parsed, index + 2, None):
            if not argument.string:
                if argument.value == "(" : depth += 1
                elif argument.value == ")":
                    if depth == 0: break
                    depth -= 1
                elif argument.value == "," and depth == 0: break
            elif argument.value:
                found.add((argument.value, argument.line))
                has_literal = True
        if not has_literal and dynamic_lines is not None:
            dynamic_lines.append(token.line)
    return sorted(found, key=lambda value: (value[1], value[0]))


def scan(root: Path) -> dict:
    assets = root / "assets/icons"
    namespaces = {item.name for item in assets.iterdir() if item.is_dir()} if assets.is_dir() else set()
    seen: dict[str, list[str]] = {}
    dynamic = []
    for directory in (root / "src", root / "include"):
        for file in sorted(directory.rglob("*")):
            if file.is_file() and file.suffix in {".cpp", ".h", ".hpp", ".cc", ".cxx"}:
                source = file.read_text(encoding="utf-8-sig")
                # 不让编译器/归档等无关大型源进入词法扫描，扫描所有候选但只解析图标使用处。
                if "SvgIcon" not in source and not any(namespace + "/" in source for namespace in namespaces):
                    continue
                dynamic_lines: list[int] = []
                for name, line in references(source, namespaces, dynamic_lines):
                    seen.setdefault(name, []).append(f"{file.relative_to(root).as_posix()}:{line}")
                dynamic.extend(f"{file.relative_to(root).as_posix()}:{line}" for line in dynamic_lines)
    failures = []
    for name, locations in sorted(seen.items()):
        if not re.fullmatch(r"[a-zA-Z0-9_-]+(?:/[a-zA-Z0-9_-]+)+", name):
            failures.append({"name": name, "reason": "名称不是完整、安全的相对SVG资源路径", "locations": locations})
            continue
        file = assets / (name + ".svg")
        if not file.is_file():
            failures.append({"name": name, "reason": "资源文件不存在", "locations": locations})
            continue
        try:
            if file.stat().st_size > 1024 * 1024:
                raise ValueError("SVG超过1MiB检查预算")
            if ET.parse(file).getroot().tag not in {"svg", "{http://www.w3.org/2000/svg}svg"}:
                raise ValueError("根元素不是SVG")
        except (OSError, ET.ParseError, ValueError) as error:
            failures.append({"name": name, "reason": str(error), "locations": locations})
    if not seen:
        failures.append({"name": "", "reason": "未找到产品图标引用，不能通过空扫描", "locations": []})
    return {"reference_count": sum(map(len, seen.values())), "icon_count": len(seen),
            "references": seen, "failures": failures,
            "dynamic_calls": dynamic,
            "scope": "产品C++直接SvgIcon字面量及资源命名空间的间接字面量；不扫描测试fixture。变量参数/运行期生成的名称不构成静态全量证明，已另列dynamic_calls；编译后资源由Qt测试验证"}


def main() -> int:
    # Windows终端的本地代码页不应破坏CTest的UTF8回执。
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="待检查的仓库根目录")
    parser.add_argument("--output", type=Path, help="可选JSON回执；拒绝覆盖已有文件")
    args = parser.parse_args()
    try:
        report = scan(args.root.resolve())
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                json.dump(report, output, ensure_ascii=False, indent=2)
                output.write("\n")
        for failure in report["failures"]:
            print(f"{failure['name']}: {failure['reason']} ({', '.join(failure['locations'])})", file=sys.stderr)
        print(f"SVG资源扫描：{report['reference_count']}处引用，{report['icon_count']}种名称，{len(report['failures'])}项失败")
        print(f"变量名称调用：{len(report['dynamic_calls'])}处；包装函数中的资源命名空间字面量已核对，任意运行期名称仍由空图标/文字回退处理")
        return int(bool(report["failures"]))
    except (OSError, UnicodeError) as error:
        print(f"SVG资源扫描未完成：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
