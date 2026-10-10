// main/main.c —— 桌面索引 + Hermes 语音玩法入口
//
// 由 GitHub Actions build-firmware.yml 在编译前复制到 sdk-source/main/main.c
//
// v3 (2026-10-10): voice_link 集成 —— 按住 OK 说话问 Hermes
//   按键分工见 desktop_index_player.c handle_input() (语音逻辑在 dip 模块里)
//   voice_link 模块: ../02-sdk/voice_link.c (录音/HTTP/播放/配网)
#include "bsp_display.h"
#include "bsp_button.h"
#include "desktop_index_player.h"
#include "voice_link.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 后台拉取任务（C 不支持 lambda，独立函数）
static void fetch_task(void *arg) {
    if (dip_is_wifi_provisioned()) {
        ESP_LOGI(TAG, "WiFi provisioned, fetching...");
        dip_fetch_remote_async();
    } else {
        ESP_LOGW(TAG, "No WiFi config, cache only");
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Booting desktop_index_player v3 (voice)...");

    // NVS（WiFi 凭据 + 缓存 + 桥地址）
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // BSP + LVGL
    ESP_ERROR_CHECK(bsp_display_init());
    (void)bsp_lvgl_init();

    // 玩法初始化 (含语音 UI label 创建)
    ESP_ERROR_CHECK(dip_init());

    // 语音链路初始化 (加载桥地址, 未配置则提示进配网)
    esp_err_t vl_err = vl_init(NULL);
    if (vl_err != ESP_OK) {
        ESP_LOGW(TAG, "Voice not configured - double-click DOWN to provision");
    }

    // 后台拉取（如已配网）
    xTaskCreate(fetch_task, "dip_fetch_start", 4096, NULL, 5, NULL);

    // 主循环 (永不返回; 按键/语音轮询都在 dip_main_loop 里)
    dip_main_loop();
}
