/**
 * @file hxd039b2.h
 * @brief HXD039B2-MML 红外串口收发模块驱动
 *
 * 控制命令格式: {0x30, 0x06, codeGrud[0], codeGrud[1], CMD}
 * 学习码:       {0x30, 0x70, 0xA0}
 * 通过 USART3 (PB10 TX, 57600bps) 通信。
 */

#ifndef __HXD039B2_H__
#define __HXD039B2_H__

#include <stdint.h>
#include <stdbool.h>

/* ── 学习状态枚举 (给 OLED 页面使用) ── */
#define HXD_LEARN_IDLE    0
#define HXD_LEARN_ACTIVE  1
#define HXD_LEARN_DONE    2
#define HXD_LEARN_TIMEOUT 3

/* ── 空调电源 ── */
typedef enum {
  HXD_AC_POWER_OFF = 0x80,
  HXD_AC_POWER_ON  = 0x81,
} hxd_ac_power_t;

/* ── 运行模式 ── */
typedef enum {
  HXD_AC_MODE_AUTO = 0xA1,
  HXD_AC_MODE_COOL = 0xA2,
  HXD_AC_MODE_DRY  = 0xA3,
  HXD_AC_MODE_FAN  = 0xA4,
  HXD_AC_MODE_HEAT = 0xA5,
} hxd_ac_mode_t;

/* ── 风速 ── */
typedef enum {
  HXD_AC_FAN_AUTO = 0x51,
  HXD_AC_FAN_LOW  = 0x52,
  HXD_AC_FAN_MID  = 0x53,
  HXD_AC_FAN_HIGH = 0x54,
} hxd_ac_fan_t;

void hxd039b2_init(void);
void hxd039b2_send_cmd(uint8_t cmd);
void hxd039b2_ac_power(hxd_ac_power_t power);
void hxd039b2_ac_temperature(uint8_t temp);       /* 16 ~ 31 ℃ */
void hxd039b2_ac_mode(hxd_ac_mode_t mode);
void hxd039b2_ac_fan(hxd_ac_fan_t fan);

/* ── 学习功能 ── */
void hxd039b2_start_learn(void);                  /* 启动学习 (逐字节发+700ms等+RX开) */
int  hxd039b2_learn_state(void);                  /* 返回 learn_state 枚举值 */
bool hxd039b2_learn_done(void);                   /* true=学习成功完成 */
void hxd039b2_learn_reset(void);                  /* 取消学习, 关RX中断 */
int  hxd039b2_busy_state(void);                   /* 1=HIGH(空闲), 0=LOW(忙) */

void hxd039b2_get_codeGrud(uint8_t *buf);         /* 读取当前码组 (2 bytes) */
void hxd039b2_set_codeGrud(uint8_t h, uint8_t l); /* 设置码组 */

#endif /* __HXD039B2_H__ */
