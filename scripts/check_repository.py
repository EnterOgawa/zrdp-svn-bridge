#!/usr/bin/env python3
"""公開treeのlicense・版・file種別・リンク・CI境界をofflineで検査する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0
値を検出しても内容・資格情報を表示せず、fileと分類だけを返す。
履歴・binaryの完全secret scanや法的な権利認定の代わりではない。
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
APACHE_SHA256 = "50e6751797c50dedd75ef1b8a0d9e42f5f8472e9fbce91f34718e9f97b0c780a"
FORBIDDEN_EXTENSIONS = {".exe", ".dll", ".pdb", ".pfx", ".p12", ".key", ".pem", ".dmp", ".pcap", ".rdp"}
FORBIDDEN_NAMES = {"agent.ini", "client.ini", "connection-history.bin", "agent-settings.bin"}
SECRETS = re.compile(
    r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----|"
    r"\bgh[pousr]_[A-Za-z0-9]{30,}\b|\bgithub_pat_[A-Za-z0-9_]{30,}\b|"
    r"\bAKIA[0-9A-Z]{16}\b|\bxox[baprs]-[A-Za-z0-9-]{20,}\b"
)
SOURCE_SUFFIXES = {".cpp", ".h", ".rc", ".py", ".ps1"}
MD_LINK = re.compile(r'!?\[[^\]\n]*\]\(([^)\n]+)\)|<img\b[^>]*\bsrc="([^"]+)"')


def repository_paths(root: Path) -> list[Path]:
    """Git管理fileと追加候補だけを読み、生成物はwalkへ混ぜない。"""
    if (root / ".git").exists():
        data = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
        )
        return sorted({root / p for p in data.decode("utf-8").split("\0") if p})
    ignored = {".git", "out", "build", "work", ".vs", "__pycache__"}
    return sorted(p for p in root.rglob("*") if p.is_file() and not ignored.intersection(p.relative_to(root).parts))


def file_issues(path: str, data: bytes) -> list[str]:
    """機密／生成物のfile名、既知secret形式、自作codeの権利表示を確認する。"""
    name = Path(path)
    issues = []
    if name.suffix.lower() in FORBIDDEN_EXTENSIONS or name.name.lower() in FORBIDDEN_NAMES or name.name.startswith(".env"):
        issues.append("forbidden-file")
    if any(part in {"work", "build", "out", ".vs"} for part in name.parts):
        issues.append("generated-or-private-directory")
    if b"\x00" in data:
        return issues
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return issues + ["non-utf8-text"]
    if SECRETS.search(text):
        issues.append("secret-pattern")
    if name.suffix in SOURCE_SUFFIXES and ("SPDX-License-Identifier: Apache-2.0" not in text or
            "Copyright (c) 2026 EnterSystem CO.,LTD." not in text):
        issues.append("missing-own-license-header")
    return issues


def missing_links(root: Path, path: Path, text: str) -> list[str]:
    """Markdownの相対fileリンクを検査し、外部URLとanchor-onlyは通信しない。"""
    text = re.sub(r"(?ms)^```[^\n]*\n.*?^```[ \t]*$", "", text)
    missing = []
    for match in MD_LINK.finditer(text):
        target = (match.group(1) or match.group(2)).strip().split(" \"", 1)[0].strip("<>")
        if target.startswith(("#", "http:", "https:", "mailto:")):
            continue
        target = unquote(target.split("#", 1)[0])
        candidate = (root / target.lstrip("/")) if target.startswith("/") else (path.parent / target)
        candidate = candidate.resolve()
        if not candidate.is_relative_to(root.resolve()) or not candidate.exists():
            missing.append(target)
    return missing


def workflow_issues(text: str) -> list[str]:
    """承認されたCI以外の権限・credential・uploadを拒否する。"""
    issues = []
    uses = re.findall(r"^\s*uses:\s*([^\s#]+)", text, re.M)
    if not uses or any(not re.fullmatch(r"actions/checkout@[0-9a-f]{40}", item) for item in uses):
        issues.append("action-not-fixed-official-checkout")
    if not re.search(r"^permissions:\s*\n\s+contents: read\s*$", text, re.M):
        issues.append("unexpected-ci-permissions")
    if re.search(r"^\s*(?:[a-z-]+):\s*write\s*$", text, re.M):
        issues.append("ci-write-permission")
    if "persist-credentials: false" not in text:
        issues.append("persistent-ci-credential")
    for token in ("pull_request_target", "secrets.", "upload-artifact", "gh release", "signtool"):
        if token in text:
            issues.append("ci-forbidden-operation")
    if "runs-on: windows-2022" not in text or "timeout-minutes:" not in text:
        issues.append("unbounded-or-unexpected-ci-runner")
    return sorted(set(issues))


def audit(root: Path) -> dict:
    """現在treeだけを読み、実設定・network・binary実行へ触れず結果を集める。"""
    issues = []
    paths = repository_paths(root)
    for path in paths:
        if not path.is_file():
            issues.append({"path": str(path.relative_to(root)), "kind": "tracked-file-missing"})
            continue
        relative = path.relative_to(root).as_posix()
        data = path.read_bytes()
        issues.extend({"path": relative, "kind": kind} for kind in file_issues(relative, data))
        if path.suffix == ".md":
            issues.extend({"path": relative, "kind": "missing-local-link", "target": t}
                          for t in missing_links(root, path, data.decode("utf-8-sig")))
        if relative.startswith(".github/workflows/"):
            issues.extend({"path": relative, "kind": kind} for kind in workflow_issues(data.decode("utf-8-sig")))
    license_text = (root / "LICENSE").read_text(encoding="utf-8-sig").replace("\r\n", "\n").strip() + "\n"
    if hashlib.sha256(license_text.encode()).hexdigest() != APACHE_SHA256:
        issues.append({"path": "LICENSE", "kind": "canonical-apache-license-mismatch"})
    for name in ("NOTICE", "SECURITY.md", "CONTRIBUTING.md", "docs/dependencies.md", "docs/publication-checklist.md"):
        if not (root / name).is_file():
            issues.append({"path": name, "kind": "missing-publication-document"})
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"\bVERSION\s+(\d+\.\d+\.\d+)", cmake)
    if match:
        version = match.group(1) + ".0"
        tuple_value = ",".join(version.split("."))
        for role, filename in (("client", "ZRdpSvnBridge"), ("agent", "ZRdpSvnAgent")):
            path = root / "src" / role / "app" / (filename + ".rc")
            resource = path.read_text(encoding="utf-8")
            if f"FILEVERSION {tuple_value}" not in resource or f"PRODUCTVERSION {tuple_value}" not in resource or version not in resource:
                issues.append({"path": path.relative_to(root).as_posix(), "kind": "version-resource-mismatch"})
            manifest = path.with_suffix(".manifest")
            identity = ET.parse(manifest).getroot().find("{urn:schemas-microsoft-com:asm.v1}assemblyIdentity")
            if identity is None or identity.attrib.get("version") != version:
                issues.append({"path": manifest.relative_to(root).as_posix(), "kind": "manifest-version-mismatch"})
    else:
        version = "unknown"
        issues.append({"path": "CMakeLists.txt", "kind": "missing-product-version"})
    return {"checked_files": len(paths), "product_version": version, "issues": issues}


def main() -> int:
    """検出内容ではなく分類だけをJSONで表示する。"""
    result = audit(ROOT)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return bool(result["issues"])


if __name__ == "__main__":
    raise SystemExit(main())
