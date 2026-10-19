#!/usr/bin/env python3
"""自作C++の局所callback契約とDoxygen XMLをオフラインで監査する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0

literal/commentを保持する既存formatterのlexerを使う。完全なC++ ASTではないため、
未対応構文や文面の意味はレビューで補う。実設定、RDP、ネットワークへ接続しない。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from format_source import ROOT, token_values, tokens

WORDS = re.compile(r"[A-Za-z_]\w*\Z")
QUALIFIERS = {"const", "volatile", "auto", "unsigned", "signed", "long", "short"}
BUILTINS = {"void", "bool", "int", "char", "wchar_t", "float", "double"}
DOC = re.compile(r"/\*\*[\s\S]*?\*/")


def closing(items: list, begin: int, left: str, right: str) -> int | None:
    """同種delimiterの対応位置を有限token列から返す。"""
    depth = 0
    for index in range(begin, len(items)):
        value = items[index].group()
        if value == left:
            depth += 1
        elif value == right:
            depth -= 1
            if depth == 0:
                return index
    return None


def parameters(items: list) -> list[dict]:
    """callbackの単純な仮引数を分割する。無名引数は名前を推測しない。"""
    groups: list[list] = [[]]
    depth = 0
    for item in items:
        value = item.group()
        if value in {"<", "(", "["}:
            depth += 1
        elif value in {">", ")", "]"}:
            depth -= 1
        elif value == ">>":
            depth -= 2
        if value == "," and depth == 0:
            groups.append([])
        else:
            groups[-1].append(item)
    result = []
    for group in groups:
        if not group:
            continue
        values = [item.group() for item in group]
        if "=" in values:
            values = values[:values.index("=")]
        words = [(index, value) for index, value in enumerate(values) if WORDS.fullmatch(value)]
        name = None
        if words:
            index, last = words[-1]
            if (len(words) > 1 and last not in QUALIFIERS | BUILTINS
                    and not last.isupper() and (index == 0 or values[index - 1] != "::")):
                name = last
        result.append({"name": name, "declaration": " ".join(values)})
    return result


def local_contract(source: str, position: int) -> tuple[str, tuple[int, int] | None]:
    """直前の局所契約だけを選び、母関数のcommentを代用しない。"""
    docs = list(DOC.finditer(source, 0, position))
    if not docs:
        return "", None
    doc = docs[-1]
    tail = source[doc.end():position]
    if any(item.group() in {"{", "}", ";"} for item in tokens(tail)):
        return "", None
    return doc.group(), (doc.start(), doc.end())


def lambda_contracts(source: str) -> list[dict]:
    """capture、引数、bodyのdelimiterから局所lambdaと説明不足候補を抽出する。"""
    items = tokens(source)
    result = []
    for begin, item in enumerate(items):
        if item.group() != "[":
            continue
        previous = items[begin - 1].group() if begin else ""
        if WORDS.fullmatch(previous) and previous not in {"return", "co_return"}:
            continue
        capture_end = closing(items, begin, "[", "]")
        if capture_end is None or capture_end - begin > 64:
            continue
        current = capture_end + 1
        parameter_items = []
        if current < len(items) and items[current].group() == "(":
            end = closing(items, current, "(", ")")
            if end is None:
                continue
            parameter_items = items[current + 1:end]
            current = end + 1
        while current < len(items) and items[current].group() in {"mutable", "noexcept"}:
            current += 1
            if current < len(items) and items[current].group() == "(":
                end = closing(items, current, "(", ")")
                if end is None:
                    break
                current = end + 1
        if current < len(items) and items[current].group() == "->":
            current += 1
            while current < len(items) and items[current].group() not in {"{", ";"}:
                current += 1
        if current >= len(items) or items[current].group() != "{":
            continue
        body_end = closing(items, current, "{", "}")
        if body_end is None:
            continue
        position = item.start()
        doc, span = local_contract(source, position)
        params = parameters(parameter_items)
        documented = set(re.findall(r"@param\[(?:in|out|in,out)\]\s+(\w+)", doc))
        missing_params = [param["name"] for param in params
                          if param["name"] and param["name"] not in documented]
        returns_value = any(
            items[index].group() == "return" and items[index + 1].group() != ";"
            for index in range(current + 1, body_end - 1)
        )
        issues = []
        if "@brief" not in doc:
            issues.append("missing_brief")
        if missing_params:
            issues.append("missing_named_parameters")
        if returns_value and "@return" not in doc and "@retval" not in doc:
            issues.append("missing_return")
        if any(param["name"] is None for param in params) and "@note" not in doc:
            issues.append("missing_unnamed_parameter_note")
        result.append({
            "line": source.count("\n", 0, position) + 1,
            "start": position,
            "body_start": items[current].start(),
            "end": items[body_end].end(),
            "signature": source[position:items[current].start()].strip(),
            "parameters": params,
            "returns_value": returns_value,
            "doc_span": span,
            "issues": issues,
            "missing_parameters": missing_params,
        })
    return result


def xml_issues(directory: Path) -> list[dict]:
    """実宣言へ照合し、自身の関数契約とfield/enumの直前形式を検査する。"""
    result = []
    cache = {}
    seen = set()
    for path in directory.glob("*.xml"):
        if path.name == "index.xml":
            continue
        root = ET.parse(path).getroot()
        for member in root.findall(".//memberdef[@kind='function']"):
            location = member.find("location")
            if location is None:
                continue
            file = location.get("file", "")
            line = int(location.get("line", "0"))
            name = member.findtext("name", "")
            key = (file, line, name)
            if not file.startswith(("src/", "tests/")) or key in seen:
                continue
            source = cache.setdefault(file, (ROOT / file).read_text(encoding="utf-8"))
            lines = source.splitlines()
            if not (0 < line <= len(lines)) or name not in lines[line - 1]:
                continue
            seen.add(key)
            brief = "".join(member.find("briefdescription").itertext()).strip()
            if not brief:
                result.append({"file": file, "line": line, "name": name,
                               "issue": "missing_own_function_contract"})
                continue
            parameter_names = {node.text for node in member.findall("param/declname") if node.text}
            documented = {node.text for node in member.findall(
                ".//parameterlist[@kind='param']//parametername") if node.text}
            missing = sorted(parameter_names - documented)
            if missing:
                result.append({"file": file, "line": line, "name": name,
                               "issue": "missing_named_parameters", "parameters": missing})
            invalid = [node.text for node in member.findall(
                ".//parameterlist[@kind='param']//parametername") if not node.get("direction")]
            if invalid:
                result.append({"file": file, "line": line, "name": name,
                               "issue": "missing_parameter_direction", "parameters": invalid})
            return_type = "".join(member.find("type").itertext()).strip()
            is_void = re.fullmatch(r"(?:(?:constexpr|static|inline)\s+)*void", return_type)
            has_return = (member.find(".//simplesect[@kind='return']") is not None
                          or member.find(".//parameterlist[@kind='retval']") is not None)
            if return_type and not is_void and not has_return:
                result.append({"file": file, "line": line, "name": name,
                               "issue": "missing_return"})
        for compound in root.findall(".//compounddef"):
            for member in compound.findall("sectiondef/memberdef"):
                kind = member.get("kind")
                if kind not in {"variable", "enum"}:
                    continue
                if kind == "variable" and compound.get("kind") not in {"class", "struct", "union"}:
                    continue
                location = member.find("location")
                if location is None:
                    continue
                file = location.get("file", "")
                line = int(location.get("line", "0"))
                name = member.findtext("name", "")
                key = (file, line, name, kind)
                if not file.startswith(("src/", "tests/")) or key in seen:
                    continue
                source = cache.setdefault(file, (ROOT / file).read_text(encoding="utf-8"))
                lines = source.splitlines()
                if not (0 < line <= len(lines)) or name not in lines[line - 1]:
                    continue
                seen.add(key)
                targets = [(line - 1, name)]
                if kind == "enum":
                    targets = []
                    end = line
                    while end < len(lines) and not re.match(r"\s*}\s*;", lines[end]):
                        end += 1
                    for value in member.findall("enumvalue"):
                        value_name = value.findtext("name", "")
                        for index in range(line, end):
                            if re.match(r"\s*" + re.escape(value_name) + r"\s*(?:=|,|$)", lines[index]):
                                targets.append((index, value_name))
                                break
                for index, target_name in targets:
                    previous = index - 1
                    while previous >= 0 and not lines[previous].strip():
                        previous -= 1
                    if previous < 0 or not re.match(r"\s*///(?!<)\s*\S", lines[previous]):
                        result.append({"file": file, "line": index + 1, "name": target_name,
                                       "issue": "missing_preceding_member_comment"})
    return result


def main() -> int:
    """全自作C++の候補をJSONへ出力し、契約不足では非0終了する。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml-dir", type=Path, help="INHERIT_DOCS=NOで生成したXML directory")
    parser.add_argument("--inventory", action="store_true", help="契約ありのlambdaも表示")
    parser.add_argument("--compare-tree", type=Path, help="変更前snapshotとC++ token列を比較")
    args = parser.parse_args()
    inventory = []
    token_mismatches = []
    compared = 0
    for base in (ROOT / "src", ROOT / "tests"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".h", ".cpp"}:
                continue
            source = path.read_text(encoding="utf-8")
            if args.compare_tree:
                original = args.compare_tree / path.relative_to(ROOT)
                if original.is_file():
                    compared += 1
                    if token_values(source) != token_values(original.read_text(encoding="utf-8")):
                        token_mismatches.append(path.relative_to(ROOT).as_posix())
            for item in lambda_contracts(source):
                if args.inventory or item["issues"]:
                    inventory.append({"file": path.relative_to(ROOT).as_posix(), **item})
    xml = xml_issues(args.xml_dir) if args.xml_dir else []
    print(json.dumps({"lambda_contracts": inventory, "xml_issues": xml,
                      "compared_files": compared, "token_mismatches": token_mismatches},
                     ensure_ascii=False, indent=2))
    return 1 if xml or token_mismatches or any(item["issues"] for item in inventory) else 0


if __name__ == "__main__":
    raise SystemExit(main())
