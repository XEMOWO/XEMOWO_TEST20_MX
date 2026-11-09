/**
 * @file hxd039b2.c
 * @brief HXD039B2-MML 驱动 — 红外空调控制 + 学习功能
 *
 * 控制命令: {0x30, 0x06, codeGrud[0], codeGrud[1], CMD}
 * 学习码:   {0x30, 0x70, 0xA0}
 * USART3: PB10 TX / PB11 RX / PB1 BUSY @ 57600-8N1
 * RX 中断始终开启（和参考代码一致），收满 2 字节即为码组。
 */

#include "hxd039b2.h"
#include "usart.h"
#include "log.h"
#include "stm32f1xx_hal.h"
#include <stdbool.h>
#include <string.h>

extern UART_HandleTypeDef huart3;

#define HXD_BUSY_PORT   GPIOB
#define HXD_BUSY_PIN    GPIO_PIN_1
#define LEARN_DELAY_MS  700

/* ── 码组 ── */
static uint8_t ac_codeGrud[2] = {0x03, 0x3E};

/* ── 学习状态 ── */
typedef enum {
  LEARN_IDLE,
  LEARN_ACTIVE,
  LEARN_DONE,
  LEARN_TIMEOUT,
} learn_state_t;

static volatile learn_state_t learn_state = LEARN_IDLE;
static volatile uint32_t      learn_start_tick = 0;

/* ── RX 环形缓冲 (2 字节，收满表示码组到达) ── */
static volatile uint8_t  rx_ring[2] = {0};
static volatile uint8_t  rx_idx = 0;
static volatile bool     rx_full = false;

/* ── USART3 RX 中断回调（始终运行，每收到 2 字节更新码组） ── */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance != USART3) return;

  if (learn_state == LEARN_ACTIVE) {
    /* 学习模式：收满 2 字节 = 码组到达 */
    ac_codeGrud[0] = rx_ring[0];
    ac_codeGrud[1] = rx_ring[1];
    learn_state = LEARN_DONE;
    log_info("IR learn OK: %02X %02X", ac_codeGrud[0], ac_codeGrud[1]);
  } else {
    /* 非学习模式：仅更新码组（如外部手动写入） */
    rx_full = true;
  }
  /* 重新启动 RX 接收下一组 2 字节 */
  rx_idx = 0;
  memset((void *)rx_ring, 0, sizeof(rx_ring));
  HAL_UART_Receive_IT(&huart3, (uint8_t *)rx_ring, 2);
}

/* ── 每字节中断回调（HAL_UART_Receive_IT 用 RxCpltCallback 就够了，无需这个） ── */
/* HAL_UART_Receive_IT(&huart3, buf, 2) → 收满 2 字节触发 RxCpltCallback */

/* ── 初始化 ── */
void hxd039b2_init(void) {
  /* 始终开启 RX 中断，和参考代码一致 */
  HAL_UART_Receive_IT(&huart3, (uint8_t *)rx_ring, 2);
}

/* ── BUSY 读取 ── */
static int busy_read(void) {
  return (HAL_GPIO_ReadPin(HXD_BUSY_PORT, HXD_BUSY_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

/* ── 发送红外控制指令 ── */
void hxd039b2_send_cmd(uint8_t cmd) {
  if (!busy_read()) {
    log_error("HXD busy, skip send");
    return;
  }
  uint8_t frame[5] = {0x30, 0x06, ac_codeGrud[0], ac_codeGrud[1], cmd};
  log_info("IR: %02X %02X %02X %02X %02X", frame[0], frame[1], frame[2], frame[3], frame[4]);
  HAL_UART_Transmit(&huart3, frame, 5, HAL_MAX_DELAY);
}

void hxd039b2_ac_power(hxd_ac_power_t power)    { hxd039b2_send_cmd((uint8_t)power); }
void hxd039b2_ac_temperature(uint8_t temp) {
  if (temp < 16) temp = 16;
  if (temp > 31) temp = 31;
  hxd039b2_send_cmd(0x40 + (temp - 16));
}
void hxd039b2_ac_mode(hxd_ac_mode_t mode)        { hxd039b2_send_cmd((uint8_t)mode); }
void hxd039b2_ac_fan(hxd_ac_fan_t fan)           { hxd039b2_send_cmd((uint8_t)fan); }

/* ── 启动红外学习 ── */
void hxd039b2_start_learn(void) {
  learn_state = LEARN_ACTIVE;
  rx_idx = 0;
  rx_full = false;
  memset((void *)rx_ring, 0, sizeof(rx_ring));

  log_info("IR learn start");
  /* 发学习码：和参考代码一样连发 3 字节 */
  uint8_t learn_code[3] = {0x30, 0x70, 0xA0};
  HAL_UART_Transmit(&huart3, learn_code, 3, HAL_MAX_DELAY);

  /* 等 700ms */
  HAL_Delay(LEARN_DELAY_MS);

  learn_start_tick = HAL_GetTick();
  log_info("IR learn: waiting, BUSY=%d", busy_read());
}

/* ── 状态查询 ── */
int hxd039b2_learn_state(void) {
  if (learn_state == LEARN_ACTIVE && HAL_GetTick() - learn_start_tick > 15000) {
    log_error("IR learn timeout");
    learn_state = LEARN_TIMEOUT;
  }
  return (int)learn_state;
}

bool hxd039b2_learn_done(void) { return learn_state == LEARN_DONE; }

void hxd039b2_learn_reset(void) {
  learn_state = LEARN_IDLE;
  rx_idx = 0;
  rx_full = false;
}

/* ── 码组读写 ── */
void hxd039b2_get_codeGrud(uint8_t *buf) {
  buf[0] = ac_codeGrud[0];
  buf[1] = ac_codeGrud[1];
}
void hxd039b2_set_codeGrud(uint8_t h, uint8_t l) {
  ac_codeGrud[0] = h;
  ac_codeGrud[1] = l;
}
int hxd039b2_busy_state(void) { return busy_read(); }
