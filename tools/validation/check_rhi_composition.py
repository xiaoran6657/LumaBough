"""核对 CMake File API 的实际编译/依赖信息，输出 M6-06/07 组合边界报告。"""
from __future__ import annotations
import argparse
import json
from pathlib import Path

def inspect(build: Path) -> dict:
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"), key=lambda p: p.stat().st_mtime)
    if not indices:
        raise ValueError("missing File API reply; create query/codemodel-v2 and configure")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    model_ref = next(x for x in index["objects"] if x["kind"] == "codemodel")
    model = json.loads((reply / model_ref["jsonFile"]).read_text(encoding="utf-8"))
    reports = []
    failures = []
    for config in model["configurations"]:
        targets = {}
        by_id = {}
        for item in config["targets"]:
            data = json.loads((reply / item["jsonFile"]).read_text(encoding="utf-8"))
            targets[item["name"]] = data
            by_id[item["id"]] = item["name"]
        def dependencies(name: str, transitive: bool = False) -> set[str]:
            seen = set()
            pending = [name]
            while pending:
                current = pending.pop()
                for edge in targets[current].get("dependencies", []):
                    dep = by_id[edge["id"]]
                    if dep not in seen:
                        seen.add(dep)
                        if transitive:
                            pending.append(dep)
            return seen
        required = ["MiniEngineRhiPublic", "MiniEngineRenderer", "MiniEngineRhiSmoke", "MiniEngineRenderGraph",
                    "MiniEngineRhiFactory", "MiniEngineD3D11RhiAdapter",
                    "MiniEngineD3D12RhiAdapter", "MiniEngineD3D11Runtime",
                    "MiniEngineD3D12Runtime", "MiniEngineSandbox"]
        for name in required:
            if name not in targets:
                failures.append(f'{config["name"]}: missing {name}')
        if any(name not in targets for name in required):
            continue
        selected = {}
        for name in required:
            target = targets[name]
            includes = sorted({i["path"] for group in target.get("compileGroups", [])
                               for i in group.get("includes", [])})
            selected[name] = {
                "directDependencies": sorted(dependencies(name)),
                "transitiveDependencies": sorted(dependencies(name, True)),
                "includePaths": includes,
                "sources": sorted(x["path"] for x in target.get("sources", [])
                                  if not x.get("isGenerated", False)),
                "artifacts": target.get("artifacts", []),
                "linkFragments": target.get("link", {}).get("commandFragments", [])
            }
        for name in ["MiniEngineRhiPublic", "MiniEngineRenderer", "MiniEngineRhiSmoke", "MiniEngineRenderGraph"]:
            info = selected[name]
            for dep in info["transitiveDependencies"]:
                if any(x in dep.lower() for x in ("d3d11", "d3d12", "dxgi", "rhifactory")):
                    failures.append(f'{config["name"]}: {name} depends on {dep}')
            for include in info["includePaths"]:
                path = include.replace("\\", "/").lower()
                if any(x in path for x in ("/rhi/d3d11", "/rhi/d3d12", "/rhi/factory")):
                    failures.append(f'{config["name"]}: {name} includes {include}')
        for name in ["MiniEngineD3D11RhiAdapter", "MiniEngineD3D12RhiAdapter",
                     "MiniEngineD3D11Runtime", "MiniEngineD3D12Runtime"]:
            for dep in selected[name]["transitiveDependencies"]:
                if any(x in dep for x in ("Renderer", "World", "Assets", "RhiFactory")):
                    failures.append(f'{config["name"]}: {name} reverses dependency to {dep}')
        for api in ("D3D11", "D3D12"):
            shared = f"MiniEngine{api}Runtime"
            for name in (f"MiniEngine{api}", f"MiniEngine{api}RhiAdapter"):
                if shared not in dependencies(name, True):
                    failures.append(f'{config["name"]}: {name} does not reuse {shared}')
            if f"MiniEngine{api}RhiAdapter" not in dependencies("MiniEngineRhiFactory", True):
                failures.append(f'{config["name"]}: factory missing {api}')
        if "MiniEngineRhiFactory" not in dependencies("MiniEngineSandbox", True):
            failures.append(f'{config["name"]}: sandbox does not use factory')
        # Visual Studio 的 ZERO_CHECK 是 CMake 再生成工具，不是链接依赖。
        if dependencies("MiniEngineRenderGraph") - {"ZERO_CHECK"} != {"MiniEngineRhiPublic"}:
            failures.append(f'{config["name"]}: graph must link only public RHI')
        if "MiniEngineRenderGraph" not in dependencies("MiniEngineRenderer"):
            failures.append(f'{config["name"]}: renderer does not declare graph dependency')
        reports.append({"configuration": config["name"], "targets": selected})
    return {"schema": "miniengine.m6-06.composition.v1",
            "status": "FAIL" if failures else "PASS",
            "renderGraph": "M6-08 graph compiler, culling and stable execution; resource reuse remains M6-09",
            "configurations": reports, "failures": failures}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = inspect(args.build)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f'{result["status"]}: {len(result["configurations"])} configurations')
    for failure in result["failures"]:
        print(failure)
    return 0 if result["status"] == "PASS" else 1

if __name__ == "__main__":
    raise SystemExit(main())
