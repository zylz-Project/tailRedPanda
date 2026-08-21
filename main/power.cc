#include "power.h"
#include "config.h"
#include "audio.h"
#include "chat.h"
#include "servo.h"

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "action_cat";

static adc_oneshot_unit_handle_t adc_handle_ = nullptr;
static adc_cali_handle_t adc_cali_handle_ = nullptr;
static int battery_vbat_filtered_mv_ = 0;
static int battery_level_ = 0;

static int AdcToMv(int raw)
{
  if (adc_cali_handle_)
  {
    int mv = 0;
    if (adc_cali_raw_to_voltage(adc_cali_handle_, raw, &mv) == ESP_OK)
      return mv;
  }
  return raw * 3300 / 4096;
}

/* ============================================================================
 * 电源键状态机 (参照 testBoard_wifiweb):
 *   IDLE → 按下消抖 → PRESSED(长按计时) → 松开消抖 → IDLE
 *   - 时间戳计时 (esp_log_timestamp), 不受调度抖动影响
 *   - 长按达到阈值即关机(无需松开); 松开可取消
 *   - 上电防误关机: 上电瞬间按键被按住时, 先释放一次才生效(armed)
 *   - 短按释放: 双击(400ms内)切换对话
 * ========================================================================== */
#define POWER_PRESS_MV 1000  // <1V = 按下 (与参考一致, 用电压判定)
typedef enum {
    BTN_IDLE = 0,
    BTN_DEBOUNCE_PRESS,
    BTN_PRESSED,
    BTN_DEBOUNCE_RELEASE,
} power_btn_state_t;

static void PowerShutdown()
{
    ESP_LOGW(TAG, "长按 %dms, 执行关机序列...", POWER_LONG_PRESS_MS);

    /* 0. 关机提示音 (合成下扬叮咚) */
    AudioPlayChime(false);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 1. 舵机回中 + 断开舵机电源 */
    SetServoAngle(SERVO_HEAD, SERVO_HEAD_DEFAULT_ANGLE);
    SetServoAngle(SERVO_TAIL_LR, SERVO_TAIL_LR_DEFAULT_ANGLE);
    SetServoAngle(SERVO_TAIL_UD, SERVO_TAIL_UD_DEFAULT_ANGLE);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(SERVO_POWER_GPIO, 0);
    ESP_LOGI(TAG, "舵机电源 IO%d -> LOW", SERVO_POWER_GPIO);

    /* 2. 切断电源自锁 */
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(POWER_CTRL_GPIO, 0);
    ESP_LOGW(TAG, "POWER_CTRL IO%d -> LOW, 系统即将断电", POWER_CTRL_GPIO);
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void InitPower()
{
  // --- POWER_CTRL (IO7): latch HIGH immediately ---
  gpio_config_t pwr_ctrl = {
      .pin_bit_mask = 1ULL << POWER_CTRL_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_ctrl);
  gpio_set_level(POWER_CTRL_GPIO, 1);
  ESP_LOGI(TAG, "POWER_CTRL IO%d HIGH, power latched", POWER_CTRL_GPIO);

  // POWER_OUT (IO6): ADC monitoring via ADC1_CH5
  gpio_config_t pwr_out = {
      .pin_bit_mask = 1ULL << POWER_OUT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_out);
  gpio_hold_en(POWER_OUT_GPIO);

  // ADC init
  adc_oneshot_unit_init_cfg_t adc_cfg = {
      .unit_id = ADC_UNIT_1,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&adc_cfg, &adc_handle_) == ESP_OK)
  {
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_5, &ch);      // IO6 power button
    adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &ch); // IO3 battery

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_5,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cali, &adc_cali_handle_);
  }

  // Power monitor task: 4-state machine + long-press shutdown + double-click + battery
  xTaskCreate([](void *)
              {
    vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for stabilization

    power_btn_state_t st = BTN_IDLE;
    uint32_t st_since = 0;   // 进入当前状态的时刻 (ms)
    uint32_t press_start = 0; // 确认按下时刻 (ms)
    bool     armed = false;   // 上电防误关机: 释放一次后生效
    bool     boot_hold_logged = false;
    uint32_t last_click_ms = 0;
    uint32_t click_count = 0;
    uint32_t tick = 0;

    while (true) {
        tick++;
        int raw = 0;
        adc_oneshot_read(adc_handle_, ADC_CHANNEL_5, &raw);
        bool pressed = (AdcToMv(raw) < POWER_PRESS_MV);
        uint32_t now = esp_log_timestamp();

        switch (st) {
        case BTN_IDLE:
            if (pressed) {
                st = BTN_DEBOUNCE_PRESS;
                st_since = now;
            } else if (!armed) {
                /* 上电后第一次确认释放 → 武装长按关机 */
                armed = true;
                boot_hold_logged = false;
                ESP_LOGI(TAG, "电源键已释放, 长按关机功能生效");
            }
            break;

        case BTN_DEBOUNCE_PRESS:
            if (!pressed) {
                st = BTN_IDLE;  // 毛刺
            } else if (now - st_since >= POWER_DEBOUNCE_MS) {
                if (!armed) {
                    /* 上电期间一直按着: 不进入长按, 等松开 */
                    if (!boot_hold_logged) {
                        boot_hold_logged = true;
                        ESP_LOGI(TAG, "上电按住电源键, 长按忽略 (松开后生效)");
                    }
                    st = BTN_IDLE;
                    break;
                }
                st = BTN_PRESSED;
                press_start = now;
                ESP_LOGI(TAG, "电源键按下确认");
            }
            break;

        case BTN_PRESSED:
            if (!pressed) {
                st = BTN_DEBOUNCE_RELEASE;
                st_since = now;
            } else {
                int hold = (int)(now - press_start);
                if (hold >= POWER_LONG_PRESS_MS) {
                    PowerShutdown();  // 长按到达 → 关机(无需松开)
                }
                /* 长按进度提示 (每 500ms) */
                static int tip = 0;
                int t = hold / 500;
                if (t != tip && t > 0) { tip = t; ESP_LOGI(TAG, "长按中 %d.%d s (共需 %d s), 松开可取消", hold/1000, (hold%1000)/100, POWER_LONG_PRESS_MS/1000); }
                if (hold / 500 == 0) tip = 0;
            }
            break;

        case BTN_DEBOUNCE_RELEASE:
            if (pressed) {
                st = BTN_PRESSED;  // 没松干净
            } else if (now - st_since >= POWER_DEBOUNCE_MS) {
                int dur = press_start ? (int)(now - press_start) : 0;
                ESP_LOGI(TAG, "电源键释放 (按住 %dms)", dur);
                if (dur < POWER_LONG_PRESS_MS) {
                    /* 短按 → 双击检测(切对话) */
                    if (now - last_click_ms <= 400) click_count++;
                    else click_count = 1;
                    last_click_ms = now;
                    if (click_count >= 2) {
                        click_count = 0;
                        ESP_LOGI(TAG, "DOUBLE CLICK -> toggle chat");
                        ChatToggle();
                    }
                }
                st = BTN_IDLE;
                press_start = 0;
            }
            break;
        }

        /* ---------- 电池电压 (IO3, 每 5s) ---------- */
        if (tick % BATTERY_READ_TICKS == 0) {
            int64_t bat_sum = 0;
            for (int n = 0; n < 32; n++) {
                int bat_raw = 0;
                adc_oneshot_read(adc_handle_, BATTERY_ADC_CHANNEL, &bat_raw);
                bat_sum += bat_raw;
            }
            int bat_avg = static_cast<int>(bat_sum / 32);
            int vpin_mv = AdcToMv(bat_avg);
            int vbat_mv = static_cast<int>(vpin_mv * BATTERY_DIVIDER_RATIO);
            if (battery_vbat_filtered_mv_ == 0) battery_vbat_filtered_mv_ = vbat_mv;
            else battery_vbat_filtered_mv_ += (vbat_mv - battery_vbat_filtered_mv_) / 5;
            int vbat_f = battery_vbat_filtered_mv_;
            battery_level_ = (vbat_f - BATTERY_EMPTY_VOLTAGE_MV) * 100 /
                             (BATTERY_FULL_VOLTAGE_MV - BATTERY_EMPTY_VOLTAGE_MV);
            if (battery_level_ < 0) battery_level_ = 0;
            if (battery_level_ > 100) battery_level_ = 100;
            ESP_LOGI(TAG, "[BAT] %dmV (filt=%dmV) level=%d%%",
                     vbat_mv, vbat_f, battery_level_);
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    } }, "power_mon", 4096, nullptr, 1, nullptr);
  ESP_LOGI(TAG, "Power monitor started (state machine, long press %dms, debounce %dms)",
           POWER_LONG_PRESS_MS, POWER_DEBOUNCE_MS);
}

int GetBatteryLevel() { return battery_level_; }

int GetBatteryVoltageMv() { return battery_vbat_filtered_mv_; }
