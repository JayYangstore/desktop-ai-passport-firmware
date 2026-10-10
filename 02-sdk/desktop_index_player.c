// desktop_index_player.c
// ======================
// AI Passport 桌面索引玩法实现
//
// 编译进固件前需要:
//   1. 把 desktop_index_scanner.py 生成的 desktop_index.json 嵌成 C 数组（v1 fallback）
//   2. 或者直接走 v2 WiFi 拉取（本文件就是 v2 实现）
//
// 编译:
//   cd sdk-source
//   idf.py set-target esp32c3
//   idf.py build
//   # 产物: build/merged-binary.bin (0x0 单文件，可 Web Flasher 直刷)
//
// 作者: Hermes Agent
// 日期: 2026-10-08

#include "desktop_index_player.h"
#include "voice_link.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "dip";

// === 状态机 ===
static dip_state_t    s_state = DIP_STATE_INIT;
static dip_index_t    s_index = {0};
static uint16_t       s_selected = 0;        // 当前选中条
static uint16_t       s_top = 0;             // 列表可视区顶部条
static bool           s_show_detail = false; // OK 是否按下显示详情

// LVGL 对象
static lv_obj_t *s_scr_list = NULL;
static lv_obj_t *s_scr_detail = NULL;
static lv_obj_t *s_list_rows[DIP_MAX_ENTRIES];   // 10 行 label
static lv_obj_t *s_status_label;                  // 顶栏 "X天前更新 ▲25/138"
static lv_obj_t *s_detail_label;                  // 详情页全路径
static lv_obj_t *s_hint_label;                    // 底部按键提示

// 配网相关
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// 按键事件队列
typedef struct {
    bsp_btn_t     btn;
    bsp_btn_ev_t  ev;
} dip_input_t;
static QueueHandle_t s_input_queue;

// 屏幕 240×320，每页 10 行（每行 25px + 4px 间距）
#define ROW_HEIGHT  26
#define VISIBLE_ROWS 10

// === 工具函数 ===

static void format_size(uint32_t bytes, char *out, size_t outlen) {
    if (bytes < 1024) snprintf(out, outlen, "%" PRIu32 "B", bytes);
    else if (bytes < 1024*1024) snprintf(out, outlen, "%" PRIu32 "KB", bytes/1024);
    else if (bytes < (uint32_t)1024*1024*1024) snprintf(out, outlen, "%" PRIu32 "MB", bytes/(1024*1024));
    else snprintf(out, outlen, "%" PRIu32 "GB", bytes/(1024*1024*1024));
}

static void format_relative_time(int64_t mtime, char *out, size_t outlen) {
    time_t now = time(NULL);
    int64_t delta = now - mtime;
    int days = delta / 86400;
    if (days == 0) snprintf(out, outlen, "今天");
    else if (days == 1) snprintf(out, outlen, "昨天");
    else if (days < 30) snprintf(out, outlen, "%d天前", days);
    else if (days < 365) snprintf(out, outlen, "%d月前", days/30);
    else snprintf(out, outlen, "%d年前", days/365);
}

// === NVS 缓存（断电不丢） ===

static esp_err_t load_cache_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(DIP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t required = sizeof(s_index);
    err = nvs_get_blob(h, "index", &s_index, &required);
    nvs_close(h);

    if (err == ESP_OK && s_index.total > 0) {
        ESP_LOGI(TAG, "Loaded %" PRIu32 " entries from NVS cache", (uint32_t)s_index.total);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t save_cache_to_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(DIP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, "index", &s_index, sizeof(s_index));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Saved %" PRIu32 " entries to NVS cache", (uint32_t)s_index.total);
    return err;
}

// === WiFi 事件处理 ===

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_state = DIP_STATE_FETCHING;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected, got IP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        // 标记失败，但保留缓存显示
        if (s_index.total == 0) s_state = DIP_STATE_WIFI_FAILED;
    }
}

static esp_err_t wifi_init_and_connect(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 从 NVS 读 SSID/密码
    nvs_handle_t h;
    char ssid[33] = {0};
    char pass[64] = {0};
    if (nvs_open(DIP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(ssid);
        nvs_get_str(h, DIP_NVS_KEY_SSID, ssid, &len);
        len = sizeof(pass);
        nvs_get_str(h, DIP_NVS_KEY_PASS, pass, &len);
        nvs_close(h);
    }

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi credentials in NVS, need provisioning");
        s_state = DIP_STATE_WIFI_FAILED;
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, 32);
    strncpy((char*)wifi_config.sta.password, pass, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = DIP_STATE_WIFI_CONNECTING;

    // 等 10 秒
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
    s_state = DIP_STATE_WIFI_FAILED;
    return ESP_ERR_TIMEOUT;
}

bool dip_is_wifi_provisioned(void) {
    nvs_handle_t h;
    if (nvs_open(DIP_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    bool ok = nvs_get_str(h, DIP_NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

// === HTTP 拉取 + JSON 解析 ===

static esp_err_t parse_index_json(const char *json_str, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    memset(&s_index, 0, sizeof(s_index));

    cJSON *ver = cJSON_GetObjectItem(root, "version");
    cJSON *gen_at = cJSON_GetObjectItem(root, "generated_at");
    cJSON *gen_h = cJSON_GetObjectItem(root, "generated_at_human");
    cJSON *host = cJSON_GetObjectItem(root, "host");
    cJSON *entries = cJSON_GetObjectItem(root, "entries");

    if (ver && cJSON_IsNumber(ver)) s_index.version = ver->valueint;
    if (gen_at && cJSON_IsNumber(gen_at)) s_index.generated_at = (int64_t)gen_at->valuedouble;
    if (gen_h && cJSON_IsString(gen_h)) strncpy(s_index.generated_at_human, gen_h->valuestring, sizeof(s_index.generated_at_human)-1);
    if (host && cJSON_IsString(host)) strncpy(s_index.host, host->valuestring, sizeof(s_index.host)-1);

    if (entries && cJSON_IsArray(entries)) {
        int count = cJSON_GetArraySize(entries);
        if (count > DIP_MAX_ENTRIES) count = DIP_MAX_ENTRIES;
        s_index.total = count;

        for (int i = 0; i < count; i++) {
            cJSON *e = cJSON_GetArrayItem(entries, i);
            if (!cJSON_IsObject(e)) continue;

            dip_entry_t *dst = &s_index.entries[i];

            cJSON *n = cJSON_GetObjectItem(e, "n");
            cJSON *p = cJSON_GetObjectItem(e, "p");
            cJSON *t = cJSON_GetObjectItem(e, "t");
            cJSON *s = cJSON_GetObjectItem(e, "s");
            cJSON *m = cJSON_GetObjectItem(e, "m");
            cJSON *icon = cJSON_GetObjectItem(e, "i");
            cJSON *cat = cJSON_GetObjectItem(e, "c");
            cJSON *src = cJSON_GetObjectItem(e, "src");

            if (n && cJSON_IsString(n)) strncpy(dst->name, n->valuestring, sizeof(dst->name)-1);
            if (p && cJSON_IsString(p)) strncpy(dst->path, p->valuestring, sizeof(dst->path)-1);
            if (t && cJSON_IsString(t)) dst->type = (t->valuestring[0] == 'd') ? DIP_TYPE_DIR : DIP_TYPE_FILE;
            if (s && cJSON_IsNumber(s)) dst->size = (uint32_t)s->valuedouble;
            if (m && cJSON_IsNumber(m)) dst->mtime = (int64_t)m->valuedouble;
            if (icon && cJSON_IsString(icon)) strncpy(dst->icon, icon->valuestring, sizeof(dst->icon)-1);
            if (cat && cJSON_IsString(cat)) {
                const char *cs = cat->valuestring;
                if (!strcmp(cs, "doc")) dst->category = DIP_CAT_DOC;
                else if (!strcmp(cs, "data")) dst->category = DIP_CAT_DATA;
                else if (!strcmp(cs, "media")) dst->category = DIP_CAT_MEDIA;
                else if (!strcmp(cs, "image")) dst->category = DIP_CAT_IMAGE;
                else if (!strcmp(cs, "video")) dst->category = DIP_CAT_VIDEO;
                else if (!strcmp(cs, "audio")) dst->category = DIP_CAT_AUDIO;
                else if (!strcmp(cs, "archive")) dst->category = DIP_CAT_ARCHIVE;
            }
            if (src && cJSON_IsString(src)) strncpy(dst->source, src->valuestring, sizeof(dst->source)-1);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %" PRIu32 " entries from JSON", (uint32_t)s_index.total);
    return ESP_OK;
}

static esp_err_t http_fetch_index(void) {
    esp_http_client_config_t cfg = {
        .url = DIP_INDEX_URL,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,  // GitHub Pages 证书 OK，但 esp_http_client 默认严格
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // 用动态 buffer 接 body
    size_t total = 0;
    char buf[4096];
    char *body = NULL;
    size_t cap = 0;

    while (1) {
        int read = esp_http_client_read(client, buf, sizeof(buf));
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) break;  // EOF

        if (total + read + 1 > cap) {
            cap = (total + read) * 2 + 1024;
            body = realloc(body, cap);
            if (!body) { err = ESP_ERR_NO_MEM; break; }
        }
        memcpy(body + total, buf, read);
        total += read;
    }
    body[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP fetch failed: status=%d err=%s", status, esp_err_to_name(err));
        free(body);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetched %" PRIu32 " bytes", (uint32_t)total);
    err = parse_index_json(body, total);
    free(body);

    if (err == ESP_OK) {
        save_cache_to_nvs();
    }
    return err;
}

// 后台 fetch 任务（C 不支持 lambda，独立函数）
static void fetch_task(void *arg) {
    esp_err_t err = wifi_init_and_connect();
    if (err == ESP_OK) {
        err = http_fetch_index();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Fetch failed, will use cache");
            s_state = DIP_STATE_FETCH_FAILED;
        } else {
            s_state = DIP_STATE_READY;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t dip_fetch_remote_async(void) {
    // 在新 task 里跑（不阻塞按键）
    TaskHandle_t task;
    xTaskCreate(fetch_task, "dip_fetch", 8192, NULL, 5, &task);
    return ESP_OK;
}

// === LVGL UI 渲染 ===

static void list_update_visible(void) {
    if (!s_scr_list) return;

    char buf[128];

    // 状态栏
    if (s_index.total > 0) {
        char rel_time[16];
        format_relative_time(s_index.generated_at, rel_time, sizeof(rel_time));
        snprintf(buf, sizeof(buf), "桌面 %s  ▲%" PRIu32 "/%" PRIu32,
                 rel_time, (uint32_t)(s_selected + 1), (uint32_t)s_index.total);
    } else {
        snprintf(buf, sizeof(buf), "桌面 无数据");
    }
    lv_label_set_text(s_status_label, buf);

    // 行
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        uint16_t idx = s_top + i;
        if (idx >= s_index.total) {
            lv_obj_add_flag(s_list_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_list_rows[i], LV_OBJ_FLAG_HIDDEN);

        dip_entry_t *e = &s_index.entries[idx];
        char size_str[16] = "";
        if (e->type == DIP_TYPE_FILE && e->size > 0) {
            format_size(e->size, size_str, sizeof(size_str));
        }

        // 截断名字（去掉 .扩展名节省空间? 不去，老板要看得懂）
        char name_trunc[DIP_NAME_MAX_LEN + 1];
        strncpy(name_trunc, e->name, sizeof(name_trunc)-1);
        name_trunc[sizeof(name_trunc)-1] = '\0';

        // 中文截断到 ~16 字符（按屏幕宽度估算）
        // 简单做法：UTF-8 字节截断到 32
        if (strlen(name_trunc) > 28) {
            // 粗暴截到 28 字节 + "…"
            // ⚠️ 注意：可能在 UTF-8 多字节字符中间截断导致乱码
            // 简化方案：先按字节截，再找上一个完整 UTF-8 边界
            name_trunc[27] = '\0';
            strcat(name_trunc, "…");
        }

        if (e->icon[0]) {
            snprintf(buf, sizeof(buf), "%s %s", e->icon, name_trunc);
        } else {
            snprintf(buf, sizeof(buf), "📁 %s", name_trunc);  // 目录默认图标
        }

        // 选中态：底色高亮
        if (idx == s_selected) {
            lv_obj_set_style_bg_color(s_list_rows[i], lv_color_hex(0x3050FF), 0);
            lv_obj_set_style_text_color(s_list_rows[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(s_list_rows[i], lv_color_hex(0x1A1A2E), 0);
            lv_obj_set_style_text_color(s_list_rows[i], lv_color_hex(0xDDDDDD), 0);
        }

        lv_label_set_text(s_list_rows[i], buf);
    }
}

static void detail_show(void) {
    if (!s_scr_detail) return;
    if (s_index.total == 0) return;

    dip_entry_t *e = &s_index.entries[s_selected];

    char size_str[16] = "";
    if (e->type == DIP_TYPE_FILE && e->size > 0) {
        format_size(e->size, size_str, sizeof(size_str));
    }

    char time_str[32];
    {
        time_t t = e->mtime;
        struct tm *tm = localtime(&t);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm);
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "%s %s\n\n"
             "路径: ~/%s\n"
             "类型: %s\n"
             "大小: %s\n"
             "修改: %s\n"
             "来源: %s\n\n"
             "(按 OK 返回)",
             e->icon, e->name,
             e->path,
             e->type == DIP_TYPE_DIR ? "文件夹" : "文件",
             e->size > 0 ? size_str : "-",
             time_str,
             e->source[0] ? e->source : "-");
    lv_label_set_text(s_detail_label, buf);

    lv_screen_load(s_scr_detail);
    s_show_detail = true;
}

// === 按键回调 ===
// ⚠️ BSP 官方说：按键回调里不能阻塞、不能访问 LVGL
// 所以：只塞队列，让 main loop 处理
static void button_callback(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    dip_input_t in = { .btn = btn, .ev = ev };
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_input_queue, &in, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

static void handle_input(const dip_input_t *in) {
    // === 语音态优先 (录音/思考/播放中, 语音键自己处理, 其他键忽略) ===
    vl_state_t vs = vl_get_state();
    if (vs != VL_IDLE) {
        if (in->btn == BSP_BTN_OK) {
            if (in->ev == BSP_BTN_CLICK && vs == VL_RECORDING) vl_rec_stop(true);
            else if (in->ev == BSP_BTN_LONG && vs == VL_RECORDING) vl_rec_cancel();
            // THINKING/PLAYING 时按键忽略 (等状态结束, 播放完自动回 IDLE)
        }
        return;  // 录音中不响应列表操作
    }

    if (s_show_detail) {
        // 详情页：任意键返回列表
        if (in->ev == BSP_BTN_CLICK) {
            lv_screen_load(s_scr_list);
            s_show_detail = false;
        }
        return;
    }

    if (s_index.total == 0) {
        // 没数据：长按 OK 触发刷新
        if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
            dip_trigger_refresh();
        }
        // 双击 DOWN = Hermes 桥配网
        if (in->btn == BSP_BTN_DOWN && in->ev == BSP_BTN_DOUBLE) {
            vl_start_provisioning();
        }
        return;
    }

    switch (in->btn) {
    case BSP_BTN_UP:
        if (in->ev == BSP_BTN_CLICK) {
            if (s_selected > 0) s_selected--;
            if (s_selected < s_top) s_top = s_selected;
        } else if (in->ev == BSP_BTN_LONG) {
            // 长按：上一页（10条）
            if (s_selected >= VISIBLE_ROWS) s_selected -= VISIBLE_ROWS;
            else s_selected = 0;
            s_top = (s_selected / VISIBLE_ROWS) * VISIBLE_ROWS;
        }
        list_update_visible();
        break;

    case BSP_BTN_DOWN:
        if (in->ev == BSP_BTN_CLICK) {
            if (s_selected + 1 < s_index.total) s_selected++;
            if (s_selected >= s_top + VISIBLE_ROWS) s_top = s_selected - VISIBLE_ROWS + 1;
        } else if (in->ev == BSP_BTN_LONG) {
            if (s_selected + VISIBLE_ROWS < s_index.total) s_selected += VISIBLE_ROWS;
            else if (s_index.total > 0) s_selected = s_index.total - 1;
            s_top = (s_selected / VISIBLE_ROWS) * VISIBLE_ROWS;
        } else if (in->ev == BSP_BTN_DOUBLE) {
            // 双击 DOWN = Hermes 桥配网 (热点 HermesPass → 192.168.4.1)
            vl_start_provisioning();
        }
        list_update_visible();
        break;

    case BSP_BTN_OK:
        if (in->ev == BSP_BTN_PRESS) {
            // 按住说话: 开始录音
            if (vl_rec_begin() == ESP_OK) {
                ESP_LOGI(TAG, "Voice: recording start");
            }
        } else if (in->ev == BSP_BTN_CLICK) {
            detail_show();
        } else if (in->ev == BSP_BTN_LONG) {
            // 长按 OK 5 秒触发刷新（简单做法：直接 trigger）
            ESP_LOGI(TAG, "Manual refresh triggered");
            dip_trigger_refresh();
        }
        break;

    default:
        break;
    }
}

// === 初始化 ===

static void build_list_screen(void) {
    s_scr_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_list, lv_color_hex(0x0E0E1A), 0);

    // 状态栏
    s_status_label = lv_label_create(s_scr_list);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x88FF88), 0);
    lv_obj_set_pos(s_status_label, 6, 4);
    lv_obj_set_width(s_status_label, 228);
    lv_label_set_text(s_status_label, "桌面 加载中...");

    // 10 行
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        s_list_rows[i] = lv_label_create(s_scr_list);
        lv_obj_set_size(s_list_rows[i], 228, ROW_HEIGHT - 2);
        lv_obj_set_pos(s_list_rows[i], 6, 28 + i * ROW_HEIGHT);
        lv_obj_set_style_text_font(s_list_rows[i], &lv_font_montserrat_14, 0);  // TODO: 改中文字体
        lv_obj_set_style_bg_color(s_list_rows[i], lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_bg_opa(s_list_rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(s_list_rows[i], 6, 0);
        lv_obj_set_style_pad_top(s_list_rows[i], 4, 0);
        lv_label_set_long_mode(s_list_rows[i], LV_LABEL_LONG_CLIP);
    }

    // 底栏提示
    s_hint_label = lv_label_create(s_scr_list);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x666688), 0);
    lv_obj_set_pos(s_hint_label, 6, 296);
    lv_label_set_text(s_hint_label, "OK按住说话 短按详情 双击DOWN配网");

    // 语音状态提示 (顶部状态栏下方, 默认隐藏)
    s_voice_label = lv_label_create(s_scr_list);
    lv_obj_set_style_text_color(s_voice_label, lv_color_hex(0xFFD166), 0);
    lv_obj_set_pos(s_voice_label, 6, 22);
    lv_obj_add_flag(s_voice_label, LV_OBJ_FLAG_HIDDEN);
}

static void build_detail_screen(void) {
    s_scr_detail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_detail, lv_color_hex(0x0E0E1A), 0);

    s_detail_label = lv_label_create(s_scr_detail);
    lv_obj_set_pos(s_detail_label, 6, 6);
    lv_obj_set_width(s_detail_label, 228);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0xEEEEEE), 0);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);
}

esp_err_t dip_init(void) {
    ESP_LOGI(TAG, "Initializing desktop_index_player...");

    // NVS 初始化
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK || err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // OK
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 按键队列
    s_input_queue = xQueueCreate(8, sizeof(dip_input_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;

    // 注册按键
    ESP_ERROR_CHECK(bsp_button_init(button_callback, NULL));

    // 加载 NVS 缓存
    if (load_cache_from_nvs() == ESP_OK) {
        s_state = DIP_STATE_READY;
        ESP_LOGI(TAG, "Have %" PRIu32 " cached entries", (uint32_t)s_index.total);
    }

    // 建 UI（必须在 LVGL 任务里，先简化：直接调）
    // ⚠️ 严格做法：bsp_lvgl_lock + 调 lv_*
    if (bsp_lvgl_lock(1000)) {
        build_list_screen();
        build_detail_screen();
        lv_screen_load(s_scr_list);
        list_update_visible();
        bsp_lvgl_unlock();
    }

    return ESP_OK;
}

esp_err_t dip_trigger_refresh(void) {
    return dip_fetch_remote_async();
}

dip_state_t dip_get_state(void) {
    return s_state;
}

// === 语音 UI (v3) ===
// (voice_ui_update 定义在文件尾部, dip_main_loop 通过此前置声明调用)
static void voice_ui_update(void);

static lv_obj_t *s_voice_label = NULL;

void dip_main_loop(void) {
    // 在 LVGL 任务里跑（按键事件队列消费）
    dip_input_t in;
    while (1) {
        // 语音录音流式读取 (RECORDING 态时每次拉一块 PCM)
        vl_rec_poll();

        if (xQueueReceive(s_input_queue, &in, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (bsp_lvgl_lock(100)) {
                handle_input(&in);
                voice_ui_update();
                bsp_lvgl_unlock();
            }
        } else {
            // 空转也要刷语音状态 (录音中/播放完)
            if (bsp_lvgl_lock(100)) {
                voice_ui_update();
                bsp_lvgl_unlock();
            }
        }
    }
}

// === 语音 UI (v3) ===

static void voice_ui_update(void) {
    vl_state_t st = vl_get_state();
    if (!s_voice_label) return;
    const char *txt = NULL;
    switch (st) {
    case VL_RECORDING: txt = LV_SYMBOL_AUDIO " REC 松开发送"; break;
    case VL_THINKING:  txt = LV_SYMBOL_REFRESH " Hermes..."; break;
    case VL_PLAYING:   txt = LV_SYMBOL_PLAY " 播放中"; break;
    default: break;
    }
    if (txt) {
        lv_label_set_text(s_voice_label, txt);
        lv_obj_clear_flag(s_voice_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_voice_label, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *dip_get_list_screen(void) { return s_scr_list; }
