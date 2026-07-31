#include "config.h"
#include "audio.h"
#include "auto_run.h"
#include "flash_audio.h"
#include "http_server.h"
#include "panda_samples.h"
#include "power.h"
#include "servo.h"
#include "sync_audio.h"
#include "wifi.h"

#include <esp_log.h>
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

#if !OFFLINE_DEMO
    // --- WiFi ---
    InitWiFi();
    bool online = WaitForWiFi(WIFI_STA_TIMEOUT_S);
#endif

    flash_audio_init();
    PlayPandaSound(PANDA_SOUND_熊猫叫声_爱给网_AIGEI_COM);  // startup chime

#if !OFFLINE_DEMO
    // Start HTTP early (available during sync)
    StartHttpServer();

    // --- Sync ---
    if (online) {
        WiFiPowerSave(false);           // disable PS during download
        sync_audio_files();
        WiFiPowerSave(true);            // re-enable PS for battery life
    } else {
        ESP_LOGW(TAG, "Offline — using existing flash content");
    }
#else
    ESP_LOGI(TAG, "Offline demo — skip WiFi, use flash audio");
#endif

#if ENABLE_AUTO_RUN
    InitAutoRun();

#endif
}
