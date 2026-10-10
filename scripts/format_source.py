#!/usr/bin/env python3
"""ZRdpのC++書式を、literalとtokenを保持して正規化する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0

clang-formatの共通設定に、指定規約の片側括弧空白とセミコロン前空白を補う。
ファイル版、更新履歴、説明本文は自動更新しない。
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(
    r'(?P<space>\s+)'
    r'|(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<string>(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*")'
    r"|(?P<char>(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*')"
    r"|(?P<word>[A-Za-z_0-9]+(?:'[A-Za-z_0-9]+)*)"
    r'|(?P<operator>>>=|<<=|->\*|<=>|\.\.\.|::|->|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\[\[|\]\])'
    r'|(?P<other>[^\s])'
)


def tokens(source: str) -> list[re.Match[str]]:
    """commentと空白を除き、literalを一つのtokenとして保持する。"""
    return [match for match in TOKEN.finditer(source)
            if match.lastgroup not in {"space", "comment"}
            and not (match.group() == "\\" and
                     source[match.end():].startswith(("\n", "\r\n")))]


def token_values(source: str) -> list[str]:
    """実装不変の比較に使うtoken列を返す。"""
    return [match.group() for match in tokens(source)]


def directive_positions(source: str) -> set[int]:
    """継続行を含むpreprocessor領域を機械整形から保護する。"""
    positions: set[int] = set()
    continued = False
    offset = 0
    for line in source.splitlines(keepends=True):
        if continued or line.lstrip().startswith("#"):
            positions.update(range(offset, offset + len(line)))
            continued = line.rstrip().endswith("\\")
        offset += len(line)
    return positions


def prescribed_spaces(source: str) -> str:
    """プリプロセッサ・literal・for区切りを保護して指定空白だけを補う。"""
    items = tokens(source)
    stack: list[tuple[int, bool]] = []
    edits: list[tuple[int, int, str]] = []
    directive_lines = directive_positions(source)

    for index, item in enumerate(items):
        value = item.group()
        previous = items[index - 1] if index else None
        if value == "(":
            is_for = previous is not None and previous.group() == "for"
            stack.append((index, is_for))
            if index + 1 >= len(items) or items[index + 1].group() == ")":
                continue
            if item.start() in directive_lines or previous is None:
                continue
            prev = previous.group()
            # clang-formatが離して置いた比較演算子をtemplate呼出しと誤認しない。
            callable_paren = (
                re.fullmatch(r"[A-Za-z_]\w*", prev) is not None
                and prev not in {"return", "co_return", "throw", "case"}
            ) or (
                prev in {">", ">>", ")", "]"}
                and previous.end() == item.start()
            ) or (
                index >= 2 and items[index - 2].group() == "operator"
            )
            if callable_paren:
                end = item.end()
                while end < len(source) and source[end] in " \t":
                    end += 1
                # 改行直後へ余分な空白を入れず、既存の継続行配置を維持する。
                if end < len(source) and source[end] not in "\r\n":
                    edits.append((item.end(), end, " "))
        elif value == ")":
            if stack:
                stack.pop()
        elif value == ";" and item.start() not in directive_lines:
            if any(is_for for _, is_for in stack):
                continue
            start = item.start()
            while start > 0 and source[start - 1] in " \t":
                start -= 1
            if start > 0 and source[start - 1] not in "\r\n":
                edits.append((start, item.start(), " "))

    for start, end, replacement in reversed(edits):
        source = source[:start] + replacement + source[end:]
    return source


def prescribed_breaks(source: str) -> str:
    """連続構造を保護して、block後とcase境界の独立処理を空行で区切る。"""
    items = tokens(source)
    directives = directive_positions(source)
    edits: set[int] = set()
    for index, item in enumerate(items[:-1]):
        if item.start() in directives:
            continue
        value = item.group()
        following = items[index + 1]
        next_value = following.group()
        block_end = value == "}" and next_value not in {
            "}", ";", ",", ")", "]", "else", "catch", "while",
        }
        case_end = (value == ";" and index > 0 and items[index - 1].group() == "break"
                    and next_value in {"case", "default"})
        if not block_end and not case_end:
            continue
        newline = source.find("\n", item.end())
        if newline < 0 or following.start() < newline:
            continue
        # 行末commentはそのまま保持し、次行の説明を独立処理側へ付ける。
        tail = source[item.end():newline].strip()
        if tail and not tail.startswith("//"):
            continue
        if re.match(r"[\t \r]*\n", source[newline + 1:]):
            continue
        edits.add(newline + 1)
    for position in sorted(edits, reverse=True):
        source = source[:position] + "\n" + source[position:]
    return source


def prescribed_comment_indent(source: str) -> str:
    """file版や説明本文を変えず、Doxygenのstarを開始行のindentへそろえる。"""
    edits = []
    for item in TOKEN.finditer(source):
        if item.lastgroup != "comment" or not item.group().startswith("/**"):
            continue
        if "\n" not in item.group():
            continue
        line_start = source.rfind("\n", 0, item.start()) + 1
        indent = source[line_start:item.start()]
        if indent.strip():
            continue
        lines = item.group().splitlines(keepends=True)
        normalized = lines[0] + "".join(
            indent + " " + line.lstrip("\t ") if line.lstrip("\t ").startswith("*") else line
            for line in lines[1:])
        if normalized != item.group():
            edits.append((item.start(), item.end(), normalized))
    for start, end, replacement in reversed(edits):
        source = source[:start] + replacement + source[end:]
    return source


def find_formatter(explicit: str | None) -> str:
    """指定・環境変数・PATH・既存開発toolの順でformatterを解決する。"""
    candidates = [explicit, os.environ.get("CLANG_FORMAT"), shutil.which("clang-format")]
    if os.name == "nt":
        candidates.append("C:/develop/tools/Microsoft/VC/Tools/Llvm/x64/bin/clang-format.exe")
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("clang-formatを指定してください（--clang-formatまたはCLANG_FORMAT）。")


def format_text(source: str, path: Path, formatter: str) -> str:
    """基本整形と規約補正の前後でtokenとliteralの不変を確認する。"""
    result = subprocess.run(
        [formatter, "--style=file", f"--assume-filename={path}"],
        input=source.encode("utf-8"), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=True, cwd=ROOT,
    )
    formatted = prescribed_comment_indent(
        prescribed_breaks(prescribed_spaces(result.stdout.decode("utf-8"))))
    if token_values(source) != token_values(formatted):
        raise RuntimeError(f"token変更を検出したため整形を拒否しました: {path.relative_to(ROOT)}")
    return formatted


def main() -> int:
    """自作C++だけを整形し、--checkでは差分の存在を失敗として返す。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--clang-format")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    formatter = find_formatter(args.clang_format)
    paths = ([ROOT / path for path in args.paths] if args.paths else
             sorted(path for root in (ROOT / "src", ROOT / "tests")
                    for path in root.rglob("*") if path.suffix in {".h", ".cpp"}))
    changed = []
    for path in paths:
        path = path.resolve()
        if not path.is_relative_to(ROOT) or path.suffix not in {".h", ".cpp"}:
            raise RuntimeError("対象はrepository配下の自作.h／.cppだけに限定します。")
        original = path.read_text(encoding="utf-8")
        formatted = format_text(original, path, formatter)
        if original != formatted:
            changed.append(str(path.relative_to(ROOT)))
            if not args.check:
                path.write_text(formatted, encoding="utf-8", newline="\n")
    print(f"{len(paths)} files: {len(changed)} {'nonconforming' if args.check else 'formatted'}.")
    if args.check:
        for path in changed:
            print(path)
    return 1 if args.check and changed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        raise SystemExit(2) from error
