#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "ThingSpeak.h"
#include <time.h> 
#include <ArduinoJson.h> // Hivatalos ipari JSON feldolgozó könyvtár

// ==============================================================================
// --- FELHASZNÁLÓI BEÁLLÍTÁSOK (KONFIGURÁCIÓ) ---
// ==============================================================================
// WI-FI HÁLÓZATI BEÁLLÍTÁSOK
const char* WIFI_SSID     = "xxxxxxxxxxxx";
const char* WIFI_PASSWORD = "xxxxxxxxxxxx";

// THINGSPEAK BEÁLLÍTÁSOK
unsigned long myChannelNumber = xxxxxxxxxx;
const char* myReadAPIKey      = "xxxxxxxxxxx";

// METEOROLÓGIAI KÜSZÖBÉRTÉKEK (A TRÉNING ALAPJÁN FINOMHANGOLHATÓ)
const float STORM_THRESHOLD         = -1.8;   // Viharjelzés küszöbérték (hPa / 5 perc) -> pl. -4.20
const float BAD_WEATHER_THRESHOLD   = -1.33;   // Időjárás-romlás küszöb (hPa / 60 perc)
const float SUNNY_CLEAR_THRESHOLD   = 0.50;    // Tiszta/napos idő küszöb (hPa / 60 perc)
const float SLOW_IMPROV_THRESHOLD   = 0.20;    // Lassú javulás küszöb (hPa / 60 perc)
const float SEASONAL_OFFSET         = -0.43;    // Szezonális eltolás mértéke
const float WIND_MULTIPLIER         = 0.3;    // V VÉRMEZŐRE OPTIMALIZÁLT SZÉLIRÁNY-SZORZÓ V

// ABSZOLÚT NYOMÁSI ZÓNÁK (hPa)
const float PRESSURE_EXTREME_HIGH   = 1030.3; // Extrém magas nyomás határ (Anticiklon)
const float PRESSURE_STANDARD_MID   = 1016.7; // Standard tengerszinti alapérték
const float PRESSURE_EXTREME_LOW    = 1003.8;  // Extrém alacsony nyomás határ (Ciklon)

// IDŐZÍTÉSEK ÉS MATEMATIKAI ABLAKOK
const unsigned long UPDATE_INTERVAL_MS     = 60000UL;  // ThingSpeak adatletöltési gyakoriság (60 mp)
const unsigned long WIND_CHECK_INTERVAL_MS = 900000UL; // Szélirány és szezon frissítése (15 perc)
const unsigned long SCREEN_1_DURATION_MS   = 7000UL;   // 1. képernyő (Alapadatok) láthatósága (7 mp)
const unsigned long SCREEN_2_DURATION_MS   = 4000UL;   // 2. és 3. képernyő (Előrejelzés) láthatósága (4 mp)
const int MA_WINDOW                        = 5;        // Szenzorzaj-szűrés mozgóátlag ablaka (elem)
// ==============================================================================

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient client;

// Global measurement variables
float inTemp = 0.0, outTemp = 0.0, pressure = 0.0;
float pressure12h = 0.0; // Tároló a 12 órával ezelőtti makro nyomásadatnak
float pressure6h = 0.0;  // ÚJ: Tároló a 6 órával ezelőtti Sager nyomásadatnak

// MATEMATIKAI JAVÍTÁS: 61 elem kell a 60 perces, 11 elem a 10 perces tiszta időkülönbséghez
float pressureHistory[61] = {0.0};
float pressureMA[61] = {0.0};
float tempInHistory[11] = {0.0};
float tempOutHistory[11] = {0.0};

String currentWindDir = "N"; 
bool isBarometricCrash = false;      

// Timers
unsigned long lastUpdateCheck = 0;
unsigned long lastScreenSwitch = 0;
unsigned long lastWindCheck = 0;     
int currentScreen = 1;
bool historyReady = false;
bool isSummer = true;

// Status variables for Boot Screen
String wifiStatusStr = "WAIT";
String openMeteoStr   = "WAIT";
String tsStatusStr   = "WAIT";

void drawBootScreen(String currentAction) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("--- SYSTEM START ---");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  
  display.setCursor(0, 16);
  display.print("WiFi Network: "); display.println(wifiStatusStr);
  display.setCursor(0, 26);
  display.print("Open-Meteo:   "); display.println(openMeteoStr);
  display.setCursor(0, 36);
  display.print("ThingSpeak:   "); display.println(tsStatusStr);
  
  display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
  display.setCursor(0, 53);
  display.print(">"); display.print(currentAction);
  display.display();
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiStatusStr = "OK";
    return;
  }
  wifiStatusStr = "NOK";
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 30) {
    delay(500);
    attempt++;
    drawBootScreen("WiFi connecting (" + String(attempt/2) + "s)");
  }
  if (WiFi.status() == WL_CONNECTED) wifiStatusStr = "OK";
  else wifiStatusStr = "NOK";
}

void syncInternetTime() {
  drawBootScreen("Syncing NTP Time...");
  configTime(1 * 3600, 0, "time.google.com", "pool.ntp.org");
  int retry = 0;
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 24 && retry < 20) {
    delay(500);
    now = time(nullptr);
    retry++;
  }
}

void updateSeason() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t != nullptr) {
    int month = t->tm_mon + 1;
    isSummer = (month >= 3 && month <= 9);
  } else {
    isSummer = true; 
  }
}

bool fetchWindDirection() {
  if (WiFi.status() != WL_CONNECTED) {
    openMeteoStr = "NOK (No Net)";
    return false;
  }
  
  HTTPClient http;
  http.begin("http://api.open-meteo.com/v1/forecast?latitude=47.4979&longitude=19.0402&current_weather=true"); 
  int httpCode = http.GET();
  bool success = false;
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      float wind_deg = doc["current_weather"]["winddirection"];
      
      if (337.5 <= wind_deg || wind_deg < 22.5)        currentWindDir = "N";
      else if (22.5 <= wind_deg && wind_deg < 67.5)    currentWindDir = "NE";
      else if (67.5 <= wind_deg && wind_deg < 112.5)   currentWindDir = "E";
      else if (112.5 <= wind_deg && wind_deg < 157.5)  currentWindDir = "SE";
      else if (157.5 <= wind_deg && wind_deg < 202.5)  currentWindDir = "S";
      else if (202.5 <= wind_deg && wind_deg < 247.5)  currentWindDir = "SW";
      else if (247.5 <= wind_deg && wind_deg < 292.5)  currentWindDir = "W";
      else                                             currentWindDir = "NW";
      
      openMeteoStr = "OK";
      success = true;
    } else {
      openMeteoStr = "JSON ERR";
    }
  } else {
    openMeteoStr = "HTTP " + String(httpCode);
  }
  http.end();
  return success;
}

char getTempTrendChar(float current, float past) {
  if (past == 0.0) return '-'; 
  float delta = current - past;
  if (delta >= 0.20) return '^';
  if (delta <= -0.20) return 'v';
  return '-';
}

char getPressTrendChar(float current, float past) {
  if (past == 0.0) return '-'; 
  float delta = current - past;
  if (delta >= 0.10) return '^';
  if (delta <= -0.10) return 'v';
  return '-';
}

bool fetchLatestData() {
  if (WiFi.status() != WL_CONNECTED) {
    tsStatusStr = "NOK (No Net)";
    return false;
  }
  
  bool indoorFound = false;
  bool outdoorFound = false;
  bool pressureFound = false;

  HTTPClient http;
  String url = "http://api.thingspeak.com/channels/" + String(myChannelNumber) + "/feeds.json?api_key=" + String(myReadAPIKey) + "&minutes=4";
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      JsonArray feeds = doc["feeds"].as<JsonArray>();
      int feedsCount = feeds.size();
      
      if (feedsCount > 0) {
        JsonObject last_feed = feeds[feedsCount - 1];
        
        if (!last_feed["field1"].isNull()) {
          float t1 = last_feed["field1"].as<float>();
          if (!isnan(t1) && t1 != 0.0) {
            inTemp = t1;
            indoorFound = true;
          }
        }
        
        if (!last_feed["field5"].isNull()) {
          float t5 = last_feed["field5"].as<float>();
          if (!isnan(t5) && t5 != 0.0) {
            outTemp = t5;
            outdoorFound = true;
          }
        }
        
        float sumPressures = 0.0;
        int validPressureCount = 0;
        
        for (JsonVariant f : feeds) {
          if (!f["field3"].isNull()) {
            float p = f["field3"].as<float>();
            if (!isnan(p) && p > 950.0 && p < 1050.0) {
              sumPressures += p;
              validPressureCount++;
            }
          }
        }
        
        if (validPressureCount > 0) {
          pressure = sumPressures / validPressureCount;
          pressureFound = true;
        }
      }
    }
  }
  http.end();

  time_t now = time(nullptr);
  struct tm* t = gmtime(&now); 
  if (t != nullptr && now > 8 * 3600 * 24) {
    char timeBuf[30];
    
    // --- 12 ÓRÁVAL EZELŐTTI ADAT (ZAMBRETTI) ---
    time_t time12hAgo = now - (12 * 3600);
    struct tm* t12 = gmtime(&time12hAgo);
    // JAVÍTVA: ISO 8601 formátum 'T' és 'Z' (UTC) jelöléssel a pontos ThingSpeak API lekéréshez
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", t12);

    String macroUrl = "http://api.thingspeak.com/channels/" + String(myChannelNumber) + "/fields/3.json?api_key=" + String(myReadAPIKey) + "&end=" + String(timeBuf) + "&results=1";
    http.begin(macroUrl);
    int macroHttpCode = http.GET();
    if (macroHttpCode == 200) {
      String macroPayload = http.getString();
      JsonDocument macroDoc;
      if (!deserializeJson(macroDoc, macroPayload)) {
        JsonArray macroFeeds = macroDoc["feeds"].as<JsonArray>();
        if (macroFeeds.size() > 0 && !macroFeeds[0]["field3"].isNull()) {
          float p12 = macroFeeds[0]["field3"].as<float>();
          if (!isnan(p12) && p12 > 950.0 && p12 < 1050.0) {
            pressure12h = p12;
          }
        }
      }
    }
    http.end();

    delay(200); // JAVÍTVA: Rövid hálózati stabilitási szünet, hogy a ThingSpeak ne dobja el a sorozatos kéréseket

    // --- ÚJ: 6 ÓRÁVAL EZELŐTTI ADAT (SAGER) ---
    time_t time6hAgo = now - (6 * 3600);
    struct tm* t6 = gmtime(&time6hAgo);
    // JAVÍTVA: Ugyanaz az ISO 8601 szabványos formátum
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", t6);

    String sagerUrl = "http://api.thingspeak.com/channels/" + String(myChannelNumber) + "/fields/3.json?api_key=" + String(myReadAPIKey) + "&end=" + String(timeBuf) + "&results=1";
    http.begin(sagerUrl);
    int sagerHttpCode = http.GET();
    if (sagerHttpCode == 200) {
      String sagerPayload = http.getString();
      JsonDocument sagerDoc;
      if (!deserializeJson(sagerDoc, sagerPayload)) {
        JsonArray sagerFeeds = sagerDoc["feeds"].as<JsonArray>();
        if (sagerFeeds.size() > 0 && !sagerFeeds[0]["field3"].isNull()) {
          float p6 = sagerFeeds[0]["field3"].as<float>();
          if (!isnan(p6) && p6 > 950.0 && p6 < 1050.0) {
            pressure6h = p6;
          }
        }
      }
    }
    http.end();
  }

  // 2. BIZTONSÁGI HÁLÓ (Fallback): Amelyik hiányzik, azt célzottan lekérjük egyedi kéréssel
  if (!indoorFound) {
    delay(1000); 
    float t1 = ThingSpeak.readFloatField(myChannelNumber, 1, myReadAPIKey);
    if (!isnan(t1) && t1 != 0.0) { inTemp = t1; indoorFound = true; }
  }
  
  if (!outdoorFound) {
    delay(1000);
    float t5 = ThingSpeak.readFloatField(myChannelNumber, 5, myReadAPIKey);
    if (!isnan(t5) && t5 != 0.0) { outTemp = t5; outdoorFound = true; }
  }
  
  if (!pressureFound) {
    delay(1000);
    float p3 = ThingSpeak.readFloatField(myChannelNumber, 3, myReadAPIKey);
    if (!isnan(p3) && p3 > 950.0 && p3 < 1050.0) { pressure = p3; pressureFound = true; }
  }

  if (indoorFound || outdoorFound || pressureFound) {
    tsStatusStr = "OK";
    return true;
  } else {
    tsStatusStr = "ERROR";
    return false;
  }
}

void updateLocalHistory() {
  for (int i = 0; i < 10; i++) {
    tempInHistory[i] = tempInHistory[i+1];
    tempOutHistory[i] = tempOutHistory[i+1];
  }
  tempInHistory[10] = inTemp;
  tempOutHistory[10] = outTemp;

  for (int i = 0; i < 60; i++) {
    pressureHistory[i] = pressureHistory[i+1];
    pressureMA[i] = pressureMA[i+1];
  }
  pressureHistory[60] = pressure;
  
  float sum = 0.0;
  int count = 0;
  for (int i = 61 - MA_WINDOW; i < 61; i++) {
    if (pressureHistory[i] > 950.0) { sum += pressureHistory[i]; count++; }
  }
  pressureMA[60] = (count > 0) ? sum / count : pressure;
  
  float shortTrend = pressureMA[60] - pressureMA[55];
  if (shortTrend <= STORM_THRESHOLD && pressureMA[55] > 950.0) {
    isBarometricCrash = true; 
  } else {
    isBarometricCrash = false;
  }
  
  int valid = 0;
  for (int i = 0; i < 61; i++) if (pressureHistory[i] > 950.0) valid++;
  if (valid >= MA_WINDOW) historyReady = true;
}

String getForecastText() {
  if (!historyReady || pressureMA[60] < 950.0) return "COLLECTING...";
  if (isBarometricCrash) return "STORM WARNING"; 
  
  float p = pressureMA[60];
  float raw_trend = pressureMA[60] - pressureMA[0]; 
  
  float wind_mod = 0.0;
  if (currentWindDir == "S" || currentWindDir == "SW") {
      wind_mod = 2.0;   
  } else if (currentWindDir == "SE" || currentWindDir == "W") {
      wind_mod = 0.5;   
  } else if (currentWindDir == "NW") {
      wind_mod = (raw_trend < 0) ? 0.6 : -0.2;
  } else if (currentWindDir == "E" || currentWindDir == "NE" || currentWindDir == "N") {
      wind_mod = -0.6;  
  }
  
  float trend = raw_trend - (wind_mod * WIND_MULTIPLIER);
  float seasonalFactor = isSummer ? -SEASONAL_OFFSET : SEASONAL_OFFSET; 
  
  if (pressure12h > 950.0) {
    float macro_trend = p - pressure12h;
    if (macro_trend <= -3.0 && trend <= BAD_WEATHER_THRESHOLD) return "CYCLONE / STORM";
    if (macro_trend >= 3.0 && trend <= BAD_WEATHER_THRESHOLD)  return "PASSING FRONT";
    if (macro_trend <= -3.0 && trend >= 0.5)                   return "SLOW IMPROV.";
  }
  
  if (trend <= STORM_THRESHOLD + seasonalFactor) return (p < 1005.0) ? "STORMY RAIN" : "RAIN/WEATHER";
  if (trend <= BAD_WEATHER_THRESHOLD) return "BAD WEATHER";
  if (trend >= SUNNY_CLEAR_THRESHOLD + seasonalFactor) return "SUNNY/CLEAR"; 
  if (trend >= SLOW_IMPROV_THRESHOLD) return "SLOW IMPROV.";
  
  if (p >= PRESSURE_EXTREME_HIGH) return isSummer ? "STABLE SUNNY" : "COLD/FOGGY ANTI";
  if (p >= PRESSURE_STANDARD_MID) return isSummer ? "SUNNY/DRY" : "CLOUDY/DRY"; 
  if (p <= PRESSURE_EXTREME_LOW)  return "LOW/HEAVY STORM";
  
  return "STABLE/FAIR";
}

String getSagerForecastText() {
  if (pressure6h < 950.0 || pressure < 950.0) {
    return "COLLECTING 6H\nDATA...";
  }

  float diff_6h = pressure - pressure6h;
  
  String p_level = "Normal";
  if (pressure < (PRESSURE_STANDARD_MID + PRESSURE_EXTREME_LOW) / 2.0) {
    p_level = "Low";
  } else if (pressure > (PRESSURE_STANDARD_MID + PRESSURE_EXTREME_HIGH) / 2.0) {
    p_level = "High";
  }
  
  float abs_d6h = abs(diff_6h);
  String speed = "Slow";
  if (abs_d6h >= 1.0 && abs_d6h < 2.0) speed = "Moderate";
  else if (abs_d6h >= 2.0 && abs_d6h < 3.5) speed = "Fast";
  else if (abs_d6h >= 3.5) speed = "Severe";

  if (diff_6h <= -0.2) { 
    if (p_level == "High") {
      return (speed == "Moderate" || speed == "Slow") ? "Fair weather ends,\nclouds increasing" : "Wind increasing,\nrain approaching";
    } else if (p_level == "Normal") {
      return (speed == "Fast" || speed == "Severe") ? "Weather deteriorating,\nwindy rain" : "Clouding over,\nrain possible";
    } else {
      return (speed == "Fast" || speed == "Severe") ? "Stormy, prolonged\nbad weather" : "Rain, unstable\nstormy air";
    }
  } else if (diff_6h >= 0.2) { 
    if (p_level == "Low") {
      return (speed == "Fast" || speed == "Severe") ? "Storm passed,\nwindy clearing" : "Slow, uncertain\nimprovement";
    } else if (p_level == "Normal") {
      return (speed == "Fast" || speed == "Severe") ? "Definite clearing,\nfair weather coming" : "Slowly improving\nconditions";
    } else {
      return "Stable, sunny,\ndry weather";
    }
  } else { 
    return "Stable situation,\nno change (24h)";
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (currentScreen == 1) {
    char trendIn = getTempTrendChar(tempInHistory[10], tempInHistory[0]);
    char trendOut = getTempTrendChar(tempOutHistory[10], tempOutHistory[0]);
    
    char trendP = getPressTrendChar(pressureMA[60], pressureMA[50]);
    float diff = inTemp - outTemp;
    
    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print("Indoor:  "); display.print(inTemp, 1); display.print(" C "); display.print(trendIn);
    display.setCursor(0, 18);
    display.print("Outdoor: "); display.print(outTemp, 1); display.print(" C "); display.print(trendOut);
    display.setCursor(0, 34);
    display.print("Delta:   "); if (diff >= 0) display.print("+"); display.print(diff, 1); display.print(" C");
    display.setCursor(0, 50);
    display.print("Baro:    "); display.print(pressure, 1); display.print(" hPa "); display.print(trendP);
    
  } else if (currentScreen == 2) {
    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print(isSummer ? "ZAMB(SUMMER)|Wind:" : "ZAMB(WINTER)|Wind:");
    display.print(currentWindDir);
    display.drawLine(0, 15, 128, 15, SSD1306_WHITE);
    
    display.setTextSize(2);
    display.setCursor(0, 25);
    display.print(getForecastText());
    
  } else if (currentScreen == 3) {
    float diff_6h = (pressure6h > 950.0) ? (pressure - pressure6h) : 0.0;
    
    // JAVÍTVA: A "-0.0" kijelzési anomália megszüntetése mikroszkopikus változásoknál
    if (abs(diff_6h) < 0.05) {
      diff_6h = 0.0;
    }

    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print("SAGER | 6h Tr: "); 
    if (diff_6h >= 0) display.print("+");
    display.print(diff_6h, 1);
    display.drawLine(0, 15, 128, 15, SSD1306_WHITE);
    
    display.setTextSize(1); 
    display.setCursor(0, 25);
    display.print(getSagerForecastText());
  }
  
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  
  drawBootScreen("System booting...");
  delay(1000);
  
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    wifiStatusStr = "NOK"; openMeteoStr = "X"; tsStatusStr = "X";
    while(true) { drawBootScreen("WiFi ERROR! Halted."); delay(1000); }
  }
  drawBootScreen("WiFi connected!");
  delay(500);
  
  syncInternetTime();
  updateSeason();
  delay(500);
  
  ThingSpeak.begin(client);
  drawBootScreen("ThingSpeak fetch...");
  fetchLatestData(); 
  delay(500);
  
  drawBootScreen("Open-Meteo fetch...");
  fetchWindDirection();
  delay(500);
  
  drawBootScreen("All ready!");
  if (tsStatusStr == "OK") {
    for(int i=0; i<11; i++) { tempInHistory[i] = inTemp; tempOutHistory[i] = outTemp; }
    for(int i=0; i<61; i++) { pressureHistory[i] = pressure; pressureMA[i] = pressure; }
  }
  delay(2000); 
  updateDisplay();
}

void loop() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastUpdateCheck >= UPDATE_INTERVAL_MS || lastUpdateCheck == 0) {
    lastUpdateCheck = currentMillis;
    connectWiFi();
    if (fetchLatestData()) {
      updateLocalHistory(); 
      if (currentScreen == 1) updateDisplay(); 
    }
  }
  
  if (currentMillis - lastWindCheck >= WIND_CHECK_INTERVAL_MS || lastWindCheck == 0) {
    lastWindCheck = currentMillis;
    fetchWindDirection();
    updateSeason(); 
  }
  
  if (currentScreen == 1 && currentMillis - lastScreenSwitch >= SCREEN_1_DURATION_MS) {
    lastScreenSwitch = currentMillis;
    currentScreen = 2;
    updateDisplay();
  } 
  else if (currentScreen == 2 && currentMillis - lastScreenSwitch >= SCREEN_2_DURATION_MS) {
    lastScreenSwitch = currentMillis;
    currentScreen = 3;
    updateDisplay();
  }
  else if (currentScreen == 3 && currentMillis - lastScreenSwitch >= SCREEN_2_DURATION_MS) {
    lastScreenSwitch = currentMillis;
    currentScreen = 1;
    updateDisplay();
  }
}
