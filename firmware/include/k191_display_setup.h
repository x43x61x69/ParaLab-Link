#pragma once
// Based on LILYGO T-QT Setup211: GC9107 panel using GC9A01 init.
#define USER_SETUP_LOADED
#define USE_HSPI_PORT
#define GC9A01_DRIVER
#define TFT_WIDTH 128
#define TFT_HEIGHT 128
#define CGRAM_OFFSET
#define TFT_BL 10
#define TFT_BACKLIGHT_ON 0
#define TFT_MISO -1
#define TFT_MOSI 2
#define TFT_SCLK 3
#define TFT_CS 5
#define TFT_DC 6
#define TFT_RST 1
#define LOAD_GLCD
#define LOAD_FONT2
#define SPI_FREQUENCY 40000000
#define DISABLE_ALL_LIBRARY_WARNINGS
