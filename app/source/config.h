#ifndef CONFIG_H
#define CONFIG_H

#define APP_DIR "/dev_hdd0/game/ANIMEPS30/USRDIR/"

/* Raise the code with each release: it's what the update check compares. */
#define APP_VERSION_CODE 2
#define APP_VERSION_NAME "0.1.1"

#define SCREEN_W 848.0f
#define SCREEN_H 512.0f

#define COLOR_BACKGROUND 0x0E0E12FF
#define COLOR_SURFACE 0x1C1C24FF
#define COLOR_ACCENT 0x8B7CF6FF
#define COLOR_TEXT 0xFFFFFFFF
#define COLOR_MUTED 0xA0A0A8FF

void app_log(const char *fmt, ...);

#endif
