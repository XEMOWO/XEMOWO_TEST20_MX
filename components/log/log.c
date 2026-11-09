/**
 * @file log.c
 * @author SeaHi-Mo (Seahi-Mo@Foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-09-29
 *
 * @copyright Copyright (c) 2025
 *
 */
#include "log.h"
#include "usart.h"

#define LOG_BUFFER_SIZE 256

LogLevel g_log_level = LOG_LEVEL_DEBUG;

/**
 * @brief
 *
 */
void log_printf(const char *format, ...) {
  char buffer[LOG_BUFFER_SIZE]; // 用于存储格式化后的字符串
  va_list args;

  // 1. 初始化可变参数列表
  va_start(args, format);
  int len = vsnprintf(buffer, LOG_BUFFER_SIZE, format, args);
  va_end(args);
  /* vsnprintf 返回的是“原本想写多少”，超长时必须按实际写入量截断，
   * 否则 HAL_UART_Transmit 会越界读栈上的 buffer */
  if (len < 0) return;
  if (len >= LOG_BUFFER_SIZE) len = LOG_BUFFER_SIZE - 1;
  HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)len, 100);
  //   HAL_UART_Transmit_DMA(&huart1, (uint8_t *)buffer, len);
}

/* 原样输出一段字符串：不做格式化、不截断（仅按 strlen 发送），
 * 用于把语音模组的整帧 JSON 直接打到日志串口 */
void log_raw(const char *data) {
  if (data == NULL) return;
  size_t len = strlen(data);
  if (len == 0) return;
  if (len > 0xFFFFu) len = 0xFFFFu;
  HAL_UART_Transmit(&huart1, (uint8_t *)data, (uint16_t)len, 100);
}

/**
 * @brief 打印错误日志
 *
 */
void log_init(void) {
  // 初始化串口1

  MX_USART1_UART_Init();
}
