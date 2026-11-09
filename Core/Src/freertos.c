/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "axk_sht3x.h"
#include "axk_ssd1306.h"
#include "axk_radar.h"
#include "axk_ws2812.h"
#include "emMCP.h"
#include "hxd039b2.h"
#include "relay.h"
#include <stdlib.h>
#include "uartPort.h"
#include "usart.h"

/* 猫咪动画开关: 1=启用(占~28KB FLASH), 0=禁用 */
/* 注意！！！！ 猫咪动画会占20%的FLASH，启用之后可能会导致屏幕黑屏！！！！！！！！ */
#define CAT_ANIMATION_ENABLED 0

#include "salary_cat_frames.h"
#include "stm32f10x_bsp_spi.h"
#include <stdint.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RXBUFFSER_MAX_SIZE 256
#define RX_MSG_MAX        4
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for sht3x_read */
osThreadId_t sht3x_readHandle;
const osThreadAttr_t sht3x_read_attributes = {
  .name = "sht3x_read",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal1,
};
/* Definitions for ws2812_mode */
osThreadId_t ws2812_modeHandle;
const osThreadAttr_t ws2812_mode_attributes = {
  .name = "ws2812_mode",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal2,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static uint8_t rxBuffer[RXBUFFSER_MAX_SIZE] = {0};
static uint8_t uart1_rx_buf[128] = {0};
static char     rx_msg_buf[RX_MSG_MAX][RXBUFFSER_MAX_SIZE];
static volatile uint8_t rx_msg_count = 0;
static uint8_t  rx_msg_idx = 0;
static emMCP_t emMCP;
static bool relay_on = false;
static volatile bool serial_monitor_enabled = false;
static volatile uint8_t status_code = 0xFF;
static volatile bool status_updated = false;
axk_ws2812_strip_t ws2812 = {.led_count = WS2812_MAX_NUM};
color_t RED = {0xff, 0x00, 0x00};
color_t GREEN = {0x00, 0xff, 0x00};
color_t BLUE = {0x00, 0x00, 0xff};
bool sht30_is_init = false;

#define BTN_UP_PIN   GPIO_PIN_4
#define BTN_DOWN_PIN GPIO_PIN_8
#define BTN_PORT     GPIOB
#define PAGE_COUNT   9

static volatile uint8_t page_index       = 0;
static uint8_t  task_display_idx = 0;
static double   g_temp           = 0.0;
static double   g_hum            = 0.0;
extern emMCP_event_t emMCP_event;
extern char *uart_data_buf;
extern emMCP_tool_t mcp_tool_arry[MCP_SERVER_TOOL_NUMBLE_MAX];
static emMCP_event_t saved_ai_event = emMCP_EVENT_NONE;
static const char   *ai_wifi_status  = "未知";
static const char   *ai_event_text   = "无事件";
static volatile uint8_t ai_event_updated = 0;
#if CAT_ANIMATION_ENABLED
static uint8_t cat_frame = 0;
#endif
static uint8_t scroll_offset = 0;
static int8_t  scroll_dir    = 1;
static char    mcp_text_buf[128] = "";
static uint8_t mcp_scroll_off = 0;
static int8_t  mcp_scroll_dir = 1;
static char    rx_raw_buf[RXBUFFSER_MAX_SIZE];
static uint16_t rx_raw_size = 0;
static volatile uint8_t rx_raw_pending = 0;
#if CAT_ANIMATION_ENABLED
static char    mcp_fs_text[512] = "";
static volatile uint8_t mcp_fs_active = 0;
#endif

/* ---- Relay MCP 工具回调 (继电器，空调总电源) ---- */
static void relay_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[Relay SET] param: %s", js);
  cJSON_free(js);
  cJSON *state_item = cJSON_GetObjectItem(root, "state");
  if (state_item != NULL && cJSON_IsString(state_item)) {
    const char *s = state_item->valuestring;
    if (strcmp(s, "on") == 0) {
      relay_on = true;
      axk_relay_set(ON);
      log_info("Relay -> ON, PB5 -> HIGH");
    } else if (strcmp(s, "off") == 0) {
      relay_on = false;
      axk_relay_set(OFF);
      log_info("Relay -> OFF, PB5 -> LOW");
    } else {
      log_error("Relay: unknown state '%s', use 'on' or 'off'", s);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void relay_check_handler(void *arg) {
  (void)arg;
  log_info("[Relay CHECK] -> %s", relay_on ? "on" : "off");
  emMCP_ResponseValue(relay_on ? "on" : "off");
}

/* ---- ACTemperature MCP 工具回调 (温度调节) ---- */
static void ac_temperature_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[temperature SET] param: %s", js);
  cJSON_free(js);
  cJSON *temp_item = cJSON_GetObjectItem(root, "value");
  if (temp_item != NULL && cJSON_IsNumber(temp_item)) {
    uint8_t temp = (uint8_t)temp_item->valueint;
    hxd039b2_ac_temperature(temp);
    log_info("temperature -> %d C", temp);
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void ac_temperature_check_handler(void *arg) {
  (void)arg;
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

/* ---- ACMode MCP 工具回调 (运行模式) ---- */
static void ac_mode_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[acmode SET] param: %s", js);
  cJSON_free(js);
  cJSON *mode_item = cJSON_GetObjectItem(root, "value");
  if (mode_item != NULL && cJSON_IsString(mode_item)) {
    const char *mode_str = mode_item->valuestring;
    hxd_ac_mode_t mode;
    if (strcmp(mode_str, "auto") == 0)      mode = HXD_AC_MODE_AUTO;
    else if (strcmp(mode_str, "cool") == 0) mode = HXD_AC_MODE_COOL;
    else if (strcmp(mode_str, "dry") == 0)  mode = HXD_AC_MODE_DRY;
    else if (strcmp(mode_str, "fan") == 0)  mode = HXD_AC_MODE_FAN;
    else if (strcmp(mode_str, "heat") == 0) mode = HXD_AC_MODE_HEAT;
    else { log_error("acmode: unknown mode '%s'", mode_str); emMCP_ResponseValue(emMCP_CTRL_ERROR); return; }
    hxd039b2_ac_mode(mode);
    log_info("acmode -> %s", mode_str);
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void ac_mode_check_handler(void *arg) {
  (void)arg;
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

/* ---- ACPower MCP 工具回调 (红外开关空调) ---- */
static void ac_power_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[ACPower SET] param: %s", js);
  cJSON_free(js);
  cJSON *power_item = cJSON_GetObjectItem(root, "power");
  if (power_item != NULL && cJSON_IsString(power_item)) {
    const char *p = power_item->valuestring;
    if (strcmp(p, "on") == 0) {
      hxd039b2_ac_power(HXD_AC_POWER_ON);
      log_info("ACPower -> IR power ON (0x81)");
    } else if (strcmp(p, "off") == 0) {
      hxd039b2_ac_power(HXD_AC_POWER_OFF);
      log_info("ACPower -> IR power OFF (0x80)");
    } else {
      log_error("ACPower: unknown power '%s', use 'on' or 'off'", p);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void ac_power_check_handler(void *arg) {
  (void)arg;
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

/* ---- ACFan MCP 工具回调 (红外风速调节) ---- */
static void ac_fan_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[ACFan SET] param: %s", js);
  cJSON_free(js);
  cJSON *speed_item = cJSON_GetObjectItem(root, "speed");
  if (speed_item != NULL && cJSON_IsString(speed_item)) {
    const char *s = speed_item->valuestring;
    hxd_ac_fan_t fan;
    if (strcmp(s, "auto") == 0)      fan = HXD_AC_FAN_AUTO;
    else if (strcmp(s, "low") == 0)  fan = HXD_AC_FAN_LOW;
    else if (strcmp(s, "mid") == 0)  fan = HXD_AC_FAN_MID;
    else if (strcmp(s, "high") == 0) fan = HXD_AC_FAN_HIGH;
    else { log_error("ACFan: unknown speed '%s', use auto/low/mid/high", s); emMCP_ResponseValue(emMCP_CTRL_ERROR); return; }
    hxd039b2_ac_fan(fan);
    log_info("ACFan -> %s", s);
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void ac_fan_check_handler(void *arg) {
  (void)arg;
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

/* ---- LEDStrip MCP 工具回调 (WS2812 灯带控制) ---- */
static uint8_t  led_r = 255, led_g = 0, led_b = 0;
static uint8_t  led_brightness = 50;
static volatile bool led_on = false;
static char     led_mode[16] = "off";
static volatile bool radar_auto_off = false;

static void ledstrip_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  /* 串口监控开启时，仅允许 query 和调整亮度，其他指令被拦截 */
  if (serial_monitor_enabled) {
    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (mode_item && strcmp(mode_item->valuestring, "query") == 0) {
      /* query 放行 */
    } else {
      /* 非 query 时，只允许改亮度 */
      cJSON *br = cJSON_GetObjectItem(root, "brightness");
      if (br && cJSON_IsNumber(br)) {
        led_brightness = (uint8_t)br->valueint;
        log_info("[LEDStrip] 监控模式下仅更新亮度=%d", led_brightness);
      }
      emMCP_ResponseValue(emMCP_CTRL_OK);
      return;
    }
  }
  char *js = cJSON_PrintUnformatted(root);
  log_info("[LEDStrip SET] param: %s", js);
  cJSON_free(js);

  cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
  if (mode_item && cJSON_IsString(mode_item)) {
    const char *m = mode_item->valuestring;
    if (strcmp(m, "on") == 0) {
      led_on = true;
      radar_auto_off = false;
      snprintf(led_mode, sizeof(led_mode), "on");
      axk_ws2812_set_all_pixels_color(led_r, led_g, led_b, led_brightness / 100.0f);
    } else if (strcmp(m, "off") == 0) {
      led_on = false;
      snprintf(led_mode, sizeof(led_mode), "off");
      axk_ws2812_set_all_pixels_color(0, 0, 0, 0.0f);
    } else if (strcmp(m, "set") == 0) {
      cJSON *r = cJSON_GetObjectItem(root, "r");
      cJSON *g = cJSON_GetObjectItem(root, "g");
      cJSON *b = cJSON_GetObjectItem(root, "b");
      if (r && cJSON_IsNumber(r)) led_r = (uint8_t)r->valueint;
      if (g && cJSON_IsNumber(g)) led_g = (uint8_t)g->valueint;
      if (b && cJSON_IsNumber(b)) led_b = (uint8_t)b->valueint;
      snprintf(led_mode, sizeof(led_mode), "set");
      led_on = true;
      radar_auto_off = false;
      cJSON *br = cJSON_GetObjectItem(root, "brightness");
      if (br && cJSON_IsNumber(br)) led_brightness = (uint8_t)br->valueint;
      axk_ws2812_set_all_pixels_color(led_r, led_g, led_b, led_brightness / 100.0f);
    } else if (strcmp(m, "query") == 0) {
      snprintf(led_mode, sizeof(led_mode), "query");
      /* 查询：返回简单键值状态（不含双引号，避免破坏 JSON） */
      {
        char rsp[256];
        snprintf(rsp, sizeof(rsp),
          "mcp-responsive {\"role\":\"MCU\",\"msgType\":\"status\","
          "\"data\":\"on=%s r=%d g=%d b=%d brightness=%d\"}\r\n",
          led_on ? "true" : "false", led_r, led_g, led_b, led_brightness);
        uartPortSendData(rsp, strlen(rsp));
      }
      return;
    } else {
      log_error("ledstrip: unknown mode '%s'", m);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
    emMCP_ResponseValue(emMCP_CTRL_OK);
    return;
  }

  /* 无 mode 时按单独属性处理 */
  cJSON *r = cJSON_GetObjectItem(root, "r");
  cJSON *g = cJSON_GetObjectItem(root, "g");
  cJSON *b = cJSON_GetObjectItem(root, "b");
  cJSON *br = cJSON_GetObjectItem(root, "brightness");
  if (r && cJSON_IsNumber(r)) led_r = (uint8_t)r->valueint;
  if (g && cJSON_IsNumber(g)) led_g = (uint8_t)g->valueint;
  if (b && cJSON_IsNumber(b)) led_b = (uint8_t)b->valueint;
  if (br && cJSON_IsNumber(br)) led_brightness = (uint8_t)br->valueint;
  if (r || g || b || br) {
    led_on = true;
    radar_auto_off = false;
    snprintf(led_mode, sizeof(led_mode), "set");
    axk_ws2812_set_all_pixels_color(led_r, led_g, led_b, led_brightness / 100.0f);
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

static void ledstrip_check_handler(void *arg) {
  (void)arg;
  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"mode\":\"%s\",\"on\":%s,\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d}",
    led_mode, led_on ? "true" : "false", led_r, led_g, led_b, led_brightness);
  emMCP_ResponseValue(buf);
}

static void serial_monitor_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[SerialMonitor SET] param: %s", js);
  cJSON_free(js);

  cJSON *action = cJSON_GetObjectItem(root, "action");
  if (action && cJSON_IsString(action)) {
    if (strcmp(action->valuestring, "open") == 0) {
      serial_monitor_enabled = true;
      status_updated = true; /* 立即应用最近收到的 STATUS（如有） */
      log_info("[SerialMonitor] 串口监控已开启, last_code=%d", status_code == 0xFF ? -1 : status_code);
      emMCP_ResponseValue(emMCP_CTRL_OK);
      return;
    } else if (strcmp(action->valuestring, "close") == 0) {
      serial_monitor_enabled = false;
      status_code = 0xFF;
      status_updated = false;
      log_info("[SerialMonitor] 串口监控已关闭");
      emMCP_ResponseValue(emMCP_CTRL_OK);
      return;
    }
  }
  log_error("[SerialMonitor] unknown action, use 'open' or 'close'");
  emMCP_ResponseValue(emMCP_CTRL_ERROR);
}

static void serial_monitor_check_handler(void *arg) {
  (void)arg;
  char buf[64];
  snprintf(buf, sizeof(buf),
    "{\"enabled\":%s,\"code\":%d}",
    serial_monitor_enabled ? "true" : "false",
    status_code == 0xFF ? -1 : status_code);
  emMCP_ResponseValue(buf);
}

void emMCP_EventCallback(emMCP_event_t event, mcp_server_tool_type_t type, void *param) {
  (void)type;
  if (event == emMCP_EVENT_NONE) return;
  /* 瞬态命令响应不覆盖 AI 状态 */
  if (event == emMCP_EVENT_CMD_OK || event == emMCP_EVENT_CMD_ERROR) return;
  /* 提取 JSON 中 data 字段内容，用于页面 6 底行滚动显示 */
  if (uart_data_buf && uart_data_buf[0]) {
    cJSON *j = cJSON_Parse(uart_data_buf);
    if (j) {
      cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
      if (d && cJSON_IsString(d) && d->valuestring)
        snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", d->valuestring);
      else {
        /* MCP 类消息用 param */
        char *p = (char *)param;
        if (p && p[0]) snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", p);
      }
      cJSON_Delete(j);
    }
    mcp_scroll_off = 0;
    mcp_scroll_dir = 1;
  }
  saved_ai_event = event;
  /* 根据事件类型 + 原始字符串确定准确的显示文字 */
  switch (event) {
  case emMCP_EVENT_AI_START:           ai_event_text="AI启动"; break;
  case emMCP_EVENT_AI_WIFI_CONNNECT:
    if (uart_data_buf && strstr(uart_data_buf, "1.WiFi connect OK"))
      ai_event_text="WiFi已连";
    else ai_event_text="WiFi连接中";
    break;
  case emMCP_EVENT_AI_WIFI_CONNECTED:  ai_event_text="WiFi已连"; break;
  case emMCP_EVENT_AI_WIFI_GOT_IP:     ai_event_text="已获IP"; break;
  case emMCP_EVENT_AI_WIFI_DISCONNECT: ai_event_text="WiFi断开"; break;
  case emMCP_EVENT_AI_WAKE:            ai_event_text="已唤醒"; break;
  case emMCP_EVENT_AI_SLEEP:           ai_event_text="已休眠"; break;
  case emMCP_EVENT_AI_NETCFG:          ai_event_text="配网中"; break;
  case emMCP_EVENT_AI_NETERR:          ai_event_text="网络错"; break;
  case emMCP_EVENT_AI_OTAUPDATE:       ai_event_text="OTA中"; break;
  case emMCP_EVENT_AI_OTAOK:           ai_event_text="OTA完成"; break;
  case emMCP_EVENT_AI_OTAERR:          ai_event_text="OTA失败"; break;
  case emMCP_EVENT_CMD_OK:             ai_event_text="命令OK"; break;
  case emMCP_EVENT_CMD_ERROR:          ai_event_text="命令错误"; break;
  case emMCP_EVENT_AI_MCP_CMD:
    ai_event_text="MCP命令";
    log_info(">>> MCP_CMD: %s", (char *)param);
    break;
  case emMCP_EVENT_AI_MCP_Text:
    ai_event_text="MCP文本";
    log_info(">>> MCP_Text: %s", (char *)param);
    if (param) {
      cJSON *j = cJSON_Parse((char *)param);
      if (j) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "text");
        if (t && cJSON_IsString(t) && t->valuestring) {
#if CAT_ANIMATION_ENABLED
          snprintf(mcp_fs_text, sizeof(mcp_fs_text), "%s", t->valuestring);
          mcp_fs_active = 1;
#endif
        }
        cJSON_Delete(j);
      }
    }
    break;
  case emMCP_EVENT_AI_MCP_CHECK:
    ai_event_text="MCP检查";
    log_info(">>> MCP_CHECK: %s", (char *)param);
    break;
  default: break;
  }
  /* WiFi 状态跟踪：WakeUP / AI Start 说明 WiFi 已连 */
  switch (event) {
  case emMCP_EVENT_AI_START:
  case emMCP_EVENT_AI_WAKE:
    ai_wifi_status = "已连接"; break;
  case emMCP_EVENT_AI_WIFI_CONNNECT:
    if (uart_data_buf && strstr(uart_data_buf, "1.WiFi connect OK"))
      ai_wifi_status = "已连接";
    else
      ai_wifi_status = "连接中";
    break;
  case emMCP_EVENT_AI_WIFI_CONNECTED:  ai_wifi_status="已连接"; break;
  case emMCP_EVENT_AI_WIFI_GOT_IP:     ai_wifi_status="已获IP"; break;
  case emMCP_EVENT_AI_WIFI_DISCONNECT: ai_wifi_status="已断开"; break;
  case emMCP_EVENT_AI_NETCFG:          ai_wifi_status="配网中"; break;
  case emMCP_EVENT_AI_NETERR:          ai_wifi_status="网络错"; break;
  default: break;
  }
  ai_event_updated = 1;
}

static uint8_t btn_pressed(uint16_t pin) {
  if (HAL_GPIO_ReadPin(BTN_PORT, pin) == GPIO_PIN_RESET) {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(BTN_PORT, pin) == GPIO_PIN_RESET) {
      while (HAL_GPIO_ReadPin(BTN_PORT, pin) == GPIO_PIN_RESET);
      return 1;
    }
  }
  return 0;
}

/* ---- MCP 全屏文本显示辅助函数 (猫咪动画用) ---- */
#if CAT_ANIMATION_ENABLED
#define MCP_FS_LINE_BYTES 33  /* 最坏情况: 8个中文字 × 3字节 + \0 = 25, 取33安全 */
/* 将 UTF-8 文本按屏幕宽度（128px）拆分为多行 */
static uint8_t mcp_fs_break_lines(const char *utf8, char lines[][MCP_FS_LINE_BYTES], uint8_t max_lines) {
    uint8_t li = 0, lp = 0, pw = 0, i = 0;
    while (utf8[i] && li < max_lines) {
        uint8_t cw, cb;
        if ((utf8[i] & 0x80) == 0)          { cw = 8;  cb = 1; }
        else if ((utf8[i] & 0xE0) == 0xC0)  { cw = 16; cb = 2; }
        else if ((utf8[i] & 0xF0) == 0xE0)  { cw = 16; cb = 3; }
        else if ((utf8[i] & 0xF8) == 0xF0)  { cw = 16; cb = 4; }
        else { i++; continue; }
        /* 像素宽度不够 或 缓冲区放不下完整字符 → 换行 */
        if (lp > 0 && (pw + cw > 128 || lp + cb >= MCP_FS_LINE_BYTES)) {
            lines[li][lp] = '\0'; li++; lp = 0; pw = 0;
        }
        for (uint8_t b = 0; b < cb; b++) lines[li][lp++] = utf8[i + b];
        pw += cw; i += cb;
    }
    if (lp > 0 && li < max_lines) { lines[li][lp] = '\0'; li++; }
    return li;
}

/* 全屏弹窗显示 MCP 文本，时长按行数：1行→1秒，2行→2秒，3行→3秒，滚动→5秒 */
static void mcp_fs_show(void) {
    char lines[16][MCP_FS_LINE_BYTES];
    uint8_t lc = mcp_fs_break_lines(mcp_fs_text, lines, 16);
    if (lc == 0) return;

    if (lc <= 4) {
        /* 无需滚动，直接全屏显示 lc 秒 */
        axk_ssd1306_clear_screen();
        for (uint8_t ln = 0; ln < lc; ln++) {
            axk_ssd1306_show_utf8_str(0, ln * 2, lines[ln]);
        }
        for (int w = 0; w < (lc + 1) * 20; w++) {
            osDelay(pdMS_TO_TICKS(50));
            if (btn_pressed(BTN_UP_PIN) || btn_pressed(BTN_DOWN_PIN)) return;
        }
    } else {
        /* 需要滚动，总共约 6 秒 */
        int8_t steps = lc - 4;
        int delay_ticks = 120 / (steps + 1);  /* 50ms 单位，steps+1 屏平分 6 秒 */
        for (int8_t s = 0; s <= steps; s++) {
            axk_ssd1306_clear_screen();
            for (uint8_t ln = 0; ln < 4 && (s + ln) < lc; ln++) {
                axk_ssd1306_show_utf8_str(0, ln * 2, lines[s + ln]);
            }
            for (int w = 0; w < delay_ticks; w++) {
                osDelay(pdMS_TO_TICKS(50));
                if (btn_pressed(BTN_UP_PIN) || btn_pressed(BTN_DOWN_PIN)) return;
            }
        }
    }
}
#endif /* CAT_ANIMATION_ENABLED */

static void draw_value_line(void) {
  char buf[32];
  switch (page_index) {
  case 0: break;
  case 1: {
    uint8_t ti=(uint8_t)g_temp; uint8_t td=((uint8_t)(g_temp*10))%10;
    snprintf(buf,sizeof(buf),"    T:%2d.%d C    ",ti,td);
    axk_ssd1306_show_utf8_str(0,2,buf); break;
  }
  case 2: {
    uint8_t hi=(uint8_t)g_hum; uint8_t hd=((uint8_t)(g_hum*10))%10;
    snprintf(buf,sizeof(buf),"    H:%2d.%d %%   ",hi,hd);
    axk_ssd1306_show_utf8_str(0,2,buf); break;
  }
  case 3: {
    TaskStatus_t taskArray[8];
    UBaseType_t taskCount=uxTaskGetSystemState(taskArray,8,NULL);
    if(taskCount>0){
      task_display_idx=(task_display_idx+1)%taskCount;
      TaskStatus_t *t=&taskArray[task_display_idx];
      const char *stateStr="未知";
      switch(t->eCurrentState){
      case eRunning: stateStr="运行中"; break;
      case eReady: stateStr="就绪"; break;
      case eBlocked: stateStr="阻塞"; break;
      case eSuspended: stateStr="挂起"; break;
      case eDeleted: stateStr="已删除"; break;
      default: break;
      }
      snprintf(buf,sizeof(buf),"  %-14s",t->pcTaskName); axk_ssd1306_show_utf8_str(0,2,buf);
      snprintf(buf,sizeof(buf),"  状态 %s     ",stateStr); axk_ssd1306_show_utf8_str(0,4,buf);
      snprintf(buf,sizeof(buf),"  优先%2u栈余%4u",
               (unsigned)t->uxCurrentPriority,(unsigned)t->usStackHighWaterMark);
      axk_ssd1306_show_utf8_str(0,6,buf);
    }
    break;
  }
  case 4: {
    const char *raw=(uart_data_buf&&uart_data_buf[0])?uart_data_buf:"(空)";
    int len=strlen(raw);
    if(len>14){
      scroll_offset+=scroll_dir*2;
      if(scroll_offset+14>=len){scroll_offset=len-14;scroll_dir=-1;}
      else if(scroll_offset<=0){scroll_offset=0;scroll_dir=1;}
    }else scroll_offset=0;
    snprintf(buf,sizeof(buf),"%.14s",raw+scroll_offset); axk_ssd1306_show_utf8_str(0,2,buf);
    break;
  }
  case 5: {
    snprintf(buf,sizeof(buf),"状态 %-14s",ai_event_text); axk_ssd1306_show_utf8_str(0,2,buf);
    snprintf(buf,sizeof(buf),"WiFi %-12s",ai_wifi_status); axk_ssd1306_show_utf8_str(0,4,buf);
    { /* MCP 文本单行滚动 */
      const char *mt=(mcp_text_buf[0])?mcp_text_buf:"(无文本)";
      int mlen=strlen(mt);
      if(mlen>14){
        mcp_scroll_off+=mcp_scroll_dir*2;
        if(mcp_scroll_off+14>=mlen){mcp_scroll_off=mlen-14;mcp_scroll_dir=-1;}
        else if(mcp_scroll_off<=0){mcp_scroll_off=0;mcp_scroll_dir=1;}
      }else mcp_scroll_off=0;
      snprintf(buf,sizeof(buf),"%.14s",mt+mcp_scroll_off);
    }
    axk_ssd1306_show_utf8_str(0,6,buf);
    break;
  }
  case 6: break;
  case 7: {
    /* 多工具时自动轮播显示 */
    uint8_t tool_count = 0;
    for (int i = 0; i < MCP_SERVER_TOOL_NUMBLE_MAX; i++) {
      if (mcp_tool_arry[i].name != NULL) tool_count++;
    }
    if (tool_count > 1) {
      task_display_idx = (task_display_idx + 1) % tool_count;
      emMCP_tool_t *t = &mcp_tool_arry[task_display_idx];
      snprintf(buf, sizeof(buf), "%d.%-14s", task_display_idx + 1, t->name);
      axk_ssd1306_show_utf8_str(0, 4, buf);
      snprintf(buf, sizeof(buf), "  %.14s", t->description ? t->description : "");
      axk_ssd1306_show_utf8_str(0, 6, buf);
    }
    break;
  }
  case 8: {
    int st = hxd039b2_learn_state();
    switch (st) {
    case HXD_LEARN_ACTIVE: {
      snprintf(buf, sizeof(buf), "  BUSY=%d 等待中", hxd039b2_busy_state());
      axk_ssd1306_show_utf8_str(0, 6, buf);
      break;
    }
    case HXD_LEARN_DONE:   axk_ssd1306_show_utf8_str(0, 6, "  按键重新学习"); break;
    case HXD_LEARN_TIMEOUT: axk_ssd1306_show_utf8_str(0, 6, "  超时, 按键重试"); break;
    default:               axk_ssd1306_show_utf8_str(0, 6, "  按键进入学习"); break;
    }
    break;
  }
  }
}

static void draw_page_full(void) {
  char buf[32];
  axk_ssd1306_clear_screen();
  switch (page_index) {
  case 0:
    axk_ssd1306_show_utf8_str(0,0,"# 系统      1/9");
    axk_ssd1306_show_utf8_str(0,2,"  XEMOWO");
    axk_ssd1306_show_utf8_str(0,4,"   安信可科技   ");
    axk_ssd1306_show_utf8_str(0,6,sht30_is_init?"  SHT3x | OK       ":"SHT3x | ERR      ");
    break;
  case 1:
    axk_ssd1306_show_utf8_str(0,0,"# 温度      2/9");
    { uint8_t ti=(uint8_t)g_temp; uint8_t td=((uint8_t)(g_temp*10))%10;
      snprintf(buf,sizeof(buf),"    T:%2d.%d C    ",ti,td); }
    axk_ssd1306_show_utf8_str(0,2,buf);
    axk_ssd1306_show_utf8_str(0,4,"----------------");
    axk_ssd1306_show_utf8_str(0,6,sht30_is_init?"  SHT3x | OK       ":"SHT3x | ERR      ");
    break;
  case 2:
    axk_ssd1306_show_utf8_str(0,0,"# 湿度      3/9");
    { uint8_t hi=(uint8_t)g_hum; uint8_t hd=((uint8_t)(g_hum*10))%10;
      snprintf(buf,sizeof(buf),"    H:%2d.%d %%   ",hi,hd); }
    axk_ssd1306_show_utf8_str(0,2,buf);
    axk_ssd1306_show_utf8_str(0,4,"----------------");
    axk_ssd1306_show_utf8_str(0,6,sht30_is_init?"  SHT3x | OK       ":"SHT3x | ERR      ");
    break;
  case 3: {
    TaskStatus_t taskArray[8];
    UBaseType_t taskCount=uxTaskGetSystemState(taskArray,8,NULL);
    if(taskCount==0)taskCount=1;
    if(task_display_idx>=taskCount)task_display_idx=0;
    TaskStatus_t *t=&taskArray[task_display_idx];
    const char *stateStr="未知";
    switch(t->eCurrentState){
    case eRunning: stateStr="运行中"; break;
    case eReady: stateStr="就绪"; break;
    case eBlocked: stateStr="阻塞"; break;
    case eSuspended: stateStr="挂起"; break;
    case eDeleted: stateStr="已删除"; break;
    default: break;
    }
    axk_ssd1306_show_utf8_str(0,0,"# 任务监控  4/9");
    snprintf(buf,sizeof(buf),"  %-14s",t->pcTaskName); axk_ssd1306_show_utf8_str(0,2,buf);
    snprintf(buf,sizeof(buf),"  状态 %s     ",stateStr); axk_ssd1306_show_utf8_str(0,4,buf);
    snprintf(buf,sizeof(buf),"  优先%2u栈余%4u",
             (unsigned)t->uxCurrentPriority,(unsigned)t->usStackHighWaterMark);
    axk_ssd1306_show_utf8_str(0,6,buf);
    break;
  }
  case 4: {
    const char *raw=(uart_data_buf&&uart_data_buf[0])?uart_data_buf:"(空)";
    scroll_offset=0;scroll_dir=1;
    axk_ssd1306_show_utf8_str(0,0,"# UART数据  5/9");
    snprintf(buf,sizeof(buf),"%.14s",raw); axk_ssd1306_show_utf8_str(0,2,buf);
    break;
  }
  case 5: {
    axk_ssd1306_show_utf8_str(0,0,"# AI模块    6/9");
    snprintf(buf,sizeof(buf),"状态 %-14s",ai_event_text); axk_ssd1306_show_utf8_str(0,2,buf);
    snprintf(buf,sizeof(buf),"WiFi %-12s",ai_wifi_status); axk_ssd1306_show_utf8_str(0,4,buf);
    snprintf(buf,sizeof(buf),"%.14s",mcp_text_buf[0]?mcp_text_buf:"(无文本)");
    axk_ssd1306_show_utf8_str(0,6,buf);
    break;
  }
#if CAT_ANIMATION_ENABLED
  case 6:
    axk_ssd1306_show_image_128x64((unsigned char *)SALARY_CAT_FRAMES[0]);
    break;
#else
  case 6:
    axk_ssd1306_show_utf8_str(0,0,"# 猫咪动画  7/9");
    axk_ssd1306_show_utf8_str(0,3,"  动画已禁用");
    axk_ssd1306_show_utf8_str(0,5,"  请按键切换");
    break;
#endif
  case 7: {
    /* 统计已注册工具数量 */
    uint8_t tool_count = 0;
    for (int i = 0; i < MCP_SERVER_TOOL_NUMBLE_MAX; i++) {
      if (mcp_tool_arry[i].name != NULL) tool_count++;
    }
    axk_ssd1306_show_utf8_str(0, 0, "# MCP工具   8/9");
    snprintf(buf, sizeof(buf), "已注册: %d 个", tool_count);
    axk_ssd1306_show_utf8_str(0, 2, buf);
    if (tool_count == 0) {
      axk_ssd1306_show_utf8_str(0, 4, "  (无工具)");
    } else {
      /* 显示第一个工具的名称和描述（多工具时自动轮播） */
      if (task_display_idx >= tool_count) task_display_idx = 0;
      emMCP_tool_t *t = &mcp_tool_arry[task_display_idx];
      snprintf(buf, sizeof(buf), "%d.%-14s", task_display_idx + 1, t->name);
      axk_ssd1306_show_utf8_str(0, 4, buf);
      /* 描述可能较长，截断到屏幕宽度（14个半角字符 ≈ 7个中文） */
      snprintf(buf, sizeof(buf), "  %.14s", t->description ? t->description : "");
      axk_ssd1306_show_utf8_str(0, 6, buf);
    }
    break;
  }
  case 8: {
    axk_ssd1306_show_utf8_str(0, 0, "# IR学习   9/9");
    uint8_t cg[2];
    hxd039b2_get_codeGrud(cg);
    int st = hxd039b2_learn_state();
    switch (st) {
    case HXD_LEARN_ACTIVE:
      axk_ssd1306_show_utf8_str(0, 2, "  学习中...");
      axk_ssd1306_show_utf8_str(0, 4, "  请按遥控器开关");
      break;
    case HXD_LEARN_DONE:
      snprintf(buf, sizeof(buf), "  码组: %02X %02X  ", cg[0], cg[1]);
      axk_ssd1306_show_utf8_str(0, 2, "  学习完成!");
      axk_ssd1306_show_utf8_str(0, 4, buf);
      break;
    case HXD_LEARN_TIMEOUT:
      axk_ssd1306_show_utf8_str(0, 2, "  学习超时!");
      axk_ssd1306_show_utf8_str(0, 4, "  没收到遥控信号");
      break;
    default:
      axk_ssd1306_show_utf8_str(0, 2, "  按BTN进入学习");
      snprintf(buf, sizeof(buf), "  当前: %02X %02X  ", cg[0], cg[1]);
      axk_ssd1306_show_utf8_str(0, 4, buf);
      break;
    }
    break;
  }
  }
}

static void smoothcolorTransition_callbark(color_t color, void *arg);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void sht3x_read_task(void *argument);
void ws2812_modeTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of sht3x_read */
  sht3x_readHandle = osThreadNew(sht3x_read_task, NULL, &sht3x_read_attributes);

  /* creation of ws2812_mode */
  ws2812_modeHandle = osThreadNew(ws2812_modeTask, NULL, &ws2812_mode_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  HAL_UARTEx_ReceiveToIdle_DMA(&huart2, (uint8_t *)rxBuffer, sizeof(rxBuffer));
  __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);
  /* USART1 中断接收 — 抓取 PC 发来的 STATUS 消息 */
  HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart1_rx_buf, sizeof(uart1_rx_buf));
  emMCP_Init(&emMCP);
  log_info("emMCP init done, MAX_TOOLS=%d", MCP_SERVER_TOOL_NUMBLE_MAX);

  /* 注册工具：用栈变量复用避免静态内存重复占用 (节省 5*432=2160B) */
  emMCP_tool_t t;

  memset(&t, 0, sizeof(t));
  t.name = "relay";
  t.description = "继电器控制工具，用于打开/关闭继电器(空调总电源)";
  t.inputSchema.properties[0].name = "state";
  t.inputSchema.properties[0].description = "on打开继电器 / off关闭继电器";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = relay_set_handler;
  t.checkRequestHandler = relay_check_handler;
  emMCP_AddToolToToolList(&t);

  /* 初始化红外模块 */
  hxd039b2_init();

  memset(&t, 0, sizeof(t));
  t.name = "temperature";
  t.description = "温度调节工具，用于设置空调目标温度";
  t.inputSchema.properties[0].name = "value";
  t.inputSchema.properties[0].description = "温度值，范围16到31度";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_NUMBER;
  t.setRequestHandler = ac_temperature_set_handler;
  t.checkRequestHandler = ac_temperature_check_handler;
  emMCP_AddToolToToolList(&t);

  memset(&t, 0, sizeof(t));
  t.name = "acmode";
  t.description = "模式调节工具，用于切换空调运行模式";
  t.inputSchema.properties[0].name = "value";
  t.inputSchema.properties[0].description = "运行模式：auto自动 cool制冷 dry除湿 fan送风 heat制热";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = ac_mode_set_handler;
  t.checkRequestHandler = ac_mode_check_handler;
  emMCP_AddToolToToolList(&t);

  memset(&t, 0, sizeof(t));
  t.name = "acpower";
  t.description = "空调电源控制工具，通过红外遥控开关空调";
  t.inputSchema.properties[0].name = "power";
  t.inputSchema.properties[0].description = "on打开空调 / off关闭空调";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = ac_power_set_handler;
  t.checkRequestHandler = ac_power_check_handler;
  emMCP_AddToolToToolList(&t);

  memset(&t, 0, sizeof(t));
  t.name = "acfan";
  t.description = "空调风速调节工具，用于设置空调风速";
  t.inputSchema.properties[0].name = "speed";
  t.inputSchema.properties[0].description = "auto自动 / low低 / mid中 / high高";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = ac_fan_set_handler;
  t.checkRequestHandler = ac_fan_check_handler;
  emMCP_AddToolToToolList(&t);

  memset(&t, 0, sizeof(t));
  t.name = "ledstrip";
  t.description = "WS2812灯带控制工具，RGB颜色(r/g/b:0-255)、可选mode(on/off)、brightness(0-100)";
  t.inputSchema.properties[0].name = "r";
  t.inputSchema.properties[0].description = "红色分量 0-255";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_NUMBER;
  t.inputSchema.properties[1].name = "g";
  t.inputSchema.properties[1].description = "绿色分量 0-255";
  t.inputSchema.properties[1].type = MCP_SERVER_TOOL_TYPE_NUMBER;
  t.inputSchema.properties[2].name = "b";
  t.inputSchema.properties[2].description = "蓝色分量 0-255";
  t.inputSchema.properties[2].type = MCP_SERVER_TOOL_TYPE_NUMBER;
  t.setRequestHandler = ledstrip_set_handler;
  t.checkRequestHandler = ledstrip_check_handler;
  emMCP_AddToolToToolList(&t);

  memset(&t, 0, sizeof(t));
  t.name = "serial_monitor";
  t.description = "串口STATUS监控工具，解析STATUS码控制灯带(0黄呼吸/1-2黄/3绿/4红)";
  t.inputSchema.properties[0].name = "action";
  t.inputSchema.properties[0].description = "open打开监控 / close关闭监控";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = serial_monitor_set_handler;
  t.checkRequestHandler = serial_monitor_check_handler;
  emMCP_AddToolToToolList(&t);

#ifndef EMMCP_MANUAL_TOOLS_ONLY
  emMCP_RegistrationTools();
#endif

  /* 用 emMCP 已验证的 msgType:MCP 格式，发送 7 工具 */
  {
    const char *mcp_tools_fmt =
      "mcp-tool "
      "{\"role\":\"MCU\",\"msgType\":\"MCP\",\"data\":"
      "{\"tools\":["
      "{\"name\":\"relay\",\"description\":\"继电器控制，打开/关闭继电器(空调总电源)\",\"inputSchema\":{\"properties\":{\"state\":{\"description\":\"on打开/off关闭\",\"type\":\"string\"}}}},"
      "{\"name\":\"temperature\",\"description\":\"温度调节\",\"inputSchema\":{\"properties\":{\"value\":{\"description\":\"目标温度16-31度\",\"type\":\"number\"}}}},"
      "{\"name\":\"acmode\",\"description\":\"模式调节\",\"inputSchema\":{\"properties\":{\"value\":{\"description\":\"auto/cool/dry/fan/heat\",\"type\":\"string\"}}}},"
      "{\"name\":\"acpower\",\"description\":\"红外空调电源，开关空调\",\"inputSchema\":{\"properties\":{\"power\":{\"description\":\"on打开/off关闭\",\"type\":\"string\"}}}},"
      "{\"name\":\"acfan\",\"description\":\"空调风速调节\",\"inputSchema\":{\"properties\":{\"speed\":{\"description\":\"auto/low/mid/high\",\"type\":\"string\"}}}},"
      "{\"name\":\"ledstrip\",\"description\":\"WS2812灯带开关、颜色和亮度控制\",\"inputSchema\":{\"properties\":{\"mode\":{\"description\":\"on打开/off关闭/set设置/query查询状态\",\"type\":\"string\"},\"r\":{\"description\":\"红色0-255(mode=set)\",\"type\":\"number\"},\"g\":{\"description\":\"绿色0-255\",\"type\":\"number\"},\"b\":{\"description\":\"蓝色0-255\",\"type\":\"number\"},\"brightness\":{\"description\":\"亮度0-100\",\"type\":\"number\"}}}},"
      "{\"name\":\"serial_monitor\",\"description\":\"串口STATUS监控，解析STATUS码控制灯带\",\"inputSchema\":{\"properties\":{\"action\":{\"description\":\"open打开监控/close关闭监控\",\"type\":\"string\"}}}}"
      "]}}";
    uartPortSendData((char *)mcp_tools_fmt, strlen(mcp_tools_fmt));
    uartPortSendData("\r\n", 2);
    log_info("mcp-tool sent");
  }

  for (;;) {
    while (rx_msg_idx < rx_msg_count) {
      uartPortRecvData(rx_msg_buf[rx_msg_idx], strlen(rx_msg_buf[rx_msg_idx]));
      rx_msg_idx++;
      emMCP_TickHandle(100);
    }
    emMCP_TickHandle(100);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_sht3x_read_task */
/**
* @brief Function implementing the sht3x_read thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_sht3x_read_task */
void sht3x_read_task(void *argument)
{
  /* USER CODE BEGIN sht3x_read_task */
  uint8_t res=axk_sht3x_init();
  if(res!=0){log_error("sht3x not driver");sht30_is_init=false;}
  else{sht30_is_init=true;log_info("sht3x init OK!");}
  axk_relay_init();

  double temperature=0.0,humidity=0.0;
  draw_page_full();

  for(;;){
#if CAT_ANIMATION_ENABLED
    /* MCP 全屏文本到达时自动切到第六页(猫咪) */
    if(mcp_fs_active && page_index != 6){
      page_index = 6;
      draw_page_full();
    }
#endif
#if CAT_ANIMATION_ENABLED
    if(page_index==6){
      /* 猫咪页：原地死循环播动画 */
      /* 设水平寻址模式 */
      axk_ssd1306_write_byte(0x20, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x00, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x21, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x00, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x7F, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x22, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x00, AXK_SSD1306_WRITE_CMD);
      axk_ssd1306_write_byte(0x07, AXK_SSD1306_WRITE_CMD);
      for(;;){
        if(mcp_fs_active) break;
        if(btn_pressed(BTN_UP_PIN)){
          page_index=(page_index+PAGE_COUNT-1)%PAGE_COUNT;
          axk_ssd1306_write_byte(0x20, AXK_SSD1306_WRITE_CMD);
          axk_ssd1306_write_byte(0x02, AXK_SSD1306_WRITE_CMD);
          draw_page_full(); break;
        }
        if(btn_pressed(BTN_DOWN_PIN)){
          page_index=(page_index+1)%PAGE_COUNT;
          axk_ssd1306_write_byte(0x20, AXK_SSD1306_WRITE_CMD);
          axk_ssd1306_write_byte(0x02, AXK_SSD1306_WRITE_CMD);
          draw_page_full(); break;
        }
        cat_frame=(cat_frame+1)%SALARY_CAT_FRAME_COUNT;
        bsp_spi_transmit_dma(SALARY_CAT_FRAMES[cat_frame], 1024);
        osDelay(pdMS_TO_TICKS(30));
      }
      /* MCP 全屏文本弹窗：中断猫咪动画，全屏显示文本后切到 AI模块 6/7 */
      if(mcp_fs_active){
        axk_ssd1306_write_byte(0x20, AXK_SSD1306_WRITE_CMD);
        axk_ssd1306_write_byte(0x02, AXK_SSD1306_WRITE_CMD);
        mcp_fs_show();
        mcp_fs_active = 0;
        page_index = 5;
        draw_page_full();
      }
    }
#else /* !CAT_ANIMATION_ENABLED */
    if(page_index==6){
      /* 猫咪动画已禁用，仅响应按键 */
      for(;;){
        osDelay(pdMS_TO_TICKS(100));
        if(btn_pressed(BTN_UP_PIN)){
          page_index=(page_index+PAGE_COUNT-1)%PAGE_COUNT;
          draw_page_full(); break;
        }
        if(btn_pressed(BTN_DOWN_PIN)){
          page_index=(page_index+1)%PAGE_COUNT;
          draw_page_full(); break;
        }
      }
    }
#endif /* CAT_ANIMATION_ENABLED */
    uint8_t page_changed=0;
    for(int i=0;i<20;i++){
      osDelay(pdMS_TO_TICKS(50));
      if(ai_event_updated&&(page_index==4||page_index==5)){ai_event_updated=0;break;}
      if(btn_pressed(BTN_UP_PIN)){page_index=(page_index+PAGE_COUNT-1)%PAGE_COUNT;page_changed=1;break;}
      if(btn_pressed(BTN_DOWN_PIN)){page_index=(page_index+1)%PAGE_COUNT;page_changed=1;break;}
    }
    if(sht30_is_init){
      res=axk_sht3x_read(0x2c06,&temperature,&humidity);
      if(res==0){g_temp=temperature;g_hum=humidity;
        /* log_info("sht30: %0.2d C,%0.2d %%",(uint8_t)temperature,(uint8_t)humidity); */ }
      else log_error("sht3x read error: %d",res);
    }
    if(page_changed){
      draw_page_full();
      if(page_index == 8) { hxd039b2_learn_reset(); hxd039b2_start_learn(); }
      else { hxd039b2_learn_reset(); }
    }
    else draw_value_line();
  }
  /* USER CODE END sht3x_read_task */
}

/* USER CODE BEGIN Header_ws2812_modeTask */
/**
* @brief Function implementing the ws2812_mode thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_ws2812_modeTask */
void ws2812_modeTask(void *argument)
{
  /* USER CODE BEGIN ws2812_modeTask */
  log_info("SSD1306: task start, SPI=%d", bsp_spi_get_state());
  delay_ms(50);
  axk_ssd1306_init();
  log_info("SSD1306: init done, SPI=%d", bsp_spi_get_state());
  axk_ssd1306_set_color_turn(0);
  axk_ssd1306_set_display_turn(0);
  axk_ssd1306_clear_screen();
  axk_ws2812_init(&ws2812);
  axk_radar_init();
  log_info("[RADAR] Rd-03L_V2 初始化完成, PA8 GPIO输入, 去抖阈值=%d次", AXK_RADAR_ABSENT_THRESHOLD);
  {
    float hue = 0.0f;
    float breath = 0.5f;
    int8_t breath_dir = 1;
    bool radar_off_applied = false;
    bool radar_prev_present = false;
    for(;;){
      /* ---- 雷达人体存在检测，每 ~100ms 检测一次 ---- */
      {
        static uint8_t radar_tick = 0;
        if (++radar_tick >= 7) {  /* 7 * 15ms ≈ 105ms */
          radar_tick = 0;
          bool radar_now = axk_radar_is_present();
          if (radar_now) {
            /* 有人：清除自动关灯标志，恢复彩虹呼吸 */
            if (!radar_prev_present) {
              log_info("[RADAR] 有人进入检测区域, PA8=HIGH");
            }
            if (radar_auto_off) {
              log_info("[RADAR] 人已返回，恢复彩虹呼吸");
              radar_auto_off = false;
              led_on = false;
            }
            axk_radar_reset_absent_count();
          } else {
            /* 无人：累加计数，达到阈值后自动关灯 */
            if (radar_prev_present) {
              log_info("[RADAR] 人已离开, PA8=LOW, 开始计时...");
            }
            if (axk_radar_check_absent() && !radar_auto_off && !led_on) {
              /* 确认人已离开且灯带处于彩虹模式 → 自动关灯 */
              log_info("[RADAR] 确认无人(>%d次), 自动关灯!", AXK_RADAR_ABSENT_THRESHOLD);
              radar_auto_off = true;
            }
          }
          radar_prev_present = radar_now;
        }
      }

      /* ---- STATUS 串口监控灯带控制 ---- */
      {
        static uint8_t  sm_last_code  = 0xFF;
        static float    sm_breath     = 0.5f;
        static int8_t   sm_breath_dir = 1;

        if (serial_monitor_enabled && status_updated) {
          status_updated = false;
          sm_last_code  = status_code;
          sm_breath     = 0.5f;
          sm_breath_dir = 1;
          radar_off_applied = false;
          log_info("[STATUS] 灯带响应 code=%d", sm_last_code);
        }

        if (serial_monitor_enabled && sm_last_code <= 4) {
          float br = led_brightness / 100.0f;
          switch (sm_last_code) {
            case 0: /* 黄色呼吸灯 — 改 0.004f 调频率 */
              sm_breath += sm_breath_dir * 0.010f;
              if (sm_breath >= 1.0f)  { sm_breath = 1.0f;  sm_breath_dir = -1; }
              if (sm_breath <= 0.05f) { sm_breath = 0.05f; sm_breath_dir = 1;  }
              axk_ws2812_set_all_pixels_color(255, 255, 0, sm_breath * br);
              break;
            case 1:
            case 2: /* 黄色常亮 */
              axk_ws2812_set_all_pixels_color(255, 255, 0, br);
              break;
            case 3: /* 绿色常亮 */
              axk_ws2812_set_all_pixels_color(0, 255, 0, br);
              break;
            case 4: /* 红色常亮 */
              axk_ws2812_set_all_pixels_color(255, 0, 0, br);
              break;
            default:
              break;
          }
          osDelay(pdMS_TO_TICKS(15));
          continue; /* STATUS 模式跳过原有灯带逻辑 */
        }
      }

      /* ---- 灯带效果 ---- */
      if (radar_auto_off) {
        /* 雷达自动关灯：熄灭灯带（仅执行一次） */
        if (!radar_off_applied) {
          axk_ws2812_set_all_pixels_color(0, 0, 0, 0.0f);
          radar_off_applied = true;
        }
      } else {
        radar_off_applied = false;
        if (!led_on) {
          /* 彩虹色相循环 */
          hue += 0.5f;
          if (hue >= 360.0f) hue = 0.0f;
          /* 呼吸亮度 */
          breath += breath_dir * 0.004f;
          if (breath >= 1.0f) { breath = 1.0f; breath_dir = -1; }
          if (breath <= 0.05f) { breath = 0.05f; breath_dir = 1; }
          hsv_color_t hsv = {hue, 1.0f, breath};
          color_t rgb = hsv_to_rgb(hsv);
          axk_ws2812_set_all_pixels_color(rgb.r, rgb.g, rgb.b, breath);
        }
      }
      osDelay(pdMS_TO_TICKS(15));
    }
  }
  /* USER CODE END ws2812_modeTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
  int uart_id = huart == &huart1 ? 1 : huart == &huart2 ? 2 : 3;

  /* 解析 STATUS:{code} — 适用于所有串口 */
  if (serial_monitor_enabled && Size > 0 && Size < 128) {
    if (huart == &huart1) {
      uart1_rx_buf[Size] = '\0';
      log_info("RAW[U1 S=%d]: %s", Size, uart1_rx_buf);
      if (strncmp((char *)uart1_rx_buf, "STATUS:", 7) == 0) {
        int code = atoi((char *)uart1_rx_buf + 7);
        if (code >= 0 && code <= 4 && (uint8_t)code != status_code) {
          status_code = (uint8_t)code;
          status_updated = true;
          log_info("[STATUS] code=%d", code);
        }
      }
    }
  }

  /* USART1 收到数据后重启接收 */
  if (huart == &huart1) {
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart1_rx_buf, sizeof(uart1_rx_buf));
    return;
  }

  if (huart == &huart2) {
    uint16_t sz = Size < RXBUFFSER_MAX_SIZE ? Size : RXBUFFSER_MAX_SIZE - 1;
    memcpy(rx_raw_buf, rxBuffer, sz);
    rx_raw_buf[sz] = '\0';
    rx_raw_size = sz;
    rx_raw_pending = 1;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart2, rxBuffer, RXBUFFSER_MAX_SIZE);
    __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);

    /* 拆包：用大括号深度匹配，正确处理 {"data":{...}} 嵌套 JSON */
    char *p = (char *)rx_raw_buf;
    rx_msg_count = 0;
    while (rx_msg_count < RX_MSG_MAX) {
      char *start = memchr(p, '{', sz - (p - (char *)rx_raw_buf));
      if (!start) break;
      int depth = 0;
      char *end = start;
      while (end < (char *)rx_raw_buf + sz) {
        if (*end == '{') depth++;
        else if (*end == '}') {
          depth--;
          if (depth == 0) break;
        }
        end++;
      }
      if (depth != 0) break;
      int len = end - start + 1;
      if (len >= RXBUFFSER_MAX_SIZE) len = RXBUFFSER_MAX_SIZE - 1;
      memcpy(rx_msg_buf[rx_msg_count], start, len);
      rx_msg_buf[rx_msg_count][len] = '\0';
      rx_msg_count++;
      p = end + 1;
    }
    /* 第一条消息走原有通道，剩余的由 defaultTask 排队处理 */
    if (rx_msg_count > 0) {
      uartPortRecvData(rx_msg_buf[0], strlen(rx_msg_buf[0]));
      rx_msg_idx = 1;
    }
  }
}

/* USER CODE END Application */

