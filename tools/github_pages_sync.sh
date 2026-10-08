#!/bin/bash
# github_pages_sync.sh
# ====================
# 把 desktop_index.json 推到 GitHub Pages，老板可随时访问
#
# 用法:
#   ./github_pages_sync.sh                  # 正常推送
#   ./github_pages_sync.sh --skip-scan      # 跳过扫描，只推已有 JSON
#   ./github_pages_sync.sh --dry-run        # 看会做什么但不执行
#
# 前置条件（一次性配置）:
#   1. 建 GitHub repo: jayyangstore/desktop-ai-passport
#   2. 启用 Pages: Settings → Pages → Source: main / root
#   3. 本地 git 已配 SSH key（参考 skill: github-auth）
#
# 自动 cron（每天凌晨 3 点跑）:
#   crontab -e
#   0 3 * * * cd ~/Downloads/Hermes/ai-passport/tools && ./github_pages_sync.sh >> sync.log 2>&1

set -e

# === 路径 ===
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
GIT_REPO_DIR="$HOME/jayyangstore.github.io"  # 假设老板主页 repo 已克隆
INDEX_FILE_NAME="desktop_index.json"
JSON_SRC="$SCRIPT_DIR/$INDEX_FILE_NAME"
JSON_DST="$GIT_REPO_DIR/$INDEX_FILE_NAME"
PAGES_URL="https://jayyangstore.github.io/desktop_index.json"

# === 参数解析 ===
SKIP_SCAN=false
DRY_RUN=false
for arg in "$@"; do
    case $arg in
        --skip-scan) SKIP_SCAN=true ;;
        --dry-run)   DRY_RUN=true ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

# === 颜色输出 ===
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $1"; }
warn() { echo -e "${YELLOW}[$(date +%H:%M:%S)]${NC} ⚠️  $1"; }
err() { echo -e "${RED}[$(date +%H:%M:%S)]${NC} ❌ $1"; }

# === 步骤 1: 扫描（除非 --skip-scan）===
if [ "$SKIP_SCAN" = false ]; then
    log "步骤 1/4: 扫描 Mac 桌面/下载/文档..."
    if [ "$DRY_RUN" = true ]; then
        echo "  [DRY-RUN] 会跑: python3 $SCRIPT_DIR/desktop_index_scanner.py"
    else
        python3 "$SCRIPT_DIR/desktop_index_scanner.py"
    fi
else
    log "步骤 1/4: 跳过扫描（--skip-scan）"
fi

# === 步骤 2: 校验 JSON ===
log "步骤 2/4: 校验 JSON 文件..."
if [ ! -f "$JSON_SRC" ]; then
    err "找不到 $JSON_SRC"
    err "先跑扫描器: python3 $SCRIPT_DIR/desktop_index_scanner.py"
    exit 1
fi

if [ "$DRY_RUN" = true ]; then
    SIZE=$(wc -c < "$JSON_SRC" | tr -d ' ')
    echo "  [DRY-RUN] JSON 大小: ${SIZE} bytes"
    # Python 校验 JSON 合法性
    python3 -c "import json; json.load(open('$JSON_SRC')); print('  [DRY-RUN] JSON 合法 ✅')"
else
    SIZE=$(wc -c < "$JSON_SRC" | tr -d ' ')
    python3 -c "import json; json.load(open('$JSON_SRC'))" || {
        err "JSON 解析失败"
        exit 1
    }
    log "  JSON 大小: ${SIZE} bytes ✅"
fi

# === 步骤 3: 复制到 GitHub Pages repo ===
log "步骤 3/4: 复制到 GitHub Pages repo..."
if [ ! -d "$GIT_REPO_DIR" ]; then
    err "找不到 $GIT_REPO_DIR"
    err "先克隆: git clone git@github.com:jayyangstore/jayyangstore.github.io.git ~/jayyangstore.github.io"
    exit 1
fi

if [ "$DRY_RUN" = true ]; then
    echo "  [DRY-RUN] 会复制: $JSON_SRC → $JSON_DST"
else
    cp "$JSON_SRC" "$JSON_DST"
    log "  已复制到 $JSON_DST ✅"
fi

# === 步骤 4: git commit + push ===
log "步骤 4/4: Git commit + push..."
cd "$GIT_REPO_DIR"

# 检查是否有变更
if git diff --quiet "$INDEX_FILE_NAME" 2>/dev/null && \
   git diff --cached --quiet "$INDEX_FILE_NAME" 2>/dev/null; then
    log "  没有变更，跳过 commit/push"
else
    if [ "$DRY_RUN" = true ]; then
        echo "  [DRY-RUN] 会: git add $INDEX_FILE_NAME"
        echo "  [DRY-RUN] 会: git commit -m 'chore: update desktop_index.json'"
        echo "  [DRY-RUN] 会: git push origin main"
    else
        git add "$INDEX_FILE_NAME"
        COMMIT_MSG="chore: update desktop_index.json - $(date +'%Y-%m-%d %H:%M')"
        git commit -m "$COMMIT_MSG"
        git push origin main
        log "  已 push ✅"
    fi
fi

echo ""
log "🎉 同步完成！"
echo "   GitHub Pages URL: $PAGES_URL"
echo "   AI Passport 端下次联网会拉到这份 JSON"
