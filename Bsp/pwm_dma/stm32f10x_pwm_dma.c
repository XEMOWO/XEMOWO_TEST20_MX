/**
 * @file stm32f10x_pwm_dma.c
 * @author SeaHi-Mo (Seahi-Mo@Foxmail.com)
 * @brief
 * @version 0.1
 * @date 2026-01-30
 *
 * @copyright Copyright (c) 2026
 *
 */
#include "stm32f10x_pwm_dma.h"
#include "FreeRTOS.h"
#include "log.h"
#include "portable.h"

#include "axk_ws2812.h"
#include "tim.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint16_t ws2812_max_led_num = 0;
static uint16_t *buffer_ptr = NULL;
extern DMA_HandleTypeDef hdma_tim1_ch4_trig_com;

void bsp_pwm_dma_init(uint16_t led_num) {
  ws2812_max_led_num = led_num;
  if (buffer_ptr != NULL) {
    vPortFree(buffer_ptr);
  }
  buffer_ptr =
      (uint16_t *)pvPortMalloc((ws2812_max_led_num * 24 + 1) * sizeof(uint16_t));
  if (buffer_ptr == NULL) {
    log_error("Failed to allocate memory for WS2812 buffer");
  }
}

void bsp_pwm_dma_deinit(void) {
  if (buffer_ptr != NULL) {
    vPortFree(buffer_ptr);
    buffer_ptr = NULL;
  }
  ws2812_max_led_num = 0;
}

static void convert_rgb_to_ws2812_data(uint8_t r, uint8_t g, uint8_t b,
                                       uint16_t *buffer) {
  if (buffer == NULL) return;
  for (unsigned char i = 0; i < 8; i++) {
    buffer[i] = (g & (0x80 >> i)) ? CODE1 : CODE0;
    buffer[i + 8] = (r & (0x80 >> i)) ? CODE1 : CODE0;
    buffer[i + 16] = (b & (0x80 >> i)) ? CODE1 : CODE0;
  }
}

int bsp_pwm_dma_with_num(void) {
  if (axk_ws2812_strip_dev == NULL || axk_ws2812_strip_dev->dev == NULL) {
    log_error("WS2812 device not initialized");
    return -1;
  }
  uint16_t total_bits = 0;
  uint16_t *current_pos = buffer_ptr;
  uint16_t led_count = (axk_ws2812_strip_dev->led_count < WS2812_MAX_NUM)
                           ? axk_ws2812_strip_dev->led_count
                           : WS2812_MAX_NUM;
  for (uint16_t i = 0; i < led_count; i++) {
    uint8_t r = (uint8_t)(axk_ws2812_strip_dev->dev[i].color.r *
                          axk_ws2812_strip_dev->brightness);
    uint8_t g = (uint8_t)(axk_ws2812_strip_dev->dev[i].color.g *
                          axk_ws2812_strip_dev->brightness);
    uint8_t b = (uint8_t)(axk_ws2812_strip_dev->dev[i].color.b *
                          axk_ws2812_strip_dev->brightness);
    convert_rgb_to_ws2812_data(r, g, b, current_pos);
    current_pos += 24;
    total_bits += 24;
  }
  buffer_ptr[total_bits] = CODE_RESET;
  total_bits++;
  HAL_TIM_PWM_Stop_DMA(&htim1, TIM_CHANNEL_4);
  if (HAL_TIM_PWM_Start_DMA(&htim1, TIM_CHANNEL_4, (uint32_t *)buffer_ptr,
                            total_bits) != HAL_OK) {
    log_error("Failed to start DMA transmission");
    return -1;
  }
  return 0;
}