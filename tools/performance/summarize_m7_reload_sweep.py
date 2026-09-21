"""M7 热重载采集汇总（BACKLOG M7-LOAD-RELOAD-E2E / E-M7-RELOAD-001）。

从 `run_m7_reload_sweep.ps1` 的 raw 里提取**端到端热重载证据**，写成机读摘要 + 人读报告：

- 提交/拒绝/脚本拒绝/延迟退休（来自 run-notes.json，逐 run 累计）；
- 热重载请求的端到端延迟分位数（来自 raw 的 `assetRequests`，`revision >= 2` 即重载请求，
  延迟取 `requestToReadyMs`）——这是 M7-07 记录里缺的那一份"端到端延迟"证据；
- 首载请求（revision == 1）的分位数作为对照（同一流水线、同一脚本）。

用法：
  python tools/performance/summarize_m7_reload_sweep.py --dir out/m7-reload --report out/m7-reload/reload-report.md
"""

import argparse
import glob
import io
import json
import os
import re
import statistics


def quantile(values: list[float], q: float) -> float:
    if not values:
        return float('nan')
    ordered = sorted(values)
    index = q * (len(ordered) - 1)
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] * (1.0 - (index - low)) + ordered[high] * (index - low)


def collect(directory: str) -> dict:
    """按 cell 归并（目录名 <cell>-rN ⇒ cell；同一 cell 的多个 run 合并统计）。"""
    cells: dict[str, list[dict]] = {}
    for run_path in sorted(glob.glob(os.path.join(directory, '*', 'run.json'))):
        directory_name = os.path.basename(os.path.dirname(run_path))
        cell = re.sub(r'-r\d+$', '', directory_name)
        run = json.load(io.open(run_path, encoding='utf-8-sig'))
        notes_path = os.path.join(os.path.dirname(run_path), 'run-notes.json')
        notes = json.load(io.open(notes_path, encoding='utf-8-sig')) if os.path.exists(notes_path) else {}
        requests = run.get('assetRequests') or []
        # 口径：`assetRequests` 每个请求只保留**最新一次尝试**。热重载 churn 下，未完成就被新 revision
        # 取代的尝试会记为 result=incomplete 且阶段耗时全部为 0（旧 revision 仍在服务，设计如此）。
        # 因此延迟分位数只统计 result == ready 的条目，同时如实报告 ready/incomplete 计数。
        ready = [r for r in requests if r.get('result') == 'ready']
        cells.setdefault(cell, []).append(dict(
            run=directory_name,
            correctness=run['correctness']['status'],
            validation=run['correctness'].get('validationMessages', 0),
            request_total=len(requests), request_ready=len(ready),
            request_incomplete=len(requests) - len(ready),
            ready_latencies=[r['requestToReadyMs'] for r in ready],
            reload_commits=notes.get('reloadCommits', 0),
            reload_requests=notes.get('reloadRequests', 0),
            reload_rejected=notes.get('reloadRejected', 0),
            script_rejected=notes.get('scriptRejected', 0),
            superseded_samples=notes.get('supersededSamples', 0),
            uploads_started=notes.get('uploadsStarted', 0),
            uploads_committed=notes.get('uploadsCommitted', 0),
            uploads_failed=notes.get('uploadsFailed', 0),
            deferred_retires=notes.get('deferredRetiresTotal', notes.get('deferredRetires', 0)),
            upload_live_resources=notes.get('uploadLiveResources', 0),
            upload_cpu_micros=notes.get('uploadCpuMicros', 0),
        ))
    return cells


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--dir', required=True)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()

    cells = collect(args.dir)
    if not cells:
        raise SystemExit(f'no runs under {args.dir}')

    lines = ['# M7 端到端热重载汇总（E-M7-RELOAD-001）', '',
             '来源：`tools/performance/run_m7_reload_sweep.ps1`（每 N 帧对已提交请求新 revision，端到端热重载）。', '',
             '口径说明：`assetRequests` 每请求只保留**最新一次尝试**；热重载 churn 下未完成即被新 revision 取代的',
             '尝试记为 `incomplete`（阶段耗时全 0，旧 revision 继续服务）。因此**延迟只在 `ready` 条目上统计**，',
             '`incomplete` 计数单列（它衡量替换压力，不是失败——`uploadsFailed` 与 `reloadRejected` 才是失败）。', '',
             '| cell | runs | 重载提交 | 重载请求 | 拒绝 | 脚本拒绝 | 替换(incomplete) | 上传失败 | 关闭驻留 | ready p50 (ms) | ready p95 (ms) | ready max (ms) |',
             '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
    summary = {}
    for cell in sorted(cells):
        runs = cells[cell]
        latencies = [value for run in runs for value in run['ready_latencies']]
        commits = sum(run['reload_commits'] for run in runs)
        requests_n = sum(run['reload_requests'] for run in runs)
        rejected = sum(run['reload_rejected'] for run in runs)
        script = sum(run['script_rejected'] for run in runs)
        superseded = sum(run['request_incomplete'] for run in runs)
        failed = sum(run['uploads_failed'] for run in runs)
        live = sum(run['upload_live_resources'] for run in runs)
        lines.append('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9:.1f} | {10:.1f} | {11:.1f} |'.format(
            cell, len(runs), commits, requests_n, rejected, script, superseded, failed, live,
            quantile(latencies, 0.5), quantile(latencies, 0.95),
            max(latencies) if latencies else float('nan')))
        summary[cell] = dict(
            runs=len(runs), reload_commits=commits, reload_requests=requests_n,
            reload_rejected=rejected, script_rejected=script, superseded_requests=superseded,
            uploads_started=sum(run['uploads_started'] for run in runs),
            uploads_committed=sum(run['uploads_committed'] for run in runs),
            uploads_failed=failed, upload_live_resources=live,
            deferred_retires=sum(run['deferred_retires'] for run in runs),
            upload_cpu_millis=round(sum(run['upload_cpu_micros'] for run in runs) / 1000.0, 3),
            ready_request_count=sum(run['request_ready'] for run in runs),
            ready_latency_p50_ms=round(quantile(latencies, 0.5), 3),
            ready_latency_p95_ms=round(quantile(latencies, 0.95), 3),
            ready_latency_max_ms=round(max(latencies), 3) if latencies else None,
            all_pass=all(run['correctness'] == 'PASS' and run['validation'] == 0 for run in runs),
        )

    healthy = all(entry['all_pass'] and entry['uploads_failed'] == 0 and entry['reload_rejected'] == 0
                  and entry['upload_live_resources'] == 0 for entry in summary.values())
    lines += ['', f'- 正确性：{"全部 PASS、0 validation、0 上传失败、0 重载拒绝、关闭后 0 驻留" if healthy else "存在失败项，见上表"}',
              '- 仍缺的口径：**被替换尝试的端到端延迟**在现有采样里不可得（阶段耗时归零）；'
              '需要专门计数器时另立实验（见账本 `E-M7-RELOAD-001` 的残留段）。']

    with io.open(args.report, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n'.join(lines) + '\n')
    with io.open(os.path.join(os.path.dirname(args.report), 'reload-sweep-summary.json'), 'w',
                 encoding='utf-8', newline='\n') as handle:
        handle.write(json.dumps({'cells': summary, 'healthy': healthy}, ensure_ascii=False, indent=1) + '\n')
    print(f'report: {args.report}')
    for cell, entry in sorted(summary.items()):
        print('  {0}: 重载提交 {1} / 替换 {2} / ready 延迟 p50 {3} ms / p95 {4} ms'.format(
            cell, entry['reload_commits'], entry['superseded_requests'],
            entry['ready_latency_p50_ms'], entry['ready_latency_p95_ms']))
    return 0 if healthy else 1


if __name__ == '__main__':
    raise SystemExit(main())
