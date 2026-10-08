# AI Passport 桌面索引 · 部署指南

老板照着这个走，30 分钟能跑通：扫描 → GitHub Pages → AI Passport 拉取 → 屏幕翻页。

## 📋 总览：完整工作流

```
┌──────────────────────────────────────────────────────────────┐
│ Mac 端（你）                                                  │
│                                                               │
│  1. 跑扫描 → desktop_index.json                              │
│  2. 跑 deploy → 推到 GitHub Pages                            │
│  3. 配 cron 每天自动跑                                       │
│                                                               │
└──────────────────────────────────────────────────────────────┘
                          ↓
              GitHub Pages (公开 URL)
   https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json
                          ↓
┌──────────────────────────────────────────────────────────────┐
│ AI Passport（设备）                                           │
│                                                               │
│  1. 首次开机 → 长按 OK 进配网 → 填 WiFi 密码                  │
│  2. 自动连 WiFi → 拉 GitHub JSON → 缓存 NVS                  │
│  3. 屏幕显示: "桌面 X天前更新 ▲25/138"                       │
│  4. ↑↓ 翻条 / OK 看详情 / 长按 OK 刷新                       │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

## 🚀 步骤 0：建 GitHub repo（一次性，5 分钟）

### 0.1 建 repo

1. 打开 https://github.com/new
2. 填写：
   - **Owner**: `JayYangstore`
   - **Repository name**: `desktop-ai-passport`
   - **Description**: `AI Passport 桌面索引的 GitHub Pages 后端`
   - **Public**（必须 public 才能用 Pages 公开访问）
   - ✅ Add a README file
3. 点 **Create repository**

### 0.2 启用 Pages

1. 进 repo → **Settings** → **Pages**
2. **Source**: Deploy from a branch
3. **Branch**: `main` / `(root)`
4. 点 **Save**
5. 等 1-2 分钟，看到 "Your site is live at..." 就行

### 0.3 首次推送 desktop_index.json

```bash
# 第一次推送（要 gh CLI 登录）
cd ~/Downloads/Hermes/ai-passport/tools
python3 desktop_index_scanner.py      # 生成 JSON
python3 deploy_to_github.py           # 推到 GitHub
```

完成后访问这个 URL 验证（应该返回 JSON）：
```
https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json
```

### 0.4 配 macOS 自动同步（推荐）

```bash
# 编辑 crontab
crontab -e

# 加一行（每天凌晨 3 点跑扫描 + 推送）
0 3 * * * cd ~/Downloads/Hermes/ai-passport/tools && python3 deploy_to_github.py >> sync.log 2>&1
```

## 🔨 步骤 1：构建 AI Passport 固件（云端，零本地配置）

### 1.1 推代码到 GitHub

老板，我们整个项目还没推到 GitHub。需要：

**A. 方案一：在 GitHub 上建 `desktop-ai-passport` repo，把整个 `~/Downloads/Hermes/ai-passport/` 推上去**

（老板你之前桌面索引也是 Mac 本地 + liveware 推，不在 GitHub。但 AI Passport 项目要从 GitHub 构建，所以必须建。）

```bash
cd ~/Downloads/Hermes/ai-passport
git init
git add .
git commit -m "feat: 桌面索引玩法 v2 - WiFi 拉取版"
git branch -M main
git remote add origin git@github.com:JayYangstore/desktop-ai-passport.git
git push -u origin main
```

⚠️ **等等**：刚才建的 repo 用来放 GitHub Pages 的 JSON（步骤 0）。**现在又要建一个 repo 放源码**？

**冲突解决**：把**两个用途合并到同一个 repo**：
- 同一个 repo: `JayYangstore/desktop-ai-passport`
- Pages URL 暴露 `desktop_index.json`
- 源码、`tools/`、`02-sdk/`、`workflows/` 都在里面
- 一次 push 同时触发: GitHub Pages 更新（手动）+ Firmware 构建（自动）

### 1.2 触发构建

push 完后：
1. 进 repo → **Actions** tab
2. 看到 "Build AI Passport Firmware" workflow 正在跑
3. 等 5-10 分钟（首次装 ESP-IDF 工具链较慢）
4. 构建完成后：
   - **Artifacts** → 下载 `ai-passport-desktop-index-firmware.zip`
   - 或 **Releases** → 下载 `merged-binary.bin`

## 🔥 步骤 2：烧录到设备（2 分钟）

### 2.1 下载 .bin

从 GitHub Actions / Releases 下载 `merged-binary.bin`

### 2.2 Web Flasher 烧录

1. 用 USB 数据线连 AI Passport 到电脑
2. 打开 https://ai-passport.folotoy.cn/tools/web-flasher/
3. Chrome / Edge 浏览器（**必须 Chromium 内核**，Safari 不支持 WebSerial）
4. 点 **连接设备** → 选择 `AI Passport` 串口
5. 选 **merged-binary.bin**
6. 点 **烧录**（会擦 Flash 全片，约 30 秒）
7. 烧完会自动重启设备

### 2.3 首次配网

设备重启后屏幕会显示 "无 WiFi 配置"：

1. **长按 OK 键 3 秒** → 设备进入 SoftAP 模式（创建热点）
2. 屏幕显示热点名（默认 `AI-Passport-XXXX`）
3. 手机/电脑连这个热点
4. 浏览器会自动弹出配网页（10.0.0.1）
5. 填 WiFi 名字 + 密码 → 点保存
6. 设备自动重启连 WiFi → 拉 JSON → 屏幕显示桌面索引！

## 🎮 步骤 3：日常使用

| 操作 | 按键 |
|---|---|
| 上一条 | ↑ |
| 下一条 | ↓ |
| 看详情 | OK |
| 返回列表 | OK（详情页）|
| 上一页（10条） | 长按 ↑ |
| 下一页（10条） | 长按 ↓ |
| **手动刷新** | 长按 OK |

## 🛠️ 故障排查

### 设备连不上 WiFi

- 检查 WiFi 是 **2.4GHz**（ESP32-C3 不支持 5GHz）
- 检查密码没特殊字符问题
- 配网页没弹出？手动访问 `10.0.0.1`

### 设备连上 WiFi 但拉不到 JSON

- 在 Mac 上访问 `https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json` 验证 URL 正确
- 检查 GitHub Pages 是否启用（Settings → Pages）
- 第一次 push 后 Pages 要等 1-2 分钟生效

### 屏幕乱码（方块）

- 当前 C 代码用的是 Montserrat 14 字体，**没有中文字形**
- 需要烧录时加中文字体（参考 `docs/development/engineering/lvgl-chinese-fonts.md`）
- **临时方案**：Mac 端扫描时把文件名转拼音（暂时不做，等老板确认是否需要）

### 屏幕只显示英文/数字

- 同上，字体不支持中文

## 📝 下一步优化（老板拍板）

- [ ] 加中文字体支持（让文件名能显示中文）
- [ ] 加首字母搜索（v2 增强）
- [ ] 加 BLE 推送模式（靠近 Mac 自动更新）
- [ ] 屏幕 90 秒无操作自动息屏（省电）

## 🔗 关键链接

- GitHub Pages 后端 URL: `https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json`
- Web Flasher: https://ai-passport.folotoy.cn/tools/web-flasher/
- 官方 SDK: https://github.com/FoloToy/ai-passport
- 官方 AGENTS.md: 必读，规定 C 代码不能复用官方 demo UI
