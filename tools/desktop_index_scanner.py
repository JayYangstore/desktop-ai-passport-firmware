#!/usr/bin/env python3
"""
desktop_index_scanner.py
========================
扫描 Mac 桌面/下载/文档 顶层目录 → 生成 AI Passport 能用的精简索引 JSON。

输出: desktop_index.json（控制在 30KB 以内，AI Passport 8MB Flash 装得下）

作者: Hermes Agent
日期: 2026-10-08
"""

import json
import os
import platform
import sys
from datetime import datetime, timedelta
from pathlib import Path

# === 配置 ===
HOME = Path.home()
SCAN_DIRS = [
    HOME / "Desktop",
    HOME / "Downloads",
    HOME / "Documents",
]
# AI Passport 屏幕 240x320，一屏能装 ~10 条
# 总条数太多会爆 Flash，先限 200 条最重要的
MAX_ENTRIES = 200
# 忽略的目录（系统垃圾）
IGNORE_DIRS = {
    ".DS_Store", ".Trash", ".git", ".svn", "node_modules",
    "__pycache__", ".cache", ".npm", ".next", "venv", ".venv",
    "Library",  # macOS 用户库
    "Applications",
}
# 忽略的文件
IGNORE_FILES = {
    ".DS_Store", ".localized", "Thumbs.db",
}

# 文件类型 → emoji
TYPE_ICON = {
    "pdf":  "📄",
    "doc":  "📝", "docx": "📝",
    "xls":  "📊", "xlsx": "📊", "csv": "📊",
    "ppt":  "🎞",  "pptx": "🎞",
    "txt":  "📃", "md": "📃",
    "jpg":  "🖼", "jpeg": "🖼", "png": "🖼", "gif": "🖼", "heic": "🖼",
    "mp4":  "🎬", "mov": "🎬", "avi": "🎬",
    "mp3":  "🎵", "wav": "🎵", "m4a": "🎵",
    "zip":  "🗜", "rar": "🗜", "7z": "🗜", "tar": "🗜", "gz": "🗜",
    "html": "🌐", "htm": "🌐",
    "json": "⚙️", "xml": "⚙️", "yml": "⚙️", "yaml": "⚙️",
    "py":   "🐍", "js": "📜", "ts": "📜", "sh": "📜",
    "app":  "📦", "dmg": "📦", "pkg": "📦",
    "sketch": "🎨", "fig": "🎨", "psd": "🎨", "ai": "🎨",
}

# 文件类型分类（用于分组/着色）
TYPE_CATEGORY = {
    "pdf":  "doc",
    "doc":  "doc", "docx": "doc", "txt": "doc", "md": "doc",
    "xls":  "data", "xlsx": "data", "csv": "data", "json": "data",
    "ppt":  "media", "pptx": "media",
    "jpg":  "image", "jpeg": "image", "png": "image", "gif": "image", "heic": "image",
    "mp4":  "video", "mov": "video", "avi": "video",
    "mp3":  "audio", "wav": "audio", "m4a": "audio",
    "zip":  "archive", "rar": "archive", "7z": "archive", "tar": "archive", "gz": "archive",
}


def get_icon(filename: str) -> str:
    """根据扩展名返回 emoji 图标。"""
    ext = filename.rsplit(".", 1)[-1].lower() if "." in filename else ""
    return TYPE_ICON.get(ext, "📁")


def get_category(filename: str) -> str:
    """根据扩展名返回分类。"""
    ext = filename.rsplit(".", 1)[-1].lower() if "." in filename else ""
    return TYPE_CATEGORY.get(ext, "other")


def human_size(size_bytes: int) -> str:
    """人类可读的文件大小。"""
    if size_bytes < 1024:
        return f"{size_bytes}B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes // 1024}KB"
    elif size_bytes < 1024 * 1024 * 1024:
        return f"{size_bytes // (1024*1024)}MB"
    else:
        return f"{size_bytes // (1024*1024*1024)}GB"


def relative_time(mtime: float) -> str:
    """返回 '3天前' 这样的相对时间。"""
    delta = datetime.now() - datetime.fromtimestamp(mtime)
    days = delta.days
    if days == 0:
        hours = delta.seconds // 3600
        if hours == 0:
            return "刚刚"
        return f"{hours}小时前"
    elif days == 1:
        return "昨天"
    elif days < 7:
        return f"{days}天前"
    elif days < 30:
        return f"{days // 7}周前"
    elif days < 365:
        return f"{days // 30}个月前"
    else:
        return f"{days // 365}年前"


def scan_one_level(directory: Path) -> list:
    """扫描单层目录（不递归），返回条目列表。"""
    entries = []
    if not directory.exists():
        return entries

    try:
        for item in sorted(directory.iterdir(), key=lambda p: p.name.lower()):
            # 过滤
            if item.name.startswith("."):
                continue
            if item.name in IGNORE_FILES:
                continue
            if item.is_dir() and item.name in IGNORE_DIRS:
                continue

            try:
                stat = item.stat()
            except (PermissionError, OSError):
                continue

            entry = {
                "n": item.name,                              # name（短）
                "p": str(item.relative_to(HOME)),            # path（相对 ~）
                "t": "d" if item.is_dir() else "f",          # type
                "s": stat.st_size if item.is_file() else 0,  # size bytes
                "m": int(stat.st_mtime),                     # mtime unix
                "i": "" if item.is_dir() else get_icon(item.name),
                "c": "" if item.is_dir() else get_category(item.name),
            }
            entries.append(entry)
    except PermissionError:
        pass

    return entries


def build_index() -> dict:
    """扫描所有配置目录，生成索引。"""
    all_entries = []
    sources = []

    for scan_dir in SCAN_DIRS:
        if not scan_dir.exists():
            continue
        entries = scan_one_level(scan_dir)
        sources.append({
            "dir": str(scan_dir.relative_to(HOME)),
            "count": len(entries),
        })
        # 标记来源
        for e in entries:
            e["src"] = scan_dir.name  # "Desktop" / "Downloads" / "Documents"
        all_entries.extend(entries)

    # 按修改时间倒序（最新的在前）
    all_entries.sort(key=lambda e: e["m"], reverse=True)

    # 限条数（保留最新的）
    if len(all_entries) > MAX_ENTRIES:
        all_entries = all_entries[:MAX_ENTRIES]

    index = {
        "version": 1,
        "generated_at": int(datetime.now().timestamp()),
        "generated_at_human": datetime.now().strftime("%Y-%m-%d %H:%M"),
        "host": os.uname().nodename if hasattr(os, 'uname') else platform.node(),
        "sources": sources,
        "total": len(all_entries),
        "entries": all_entries,
    }

    return index


def main():
    """主入口。"""
    output_path = Path(__file__).parent / "desktop_index.json"

    print("🔍 扫描桌面/下载/文档顶层目录...")
    print(f"   HOME: {HOME}")
    for d in SCAN_DIRS:
        exists = "✅" if d.exists() else "❌"
        print(f"   {exists} {d.relative_to(HOME)}")

    index = build_index()

    # 写文件
    json_str = json.dumps(index, ensure_ascii=False, separators=(",", ":"))
    output_path.write_text(json_str, encoding="utf-8")

    size_kb = len(json_str.encode("utf-8")) / 1024

    print()
    print(f"✅ 生成完成: {output_path}")
    print(f"   条目数: {index['total']}")
    print(f"   文件大小: {size_kb:.1f} KB")
    print(f"   生成时间: {index['generated_at_human']}")

    # 跑得通自检：必须 < 50KB，否则 AI Passport Flash 装不下
    if size_kb > 50:
        print(f"⚠️  警告: 文件 > 50KB，建议减少 MAX_ENTRIES")
        sys.exit(1)

    # 最新几条预览
    print()
    print("📋 最新 5 条:")
    for e in index["entries"][:5]:
        icon = e.get("i") or "📁"
        rel = relative_time(e["m"])
        print(f"   {icon} {e['n'][:40]:<40} [{rel}]")


if __name__ == "__main__":
    main()
