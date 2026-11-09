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
#include "axk_ws2812.h"
#include "axk_radar.h"
#include "emMCP.h"
#include "hxd039b2.h"
#include <stdlib.h>
#include <string.h>
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
/* 第 1 页 AI 表情状态机。
 * emMCP 没有单独的“聆听/思考/讲话”事件，只能从现有事件推断：
 *   AI_WAKE                    -> 聆听
 *   AI_MCP_CMD                 -> 思考（AI 正在调用工具）
 *   AI_MCP_Text state=true     -> 讲话
 *   AI_SLEEP                   -> 休眠
 * 唤醒后一段时间没有讲话，也推断为思考（等待云端回复）。 */
typedef enum {
  AI_FACE_IDLE = 0,
  AI_FACE_LISTENING,
  AI_FACE_THINKING,
  AI_FACE_SPEAKING,
  AI_FACE_SLEEPING,
} ai_face_state_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 长回复需要更大的串口缓冲：一条 MCP Text 的 JSON 可能 >256 字节，
 * 原来 256 字节会在中途被 DMA 截断，后半句直接丢失。 */
#define RXBUFFSER_MAX_SIZE 512
#define RX_MSG_MAX        2
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
static uint8_t emoji_id_from_emotion(const char *emotion);
static uint8_t emoji_id_from_emoji(const char *emoji);
static void ai_face_page_draw(void);
static void ai_face_page_full(void);
static void ai_face_tick(void);

static uint8_t rxBuffer[RXBUFFSER_MAX_SIZE] = {0};
static char     rx_msg_buf[RX_MSG_MAX][RXBUFFSER_MAX_SIZE];
static volatile uint8_t rx_msg_count = 0;
static uint8_t  rx_msg_idx = 0;
static emMCP_t emMCP;
axk_ws2812_strip_t ws2812 = {.led_count = WS2812_MAX_NUM};
color_t RED = {0xff, 0x00, 0x00};
color_t GREEN = {0x00, 0xff, 0x00};
color_t BLUE = {0x00, 0x00, 0xff};
bool sht30_is_init = false;

#define BTN_UP_PIN   GPIO_PIN_4
#define BTN_DOWN_PIN GPIO_PIN_8
#define BTN_PORT     GPIOB
#define PAGE_COUNT   10

static volatile uint8_t page_index       = 0;
static uint8_t  task_display_idx = 0;
static double   g_temp           = 0.0;
static double   g_hum            = 0.0;
/* 雷达人体存在：只在状态变化时打印一次，3 次采样(150ms)去抖 */
static bool     radar_last_present = false;
static uint8_t  radar_debounce_cnt = 0;
extern emMCP_event_t emMCP_event;
extern char *uart_data_buf;
extern emMCP_tool_t mcp_tool_arry[MCP_SERVER_TOOL_NUMBLE_MAX];
static emMCP_event_t saved_ai_event = emMCP_EVENT_NONE;
static const char   *ai_wifi_status  = "未知";
/* 是否真正连上网络（只认 WiFi 事件；本地唤醒/启动不代表联网） */
static volatile bool ai_net_ok       = false;
static const char   *ai_event_text   = "无事件";
static volatile uint8_t ai_event_updated = 0;
static volatile bool mcp_tools_resend = false;
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

/* ---- 第 1 页 AI 表情/状态 ---- */
static volatile ai_face_state_t ai_face_state = AI_FACE_IDLE;
static volatile uint32_t ai_state_since      = 0;   /* 进入当前状态的 tick */
static volatile uint32_t ai_last_text_tick   = 0;   /* 最后一次 MCP Text 的 tick */
static volatile bool     ai_text_active      = false; /* AI 还在输出文本 */
static volatile bool     ai_listen_after_speak = false; /* 讲话结束后的聆听，不做“思考”超时 */
/* 模组把云端 stt（用户说话）和 tts（AI 回复）都转成同一种 MCP Text，
 * 没有 type/role 字段。这里按模组固件的固定发送顺序区分：
 *   每轮 stt -> llm -> tts，stt 是本轮第一条正文。
 * ai_saw_ai_text：模组侧是否已经建立过 emoji（只有模组刚启动时为 false，
 *   此时第一条 stt 不会转发）。初始给 true 是为了兼容“只复位 MCU、模组仍在跑”；
 *   收到模组的 AI Start（说明模组刚启动）才会清成 false。
 * ai_expect_user_text：刚唤醒，下一条正文就是用户说的话。 */
static volatile bool     ai_saw_ai_text      = true;
static volatile bool     ai_expect_user_text = false;
static volatile uint8_t  ai_emotion_id       = 7;   /* 7 = neutral */
static volatile uint8_t  ai_page_dirty       = 0;
static ai_face_state_t   ai_shown_state      = AI_FACE_IDLE; /* 屏幕当前画的状态 */
static char              ai_reply_buf[512]   = "";  /* 完整回复（分片可拼接） */
static char              ai_reply_draw[512]  = "";  /* 绘制用快照，避免临界区里刷屏 */
static volatile bool     ai_reply_user_echo  = false; /* 回复区里暂存的是用户识别结果 */
/* 回复按行折好后只存每行起始偏移，绘制时再取字符，省 RAM */
#define AI_TEXT_MAX_LINES 32
static uint16_t          ai_text_line_off[AI_TEXT_MAX_LINES];
static uint8_t           ai_text_line_cnt = 0;
static volatile uint8_t  ai_text_top      = 0;  /* 当前显示的第一行 */
static volatile uint32_t ai_text_scroll_tick = 0;

/* ---- AC MCP 工具回调 (电源/温度/模式/风速 合并为一个工具) ---- */
static void ac_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[AC SET] param: %s", js);
  cJSON_free(js);

  bool handled = false;

  cJSON *power_item = cJSON_GetObjectItem(root, "power");
  if (power_item != NULL && cJSON_IsString(power_item)) {
    const char *p = power_item->valuestring;
    if (strcmp(p, "on") == 0) {
      hxd039b2_ac_power(HXD_AC_POWER_ON);
      log_info("AC power -> ON (0x81)");
      handled = true;
    } else if (strcmp(p, "off") == 0) {
      hxd039b2_ac_power(HXD_AC_POWER_OFF);
      log_info("AC power -> OFF (0x80)");
      handled = true;
    } else {
      log_error("AC: unknown power '%s', use 'on' or 'off'", p);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
  }

  cJSON *temp_item = cJSON_GetObjectItem(root, "temperature");
  if (temp_item != NULL && cJSON_IsNumber(temp_item)) {
    int temp = temp_item->valueint;
    if (temp < 16 || temp > 31) {
      log_error("AC: temperature %d out of range 16-31", temp);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
    hxd039b2_ac_temperature((uint8_t)temp);
    log_info("AC temperature -> %d C", temp);
    handled = true;
  }

  cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
  if (mode_item != NULL && cJSON_IsString(mode_item)) {
    const char *mode_str = mode_item->valuestring;
    hxd_ac_mode_t mode;
    if (strcmp(mode_str, "auto") == 0)      mode = HXD_AC_MODE_AUTO;
    else if (strcmp(mode_str, "cool") == 0) mode = HXD_AC_MODE_COOL;
    else if (strcmp(mode_str, "dry") == 0)  mode = HXD_AC_MODE_DRY;
    else if (strcmp(mode_str, "fan") == 0)  mode = HXD_AC_MODE_FAN;
    else if (strcmp(mode_str, "heat") == 0) mode = HXD_AC_MODE_HEAT;
    else {
      log_error("AC: unknown mode '%s', use auto/cool/dry/fan/heat", mode_str);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
    hxd039b2_ac_mode(mode);
    log_info("AC mode -> %s", mode_str);
    handled = true;
  }

  cJSON *fan_item = cJSON_GetObjectItem(root, "fan");
  if (fan_item != NULL && cJSON_IsString(fan_item)) {
    const char *s = fan_item->valuestring;
    hxd_ac_fan_t fan;
    if (strcmp(s, "auto") == 0)      fan = HXD_AC_FAN_AUTO;
    else if (strcmp(s, "low") == 0)  fan = HXD_AC_FAN_LOW;
    else if (strcmp(s, "mid") == 0)  fan = HXD_AC_FAN_MID;
    else if (strcmp(s, "high") == 0) fan = HXD_AC_FAN_HIGH;
    else {
      log_error("AC: unknown fan '%s', use auto/low/mid/high", s);
      emMCP_ResponseValue(emMCP_CTRL_ERROR);
      return;
    }
    hxd039b2_ac_fan(fan);
    log_info("AC fan -> %s", s);
    handled = true;
  }

  if (!handled) {
    log_error("AC: no valid parameter, use power/temperature/mode/fan");
    emMCP_ResponseValue(emMCP_CTRL_ERROR);
    return;
  }
  emMCP_ResponseValue(emMCP_CTRL_OK);
}
static void ac_check_handler(void *arg) {
  (void)arg;
  emMCP_ResponseValue(emMCP_CTRL_OK);
}

/* ---- 温湿度查询 MCP 工具回调 (读取 SHT3x 缓存值) ---- */
static void temp_humidity_response(void) {
  if (!sht30_is_init) {
    emMCP_ResponseValue("{\"error\":\"sht3x not initialized\"}");
    return;
  }
  /* newlib-nano 未启用浮点 printf，用整数拼出一位小数 */
  int t_neg = (g_temp < 0.0);
  int t10 = (int)((t_neg ? -g_temp : g_temp) * 10.0 + 0.5);
  int h10 = (int)(g_hum * 10.0 + 0.5);
  char buf[64];
  snprintf(buf, sizeof(buf),
    "temperature=%s%d.%dC,humidity=%d.%d%%",
    t_neg ? "-" : "", t10 / 10, t10 % 10, h10 / 10, h10 % 10);
  log_info("[TempHum] -> %s", buf);
  emMCP_ResponseValue(buf);
}
static void temp_humidity_set_handler(void *arg) {
  (void)arg;
  temp_humidity_response();
}
static void temp_humidity_check_handler(void *arg) {
  (void)arg;
  temp_humidity_response();
}

/* ---- LEDStrip MCP 工具回调 (WS2812 灯带控制) ---- */
/* 暖黄色 (RGB) —— "开灯"默认颜色 */
#define LED_WARM_R 255
#define LED_WARM_G 180
#define LED_WARM_B 50

static uint8_t  led_r = LED_WARM_R, led_g = LED_WARM_G, led_b = LED_WARM_B;
static uint8_t  led_brightness = 50;
static volatile bool led_on = false;
static char     led_mode[16] = "off";

static void ledstrip_set_handler(void *arg) {
  cJSON *root = (cJSON *)arg;
  char *js = cJSON_PrintUnformatted(root);
  log_info("[LEDStrip SET] param: %s", js);
  cJSON_free(js);

  cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
  if (mode_item && cJSON_IsString(mode_item)) {
    const char *m = mode_item->valuestring;
    if (strcmp(m, "on") == 0) {
      /* 开灯：默认暖黄色 */
      cJSON *br = cJSON_GetObjectItem(root, "brightness");
      if (br && cJSON_IsNumber(br)) led_brightness = (uint8_t)br->valueint;
      led_r = LED_WARM_R;
      led_g = LED_WARM_G;
      led_b = LED_WARM_B;
      led_on = true;
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

/* 从 MCP Text 的 text 字段里提取真正要显示的文字。
 * 有些模组会把协议原文也塞进 text 字段，例如：
 *     MCP_Text: {"state":false}
 *     MCP_Text: {"state":true,"text":"你好"}
 *     {"state":false}
 * 这类内容不能上屏：带 text 字段就只取 text，纯状态帧返回 NULL 直接丢弃。
 * 返回 NULL 表示“没有可显示的文字”；
 * 返回值可能指向 src 本身，也可能指向新解析出来的 *inner（调用方用完要
 * cJSON_Delete(*inner)，并且要在删除之后再丢弃返回值）。 */
static const char *ai_text_plain(const char *src, cJSON **inner) {
  const char *p = src;
  bool protocol = false;

  *inner = NULL;
  if (!src || !src[0]) return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

  if (strncmp(p, "MCP_Text:", 9) == 0)      { p += 9; protocol = true; }
  else if (strncmp(p, "MCP Text:", 9) == 0) { p += 9; protocol = true; }
  while (*p == ' ' || *p == '\t') p++;

  if (*p == '{') {
    cJSON *j = cJSON_Parse(p);
    if (j) {
      cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "text");
      if (cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
        *inner = j;                     /* 调用方负责释放，返回值指向里面的 text */
        return t->valuestring;
      }
      if (protocol || cJSON_GetObjectItemCaseSensitive(j, "state") != NULL) {
        cJSON_Delete(j);
        return NULL;                    /* 只有状态的协议帧：不显示 */
      }
      cJSON_Delete(j);
      return src;                       /* 普通 JSON 文本：原样显示 */
    }
    return protocol ? NULL : src;       /* 残缺协议帧丢弃，普通文本照显 */
  }
  if (protocol) return p;               /* "MCP_Text: 普通文字"：去掉前缀 */
  return src;
}

/* 长回复拼接。AI 模块可能一条条发分片，也可能每句发累积全文：
 * - 新文本以旧文本开头 -> 累积式，直接覆盖
 * - 旧文本以新文本开头 -> 重复的旧前缀，忽略
 * - 其它               -> 分片式，追加到末尾
 * 新的一轮（缓冲为空）会把翻页位置复位到第一行。 */
static void ai_text_merge(const char *txt) {
  size_t oldlen, newlen;

  if (!txt || !txt[0]) return;
  oldlen = strlen(ai_reply_buf);
  newlen = strlen(txt);

  if (oldlen == 0) {
    strncpy(ai_reply_buf, txt, sizeof(ai_reply_buf) - 1);
    ai_reply_buf[sizeof(ai_reply_buf) - 1] = '\0';
    ai_text_top = 0;
    ai_text_scroll_tick = xTaskGetTickCount();
    return;
  }
  if (newlen >= oldlen && strncmp(txt, ai_reply_buf, oldlen) == 0) {
    strncpy(ai_reply_buf, txt, sizeof(ai_reply_buf) - 1);
    ai_reply_buf[sizeof(ai_reply_buf) - 1] = '\0';
    return;
  }
  if (oldlen >= newlen && strncmp(ai_reply_buf, txt, newlen) == 0)
    return;
  if (oldlen + newlen >= sizeof(ai_reply_buf))
    return;                         /* 缓冲满，丢弃放不下的分片 */
  memcpy(ai_reply_buf + oldlen, txt, newlen + 1);
}

void emMCP_EventCallback(emMCP_event_t event, mcp_server_tool_type_t type, void *param) {
  (void)type;
  /* 先把语音模组发来的整帧 JSON 原样打到日志串口，方便核对模块到底发了什么。
   * 不经过 log_printf 的 256 字节格式化缓冲，长帧不会被截断。 */
  if (uart_data_buf && uart_data_buf[0]) {
    log_raw(">>> RAW: ");
    log_raw(uart_data_buf);
    log_raw("\r\n");
  }
  if (event == emMCP_EVENT_NONE) return;
  /* 瞬态命令响应不覆盖 AI 状态 */
  if (event == emMCP_EVENT_CMD_ERROR) {
    if (uart_data_buf) log_error("[MCP] AI ERROR: %s", uart_data_buf);
    if (uart_data_buf && strstr(uart_data_buf, "Tools NULL")) {
      log_info("[MCP] AI 报告工具为空，重新注册");
      mcp_tools_resend = true;
    }
    return;
  }
  if (event == emMCP_EVENT_CMD_OK) return;
  /* 提取 JSON 中 data 字段内容，用于页面 6 底行滚动显示 */
  if (uart_data_buf && uart_data_buf[0]) {
    cJSON *j = cJSON_Parse(uart_data_buf);
    if (j) {
      cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
      if (d && cJSON_IsString(d) && d->valuestring)
        snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", d->valuestring);
      else {
        /* MCP 类消息用 param；MCP Text 的正文在下面单独提取，
         * 避免 {"state":false} 这种结束帧把文本覆盖掉 */
        char *p = (char *)param;
        if (p && p[0] && event != emMCP_EVENT_AI_MCP_Text)
          snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", p);
      }
      cJSON_Delete(j);
    }
    mcp_scroll_off = 0;
    mcp_scroll_dir = 1;
  }
  saved_ai_event = event;
  /* 根据事件类型 + 原始字符串确定准确的显示文字 */
  switch (event) {
  case emMCP_EVENT_AI_START:
    ai_event_text="AI启动";
    mcp_tools_resend = true;
    taskENTER_CRITICAL();
    ai_reply_buf[0] = '\0';
    ai_reply_user_echo = false;
    taskEXIT_CRITICAL();
    /* 先更新时间戳，再改状态，避免显示任务在中间抢占时误判超时 */
    ai_last_text_tick = xTaskGetTickCount();
    ai_state_since = ai_last_text_tick;
    ai_text_active = false;
    ai_listen_after_speak = false;
    /* 模组重启后 emoji 缓冲清空，第一条用户语音不会被转发 */
    ai_saw_ai_text = false;
    ai_expect_user_text = false;
    ai_face_state = AI_FACE_IDLE;
    ai_page_dirty = 1;
    break;
  case emMCP_EVENT_AI_WIFI_CONNNECT:
    if (uart_data_buf && strstr(uart_data_buf, "1.WiFi connect OK"))
      ai_event_text="WiFi已连";
    else ai_event_text="WiFi连接中";
    break;
  case emMCP_EVENT_AI_WIFI_CONNECTED:  ai_event_text="WiFi已连"; break;
  case emMCP_EVENT_AI_WIFI_GOT_IP:     ai_event_text="已获IP"; break;
  case emMCP_EVENT_AI_WIFI_DISCONNECT: ai_event_text="WiFi断开"; break;
  case emMCP_EVENT_AI_WAKE:
    ai_event_text="已唤醒";
    mcp_tools_resend = true;
    taskENTER_CRITICAL();
    ai_reply_buf[0] = '\0';
    ai_reply_user_echo = false;
    taskEXIT_CRITICAL();
    /* 先更新时间戳，再改状态，避免显示任务在中间抢占时误判超时 */
    ai_last_text_tick = xTaskGetTickCount();
    ai_state_since = ai_last_text_tick;
    ai_text_active = false;
    ai_listen_after_speak = false;
    ai_expect_user_text = true;   /* 唤醒后第一条正文是用户识别结果 */
    ai_face_state = AI_FACE_LISTENING;
    ai_page_dirty = 1;
    break;
  case emMCP_EVENT_AI_SLEEP:
    ai_event_text="已休眠";
    taskENTER_CRITICAL();
    ai_reply_buf[0] = '\0';
    ai_reply_user_echo = false;
    taskEXIT_CRITICAL();
    /* 先更新时间戳，再改状态，避免显示任务在中间抢占时误判超时 */
    ai_last_text_tick = xTaskGetTickCount();
    ai_state_since = ai_last_text_tick;
    ai_text_active = false;
    ai_listen_after_speak = false;
    ai_face_state = AI_FACE_SLEEPING;
    ai_page_dirty = 1;
    break;
  case emMCP_EVENT_AI_NETCFG:          ai_event_text="配网中"; break;
  case emMCP_EVENT_AI_NETERR:
    ai_event_text="网络错";
    if (ai_face_state == AI_FACE_THINKING || ai_face_state == AI_FACE_LISTENING) {
      ai_state_since = xTaskGetTickCount();
      ai_face_state = AI_FACE_IDLE;
      ai_page_dirty = 1;
    }
    break;
  case emMCP_EVENT_AI_OTAUPDATE:       ai_event_text="OTA中"; break;
  case emMCP_EVENT_AI_OTAOK:           ai_event_text="OTA完成"; break;
  case emMCP_EVENT_AI_OTAERR:          ai_event_text="OTA失败"; break;
  case emMCP_EVENT_CMD_OK:             ai_event_text="命令OK"; break;
  case emMCP_EVENT_CMD_ERROR:          ai_event_text="命令错误"; break;
  case emMCP_EVENT_AI_MCP_CMD: {
    ai_event_text="MCP命令";
    log_info(">>> MCP_CMD: %s", (char *)param);
    /* 能下发工具调用说明云端链路是通的（MCU 单独复位后也能恢复联网状态） */
    if (!ai_net_ok) { ai_net_ok = true; ai_page_dirty = 1; }
    /* AI 正在调用工具 -> 思考中；如果已经在讲话，不打断讲话表情 */
    if (ai_face_state != AI_FACE_SPEAKING) {
      ai_state_since = xTaskGetTickCount();
      ai_face_state = AI_FACE_THINKING;
      ai_page_dirty = 1;
    }
    break;
  }
  case emMCP_EVENT_AI_MCP_Text: {
    ai_event_text="MCP文本";
    if (param) {
      cJSON *j = cJSON_Parse((char *)param);
      if (j) {
        cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "state");
        cJSON *t  = cJSON_GetObjectItemCaseSensitive(j, "text");
        cJSON *em = cJSON_GetObjectItemCaseSensitive(j, "emotion");
        cJSON *ej = cJSON_GetObjectItemCaseSensitive(j, "emoji");
        cJSON *plain_json = NULL;
        const char *plain = NULL;
        bool has_text = (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0]);
        bool speaking;
        if (has_text)
          plain = ai_text_plain(t->valuestring, &plain_json);
        bool has_plain = (plain && plain[0]);

        if (st && cJSON_IsBool(st))        speaking = cJSON_IsTrue(st);
        else if (st && cJSON_IsNumber(st)) speaking = (st->valueint != 0);
        else                               speaking = has_plain;

        if (speaking) {
          uint32_t now = xTaskGetTickCount();
          /* 上一轮已经结束且隔了 6 秒以上，认为是新一轮回复 */
          bool new_turn = !ai_text_active &&
                          (uint32_t)(now - ai_last_text_tick) > pdMS_TO_TICKS(6000);
          bool is_user = false;

          /* 模组把云端 stt（用户）和 tts（AI）都转成 MCP Text，没有类型字段。
           * 但每轮顺序固定：stt -> llm -> tts，stt 是本轮第一条正文。
           * 注意模组刚启动时 emoji 还没建立，第一条 stt 不会转发，
           * 所以模组刚启动后的第一帧正文一定是 AI 的 tts
           * （只复位 MCU 时模组一直在跑，不受这条影响）。 */
          if (has_plain) {
            if (!ai_saw_ai_text) {
              ai_saw_ai_text = true;
              ai_expect_user_text = false;
            } else if (ai_expect_user_text) {
              is_user = true;
              ai_expect_user_text = false;
            } else if (new_turn) {
              is_user = true;
            }
          }

          ai_last_text_tick = now;
          ai_state_since = now;
          ai_text_active = true;
          /* 有真实文本说明云端链路正常 */
          ai_net_ok = true;
          ai_page_dirty = 1;

          if (is_user) {
            /* 用户识别结果：串口单独标出。回复区先暂存这句，
             * AI 正文一到就整体替换，避免问答粘在一起。 */
            log_info(">>> 你说: %s", plain);
            ai_face_state = AI_FACE_THINKING;   /* 用户说完，等 AI 回答 */
            taskENTER_CRITICAL();
            ai_reply_buf[0] = '\0';
            ai_text_merge(plain);
            ai_reply_user_echo = true;
            taskEXIT_CRITICAL();
            snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", plain);
          } else {
            if (has_plain) log_info(">>> AI: %s", plain);
            ai_face_state = AI_FACE_SPEAKING;
            if (has_plain) {
              taskENTER_CRITICAL();
              if (ai_reply_user_echo || new_turn) {
                ai_reply_buf[0] = '\0';
                ai_reply_user_echo = false;
              }
              ai_text_merge(plain);
              taskEXIT_CRITICAL();
              snprintf(mcp_text_buf, sizeof(mcp_text_buf), "%s", plain);
#if CAT_ANIMATION_ENABLED
              snprintf(mcp_fs_text, sizeof(mcp_fs_text), "%s", plain);
              mcp_fs_active = 1;
#endif
            }
          }
        } else {
          ai_last_text_tick = xTaskGetTickCount();
          ai_text_active = false;
        }

        if (em && cJSON_IsString(em) && em->valuestring && em->valuestring[0])
          ai_emotion_id = emoji_id_from_emotion(em->valuestring);
        else if (ej && cJSON_IsString(ej) && ej->valuestring && ej->valuestring[0])
          ai_emotion_id = emoji_id_from_emoji(ej->valuestring);

        cJSON_Delete(plain_json);   /* plain 已经用完，可以释放内层 JSON */
        cJSON_Delete(j);
      }
    }
    break;
  }
  case emMCP_EVENT_AI_MCP_CHECK:
    ai_event_text="MCP检查";
    log_info(">>> MCP_CHECK: %s", (char *)param);
    break;
  default: break;
  }
  /* WiFi 状态跟踪：只认真实的 WiFi 事件。
   * WakeUP 只是本地唤醒词触发，没联网也会发生，不能当作已连接。 */
  {
    bool net = ai_net_ok;
    switch (event) {
    case emMCP_EVENT_AI_WIFI_CONNNECT:
      if (uart_data_buf && strstr(uart_data_buf, "1.WiFi connect OK")) {
        ai_wifi_status = "已连接"; net = true;
      } else {
        ai_wifi_status = "连接中"; net = false;
      }
      break;
    case emMCP_EVENT_AI_WIFI_CONNECTED:  ai_wifi_status="已连接"; net=true;  break;
    case emMCP_EVENT_AI_WIFI_GOT_IP:     ai_wifi_status="已获IP"; net=true;  break;
    case emMCP_EVENT_AI_WIFI_DISCONNECT: ai_wifi_status="已断开"; net=false; break;
    case emMCP_EVENT_AI_NETCFG:          ai_wifi_status="配网中"; net=false; break;
    case emMCP_EVENT_AI_NETERR:          ai_wifi_status="网络错"; net=false; break;
    default: break;
    }
    if (net != ai_net_ok) {
      ai_net_ok = net;
      ai_page_dirty = 1;   /* 联网状态变了，第 1 页立即重画 */
    }
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

/* ==================================================================
 * AI 表情演示页（第 10 页）
 * 用程序画 72x32 单色表情脸，自动轮播，方便评估效果。
 * 注意：这里画的是“表情图标”，不是直接渲染 emoji 字符。
 * ================================================================== */
#define EMOJI_W       72
#define EMOJI_H       32
#define EMOJI_PAGES   (EMOJI_H / 8)
#define EMOJI_X       ((128 - EMOJI_W) / 2)
#define EMOJI_PAGE    2

/* 第 1 页布局：整屏圆角框；左上大脸，右上小状态图标；下半屏两行文字。
 * 文字区 y=31..62，左右各留 1px 给边框；两行 16px 字刚好不压底部边框。 */
#define AI_FACE_X     2
#define AI_ICON_X     102
#define AI_ICON_Y     4
#define AI_ICON_W     24

#define AI_TEXT_X     1
#define AI_TEXT_Y0    31
#define AI_TEXT_Y1    47
#define AI_TEXT_MAXW  126   /* x=1..126，右边框在 x=127 */
#define AI_TEXT_SCROLL_MS 2500

static uint8_t emoji_fb[EMOJI_PAGES * EMOJI_W];
static uint8_t emoji_demo_idx = 0;

static const char *const emoji_labels[] = {
  "happy 开心",
  "sad 难过",
  "angry 生气",
  "surprised 惊讶",
  "cool 耍酷",
  "wink 眨眼",
  "love 爱心",
  "neutral 平静",
};
#define EMOJI_COUNT (sizeof(emoji_labels) / sizeof(emoji_labels[0]))

static void emoji_clear(void) {
  memset(emoji_fb, 0, sizeof(emoji_fb));
}

static void emoji_px(int x, int y, int on) {
  if ((unsigned)x >= EMOJI_W || (unsigned)y >= EMOJI_H) return;
  uint8_t *p = &emoji_fb[(y >> 3) * EMOJI_W + x];
  if (on) *p |= (uint8_t)(1u << (y & 7));
  else    *p &= (uint8_t)~(1u << (y & 7));
}

static void emoji_hline(int x0, int x1, int y, int on) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) emoji_px(x, y, on);
}

static void emoji_rect(int x0, int y0, int x1, int y1, int on) {
  for (int y = y0; y <= y1; y++) emoji_hline(x0, x1, y, on);
}

static void emoji_line(int x0, int y0, int x1, int y1, int on) {
  int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
  int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    emoji_px(x0, y0, on);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void emoji_fill_circle(int cx, int cy, int r, int on) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
    emoji_hline(cx - dx, cx + dx, cy + dy, on);
  }
}

/* U 形微笑：白线、线宽 2，half 为半宽、depth 为弧度深度 */
static void emoji_smile(int cx, int cy, int half, int depth) {
  for (int x = cx - half; x <= cx + half; x++) {
    int dx = x - cx;
    int y = cy + depth - (depth * dx * dx) / (half * half);
    emoji_px(x, y, 1);
    emoji_px(x, y + 1, 1);
  }
}

/* ∩ 形皱眉：白线、线宽 2 */
static void emoji_frown(int cx, int cy, int half, int depth) {
  for (int x = cx - half; x <= cx + half; x++) {
    int dx = x - cx;
    int y = cy + (depth * dx * dx) / (half * half);
    emoji_px(x, y, 1);
    emoji_px(x, y + 1, 1);
  }
}

/* 圆嘴：白环（r>1）或小白点（r=1） */
static void emoji_mouth_o(int cx, int cy, int r) {
  emoji_fill_circle(cx, cy, r, 1);
  if (r > 1) emoji_fill_circle(cx, cy, r - 1, 0);
}

/* 大眼：白色眼白 + 小黑瞳，pdx/pdy 偏移表示视线方向 */
static void emoji_eye(int cx, int cy, int pdx, int pdy) {
  emoji_fill_circle(cx, cy, 8, 1);
  emoji_fill_circle(cx + pdx, cy + pdy, 3, 0);
}

/* 开心眯眼 ^ ^ */
static void emoji_eye_happy(int cx, int cy) {
  emoji_line(cx - 8, cy + 5, cx, cy - 4, 1);
  emoji_line(cx, cy - 4, cx + 8, cy + 5, 1);
  emoji_line(cx - 8, cy + 6, cx, cy - 3, 1);
  emoji_line(cx, cy - 3, cx + 8, cy + 6, 1);
}

/* 睡觉闭眼 ‿ ‿ */
static void emoji_eye_sleep(int cx, int cy) {
  emoji_line(cx - 8, cy - 4, cx, cy + 4, 1);
  emoji_line(cx, cy + 4, cx + 8, cy - 4, 1);
}

/* 墨镜 */
static void emoji_eye_sunglass(int cx, int cy) {
  emoji_rect(cx - 9, cy - 6, cx + 9, cy + 6, 1);
  emoji_rect(cx - 8, cy - 5, cx + 8, cy + 5, 0);
  emoji_px(cx - 6, cy + 2, 1);
  emoji_px(cx - 5, cy + 1, 1);
}

/* 爱心眼（约 17x17） */
static void emoji_heart(int cx, int cy) {
  emoji_fill_circle(cx - 4, cy - 4, 4, 1);
  emoji_fill_circle(cx + 4, cy - 4, 4, 1);
  for (int y = 0; y <= 8; y++) {
    int dx = 8 - y;
    emoji_hline(cx - dx, cx + dx, cy + y, 1);
  }
}

/* 只画眼睛和嘴巴，不再画白色圆脸 */
static void emoji_draw_face(uint8_t id) {
  emoji_clear();

  switch (id) {
  case 0: /* happy */
    emoji_eye_happy(24, 14);
    emoji_eye_happy(48, 14);
    emoji_smile(36, 23, 11, 3);
    break;

  case 1: /* sad */
    emoji_eye(24, 14, 0, 3);
    emoji_eye(48, 14, 0, 3);
    emoji_line(5, 8, 13, 4, 1);      /* 八字眉 */
    emoji_line(67, 8, 59, 4, 1);
    emoji_frown(36, 22, 9, 3);
    break;

  case 2: /* angry */
    emoji_eye(24, 14, 0, 2);
    emoji_eye(48, 14, 0, 2);
    emoji_line(4, 3, 16, 8, 1);      /* 怒眉 */
    emoji_line(68, 3, 56, 8, 1);
    emoji_frown(36, 23, 8, 2);
    break;

  case 3: /* surprised */
    emoji_eye(24, 14, 0, 0);
    emoji_eye(48, 14, 0, 0);
    emoji_mouth_o(36, 23, 5);        /* 大圆嘴（下沿 y=28，给分隔线留一行空隙） */
    break;

  case 4: /* cool */
    emoji_eye_sunglass(24, 14);
    emoji_eye_sunglass(48, 14);
    emoji_line(33, 13, 39, 13, 1);   /* 墨镜鼻梁 */
    emoji_smile(39, 24, 8, 2);       /* 歪嘴笑 */
    break;

  case 5: /* wink */
    emoji_eye(24, 14, 0, 1);
    emoji_eye_happy(48, 14);
    emoji_smile(36, 23, 11, 3);
    break;

  case 6: /* love */
    emoji_heart(24, 14);
    emoji_heart(48, 14);
    emoji_smile(36, 23, 11, 3);
    break;

  case 7: /* neutral */
  default:
    emoji_eye(24, 14, 0, 2);
    emoji_eye(48, 14, 0, 2);
    emoji_smile(36, 25, 8, 2);
    break;
  }
}

/* 把缓冲前 cols 列写到 OLED（page 寻址模式），第 10 页表情演示用 */
static void emoji_blit_columns(uint8_t x, uint8_t page, uint8_t cols) {
  for (uint8_t p = 0; p < EMOJI_PAGES; p++) {
    axk_ssd1306_set_address(x, page + p);
    for (uint8_t c = 0; c < cols; c++)
      axk_ssd1306_write_byte(emoji_fb[p * EMOJI_W + c], AXK_SSD1306_WRITE_DATA);
  }
}

static void emoji_blit(uint8_t x, uint8_t page) {
  emoji_blit_columns(x, page, EMOJI_W);
}

/* ================= 第 1 页全屏合成 =================
 * 驱动里已经有一块 128x64 的显存 axk_oled_gram，这里直接复用它：
 * 先把圆角框、表情、图标、文字全部画进显存，最后 refresh 一次。
 * 这样文字可以按任意像素行摆放（不会被 page 对齐限制），也能做局部擦除。 */
extern unsigned char axk_oled_gram[128][8];

static void ui_clear(void) {
  memset(axk_oled_gram, 0, 128 * 8);
}

static void ui_px(int x, int y, int on) {
  if ((unsigned)x >= 128 || (unsigned)y >= 64) return;
  uint8_t *p = &axk_oled_gram[x][y >> 3];
  if (on) *p |= (uint8_t)(1u << (y & 7));
  else    *p &= (uint8_t)~(1u << (y & 7));
}

static void ui_hline(int x0, int x1, int y, int on) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) ui_px(x, y, on);
}

static void ui_vline(int x, int y0, int y1, int on) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) ui_px(x, y, on);
}

static void ui_clear_rect(int x0, int y0, int x1, int y1) {
  for (int y = y0; y <= y1; y++) ui_hline(x0, x1, y, 0);
}

/* 四分之一圆弧：sx/sy 决定象限 */
static void ui_arc_quad(int cx, int cy, int r, int sx, int sy) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    ui_px(cx + sx * x, cy + sy * y, 1);
    ui_px(cx + sx * y, cy + sy * x, 1);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

/* 整屏 1px 圆角框，半径 6；y=30 横线把上方表情区和下方回复区分开 */
static void ui_frame(void) {
  const int r = 6;
  ui_hline(r, 127 - r, 0, 1);
  ui_hline(r, 127 - r, 63, 1);
  ui_hline(1, 126, 30, 1);          /* 表情/文字分隔线 */
  ui_vline(0, r, 63 - r, 1);
  ui_vline(127, r, 63 - r, 1);
  ui_arc_quad(r, r, r, -1, -1);
  ui_arc_quad(127 - r, r, r, 1, -1);
  ui_arc_quad(r, 63 - r, r, -1, 1);
  ui_arc_quad(127 - r, 63 - r, r, 1, 1);
}

/* 把 emoji_fb 里 cols x rows 的区域 OR 进显存（只置位，不擦除） */
static void emoji_blit_gram(uint8_t x, uint8_t y, uint8_t cols, uint8_t rows) {
  for (uint8_t p = 0; p * 8 < rows; p++) {
    for (uint8_t c = 0; c < cols; c++) {
      uint8_t bits = emoji_fb[p * EMOJI_W + c];
      for (uint8_t b = 0; b < 8; b++) {
        if ((uint8_t)(p * 8 + b) >= rows) break;
        if (bits & (1u << b)) ui_px(x + c, y + p * 8 + b, 1);
      }
    }
  }
}

/* 返回 UTF-8 字符字节数，width = 8/16/0（0 表示无法显示，跳过） */
static uint8_t ui_utf8_info(const char *s, uint8_t *width) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80)              { *width = 8;  return 1; }
  if ((c & 0xE0) == 0xC0)    { *width = 16; return 2; }
  if ((c & 0xF0) == 0xE0)    { *width = 16; return 3; }
  *width = 0;
  return (c & 0xF8) == 0xF0 ? 4 : 1;
}

/* 把 16 行点阵按任意像素 y 画进显存（top=上 8 行，bot=下 8 行） */
static void ui_draw_glyph_bits(int x, int y, const unsigned char *top,
                               const unsigned char *bot, int cols) {
  for (int c = 0; c < cols; c++) {
    unsigned bits = (unsigned)top[c] | ((unsigned)bot[c] << 8);
    for (int b = 0; b < 16; b++)
      if (bits & (1u << b)) ui_px(x + c, y + b, 1);
  }
}

/* 从 GT20L16S 取一个字模并画到显存；x/y 为左上角像素坐标 */
static void ui_draw_char(int x, int y, const char *s, uint8_t bytes, uint8_t width) {
  if (width == 8 && bytes == 1) {
    unsigned char g[16];
    unsigned int fa = 0x3cf80u + (unsigned)((unsigned char)s[0] - 0x20u) * 16u;
    axk_gt20l16s_get_fonts_form_rom((unsigned char)(fa >> 16),
                                    (unsigned char)(fa >> 8),
                                    (unsigned char)fa, g, 16);
    ui_draw_glyph_bits(x, y, g, g + 8, 8);
    return;
  }
  if (width != 16) return;

  unsigned char blen = 0;
  unsigned int unicode = utf8_to_unicode(s, &blen);
  if (!unicode) return;
  unsigned int fa = unicode_to_gb2312_fontaddr(unicode);
  if (!fa) return;

  unsigned char code[2];
  axk_gt20l16s_get_fonts_form_rom((unsigned char)(fa >> 16),
                                  (unsigned char)(fa >> 8),
                                  (unsigned char)fa, code, 2);
  unsigned char hi = code[0], lo = code[1];
  unsigned int ga;
  if (hi >= 0xB0 && hi <= 0xF7 && lo >= 0xA1)
    ga = ((unsigned int)(hi - 0xB0) * 94u + (unsigned)(lo - 0xA1) + 846u) * 32u;
  else if (hi >= 0xA1 && hi <= 0xA3 && lo >= 0xA1)
    ga = ((unsigned int)(hi - 0xA1) * 94u + (unsigned)(lo - 0xA1)) * 32u;
  else
    return;

  unsigned char glyph[32];
  axk_gt20l16s_get_fonts_form_rom((unsigned char)(ga >> 16),
                                  (unsigned char)(ga >> 8),
                                  (unsigned char)ga, glyph, 32);
  ui_draw_glyph_bits(x, y, glyph, glyph + 16, 16);
}

/* 画一行文字，遇到 \n 或右边界停止 */
static void ui_draw_text_line(int x, int y, const char *s) {
  while (*s && *s != '\n') {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20) { s++; continue; }
    uint8_t w = 0;
    uint8_t cb = ui_utf8_info(s, &w);
    if (x + w > 127) break;
    if (w) ui_draw_char(x, y, s, cb, w);
    x += w;
    s += cb;
  }
}

/* AI 模块返回的 emotion 字符串 -> 表情编号（与 emoji_draw_face 的 id 对应） */
static uint8_t emoji_id_from_emotion(const char *emotion) {
  if (!emotion || !emotion[0]) return 7;
  if (!strcmp(emotion, "happy")  || !strcmp(emotion, "excited"))  return 0;
  if (!strcmp(emotion, "sad")    || !strcmp(emotion, "depressed")) return 1;
  if (!strcmp(emotion, "angry")  || !strcmp(emotion, "mad"))      return 2;
  if (!strcmp(emotion, "surprised") || !strcmp(emotion, "shocked")) return 3;
  if (!strcmp(emotion, "cool"))    return 4;
  if (!strcmp(emotion, "winking") || !strcmp(emotion, "wink") ||
      !strcmp(emotion, "confident") || !strcmp(emotion, "silly")) return 5;
  if (!strcmp(emotion, "loving") || !strcmp(emotion, "love"))     return 6;
  return 7;
}

/* 没有 emotion 时，用 emoji 字符猜一个表情 */
static uint8_t emoji_id_from_emoji(const char *emoji) {
  if (!emoji || !emoji[0]) return 7;
  if (strstr(emoji, "\xF0\x9F\x98\x8A") || strstr(emoji, "\xF0\x9F\x98\x84") ||
      strstr(emoji, "\xF0\x9F\x98\x81") || strstr(emoji, "\xF0\x9F\x98\x82")) return 0;
  if (strstr(emoji, "\xF0\x9F\x98\xA2") || strstr(emoji, "\xF0\x9F\x98\xAD") ||
      strstr(emoji, "\xF0\x9F\x98\x94")) return 1;
  if (strstr(emoji, "\xF0\x9F\x98\xA0") || strstr(emoji, "\xF0\x9F\x98\xA1")) return 2;
  if (strstr(emoji, "\xF0\x9F\x98\xAE") || strstr(emoji, "\xF0\x9F\x98\xB2")) return 3;
  if (strstr(emoji, "\xF0\x9F\x98\x8E")) return 4;
  if (strstr(emoji, "\xF0\x9F\x98\x89")) return 5;
  if (strstr(emoji, "\xE2\x9D\xA4"))     return 6;  /* U+2764 心 */
  return 7;
}

/* 按 AI 状态画脸。frame 用于讲话嘴型 / 思考点 / 睡觉 Zzz 的动画 */
static void emoji_draw_state_face(ai_face_state_t st, uint8_t emotion, uint8_t frame) {
  switch (st) {
  case AI_FACE_LISTENING:
    emoji_clear();
    emoji_eye(24, 14, 0, 0);                /* 睁大眼睛 */
    emoji_eye(48, 14, 0, 0);
    emoji_mouth_o(36, 24, 4);               /* 小圆嘴，专注听 */
    break;

  case AI_FACE_THINKING:
    emoji_clear();
    emoji_eye(24, 14, 2, -2);               /* 眼睛往右上瞟 */
    emoji_eye(48, 14, 2, -2);
    emoji_hline(28, 44, 25, 1);             /* 抿嘴 */
    emoji_hline(28, 44, 26, 1);
    if (frame & 1) {                         /* 右上角思考泡泡 */
      emoji_fill_circle(60, 12, 2, 1);
      emoji_fill_circle(62, 7, 3, 1);
      emoji_fill_circle(64, 3, 2, 1);
    }
    break;

  case AI_FACE_SPEAKING:
    emoji_draw_face(emotion);                /* 保留情绪的眼睛/眉毛 */
    emoji_rect(21, 23, 51, 30, 0);           /* 擦掉原来的嘴（避开眼睛） */
    emoji_hline(26, 46, 22, 0);              /* 八字眉/惊讶嘴在 y=22 的残留 */
    emoji_hline(31, 41, 18, 0);              /* 清掉惊讶圆嘴的顶部 */
    emoji_hline(31, 41, 19, 0);
    emoji_hline(31, 41, 20, 0);
    emoji_hline(31, 41, 21, 0);
    emoji_fill_circle(36, 24, (frame & 1) ? 4 : 2, 1); /* 张嘴/闭嘴 */
    break;

  case AI_FACE_SLEEPING:
    emoji_clear();
    emoji_eye_sleep(24, 14);                 /* 闭眼 */
    emoji_eye_sleep(48, 14);
    emoji_hline(28, 44, 25, 1);              /* 小嘴 */
    emoji_hline(28, 44, 26, 1);
    break;

  case AI_FACE_IDLE:
  default:
    emoji_draw_face(emotion);
    break;
  }
}

/* ============ 右上角 24x24 状态图标 ============ */
/* 待机：电源符号 */
static void emoji_icon_idle(void) {
  emoji_fill_circle(12, 13, 8, 1);
  emoji_fill_circle(12, 13, 6, 0);
  emoji_rect(9, 1, 14, 5, 0);        /* 顶部缺口 */
  emoji_rect(11, 2, 12, 13, 1);      /* 电源竖线 */
}

/* 聆听：麦克风 */
static void emoji_icon_listening(void) {
  emoji_fill_circle(12, 6, 3, 1);
  emoji_rect(9, 6, 14, 12, 1);
  emoji_fill_circle(12, 12, 3, 1);
  emoji_line(6, 9, 6, 14, 1);
  emoji_line(18, 9, 18, 14, 1);
  emoji_line(6, 14, 9, 17, 1);
  emoji_line(18, 14, 15, 17, 1);
  emoji_hline(9, 15, 17, 1);
  emoji_rect(11, 17, 12, 20, 1);
  emoji_hline(8, 16, 21, 1);
}

/* 思考：灯泡 */
static void emoji_icon_thinking(void) {
  emoji_fill_circle(12, 8, 6, 1);
  emoji_fill_circle(12, 8, 4, 0);
  emoji_line(9, 10, 10, 6, 1);       /* 灯丝 */
  emoji_line(10, 6, 12, 10, 1);
  emoji_line(12, 10, 14, 6, 1);
  emoji_line(14, 6, 15, 10, 1);
  emoji_rect(9, 14, 14, 15, 1);      /* 灯座 */
  emoji_rect(10, 16, 13, 17, 1);
  emoji_rect(11, 18, 12, 19, 1);
  emoji_line(12, 0, 12, 2, 1);       /* 光芒 */
  emoji_line(5, 2, 7, 4, 1);
  emoji_line(19, 2, 17, 4, 1);
  emoji_line(2, 8, 4, 8, 1);
  emoji_line(22, 8, 20, 8, 1);
}

/* 讲话：喇叭 + 声波 */
static void emoji_icon_speaking(void) {
  emoji_rect(3, 10, 6, 14, 1);
  for (int y = 5; y <= 19; y++) {
    int x0;
    if (y < 10) x0 = 13 - (y - 5) * 7 / 5;
    else if (y <= 14) x0 = 7;
    else x0 = 7 + (y - 14) * 7 / 5;
    emoji_hline(x0, 13, y, 1);
  }
  emoji_line(16, 8, 18, 12, 1);
  emoji_line(18, 12, 16, 16, 1);
  emoji_line(18, 5, 21, 12, 1);
  emoji_line(21, 12, 18, 19, 1);
}

/* 休眠：月亮 + 星星 */
static void emoji_icon_sleeping(void) {
  emoji_fill_circle(10, 12, 7, 1);
  emoji_fill_circle(13, 9, 6, 0);
  emoji_px(19, 3, 1);
  emoji_px(19, 4, 1);
  emoji_px(19, 5, 1);
  emoji_px(18, 4, 1);
  emoji_px(20, 4, 1);
}

/* 圆的上半弧，wifi 图标用 */
static void emoji_arc_top(int cx, int cy, int r) {
  for (int dx = -r; dx <= r; dx++) {
    int yy = 0;
    while ((yy + 1) * (yy + 1) + dx * dx <= r * r) yy++;
    emoji_px(cx + dx, cy - yy, 1);
  }
}

/* 未联网：WiFi 信号 + 斜杠 */
static void emoji_icon_wifi_off(void) {
  emoji_arc_top(12, 18, 6);
  emoji_arc_top(12, 18, 11);
  emoji_fill_circle(12, 19, 2, 1);
  emoji_line(4, 20, 19, 4, 1);       /* 禁止斜杠 */
}

/* 画右上角小状态图标，并 OR 进显存；没联网时固定显示 WiFi 断开图标 */
static void ai_draw_state_icon(ai_face_state_t st) {
  emoji_clear();
  if (!ai_net_ok) {
    emoji_icon_wifi_off();
  } else {
    switch (st) {
    case AI_FACE_LISTENING: emoji_icon_listening(); break;
    case AI_FACE_THINKING:  emoji_icon_thinking();  break;
    case AI_FACE_SPEAKING:  emoji_icon_speaking();  break;
    case AI_FACE_SLEEPING:  emoji_icon_sleeping();  break;
    default:                emoji_icon_idle();      break;
    }
  }
  emoji_blit_gram(AI_ICON_X, AI_ICON_Y, AI_ICON_W, AI_ICON_W);
}

/* 一行文字的像素宽度（与 ui_draw_text_line 的计量一致） */
static uint8_t ui_text_width(const char *s) {
  uint8_t w = 0;
  while (*s && *s != '\n') {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20) { s++; continue; }
    uint8_t cw = 0;
    uint8_t cb = ui_utf8_info(s, &cw);
    w += cw;
    s += cb;
  }
  return w;
}

/* 按像素宽度折行（ASCII 8px，中文/emoji 16px），只记录每行起始偏移。
 * 结果放在 ai_text_line_off[]/ai_text_line_cnt，绘制时再引用原串，省 RAM。 */
static void ai_wrap_lines(const char *src) {
  uint16_t i = 0, line_start = 0;
  uint8_t li = 0, w = 0;

  ai_text_line_cnt = 0;
  if (!src || !src[0]) {
    ai_text_line_off[0] = 0;
    ai_text_line_cnt = 1;
    return;
  }

  while (src[i] && li < AI_TEXT_MAX_LINES) {
    unsigned char c = (unsigned char)src[i];
    uint8_t cw, cb;

    if (c == '\r') { i++; continue; }
    if (c == '\n') {
      ai_text_line_off[li++] = line_start;
      i++;
      line_start = i;
      w = 0;
      continue;
    }
    if (c < 0x80)                { cw = 8;  cb = 1; }
    else if ((c & 0xE0) == 0xC0) { cw = 16; cb = 2; }
    else if ((c & 0xF0) == 0xE0) { cw = 16; cb = 3; }
    else if ((c & 0xF8) == 0xF0) { cw = 16; cb = 4; }
    else { i++; continue; }

    if (w > 0 && (uint16_t)w + cw > AI_TEXT_MAXW) {
      ai_text_line_off[li++] = line_start;
      if (li >= AI_TEXT_MAX_LINES) break;
      line_start = i;
      w = 0;
    }
    w += cw;
    i += cb;
  }
  if (li < AI_TEXT_MAX_LINES && w > 0)
    ai_text_line_off[li++] = line_start;
  ai_text_line_cnt = li ? li : 1;
}

/* 画下半屏回复区（y=31..62，最多两行）。
 * ai_text_top 是当前显示的第一行，长回复由 ai_face_tick 自动翻页；
 * 没有回复时按状态显示提示语，单行垂直居中（y=39）。 */
static void ai_render_reply_text(ai_face_state_t st, const char *reply) {
  const char *hint;

  /* 没联网：不考虑 AI 状态，固定提示“未连接网络” */
  if (!ai_net_ok) {
    static const char off_text[] = "未连接网络";
    ai_text_line_cnt = 1;
    ai_text_top = 0;
    ui_clear_rect(AI_TEXT_X, AI_TEXT_Y0, AI_TEXT_X + AI_TEXT_MAXW - 1, 62);
    ui_draw_text_line((uint8_t)((128 - ui_text_width(off_text)) / 2), 39,
                      off_text);
    return;
  }

  ai_wrap_lines(reply);
  if (ai_text_top >= ai_text_line_cnt) ai_text_top = 0;

  ui_clear_rect(AI_TEXT_X, AI_TEXT_Y0, AI_TEXT_X + AI_TEXT_MAXW - 1, 62);

  if (!reply || !reply[0]) {
    switch (st) {
    case AI_FACE_LISTENING: hint = "我在听，请说..."; break;
    case AI_FACE_THINKING:  hint = "让我想想...";     break;
    case AI_FACE_SLEEPING:  hint = "我先睡会儿...";   break;
    case AI_FACE_SPEAKING:  hint = "...";             break;
    default:                hint = "你好，我是小安";  break;
    }
    ui_draw_text_line((uint8_t)((128 - ui_text_width(hint)) / 2), 39, hint);
    return;
  }

  ui_draw_text_line(AI_TEXT_X, AI_TEXT_Y0,
                    reply + ai_text_line_off[ai_text_top]);
  if ((uint8_t)(ai_text_top + 1) < ai_text_line_cnt)
    ui_draw_text_line(AI_TEXT_X, AI_TEXT_Y1,
                      reply + ai_text_line_off[ai_text_top + 1]);
}

/* 画一个小 z（宽高 size），用于睡觉动画 */
static void ui_small_z(int x, int y, int size) {
  ui_hline(x, x + size - 1, y, 1);
  for (int i = 0; i < size; i++) ui_px(x + size - 1 - i, y + i, 1);
  ui_hline(x, x + size - 1, y + size - 1, 1);
}

/* 睡觉时在大脸和右上图标之间飘 Zz（x=74..101, y=0..30）。
 * 注意会擦到顶部边框，调用方之后要再画一次 ui_frame()。 */
static void ai_draw_sleep_zzz(uint8_t frame) {
  ui_clear_rect(74, 0, 101, 30);
  switch (frame % 6) {
  case 0: ui_small_z(76, 21, 7); break;
  case 1: ui_small_z(76, 15, 7); ui_small_z(86, 23, 7); break;
  case 2: ui_small_z(80, 9, 7);  ui_small_z(90, 17, 7); break;
  case 3: ui_small_z(84, 3, 7);  ui_small_z(94, 11, 7); break;
  case 4: ui_small_z(88, 2, 7);  ui_small_z(94, 10, 7); break;
  default: ui_small_z(92, 2, 7); break;
  }
}

/* 整屏合成一次再刷新：显存里画完才推屏，任何状态变化都不会闪 */
static void ai_compose(ai_face_state_t st, uint8_t emo, uint8_t frame,
                       const char *text) {
  /* 没联网时固定待机脸，避免还在演“聆听/思考/讲话” */
  if (!ai_net_ok) {
    st = AI_FACE_IDLE;
    emo = 7;                 /* neutral */
    frame = 0;
  }
  ui_clear();
  emoji_draw_state_face(st, emo, frame);
  emoji_blit_gram(AI_FACE_X, 0, EMOJI_W, EMOJI_H);
  ai_draw_state_icon(st);
  ai_render_reply_text(st, text);
  if (st == AI_FACE_SLEEPING) ai_draw_sleep_zzz(frame);
  ui_frame();                       /* 边框最后画，任何局部擦除都会被补回 */
  axk_ssd1306_refresh();
  ai_shown_state = st;
}

/* 取回复快照，避免刷屏期间被 MCP 解析任务改写 */
static void ai_reply_snapshot(void) {
  taskENTER_CRITICAL();
  {
    size_t n = strlen(ai_reply_buf);
    if (n >= sizeof(ai_reply_draw)) n = sizeof(ai_reply_draw) - 1;
    memcpy(ai_reply_draw, ai_reply_buf, n);
    ai_reply_draw[n] = '\0';
  }
  taskEXIT_CRITICAL();
}

/* 画完整第 1 页 */
static void ai_face_page_draw(void) {
  ai_face_state_t st;
  uint8_t emo;

  taskENTER_CRITICAL();
  st = ai_face_state;
  emo = ai_emotion_id;
  taskEXIT_CRITICAL();

  ai_reply_snapshot();
  ai_compose(st, emo, 0, ai_reply_draw);
}

/* 状态变化时重画整页 */
static void ai_face_page_full(void) {
  ai_page_dirty = 0;
  ai_face_page_draw();
}

/* 同一状态下更新（表情或回复变了）：整屏重合成，一次推屏不闪 */
static void ai_reply_text_update(void) {
  ai_face_page_draw();
}

/* 第 1 页动画/状态机心跳，每 50ms 调一次 */
static void ai_face_tick(void) {
  static uint32_t last_anim = 0;
  static uint8_t  anim_frame = 0;
  uint32_t now = xTaskGetTickCount();
  ai_face_state_t st = ai_face_state;
  uint8_t emo = ai_emotion_id;

  /* 状态超时推断 */
  if (st == AI_FACE_LISTENING && !ai_listen_after_speak &&
      (uint32_t)(now - ai_state_since) > pdMS_TO_TICKS(3500)) {
    /* 唤醒后一段时间没听到讲话，认为用户说完了，AI 正在思考 */
    st = AI_FACE_THINKING;
    ai_face_state = st;
    ai_state_since = now;
    ai_page_dirty = 1;
  } else if (st == AI_FACE_SPEAKING && !ai_text_active &&
             (uint32_t)(now - ai_last_text_tick) > pdMS_TO_TICKS(2000)) {
    /* 讲话结束，回到聆听（等待多轮追问） */
    st = AI_FACE_LISTENING;
    ai_face_state = st;
    ai_state_since = now;
    ai_listen_after_speak = true;
    ai_page_dirty = 1;
  } else if (st == AI_FACE_THINKING &&
             (uint32_t)(now - ai_state_since) > pdMS_TO_TICKS(15000)) {
    /* 长时间没有回应，别一直卡在思考 */
    st = AI_FACE_LISTENING;
    ai_face_state = st;
    ai_state_since = now;
    ai_listen_after_speak = true;
    ai_page_dirty = 1;
  }

  if (ai_page_dirty) {
    ai_face_state_t cur;
    ai_page_dirty = 0;
    cur = ai_face_state;
    if (cur != ai_shown_state) {
      ai_face_page_full();
    } else {
      /* 状态没变（新文字或新表情）：整屏重合成一次，不闪 */
      ai_reply_text_update();
    }
    /* 新内容先完整停留一个翻页周期 */
    ai_text_scroll_tick = now;
    last_anim = now;
    return;
  }

  if (!ai_net_ok) {
    /* 离线时不播任何状态动画，屏幕保持“WiFi 断开 + 未连接网络” */
  } else if (st == AI_FACE_SPEAKING) {
    if ((uint32_t)(now - last_anim) >= pdMS_TO_TICKS(200)) {
      last_anim = now;
      anim_frame ^= 1;
      /* 只重画左上大脸区，其他内容留在显存里 */
      ui_clear_rect(AI_FACE_X, 0, AI_FACE_X + EMOJI_W - 1, 30);
      emoji_draw_state_face(st, emo, anim_frame);
      emoji_blit_gram(AI_FACE_X, 0, EMOJI_W, EMOJI_H);
      ui_frame();
      axk_ssd1306_refresh();
    }
  } else if (st == AI_FACE_SLEEPING) {
    if ((uint32_t)(now - last_anim) >= pdMS_TO_TICKS(500)) {
      last_anim = now;
      anim_frame = (uint8_t)((anim_frame + 1) % 6);
      ai_draw_sleep_zzz(anim_frame);
      ui_frame();
      axk_ssd1306_refresh();
    }
  } else if (st == AI_FACE_THINKING) {
    if ((uint32_t)(now - last_anim) >= pdMS_TO_TICKS(700)) {
      last_anim = now;
      anim_frame ^= 1;
      ui_clear_rect(AI_FACE_X, 0, AI_FACE_X + EMOJI_W - 1, 30);
      emoji_draw_state_face(st, emo, anim_frame);
      emoji_blit_gram(AI_FACE_X, 0, EMOJI_W, EMOJI_H);
      ui_frame();
      axk_ssd1306_refresh();
    }
  }

  /* 长回复自动翻页：每次下移两行，到底后回到开头循环 */
  if (ai_text_line_cnt > 2 &&
      (uint32_t)(now - ai_text_scroll_tick) >= pdMS_TO_TICKS(AI_TEXT_SCROLL_MS)) {
    ai_text_scroll_tick = now;
    ai_text_top = (uint8_t)(ai_text_top + 2);
    if (ai_text_top >= ai_text_line_cnt) ai_text_top = 0;
    ui_clear_rect(AI_TEXT_X, AI_TEXT_Y0, AI_TEXT_X + AI_TEXT_MAXW - 1, 62);
    ui_draw_text_line(AI_TEXT_X, AI_TEXT_Y0,
                      ai_reply_draw + ai_text_line_off[ai_text_top]);
    if ((uint8_t)(ai_text_top + 1) < ai_text_line_cnt)
      ui_draw_text_line(AI_TEXT_X, AI_TEXT_Y1,
                        ai_reply_draw + ai_text_line_off[ai_text_top + 1]);
    axk_ssd1306_refresh();
  }
}

static uint8_t emoji_label_width(const char *s) {
  uint8_t w = 0;
  while (*s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { w += 8; s++; }
    else if ((c & 0xE0) == 0xC0) { w += 16; s += 2; }
    else if ((c & 0xF0) == 0xE0) { w += 16; s += 3; }
    else { w += 16; s += 4; }
  }
  return w;
}

/* 清掉底部两页（表情名称所在行），避免不同长度的标签留下残影 */
static void emoji_clear_label_line(void) {
  for (uint8_t p = 0; p < 2; p++) {
    axk_ssd1306_set_address(0, 6 + p);
    for (uint8_t x = 0; x < 128; x++)
      axk_ssd1306_write_byte(0x00, AXK_SSD1306_WRITE_DATA);
  }
}

static void draw_emoji_page(void) {
  const char *label = emoji_labels[emoji_demo_idx];
  uint8_t w = emoji_label_width(label);

  axk_ssd1306_show_utf8_str(0, 0, "# AI表情 10/10");
  emoji_draw_face(emoji_demo_idx);
  emoji_blit(EMOJI_X, EMOJI_PAGE);
  emoji_clear_label_line();
  axk_ssd1306_show_utf8_str(w < 128 ? (uint8_t)((128 - w) / 2) : 0, 6, label);
}

static void draw_value_line(void) {
  char buf[32];
  switch (page_index) {
  case 0:
    ai_face_tick();
    break;
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
  case 9:
    /* 自动轮播：每次刷新切下一张表情。
     * 表情区 72x32 每次整块覆盖，标签行单独清，避免整屏闪烁。 */
    emoji_demo_idx = (uint8_t)((emoji_demo_idx + 1) % EMOJI_COUNT);
    draw_emoji_page();
    break;
  }
}

static void draw_page_full(void) {
  char buf[32];
  axk_ssd1306_clear_screen();
  switch (page_index) {
  case 0:
    /* 第 1 页：AI 表情 + 状态 + 回复；屏幕已由 draw_page_full 清空 */
    ai_page_dirty = 0;
    ai_face_page_draw();
    break;
  case 1:
    axk_ssd1306_show_utf8_str(0,0,"# 温度      2/10");
    { uint8_t ti=(uint8_t)g_temp; uint8_t td=((uint8_t)(g_temp*10))%10;
      snprintf(buf,sizeof(buf),"    T:%2d.%d C    ",ti,td); }
    axk_ssd1306_show_utf8_str(0,2,buf);
    axk_ssd1306_show_utf8_str(0,4,"----------------");
    axk_ssd1306_show_utf8_str(0,6,sht30_is_init?"  SHT3x | OK       ":"SHT3x | ERR      ");
    break;
  case 2:
    axk_ssd1306_show_utf8_str(0,0,"# 湿度      3/10");
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
    axk_ssd1306_show_utf8_str(0,0,"# 任务监控  4/10");
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
    axk_ssd1306_show_utf8_str(0,0,"# UART数据  5/10");
    snprintf(buf,sizeof(buf),"%.14s",raw); axk_ssd1306_show_utf8_str(0,2,buf);
    break;
  }
  case 5: {
    axk_ssd1306_show_utf8_str(0,0,"# AI模块    6/10");
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
    axk_ssd1306_show_utf8_str(0,0,"# 猫咪动画  7/10");
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
    axk_ssd1306_show_utf8_str(0, 0, "# MCP工具   8/10");
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
    axk_ssd1306_show_utf8_str(0, 0, "# IR学习   9/10");
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
  case 9:
    /* AI 表情演示页：进入时从第一张开始，之后每秒自动轮播 */
    emoji_demo_idx = 0;
    draw_emoji_page();
    break;
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

/* ---- 向 AI 模组注册/更新 MCP 工具列表 ---- */
static void send_mcp_tool_list(void) {
  const char *mcp_tools_fmt =
    "mcp-tool "
    "{\"role\":\"MCU\",\"msgType\":\"MCP\",\"data\":"
    "{\"tools\":["
    "{\"name\":\"ac\",\"description\":\"空调控制，可开关机、设温度、切模式、调风速，一次可传多个参数\",\"inputSchema\":{\"properties\":{\"power\":{\"description\":\"on打开/off关闭(可选)\",\"type\":\"string\"},\"temperature\":{\"description\":\"目标温度16-31(可选)\",\"type\":\"number\"},\"mode\":{\"description\":\"auto/cool/dry/fan/heat(可选)\",\"type\":\"string\"},\"fan\":{\"description\":\"auto/low/mid/high(可选)\",\"type\":\"string\"}}}},"
    "{\"name\":\"ledstrip\",\"description\":\"WS2812灯带开关、颜色和亮度控制。开灯默认暖黄色，关灯完全熄灭\",\"inputSchema\":{\"properties\":{\"mode\":{\"description\":\"on开灯(默认暖黄色)/off关灯(完全熄灭)/set设置颜色/query查询状态\",\"type\":\"string\"},\"r\":{\"description\":\"红色0-255(mode=set时)\",\"type\":\"number\"},\"g\":{\"description\":\"绿色0-255(mode=set时)\",\"type\":\"number\"},\"b\":{\"description\":\"蓝色0-255(mode=set时)\",\"type\":\"number\"},\"brightness\":{\"description\":\"亮度0-100\",\"type\":\"number\"}}}},"
    "{\"name\":\"temperature_humidity\",\"description\":\"查询当前环境温度和湿度\",\"inputSchema\":{\"properties\":{\"query\":{\"description\":\"固定填all即可(可选)\",\"type\":\"string\"}}}}"
    "]}}";
  uartPortSendData((char *)mcp_tools_fmt, strlen(mcp_tools_fmt));
  uartPortSendData("\r\n", 2);
  log_info("[MCP] mcp-tool sent (3 tools)");
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
  emMCP_Init(&emMCP);
  log_info("emMCP init done, MAX_TOOLS=%d", MCP_SERVER_TOOL_NUMBLE_MAX);

  /* 注册工具：用栈变量复用避免静态内存重复占用 */
  emMCP_tool_t t;

  /* 初始化红外模块 */
  hxd039b2_init();

  /* 空调控制：电源/温度/模式/风速 合并为一个工具 */
  memset(&t, 0, sizeof(t));
  t.name = "ac";
  t.description = "空调控制工具，可开关机、设置温度、模式和风速，一次调用可传多个参数";
  t.inputSchema.properties[0].name = "power";
  t.inputSchema.properties[0].description = "on打开空调 / off关闭空调 (可选)";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.inputSchema.properties[1].name = "temperature";
  t.inputSchema.properties[1].description = "目标温度，范围16到31度 (可选)";
  t.inputSchema.properties[1].type = MCP_SERVER_TOOL_TYPE_NUMBER;
  t.inputSchema.properties[2].name = "mode";
  t.inputSchema.properties[2].description = "运行模式：auto自动 cool制冷 dry除湿 fan送风 heat制热 (可选)";
  t.inputSchema.properties[2].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.inputSchema.properties[3].name = "fan";
  t.inputSchema.properties[3].description = "风速：auto自动 low低 mid中 high高 (可选)";
  t.inputSchema.properties[3].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = ac_set_handler;
  t.checkRequestHandler = ac_check_handler;
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

  /* 温湿度查询 */
  memset(&t, 0, sizeof(t));
  t.name = "temperature_humidity";
  t.description = "环境温湿度查询工具，返回当前温度(摄氏度)和湿度(百分比)";
  t.inputSchema.properties[0].name = "query";
  t.inputSchema.properties[0].description = "固定填all即可 (可选)";
  t.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_STRING;
  t.setRequestHandler = temp_humidity_set_handler;
  t.checkRequestHandler = temp_humidity_check_handler;
  emMCP_AddToolToToolList(&t);

#ifndef EMMCP_MANUAL_TOOLS_ONLY
  emMCP_RegistrationTools();
#endif

  /* 首次尝试注册工具；模组就绪后会在 AI_START / 唤醒时自动重发 */
  send_mcp_tool_list();

  for (;;) {
    while (rx_msg_idx < rx_msg_count) {
      uartPortRecvData(rx_msg_buf[rx_msg_idx], strlen(rx_msg_buf[rx_msg_idx]));
      rx_msg_idx++;
      emMCP_TickHandle(100);
    }
    if (mcp_tools_resend) {
      mcp_tools_resend = false;
      send_mcp_tool_list();
    }
    emMCP_TickHandle(100);
  }
  /* USER CODE END StartDefaultTask */
}

/* 轮询雷达；状态变化并稳定 3 次后，只在串口打印一行 */
static void radar_poll(void)
{
  bool now_present = axk_radar_is_present();
  if(now_present != radar_last_present){
    if(++radar_debounce_cnt >= 3){
      radar_last_present = now_present;
      radar_debounce_cnt = 0;
      log_info(">>> 雷达: %s", now_present ? "有人" : "无人");
    }
  }else{
    radar_debounce_cnt = 0;
  }
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

  /* 雷达：记录初始状态，之后只在变化时打印 */
  axk_radar_init();
  radar_last_present = axk_radar_is_present();
  log_info("雷达初始: %s", radar_last_present ? "有人" : "无人");

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
        radar_poll();
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
        radar_poll();
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
      if(page_index==0) ai_face_tick();
      radar_poll();
      if(ai_event_updated&&(page_index==0||page_index==4||page_index==5)){ai_event_updated=0;break;}
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
  log_info("[SYS] FreeRTOS heap free: %u B", (unsigned)xPortGetFreeHeapSize());
  /* 上电默认熄灭，开灯由语音指令控制（默认暖黄色） */
  axk_ws2812_set_all_pixels_color(0, 0, 0, 0.0f);
  for(;;){
    osDelay(pdMS_TO_TICKS(1000));
  }
  /* USER CODE END ws2812_modeTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
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

