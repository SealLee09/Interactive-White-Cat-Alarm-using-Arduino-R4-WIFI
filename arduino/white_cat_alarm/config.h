/**
 * config.h — User Configuration
 * ==============================
 * Edit this file before uploading to your Arduino.
 * All user-specific settings are centralized here.
 */

#pragma once

// ==========================================
// Wi-Fi
// ==========================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"


// ==========================================
// NTP / Timezone
// ==========================================
// UTC offset in seconds.
// UTC+8 (Taiwan / China Standard Time) = 8 * 3600 = 28800
#define NTP_OFFSET  (8 * 3600)


// ==========================================
// Weather Endpoint (Google Apps Script)
// ==========================================
// After deploying weather_endpoint.gs as a web app, paste the
// script.google.com host and path here.
// Example path:
//   /macros/s/AKfycb.../exec
#define WEATHER_HOST "script.google.com"
#define WEATHER_PATH "/macros/s/YOUR_DEPLOYMENT_ID/exec"


// ==========================================
// Pin Definitions
// ==========================================
#define CLK        2   // TM1637 clock
#define DIO        3   // TM1637 data
#define TOUCH_PIN  4   // Capacitive touch sensor (HIGH = touched)
#define SD_CS      5   // SD card chip select
#define MOTOR_PIN  6   // Vibration motor (PWM-capable pin)
#define TFT_RST    8   // ST7735 reset
#define TFT_DC     9   // ST7735 data/command
#define TFT_CS    10   // ST7735 chip select


// ==========================================
// DFPlayer Mini
// ==========================================
// Volume: 0 (mute) – 30 (max)
#define DFPLAYER_VOLUME  25


// ==========================================
// Touch Timing
// ==========================================
// Presses shorter than this (ms) are treated as short presses.
// Presses longer are treated as long presses.
#define SHORT_PRESS_MAX  500
