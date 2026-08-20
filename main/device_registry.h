#pragma once

#include <freertos/FreeRTOS.h>

#include <cstddef>

#define DEVICE_API_TOKEN_SIZE 96

/**
 * 启动 Audio Hub 设备注册、激活轮询和心跳后台任务。
 * 调用前必须已连接 Wi-Fi；重复调用不会创建重复任务。
 */
void device_registry_start(void);

/** 等待服务端确认设备已激活且设备令牌有效。 */
bool device_registry_wait_for_activation(TickType_t timeout_ticks);

/** 复制经过服务端验证的设备令牌；尚未激活时返回 false。 */
bool device_registry_get_api_token(char *output, size_t output_size);
