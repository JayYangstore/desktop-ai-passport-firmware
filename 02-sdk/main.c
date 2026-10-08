// main/main.c —— 桌面索引玩法入口（替换官方 demo）
//
// 这个文件由 GitHub Actions 的 build-firmware.yml 在编译前复制到
// sdk-source/main/main.c，替换官方 demo 入口。
//
// 详细设计: ../02-sdk/desktop_index_player.h
#include "bsp_display.h"
#include "bsp_button.h"
#include "desktop_index_player.h"
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
    ESP_LOGI(TAG, "Booting desktop_index_player v2...");

    // NVS（WiFi 凭据 + 缓存）
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // BSP + LVGL
    ESP_ERROR_CHECK(bsp_display_init());
    (void)bsp_lvgl_init();

    // 玩法初始化
    ESP_ERROR_CHECK(dip_init());

    // 后台拉取（如已配网）
    xTaskCreate(fetch_task, "dip_fetch_start", 4096, NULL, 5, NULL);

    // 主循环
    dip_main_loop();
}
