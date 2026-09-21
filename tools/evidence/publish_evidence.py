#!/usr/bin/env python3
"""把本地证据目录发布到外部证据库（`tools/evidence/publish_evidence.ps1` 的等价实现）。

Python 和 PowerShell 版本保持相同 schema、校验顺序与输出格式；修改其一须核对另一版本。

    python tools/evidence/publish_evidence.py \
        --source out/soak-rss-002 --milestone M7 --topic 2026-09-19-m7-soak-rss-002 \
        --destination-root <evidence-store> --require-prereg --note "..."

流程（与 PS 版逐步对应）：
  0) 预注册门（可选）：调用 `tools/evidence/check_m7_prereg.py`，失败即**在写任何目标文件之前**中止；
  1) 收集源文件（跳过 `*.tmp` / `*.partial` / `artifact-manifest.json`，按 `--include` glob 过滤）；
  2) 复制并**回读校验**（源与目标 SHA-256 必须一致，否则失败退出）；
  3) 写 `artifact-manifest.json`（含来源仓库 commit / 脏文件数 / 逐文件 SHA-256）；
  4) 更新顶层 `index.json`（按 `<Milestone>/<Topic>` 去重、按 key 排序）。

幂等：同一 Milestone/Topic 重复发布覆盖同名文件并更新 index 条目，不产生副本目录。
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone

# 与 PS 版一致：按**文件名**跳过临时产物与由发布器自己生成的 manifest。
SKIP_SUFFIXES = ('.tmp', '.partial')
MANIFEST_NAME = 'artifact-manifest.json'
STORE_SCHEMA = 'miniengine.evidence-store.v1'
MANIFEST_SCHEMA = 'miniengine.evidence-artifact-manifest.v1'


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest().upper()


def git(repository: str, *args: str) -> str:
    result = subprocess.run(['git', '-C', repository, *args], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return ''
    return result.stdout


def compact(value) -> str:
    """紧凑 JSON（等价 PS 的 `ConvertTo-Json -Compress`；非 ASCII 原样输出）。"""
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'))


def write_text(path: str, text: str) -> None:
    """PS `Set-Content -Encoding UTF8` 的等价物：UTF-8 **带 BOM** + CRLF。"""
    with open(path, 'w', encoding='utf-8-sig', newline='') as handle:
        handle.write(text)


def read_json(path: str):
    with open(path, encoding='utf-8-sig') as handle:
        return json.load(handle)


def main() -> int:
    parser = argparse.ArgumentParser(description='publish a local evidence directory to the evidence store')
    parser.add_argument('--source', required=True)
    parser.add_argument('--milestone', required=True)
    parser.add_argument('--topic', required=True)
    parser.add_argument('--destination-root', required=True)
    parser.add_argument('--include', default='*', help='逗号分隔的 glob（默认 *）')
    parser.add_argument('--repository', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '../..')))
    parser.add_argument('--note', default='')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--require-prereg', action='store_true')
    parser.add_argument('--prereg-checker', default='')
    args = parser.parse_args()

    source_full = os.path.abspath(args.source)
    if not os.path.isdir(source_full):
        print(f'source not found: {source_full}', file=sys.stderr)
        return 2
    destination = os.path.join(args.destination_root, args.milestone, args.topic)
    manifest_path = os.path.join(destination, MANIFEST_NAME)
    index_path = os.path.join(args.destination_root, 'index.json')

    # 0) 预注册门（可选）：在收集/复制任何文件之前验证证据链，失败即中止。
    if args.require_prereg:
        checker = args.prereg_checker or os.path.join(args.repository, 'tools', 'evidence', 'check_m7_prereg.py')
        if not os.path.isfile(checker):
            print(f'--require-prereg: checker not found at {checker}', file=sys.stderr)
            return 2
        result = subprocess.run([sys.executable, checker, '--dir', source_full], capture_output=True, text=True)
        if result.returncode != 0:
            print('pre-registration gate FAILED; publish aborted before writing anything:', file=sys.stderr)
            print((result.stdout or '') + (result.stderr or ''), file=sys.stderr)
            return 2
        tail = [line for line in (result.stdout or '').splitlines() if line.strip()]
        print(f'pre-registration gate OK: {tail[-1] if tail else ""}')

    # 1) 收集源文件。
    patterns = [item.strip() for item in args.include.split(',') if item.strip()] or ['*']
    selected = []
    for dirpath, dirnames, files in os.walk(source_full):
        dirnames.sort()
        for name in sorted(files):
            lower = name.lower()
            if lower.endswith(SKIP_SUFFIXES) or lower == MANIFEST_NAME:
                continue
            path = os.path.join(dirpath, name)
            relative = os.path.relpath(path, source_full)
            for pattern in patterns:
                if fnmatch.fnmatch(relative, pattern) or fnmatch.fnmatch(name, pattern):
                    selected.append(relative)
                    break
    if not selected:
        print(f'no files selected under {source_full} for include patterns: {", ".join(patterns)}', file=sys.stderr)
        return 2

    print(f'publish {len(selected)} file(s): {source_full} -> {destination}')
    if args.dry_run:
        for relative in selected[:8]:
            print(f'  (dry-run) {relative}')
        return 0

    os.makedirs(destination, exist_ok=True)

    # 2) 复制并回读校验（源哈希 → 目标哈希必须一致）。
    entries = []
    total_bytes = 0
    for relative in selected:
        source_path = os.path.join(source_full, relative)
        target_path = os.path.join(destination, relative)
        target_directory = os.path.dirname(target_path)
        if target_directory:
            os.makedirs(target_directory, exist_ok=True)
        shutil.copy2(source_path, target_path)

        entry = {'path': relative.replace('\\', '/'), 'bytes': os.path.getsize(source_path),
                 'sha256': sha256_file(source_path)}
        copied = sha256_file(target_path)
        if copied != entry['sha256']:
            print(f'copy verification failed for {relative} (source {entry["sha256"]} vs destination {copied})',
                  file=sys.stderr)
            return 3
        entries.append(entry)
        total_bytes += entry['bytes']

    # 3) 写 artifact-manifest.json（含来源仓库 commit / dirty 状态）。
    commit = git(args.repository, 'rev-parse', '--verify', '--quiet', 'HEAD').strip() or None
    dirty_count = len([line for line in git(args.repository, 'status', '--porcelain=v1').splitlines() if line.strip()])
    published_utc = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

    manifest_text = '\r\n'.join([
        '{',
        f'  "schema": {compact(MANIFEST_SCHEMA)},',
        f'  "milestone": {compact(args.milestone)},',
        f'  "topic": {compact(args.topic)},',
        f'  "publishedUtc": {compact(published_utc)},',
        f'  "source": {compact(source_full)},',
        f'  "repository": {compact(args.repository)},',
        f'  "sourceCommit": {compact(commit)},',
        f'  "sourceDirtyFiles": {dirty_count},',
        f'  "fileCount": {len(entries)},',
        f'  "totalBytes": {total_bytes},',
        f'  "note": {compact(args.note)},',
        '  "files": [',
        ',\r\n'.join('    ' + compact(entry) for entry in entries),
        '  ]',
        '}',
    ]) + '\r\n'
    write_text(manifest_path, manifest_text)
    manifest_hash = sha256_file(manifest_path)

    # 4) 更新顶层 index.json（按 <Milestone>/<Topic> 键去重，稳定排序）。
    store = None
    if os.path.isfile(index_path):
        store = read_json(index_path)
        if store.get('schema') != STORE_SCHEMA:
            print(f'unexpected index schema: {store.get("schema")}', file=sys.stderr)
            return 2
    key = f'{args.milestone}/{args.topic}'
    kept = [item for item in (store or {}).get('entries', [])
            if f'{item.get("milestone")}/{item.get("topic")}' != key]
    entry = {
        'key': key,
        'milestone': args.milestone,
        'topic': args.topic,
        'directory': f'{args.milestone}/{args.topic}',
        'publishedUtc': published_utc,
        'fileCount': len(entries),
        'totalBytes': total_bytes,
        'manifest': f'{args.milestone}/{args.topic}/{MANIFEST_NAME}',
        'manifestSha256': manifest_hash,
        'sourceCommit': commit,
        'sourceDirtyFiles': dirty_count,
        'note': args.note,
    }
    ordered = sorted(kept + [entry], key=lambda item: str(item.get('key', '')).lower())
    index_text = '\r\n'.join([
        '{',
        f'  "schema": {compact(STORE_SCHEMA)},',
        f'  "root": {compact(args.destination_root)},',
        f'  "updatedUtc": {compact(published_utc)},',
        '  "entries": [',
        ',\r\n'.join('    ' + compact(item) for item in ordered),
        '  ]',
        '}',
    ]) + '\r\n'
    write_text(index_path, index_text)

    print(f'published {len(entries)} files, {total_bytes / 1048576:.2f} MB; manifest SHA-256 {manifest_hash}')
    print(f'index: {index_path} ({len(ordered)} entries)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
