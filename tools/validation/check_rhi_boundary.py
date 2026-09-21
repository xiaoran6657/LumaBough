"""M6 公共契约静态边界检查；这是源码保护栏，不代替 C++ 编译或运行时验证。"""
import argparse
import json
import re
import tempfile
from pathlib import Path

PUBLIC = Path("engine/rhi/include/MiniEngine/Rhi")
REQUIRED = {"RhiTypes.h", "RhiHandle.h", "RhiDescriptors.h", "RhiCapabilities.h",
            "RhiError.h", "IRhiDevice.h", "IRhiCommandList.h", "IRhiGraphCommandSink.h", "RhiFactory.h", "RhiValidation.h", "RhiPipeline.h", "RhiShaderPackage.h", "RhiResults.h"}
ROOTS = [PUBLIC, Path("engine/rhi/src"), Path("engine/assets"), Path("engine/world"),
         Path("engine/render"), Path("engine/render_graph")]
# 旧 samples/sandbox、samples/sandbox_d3d12 是仍可回退的 concrete composition；不将它们误报为已迁移的高层。
# M6 统一 sandbox（samples/rhi_sandbox）按 M6-11 纳入扫描：native D3D token 全域禁止，
# 平台 include 与 RhiBackend 名称只对逐文件白名单开放，不能用目录 exclude 隐藏新文件。
SAMPLES = [Path("samples/rhi_sandbox"), Path("samples/common")]
NATIVE = re.compile(r"\b(?:ID3D\w*|IDXGI\w*|D3D11_\w*|D3D12_\w*|DXGI_\w*|ComPtr|HRESULT|GetNative\w*|Native\w*Handle|GPUVirtualAddress)\b")
# samples 的 native 禁止 token 不含 NativeHandle：平台窗口访问器由 engine/platform 提供，不是图形 API 逃生口。
SAMPLES_NATIVE = re.compile(r"\b(?:ID3D\w*|IDXGI\w*|D3D11_\w*|D3D12_\w*|DXGI_\w*|ComPtr|HRESULT|GetNative\w*|GPUVirtualAddress)\b")
BAD_PATH = re.compile(r"(?:^|/)(?:windows|d3d\w*|dxgi\w*|wrl)(?:[./]|$)|MiniEngine/Rhi/(?:D3D11|D3D12)/|rhi/(?:d3d11|d3d12)/", re.I)
# 逐文件白名单：平台胶合文件可 include Windows.h 等；启动/元数据文件可出现 RhiBackend 名称。
SAMPLES_PLATFORM_ALLOW = {"samples/rhi_sandbox/main.cpp", "samples/rhi_sandbox/M6SceneRunner.cpp",
                          "samples/rhi_sandbox/M7SceneRunner.cpp", "samples/rhi_sandbox/M6LegacyLaunch.cpp",
                          # M7-12：GPU capture 作用域的共享头（Windows.h/pix3.h/renderdoc_app.h），
                          # 只被 sandbox 的运行器使用；引擎侧仍不得出现平台/图形工具头。
                          "samples/rhi_sandbox/CaptureScopes.h"}
SAMPLES_BACKEND_ALLOW = {"samples/rhi_sandbox/RhiSelection.h", "samples/rhi_sandbox/RhiSelection.cpp",
                         "samples/rhi_sandbox/main.cpp", "samples/rhi_sandbox/M6SceneRunner.cpp"}
SAMPLES_PLATFORM_INCLUDE = re.compile(r"^(?:windows\.h|windows\.hpp|pix3\.h|renderdoc_app\.h)", re.I)
SAMPLES_GRAPHICS_INCLUDE = re.compile(r"^(?:d3d\w*|dxgi\w*|wrl/)", re.I)
SAMPLES_BACKEND_INCLUDE = re.compile(r"(?:D3D11|D3D12)RhiBackend\.h|NativeRhiBackend\.h|MiniEngine/Rhi/(?:D3D11|D3D12)/|rhi/(?:d3d11|d3d12)/", re.I)
STANDARD = {"array", "cstddef", "cstdint", "compare", "functional", "limits", "memory",
            "optional", "span", "stdexcept", "string", "string_view", "utility", "vector"}
# M7-03：任务库是纯 CPU 调度，不得依赖 RHI/资产/渲染/图形 API/平台头（ADR-0008）。
# 允许：MiniEngine/Tasks（自身）、MiniEngine/Core（断言/日志）、MiniEngine/Profiling（线程名/zone）。
TASKS = Path("engine/tasks")
TASKS_ALLOWED_PREFIX = ("MiniEngine/Tasks/", "MiniEngine/Core/", "MiniEngine/Profiling/")
TASKS_FORBIDDEN_INCLUDE = re.compile(r"MiniEngine/(?:Rhi|Assets|Render|RenderGraph)/|d3d\w*|dxgi\w*|wrl/|Windows\.h",
                                     re.I)
TASKS_ALLOWED_SYSTEM = {"atomic", "chrono", "condition_variable", "cstddef", "cstdint", "cstdio", "cstdlib",
                        "deque", "exception", "functional", "limits", "memory", "mutex", "new", "optional",
                        "semaphore", "stdexcept", "string", "string_view", "thread", "utility", "vector"}
# 旧 concrete samples 已于 2026-09-16 退役（LEGACY-SAMPLES-RETIRE），目录已删除；豁免机制保留，
# 新增文件在显式更新清单（--write-legacy-manifest）前一律 FAIL，避免用目录 exclude 隐藏新代码。
LEGACY = [Path("samples/sandbox"), Path("samples/sandbox_d3d12")]
# M7-05：并行 RenderPacket 构建是 worker 侧纯 CPU 阶段（ADR-0008 线程边界）。engine/render 中
# 依赖 MiniEngine/Tasks 的文件不得引用 RHI/图形 API/平台头——worker 不触碰 RHI/World/平台对象。
# 不含 Tasks 的 engine/render 文件（M6 迁移期 pass）不受此规则影响。
TASK_DRIVEN_RENDER = Path("engine/render")
TASK_DRIVEN_FORBIDDEN = re.compile(r"MiniEngine/Rhi/|(?:^|/)(?:d3d\w*|dxgi\w*|wrl)(?:[./]|$)|Windows\.h", re.I)
LEGACY_MANIFEST = Path("tools/legacy/legacy_samples_manifest.json")
LEGACY_SUFFIXES = {".h", ".hpp", ".cpp", ".inl", ".c", ".hlsl", ".txt", ".json", ".pipeline"}


def legacy_files(root):
    found = []
    for folder in LEGACY:
        if not (root / folder).exists():
            continue
        for file in sorted((root / folder).rglob("*")):
            if file.is_file() and file.suffix.lower() in LEGACY_SUFFIXES:
                found.append(file.relative_to(root).as_posix())
    return found


def write_legacy_manifest(root):
    payload = {
        "schemaVersion": 1,
        "note": "旧 concrete samples 的 A/B 边界清单；新增文件必须先显式更新本清单并说明理由。",
        "files": legacy_files(root),
    }
    path = root / LEGACY_MANIFEST
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return payload

def uncomment(text):
    # 保留字符串（包括 include 文件名），避免注释中的历史 API 名触发误报。
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda m: "\n" * m[0].count("\n") if m[0].startswith(("//", "/*")) else m[0], text)

def audit(root):
    errors, scanned = [], []
    for name in sorted(REQUIRED):
        if not (root / PUBLIC / name).is_file():
            errors.append(f"missing required public header: {name}")
    for folder in ROOTS:
        if not (root / folder).exists():
            continue
        for file in sorted((root / folder).rglob("*")):
            if file.suffix not in {".h", ".hpp", ".cpp", ".inl"}:
                continue
            rel = file.relative_to(root)
            text = uncomment(file.read_text(encoding="utf-8-sig"))
            scanned.append(rel.as_posix())
            if NATIVE.search(text):
                errors.append(f"{rel}: native token")
            includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M)
            if any(BAD_PATH.search(name.replace("\\", "/")) for name in includes):
                errors.append(f"{rel}: native/concrete include")
            if PUBLIC in rel.parents:
                # 公共头仅允许已审核的 STL 与本层头；堵住间接平台 include。
                for name in includes:
                    if name not in STANDARD and not (name.startswith("MiniEngine/Rhi/") and
                        name.removeprefix("MiniEngine/Rhi/") in REQUIRED):
                        errors.append(f"{rel}: unapproved public include {name}")
                directives = re.findall(r'^\s*#\s*include\s+(.+)$', text, re.M)
                if len(directives) != len(includes):
                    errors.append(f"{rel}: macro include requires review")
                pointer_text = text
                if rel.name in {"RhiFactory.h", "IRhiDevice.h"}:
                    pointer_text = pointer_text.replace("void* nativeWindow = nullptr;", "")
                if re.search(r'\b(?:void\s*\*|uintptr_t)\b|\bvoid\s*\*', pointer_text):
                    errors.append(f"{rel}: opaque native payload outside composition window")
            elif folder != Path("engine/rhi/src"):
                if re.search(r'\bRhiBackend\b|\bWaitIdle\s*\(', text):
                    errors.append(f"{rel}: backend selection/synchronization in high-level code")
                if folder == TASK_DRIVEN_RENDER and "MiniEngine/Tasks/" in text:
                    for name in includes:
                        if TASK_DRIVEN_FORBIDDEN.search(name.replace("\\", "/")):
                            errors.append(
                                f"{rel}: task-driven render stage must stay backend-free ({name})")
                # Sink 属于图执行器；普通 pass/renderer 与声明/编译阶段不得取得它。
                if re.search(r'\bGraphCommandSink\s*\(', text) and rel.as_posix() != "engine/render_graph/src/RenderGraphExecute.cpp":
                    errors.append(f"{rel}: graph sink outside graph executor")
    if (root / TASKS).exists():
        for file in sorted((root / TASKS).rglob("*")):
            if file.suffix not in {".h", ".hpp", ".cpp", ".inl"}:
                continue
            rel = file.relative_to(root)
            text = uncomment(file.read_text(encoding="utf-8-sig"))
            scanned.append(rel.as_posix())
            if NATIVE.search(text):
                errors.append(f"{rel}: native token in the task system")
            if re.search(r"\bRhiBackend\b|\bWaitIdle\s*\(", text):
                errors.append(f"{rel}: backend selection/synchronization in the task system")
            for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M):
                normalized = name.replace("\\", "/")
                if TASKS_FORBIDDEN_INCLUDE.search(normalized):
                    errors.append(f"{rel}: forbidden dependency in the task system ({name})")
                    continue
                if normalized.startswith(TASKS_ALLOWED_PREFIX) or "/" not in normalized:
                    continue  # 自身/白名单模块头，或同目录私有头（GlobalTaskQueue.h 等）。
                if normalized not in TASKS_ALLOWED_SYSTEM:
                    errors.append(f"{rel}: unapproved include in the task system ({name})")
    for folder in SAMPLES:
        if not (root / folder).exists():
            continue
        for file in sorted((root / folder).rglob("*")):
            if file.suffix not in {".h", ".hpp", ".cpp", ".inl"}:
                continue
            rel = file.relative_to(root).as_posix()
            text = uncomment(file.read_text(encoding="utf-8-sig"))
            scanned.append(rel)
            if SAMPLES_NATIVE.search(text):
                errors.append(f"{rel}: native token in the unified sandbox")
            for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M):
                normalized = name.replace("\\", "/")
                if SAMPLES_BACKEND_INCLUDE.search(normalized):
                    errors.append(f"{rel}: concrete backend include")
                if SAMPLES_GRAPHICS_INCLUDE.match(normalized):
                    errors.append(f"{rel}: graphics API include")
                if SAMPLES_PLATFORM_INCLUDE.match(normalized) and rel not in SAMPLES_PLATFORM_ALLOW:
                    errors.append(f"{rel}: platform include outside the per-file whitelist ({name})")
            if re.search(r"\bRhiBackend\b", text) and rel not in SAMPLES_BACKEND_ALLOW:
                errors.append(f"{rel}: backend name outside bootstrap/metadata files")
    current_legacy = legacy_files(root)
    manifest_path = root / LEGACY_MANIFEST
    if manifest_path.is_file():
        known_legacy = json.loads(manifest_path.read_text(encoding="utf-8")).get("files", [])
        added = sorted(set(current_legacy) - set(known_legacy))
        removed = sorted(set(known_legacy) - set(current_legacy))
        if added:
            errors.append("legacy concrete samples grew without manifest update: " + ", ".join(added[:8]))
        legacy_report = {"files": len(current_legacy), "added": added, "removed": removed}
    else:
        errors.append(f"legacy concrete samples manifest missing: {LEGACY_MANIFEST}")
        legacy_report = {"files": len(current_legacy), "added": [], "removed": []}
    return {"status": "FAIL" if errors else "PASS", "scannedFiles": scanned,
            "errors": errors, "legacyExempt": legacy_report,
            "scope": "M6 public/common implementation/assets/world and render/graph; M7-03 engine/tasks (no RHI/asset/render/platform dependency); M7-05 task-driven engine/render files (no RHI/graphics/platform dependency); unified samples/rhi_sandbox and samples/common with per-file platform/backend whitelist; legacy concrete samples exempt but bounded by manifest"}

def self_test():
    """自检：断言执行时计数（不写死数字），并覆盖 legacy 清单边界。"""
    counters = {"negative": 0, "positive": 0}
    with tempfile.TemporaryDirectory(prefix="m6-boundary-") as name:
        root = Path(name)
        public = root / PUBLIC
        public.mkdir(parents=True)
        for header in REQUIRED:
            (public / header).write_text("#pragma once\n", encoding="utf-8")
        # 旧 samples：先建立有界豁免（清单 + 文件），随后验证未列出的新文件被拒绝。
        legacy = root / "samples/sandbox"
        legacy.mkdir(parents=True)
        (legacy / "LegacyRenderer.cpp").write_text("ID3D11Device* device;\n", encoding="utf-8")
        write_legacy_manifest(root)

        def expect(status):
            counters["positive" if status == "PASS" else "negative"] += 1
            return audit(root)["status"] == status

        target = public / "RhiTypes.h"
        assert expect("PASS")
        for defect in ['#include <Windows.h>', '#include <MiniEngine/Rhi/D3D12/D3D12Device.h>',
                       'ID3D12Resource* resource;', 'void* resource;', '#include <MiniEngine/Core/Hidden.h>',
                       '#include HIDDEN_PLATFORM_HEADER', 'uintptr_t resource;']:
            target.write_text(defect, encoding="utf-8")
            assert expect("FAIL"), defect
        target.write_text('// ID3D12Resource is forbidden\n', encoding="utf-8")
        assert expect("PASS")
        executor = root / "engine/render_graph/src/RenderGraphExecute.cpp"
        executor.parent.mkdir(parents=True)
        executor.write_text("device.GraphCommandSink(frame);", encoding="utf-8")
        assert expect("PASS")
        for defect in ["RhiBackend backend;", "device.WaitIdle();"]:
            executor.write_text(defect, encoding="utf-8")
            assert expect("FAIL"), defect
        executor.write_text("device.GraphCommandSink(frame);", encoding="utf-8")
        for relative in ["engine/render/Pass.cpp", "engine/render_graph/src/RenderGraphCompile.cpp"]:
            forbidden = root / relative
            forbidden.parent.mkdir(parents=True, exist_ok=True)
            forbidden.write_text("device.GraphCommandSink(frame);", encoding="utf-8")
            assert expect("FAIL"), relative
            forbidden.unlink()
        target.unlink()
        assert expect("FAIL")
        # samples 区：启动/平台胶合白名单文件通过；新文件与越界 token 必须失败。
        (public / "RhiTypes.h").write_text("#pragma once\n", encoding="utf-8")
        sandbox = root / "samples/rhi_sandbox"
        sandbox.mkdir(parents=True)
        bootstrap = sandbox / "RhiSelection.cpp"
        bootstrap.write_text("RhiBackend backend;\n", encoding="utf-8")
        harness = sandbox / "M6SceneRunner.cpp"
        harness.write_text("#include <Windows.h>\n#include <pix3.h>\nRhiBackend backend;\n", encoding="utf-8")
        assert expect("PASS")
        new_file = sandbox / "NewHarness.cpp"
        for defect in ["RhiBackend backend;", "#include <Windows.h>", "ID3D12Resource* leaked;",
                       "#include <d3d12.h>", "#include <dxgi1_6.h>",
                       "#include <MiniEngine/Rhi/D3D12/D3D12Device.h>", '#include "D3D12RhiBackend.h"',
                       "#include <MiniEngine/Rhi/D3D11/D3D11Device.h>"]:
            new_file.write_text(defect, encoding="utf-8")
            assert expect("FAIL"), defect
        new_file.unlink()
        for defect in ["ID3D11DeviceContext* context;", "#include <wrl/client.h>"]:
            harness.write_text(defect, encoding="utf-8")
            assert expect("FAIL"), defect
        # legacy 清单边界：新增未列出文件、清单缺失都必须 FAIL；恢复后回到 PASS。
        harness.write_text("#include <Windows.h>\n#include <pix3.h>\nRhiBackend backend;\n", encoding="utf-8")
        # M7-03 tasks 区：白名单依赖通过；RHI/资产/渲染/平台/第三方头必须 FAIL。
        # 位置要求在 samples 恢复合法状态之后，否则正向控制会被上一个故意缺陷污染。
        tasks_dir = root / "engine/tasks/src"
        tasks_dir.mkdir(parents=True)
        task_file = tasks_dir / "TaskSystem.cpp"
        task_file.write_text(
            "#include <MiniEngine/Tasks/TaskSystem.h>\n#include <MiniEngine/Core/Assert.h>\n"
            "#include <MiniEngine/Profiling/Profile.h>\n#include <atomic>\n#include <thread>\n"
            '#include "GlobalTaskQueue.h"\n',
            encoding="utf-8")
        assert expect("PASS")
        for defect in ["#include <MiniEngine/Rhi/IRhiDevice.h>", "#include <Windows.h>",
                       "#include <MiniEngine/Assets/AssetManager.h>", "#include <MiniEngine/Render/Renderer.h>",
                       "ID3D12Device* device;", "RhiBackend backend;", "device.WaitIdle();",
                       "#include <nlohmann/json.hpp>"]:
            task_file.write_text(defect, encoding="utf-8")
            assert expect("FAIL"), defect
        task_file.unlink()
        # M7-05 engine/render 区：依赖 Tasks 的并行阶段必须保持 backend-free；不含 Tasks 的
        # render 文件仍可引用 RHI（M6 迁移期 pass 的合法依赖）。
        render_dir = root / "engine/render/src"
        render_dir.mkdir(parents=True)
        parallel_file = render_dir / "RenderPacketBuilder.cpp"
        parallel_file.write_text(
            "#include <MiniEngine/Tasks/TaskSystem.h>\n#include <MiniEngine/World/RenderPacket.h>\n",
            encoding="utf-8")
        assert expect("PASS")
        parallel_file.write_text("#include <MiniEngine/Rhi/IRhiDevice.h>\n", encoding="utf-8")
        assert expect("PASS"), "render 文件不含 Tasks 时引用 RHI 应通过"
        parallel_file.write_text(
            "#include <MiniEngine/Tasks/TaskSystem.h>\n#include <MiniEngine/Rhi/IRhiDevice.h>\n", encoding="utf-8")
        assert expect("FAIL"), "task-driven render stage must stay backend-free"
        parallel_file.unlink()
        extra = legacy / "NewLegacyOnly.cpp"
        extra.write_text("// 未经清单允许的新旧路径文件\n", encoding="utf-8")
        assert expect("FAIL")
        extra.unlink()
        assert expect("PASS")
        manifest = root / LEGACY_MANIFEST
        backup = manifest.with_name("legacy_samples_manifest.bak")
        manifest.rename(backup)
        assert expect("FAIL")
        backup.rename(manifest)
        assert expect("PASS")
        bootstrap.unlink()
        harness.unlink()
    return {"status": "PASS", "negativeControls": counters["negative"], "positiveControls": counters["positive"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--write-legacy-manifest", action="store_true",
                        help="按当前树重写旧 concrete samples 的 A/B 边界清单（显式操作，不自动更新）")
    args = parser.parse_args()
    if args.write_legacy_manifest:
        payload = write_legacy_manifest(args.root.resolve())
        print(json.dumps({"status": "PASS", "manifest": str(LEGACY_MANIFEST), "files": len(payload["files"])},
                         ensure_ascii=False, indent=2))
        raise SystemExit(0)
    report = self_test() if args.self_test else audit(args.root.resolve())
    print(json.dumps(report, ensure_ascii=False, indent=2))
    raise SystemExit(0 if report["status"] == "PASS" else 1)
