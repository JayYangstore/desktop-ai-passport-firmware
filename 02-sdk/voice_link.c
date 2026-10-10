// voice_link.c — 工牌语音链路 v1 (P2)
// =====================================
// 按住 OK 说话 -> 流式录 PCM -> POST /v2/ask_say -> 裸 PCM 回放
//
// 交互 (受 BSP 按键事件限制, 无 RELEASE 事件):
//   OK PRESS  = 开始录音
//   OK CLICK  = 结束并发送 (松开时 BSP 会上报 CLICK)
//   OK LONG   = 取消本次录音
//   DOWN DOUBLE = 开 Hermes 配网热点 (voice 配置, P3)
//
// 硬件约束:
//   - ESP32-C3 无 PSRAM: 录音缓冲 64KB(2s) + 回复缓冲 512KB 用堆, 不一次拿大块
//   - 回复 PCM 16k/16bit/mono, 512KB ≈ 16.4s 语音上限
//   - HTTP 超时 120s (Hermes 思考 + TTS + 转码需要时间)
//
// 协议 (与 02-hermes-bridge /v2/ask_say 对齐):
//   请求: body = 裸 PCM s16le ?sr=16000
//   响应: body = 裸 PCM audio/L16 16k mono; X-Question/X-Reply = URL编码文本

#include "voice_link.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dhcp.h"

#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

static const char *TAG = "voice";

// === 配置 ===
#define VL_NVS_NAMESPACE   "voice"
#define VL_NVS_KEY_BRIDGE  "bridge"    // 如 http://192.168.20.224:8888
#define VL_SR              16000
#define VL_REC_CHUNK       2048        // 每次读的 PCM 字节 (≈64ms)
#define VL_REC_MAX         (16000 * 2 * 10)  // 最长 10s = 320KB
#define VL_PLAY_MAX        (512 * 1024)      // 回复上限 ≈16s 语音
#define VL_HTTP_TIMEOUT_MS 120000

// === 状态 ===
// vl_state_t 定义见 voice_link.h
static volatile vl_state_t s_state = VL_IDLE;
static char s_bridge_url[96] = {0};   // 完整 URL: <bridge>/v2/ask_say

// 录音缓冲 (启动时 malloc)
static uint8_t *s_rec_buf = NULL;
static volatile size_t s_rec_len = 0;

// 回复缓冲
static uint8_t *s_play_buf = NULL;
static volatile size_t s_play_len = 0;

// 取消标记
static volatile bool s_cancel = false;

// 状态回调 (给 UI 层显示)
static vl_state_cb_t s_ui_cb = NULL;

// SoftAP 配网
static EventGroupHandle_t s_prov_group;
#define PROV_DONE_BIT BIT0

static void ask_task(void *arg);  // 前置声明 (vl_rec_stop 里创建)

vl_state_t vl_get_state(void) { return s_state; }

void vl_set_state_cb(vl_state_cb_t cb) { s_ui_cb = cb; }

static void notify_ui(vl_state_t st) {
    s_state = st;
    if (s_ui_cb) s_ui_cb(st);
}

// === NVS 配置 ===

bool vl_is_configured(void) {
    nvs_handle_t h;
    if (nvs_open(VL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char url[96] = {0};
    size_t len = sizeof(url);
    bool ok = nvs_get_str(h, VL_NVS_KEY_BRIDGE, url, &len) == ESP_OK && url[0] != '\0';
    nvs_close(h);
    return ok;
}

esp_err_t vl_load_config(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(VL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_bridge_url);
    err = nvs_get_str(h, VL_NVS_KEY_BRIDGE, s_bridge_url, &len);
    nvs_close(h);
    if (err != ESP_OK || s_bridge_url[0] == '\0') {
        ESP_LOGW(TAG, "No bridge URL configured");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Bridge: %s", s_bridge_url);
    return ESP_OK;
}

esp_err_t vl_save_bridge_url(const char *url) {
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(VL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, VL_NVS_KEY_BRIDGE, url);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strncpy(s_bridge_url, url, sizeof(s_bridge_url) - 1);
        ESP_LOGI(TAG, "Bridge URL saved: %s", url);
    }
    return err;
}

const char *vl_get_bridge_url(void) { return s_bridge_url; }

// === SoftAP 配网 (P3) ===

static void softap_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client connected to provisioning AP");
    }
}

// 极简配置页 (响应小, 不嵌大 HTML)
static const char PROV_PAGE[] =
    "<html><head><meta charset=utf-8><meta name=viewport "
    "content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;padding:20px;max-width:420px;margin:auto}"
    "input,button{width:100%;padding:12px;margin:6px 0;font-size:16px;"
    "box-sizing:border-box}button{background:#1f6feb;color:#fff;border:0;"
    "border-radius:8px}</style></head>"
    "<body><h2>Hermes 工牌配网</h2>"
    "<p>填 Mac 上桥的地址 (含端口):</p>"
    "<form action=/save method=GET>"
    "<input name=bridge placeholder='http://192.168.1.100:8888' required>"
    "<button>保存并连接</button></form></body></html>";

// 极简 HTTP server (blocking, 只为配网, 用完即关)
#include "lwip/sockets.h"

static void prov_server_task(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 2) < 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Provisioning server on 192.168.4.1");

    while (!(xEventGroupGetBits(s_prov_group) & PROV_DONE_BIT)) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int c = accept(srv, (struct sockaddr *)&cli, &cl);
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        char req[1024] = {0};
        int n = recv(c, req, sizeof(req) - 1, 0);
        if (n <= 0) { close(c); continue; }

        // 解析 GET /save?bridge=xxx
        char url[160] = {0};
        char *p = strstr(req, "GET /save?bridge=");
        if (p) {
            p += 17;
            char *end = strchr(p, ' ');
            if (end && (size_t)(end - p) < sizeof(url)) {
                size_t l = end - p;
                memcpy(url, p, l);
                url[l] = '\0';

                // URL decode (极简: %XX)
                char dec[96] = {0};
                size_t di = 0;
                for (size_t i = 0; url[i] && di < sizeof(dec) - 1; i++) {
                    if (url[i] == '%' && url[i+1] && url[i+2]) {
                        char hex[3] = {url[i+1], url[i+2], 0};
                        dec[di++] = (char)strtol(hex, NULL, 16);
                        i += 2;
                    } else if (url[i] == '+') {
                        dec[di++] = ' ';
                    } else {
                        dec[di++] = url[i];
                    }
                }

                if (vl_save_bridge_url(dec) == ESP_OK) {
                    send(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
                            "已保存! 设备将重启并连接桥...",
                         70, 0);
                    close(c);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    // 先停配网热点, 让 STA 模式接管; 直接重启最干净
                    esp_restart();
                    break;
                }
            }
        }

        // 默认: 配置页
        char hdr[128];
        int hl = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n",
                          (int)strlen(PROV_PAGE));
        send(c, hdr, hl, 0);
        send(c, PROV_PAGE, strlen(PROV_PAGE), 0);
        close(c);
    }
    close(srv);
    vTaskDelete(NULL);
}

esp_err_t vl_start_provisioning(void) {
    if (!s_prov_group) s_prov_group = xEventGroupCreate();

    wifi_config_t ap_cfg = {0};
    strcpy((char *)ap_cfg.ap.ssid, "HermesPass");
    strcpy((char *)ap_cfg.ap.password, "12345678");
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_cfg.ap.max_connection = 2;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP 'HermesPass' up, pass 12345678, config at http://192.168.4.1");

    if (xTaskCreate(prov_server_task, "vl_prov", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// === 录音 (流式到缓冲) ===

esp_err_t vl_rec_begin(void) {
    if (s_state != VL_IDLE) return ESP_ERR_INVALID_STATE;
    return rec_start();
}

static esp_err_t rec_start(void) {
    if (!s_rec_buf) {
        s_rec_buf = malloc(VL_REC_MAX);
        if (!s_rec_buf) {
            ESP_LOGE(TAG, "rec buf alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    s_rec_len = 0;
    s_cancel = false;

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) return err;
    err = bsp_audio_set_format(VL_SR, 16, 1);   // 16k/16bit/mono
    if (err != ESP_OK) return err;

    notify_ui(VL_RECORDING);
    ESP_LOGI(TAG, "Recording...");
    return ESP_OK;
}

// 一次录音 chunk (在主循环里轮询调用)
void vl_rec_poll(void) {
    if (s_state != VL_RECORDING || s_cancel) return;
    if (s_rec_len + VL_REC_CHUNK > VL_REC_MAX) return;  // 上限自动停

    if (bsp_audio_read(s_rec_buf + s_rec_len, VL_REC_CHUNK) == ESP_OK) {
        s_rec_len += VL_REC_CHUNK;
    }
}

void vl_rec_stop(bool send) {
    if (s_state != VL_RECORDING) return;
    if (!send || s_cancel || s_rec_len < VL_SR) {   // <0.5s 视为误触
        ESP_LOGW(TAG, "Recording cancelled/short (%" PRIu32 " bytes)", (uint32_t)s_rec_len);
        s_rec_len = 0;
        notify_ui(VL_IDLE);
        return;
    }
    ESP_LOGI(TAG, "Recorded %" PRIu32 " bytes (%" PRIu32 " ms)",
             (uint32_t)s_rec_len, (uint32_t)(s_rec_len * 1000 / VL_SR / 2));
    // 发送在独立任务里跑, 不阻塞主循环
    if (xTaskCreate(ask_task, "vl_ask", 8192, NULL, 5, NULL) != pdPASS) {
        s_rec_len = 0;
        notify_ui(VL_IDLE);
    }
}

void vl_rec_cancel(void) {
    s_cancel = true;
}

// === 发送 + 回放 ===

// HTTP 事件: 收 body
static esp_err_t http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_play_buf && evt->data_len) {
        if (s_play_len + evt->data_len <= VL_PLAY_MAX) {
            memcpy(s_play_buf + s_play_len, evt->data, evt->data_len);
            s_play_len += evt->data_len;
        }
    }
    return ESP_OK;
}

static void ask_task(void *arg) {
    (void)arg;
    notify_ui(VL_THINKING);

    if (!s_play_buf) {
        s_play_buf = malloc(VL_PLAY_MAX);
        if (!s_play_buf) {
            ESP_LOGE(TAG, "play buf alloc failed");
            goto done;
        }
    }
    s_play_len = 0;

    char url[160];
    int nurl = snprintf(url, sizeof(url), "%s/v2/ask_say?sr=%d", s_bridge_url, (int)VL_SR);
    if (nurl < 0 || nurl >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "bridge URL too long");
        goto done;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VL_HTTP_TIMEOUT_MS,
        .event_handler = http_event,
        .buffer_size = 8192,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto done;

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    // chunked 发送 (数据已经在 s_rec_buf 里, 一次性 open+write 即可)
    esp_err_t err = esp_http_client_open(client, (int)s_rec_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto done;
    }
    esp_http_client_write(client, (const char *)s_rec_buf, (int)s_rec_len);
    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    // 读响应 (事件回调里已存入 s_play_buf, 这里 drain)
    char tmp[2048];
    while (esp_http_client_read(client, tmp, sizeof(tmp)) > 0) {}

    esp_http_client_cleanup(client);

    if (status != 200 || s_play_len == 0) {
        ESP_LOGE(TAG, "ask failed: status=%d len=%" PRIu32, status, (uint32_t)s_play_len);
        goto done;
    }

    ESP_LOGI(TAG, "Reply PCM: %" PRIu32 " bytes", (uint32_t)s_play_len);

    // === 播放 ===
    notify_ui(VL_PLAYING);
    // 复用录音 buffer 前的安全: 播放用 s_play_buf
    err = bsp_audio_set_format(VL_SR, 16, 1);
    if (err == ESP_OK) {
        size_t off = 0;
        while (off < s_play_len) {
            size_t n = (s_play_len - off > VL_REC_CHUNK) ? VL_REC_CHUNK
                                                          : (s_play_len - off);
            if (bsp_audio_write(s_play_buf + off, n) != ESP_OK) break;
            off += n;
        }
    }
    ESP_LOGI(TAG, "Playback done");

done:
    s_rec_len = 0;
    notify_ui(VL_IDLE);
    vTaskDelete(NULL);
}

// === 初始化 ===

esp_err_t vl_init(vl_state_cb_t cb) {
    s_ui_cb = cb;
    return vl_load_config();
}

esp_err_t vl_deinit(void) {
    free(s_rec_buf); s_rec_buf = NULL;
    free(s_play_buf); s_play_buf = NULL;
    return ESP_OK;
}