/**
 * @file axk_radar.c
 * @author XEMOWO
 * @brief Rd-03L_V2 24GHz mmWave 雷达人体存在检测模块实现
 * @version 0.1
 * @date 2026-07-14
 *
 * @copyright Copyright (c) 2026
 *
 */
#include "axk_radar.h"

/** @brief 连续无人计数，外部通过 axk_radar_check_absent() 驱动 */
static uint16_t g_radar_absent_cnt = 0;

/**
 * @brief 初始化雷达模块
 */
void axk_radar_init(void)
{
    g_radar_absent_cnt = 0;
}

/**
 * @brief 读取雷达人体存在检测结果
 *
 * OT1 输出: 高电平=有人, 低电平=无人
 */
bool axk_radar_is_present(void)
{
    return (HAL_GPIO_ReadPin(RADAR_GPIO_Port, RADAR_Pin) == GPIO_PIN_SET);
}

/**
 * @brief 获取连续无人计数
 */
uint16_t axk_radar_get_absent_count(void)
{
    return g_radar_absent_cnt;
}

/**
 * @brief 重置连续无人计数
 */
void axk_radar_reset_absent_count(void)
{
    g_radar_absent_cnt = 0;
}

/**
 * @brief 累加无人计数并检查是否达到阈值
 *
 * 调用方应周期性（~100ms）调用此函数。
 * 当 PA8=HIGH（有人）时自动清零计数。
 * 当 PA8=LOW（无人）时累加计数。
 *
 * @return true  连续无人次数 >= AXK_RADAR_ABSENT_THRESHOLD
 * @return false 尚未达到阈值
 */
bool axk_radar_check_absent(void)
{
    if (axk_radar_is_present()) {
        g_radar_absent_cnt = 0;
        return false;
    }

    if (g_radar_absent_cnt < AXK_RADAR_ABSENT_THRESHOLD) {
        g_radar_absent_cnt++;
    }

    return (g_radar_absent_cnt >= AXK_RADAR_ABSENT_THRESHOLD);
}
