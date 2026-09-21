"""只读导入审查；输出包含私人路径，仅写目标 out。"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

ROOTS = {"engine", "samples", "shaders", "assets", "tools", "tests", "cmake"}
ROOT_FILES = {"CMakeLists.txt", "CMakePresets.json", ".clang-format", ".editorconfig", ".gitattributes", ".gitignore"}
SKIP = {".git", "out", "build", "artifacts", "dist", ".vs", ".agents", ".codex", ".workbuddy-ai"}
PATTERNS = {
    "private_source_url": re.compile(r"github\.com/xiaoran6657/MiniEngine", re.I),
    "absolute_path": re.compile(r"(?<![A-Za-z])[A-Za-z]:[\\/]"),
    "private_docs": re.compile(r"(?:\.agents/|\.codex/|docs/(?:mvp|learning|verification|performance)/)"),
    "credential_marker": re.compile(r"(?:-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[0-9A-Z]{16})"),
}

def linked(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)

def safe_root(path):
    path = path.absolute()
    for part in [path, *path.parents]:
        if linked(part):
            raise ValueError(f"reparse point in root: {part}")
    return path

def walk(root):
    """先 lstat 再进入目录，不跟随 junction 或其他 reparse point。"""
    for entry in sorted(os.scandir(root), key=lambda e: e.name):
        path = Path(entry.path)
        if linked(path):
            yield path, "reparse"
        elif entry.is_dir(follow_symlinks=False):
            if entry.name in SKIP:
                yield path, "excluded_directory"
            else:
                yield from walk(path)
        elif entry.is_file(follow_symlinks=False):
            yield path, "file"
        else:
            yield path, "special"

def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], encoding="utf-8").strip()

def source_file(root, relative):
    path = root
    for component in Path(relative).parts:
        path = path / component
        if not path.exists() or linked(path):
            return None
    return path if path.is_file() else None

def inventory(source, target):
    source, target = safe_root(source), safe_root(target)
    if source == target or source in target.parents or target in source.parents:
        raise ValueError("source and target must be separate non-nested roots")
    tracked = set(git(source, "ls-files", "-z").split("\0"))
    expected = {p for p in tracked if p in ROOT_FILES or p.split("/")[0] in ROOTS}
    records, findings, excluded = [], [], []
    for path, kind in walk(target):
        relative = path.relative_to(target).as_posix()
        if kind != "file":
            excluded.append({"path": relative, "kind": kind})
            continue
        original = source_file(source, relative)
        row = {"sourcePath": relative if original else None, "targetPath": relative,
               "category": relative.split("/")[0], "size": path.stat().st_size,
               "sourceSha256": sha(original) if original else None, "targetSha256": sha(path),
               "sourceTracked": relative in tracked, "decision": "pending", "reason": "requires review"}
        row["comparison"] = ("identical" if row["sourceSha256"] == row["targetSha256"] else "changed") if original else "target_only"
        if "__pycache__" in path.parts or path.suffix.lower() in {".pyc", ".pyo", ".log", ".pdb", ".rdc", ".wpix"}:
            row.update(decision="excluded", reason="local cache/output; retained on disk")
        records.append(row)
        if path.stat().st_size > 8_000_000 or row["decision"] == "excluded":
            continue
        try:
            content = path.read_text(encoding="utf-8-sig")
        except (UnicodeError, OSError):
            continue
        for number, line in enumerate(content.splitlines(), 1):
            for rule, pattern in PATTERNS.items():
                if pattern.search(line):
                    findings.append({"path": relative, "line": number, "rule": rule, "text": line[:500]})
    present = {r["targetPath"] for r in records}
    return {"schemaVersion": 1, "sourceRoot": str(source), "targetRoot": str(target),
            "sourceCommit": git(source, "rev-parse", "HEAD"),
            "sourceStatus": git(source, "status", "--porcelain", "-uall"),
            "files": records, "excludedBoundaries": excluded,
            "missingCopiedFiles": sorted(expected - present), "findings": findings,
            "note": "Pattern scan is triage, not proof of absence of secrets or permission to publish."}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--target", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    target = safe_root(args.target)
    output = args.output.absolute()
    if ".." in output.parts or not output.is_relative_to(target / "out"):
        parser.error("output must be inside target/out")
    # 先检查已有祖先，再创建目录，避免 mkdir 穿过重解析点。
    for ancestor in [output.parent, *output.parent.parents]:
        if ancestor.exists() and linked(ancestor):
            parser.error("output ancestry contains a reparse point")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and linked(output):
        parser.error("output must not be a reparse point")
    report = inventory(args.source, target)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"files": len(report["files"]), "missing": report["missingCopiedFiles"],
                      "findings": len(report["findings"]), "excludedBoundaries": report["excludedBoundaries"]}))

if __name__ == "__main__":
    main()

