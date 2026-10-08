# AI Passport · 桌面索引口袋版

> 把 Mac 桌面/下载的文件清单随身带走，按键就能翻

## 🎯 项目目标

让老板出门在外掏出 AI Passport 就能查"那个 PDF/Excel/PPT 上次放在哪了"，
不用掏出手机开 Find My / 不用回忆 Mac 路径。

## 🏗️ 架构图

```
┌─────────────────────────────────────────────────────────────────┐
│ Mac 端（每天凌晨自动跑）                                          │
│                                                                  │
│  desktop_index_scanner.py                                        │
│       ↓                                                          │
│  扫描 ~/Desktop + ~/Downloads + ~/Documents 顶层                  │
│       ↓                                                          │
│  生成 desktop_index.json (~20-50KB)                              │
│       ↓                                                          │
│  github_pages_sync.sh → git commit + push                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ GitHub Pages（公共 CDN）                                          │
│                                                                  │
│  https://jayyangstore.github.io/desktop-ai-passport/index.json   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ AI Passport 端                                                  │
│                                                                  │
│  v1（手动重刷）: 编译期把 JSON 打进固件 → Web Flasher 烧          │
│  v2（联网拉取）: 开机连 WiFi → 拉 JSON → 缓存 NVS → 屏幕显示    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 📂 目录结构

```
~/Downloads/Hermes/ai-passport/
├── 00-readme/           ← 本目录：项目说明、决策记录
├── 01-hardware/         ← 硬件规格、引脚、烧录日志
├── 02-sdk/              ← 玩法 C 源码（待开发）
├── 03-plays/            ← 编译产物：固件 .bin（待产出）
├── 04-flash-logs/       ← 烧录记录、版本号
├── 05-hermes-adapter/   ← 预留：未来接 Hermes 时用
├── 99-archive/          ← 历史备份
├── sdk-source/          ← 官方 SDK 克隆（folotoy/ai-passport）
└── tools/               ← Mac 端脚本 + 推送脚本
    ├── desktop_index_scanner.py
    ├── github_pages_sync.sh
    └── lib/
```

## 🛣️ 路线图

| 阶段 | 状态 | 描述 |
|---|---|---|
| v0 | ✅ | 建目录 + 摸清 SDK + 抓资料 |
| v1 | 🔨 | Mac 扫描脚本 + GitHub Pages 推送脚本 |
| v2 | ⏳ | AI Passport 玩法 C 代码（编译期嵌入 JSON） |
| v3 | ⏳ | 编译 + 烧录到设备验证 |
| v4 | ⏳ | v2 加 WiFi 自动拉取 JSON |
| v5 | ⏳ | BLE 靠近自动推送（可选） |

## 🎛️ AI Passport 按键设计（v1）

| 按键 | 短按 | 长按 |
|---|---|---|
| 上 | 上一条 | 上一页（10条） |
| 下 | 下一条 | 下一页（10条） |
| OK | 显示详情（全路径 + 修改时间 + 大小） | — |

屏幕布局（240×320）：
```
┌────────────────────────────────────┐
│ 桌面索引       3天前更新  ▲25/138  │ ← 状态栏
├────────────────────────────────────┤
│ 📊 9月销售汇总表.xlsx              │ ← 当前选中（高亮）
│ 📁 奥迪项目                       │
│ 📄 立项BM单-2026年9月.pdf         │
│ 📊 店端人效对比.xlsx              │
│ 📄 通勤日报.docx                  │
│ 🖼 板栗运动会.jpg                 │
│ 📊 10月计划.xlsx                  │
├────────────────────────────────────┤
│ ↑↓ 翻   OK 详情   长按↑↓ 翻页     │ ← 操作提示
└────────────────────────────────────┘
```

## 📋 关键决策

- **不做搜索**（老板 2026-10-08 拍板）→ 先纯翻页
- **GitHub Pages 做后端**（老板 2026-10-08 拍板）→ 公共 CDN + Mac 端 git push
- **离线可用**（v1 必）/ **WiFi 拉取**（v2 增）
- **ESP-IDF 5.5.3 + LVGL**（官方约束）
- **C 代码不进 main.c**（官方禁止复用 demo UI），新建 `main/desktop_index_player.c`

## 🔗 资料索引

- 官方教程: https://ai-passport.folotoy.cn/guides/create-a-play-with-agent/
- 官方 SDK: https://github.com/FoloToy/ai-passport
- AGENTS.md: 官方给 AI Agent 的工作说明（必读）
- 硬件规格: ESP32-C3 + 240×320 ST7789 + 8MB Flash + 无 PSRAM
