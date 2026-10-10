// voice_link.h — 工牌语音链路 (按住OK说话, Hermes 应答)
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VL_IDLE = 0,       // 空闲
    VL_RECORDING,      // 录音中 (按住 OK)
    VL_THINKING,       // 已发送, 等 Hermes
    VL_PLAYING,        // 播放回复
} vl_state_t;

typedef void (*vl_state_cb_t)(vl_state_t st);

// 初始化 (加载 NVS 里的桥地址, 注册状态回调)
esp_err_t vl_init(vl_state_cb_t cb);

// 是否已配过桥地址 (没配过应先进配网)
bool vl_is_configured(void);

// SoftAP 配网: 热点 HermesPass/12345678, 浏览器 http://192.168.4.1 填桥地址
esp_err_t vl_start_provisioning(void);

// 录音控制 (主循环轮询模式)
esp_err_t vl_rec_begin(void);           // 开始录音 (OK PRESS 时调)
void vl_rec_poll(void);                 // RECORDING 状态时在主循环里反复调
void vl_rec_stop(bool send);            // 停止; send=false 或时长<0.5s 则取消
void vl_rec_cancel(void);               // 立即取消

// 发送+回放任务 (vl_rec_stop(send=true) 内部创建, 外部一般不用)

// 配置读写
esp_err_t vl_load_config(void);
esp_err_t vl_save_bridge_url(const char *url);
const char *vl_get_bridge_url(void);

vl_state_t vl_get_state(void);
void vl_set_state_cb(vl_state_cb_t cb);

esp_err_t vl_deinit(void);

#ifdef __cplusplus
}
#endif