/* The gateway sources' Kconfig values, for the differential test build. */
#ifndef SDDIFF_AUTOCONF_H
#define SDDIFF_AUTOCONF_H
#define CONFIG_SD 1
#define CONFIG_SD_BUS_SDIO 1
#define CONFIG_SD_SDIO_CLK SDDIFF_CLK
#define CONFIG_SD_SDIO_CMD SDDIFF_CMD
#define CONFIG_SD_SDIO_D0 SDDIFF_D0
#define CONFIG_SD_SDIO_PIO -1
#define CONFIG_SD_SDIO_KHZ 25000
#define CONFIG_SD_DETECT SDDIFF_DETECT
#define CONFIG_SD_DETECT_LEVEL 1   /* Adafruit SDIO breakout: high = card in */
#define CONFIG_IO_PINS 0
#endif
