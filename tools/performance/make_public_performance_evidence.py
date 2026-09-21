"""P7：把冻结的 lb-current-004 逐帧数据整理成**去标识、可复算**的公开证据包。

只做三件事：脱敏（机器路径/主机名/适配器 LUID）、记录 original→public 哈希、复制公开分析器与协议。
逐帧样本、统计、判定字段、运行顺序全部保留，因此公众可以用包内分析器原样复算结论。
不覆盖任何冻结原始文件：输入只读，输出必须写进全新目录。
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

REMOVED_RUN_FIELDS = ("machineManifestPath", "executablePath")
REMOVED_ENV_FIELDS = ("hostName", "adapterLuid")
ABS_PREFIX = "<abs>/"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


DRIVE_PATH = re.compile(r"[A-Za-z]:[\\/][^\s\"]*")


def strip_abs(token):
    """把 token 里的绝对路径压成"从 out/ 起的相对路径"或纯文件名（保留 --flag= 前缀）。"""
    def replace(match):
        normalized = match.group(0).replace("\\", "/")
        marker = "/out/"
        if marker in normalized:
            return normalized[normalized.index(marker) + 1:]
        return ABS_PREFIX + normalized.rsplit("/", 1)[-1]

    return DRIVE_PATH.sub(replace, token)


def sanitize_run(raw):
    removed = []
    for key in REMOVED_RUN_FIELDS:
        if key in raw:
            raw[key] = None
            removed.append(key)
    environment = raw.get("environment")
    if isinstance(environment, dict):
        for key in REMOVED_ENV_FIELDS:
            if key in environment:
                environment[key] = None
                removed.append("environment." + key)
    return removed


def sanitize_summary(summary):
    removed = []
    for run in summary.get("runs", []):
        command = run.get("command")
        if isinstance(command, list):
            new = [strip_abs(token) for token in command]
            if new != command:
                run["command"] = new
                removed.append("runs[].command")
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="冻结的 lb-current-004 目录")
    parser.add_argument("--analyzer", type=Path, default=None, help="公开分析器（默认仓库内同路径）")
    parser.add_argument("--output", type=Path, required=True, help="全新输出目录")
    parser.add_argument("--zip", type=Path, default=None, help="同时产出确定性 ZIP")
    parser.add_argument("--external-checksum", type=Path, default=None, help="写含 ZIP 自身哈希的校验文件")
    args = parser.parse_args()
    source = args.input.resolve()
    analyzer = (args.analyzer or Path(__file__).resolve().parent / "summarize_portfolio_experiment.py").resolve()
    out = args.output.resolve()
    if out.exists():
        parser.error("output must be fresh: " + str(out))
    for path in (source / "PREREGISTRATION.json", source / "formal/summary.json", analyzer):
        if not path.is_file():
            parser.error("missing input: " + str(path))
    formal = read(source / "formal/summary.json")
    run_ids = [run["runId"] for run in formal["runs"]]
    if len(run_ids) != 25:
        parser.error("expected 25 runs, got " + str(len(run_ids)))
    (out / "formal").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / "PREREGISTRATION.json", out / "PREREGISTRATION.json")
    summary = read(source / "formal/summary.json")
    transforms = {}
    removed = sanitize_summary(summary)
    write(out / "formal/summary.json", summary)
    if removed:
        transforms["formal/summary.json"] = {"originalSha256": sha(source / "formal/summary.json"),
                                             "publicSha256": sha(out / "formal/summary.json"),
                                             "fieldsRemoved": sorted(set(removed))}
    for run_id in run_ids:
        for name in ("run.json", "run-notes.json"):
            relative = "formal/" + run_id + "/" + name
            original = source / relative
            if not original.is_file():
                parser.error("missing run artifact: " + relative)
            if name == "run.json":
                raw = read(original)
                fields = sanitize_run(raw)
                write(out / relative, raw)
                if fields:
                    transforms[relative] = {"originalSha256": sha(original),
                                            "publicSha256": sha(out / relative),
                                            "fieldsRemoved": sorted(set(fields))}
            else:
                shutil.copyfile(original, out / relative)
    write(out / "SANITIZATION.json", {
        "schemaVersion": 1,
        "kind": "original -> public hash binding for the desensitized performance evidence pack",
        "note": "未被列出的文件与冻结原件逐字节相同；被列出的文件只做了列出的字段处理，逐帧样本未被改动。",
        "files": transforms})
    (out / "analyzer").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(analyzer, out / "analyzer" / analyzer.name)
    protocol = read(source / "PREREGISTRATION.json")
    readme = [
        "# LumaBough performance evidence pack (desensitized, recomputable)",
        "",
        "本包是 `lb-current-004` 冻结数据的去标识副本，用来让公众在不访问任何私有数据的情况下复算结论。",
        "",
        "## 结论（不因本包而改变）",
        "",
        "- 有效组：25/25 次运行通过；结论 **1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED**。",
        "- 不宣称当前加速；历史 C-M9-002 与历史性能摘要保持 BLOCKED。",
        "- 本包不是新的性能测量，也不代表 v8 运行包的性能。",
        "",
        "## 复算",
        "",
        "```",
        "python analyzer/summarize_portfolio_experiment.py --input . --output recompute.json",
        "```",
        "",
        "（`--output` 是**结果文件**路径；输入目录里已有的冻结文件不会被改写。）",
        "",
        "分析器会校验：运行顺序与身份、控制变量是否漂移、前台门、GPU 有效样本数、",
        "以及存储统计与逐帧重算是否一致；随后重出四个比较的数值与判定。",
        "分析器与预注册的哈希绑定：`analyzerSha256` = " + protocol["analyzerSha256"] + "。",
        "",
        "## 脱敏范围",
        "",
        "- 删除/置空：机器清单绝对路径、EXE 绝对路径、主机名、适配器 LUID；`formal/summary.json` 的 command 数组压成相对形式。",
        "- 保留：实验 ID、运行顺序、有效性字段、控制变量、逐帧样本（每次 600 帧）、统计、判定字段。",
        "- `SANITIZATION.json` 记录每个被处理文件的 `originalSha256` → `publicSha256`，可与冻结记录对账。",
        "- 未随包：机器清单全文、源码快照、构建日志、截图、原始 Capture。",
        "",
        "## 限制",
        "",
        "- 单机、五 cell、每 cell 五次、D3D12、VSync off；不是跨机器或通用加速结论。",
        "- 逐帧样本可复算分布与尾部指标；构建与运行环境的原始文件只在本地保留。",
        ""]
    (out / "README.md").write_text("\n".join(readme), encoding="utf-8")
    files = sorted(p for p in out.rglob("*") if p.is_file())
    (out / "SHA256SUMS.txt").write_text(
        "\n".join(sha(p) + "  " + p.relative_to(out).as_posix() for p in files) + "\n", encoding="utf-8")
    if args.zip:
        names = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
        with zipfile.ZipFile(args.zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name in names:
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, (out / name).read_bytes())
        zip_sha = sha(args.zip)
        if args.external_checksum:
            args.external_checksum.parent.mkdir(parents=True, exist_ok=True)
            args.external_checksum.write_text(
                "# 下载后先校验 ZIP 自身，再按包内 SHA256SUMS.txt 校验逐文件\n"
                + zip_sha + "  " + args.zip.name + "\n", encoding="utf-8")
        print(json.dumps({"files": len(names), "bytes": args.zip.stat().st_size, "zipSha256": zip_sha,
                          "sanitizedFiles": len(transforms)}, ensure_ascii=False))
    else:
        print(json.dumps({"files": len(files), "sanitizedFiles": len(transforms)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
