"""验证本地公开入口、逐文件清单与来源；candidate 阶段额外核对包字节、E3 隐私门与 E4 记录。"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile
from urllib.parse import unquote, urlsplit
sys.dont_write_bytecode = True
from audit_import import safe_root, source_file, sha
from validate_import import MANIFEST, relative, validate_tree, validate_evidence

E4_RESULTS = "docs/evidence/E4-RESULTS.json"
PRIVACY_BASELINE = "docs/evidence/PRIVACY-DISPOSITIONS.json"
PRIVACY_SCANNER = "tools/portfolio/scan_privacy.py"
# 构建敏感输入：改动这些必须在 builtAtCommit 之后重建（文档与 portfolio 工具不影响 EXE/场景）。
# tools/benchmark 链接进 EXE（MiniEngineBenchmark）、tools/assets 生成烘焙输入、tools/CMakeLists.txt 决定子目录集合。
BUILD_SENSITIVE_PREFIXES = ("engine/", "samples/", "shaders/", "assets/", "tests/", "cmake/",
                            "tools/shader_compiler/", "tools/asset_cooker/", "tools/benchmark/", "tools/assets/")
BUILD_SENSITIVE_FILES = ("CMakeLists.txt", "CMakePresets.json", "tools/CMakeLists.txt")
# 包内免逐文件校验的清单文件只允许这两个（包自身引用自己不可行）。
SELF_EXCLUDED = ("PACKAGE-MANIFEST.json", "SHA256SUMS.txt")
# E4 第二台机器必须覆盖的六个运行：ID → 期望退出码（正例 0，负例 2）。
E4_REQUIRED_RUNS = (("d3d12-1202", 0), ("d3d11-1202", 0), ("d3d12-cwd-60", 0),
                    ("d3d12-events-1202", 0), ("d3d11-events-1202", 0),
                    ("negative-shaders-renamed", 2))


def build_sensitive(path):
    """该路径是否属于"改了就必须重建"的构建输入。"""
    return path.startswith(BUILD_SENSITIVE_PREFIXES) or path in BUILD_SENSITIVE_FILES

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

def package_bytes(package):
    """包目录或 .zip → {相对路径: 字节}，只读实际交付字节。"""
    if package.is_dir():
        return {p.relative_to(package).as_posix(): p.read_bytes() for p in sorted(package.rglob("*")) if p.is_file()}
    if package.suffix.lower() == ".zip":
        with zipfile.ZipFile(package) as archive:
            return {i.filename: archive.read(i) for i in archive.infolist() if not i.is_dir()}
    raise ValueError("package must be a directory or .zip: " + str(package))


def validate_package_files(files):
    """纯字节核对：manifest / SHA256SUMS / EXE 与实际包内容一致，且没有未列出的文件。"""
    errors = []
    if "PACKAGE-MANIFEST.json" not in files:
        return ["package has no PACKAGE-MANIFEST.json"], None
    manifest = json.loads(files["PACKAGE-MANIFEST.json"].decode("utf-8"))
    sums = {line.split("  ", 1)[1]: line.split("  ", 1)[0]
            for line in files.get("SHA256SUMS.txt", b"").decode("utf-8").splitlines() if "  " in line}
    for name, row in manifest["files"].items():
        payload = files.get(name)
        if payload is None:
            errors.append("manifest lists a missing file: " + name)
            continue
        if len(payload) != row["size"] or hashlib.sha256(payload).hexdigest() != row["sha256"]:
            errors.append("package file does not match its manifest: " + name)
        if sums.get(name) != row["sha256"]:
            errors.append("SHA256SUMS.txt disagrees with the manifest: " + name)
    declared = tuple(manifest.get("selfExcluded", []))
    if declared != SELF_EXCLUDED:
        # 免校验名单由门禁固定，包不能自己扩大（否则任何无哈希文件都能借它绕过）。
        errors.append("selfExcluded must be exactly " + ", ".join(SELF_EXCLUDED) + "; got " + ", ".join(declared))
    for name in files:
        if name not in manifest["files"] and name not in declared:
            errors.append("package file is not covered by the manifest: " + name)
    for name in declared:
        if name not in files:
            errors.append("selfExcluded file is missing: " + name)
    exe = files.get("runtime/MiniEngineSandbox.exe")
    if exe is None:
        errors.append("package has no runtime/MiniEngineSandbox.exe")
    elif hashlib.sha256(exe).hexdigest() != manifest.get("exeSha256"):
        errors.append("packaged EXE does not match exeSha256")
    return errors, manifest


def validate_candidate(root, data, package):
    if not package.exists():
        return ["candidate requires an existing --package: " + str(package)]
    files = package_bytes(package)
    errors, manifest = validate_package_files(files)
    if manifest is None:
        return errors
    built = manifest.get("builtAtCommit", "")
    head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", built, head]).returncode != 0:
        errors.append("builtAtCommit is not an ancestor of HEAD: " + built)
    else:
        changed = subprocess.run(["git", "-C", str(root), "diff", "--name-only", built + ".." + head],
                                 capture_output=True, text=True).stdout.split()
        stale = [c for c in changed if build_sensitive(c)]
        if stale:
            errors.append("runtime inputs changed after builtAtCommit: " + ", ".join(stale[:5]))
    scan = subprocess.run([sys.executable, str(root / PRIVACY_SCANNER), "--source-set", "--git",
                           "--package", str(package), "--baseline", str(root / PRIVACY_BASELINE),
                           "--output", str(root / "out/e5/candidate-scan.json")],
                          capture_output=True, text=True)
    if scan.returncode != 0:
        errors.append("privacy gate failed (unadjudicated or pending items): " + scan.stdout.strip()[:200])
    e4 = json.loads((root / E4_RESULTS).read_text(encoding="utf-8"))
    if e4["package"]["exeSha256"] != manifest.get("exeSha256"):
        errors.append("E4 record was produced for a different EXE")
    if package.suffix.lower() == ".zip" and e4["package"]["zipSha256"] != hashlib.sha256(package.read_bytes()).hexdigest():
        errors.append("E4 record was produced for a different package archive")
    errors += e4_run_errors(e4, manifest.get("exeSha256"))
    errors += e4_reader_errors(e4)
    return errors


def e4_run_errors(e4, exe_sha):
    """E4 运行记录必须完整、不重复、绑定本包 EXE，且退出码符合正负例预期。"""
    errors = []
    second = e4.get("secondMachine", {})
    runs = second.get("runs", [])
    ids = [r.get("id") for r in runs]
    expected = {run_id: code for run_id, code in E4_REQUIRED_RUNS}
    for run_id in sorted(set(ids)):
        if ids.count(run_id) > 1:
            errors.append("E4 run is recorded more than once: " + run_id)
    missing = [run_id for run_id in expected if run_id not in ids]
    if missing:
        errors.append("E4 second-machine record is incomplete, missing: " + ", ".join(missing))
    unknown = [run_id for run_id in ids if run_id not in expected]
    if unknown:
        errors.append("E4 second-machine record has unknown runs: " + ", ".join(sorted(set(unknown))))
    if second.get("complete") is not True:
        errors.append("E4 second-machine record is not marked complete")
    for run in runs:
        run_id = run.get("id")
        if run_id not in expected:
            continue
        if run.get("exitCode") != expected[run_id]:
            errors.append("E4 run exit code is not the expected one: " + str(run_id)
                          + " (expected " + str(expected[run_id]) + ", got " + str(run.get("exitCode")) + ")")
        wanted = "expected-failure" if expected[run_id] else "PASS"
        if run.get("status") != wanted:
            errors.append("E4 run status is not " + wanted + ": " + str(run_id))
        # 运行必须绑定到本包的 EXE：换包之后旧记录不能自动沿用。
        if run.get("packageExeSha256") != exe_sha:
            errors.append("E4 run was produced for a different package: " + str(run_id))
    return errors


def e4_reader_errors(e4):
    """读者走查：passed 干净；deferred-by-owner 记提示（缩减承诺）；其余是错误。"""
    status = e4.get("reader", {}).get("status")
    if status == "passed":
        return []
    if status == "deferred-by-owner":
        return []
    if status == "pending":
        return ["E4 reader feedback is not recorded as passed (owner action required)"]
    return ["E4 reader status is not recognised: " + str(status)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, choices=["entry", "candidate"])
    parser.add_argument("--package", type=Path, default=None, help="candidate 阶段要核对的包目录或 .zip")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        root = safe_root(args.root)
        data = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
        errors = validate_entry(root, data)
        if args.stage == "candidate" and not errors:
            if args.package is None:
                errors = ["candidate requires --package"]
            else:
                errors = validate_candidate(root, data, args.package.resolve())
    except (ValueError, KeyError, OSError) as exc:
        errors = [str(exc)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    if args.stage == "entry":
        print(f"PASS entry: {len(data['files'])} reviewed files; publication=BLOCKED")
    else:
        print(f"PASS candidate: {len(data['files'])} reviewed files, package bytes, privacy gate and E4 record verified; publication=BLOCKED")
        try:
            e4 = json.loads((root / E4_RESULTS).read_text(encoding="utf-8"))
            if e4.get("reader", {}).get("status") == "deferred-by-owner":
                print("NOTICE: E4 reader validation was deferred by the owner; the publication must not claim reader validation")
        except (OSError, KeyError):
            pass
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
