TARGET = firmware

# -------- 工具链配置 --------
CROSS_COMPILE ?= arm-none-eabi-
CC             = $(CROSS_COMPILE)gcc
OBJCOPY        = $(CROSS_COMPILE)objcopy
SIZE           = $(CROSS_COMPILE)size

# -------- 功能宏开关配置 (针对 SI4732 定制与体积优化) --------
# 1. 保留核心 UV 对讲，关闭无用耗空间模块
ENABLE_AIRCOPY              := 0
ENABLE_MDC1200              := 0
ENABLE_SPECTRUM             := 0
ENABLE_ALARM                := 0
ENABLE_TX1750               := 0
ENABLE_PWRON_PASSWORD       := 0

# 2. 硬件与基本功能支持
ENABLE_BATTERY_CHARGING     := 1
ENABLE_FLASHLIGHT           := 1
ENABLE_VOX                  := 1
ENABLE_ROGER                := 1
ENABLE_BIG_FREQ             := 1
ENABLE_SMALL_BOLD           := 1

# -------- 编译与链接参数优化 (极致压缩体积) --------
CFLAGS  = -Os -Wall -Wextra -mcpu=cortex-m0 -mthumb -flto -ffunction-sections -fdata-sections
CFLAGS += -DENABLE_AIRCOPY=$(ENABLE_AIRCOPY)
CFLAGS += -DENABLE_MDC1200=$(ENABLE_MDC1200)
CFLAGS += -DENABLE_SPECTRUM=$(ENABLE_SPECTRUM)
CFLAGS += -DENABLE_ALARM=$(ENABLE_ALARM)
CFLAGS += -DENABLE_TX1750=$(ENABLE_TX1750)
CFLAGS += -DENABLE_PWRON_PASSWORD=$(ENABLE_PWRON_PASSWORD)
CFLAGS += -DENABLE_BATTERY_CHARGING=$(ENABLE_BATTERY_CHARGING)
CFLAGS += -DENABLE_FLASHLIGHT=$(ENABLE_FLASHLIGHT)
CFLAGS += -DENABLE_VOX=$(ENABLE_VOX)
CFLAGS += -DENABLE_ROGER=$(ENABLE_ROGER)
CFLAGS += -DENABLE_BIG_FREQ=$(ENABLE_BIG_FREQ)
CFLAGS += -DENABLE_SMALL_BOLD=$(ENABLE_SMALL_BOLD)

LDFLAGS = -mcpu=cortex-m0 -mthumb -flto -Wl,--gc-sections -specs=nano.specs -specs=nosys.specs -T project.ld

# -------- 编译依赖源文件 --------
SRCS = \
	main.c \
	app/action.c \
	app/app.c \
	app/dtmf.c \
	app/fm.c \
	app/generic.c \
	app/main.c \
	app/menu.c \
	app/scanner.c \
	app/uart.c \
	bsp/dp32g030/gpio.c \
	bsp/dp32g030/syscon.c \
	driver/backlight.c \
	driver/battery.c \
	driver/bk4819.c \
	driver/eeprom.c \
	driver/keyboard.c \
	driver/spi.c \
	driver/st7565.c \
	driver/system.c \
	driver/systick.c \
	driver/uart.c \
	helper/battery.c \
	helper/boot.c \
	misc.c \
	ui/battery.c \
	ui/fm.c \
	ui/helper.c \
	ui/inputbox.c \
	ui/lock.c \
	ui/main.c \
	ui/menu.c \
	ui/scanner.c \
	ui/status.c \
	ui/ui.c

OBJS = $(SRCS:.c=.o)

# -------- 构建规则 --------
all: $(TARGET).bin $(TARGET).packed.bin

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# 打包成官方升级工具可认的带版本头格式
$(TARGET).packed.bin: $(TARGET).bin
	-python3 ./version.py $< $@ || cp $< $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).packed.bin
