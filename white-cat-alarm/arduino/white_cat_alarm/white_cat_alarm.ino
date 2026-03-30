/**
 * White Cat Alarm Clock
 * =====================
 * An Arduino-based smart alarm clock featuring:
 *  - TM1637 4-digit clock display
 *  - ST7735 TFT screen with BMP image animations (idle cat, blinking, reactions)
 *  - DFPlayer Mini for audio playback
 *  - Capacitive touch sensor for alarm dismissal & short/long press interactions
 *  - Vibration motor feedback
 *  - Wi-Fi connectivity (Arduino UNO R4 WiFi)
 *  - NTP time synchronization via RTC
 *  - Non-blocking weather data fetching (Google Apps Script endpoint)
 *  - Web server for alarm time configuration
 *
 * Hardware: Arduino UNO R4 WiFi
 *
 * BMP images on SD card (128×160 px):
 *  01.bmp  Cat eyes open (idle)
 *  02.bmp  Cat eyes closed (blink)
 *  03.bmp  Short press reaction
 *  04.bmp  Long press reaction
 *  05.bmp  Alarm dismissed reaction
 *  06.bmp  Rainy weather icon
 *  07.bmp  Cold weather icon
 *  08.bmp  Hot weather icon
 *  09.bmp  Windy weather icon
 *  10.bmp  Clear / default weather icon
 *
 * Audio files on DFPlayer SD card:
 *  001.mp3  Short press sound
 *  002.mp3  Long press sound / alarm loop
 *
 * See README.md for full wiring diagram and setup instructions.
 *
 * Author: [Your Name]
 * License: MIT
 */

#include <TM1637Display.h>
#include "RTC.h"
#include <WiFiS3.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <DFRobotDFPlayerMini.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"


// ==========================================
// Weather Data
// ==========================================
float        weatherMinT = 0;
float        weatherMaxT = 0;
int          weatherPop  = 0;   // Precipitation probability (%)
String       weatherWx   = "";  // Weather description
String       weatherWS   = "";  // Wind speed level (e.g. "L3Brez")
unsigned long lastWeatherUpdate  = 0;
const unsigned long WEATHER_INTERVAL = 3600000UL;  // Refresh every 1 hour


// ==========================================
// Non-Blocking Weather Fetch State Machine
// ==========================================
enum WeatherState {
  WS_IDLE,
  WS_CONNECTING,
  WS_READING_HEADER,
  WS_READING_BODY,
  WS_DONE
};
WeatherState  weatherState        = WS_IDLE;
WiFiSSLClient weatherClient;
String        weatherHost         = "";
String        weatherPath         = "";
String        weatherRedirectUrl  = "";
int           weatherStatusCode   = 0;
int           weatherRedirectCount = 0;
String        weatherRawData      = "";
unsigned long weatherReadTimeout  = 0;
String        weatherHeaderLine   = "";  // Accumulates current HTTP header line


// ==========================================
// Alarm State
// ==========================================
int    alarmHour        = 7;
int    alarmMinute      = 30;
String alarmTimeString  = "07:30";
bool   isAlarmRinging   = false;
bool   alarmDismissed   = false;

// Vibration motor (non-blocking toggle)
unsigned long previousMotorMillis = 0;
bool          motorState          = false;


// ==========================================
// Touch Sensor State
// ==========================================
bool          hasTriggeredLong = false;
bool          lastTouchState   = LOW;
unsigned long pressStartTime   = 0;


// ==========================================
// UI State
// ==========================================
unsigned long uiTimer               = 0;
bool          isShowingSpecialImage = false;
unsigned long nextBlinkTime         = 0;


// ==========================================
// Post-Alarm Weather Display State Machine
// ==========================================
enum PostAlarmState {
  PA_IDLE,
  PA_SHOW_DISMISS,  // Show 05.bmp, then wait 3 s
  PA_SHOW_WEATHER   // Show weather icon (handed off to isShowingSpecialImage)
};
PostAlarmState postAlarmState = PA_IDLE;
unsigned long  postAlarmTimer = 0;


// ==========================================
// Object Instances
// ==========================================
WiFiServer          server(80);
WiFiUDP             ntpUDP;
NTPClient           timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET);
TM1637Display       display(CLK, DIO);
DFRobotDFPlayerMini myDFPlayer;
Adafruit_ST7735     tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);


// ==========================================
// BMP Helper Functions
// ==========================================

uint16_t read16(File &f) {
  uint16_t result;
  f.read((uint8_t*)&result, 2);
  return result;
}

uint32_t read32(File &f) {
  uint32_t result;
  f.read((uint8_t*)&result, 4);
  return result;
}

/**
 * Draw a 24-bit BMP file from SD card onto the TFT display.
 * Assumes image width matches TFT width (128 px for ST7735 128×160).
 */
void drawBMP(const char* filename) {
  File bmpFile = SD.open(filename);
  if (!bmpFile) {
    Serial.print("File not found: ");
    Serial.println(filename);
    return;
  }

  bmpFile.seek(10);
  uint32_t dataOffset = read32(bmpFile);

  bmpFile.seek(18);
  uint32_t imgWidth  = read32(bmpFile);
  uint32_t imgHeight = read32(bmpFile);
  uint32_t rowSize   = (imgWidth * 3 + 3) & ~3;  // 4-byte aligned row

  uint16_t lineBuffer[160];
  for (int y = imgHeight - 1; y >= 0; y--) {
    bmpFile.seek(dataOffset + (uint32_t)y * rowSize);
    for (int x = 0; x < (int)imgWidth; x++) {
      uint8_t b = bmpFile.read();
      uint8_t g = bmpFile.read();
      uint8_t r = bmpFile.read();
      lineBuffer[x] = tft.color565(r, g, b);
    }
    int screenRow = imgHeight - 1 - y;
    tft.startWrite();
    tft.setAddrWindow(0, screenRow, imgWidth - 1, screenRow);
    tft.writePixels(lineBuffer, imgWidth);
    tft.endWrite();
  }
  bmpFile.close();
}

/**
 * Display a feedback/reaction image for 3 seconds,
 * then automatically return to the idle image.
 */
void showFeedbackImage(const char* filename) {
  drawBMP(filename);
  uiTimer = millis();
  isShowingSpecialImage = true;
  Serial.print("Showing feedback image: ");
  Serial.println(filename);
}

/**
 * Choose the appropriate weather icon based on current weather data.
 */
String getWeatherImageName() {
  if (weatherPop > 50)  return "06.bmp";  // Rainy
  if (weatherMinT < 15) return "07.bmp";  // Cold
  if (weatherMaxT > 25) return "08.bmp";  // Hot

  int windLevel = 0;
  if (weatherWS.startsWith("L")) {
    windLevel = weatherWS.substring(1).toInt();
  }
  if (windLevel > 3) return "09.bmp";     // Windy

  return "10.bmp";                        // Clear / default
}


// ==========================================
// Non-Blocking Weather Fetch State Machine
// ==========================================

/**
 * Kick off a new weather fetch cycle.
 * Connects to the Google Apps Script endpoint which returns CSV:
 *   minTemp,maxTemp,precipProb,weatherDesc,windLevel
 */
void startWeatherFetch() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (isAlarmRinging) return;  // Do not fetch while alarm is active

  weatherHost          = WEATHER_HOST;
  weatherPath          = WEATHER_PATH;
  weatherRedirectCount = 0;
  weatherRawData       = "";
  weatherState         = WS_CONNECTING;
  Serial.println("Starting weather fetch...");
}

/**
 * Advance the weather fetch state machine by one step.
 * Call every loop() iteration for non-blocking behavior.
 */
void processWeatherFetch() {
  if (weatherState == WS_IDLE) return;

  // Abort immediately if alarm starts ringing
  if (isAlarmRinging) {
    weatherClient.stop();
    weatherState = WS_IDLE;
    return;
  }

  switch (weatherState) {

    case WS_CONNECTING:
      Serial.print("Connecting to: ");
      Serial.println(weatherHost);
      if (!weatherClient.connect(weatherHost.c_str(), 443)) {
        Serial.println("Connection failed");
        weatherState = WS_IDLE;
        return;
      }
      weatherClient.println("GET " + weatherPath + " HTTP/1.1");
      weatherClient.println("Host: " + weatherHost);
      weatherClient.println("User-Agent: ArduinoR4");
      weatherClient.println("Connection: close");
      weatherClient.println();
      weatherStatusCode  = 0;
      weatherRedirectUrl = "";
      weatherState       = WS_READING_HEADER;
      weatherReadTimeout = millis();
      break;

    case WS_READING_HEADER: {
      int charsRead = 0;
      while (weatherClient.available() && charsRead < 64) {
        char c = weatherClient.read();
        weatherReadTimeout = millis();
        charsRead++;

        if (c == '\n') {
          weatherHeaderLine.trim();

          if (weatherHeaderLine.startsWith("HTTP/1.") && weatherStatusCode == 0) {
            weatherStatusCode = weatherHeaderLine.substring(9, 12).toInt();
            Serial.print("HTTP Status: ");
            Serial.println(weatherStatusCode);
          }
          if (weatherHeaderLine.startsWith("Location: ") ||
              weatherHeaderLine.startsWith("location: ")) {
            weatherRedirectUrl = weatherHeaderLine.substring(10);
            weatherRedirectUrl.trim();
            Serial.print("Redirect to: ");
            Serial.println(weatherRedirectUrl);
          }
          if (weatherHeaderLine.length() == 0) {
            if ((weatherStatusCode == 301 || weatherStatusCode == 302 ||
                 weatherStatusCode == 303) &&
                weatherRedirectUrl != "" && weatherRedirectCount < 5) {
              weatherClient.stop();
              weatherRedirectCount++;
              int hostStart = weatherRedirectUrl.indexOf("://") + 3;
              int pathStart = weatherRedirectUrl.indexOf("/", hostStart);
              weatherHost   = weatherRedirectUrl.substring(hostStart, pathStart);
              weatherPath   = weatherRedirectUrl.substring(pathStart);
              weatherHeaderLine = "";
              weatherState  = WS_CONNECTING;
            } else if (weatherStatusCode == 200) {
              weatherRawData    = "";
              weatherReadTimeout = millis();
              weatherHeaderLine = "";
              weatherState      = WS_READING_BODY;
            } else {
              Serial.println("Weather fetch failed (unexpected status code)");
              weatherClient.stop();
              weatherHeaderLine = "";
              weatherState      = WS_IDLE;
            }
            break;
          }
          weatherHeaderLine = "";
        } else if (c != '\r') {
          weatherHeaderLine += c;
        }
      }

      if (weatherState == WS_READING_HEADER) {
        if (!weatherClient.connected() && !weatherClient.available()) {
          Serial.println("Connection dropped during header read");
          weatherHeaderLine = "";
          weatherState = WS_IDLE;
        } else if (millis() - weatherReadTimeout > 10000) {
          Serial.println("Header read timeout");
          weatherClient.stop();
          weatherHeaderLine = "";
          weatherState = WS_IDLE;
        }
      }
      break;
    }

    case WS_READING_BODY:
      if (weatherClient.available()) {
        weatherRawData += (char)weatherClient.read();
        weatherReadTimeout = millis();
      } else if (!weatherClient.connected()) {
        weatherState = WS_DONE;
      } else if (millis() - weatherReadTimeout > 2000) {
        weatherState = WS_DONE;
      }
      break;

    case WS_DONE: {
      weatherClient.stop();
      weatherRawData.trim();
      Serial.print("Received data: ");
      Serial.println(weatherRawData);

      // Expected format: "minT,maxT,pop,description,windLevel"
      int f  = weatherRawData.indexOf(',');
      int s  = weatherRawData.indexOf(',', f + 1);
      int t  = weatherRawData.indexOf(',', s + 1);
      int fo = weatherRawData.indexOf(',', t + 1);

      if (f > 0 && s > 0 && t > 0) {
        weatherMinT = weatherRawData.substring(0, f).toFloat();
        weatherMaxT = weatherRawData.substring(f + 1, s).toFloat();
        weatherPop  = weatherRawData.substring(s + 1, t).toInt();
        weatherWx   = (fo > 0) ? weatherRawData.substring(t + 1, fo)
                                : weatherRawData.substring(t + 1);
        weatherWS   = (fo > 0) ? weatherRawData.substring(fo + 1) : "L1";
        weatherWx.trim();
        weatherWS.trim();
        Serial.println("Weather data parsed successfully");
      } else {
        Serial.println("Weather data parse error");
      }

      lastWeatherUpdate = millis();
      weatherState = WS_IDLE;
      break;
    }

    default:
      break;
  }
}


// ==========================================
// Web Server — Alarm Configuration Page
// ==========================================

/**
 * Handle a single incoming HTTP request.
 * Serves an HTML page to set the alarm time.
 * Query parameter: /?time=HH:MM
 */
void handleWebServer() {
  WiFiClient client = server.available();
  if (!client) return;

  String        currentLine = "";
  unsigned long timeout     = millis();

  while (client.connected() && millis() - timeout < 2000) {
    if (client.available()) {
      char c = client.read();
      timeout = millis();

      if (c == '\n') {
        if (currentLine.startsWith("GET /?time=")) {
          int    timeStart = currentLine.indexOf("=") + 1;
          int    timeEnd   = currentLine.indexOf(" HTTP");
          String rawTime   = currentLine.substring(timeStart, timeEnd);
          rawTime.replace("%3A", ":");
          if (rawTime.length() >= 5) {
            alarmHour       = rawTime.substring(0, 2).toInt();
            alarmMinute     = rawTime.substring(3, 5).toInt();
            alarmTimeString = rawTime;
            isAlarmRinging  = false;
            alarmDismissed  = false;
          }
        }

        if (currentLine.length() == 0) {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-type:text/html; charset=UTF-8");
          client.println("Connection: close\n");

          client.println("<!DOCTYPE html><html><head>");
          client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
          client.println("<title>White Cat Alarm</title><style>");
          client.println("body{font-family:'Helvetica Neue',sans-serif;text-align:center;"
                         "margin-top:15vw;background:#FAFAFA;color:#555}");
          client.println("h1{color:#FFA07A;font-size:28px;letter-spacing:2px}");
          client.println("p{font-size:16px;margin-bottom:30px}");
          client.println("input[type='time']{font-size:32px;padding:15px;border-radius:12px;"
                         "border:2px solid #EEE;text-align:center;color:#333;background:#FFF}");
          client.println("input[type='submit']{font-size:18px;padding:12px 40px;margin-top:30px;"
                         "border-radius:25px;background:#FFA07A;color:#fff;border:none;"
                         "cursor:pointer;font-weight:bold}");
          client.println(".status{margin-top:20px;font-weight:bold;color:#777}"
                         ".info{margin-top:10px;font-size:14px;color:#AAA}");
          client.println("</style></head><body>");

          client.println("<h1>🐾 White Cat Alarm</h1>");
          client.println("<p>Set your wake-up time and the little white cat will get you up!</p>");
          client.println("<form action='/' method='GET'>");
          client.print("<input type='time' name='time' value='");
          client.print(alarmTimeString);
          client.println("' required><br><input type='submit' value='Set Alarm'></form>");

          client.print("<div class='status'>💤 Alarm set for: ");
          client.print(alarmTimeString);
          client.println("</div>");

          client.print("<div class='info'>🌡️ ");
          client.print((int)weatherMinT);
          client.print("~");
          client.print((int)weatherMaxT);
          client.print("°C &nbsp; 🌧️ ");
          client.print(weatherPop);
          client.print("% &nbsp; ");
          client.print(weatherWx);
          client.print(" &nbsp; 🚩 Wind: ");
          client.print(weatherWS);
          client.println("</div>");

          RTCTime ct;
          RTC.getTime(ct);
          int hh = ct.getHour(), mm = ct.getMinutes();
          client.print("<div class='info'>Device time: ");
          if (hh < 10) client.print("0");
          client.print(hh);
          client.print(":");
          if (mm < 10) client.print("0");
          client.print(mm);
          client.println("</div></body></html>");
          break;
        }
        currentLine = "";
      } else if (c != '\r') {
        currentLine += c;
      }
    }
  }
  client.stop();
}


// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  // Pull all SPI CS pins HIGH first to prevent bus conflicts during Serial1 init
  pinMode(SD_CS,     OUTPUT); digitalWrite(SD_CS,     HIGH);
  pinMode(TFT_CS,    OUTPUT); digitalWrite(TFT_CS,    HIGH);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(MOTOR_PIN, OUTPUT);

  // Wait for DFPlayer to power up (3 s)
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {}

  // Initialize DFPlayer first — before SD or TFT to avoid SPI interference
  int attempts = 0;
  while (!myDFPlayer.begin(Serial1) && attempts < 10) {
    unsigned long tw = millis();
    while (millis() - tw < 500) {}
    attempts++;
  }
  if (attempts >= 10) {
    Serial.println("DFPlayer initialization failed!");
  } else {
    Serial.println("DFPlayer OK");
    myDFPlayer.volume(DFPLAYER_VOLUME);
  }

  // Initialize TFT display
  Serial.println("Initializing TFT...");
  tft.initR(INITR_BLACKTAB);
  tft.setSPISpeed(8000000);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("TFT OK");

  // Initialize SD card and show splash image
  Serial.println("Initializing SD...");
  if (SD.begin(SD_CS)) {
    Serial.println("SD OK — loading splash image...");
    drawBMP("01.bmp");
    Serial.println("Image loaded");
    digitalWrite(SD_CS, HIGH);
  } else {
    Serial.println("SD initialization failed!");
  }

  // Initialize 7-segment display
  display.setBrightness(7);
  Serial.println("Display OK");

  // Connect to Wi-Fi (timeout: 30 s)
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int wIdx = 0;
  while (WiFi.status() != WL_CONNECTED && wIdx < 60) {
    unsigned long tw = millis();
    while (millis() - tw < 500) {}
    Serial.print(".");
    wIdx++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");

    // Wait for DHCP IP assignment (timeout: 10 s)
    Serial.print("Waiting for IP");
    unsigned long ipWait = millis();
    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - ipWait < 10000) {
      unsigned long tw = millis();
      while (millis() - tw < 500) {}
      Serial.print(".");
    }
    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    server.begin();
    timeClient.begin();

    if (timeClient.update() || timeClient.forceUpdate()) {
      RTCTime startTime;
      startTime.setUnixTime(timeClient.getEpochTime());
      RTC.begin();
      RTC.setTime(startTime);
      Serial.println("RTC synchronized via NTP");
    }

    startWeatherFetch();
  } else {
    Serial.println("\nWi-Fi connection failed");
    Serial.print("Status code: ");
    Serial.println(WiFi.status());
    RTC.begin();
  }

  // Test audio playback
  myDFPlayer.play(1);
  Serial.println("DFPlayer play command sent");

  Serial.println("=== Setup complete, entering loop ===");
  nextBlinkTime = millis() + random(3000, 5001);
}


// ==========================================
// Main Loop (fully non-blocking)
// ==========================================
void loop() {

  // --- Clock display ---
  RTCTime currentTime;
  RTC.getTime(currentTime);
  int h = currentTime.getHour();
  int m = currentTime.getMinutes();
  display.showNumberDecEx(
    h * 100 + m,
    (currentTime.getSeconds() % 2 == 0) ? 0b01000000 : 0,
    true
  );


  // --- A. Random eye-blink animation (non-blocking) ---
  static bool          blinkEyeOpen = true;
  static unsigned long blinkOpenTime = 0;

  if (!isShowingSpecialImage && !isAlarmRinging) {
    unsigned long now_ms = millis();
    if (blinkEyeOpen && now_ms >= nextBlinkTime) {
      drawBMP("02.bmp");
      blinkEyeOpen  = false;
      blinkOpenTime = now_ms + 50;
    } else if (!blinkEyeOpen && now_ms >= blinkOpenTime) {
      drawBMP("01.bmp");
      blinkEyeOpen  = true;
      nextBlinkTime = now_ms + random(3000, 5001);
    }
  }


  // --- B. Advance weather fetch state machine ---
  processWeatherFetch();

  if (weatherState == WS_IDLE && !isAlarmRinging &&
      millis() - lastWeatherUpdate >= WEATHER_INTERVAL) {
    startWeatherFetch();
  }


  // --- C. Auto-return to idle image after 3 s ---
  if (isShowingSpecialImage && (millis() - uiTimer > 3000)) {
    drawBMP("01.bmp");
    isShowingSpecialImage = false;
    blinkEyeOpen  = true;
    nextBlinkTime = millis() + 3000;
  }


  // --- D. Alarm trigger logic ---
  if (h == alarmHour && m == alarmMinute) {
    if (!alarmDismissed && !isAlarmRinging) {
      isAlarmRinging    = true;
      lastWeatherUpdate = millis();

      if (weatherState != WS_IDLE) {
        weatherClient.stop();
        weatherState = WS_IDLE;
      }
      myDFPlayer.loop(1);
      Serial.println("Alarm triggered");
    }
  } else {
    alarmDismissed = false;
  }


  // --- E. Vibration motor (non-blocking 400 ms toggle) ---
  if (isAlarmRinging) {
    unsigned long curM = millis();
    if (curM - previousMotorMillis >= 400) {
      previousMotorMillis = curM;
      motorState = !motorState;
      analogWrite(MOTOR_PIN, motorState ? 255 : 0);
    }
  } else {
    analogWrite(MOTOR_PIN, 0);
  }


  // --- F. Post-alarm weather display state machine ---
  if (postAlarmState == PA_SHOW_DISMISS) {
    if (millis() - postAlarmTimer >= 3000) {
      String nextImg = getWeatherImageName();
      showFeedbackImage(nextImg.c_str());
      Serial.println("Showing weather icon: " + nextImg);
      postAlarmState = PA_IDLE;
    }
  }


  // --- G. Touch sensor (short press / long press detection) ---
  bool          currentTouch = digitalRead(TOUCH_PIN);
  unsigned long now          = millis();

  if (currentTouch == HIGH) {
    if (lastTouchState == LOW) {
      pressStartTime   = now;
      hasTriggeredLong = false;
    }

    if (!isAlarmRinging && !isShowingSpecialImage && !hasTriggeredLong) {
      if (now - pressStartTime > SHORT_PRESS_MAX) {
        showFeedbackImage("04.bmp");
        myDFPlayer.play(2);
        hasTriggeredLong = true;
        Serial.println("Long press: showing 04.bmp, playing track 2");
      }
    }
  } else if (currentTouch == LOW && lastTouchState == HIGH) {
    unsigned long duration = now - pressStartTime;

    if (isAlarmRinging) {
      isAlarmRinging = false;
      alarmDismissed = true;
      analogWrite(MOTOR_PIN, 0);
      myDFPlayer.pause();

      showFeedbackImage("05.bmp");
      Serial.println("Alarm dismissed: showing 05.bmp");
      postAlarmState = PA_SHOW_DISMISS;
      postAlarmTimer = millis();

    } else {
      if (!hasTriggeredLong && duration < SHORT_PRESS_MAX) {
        showFeedbackImage("03.bmp");
        myDFPlayer.play(1);
        Serial.println("Short press: showing 03.bmp, playing track 1");
      }
    }
    hasTriggeredLong = false;
  }
  lastTouchState = currentTouch;


  // --- H. Handle web server requests ---
  handleWebServer();
}
