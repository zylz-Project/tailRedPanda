#include "config.h"
#include "audio.h"
#include "auto_run.h"
#include "chat.h"
#include "device_registry.h"
#include "flash_audio.h"
#include "http_server.h"
#include "panda_samples.h"
#include "power.h"
#include "servo.h"
#include "sync_audio.h"
#include "wifi.h"
#include "wifi_config.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "main";

extern "C" void app_main()
{
    nvs_flash_init();
    InitPower();
    InitAudio();
    InitServos();
    ESP_LOGI(TAG, "heap free=%lu bytes after audio/servo (internal=%lu, psram=%lu)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // --- WiFi (chat depends on it; 配网: 连不上就进配网热点) ---
    InitWiFi();
    bool has_creds = WifiConfigHasCredentials();
    bool online = false;
    if (has_creds) {
        online = WaitForWiFi(WIFI_STA_TIMEOUT_S);
    }
    // 无已存凭据: 不等 15s, 立刻开热点; 有凭据但连不上(如在外无信号): 超时后也开热点,
    // 这样任何情况下都能重新配网。
    if (!online) {
        ESP_LOGI(TAG, "WiFi offline%s — starting config portal",
                 has_creds ? " (credentials unreachable)" : " (no saved credentials)");
        WifiConfigStartPortal();
    }

    flash_audio_init();
    PlayPandaSound(PANDA_SOUND_熊猫叫声_爱给网_AIGEI_COM);  // startup chime

    // Start HTTP early (web UI + WiFi pairing page, works on STA & AP)
    StartHttpServer();

#if !OFFLINE_DEMO
    // --- Sync ---
    if (online) {
        device_registry_start();
        ESP_LOGI(TAG, "等待设备在 Audio Hub 后台完成绑定…");
        if (device_registry_wait_for_activation(pdMS_TO_TICKS(30000))) {
            char api_token[DEVICE_API_TOKEN_SIZE] = {};
            if (device_registry_get_api_token(api_token, sizeof(api_token))) {
                WiFiPowerSave(false);           // disable PS during download
                sync_audio_files(api_token);
                WiFiPowerSave(true);            // re-enable PS for battery life
            } else {
                ESP_LOGE(TAG, "无法读取已验证的设备令牌，跳过同步");
            }
        } else {
            ESP_LOGW(TAG, "设备未激活(超时)，跳过音频同步");
        }
    } else {
        ESP_LOGW(TAG, "Offline — using existing flash content");
    }
#else
    ESP_LOGI(TAG, "Offline demo — skip registry/sync, use flash audio");
#endif

#if ENABLE_AUTO_RUN
    InitAutoRun();
#endif

    // --- LLM chat (double-click power button to toggle) ---
    ChatInit();
}
