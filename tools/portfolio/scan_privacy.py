"""E3：隐私/内容扫描器。只产出待审项，绝不自动批准；逐项裁定记录在基线文件里。

三个目标分开扫（互不代替）：
  source-set  公开集合（docs/evidence/PUBLICATION-FILES.json 里列出的文件，按实际字节）
  git         Git 待提交内容（git ls-files 的实际工作树字节，不读 .gitignore 结论）
  package     包解压后的实际字节（目录或 .zip 都支持）

判定：findings 默认 pending；基线文件里 status=approved 的项视为已裁定（保留/脱敏/排除 + 理由），
status=proposed 视为"已提处置、待所有者批准"。仍有未裁定项时退出码为 1。
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import zipfile
from pathlib import Path
sys.dont_write_bytecode = True
from audit_import import safe_root
from validate_import import MANIFEST

TEXT_SUFFIX = {".md", ".txt", ".json", ".py", ".cpp", ".h", ".hpp", ".hlsl", ".hlsli", ".cmake",
               ".yml", ".yaml", ".xml", ".svg", ".ps1", ".bat", ".cmd", ".csv", ".ini", ".cfg",
               ".toml", ".html", ".css", ".js", ".glsl", ".md5", ".sums", ".gitignore", ".editorconfig"}
FORBIDDEN_SUFFIX = {".pdb", ".rdc", ".wpix", ".mp4", ".mkv", ".mov", ".avi", ".ppm", ".wav",
                    ".ilk", ".obj", ".lib", ".exp", ".pyc", ".pdb", ".dmp", ".zip"}
CACHE_PARTS = {"__pycache__", "node_modules", ".vs", ".git", ".cache", "out", "build"}


def text_like(data):
    return b"\x00" not in data[:8192]


def rules(user, machine):
    out = []

    def add(rid, severity, pattern, why, flags=0):
        out.append((rid, severity, re.compile(pattern, flags), why))

    add("absolute-path", "review", r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]{1,2}[^\s\"'<>)|,;]*",
        "Windows 绝对路径（盘符），公开文档里应改为占位符")
    add("unc-path", "review", r"\\\\[A-Za-z0-9._-]+\\[^\s\"'<>)|,;]*", "UNC/网络路径")
    add("user-profile", "block", r"(?i)[\\/](users|appdata|documents and settings|temp)[\\/]",
        "用户目录/临时目录引用")
    if user:
        add("username", "block", r"(?<![A-Za-z0-9_])" + re.escape(user) + r"(?![A-Za-z0-9_])",
            "本机用户名出现")
    if machine:
        add("machine-name", "block", r"(?<![A-Za-z0-9_-])" + re.escape(machine) + r"(?![A-Za-z0-9_-])",
            "本机机器名出现")
    add("email", "review", r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "邮箱地址")
    add("private-key", "block", r"-----BEGIN [A-Z ]*PRIVATE KEY-----", "私钥块")
    add("token-pattern", "block", r"\b(ghp_[A-Za-z0-9]{20,}|gho_[A-Za-z0-9]{20,}|AKIA[0-9A-Z]{16}|sk-[A-Za-z0-9]{20,})\b",
        "疑似令牌/密钥")
    add("secret-assignment", "block",
        r"(?i)\b(api[_-]?key|secret|password|passwd|token|credential)\b\s*[:=]\s*['\"][^'\"]{6,}['\"]",
        "疑似硬编码凭据")
    add("credential-url", "block", r"://[^/\s:@]{1,64}:[^/\s@]{3,64}@", "URL 内嵌凭据")
    add("local-endpoint", "review", r"(?i)\b(localhost|127\.0\.0\.1)\b", "本机端点")
    add("history-number", "review", r"\b16\.44\b", "历史 C-M9-002 数值，不得写成当前包收益")
    add("foreign-repo", "review", r"github\.com/[^\s/\"'<>)]+/(?i:miniengine)",
        "外部来源 ID，仅可作为历史来源署名")
    return out


def scan_bytes(data, display, findings, compiled):
    if not text_like(data):
        return
    text = data.decode("utf-8", "replace")
    for rid, severity, rx, why in compiled:
        for match in rx.finditer(text):
            fragment = match.group(0)
            findings.append({
                "rule": rid,
                "severity": severity,
                "path": display,
                "line": text.count("\n", 0, match.start()) + 1,
                "match": fragment if len(fragment) <= 120 else fragment[:117] + "...",
                "matchSha256": hashlib.sha256(fragment.encode("utf-8", "replace")).hexdigest(),
                "why": why,
            })


def scan_file(path, display, findings, compiled):
    data = Path(path).read_bytes()
    suffix = Path(display).suffix.lower()
    if suffix in FORBIDDEN_SUFFIX:
        findings.append({"rule": "forbidden-artifact", "severity": "block", "path": display,
                         "line": 0, "match": suffix, "matchSha256": hashlib.sha256(suffix.encode()).hexdigest(),
                         "why": "不应公开的产物类型（PDB/Capture/视频/中间文件）"})
    if CACHE_PARTS & set(Path(display).parts):
        findings.append({"rule": "cache-or-build-dir", "severity": "block", "path": display,
                         "line": 0, "match": str(Path(display).parts[0]),
                         "matchSha256": hashlib.sha256(str(Path(display).parts[0]).encode()).hexdigest(),
                         "why": "缓存/构建目录内容"})
    scan_bytes(data, display, findings, compiled)


def target_source_set(root, findings, compiled):
    data = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    names = [row["path"] for row in data["files"]]
    for name in names:
        path = root / name
        if path.is_file():
            scan_file(path, name, findings, compiled)
    return {"target": "source-set", "files": len(names)}


def target_git(root, findings, compiled):
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    names = [n for n in out.stdout.decode("utf-8", "replace").split("\0") if n]
    scanned = 0
    for name in names:
        path = root / name
        if not path.is_file():
            continue
        suffix = path.suffix.lower()
        if suffix not in TEXT_SUFFIX and not text_like(path.read_bytes()[:8192]):
            continue
        scan_file(path, name, findings, compiled)
        scanned += 1
    return {"target": "git", "tracked": len(names), "scanned": scanned}


def target_package(package, findings, compiled):
    scanned = 0
    if package.is_dir():
        for path in sorted(package.rglob("*")):
            if path.is_file():
                scan_file(path, path.relative_to(package).as_posix(), findings, compiled)
                scanned += 1
    elif package.suffix.lower() == ".zip":
        with zipfile.ZipFile(package) as archive:
            for info in sorted(archive.infolist(), key=lambda i: i.filename):
                if info.is_dir():
                    continue
                scan_bytes(archive.read(info), info.filename, findings, compiled)
                scanned += 1
    else:
        raise ValueError("package must be a directory or .zip: " + str(package))
    return {"target": "package", "path": str(package), "scanned": scanned}


def load_baseline(path):
    if path is None or not Path(path).exists():
        return {}
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return {(row["rule"], row["path"], row["matchSha256"]): row for row in data.get("items", [])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--source-set", action="store_true", help="扫公开集合（PUBLICATION-FILES 列表）")
    parser.add_argument("--git", action="store_true", help="扫 Git 待提交内容")
    parser.add_argument("--package", type=Path, default=None, help="扫包目录或 .zip 的实际字节")
    parser.add_argument("--baseline", type=Path, default=None, help="逐项裁定基线 JSON")
    parser.add_argument("--output", type=Path, default=None, help="报告 JSON 输出路径")
    parser.add_argument("--markdown", type=Path, default=None, help="待审清单 Markdown 输出路径")
    parser.add_argument("--user", default=os.environ.get("USERNAME", ""), help="要判定的用户名（默认当前用户）")
    parser.add_argument("--machine", default=platform.node(), help="要判定的机器名（默认本机）")
    args = parser.parse_args()
    root = safe_root(args.root)
    if not (args.source_set or args.git or args.package):
        parser.error("choose at least one target: --source-set / --git / --package")
    compiled = rules(args.user, args.machine)
    findings = []
    targets = []
    if args.source_set:
        targets.append(target_source_set(root, findings, compiled))
    if args.git:
        targets.append(target_git(root, findings, compiled))
    if args.package:
        targets.append(target_package(args.package.resolve(), findings, compiled))
    baseline = load_baseline(args.baseline)
    unadjudicated, pending = [], []
    for row in findings:
        record = baseline.get((row["rule"], row["path"], row["matchSha256"]))
        if record is None:
            row["disposition"] = "pending"
            row["reason"] = ""
            unadjudicated.append(row)
        else:
            row["disposition"] = record["disposition"]
            row["reason"] = record.get("reason", "")
            row["status"] = record.get("status", "proposed")
            if record.get("status", "proposed") != "approved":
                pending.append(row)
    findings.sort(key=lambda r: (r["severity"] != "block", r["path"], r["line"], r["rule"]))
    summary = {
        "findings": len(findings),
        "bySeverity": {s: sum(1 for f in findings if f["severity"] == s) for s in ("block", "review")},
        "byRule": {r: sum(1 for f in findings if f["rule"] == r) for r in sorted({f["rule"] for f in findings})},
        "unadjudicated": len(unadjudicated),
        "pendingApproval": len(pending),
        "userChecked": bool(args.user),
        "machineChecked": bool(args.machine),
    }
    report = {"schemaVersion": 1, "stage": "E3", "targets": targets, "summary": summary,
              "findings": findings, "unadjudicated": unadjudicated}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        lines = ["# E3 隐私/内容待审清单", "",
                 "由 `tools/portfolio/scan_privacy.py` 生成；自动扫描只产出待审项，裁定必须逐项人工确认。", "",
                 "| 严重度 | 规则 | 位置 | 行 | 匹配（截断） | 处置 | 理由 |", "|---|---|---|---|---|---|---|"]
        for row in findings:
            fragment = row["match"].replace("|", "\\|")
            lines.append(f"| {row['severity']} | {row['rule']} | `{row['path']}` | {row['line']} | `{fragment}` | "
                         f"{row['disposition']} | {row.get('reason', '')} |")
        lines += ["", f"合计 {len(findings)} 项：block {summary['bySeverity']['block']}、"
                      f"review {summary['bySeverity']['review']}；未裁定 {len(unadjudicated)}、待批准 {len(pending)}。"]
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))
    return 1 if (unadjudicated or pending) else 0


if __name__ == "__main__":
    raise SystemExit(main())
