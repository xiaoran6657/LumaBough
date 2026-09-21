"""历史 parity 纯函数，保留 CPU 回归契约。"""
from __future__ import annotations

def parity_phase_payload(manifest: dict, result: dict, run_manifest_sha256: str | None) -> dict:
    """把 parity run manifest 摘要为阶段产物。legacySkipped 必须从这里贯穿到
    parity_summary（审计二轮 P2：此前阶段产物丢失该字段，汇总只得 null）。"""
    return {"schema": "miniengine.m6-12.parity.v1", "code": result["code"], "seconds": result["seconds"],
            "status": manifest.get("status"), "contractComplete": manifest.get("contractComplete"),
            "records": len(manifest.get("records", [])),
            "legacyComparisonCount": len(manifest.get("legacyComparisons", [])),
            "crossBackendCount": len(manifest.get("crossBackend", [])),
            "crossBackendResults": manifest.get("crossBackend", []),
            "legacyComparisonResults": manifest.get("legacyComparisons", []),
            "legacyRunResults": manifest.get("legacyRunResults", []),
            "legacySkipped": manifest.get("legacySkipped"),
            "representatives": manifest.get("representatives", []),
            "runManifest": "parity/m610-run.json",
            "runManifestSha256": run_manifest_sha256}

def parity_summary(payload: dict) -> dict:
    """parity 阶段摘要。phase.json 自 2026-09-16 起写 *Results 复数字段
    （crossBackendResults/legacyComparisonResults，与 run manifest 的单复数命名不同）；
    兼容旧字段名，避免汇总再次得到 0/0（审计 D-勘误 2026-09-16）。"""
    legacy = payload.get("legacyComparisonResults", payload.get("legacyComparisons", [])) or []
    cross = payload.get("crossBackendResults", payload.get("crossBackend", [])) or []
    return {"status": payload.get("status"), "records": payload.get("records"),
            "contractComplete": payload.get("contractComplete"),
            "legacyComparisons": len(legacy), "crossBackend": len(cross),
            "legacySkipped": payload.get("legacySkipped")}
