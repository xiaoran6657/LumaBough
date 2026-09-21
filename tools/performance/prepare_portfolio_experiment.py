"""新性能实验的构建准备工具（可复用、路径可配置）。

为什么独立成仓库工具：001–004 的准备逻辑原先只留在 `out/performance/<id>/bootstrap.py`，不可复用、
不可测试，每次都要复制改路径——003/004 因此各出现过一次"采集器 ATTEMPT 常量未同步"的注册缺陷。
本工具只负责"准备"：按逐文件允许清单冻结源码 → 复用已校验的依赖源码 → 全新 configure/build → 烘焙。
它不做采样、不写运行结果、不复用任何旧对象或可执行文件。

安全闸门（任一不满足即失败，不做静默降级）：
1. 工作区必须**不存在**（fresh，不用 --force）；每一级目录创建失败即停。
2. 允许清单里的路径必须解析在仓库内，且**任何一级**都不能是符号链接或联接点（防路径逃逸）。
3. 逐文件哈希必须与清单一致：复制前、复制后、构建+烘焙完成后各校验一次。
4. 依赖源码只从显式给出的父工作区复用，逐个文件比对父 BUILD-INPUTS.json 的哈希；
   0 字节文件一律拒绝——历史上出现过依赖的零字节下载。
5. 每个外部命令的退出码、日志哈希都写进 BUILD-RECORD.json；非零即停。

用法：
  python tools/performance/prepare_portfolio_experiment.py \
      --workspace out/performance/lb-current-005 \
      --parent-workspace out/performance/lb-current-004 \
      --attempt 005
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ARCHIVE_DATE = (2026, 1, 1, 0, 0, 0)  # 固定时间戳：同样的输入产出同样的 zip
DEFAULT_DELTA = (
    "samples/rhi_sandbox/M7SceneRunner.cpp",
    "tools/performance/run_portfolio_experiment.py",
    "tools/performance/summarize_portfolio_experiment.py",
    "tests/tools/contracts/test_new_performance.py",
)
COOK_VALIDATOR = "C:/tools/gltf_validator-2.0.0-dev.3.10-win64/gltf_validator.exe"


def sha256_of(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def is_link_like(path: Path) -> bool:
    path = Path(path)
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def assert_no_links(path: Path, root: Path) -> None:
    """path 及其到 root 之间的每一级都不能是链接/联接点。

    注意必须在 **resolve() 之前**检查：联接点 resolve 之后会指向仓库外的真实位置，
    那样报出来的是"路径逃逸"，虽然同样拒绝，但会把根因说错。
    """
    current = Path(path)
    root = Path(root)
    while current != root and current.parent != current:
        if is_link_like(current):
            raise ValueError(f"link or junction in path: {current}")
        current = current.parent


def resolve_inside(root: Path, relative: str) -> Path:
    root = Path(root).resolve()
    if not relative or Path(relative).is_absolute():
        raise ValueError(f"path must be a non-empty relative path: {relative!r}")
    candidate = root / relative
    assert_no_links(candidate, root)
    resolved = candidate.resolve()
    if not resolved.is_relative_to(root):
        raise ValueError(f"path escapes the repository: {relative}")
    return resolved


def load_entries(repo: Path, manifest_path: Path, delta: tuple[str, ...]) -> list[dict]:
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 2 or not isinstance(manifest.get("files"), list):
        raise ValueError("unsupported publication manifest")
    entries = {entry["path"]: dict(entry) for entry in manifest["files"]}
    self_path = manifest_path.resolve().relative_to(Path(repo).resolve()).as_posix()
    entries[self_path] = {"path": self_path, "sha256": sha256_of(manifest_path),
                          "category": manifest.get("schemaVersion"), "reason": "publication manifest itself"}
    for name in delta:
        path = resolve_inside(repo, name)
        if not path.is_file():
            raise ValueError(f"delta file missing: {name}")
        entries[name] = {"path": name, "sha256": sha256_of(path),
                         "classification": "explicit experiment revision delta"}
    resolved = []
    for name in sorted(entries):
        entry = entries[name]
        path = resolve_inside(repo, name)
        if not path.is_file():
            raise ValueError(f"listed file missing: {name}")
        actual = sha256_of(path)
        if entry.get("sha256") and actual != entry["sha256"]:
            raise ValueError(f"hash mismatch before copy: {name}")
        entry["sha256"] = actual
        resolved.append(entry)
    return resolved


def copy_sources(repo: Path, source_dir: Path, entries: list[dict]) -> None:
    for entry in entries:
        src = resolve_inside(repo, entry["path"])
        dst = Path(source_dir) / entry["path"]
        if is_link_like(Path(source_dir)) or is_link_like(dst.parent):
            raise ValueError(f"link or junction in target path: {dst}")
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.parent.is_dir():
            raise ValueError(f"cannot create directory: {dst.parent}")
        shutil.copyfile(src, dst)
        if sha256_of(dst) != entry["sha256"]:
            raise ValueError(f"hash mismatch after copy: {entry['path']}")


def verify_frozen(source_dir: Path, entries: list[dict]) -> None:
    for entry in entries:
        path = Path(source_dir) / entry["path"]
        if not path.is_file():
            raise ValueError(f"frozen file missing: {entry['path']}")
        if sha256_of(path) != entry["sha256"]:
            raise ValueError(f"frozen hash drift: {entry['path']}")


def write_snapshot(workspace: Path, repo: Path, manifest_path: Path, entries: list[dict]) -> Path:
    snapshot = {
        "schemaVersion": 1,
        "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "sourceCommit": None,
        "publicationManifestSha256": sha256_of(manifest_path),
        "repoName": Path(repo).name,
        "files": entries,
    }
    path = Path(workspace) / "SOURCE-SNAPSHOT.json"
    path.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return path


def write_archive(workspace: Path, source_dir: Path, entries: list[dict]) -> Path:
    path = Path(workspace) / "source.zip"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for entry in sorted(entries, key=lambda item: item["path"]):
            info = zipfile.ZipInfo(entry["path"], date_time=ARCHIVE_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (Path(source_dir) / entry["path"]).read_bytes())
    return path


def verify_dependencies(parent_workspace: Path, target_build: Path, allow_empty: tuple[str, ...] = ()) -> dict:
    """从父工作区复用依赖源码：逐个文件校验哈希。

    零字节文件历史上出现过（依赖下载失败），所以默认一律拒绝；但父构建里确实存在内容为空的
    合法文件（例如 `.gitmodules`），因此允许操作者用 `--allow-empty <name>` **逐个点名确认**，
    被确认的空文件写入依赖报告的 `emptyFiles`，不会静默通过。
    """
    parent_workspace = Path(parent_workspace)
    inputs_path = parent_workspace / "BUILD-INPUTS.json"
    if not inputs_path.is_file():
        raise ValueError(f"parent BUILD-INPUTS.json missing: {inputs_path}")
    dependencies = json.loads(inputs_path.read_text(encoding="utf-8")).get("dependencyFiles")
    if not dependencies:
        raise ValueError("parent BUILD-INPUTS.json has no dependencyFiles")
    allowed = set(allow_empty)
    unknown = sorted(allowed - set(dependencies))
    if unknown:
        raise ValueError(f"--allow-empty names are not dependency files: {unknown}")
    copied = {}
    empty_files = {}
    for name, expected in sorted(dependencies.items()):
        src = parent_workspace / "build" / name
        if not src.is_file():
            raise ValueError(f"dependency source missing: {name}")
        empty = src.stat().st_size == 0
        if empty and name not in allowed:
            raise ValueError(f"empty dependency file needs explicit --allow-empty: {name}")
        actual = sha256_of(src)
        if actual != expected:
            raise ValueError(f"dependency hash mismatch: {name}")
        dst = Path(target_build) / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        if sha256_of(dst) != expected or (dst.stat().st_size == 0) != empty:
            raise ValueError(f"dependency copy failed verification: {name}")
        copied[name] = expected
        if empty:
            empty_files[name] = expected
    repair = {
        "kind": "verified dependency source reuse; no previous object/executable reused",
        "parentRepairSha256": sha256_of(parent_workspace / "DEPENDENCY-DOWNLOAD-REPAIR.json"),
        "parentInputsSha256": sha256_of(inputs_path),
        "files": copied,
        "emptyFiles": empty_files,
    }
    (Path(target_build).parent / "DEPENDENCY-DOWNLOAD-REPAIR.json").write_text(
        json.dumps(repair, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return copied


def run_step(workspace: Path, label: str, command: list[str], cwd: Path, env: dict) -> dict:
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    log = Path(workspace) / f"{label}.log"
    with log.open("wb") as handle:
        result = subprocess.run([str(item) for item in command], cwd=str(cwd), env=env,
                                stdout=handle, stderr=subprocess.STDOUT)
    step = {"label": label, "command": [str(item) for item in command], "startedUtc": started,
            "exitCode": result.returncode, "logSha256": sha256_of(log)}
    record_path = Path(workspace) / "BUILD-RECORD.json"
    record = json.loads(record_path.read_text(encoding="utf-8")) if record_path.is_file() else {"steps": []}
    record["sourceSnapshotSha256"] = sha256_of(Path(workspace) / "SOURCE-SNAPSHOT.json")
    record["sourceArchiveSha256"] = sha256_of(Path(workspace) / "source.zip")
    record["steps"] = record.get("steps", []) + [step]
    record_path.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if result.returncode:
        raise ValueError(f"step failed: {label} (exit {result.returncode})")
    return step


def configure_command(cmake: Path, source_dir: Path, build_dir: Path) -> list[str]:
    return [cmake, "-S", source_dir, "-B", build_dir, "-G", "Visual Studio 18 2026", "-A", "x64", "-T", "v145",
            "-DBUILD_TESTING=ON", "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=ProgramDatabase",
            "-DME_ENABLE_TRACY=OFF",
            "-DFETCHCONTENT_SOURCE_DIR_FASTGLTF=" + str(build_dir / "_deps/fastgltf-src"),
            "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=" + str(build_dir / "_deps/googletest-src"),
            "-DFETCHCONTENT_SOURCE_DIR_MINIENGINE_PIX=" + str(build_dir / "_deps/miniengine_pix-src")]


def cook_command(cooker: Path, source_dir: Path, recipe_dir: Path, output_dir: Path, validator: Path) -> list[str]:
    return [cooker, "cook", "--source-root", source_dir / "assets/source", "--recipe-root", recipe_dir,
            "--output", output_dir, "--profile", "windows-d3d11", "--validator", validator]


def prepare(args: argparse.Namespace) -> dict:
    repo = Path(args.repo).resolve()
    workspace = Path(args.workspace).resolve()
    if workspace.exists():
        raise ValueError(f"workspace must be fresh (already exists): {workspace}")
    if not workspace.is_relative_to(repo):
        raise ValueError("workspace must live inside the repository")
    source_dir = workspace / "source"
    build_dir = workspace / "build"
    cmake = Path(args.cmake)
    validator = Path(args.validator)
    for tool in (cmake, validator):
        if not tool.is_file():
            raise ValueError(f"required tool missing: {tool}")
    workspace.mkdir(parents=True)
    if not workspace.is_dir():
        raise ValueError(f"cannot create workspace: {workspace}")
    manifest = resolve_inside(repo, args.manifest)
    entries = load_entries(repo, manifest, tuple(args.delta))
    copy_sources(repo, source_dir, entries)
    verify_frozen(source_dir, entries)
    write_snapshot(workspace, repo, manifest, entries)
    write_archive(workspace, source_dir, entries)
    verify_dependencies(Path(args.parent_workspace).resolve(), build_dir, tuple(args.allow_empty))
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PATH"] = str(cmake.parent) + os.pathsep + env["PATH"]
    tmp = workspace / "tmp"
    tmp.mkdir()
    env["TEMP"] = str(tmp)
    env["TMP"] = str(tmp)
    run_step(workspace, "configure", configure_command(cmake, source_dir, build_dir), source_dir, env)
    run_step(workspace, "build", [cmake, "--build", build_dir, "--config", "Release", "--target",
                                  "MiniEngineSandbox", "MiniEngineAssetCooker", "--parallel", "4"], source_dir, env)
    recipes = source_dir / "out/m4-09/recipes"
    recipes.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source_dir / "assets/recipes/m4-visual-baseline.asset.json",
                    recipes / "m4-visual-baseline.asset.json")
    run_step(workspace, "cook", cook_command(build_dir / "tools/asset_cooker/Release/MiniEngineAssetCooker.exe",
                                             source_dir, recipes, source_dir / "out/m4-09/scene", validator),
             source_dir, env)
    verify_frozen(source_dir, entries)
    result = {
        "schemaVersion": 1,
        "workspace": workspace.relative_to(repo).as_posix(),
        "attempt": args.attempt,
        "fileCount": len(entries),
        "sourceSnapshotSha256": sha256_of(workspace / "SOURCE-SNAPSHOT.json"),
        "sourceArchiveSha256": sha256_of(workspace / "source.zip"),
        "executableSha256": sha256_of(build_dir / "samples/rhi_sandbox/Release/MiniEngineSandbox.exe"),
        "cookedManifestSha256": sha256_of(source_dir / "out/m4-09/scene/manifest.json"),
    }
    (workspace / "PREPARE-RESULT.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                                                   encoding="utf-8")
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--parent-workspace", required=True, type=Path,
                        help="已验证过的上一个工作区（只复用其依赖源码）")
    parser.add_argument("--attempt", required=True, help="实验编号，例如 005")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--manifest", default="docs/evidence/PUBLICATION-FILES.json")
    parser.add_argument("--cmake", default="C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/"
                                          "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
    parser.add_argument("--validator", default=COOK_VALIDATOR)
    parser.add_argument("--delta", action="append", default=list(DEFAULT_DELTA),
                        help="允许清单之外需要显式冻结的实验修订文件（可重复）")
    parser.add_argument("--allow-empty", action="append", default=[],
                        help="逐个点名确认内容为空的依赖文件（默认任何零字节文件都拒绝）")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    result = prepare(args)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
