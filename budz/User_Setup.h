// TFT_eSPI config for Waveshare ESP32-S3-Touch-LCD-1.28 (GC9A01 round panel)
// Place in sketch folder — this file takes priority over the library's User_Setup_Select.h

#define USER_SETUP_LOADED 1

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MISO  12
#define TFT_MOSI  11
#define TFT_SCLK  10
#define TFT_CS     9
#define TFT_DC     8
#define TFT_RST   14
#define TFT_BL     2   // backlight — must be driven HIGH manually in setup()
#define TFT_BACKLIGHT_ON HIGH

// Do NOT define TOUCH_CS: touch is CST816S on I2C, not SPI

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY      80000000
#define SPI_READ_FREQUENCY  6000000
