// desktop_index_player.h
// ======================
// AI Passport 桌面索引玩法（v2 · WiFi 拉取版）
//
// 职责:
//   - WiFi 连接管理（首次开机配网、之后自动重连）
//   - HTTP GET 拉取 GitHub Pages 上的 desktop_index.json
//   - JSON 解析 → 内存索引
//   - LVGL 渲染：列表翻页 + 详情页
//   - 三键交互：↑↓ 翻条/翻页，OK 看详情，长按 ↑↓ 翻页（10条/页）
//
// 关键决策（2026-10-08 老板拍板）:
//   - 跳过 v1 手动重刷，直接做 v2 WiFi 拉取
//   - 后端: GitHub Pages (https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json)
//   - 暂不做搜索，纯翻页
//   - LVGL UI（不用裸画点）
//   - 数据缓存到 NVS，断网用上次缓存

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// === 限制 ===
#define DIP_MAX_ENTRIES        200   // 最大条目数（与 Mac 端一致）
#define DIP_NAME_MAX_LEN       64    // 文件名最大长度（截断显示）
#define DIP_PATH_MAX_LEN       128   // 完整路径最大长度

// === 配置 ===
// GitHub Pages URL（与 tools/deploy_to_github.py 一致）
#define DIP_INDEX_URL          "https://jayyangstore.github.io/desktop-ai-passport/desktop_index.json"

// 配网信息存 NVS（开机自动连，失败走 SoftAP 配网）
#define DIP_NVS_NAMESPACE      "dip"
#define DIP_NVS_KEY_SSID       "ssid"
#define DIP_NVS_KEY_PASS       "pass"

// === 数据结构 ===
typedef enum {
    DIP_TYPE_DIR = 0,
    DIP_TYPE_FILE = 1,
} dip_type_t;

typedef enum {
    DIP_CAT_OTHER = 0,
    DIP_CAT_DOC,
    DIP_CAT_DATA,
    DIP_CAT_MEDIA,
    DIP_CAT_IMAGE,
    DIP_CAT_VIDEO,
    DIP_CAT_AUDIO,
    DIP_CAT_ARCHIVE,
} dip_category_t;

typedef struct {
    char        name[DIP_NAME_MAX_LEN + 1];   // 文件名（短）
    char        path[DIP_PATH_MAX_LEN + 1];   // 相对 ~/ 路径（详情页显示）
    dip_type_t  type;                          // DIR / FILE
    uint32_t    size;                          // 字节（目录为 0）
    int64_t     mtime;                         // 修改时间（unix 秒）
    char        icon[8];                       // emoji（UTF-8，截断到 4 字节）
    dip_category_t category;
    char        source[16];                    // "Desktop" / "Downloads" / "Documents"
} dip_entry_t;

typedef struct {
    int32_t       version;
    int64_t       generated_at;
    char          generated_at_human[32];
    char          host[32];
    uint16_t      total;
    dip_entry_t   entries[DIP_MAX_ENTRIES];
} dip_index_t;

// === 公开 API ===

// 初始化（注册按键回调、创建 LVGL 屏幕、加载 NVS 缓存）
esp_err_t dip_init(void);

// 主循环（在 LVGL 任务里跑）
void dip_main_loop(void);

// 触发一次手动刷新（按 OK 长按 5 秒触发）
esp_err_t dip_trigger_refresh(void);

// 异步拉取 GitHub Pages（在 worker task 里跑）
esp_err_t dip_fetch_remote_async(void);

// 查询当前状态
typedef enum {
    DIP_STATE_INIT = 0,
    DIP_STATE_WIFI_CONNECTING,
    DIP_STATE_WIFI_FAILED,
    DIP_STATE_FETCHING,
    DIP_STATE_FETCH_FAILED,
    DIP_STATE_READY,           // 有数据可用（缓存或新拉的）
} dip_state_t;

dip_state_t dip_get_state(void);

// 查询 WiFi 是否已配网（用于决定是否进 SoftAP 配网页）
bool dip_is_wifi_provisioned(void);

#ifdef __cplusplus
}
#endif
