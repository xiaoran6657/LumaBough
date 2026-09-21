"""验证 import 阶段的实际文件集合；不做发布验收或自动批准。"""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path, PurePosixPath
import re
import sys
sys.dont_write_bytecode = True
from audit_import import git, linked, safe_root, sha, walk, source_file

MANIFEST = "docs/evidence/PUBLICATION-FILES.json"
HEX = re.compile(r"^[0-9a-fA-F]{64}$")

def relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError(f"invalid relative path: {value!r}")
    p = PurePosixPath(value)
    if p.is_absolute() or ".." in p.parts or str(p) != value:
        raise ValueError(f"unsafe relative path: {value!r}")
    return value

def validate_tree(root, manifest):
    errors = []
    if manifest.get("schemaVersion") != 2 or manifest.get("stage") not in {"import", "entry"}:
        return ["unsupported manifest schema/stage"]
    if manifest.get("selfExcludedPath") != MANIFEST:
        errors.append("manifest self-exclusion must be explicit")
    indexed = {}
    for group in ("files", "pending", "excluded"):
        for row in manifest.get(group, []):
            path = relative(row["path"])
            if path == MANIFEST or path in indexed:
                errors.append(f"duplicate/self entry: {path}")
            if not HEX.fullmatch(row.get("sha256", "")) or not isinstance(row.get("size"), int):
                errors.append(f"invalid file identity: {path}")
            if not row.get("reason") or not row.get("category"):
                errors.append(f"missing classification: {path}")
            if row.get("sourcePath") is not None:
                relative(row["sourcePath"])
            if row.get("sourceSha256") is not None and not HEX.fullmatch(row["sourceSha256"]):
                errors.append(f"invalid source hash: {path}")
            indexed[path] = (group, row)
    boundaries = {relative(p) for p in manifest.get("localOnlyRoots", [])}
    actual = {}
    for path, kind in walk(root):
        name = path.relative_to(root).as_posix()
        if kind == "excluded_directory":
            if name not in boundaries:
                errors.append(f"unclassified directory: {name}")
            continue
        if kind != "file":
            errors.append(f"unsafe filesystem entry: {name} ({kind})")
            continue
        if name != MANIFEST:
            actual[name] = path
    for name in sorted(set(actual) - set(indexed)):
        # 新产生的缓存仍不能自动进入允许集合；已知缓存也逐文件绑定。
        errors.append(f"unclassified file: {name}")
    for name in sorted(set(indexed) - set(actual)):
        errors.append(f"missing classified file: {name}")
    for name in sorted(set(indexed) & set(actual)):
        _, row = indexed[name]
        if actual[name].stat().st_size != row["size"] or sha(actual[name]) != row["sha256"]:
            errors.append(f"hash/size mismatch: {name}")
    if manifest.get("publicationStatus") != "BLOCKED":
        errors.append("import snapshot must not assert publication readiness")
    return errors

def validate_evidence(root):
    errors = []
    manifest_path = source_file(root, "docs/evidence/EVIDENCE-MANIFEST.json")
    if manifest_path is None:
        return ["missing/linked evidence manifest"]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 2 or manifest.get("scope") != "Core" or manifest.get("supportedBackends") != ["d3d11", "d3d12"]:
        errors.append("invalid evidence scope")
    for key in ("sourceCommit", "runtimeCommit", "publicationCommit"):
        value = manifest.get(key)
        if value is not None and not re.fullmatch(r"[0-9a-f]{40}", value):
            errors.append(f"invalid commit: {key}")
    for artifact in manifest.get("artifacts", []):
        path = source_file(root, relative(artifact["publicPath"]))
        if path is None or sha(path) != artifact["sha256"] or path.stat().st_size != artifact["bytes"]:
            errors.append(f"artifact mismatch: {artifact['artifactId']}")
    ledger_path = source_file(root, "docs/evidence/CLAIM-LEDGER.csv")
    if ledger_path is None:
        return errors + ["missing/linked ledger"]
    with ledger_path.open(encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        required = {"claim_id","public_claim","scope","source_kind","source_commit","evidence_record_commit",
                    "runtime_commit","evidence_path","public_url","producer_command","artifact_sha256",
                    "evidence_status","access_status","notes"}
        if not required.issubset(reader.fieldnames or []):
            return errors + ["missing V2 ledger columns"]
        rows = list(reader)
    if {r["claim_id"] for r in rows} != {f"C-M9-{i:03}" for i in range(1,8)} or len(rows) != 7:
        errors.append("unexpected/duplicate claim IDs")
    for row in rows:
        if row["source_kind"] not in {"historical","current"} or row["evidence_status"] not in {"PASS","FAIL","BLOCKED","N/A"} or row["access_status"] not in {"LOCAL","BLOCKED","VERIFIED"}:
            errors.append(f"invalid state: {row['claim_id']}")
        if row["evidence_status"] == "N/A" and row["scope"] != "Extended":
            errors.append("only Extended may be N/A")
        for col in ("source_commit","evidence_record_commit","runtime_commit"):
            if row[col] and not re.fullmatch(r"[0-9a-f]{40}", row[col]):
                errors.append(f"invalid ledger commit: {row['claim_id']}/{col}")
        if row["evidence_path"]:
            p = source_file(root, relative(row["evidence_path"]))
            if p is None or not HEX.fullmatch(row["artifact_sha256"]) or sha(p) != row["artifact_sha256"]:
                errors.append(f"ledger artifact mismatch: {row['claim_id']}")
        elif row["evidence_status"] == "PASS":
            errors.append("PASS requires artifact")
        if row["evidence_status"] == "PASS" and not row["producer_command"]:
            errors.append("PASS requires producer command")
        if row["public_url"] or row["access_status"] == "VERIFIED":
            # 本工具只实现本地 import，不能核验匿名访问。
            errors.append("public access requires a later published-stage validator")
    return errors

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=["import"], required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        root = safe_root(args.root)
        if Path(git(root, "rev-parse", "--show-toplevel")).resolve() != root.resolve():
            raise ValueError("not an independent Git root")
        manifest_path = source_file(root, MANIFEST)
        if manifest_path is None:
            raise ValueError("missing/linked publication manifest")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors = validate_tree(root, data) + validate_evidence(root)
        if data.get("stage") != args.stage:
            errors.append("use validate_publication.py --stage entry for an entry snapshot")
    except (ValueError, KeyError, OSError) as e:
        errors = [str(e)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"PASS import: allowed={len(data['files'])}, pending={len(data['pending'])}, excluded={len(data['excluded'])}; publication=BLOCKED")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
