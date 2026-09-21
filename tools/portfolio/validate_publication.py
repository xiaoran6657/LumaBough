"""验证本地公开入口、逐文件清单与来源；不实现远程发布验收。"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit
sys.dont_write_bytecode = True
from audit_import import safe_root, source_file, sha
from validate_import import MANIFEST, relative, validate_tree, validate_evidence

def links(root, files):
    errors = []
    for name in files:
        if not name.endswith(".md"):
            continue
        content = (root / name).read_text(encoding="utf-8-sig")
        content = re.sub(r"(?ms)^\x60{3}.*?^\x60{3}[^\n]*", "", content)
        for target in re.findall(r"!?\[[^\]]*\]\(([^)\n]+)\)", content):
            target = target.strip().split(' "')[0].strip("<>")
            url = urlsplit(target)
            if url.scheme in {"http", "https", "mailto"}:
                continue
            if url.scheme or target.startswith(("/", "\\")):
                errors.append(f"{name}: nonportable link {target}")
                continue
            path = (root / name).parent / unquote(url.path) if url.path else root / name
            resolved = path.resolve()
            if not resolved.is_relative_to(root.resolve()):
                errors.append(f"{name}: escaping link {target}")
                continue
            name_in_root = resolved.relative_to(root.resolve()).as_posix()
            allowed = name_in_root == MANIFEST or name_in_root in files or (resolved.is_dir() and any(n.startswith(name_in_root + "/") for n in files))
            if not allowed:
                errors.append(f"{name}: link outside reviewed file set {target}")
            if not resolved.exists():
                errors.append(f"{name}: missing link {target}")
            elif url.fragment and resolved.suffix == ".md":
                headings = []
                for line in resolved.read_text(encoding="utf-8-sig").splitlines():
                    if line.startswith("#"):
                        heading = re.sub(r"[^\w\- ]", "", line.lstrip("# ").strip().lower())
                        headings.append(heading.replace(" ", "-"))
                if unquote(url.fragment) not in headings:
                    errors.append(f"{name}: missing anchor {target}")
    return errors

def validate_entry(root, data):
    errors = validate_tree(root, data) + validate_evidence(root)
    if data.get("stage") != "entry" or data.get("pending") or data.get("excluded"):
        errors.append("entry requires reviewed files and no pending/excluded files in the public tree")
    names = {row["path"] for row in data.get("files", [])}
    for required in ("README.md", "LICENSE", "THIRD-PARTY-NOTICES.md", "docs/DEVELOPMENT.md",
                     "docs/architecture/context.svg", "docs/architecture/frame.svg",
                     "docs/architecture/assets.svg", "docs/portfolio/media/render-preview.png"):
        if required not in names:
            errors.append(f"missing entry deliverable: {required}")
    errors += links(root, names)
    sources = json.loads((root / "docs/evidence/THIRD-PARTY-SOURCES.json").read_text(encoding="utf-8"))
    for row in sources["items"] + sources["notices"]:
        path = source_file(root, relative(row["path"]))
        if path is None or sha(path) != row["sha256"]:
            errors.append(f"third-party identity mismatch: {row['path']}")
    diagram_source = sha(root / "docs/architecture/diagrams.json")
    for row in json.loads((root / "docs/architecture/exports.json").read_text(encoding="utf-8")):
        path = source_file(root, relative(row["path"]))
        if path is None or sha(path) != row["sha256"] or row["sourceSha256"] != diagram_source:
            errors.append("stale architecture export: " + row["path"])
    return errors

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, choices=["entry"])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        root = safe_root(args.root)
        data = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
        errors = validate_entry(root, data)
    except (ValueError, KeyError, OSError) as exc:
        errors = [str(exc)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"PASS entry: {len(data['files'])} reviewed files; publication=BLOCKED")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
