#!/usr/bin/env python3
"""
deploy_to_github.py
===================
把 desktop_index.json 推到 GitHub Pages，老板可随时访问。

跟 banli-homework 走一样的 gh api PUT 模式（不走 git push，VPN 不通 git protocol）。

用法:
  python3 deploy_to_github.py                 # 正常推送
  python3 deploy_to_github.py --dry-run       # 看会做什么，不执行
  python3 deploy_to_github.py --skip-scan     # 跳过扫描，只推已有 JSON

前置（一次性配置）:
  1. 在 GitHub 创建 repo: JayYangstore/desktop-ai-passport（public）
  2. 启用 Pages: Settings → Pages → Source: main / root
  3. gh CLI 已登录: gh auth login（token 要 repo + workflow 权限）
  4. 第一次 push 后，Pages URL 几秒内生效:
     https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json

作者: Hermes Agent
日期: 2026-10-08
"""

import argparse
import base64
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional

# === 配置 ===
GITHUB_OWNER = "JayYangstore"
GITHUB_REPO = "desktop-ai-passport"
GITHUB_BRANCH = "main"
JSON_FILENAME = "desktop_index.json"
SCRIPT_DIR = Path(__file__).parent
JSON_PATH = SCRIPT_DIR / JSON_FILENAME
PAGES_URL = f"https://jayyangstore.github.io/{GITHUB_REPO}/{JSON_FILENAME}"


def run_cmd(cmd: list, dry_run: bool = False, check: bool = True) -> tuple[int, str, str]:
    """执行命令，返回 (returncode, stdout, stderr)。"""
    print(f"  $ {' '.join(cmd)}")
    if dry_run:
        return 0, "", ""
    result = subprocess.run(cmd, capture_output=True, text=True)
    if check and result.returncode != 0:
        print(f"❌ 命令失败 (exit {result.returncode}):", file=sys.stderr)
        print(f"   stderr: {result.stderr}", file=sys.stderr)
        sys.exit(result.returncode)
    return result.returncode, result.stdout, result.stderr


def get_remote_sha(dry_run: bool = False) -> Optional[str]:
    """取远程文件当前 sha（用于 PUT 冲突检测）。不存在返回 None。"""
    cmd = [
        "gh", "api",
        f"repos/{GITHUB_OWNER}/{GITHUB_REPO}/contents/{JSON_FILENAME}",
        f"--jq", ".sha",
    ]
    rc, stdout, stderr = run_cmd(cmd, dry_run=dry_run, check=False)
    if rc != 0:
        if "Not Found" in stderr or "404" in stderr:
            return None
        print(f"⚠️  取远程 sha 失败: {stderr}", file=sys.stderr)
        return None
    sha = stdout.strip()
    return sha if sha and sha != "null" else None


def main():
    parser = argparse.ArgumentParser(description="推 desktop_index.json 到 GitHub Pages")
    parser.add_argument("--dry-run", action="store_true", help="只看会做什么，不执行")
    parser.add_argument("--skip-scan", action="store_true", help="跳过扫描，只推已有 JSON")
    args = parser.parse_args()

    # === Step 1: 扫描 ===
    if not args.skip_scan:
        print("🔍 步骤 1/4: 扫描 Mac 桌面/下载/文档...")
        scanner = SCRIPT_DIR / "desktop_index_scanner.py"
        rc, _, _ = run_cmd(["python3", str(scanner)], dry_run=args.dry_run)
    else:
        print("⏭️  步骤 1/4: 跳过扫描（--skip-scan）")

    # === Step 2: 校验 JSON ===
    print("✅ 步骤 2/4: 校验 JSON...")
    if not JSON_PATH.exists():
        print(f"❌ 找不到 {JSON_PATH}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(JSON_PATH, encoding="utf-8") as f:
            data = json.load(f)
        size_kb = JSON_PATH.stat().st_size / 1024
        print(f"   文件: {JSON_PATH.name} ({size_kb:.1f} KB, {data['total']} 条)")
    except json.JSONDecodeError as e:
        print(f"❌ JSON 解析失败: {e}", file=sys.stderr)
        sys.exit(1)

    if size_kb > 50:
        print(f"⚠️  警告: JSON {size_kb:.1f}KB > 50KB，可能不适合 AI Passport")
        print(f"   建议修改 desktop_index_scanner.py 减小 MAX_ENTRIES")

    # === Step 3: 取远程 sha ===
    print("🔗 步骤 3/4: 取远程文件 sha...")
    remote_sha = get_remote_sha(dry_run=args.dry_run)
    if remote_sha:
        print(f"   远程 sha: {remote_sha[:12]}...")
    else:
        print(f"   远程文件不存在（首次推送）")

    # === Step 4: PUT 到 GitHub ===
    print("📤 步骤 4/4: PUT 到 GitHub...")

    # 读 + base64 编码
    with open(JSON_PATH, "rb") as f:
        content_b64 = base64.b64encode(f.read()).decode("ascii")

    commit_msg = f"chore: update desktop_index.json - {data.get('generated_at_human', 'unknown')}"

    # 构造 PUT payload
    payload = {
        "message": commit_msg,
        "content": content_b64,
        "branch": GITHUB_BRANCH,
    }
    if remote_sha:
        payload["sha"] = remote_sha

    payload_file = SCRIPT_DIR / ".deploy_payload.json"
    with open(payload_file, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False)

    if args.dry_run:
        print(f"   [DRY-RUN] 会 PUT 到: repos/{GITHUB_OWNER}/{GITHUB_REPO}/contents/{JSON_FILENAME}")
        print(f"   [DRY-RUN] commit msg: {commit_msg}")
        payload_file.unlink()
    else:
        put_cmd = [
            "gh", "api",
            "--method", "PUT",
            f"repos/{GITHUB_OWNER}/{GITHUB_REPO}/contents/{JSON_FILENAME}",
            "--input", str(payload_file),
        ]
        rc, stdout, stderr = run_cmd(put_cmd, check=False)
        payload_file.unlink()

        if rc != 0:
            print(f"❌ PUT 失败: {stderr}", file=sys.stderr)
            sys.exit(1)

        # 解析返回的 commit sha
        try:
            resp = json.loads(stdout)
            commit_sha = resp.get("commit", {}).get("sha", "?")[:12]
            print(f"   ✅ 已提交: {commit_sha}")
        except (json.JSONDecodeError, KeyError):
            print(f"   ✅ PUT 成功")

    # === 完成 ===
    print()
    print("🎉 推送完成！")
    print(f"   GitHub Pages URL: {PAGES_URL}")
    print(f"   AI Passport v2 联网时会自动拉这份 JSON")


if __name__ == "__main__":
    main()
