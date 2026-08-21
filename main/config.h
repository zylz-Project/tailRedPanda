#pragma once

// === Feature toggles ===
#define OFFLINE_DEMO 0           // 0 = 正常: 联网后从 Audio Hub 同步音频到 Flash; 1 = 跳过WiFi/同步
#define ENABLE_AUTO_RUN 1        // 1 = compile auto-run feature, 0 = disable entirely
#define AUTO_RUN_DEFAULT_ON 1    // 1 = active on power-up, 0 = start paused
#define AUTO_RUN_DEFAULT_HARD 0  // 1 = hard swing (instant to extrema + hold), 0 = sin² smooth
#define HARD_SWING_SPEED_X 4.0f  // Hard-swing period multiplier (>1 = faster, 4x = continuous)
#define CHAT_ENABLE 1            // 1 = compile LLM realtime chat (double-click power to toggle)

// === Power management ===
#define POWER_CTRL_GPIO GPIO_NUM_7   // Latch HIGH = power on, LOW = power off
#define POWER_OUT_GPIO GPIO_NUM_6    // ADC read, <1V = button pressed
#define POWER_ADC_THRESHOLD 1241     // 1.0V threshold for pressed/released
#define POWER_LONG_PRESS_MS 1500
#define POWER_DEBOUNCE_MS 50
#define POWER_POLL_MS 20

// Battery ADC: IO3 = ADC1_CH2
// Voltage divider: R_upper=2k, R_lower=4.7k
// Vpin = Vbat * 4.7 / (2 + 4.7) → Vbat = Vpin * 1.426
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_DIVIDER_RATIO 2
#define BATTERY_EMPTY_VOLTAGE_MV 3200
#define BATTERY_FULL_VOLTAGE_MV 4200
#define BATTERY_READ_TICKS 250  // Every 5s (250 * 20ms)

// === Servo pins (3x 180° servos) ===
// IO18: HEAD — 0°=turn right, 90°=center, 180°=turn left
// IO15: TAIL left-right — 0°=leftmost, 90°=center, 180°=rightmost
// IO16: TAIL up-down — 0°=up, 90°=middle, 180°=down
#define SERVO_POWER_GPIO GPIO_NUM_4
#define SERVO_HEAD_GPIO GPIO_NUM_18
#define SERVO_TAIL_LR_GPIO GPIO_NUM_15
#define SERVO_TAIL_UD_GPIO GPIO_NUM_16
#define SERVO_HEAD_DEFAULT_ANGLE 90
#define SERVO_TAIL_LR_DEFAULT_ANGLE 90
#define SERVO_TAIL_UD_DEFAULT_ANGLE 180  // 0°=上, 90°=中, 180°=下
#define SERVO_MAX_ANGLE 180

// === LEDC PWM 50Hz ===
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_FREQ_HZ 50
#define SERVO_DUTY_RES LEDC_TIMER_13_BIT
#define SERVO_MAX_DUTY ((1 << 13) - 1)
#define SERVO_PERIOD_US 20000

// === Audio (ES8311 codec over I2C + I2S) ===
#define AUDIO_I2C_SDA_PIN GPIO_NUM_2
#define AUDIO_I2C_SCL_PIN GPIO_NUM_38
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_NC
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_48
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_46
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_14
#define AUDIO_SAMPLE_RATE 48000  // Opus source is 48kHz
#define AUDIO_OUTPUT_VOLUME 95   // 0-100
#define AUDIO_SILENT_INTERVAL_MIN_S 4
#define AUDIO_SILENT_INTERVAL_MAX_S 6

// === External SPI Flash ===
// 1 = W25Q256 (NOR), 2 = W25N01GVZEIG (SPI NAND, 128MB)
// 当前板子接的是 W25N01GVZEIG (NOR)
#define EXTERNAL_FLASH_TYPE 2
#define SPI_FLASH_CS_PIN   GPIO_NUM_10
#define SPI_FLASH_CLK_PIN  GPIO_NUM_9
#define SPI_FLASH_MOSI_PIN GPIO_NUM_47
#define SPI_FLASH_MISO_PIN GPIO_NUM_21

// === WiFi Station ===
// 默认 WiFi 信息已注释, 用于模拟"无 WiFi"场景测试配网热点。
// 恢复后重新编译即可 (或通过 NVS 配网保存凭据)。
#define WIFI_STA_SSID     "huachuang109"
#define WIFI_STA_PASSWORD "huachuang109"
#define WIFI_STA_TIMEOUT_S 10   // 连接超时(秒), 超时后继续运行

// === Audio Sync Server ===
#define SYNC_SERVER_IP    "192.168.1.7"    // 用户电脑 IP, 按需修改
#define SYNC_SERVER_PORT  5000
#define SYNC_PRODUCT_ID   "tail-wagging-panda" // 产品标识, 用于服务端区分音频
#define SYNC_DOWNLOAD_BUF_SIZE 4096        // 下载缓冲区大小
