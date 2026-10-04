#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "driver/st7565.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "driver/keyboard.h"

#define SI4732_ADDR 0x22

extern void I2C_WriteBuffer(uint8_t addr, const uint8_t *data, uint8_t len);
extern void I2C_ReadBuffer(uint8_t addr, uint8_t *data, uint8_t len);
extern uint8_t gFrameBuffer[8][128];
extern void ST7565_DrawSmallString(uint8_t x, uint8_t y, const char *str);
extern void ST7565_DrawBigDigits(uint8_t x, uint8_t y, const char *str);

/* ================= 模式与参数结构 ================= */
typedef enum {
    MODE_FM = 0,
    MODE_AM,
    MODE_LSB,
    MODE_USB,
    MODE_CW,
    MODE_MAX
} si_mode_t;

static const char* MODE_TAGS[] = {"FM ", "AM ", "LSB", "USB", "CW "};
static const uint16_t STEP_TABLE[] = {1, 5, 9, 10, 50, 100};
#define STEP_MAX 6

static const char* BW_TAGS[] = {"1.0k", "1.8k", "2.2k", "3.0k", "4.0k", "6.0k"};
#define BW_MAX 6

typedef struct {
    bool      active;
    uint32_t  freq_khz;
    si_mode_t mode;
    uint8_t   step_idx;
    uint8_t   bw_idx;
    uint8_t   att_level;    // 0: OFF, 1: -10dB, 2: -20dB
    int16_t   bfo_offset;
    uint8_t   rssi;
    uint8_t   snr;
    uint8_t   s_meter;      // 0-15
    uint8_t   peak_meter;   // 峰值保持
    uint8_t   peak_decay;   // 峰值衰减计数
} si_radio_ui_t;

static si_radio_ui_t gRadio = {
    .active = false,
    .freq_khz = 14270,      // 默认 20米业余段
    .mode = MODE_USB,
    .step_idx = 0,          // 1kHz
    .bw_idx = 2,            // 2.2kHz
    .att_level = 0,
    .bfo_offset = 0,
    .rssi = 0,
    .snr = 0,
    .s_meter = 0,
    .peak_meter = 0,
    .peak_decay = 0
};

/* ================= 图形引擎 (高质感绘制辅助) ================= */
static inline void DrawPixel(uint8_t x, uint8_t y, uint8_t color) {
    if (x >= 128 || y >= 64) return;
    if (color) gFrameBuffer[y / 8][x] |= (1 << (y % 8));
    else       gFrameBuffer[y / 8][x] &= ~(1 << (y % 8));
}

// 快速水平线与垂直线
static void DrawHLine(uint8_t x, uint8_t y, uint8_t w, uint8_t c) {
    for (uint8_t i = 0; i < w && (x + i) < 128; i++) DrawPixel(x + i, y, c);
}

static void DrawVLine(uint8_t x, uint8_t y, uint8_t h, uint8_t c) {
    for (uint8_t i = 0; i < h && (y + i) < 64; i++) DrawPixel(x, y + i, c);
}

// 绘制空心圆角方框
static void DrawRoundRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    DrawHLine(x + 1, y, w - 2, 1);
    DrawHLine(x + 1, y + h - 1, w - 2, 1);
    DrawVLine(x, y + 1, h - 2, 1);
    DrawVLine(x + w - 1, y + 1, h - 2, 1);
}

// 绘制反色实心圆角徽章标签（用于状态栏 Badge UI）
static void DrawBadge(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    for (uint8_t j = 0; j < h; j++) {
        for (uint8_t i = 0; i < w; i++) {
            if ((i == 0 || i == w - 1) && (j == 0 || j == h - 1)) continue; // 倒角
            DrawPixel(x + i, y + j, 1);
        }
    }
}

/* ================= SI4732 底层控制 ================= */
static void SI_Write(uint8_t *cmd, uint8_t len) {
    I2C_WriteBuffer(SI4732_ADDR, cmd, len);
    SYSTEM_DelayUs(300);
}

static void SI_SetProp(uint16_t prop, uint16_t val) {
    uint8_t b[6] = {0x12, 0x00, prop >> 8, prop & 0xFF, val >> 8, val & 0xFF};
    SI_Write(b, 6);
}

static void Apply_ATT(void) {
    if (gRadio.att_level == 0) SI_SetProp(0x4000, 0x0000); // AGC ON
    else SI_SetProp(0x4001, (gRadio.att_level == 1) ? 10 : 20);
}

static void Apply_BW(void) {
    if (gRadio.mode != MODE_FM) SI_SetProp(0x0101, gRadio.bw_idx);
}

static void Apply_Mode(si_mode_t m) {
    gRadio.mode = m;
    gRadio.bfo_offset = 0;
    if (m == MODE_FM) {
        uint8_t p[] = {0x01, 0x00, 0x05}; SI_Write(p, 3);
    } else if (m == MODE_AM) {
        uint8_t p[] = {0x01, 0x05, 0x05}; SI_Write(p, 3);
        Apply_BW();
    } else {
        uint8_t p[] = {0x01, 0x15, 0x05}; SI_Write(p, 3);
        SI_SetProp(0x0105, (m == MODE_LSB) ? 0x0001 : 0x0002);
        Apply_BW();
    }
    Apply_ATT();
}

static void Apply_Freq(void) {
    if (gRadio.mode == MODE_FM) {
        uint16_t f = gRadio.freq_khz / 10;
        uint8_t t[] = {0x20, 0x00, f >> 8, f & 0xFF}; SI_Write(t, 4);
    } else {
        uint8_t t[] = {0x40, 0x00, gRadio.freq_khz >> 8, gRadio.freq_khz & 0xFF, 0x00, 0x00};
        SI_Write(t, 6);
    }
}

static void Apply_BFO(int16_t d) {
    gRadio.bfo_offset += d;
    uint8_t cmd[4] = {0x41, 0x00, (uint8_t)(gRadio.bfo_offset >> 8), (uint8_t)(gRadio.bfo_offset & 0xFF)};
    SI_Write(cmd, 4);
}

static void Update_Signal(void) {
    uint8_t cmd[2] = {0x23, 0x00}, resp[8] = {0};
    I2C_WriteBuffer(SI4732_ADDR, cmd, 2);
    SYSTEM_DelayUs(500);
    I2C_ReadBuffer(SI4732_ADDR, resp, 8);

    gRadio.rssi = resp[4];
    gRadio.snr  = resp[5];

    // 精准映射 S 表 (0-15: S0-S9, 10=+10dB ... 15=+60dB)
    if (gRadio.rssi < 2)       gRadio.s_meter = 0;
    else if (gRadio.rssi < 25) gRadio.s_meter = gRadio.rssi / 3;
    else if (gRadio.rssi < 34) gRadio.s_meter = 9;
    else                       gRadio.s_meter = 9 + ((gRadio.rssi - 34) / 10);
    if (gRadio.s_meter > 15)   gRadio.s_meter = 15;

    // 拟真 Peak Hold 峰值保持与缓慢衰减
    if (gRadio.s_meter >= gRadio.peak_meter) {
        gRadio.peak_meter = gRadio.s_meter;
        gRadio.peak_decay = 8; // 保持 8 个刷新周期
    } else {
        if (gRadio.peak_decay > 0) gRadio.peak_decay--;
        else if (gRadio.peak_meter > 0) gRadio.peak_meter--;
    }
}

/* ================= 极具质感的 UI 渲染主函数 ================= */
static void Draw_Pro_SMeter(uint8_t x, uint8_t y) {
    // 1. 顶部标尺刻度说明文字
    ST7565_DrawSmallString(x, y - 8, "S 1. 3. 5. 7. 9 +20 +40 +60");

    // 2. 标尺外框架槽
    DrawRoundRect(x - 2, y, 102, 8);

    // 3. 动态填充信号格（每个 S 级占 6 像素，总宽 96 像素）
    uint8_t cur_w = gRadio.s_meter * 6;
    for (uint8_t i = 0; i < cur_w; i++) {
        // 超过 S9 (第 9 级，54 像素) 信号加宽加亮
        uint8_t bar_h = (i >= 54) ? 4 : 2;
        DrawVLine(x + i + 1, y + 5 - bar_h, bar_h, 1);
    }

    // 4. 绘制 Peak 峰值保持竖线 (专业仪表特有细节)
    if (gRadio.peak_meter > 0) {
        uint8_t peak_x = x + (gRadio.peak_meter * 6);
        if (peak_x > (x + 98)) peak_x = x + 98;
        DrawVLine(peak_x, y + 1, 6, 1);
    }
}

static void UI_Render_Dashboard(void) {
    char str[32];
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer)); // 清空全屏

    // ---------------- [1. 顶部专业仪表状态栏] ----------------
    // 模式徽章：圆角深色底块
    DrawBadge(2, 1, 26, 10);
    ST7565_DrawSmallString(4, 2, MODE_TAGS[gRadio.mode]); // 反色或在底块上显现

    // ATT 衰减器标签
    if (gRadio.att_level > 0) {
        DrawRoundRect(32, 1, 28, 10);
        sprintf(str, "-%ddB", gRadio.att_level * 10);
        ST7565_DrawSmallString(34, 2, str);
    } else {
        ST7565_DrawSmallString(34, 2, "NORM");
    }

    // BW 滤波器带宽标签
    DrawRoundRect(64, 1, 30, 10);
    ST7565_DrawSmallString(67, 2, BW_TAGS[gRadio.bw_idx]);

    // 实时信噪比与场强值徽章
    sprintf(str, "%2d dB", gRadio.snr);
    ST7565_DrawSmallString(98, 2, str);

    DrawHLine(0, 12, 128, 1); // 优雅的分割线

    // ---------------- [2. 居中大字体频率面板] ----------------
    if (gRadio.mode == MODE_FM) {
        sprintf(str, "%3d.%02d", (int)(gRadio.freq_khz / 1000), (int)((gRadio.freq_khz % 1000) / 10));
        ST7565_DrawBigDigits(18, 16, str);
        ST7565_DrawSmallString(98, 24, "MHz");
    } else {
        sprintf(str, "%5d", (int)gRadio.freq_khz);
        ST7565_DrawBigDigits(14, 16, str);
        ST7565_DrawSmallString(98, 24, "kHz");
    }

    // ---------------- [3. 参数辅助指示栏] ----------------
    // 步进指示与 SSB 模式下的 BFO 状态
    if (gRadio.mode >= MODE_LSB) {
        sprintf(str, "STEP:%uk  BFO:%+dHz", STEP_TABLE[gRadio.step_idx], gRadio.bfo_offset);
    } else {
        sprintf(str, "STEP: %ukHz", STEP_TABLE[gRadio.step_idx]);
    }
    ST7565_DrawSmallString(14, 35, str);

    // ---------------- [4. 底部高精拟真 S 表] ----------------
    Draw_Pro_SMeter(14, 48);

    ST7565_UpdateDisplay();
}

/* ================= 外部安全接入接口 ================= */

// 长按按键 0 切换进入/退出收音机（对讲与收音机无缝割裂）
void FM_ToggleRadio(void) {
    gRadio.active = !gRadio.active;
    if (gRadio.active) {
        BK4819_SetMode(0);          // BK4819 静音/休眠，避免原机射频干扰
        Apply_Mode(gRadio.mode);
        Apply_Freq();
    } else {
        uint8_t pwr_down[] = {0x11}; // SI4732 关机休眠
        SI_Write(pwr_down, 1);
        BK4819_SetMode(1);          // 彻底恢复原机正常 UV 通信对讲
    }
}

bool FM_IsActive(void) {
    return gRadio.active;
}

// 独享事件分发逻辑
void FM_ProcessKey(KEY_Code_t key) {
    if (!gRadio.active) return;

    switch (key) {
        case KEY_5: // 按 5 循环切换 FM/AM/LSB/USB/CW
            gRadio.mode = (si_mode_t)((gRadio.mode + 1) % MODE_MAX);
            Apply_Mode(gRadio.mode);
            break;

        case KEY_0: // 短按 0 切换 ATT 衰减器 (NORM -> -10dB -> -20dB)
            gRadio.att_level = (gRadio.att_level + 1) % 3;
            Apply_ATT();
            break;

        case KEY_1: // 短按 1 切换频率步进 STEP
            gRadio.step_idx = (gRadio.step_idx + 1) % STEP_MAX;
            break;

        case KEY_2: // 短按 2 切换滤波器带宽 BW
            gRadio.bw_idx = (gRadio.bw_idx + 1) % BW_MAX;
            Apply_BW();
            break;

        case KEY_UP: // 频率步进上调
            gRadio.freq_khz += STEP_TABLE[gRadio.step_idx];
            Apply_Freq();
            break;

        case KEY_DOWN: // 频率步进下调
            if (gRadio.freq_khz > STEP_TABLE[gRadio.step_idx]) {
                gRadio.freq_khz -= STEP_TABLE[gRadio.step_idx];
            }
            Apply_Freq();
            break;

        case KEY_SIDE1: // 侧键 1：BFO +50Hz 微调（仅在 SSB/CW 有效）
            if (gRadio.mode >= MODE_LSB) Apply_BFO(+50);
            break;

        case KEY_SIDE2: // 侧键 2：BFO -50Hz 微调
            if (gRadio.mode >= MODE_LSB) Apply_BFO(-50);
            break;

        case KEY_EXIT: // EXIT 键立即无痕退回收音机，回到原有对讲主屏
            FM_ToggleRadio();
            break;

        default:
            break;
    }
}

// 100ms 刷新任务（由主循环调度）
void FM_UpdateTask(void) {
    if (!gRadio.active) return;
    Update_Signal();
    UI_Render_Dashboard();
}
