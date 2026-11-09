/**
 * @file axk_radar.h
 * @author XEMOWO
 * @brief Rd-03L_V2 24GHz mmWave 雷达人体存在检测模块
 * @version 0.1
 * @date 2026-07-14
 *
 * @copyright Copyright (c) 2026
 *
 */
#ifndef __AXK_RADAR_H__
#define __AXK_RADAR_H__

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 雷达无人确认去抖阈值（单位: 次，建议 ≥50 即 ~5 秒@100ms 间隔） */
#define AXK_RADAR_ABSENT_THRESHOLD 50

/**
 * @brief 初始化雷达模块
 *
 * @note PA8 引脚需在 MX_GPIO_Init() 中配置为 GPIO_MODE_INPUT + GPIO_PULLDOWN
 */
void axk_radar_init(void);

/**
 * @brief 读取雷达人体存在检测结果
 *
 * @return true  检测到人体存在（OT1=高电平）
 * @return false 未检测到人体（OT1=低电平）
 */
bool axk_radar_is_present(void);

/**
 * @brief 获取连续无人计数（用于外部去抖逻辑）
 *
 * @return uint16_t 当前连续无人计数值
 */
uint16_t axk_radar_get_absent_count(void);

/**
 * @brief 重置连续无人计数（有人时调用）
 */
void axk_radar_reset_absent_count(void);

/**
 * @brief 累加连续无人计数并检查是否达到阈值
 *
 * @return true  已达到无人确认阈值（确认人已离开）
 * @return false 尚未达到阈值
 */
bool axk_radar_check_absent(void);

#ifdef __cplusplus
}
#endif

#endif /* __AXK_RADAR_H__ */
