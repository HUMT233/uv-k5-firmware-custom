#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "driver/st7565.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "driver/keyboard.h"
#include "driver/eeprom.h"

#define SI4732_ADDR 0x22

// 底层标准引用接口 (对齐 egzumer/armel 原版函数名)
extern void I2C_WriteBuffer(uint8_t addr, const uint8_t *data, uint8_t len);
extern void I2C_ReadBuffer(uint8_t addr, uint8_t *data, uint8_t len);
extern uint8_t gFrameBuffer[8][128];
extern void UI_PrintStringSmall(const char *pString, uint8_t Start, uint8_t End, uint8_t Line);
extern void UI_DisplayFrequency(const char *pDigits, uint8_t X, uint8_t Y, bool bDigitsOn);

// 模式枚举定义
typedef enum {
    MODE_FM = 0,
    MODE_AM,
    MODE_LSB,
    MODE_USB,
    MODE_CW,
    MODE_MAX
} si_mode_t;

static const char* const MODE_TAGS[] = {"FM ", "AM ", "LSB", "USB", "CW "};
static const uint16_t STEP_TABLE[] = {1, 5, 9, 10, 50, 100};
#define STEP_MAX 6

static const char* const BW_TAGS[] = {"1.0k", "1.8k", "2.2k", "3.0k", "4.0k", "6.0k"};
#define BW_MAX 6

typedef struct {
    bool      active;
    uint32_t  freq_khz;
    si_mode_t mode;
    uint8_t   step_idx;
    uint8_t   bw_idx;
    uint8_t   att_level;    // 0: NORM, 1: -10dB, 2: -20dB
    int16_t   bfo_offset;
    uint8_t   rssi;
    uint8_t   snr;
    uint8_t   s_meter;      // 0-15
    uint8_t   peak_meter;
    uint8_t   peak_decay;
} si_radio_ui_t;

static si_radio_ui_t gRadio = {
    .active = false,
    .freq_khz = 14270,      // 默认 20米短波业余段
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

/* --- 点阵绘图函数 --- */
static inline void DrawPixel(uint8_t x, uint8_t y, uint8_t color) {
    if (x >= 128 || y >= 64) return;
    if (color) gFrameBuffer[y / 8][x] |= (1 << (y % 8));
    else       gFrameBuffer[y / 8][x] &= ~(1 << (y % 8));
}

static void DrawHLine(uint8_t x, uint8_t y, uint8_t w, uint8_t c) {
    for (uint8_t i = 0; i < w && (x + i) < 128; i++) DrawPixel(x + i, y, c);
}

static void DrawVLine(uint8_t x, uint8_t y, uint8_t h, uint8_t c) {
    for (uint8_t i = 0; i < h && (y + i) < 64; i++) DrawPixel(x, y + i, c);
}

static void DrawRoundRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    DrawHLine(x + 1, y, w - 2, 1);
    DrawHLine(x + 1, y + h - 1, w - 2, 1);
    DrawVLine(x, y + 1, h - 2, 1);
    DrawVLine(x + w - 1, y + 1, h - 2, 1);
}

static void DrawBadge(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    for (uint8_t j = 0; j < h; j++) {
        for (uint8_t i = 0; i < w; i++) {
            if ((i == 0 || i == w - 1) && (j == 0 || j == h - 1)) continue;
            DrawPixel(x + i, y + j, 1);
        }
    }
}

/* --- SI4732 底层通信控制 --- */
static void SI_Write(uint8_t *cmd, uint8_t len) {
    I2C_WriteBuffer(SI4732_ADDR, cmd, len);
    SYSTEM_DelayUs(300);
}

static void SI_SetProp(uint16_t prop, uint16_t val) {
    uint8_t b[6] = {0x12, 0x00, (uint8_t)(prop >> 8), (uint8_t)(prop & 0xFF), (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    SI_Write(b, 6);
}

static void Apply_ATT(void) {
    if (gRadio.att_level == 0) SI_SetProp(0x4000, 0x0000);
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
        uint8_t t[] = {0x20, 0x00, (uint8_t)(f >> 8), (uint8_t)(f & 0xFF)}; SI_Write(t, 4);
    } else {
        uint8_t t[] = {0x40, 0x00, (uint8_t)(gRadio.freq_khz >> 8), (uint8_t)(gRadio.freq_khz & 0xFF), 0x00, 0x00};
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

    if (gRadio.rssi < 2)       gRadio.s_meter = 0;
    else if (gRadio.rssi < 25) gRadio.s_meter = gRadio.rssi / 3;
    else if (gRadio.rssi < 34) gRadio.s_meter = 9;
    else                       gRadio.s_meter = 9 + ((gRadio.rssi - 34) / 10);
    if (gRadio.s_meter > 15)   gRadio.s_meter = 15;

    if (gRadio.s_meter >= gRadio.peak_meter) {
        gRadio.peak_meter = gRadio.s_meter;
        gRadio.peak_decay = 8;
    } else {
        if (gRadio.peak_decay > 0) gRadio.peak_decay--;
        else if (gRadio.peak_meter > 0) gRadio.peak_meter--;
    }
}

/* --- 高颜值 S 表与仪表盘绘制 --- */
static void Draw_Pro_SMeter(uint8_t x, uint8_t y) {
    UI_PrintStringSmall("S 1. 3. 5. 7. 9 +20 +40 +60", x, 127, (y / 8) - 1);
    DrawRoundRect(x - 2, y, 102, 8);
    uint8_t cur_w = gRadio.s_meter * 6;
    for (uint8_t i = 0; i < cur_w; i++) {
        uint8_t bar_h = (i >= 54) ? 4 : 2;
        DrawVLine(x + i + 1, y + 5 - bar_h, bar_h, 1);
    }
    if (gRadio.peak_meter > 0) {
        uint8_t peak_x = x + (gRadio.peak_meter * 6);
        if (peak_x > (x + 98)) peak_x = x + 98;
        DrawVLine(peak_x, y + 1, 6, 1);
    }
}

static void UI_Render_Dashboard(void) {
    char str[32];
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    // 1. 顶部模式徽标与状态
    DrawBadge(2, 1, 26, 10);
    UI_PrintStringSmall(MODE_TAGS[gRadio.mode], 4, 127, 0);

    if (gRadio.att_level > 0) {
        DrawRoundRect(32, 1, 28, 10);
        sprintf(str, "-%ddB", gRadio.att_level * 10);
        UI_PrintStringSmall(str, 34, 127, 0);
    } else {
        UI_PrintStringSmall("NORM", 34, 127, 0);
    }

    DrawRoundRect(64, 1, 30, 10);
    UI_PrintStringSmall(BW_TAGS[gRadio.bw_idx], 67, 127, 0);

    sprintf(str, "%2ddB", gRadio.snr);
    UI_PrintStringSmall(str, 98, 127, 0);
    DrawHLine(0, 12, 128, 1);

    // 2. 频率大字显示
    if (gRadio.mode == MODE_FM) {
        sprintf(str, "%3d.%02d", (int)(gRadio.freq_khz / 1000), (int)((gRadio.freq_khz % 1000) / 10));
        UI_DisplayFrequency(str, 18, 2, true);
        UI_PrintStringSmall("MHz", 98, 127, 3);
    } else {
        sprintf(str, "%5d", (int)gRadio.freq_khz);
        UI_DisplayFrequency(str, 14, 2, true);
        UI_PrintStringSmall("kHz", 98, 127, 3);
    }

    // 3. 辅助参数 (STEP & BFO)
    if (gRadio.mode >= MODE_LSB) {
        sprintf(str, "STP:%uk  BFO:%+dHz", STEP_TABLE[gRadio.step_idx], gRadio.bfo_offset);
    } else {
        sprintf(str, "STEP: %ukHz", STEP_TABLE[gRadio.step_idx]);
    }
    UI_PrintStringSmall(str, 14, 127, 4);

    // 4. 底部专业 S 表
    Draw_Pro_SMeter(14, 48);

    // 使用官方 ST7565 全屏刷新函数
    ST7565_BlitFullScreen();
}

/* --- 对外业务接口 --- */
void FM_ToggleRadio(void) {
    gRadio.active = !gRadio.active;
    if (gRadio.active) {
        // 静音原机对讲通道，防止杂音干扰短波
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_AF_MUTE, true);
        Apply_Mode(gRadio.mode);
        Apply_Freq();
    } else {
        uint8_t pwr_down[] = {0x11};
        SI_Write(pwr_down, 1); // SI4732 芯片关机休眠
        // 解除原机静音，恢复正常 UV 通信
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_AF_MUTE, false);
    }
}

bool FM_IsActive(void) {
    return gRadio.active;
}

void FM_ProcessKey(KEY_Code_t key) {
    if (!gRadio.active) return;
    switch (key) {
        case KEY_5:
            gRadio.mode = (si_mode_t)((gRadio.mode + 1) % MODE_MAX);
            Apply_Mode(gRadio.mode);
            break;
        case KEY_0:
            gRadio.att_level = (gRadio.att_level + 1) % 3;
            Apply_ATT();
            break;
        case KEY_1:
            gRadio.step_idx = (gRadio.step_idx + 1) % STEP_MAX;
            break;
        case KEY_2:
            gRadio.bw_idx = (gRadio.bw_idx + 1) % BW_MAX;
            Apply_BW();
            break;
        case KEY_UP:
            gRadio.freq_khz += STEP_TABLE[gRadio.step_idx];
            Apply_Freq();
            break;
        case KEY_DOWN:
            if (gRadio.freq_khz > STEP_TABLE[gRadio.step_idx]) {
                gRadio.freq_khz -= STEP_TABLE[gRadio.step_idx];
            }
            Apply_Freq();
            break;
        case KEY_SIDE1:
            if (gRadio.mode >= MODE_LSB) Apply_BFO(+50);
            break;
        case KEY_SIDE2:
            if (gRadio.mode >= MODE_LSB) Apply_BFO(-50);
            break;
        case KEY_EXIT:
            FM_ToggleRadio();
            break;
        default:
            break;
    }
}

void FM_UpdateTask(void) {
    if (!gRadio.active) return;
    Update_Signal();
    UI_Render_Dashboard();
}
