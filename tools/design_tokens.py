"""由 assets/design/tokens.json 生成 Web 原型的 CSS 变量与 C++ 主题常量。

用法：
    python tools/design_tokens.py          # 重新生成
    python tools/design_tokens.py --check  # 只检查生成物是否最新（CTest 使用）
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "design" / "tokens.json"
CSS_OUT = ROOT / "editor-ui" / "src" / "studio" / "tokens.generated.css"
CPP_OUT = ROOT / "src" / "QtUiKit" / "Theme" / "DesignTokens.generated.h"

BANNER = "由 tools/design_tokens.py 从 assets/design/tokens.json 生成，请勿手改。"
HEX = re.compile(r"^#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?$")
IDENT = re.compile(r"^[a-z][A-Za-z0-9]*$")
# 数值令牌：分组名 → CSS 单位。
NUMERIC_SECTIONS = {"size": "px", "weight": ""}


def fail(message: str) -> None:
    raise SystemExit(f"design_tokens: {message}")


def kebab(name: str) -> str:
    return re.sub(r"([A-Z])", lambda m: "-" + m.group(1).lower(), name)


def load() -> dict:
    data = json.loads(SOURCE.read_text(encoding="utf-8"))
    if data.get("schema") != 1:
        fail("unsupported schema")
    order = data["colorOrder"]
    if len(set(order)) != len(order) or not all(IDENT.match(name) for name in order):
        fail("colorOrder must be unique camelCase identifiers")
    themes = data["themes"]
    if data["defaultTheme"] not in themes:
        fail("defaultTheme is not defined")
    for theme_id, theme in themes.items():
        if not IDENT.match(theme_id):
            fail(f"bad theme id {theme_id!r}")
        colors = theme["colors"]
        if set(colors) != set(order):
            missing = sorted(set(order) - set(colors))
            extra = sorted(set(colors) - set(order))
            fail(f"theme {theme_id}: missing {missing}, extra {extra}")
        for name, value in colors.items():
            if not HEX.match(value):
                fail(f"theme {theme_id}: {name} is not #rrggbb[aa]: {value}")
    for section in NUMERIC_SECTIONS:
        for name, value in data[section].items():
            if not IDENT.match(name) or not isinstance(value, int):
                fail(f"{section} {name} must be an integer")
    return data


def css_family(names: list[str]) -> str:
    generic = {"sans-serif", "serif", "monospace"}
    return ", ".join(name if name in generic else f'"{name}"' for name in names)


def render_css(data: dict) -> str:
    order = data["colorOrder"]
    default = data["defaultTheme"]
    lines = [f"/* {BANNER} */", ""]

    def block(selector: str, colors: dict, extra: list[str]) -> None:
        lines.append(f"{selector} {{")
        lines.extend(f"  --{kebab(name)}: {colors[name]};" for name in order)
        lines.extend(extra)
        lines.append("}")

    shared = [
        f"  --font-ui: {css_family(data['font']['ui'])};",
        f"  --font-code: {css_family(data['font']['code'])};",
    ]
    for section, unit in NUMERIC_SECTIONS.items():
        shared += [f"  --{kebab(name)}: {value}{unit};" for name, value in data[section].items()]
    block(".studio,\n.studio-dialog,\n.design-sheet", data["themes"][default]["colors"], shared)
    for theme_id, theme in data["themes"].items():
        if theme_id == default:
            continue
        lines.append("")
        block(f".studio.theme-{theme_id}", theme["colors"], [])
    return "\n".join(lines) + "\n"


def cpp_color(value: str) -> str:
    match = HEX.match(value)
    rgb, alpha = match.group(1), match.group(2) or "ff"
    r, g, b = (int(rgb[i:i + 2], 16) for i in (0, 2, 4))
    return f"{{{r}, {g}, {b}, {int(alpha, 16)}}}"


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_cpp(data: dict) -> str:
    order = data["colorOrder"]
    themes = data["themes"]
    out = [
        f"// {BANNER}",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace War3Map::UiKit::Tokens {",
        "",
        "struct Color {",
        "    std::uint8_t r, g, b, a;",
        "};",
        "",
        "struct ThemeColors {",
    ]
    out += [f"    Color {name};" for name in order]
    out += [
        "};",
        "",
        "struct ThemeTokens {",
        "    std::string_view id;",
        "    std::string_view label;",
        "    bool dark;",
        "    ThemeColors colors;",
        "};",
        "",
        f"inline constexpr std::array<ThemeTokens, {len(themes)}> kThemes{{{{",
    ]
    for theme_id, theme in themes.items():
        out.append(f"    {{{cpp_string(theme_id)}, {cpp_string(theme['label'])}, "
                   f"{'true' if theme['dark'] else 'false'}, {{")
        out += [f"        {cpp_color(theme['colors'][name])}, // {name}" for name in order]
        out.append("    }},")
    out += [
        "}};",
        "",
        f"inline constexpr std::string_view kDefaultTheme = {cpp_string(data['defaultTheme'])};",
        "",
    ]
    for role in ("ui", "code"):
        families = data["font"][role]
        name = "kUiFontFamilies" if role == "ui" else "kCodeFontFamilies"
        joined = ", ".join(cpp_string(family) for family in families)
        out.append(f"inline constexpr std::array<std::string_view, {len(families)}> {name}{{{joined}}};")
    out.append("")
    for section in NUMERIC_SECTIONS:
        namespace = section[0].upper() + section[1:]
        out.append(f"namespace {namespace} {{")
        for name, value in data[section].items():
            out.append(f"inline constexpr int k{name[0].upper()}{name[1:]} = {value};")
        out += [f"}} // namespace {namespace}", ""]
    out += ["} // namespace War3Map::UiKit::Tokens", ""]
    return "\n".join(out)


def normalized(text: str) -> str:
    return text.replace("\r\n", "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if generated files are stale")
    args = parser.parse_args()

    data = load()
    outputs = {CSS_OUT: (render_css(data), "\n"), CPP_OUT: (render_cpp(data), "\r\n")}
    stale = []
    for path, (text, newline) in outputs.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current is not None and normalized(current) == text:
            continue
        if args.check:
            stale.append(path.relative_to(ROOT).as_posix())
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline=newline) as handle:
            handle.write(text)
        print(f"wrote {path.relative_to(ROOT).as_posix()}")
    if stale:
        print("stale generated design tokens: " + ", ".join(stale), file=sys.stderr)
        print("run: python tools/design_tokens.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
