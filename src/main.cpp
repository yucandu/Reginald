#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "hobbes.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include "Audio.h"
#include "time.h"
#include "esp_sntp.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "StreamUtils.h"

#include <FS.h>
#include <LittleFS.h>
#include "constants.h"
int hours, mins, secs;
const char* mlbApiUrl = "http://statsapi.mlb.com/api/v1.1/game/777079/feed/live";

// PSRAM buffer for storing the JSON
char* jsonBuffer = nullptr;
size_t jsonSize = 0;
char auth[] = "xxxxxxxxxxxxxxx";
struct tm timeinfo;
bool isSetNtp = false;
unsigned long localTimeUnix = 0;
// Global variables to store game data (update these existing ones)
int batterID = 0;
int pitcherID = 0;
int battingOrder = 0;
String batterAvg = ".000";
int pitchCount = 0;
String awayTeamAbbrev = "";
String homeTeamAbbrev = "";
int awayTeamID = 0;
int homeTeamID = 0;
String awayTeamName = "";
String homeTeamName = "";
int awayScore = 0;
int homeScore = 0;
String inningState = "";
String currentInningOrdinal = "";
int ballCount = 0;
int strikeCount = 0;
int outCount = 0;
bool firstBase = false;
bool secondBase = false;
bool thirdBase = false;
// Add these new ones for matchup and pitch data
String batterName = "";
String pitcherName = "";
float strikeZoneTop = 0.0;
float strikeZoneBottom = 0.0;

// Structure for pitch data
struct PitchData {
  String call;
  String typeCode;
  float speed;
  float pX;
  float pZ;
  uint16_t color;
};

PitchData pitches[10]; // Store up to 10 recent pitches

void wait4SNTP();
void setTimezone();
String formatDate(struct tm* t);
void displayGameInfo();
bool parseGameDataWithBuffering(WiFiClient& stream);
bool downloadJsonToPsram();
void parseGameData();
void parseLinescore();
void parseCurrentPlay();
void parsePlayerData();
int getSpriteIndex(const String& typeCode);

#define every(interval) \
    static uint32_t __every__##interval = millis(); \
    if (millis() - __every__##interval >= interval && (__every__##interval = millis()))

#define I2S_DOUT      6
#define I2S_BCLK      5
#define I2S_LRC       4
#define I2S_SDN       7

#define LDR_PIN      1   // LDR pin, if used

#define BTN_1 2
#define BTN_2 14
#define BTN_3 39
#define BTN_4 35

#define BTN_7 3

#define BTN_SETCH 41
#define BTN_MINMAX 37

#define LED_PIN     48   // Change this if needed
#define BKL_PIN    8   // Backlight pin, if used

Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);
const char* ssid = "mikesnet";
const char* password = "springchicken";
Audio audio;
#define TFT_TRANSPARENT  0x079f
// LovyanGFX configuration for ST7789 240x320 tft
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;  // Add this
public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;  // Back to 3-wire - let LovyanGFX handle DC automatically
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 13;   // SCL pin
      cfg.pin_mosi = 12;   // SDA pin  
      cfg.pin_miso = -1;  // Not used in write-only tft
      cfg.pin_dc = 10;    // DC pin
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 9;
      cfg.pin_rst = 11;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;  // Try this - many ST7789 tfts need inversion
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    // Backlight config
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = BKL_PIN;          // Your backlight pin
      cfg.invert = false;            // true if backlight is active low
      cfg.freq = 44100;              // PWM frequency
      cfg.pwm_channel = 7;           // PWM channel number
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }

  void deallocate(void* pointer) override {
    heap_caps_free(pointer);
  }

  void* reallocate(void* ptr, size_t new_size) override {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};


// Create tft instance
LGFX tft;
LGFX_Sprite img(&tft);
LGFX_Sprite sprite[7] = {
  LGFX_Sprite(&tft), LGFX_Sprite(&tft), LGFX_Sprite(&tft),
  LGFX_Sprite(&tft), LGFX_Sprite(&tft), LGFX_Sprite(&tft),
  LGFX_Sprite(&tft)
};
const char* baseURL = "http://statsapi.mlb.com";


// Structure to hold play event data
struct PlayEvent {
  String callDescription;
  String typeCode;
  float startSpeed;
  float strikeZoneTop;
  float strikeZoneBottom;
  float pX;
  float pZ;
  
  // Constructor to initialize values
  PlayEvent() {
    callDescription = "";
    typeCode = "";
    startSpeed = 0.0;
    strikeZoneTop = 0.0;
    strikeZoneBottom = 0.0;
    pX = 0.0;
    pZ = 0.0;
  }
};

// Global array to store all play events from the last play
PlayEvent playEvents[20]; // Adjust size as needed
int playEventCount = 0;

void cbSyncTime(struct timeval *tv) { // callback function to show when NTP was synchronized
  Serial.println("NTP time synched");
  Serial.println("getlocaltime");
  getLocalTime(&timeinfo);

  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);

  Serial.println(asctime(timeinfo));
  time_t now = time(nullptr); // local-adjusted time
  localTimeUnix = static_cast<uint32_t>(now); // 32-bit to send via ESP-NOW
  isSetNtp = true;
}

String formatDate(struct tm* t) {
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}

void initSNTP() {  
  sntp_set_sync_interval(10 * 60 * 1000UL);  // 1 hour
  sntp_set_time_sync_notification_cb(cbSyncTime);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "192.168.50.197");
  esp_sntp_init();
  wait4SNTP();
  setTimezone();
}

void wait4SNTP() {
  Serial.print("Waiting for time...");
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    delay(1000);
    Serial.print(".");
  }
}

void setTimezone() {  
  setenv("TZ","EST5EDT,M3.2.0,M11.1.0",1);
  tzset();
}


String httpGETRequest(const char* serverName) {
  HTTPClient http;
  http.begin(serverName);
  
  int httpResponseCode = http.GET();
  String payload = "";
  
  if (httpResponseCode > 0) {
    payload = http.getString();
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
  } else {
    Serial.printf("Error code: %d\n", httpResponseCode);
  }
  
  http.end();
  return payload;
}

bool downloadJsonToPsram() {
  HTTPClient http;
  Serial.println("\n=== Getting Blue Jays Game ===");
  time_t now;
  struct tm startTm, endTm;
  time(&now);
  localtime_r(&now, &startTm);
  
  // Change: Start from 2 days before today
  startTm.tm_mday -= 2;
  mktime(&startTm); // normalize
  
  endTm = startTm;
  endTm.tm_mday += 7;
  mktime(&endTm); // normalize

  String startDate = formatDate(&startTm);
  String endDate = formatDate(&endTm);

  String scheduleUrl = "http://statsapi.mlb.com/api/v1/schedule?teamId=141&sportId=1&expand=schedule.teams,schedule.linescore,schedule.ticket"
               "&startDate=" + startDate + "&endDate=" + endDate;
  Serial.println(scheduleUrl);
  http.begin(scheduleUrl);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode != 200) {
    Serial.printf("HTTP Error: %d\n", httpResponseCode);
    http.end();
    return false;
  }
  Serial.println("Fetching schedule data...");
  
  String scheduleResponse = httpGETRequest(scheduleUrl.c_str());
  if (scheduleResponse.length() == 0) {
    Serial.println("Failed to get schedule data");
    return false;
  }
  
  // Parse schedule JSON
  JsonDocument scheduleDoc;
  DeserializationError error = deserializeJson(scheduleDoc, scheduleResponse);
  
  if (error) {
    Serial.print("Schedule JSON parsing failed: ");
    Serial.println(error.c_str());
    return false;
  }
  
  // Fix: Find live game by checking from today backwards
  String liveGamePath = "";
  time_t today = time(nullptr);
  struct tm todayTm;
  localtime_r(&today, &todayTm);
  
  if (scheduleDoc["dates"].is<JsonArray>()) {
    JsonArray dates = scheduleDoc["dates"];
    
    // Start from the end (most recent) and work backwards
    for (int dateIdx = dates.size() - 1; dateIdx >= 0; dateIdx--) {
      JsonObject dateObj = dates[dateIdx];
      if (dateObj["games"].is<JsonArray>()) {
        JsonArray games = dateObj["games"];
        
        for (JsonObject game : games) {
          JsonObject status = game["status"];
          if (status.containsKey("abstractGameState")) {
            String gameState = status["abstractGameState"].as<String>();
            Serial.printf("Game state: %s\n", gameState.c_str());
            
            if (gameState == "Live") {
              liveGamePath = game["link"].as<String>();
              Serial.println("Found live game!");
              break;
            } else if (gameState == "Final" && liveGamePath.length() == 0) {
              // Use most recent completed game if no live game found
              liveGamePath = game["link"].as<String>();
              Serial.println("Using most recent completed game");
            }
          }
        }
        if (liveGamePath.length() > 0 && scheduleDoc["dates"][dateIdx]["games"][0]["status"]["abstractGameState"] == "Live") {
          break; // Found live game, stop looking
        }
      }
    }
  }
  
  if (liveGamePath.length() == 0) {
    Serial.println("Could not find any suitable game");
    return false;
  }
  
  Serial.print("Game path: ");
  Serial.println(liveGamePath);
  
  // Rest of the function remains the same...
  String mlbApiUrl = String(baseURL) + liveGamePath;
  Serial.print("Fetching game data from: ");
  Serial.println(mlbApiUrl);
  http.begin(mlbApiUrl);
  
  Serial.println("Downloading JSON...");
  httpResponseCode = http.GET();
  
  if (httpResponseCode != 200) {
    Serial.printf("HTTP Error: %d\n", httpResponseCode);
    http.end();
    return false;
  }
  
  // Get content length
  int contentLength = http.getSize();
  Serial.printf("Content Length: %d bytes\n", contentLength);
  
  if (contentLength <= 0) {
    Serial.println("Invalid content length");
    http.end();
    return false;
  }
  
  // Allocate PSRAM buffer
  jsonBuffer = (char*)ps_malloc(contentLength + 1);
  if (!jsonBuffer) {
    Serial.println("Failed to allocate PSRAM buffer");
    http.end();
    return false;
  }
  
  // Download data to PSRAM
  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = 0;
  
  while (http.connected() && bytesRead < contentLength) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min(available, (size_t)(contentLength - bytesRead));
      size_t actualRead = stream->readBytes(jsonBuffer + bytesRead, toRead);
      bytesRead += actualRead;
      
      // Show progress
      if (bytesRead % 10000 == 0) {
        Serial.printf("Downloaded: %d/%d bytes\n", bytesRead, contentLength);
      }
    }
    delay(1);
  }
  
  jsonBuffer[bytesRead] = '\0';
  jsonSize = bytesRead;
  
  Serial.printf("Download complete: %d bytes\n", jsonSize);
  http.end();
  return true;
}

void parseJsonInChunks() {
  Serial.println("Parsing JSON in chunks...");
  
  parseGameData();
  parseLinescore();
  parseCurrentPlay();
  parsePlayerData();  // Add this line
}

void parseGameData() {
  Serial.println("Parsing game data...");
  
  SpiRamAllocator allocator;
  JsonDocument filter(&allocator);
  JsonDocument doc(&allocator);
  
  // Set up filter for gameData section
  filter["gameData"]["teams"]["away"]["name"] = true;
  filter["gameData"]["teams"]["home"]["name"] = true;
  filter["gameData"]["teams"]["away"]["abbreviation"] = true;
  filter["gameData"]["teams"]["home"]["abbreviation"] = true;
  filter["gameData"]["teams"]["away"]["id"] = true;
  filter["gameData"]["teams"]["home"]["id"] = true;
  
  DeserializationError error = deserializeJson(doc, jsonBuffer, DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(15));
  
  if (error) {
    Serial.printf("Game data parse error: %s\n", error.c_str());

    return;
  }
  
  // Extract team data
  const char* awayTeam = doc["gameData"]["teams"]["away"]["name"];
  const char* homeTeam = doc["gameData"]["teams"]["home"]["name"];
  const char* awayAbbr = doc["gameData"]["teams"]["away"]["abbreviation"];
  const char* homeAbbr = doc["gameData"]["teams"]["home"]["abbreviation"];
  
  if (awayTeam) awayTeamName = String(awayTeam);
  if (homeTeam) homeTeamName = String(homeTeam);
  if (awayAbbr) awayTeamAbbrev = String(awayAbbr);
  if (homeAbbr) homeTeamAbbrev = String(homeAbbr);
  
  awayTeamID = doc["gameData"]["teams"]["away"]["id"] | 0;
  homeTeamID = doc["gameData"]["teams"]["home"]["id"] | 0;
  
  if (awayTeam && homeTeam) {
    Serial.printf("%s @ %s\n", awayTeam, homeTeam);
  }
  

}

void parsePlayerData() {
  Serial.println("Parsing player data...");
  SpiRamAllocator allocator;
  JsonDocument filter(&allocator);
  JsonDocument doc(&allocator);
  
  // Filter for boxscore data including battingOrder
  filter["liveData"]["boxscore"]["teams"]["away"]["battingOrder"] = true;
  filter["liveData"]["boxscore"]["teams"]["home"]["battingOrder"] = true;
  filter["liveData"]["boxscore"]["teams"]["away"]["players"] = true;
  filter["liveData"]["boxscore"]["teams"]["home"]["players"] = true;
  filter["liveData"]["boxscore"]["info"] = true;
  
  DeserializationError error = deserializeJson(doc, jsonBuffer, DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(20));
  
  if (error) {
    Serial.printf("Player data parse error: %s\n", error.c_str());
    return;
  }
  
  // Find batting order for current batter using battingOrder array
  if (batterID > 0) {
    JsonArray awayBattingOrder = doc["liveData"]["boxscore"]["teams"]["away"]["battingOrder"];
    JsonArray homeBattingOrder = doc["liveData"]["boxscore"]["teams"]["home"]["battingOrder"];
    
    // Check away team batting order
    for (int i = 0; i < awayBattingOrder.size(); i++) {
      if (awayBattingOrder[i] == batterID) {
        battingOrder = i + 1;  // Batting order is 1-based
        Serial.printf("Found batter %d in away batting order position %d\n", batterID, battingOrder);
        break;
      }
    }
    
    // Check home team batting order if not found in away
    if (battingOrder == 0) {
      for (int i = 0; i < homeBattingOrder.size(); i++) {
        if (homeBattingOrder[i] == batterID) {
          battingOrder = i + 1;  // Batting order is 1-based
          Serial.printf("Found batter %d in home batting order position %d\n", batterID, battingOrder);
          break;
        }
      }
    }
    
    // Get batting average from players data
    JsonObject awayPlayers = doc["liveData"]["boxscore"]["teams"]["away"]["players"];
    JsonObject homePlayers = doc["liveData"]["boxscore"]["teams"]["home"]["players"];
    
    String playerKey = "ID" + String(batterID);
    JsonObject player = awayPlayers[playerKey];
    if (player.isNull()) {
      player = homePlayers[playerKey];
    }
    
    if (!player.isNull()) {
      JsonObject seasonStats = player["seasonStats"];
      if (!seasonStats.isNull()) {
        JsonObject batting = seasonStats["batting"];
        if (!batting.isNull()) {
          const char* avg = batting["avg"];
          if (avg) batterAvg = String(avg);
        }
      }
    }
  }
  
  // Parse pitch count from info section
  JsonArray info = doc["liveData"]["boxscore"]["info"];
  if (!info.isNull()) {
    for (JsonObject infoItem : info) {
      const char* label = infoItem["label"];
      if (label && String(label) == "Pitches") {
        const char* value = infoItem["value"];
        if (value) {
          String valueStr = String(value);
          // Find pitcher name in the value and extract pitch count
          if (valueStr.indexOf(pitcherName) >= 0) {
            int nameEnd = valueStr.indexOf(pitcherName) + pitcherName.length();
            String remaining = valueStr.substring(nameEnd);
            // Extract first number after pitcher name
            int spacePos = remaining.indexOf(' ');
            if (spacePos > 0) {
              String pitchStr = remaining.substring(spacePos + 1);
              int dashPos = pitchStr.indexOf('-');
              if (dashPos > 0) {
                pitchCount = pitchStr.substring(0, dashPos).toInt();
              }
            }
          }
        }
        break;
      }
    }
  }
}

void drawDiamond(int centerX, int centerY, int size, uint16_t color, bool filled = true) {
  // Calculate diamond points
  int halfSize = size / 2;
  int x1 = centerX;          // top
  int y1 = centerY - halfSize;
  int x2 = centerX + halfSize; // right
  int y2 = centerY;
  int x3 = centerX;          // bottom
  int y3 = centerY + halfSize;
  int x4 = centerX - halfSize; // left
  int y4 = centerY;
  
  if (filled) {
    // Draw filled diamond using triangles
    img.fillTriangle(x1, y1, x2, y2, x3, y3, color);
    img.fillTriangle(x1, y1, x4, y4, x3, y3, color);
  } else {
    // Draw diamond outline
    img.drawLine(x1, y1, x2, y2, color);
    img.drawLine(x2, y2, x3, y3, color);
    img.drawLine(x3, y3, x4, y4, color);
    img.drawLine(x4, y4, x1, y1, color);
  }
}

void parseLinescore() {
  Serial.println("Parsing linescore...");
  
  SpiRamAllocator allocator;
  JsonDocument filter(&allocator);
  JsonDocument doc(&allocator);
  
  // Filter for linescore section
  filter["liveData"]["linescore"]["currentInningOrdinal"] = true;
  filter["liveData"]["linescore"]["inningState"] = true;
  filter["liveData"]["linescore"]["balls"] = true;
  filter["liveData"]["linescore"]["strikes"] = true;
  filter["liveData"]["linescore"]["outs"] = true;
  filter["liveData"]["linescore"]["teams"]["home"]["runs"] = true;
  filter["liveData"]["linescore"]["teams"]["away"]["runs"] = true;
  filter["liveData"]["linescore"]["offense"]["first"] = true;
  filter["liveData"]["linescore"]["offense"]["second"] = true;
  filter["liveData"]["linescore"]["offense"]["third"] = true;
  
  DeserializationError error = deserializeJson(doc, jsonBuffer, DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(15));
  
  if (error) {
    Serial.printf("Linescore parse error: %s\n", error.c_str());
    return;
  }
  
  // Extract linescore data and properly store in global variables
  awayScore = doc["liveData"]["linescore"]["teams"]["away"]["runs"] | 0;
  homeScore = doc["liveData"]["linescore"]["teams"]["home"]["runs"] | 0;
  ballCount = doc["liveData"]["linescore"]["balls"] | 0;
  strikeCount = doc["liveData"]["linescore"]["strikes"] | 0;
  outCount = doc["liveData"]["linescore"]["outs"] | 0;
  
  // Fix: Properly extract and store inning state and ordinal
  const char* inningStateStr = doc["liveData"]["linescore"]["inningState"];
  const char* currentInningOrdinalStr = doc["liveData"]["linescore"]["currentInningOrdinal"];
  
  if (inningStateStr) {
    inningState = String(inningStateStr);
    Serial.printf("Inning State: %s\n", inningState.c_str());
  }
  
  if (currentInningOrdinalStr) {
    currentInningOrdinal = String(currentInningOrdinalStr);
    Serial.printf("Current Inning Ordinal: %s\n", currentInningOrdinal.c_str());
  }
  
  Serial.printf("%d - %d\n", awayScore, homeScore);
  if (inningState.length() > 0 && currentInningOrdinal.length() > 0) {
    Serial.printf("%s, %s\n", inningState.c_str(), currentInningOrdinal.c_str());
  }
  Serial.printf("B: %d S: %d O: %d\n", ballCount, strikeCount, outCount);
  
  // Add base runner parsing
  JsonObject offense = doc["liveData"]["linescore"]["offense"];
  if (!offense.isNull()) {
    // If the base object exists and is not null, there's a runner on that base
    firstBase = !offense["first"].isNull();
    secondBase = !offense["second"].isNull();
    thirdBase = !offense["third"].isNull();
    
    // Debug output
    Serial.printf("Base runners - 1st: %s, 2nd: %s, 3rd: %s\n", 
                  firstBase ? "YES" : "NO",
                  secondBase ? "YES" : "NO", 
                  thirdBase ? "YES" : "NO");
  } else {
    // No offense object means no runners
    firstBase = false;
    secondBase = false;
    thirdBase = false;
    Serial.println("No offense object found - no base runners");
  }
}

void parseCurrentPlay() {
  Serial.println("Parsing current play...");
 
  SpiRamAllocator allocator;
  JsonDocument filter(&allocator);
  JsonDocument doc(&allocator);
 
  // Filter for current play section including matchup data
  filter["liveData"]["plays"]["currentPlay"]["playEvents"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["batter"]["fullName"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["batSide"]["code"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["pitcher"]["fullName"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["pitchHand"]["code"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["batter"]["id"] = true;
  filter["liveData"]["plays"]["currentPlay"]["matchup"]["pitcher"]["id"] = true;

 
  DeserializationError error = deserializeJson(doc, jsonBuffer, DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(15));
 
  if (error) {
    Serial.printf("Current play parse error: %s\n", error.c_str());

    return;
  }
 
  // Extract matchup information first
  JsonObject currentPlay = doc["liveData"]["plays"]["currentPlay"];
  if (!currentPlay.isNull()) {
    JsonObject matchup = currentPlay["matchup"];
    if (!matchup.isNull()) {
      Serial.println("=== MATCHUP INFO ===");
      
      // Batter information
      JsonObject batter = matchup["batter"];
      if (!batter.isNull()) {
        const char* name = batter["fullName"];
        if (name) batterName = String(name);
        batterID = batter["id"] | 0;  // Add this line
      }

      
      // Bat side
      JsonObject batSide = matchup["batSide"];
      if (!batSide.isNull()) {
        const char* batSideCode = batSide["code"];
        if (batSideCode) {
          Serial.printf("Bat Side: %s\n", batSideCode);
        }
      }
      

      
      // Pitch hand
    JsonObject pitcher = matchup["pitcher"];
    if (!pitcher.isNull()) {
      const char* name = pitcher["fullName"];
      if (name) pitcherName = String(name);
      pitcherID = pitcher["id"] | 0;  // Add this line
    }
      
      Serial.println("==================");
    }
  }
 
  Serial.printf("Set batterID to: %d\n", batterID);
  Serial.printf("Set pitcherID to: %d\n", pitcherID);
  JsonArray playEvents = doc["liveData"]["plays"]["currentPlay"]["playEvents"];
 
  if (playEvents.isNull()) {
    Serial.println("No play events found");

    return;
  }
 
  Serial.printf("Found %d play events:\n", playEvents.size());
 
  for (JsonObject event : playEvents) {
    // Extract details
    JsonObject details = event["details"];
    if (!details.isNull()) {
      JsonObject call = details["call"];
      JsonObject type = details["type"];
     
      if (!call.isNull()) {
        const char* description = call["description"];
        if (description) {
          Serial.printf("Call: %s\n", description);
        }
      }
     
      if (!type.isNull()) {
        const char* code = type["code"];
        if (code) {
          Serial.printf("Type: %s\n", code);
        }
      }
    }
   
    // Extract pitch data
    JsonObject pitchData = event["pitchData"];
    if (!pitchData.isNull()) {
      float startSpeed = pitchData["startSpeed"] | 0.0f;
      float strikeZoneTop = pitchData["strikeZoneTop"] | 0.0f;
      float strikeZoneBottom = pitchData["strikeZoneBottom"] | 0.0f;
     
      JsonObject coordinates = pitchData["coordinates"];
      if (!coordinates.isNull()) {
        float pX = coordinates["pX"] | 0.0f;
        float pZ = coordinates["pZ"] | 0.0f;
       
        Serial.printf("Speed: %.1f\n", startSpeed);
        Serial.printf("strikeZoneTop: %.2f\n", strikeZoneTop);
        Serial.printf("strikeZoneBottom: %.2f\n", strikeZoneBottom);
        Serial.printf("pX,pZ: %.2f,%.2f\n", pX, pZ);
      }
    }
   
    Serial.println("---");
  }
   currentPlay = doc["liveData"]["plays"]["currentPlay"];
  if (!currentPlay.isNull()) {
    JsonObject matchup = currentPlay["matchup"];
    if (!matchup.isNull()) {
      JsonObject batter = matchup["batter"];
      if (!batter.isNull()) {
        const char* name = batter["fullName"];
        if (name) batterName = String(name);
      }
      
      JsonObject pitcher = matchup["pitcher"];
      if (!pitcher.isNull()) {
        const char* name = pitcher["fullName"];
        if (name) pitcherName = String(name);
      }
    }
  }
  
  // Process play events and store pitch data
   playEvents = doc["liveData"]["plays"]["currentPlay"]["playEvents"];
  if (!playEvents.isNull()) {
    pitchCount = 0;
    for (JsonObject event : playEvents) {
      if (pitchCount >= 10) break; // Limit to 10 pitches
      
      JsonObject details = event["details"];
      JsonObject pitchData = event["pitchData"];
      
      if (!details.isNull() && !pitchData.isNull()) {
        PitchData &pitch = pitches[pitchCount];
        
        // Get call description
        JsonObject call = details["call"];
        if (!call.isNull()) {
          const char* desc = call["description"];
          if (desc) pitch.call = String(desc);
        }
        
        // Get pitch type
        JsonObject type = details["type"];
        if (!type.isNull()) {
          const char* code = type["code"];
          if (code) pitch.typeCode = String(code);
        }
        
        // Get pitch data
        pitch.speed = pitchData["startSpeed"] | 0.0f;
        strikeZoneTop = pitchData["strikeZoneTop"] | 3.5f;
        strikeZoneBottom = pitchData["strikeZoneBottom"] | 1.5f;
        
        JsonObject coordinates = pitchData["coordinates"];
        if (!coordinates.isNull()) {
          pitch.pX = coordinates["pX"] | 0.0f;
          pitch.pZ = coordinates["pZ"] | 0.0f;
        }
        
        // Set color based on call
        if (pitch.call.indexOf("Ball") >= 0) {
          pitch.color = TFT_GREEN;
        } else if (pitch.call.indexOf("Strike") >= 0 || pitch.call.indexOf("Foul") >= 0) {
          pitch.color = TFT_RED;
        } else if (pitch.call.indexOf("In play") >= 0) {
          pitch.color = TFT_BLUE;
        } else {
          pitch.color = TFT_WHITE;
        }
        
        pitchCount++;
      }
    }
  }
  // Clean up allocated memory

 
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
}

void displayGameInfo() {
  //img.fillSprite(0x079f);
    if(!LittleFS.begin()){
    Serial.println("LittleFS Mount Failed");
    return;
    }

    File file = LittleFS.open("/mlbbackground.jpg", "r");
    if (!file) {
        Serial.println("Failed to open file");
        return;
    }

    size_t fileSize = file.size();
    uint8_t* buffer = (uint8_t*)ps_malloc(fileSize);
    if (!buffer) {
        Serial.println("Failed to allocate memory");
        file.close();
        return;
    }

    file.read(buffer, fileSize);
    file.close();

    img.drawJpg(buffer, fileSize, 0, 0);
    free(buffer);
  // Get team colors
  uint16_t awayBgColor = TEAM_BG_COLORS.count(awayTeamID) ? TEAM_BG_COLORS[awayTeamID] : COLORS::BLACK;
  uint16_t awayTextColor = TEAMS_TEXT_COLORS.count(awayTeamID) ? TEAMS_TEXT_COLORS[awayTeamID] : TFT_WHITE;
  uint16_t homeBgColor = TEAM_BG_COLORS.count(homeTeamID) ? TEAM_BG_COLORS[homeTeamID] : COLORS::BLACK;
  uint16_t homeTextColor = TEAMS_TEXT_COLORS.count(homeTeamID) ? TEAMS_TEXT_COLORS[homeTeamID] : TFT_WHITE;
  
  // Draw team score boxes
  int boxWidth = 111;
  int boxHeight = 32;
  int awayBoxY = 231;
  int homeBoxY = 264;
  int boxX = 64;
  
  // Away team box
  img.fillRect(boxX, awayBoxY, boxWidth, boxHeight, awayBgColor);
  img.setTextColor(awayTextColor);
  img.setTextSize(1);
  img.setFont(&fonts::FreeSansBold12pt7b);
  img.setCursor(boxX + 10, awayBoxY + 8);
  img.print(awayTeamAbbrev);
  img.setCursor(boxX + 75, awayBoxY + 2);
  img.setFont(&fonts::FreeSansBold18pt7b);
  //img.setTextSize(3);
  img.print(awayScore);
  
  // Home team box
  img.fillRect(boxX, homeBoxY, boxWidth, boxHeight, homeBgColor);
  img.setTextColor(homeTextColor);
  img.setTextSize(1);
  img.setFont(&fonts::FreeSansBold12pt7b);
  img.setCursor(boxX + 10, homeBoxY + 8);
  img.print(homeTeamAbbrev);
  img.setCursor(boxX + 75, homeBoxY + 2);
  img.setFont(&fonts::FreeSansBold18pt7b);
  //img.setTextSize(3);
  img.print(homeScore);
  
  // Draw bases (diamonds on left side)
  int baseSize = 24;
  int baseX = 31;
  int baseY = 269;
  
  // Third base (left)
  uint16_t thirdColor = thirdBase ? COLORS::YELLOW : COLORS::GRAY;
  drawDiamond(baseX - (baseSize/2) - 2, baseY, baseSize, thirdColor);
  
  // Second base (top)
  uint16_t secondColor = secondBase ? COLORS::YELLOW : COLORS::GRAY;
  drawDiamond(baseX, baseY - (baseSize/2) - 2, baseSize, secondColor);
  
  // First base (right)
  uint16_t firstColor = firstBase ? COLORS::YELLOW : COLORS::GRAY;
  drawDiamond(baseX + (baseSize/2) + 2, baseY, baseSize, firstColor);
  
  // Draw pitcher info
  img.setTextColor(TFT_WHITE);
  img.setTextSize(1);
  img.setFont(&fonts::Font2);
  img.setCursor(24, 187);
  img.print(pitcherName);
  img.setCursor(200, 187);
  img.printf("P: %d", pitchCount);
  
  // Draw batter info
  img.setCursor(5, 212);
  img.printf("%d. %s", battingOrder, batterName.c_str());
  img.setCursor(200, 212);
  img.print(batterAvg);
  
  // Draw strike zone (adjusted for y=184 as ground)
  float pixelsPerFoot = 184.0 / 4.7;
  
  int szTop = 184 - (strikeZoneTop * pixelsPerFoot);
  int szBottom = 184 - (strikeZoneBottom * pixelsPerFoot);
  int szHeight = szBottom - szTop;
  int szWidth = 1.417 * pixelsPerFoot;
  int szLeft = (240 - szWidth) / 2;
  int szRight = szLeft + szWidth;
  
  // Draw strike zone outline

  img.setFont(&fonts::Font0);

  //Draw Legend
    const char* labels[] = {
      "4SF",   // sprite[0]
      "CRV",  // sprite[1]
      "SLD",     // sprite[2]
      "CUT",     // sprite[3]
      "SPL",   // sprite[4]
      "CHG",   // sprite[5]
      "2SF"      // sprite[6]
    };

     baseX = 2;
     baseY = 80;
    img.drawRect(0, baseY-2, 37, 100, TFT_LIGHTGREY);
    for (int i = 0; i < 7; ++i) {
      int y = baseY + i * 14;

      // Draw sprite
      sprite[i].pushSprite(&img, baseX, y, TFT_TRANSPARENT);

      // Draw label
      img.setTextColor(TFT_WHITE);  // White on black background
      img.setTextSize(1);
      img.setCursor(baseX + 14, y+1);         // slight vertical offset for alignment
      img.print(labels[i]);
    }
  img.drawRect(szLeft, szTop, szWidth, szHeight, TFT_LIGHTGREY);
  
  // Draw 3x3 grid
  int gridWidth = szWidth / 3;
  for (int i = 1; i < 3; i++) {
    img.drawLine(szLeft + (i * gridWidth), szTop, szLeft + (i * gridWidth), szBottom, COLORS::GRAY);
  }
  
  int gridHeight = szHeight / 3;
  for (int i = 1; i < 3; i++) {
    img.drawLine(szLeft, szTop + (i * gridHeight), szRight, szTop + (i * gridHeight), COLORS::GRAY);
  }
  
  // Draw pitches
  for (int i = 0; i < pitchCount && i < 10; i++) {
    PitchData &pitch = pitches[i];
    
    int screenX = 120 + (pitch.pX * pixelsPerFoot);
    int screenY = 184 - (pitch.pZ * pixelsPerFoot);
    
    int spriteIndex = getSpriteIndex(pitch.typeCode);
    sprite[spriteIndex].pushSprite(&img, screenX - 6, screenY - 6, 0x079f); // center it
    
    img.setTextSize(1);
   // uint16_t textColor = (pitch.color == TFT_GREEN) ? TFT_BLACK : TFT_WHITE;
   // img.setTextColor(textColor);
   // img.setCursor(screenX - 5, screenY - 4);
    //img.print(pitch.typeCode);

    img.setTextColor(TFT_WHITE);
    img.setCursor(screenX + 10, screenY - 4);
    img.printf("%.0f", pitch.speed);
  }
  
  // Fix: Draw inning and count info at bottom with proper font and debugging
  img.setTextColor(0x0169);
  img.setTextSize(1);
  img.setFont(&fonts::FreeSansBold12pt7b);
  
  Serial.printf("Display - Inning State: '%s', Current Inning: '%s'\n", inningState.c_str(), currentInningOrdinal.c_str());
  
  img.setCursor(1, 300); // Moved Y position down slightly for visibility
  
  // Draw up/down arrow for inning state
  if (inningState == "Top") {
    img.print("Top ");
  } else if (inningState == "Bottom") {
    img.print("Bot ");
  } else if (inningState.length() > 0) {
    img.printf("%s ", inningState.c_str());
  }
  
  if (currentInningOrdinal.length() > 0) {
    img.print(currentInningOrdinal);
  }
  
  // Draw count
  img.setCursor(100, 300);
  img.printf("%d-%d", ballCount, strikeCount);
  img.setCursor(160, 300);
  img.printf("%d OUT", outCount);
  
  img.pushSprite(0, 0);
}

int getSpriteIndex(const String& typeCode) {
  if (typeCode == "FF") return 0;
  if (typeCode == "CU" || typeCode == "KC") return 1;
  if (typeCode == "SL" || typeCode == "ST" || typeCode == "SV") return 2;
  if (typeCode == "FC") return 3;
  if (typeCode == "FS" || typeCode == "FO") return 4;
  if (typeCode == "CH" || typeCode == "UN" || typeCode == "EP" ||
      typeCode == "GY" || typeCode == "KN" || typeCode == "SC") return 5;
  if (typeCode == "FT" || typeCode == "SI") return 6;
  return 0; // fallback/default
}

void doMLB() {

    Serial.print("Current time: ");
    float currentTime = millis() / 1000.0;
    Serial.println(currentTime);
    if (downloadJsonToPsram()) {
      parseJsonInChunks();
    }
    
    // Free PSRAM buffer
    if (jsonBuffer) {
      free(jsonBuffer);
      jsonBuffer = nullptr;
    }

    

    displayGameInfo();
    
    Serial.print("Current time: ");
    float newTime = millis() / 1000.0;
    Serial.println(newTime);
    Serial.print("Time difference: ");
    Serial.println(newTime - currentTime);
}

void setup() {
  pinMode(LDR_PIN, INPUT);
  pinMode(BTN_1, INPUT_PULLUP);
  pinMode(BTN_2, INPUT_PULLUP);
  pinMode(BTN_3, INPUT_PULLUP);
  pinMode(BTN_4, INPUT_PULLUP);

  pinMode(BTN_7, INPUT_PULLUP);
  pinMode(BTN_SETCH, INPUT_PULLUP);
  pinMode(BTN_MINMAX, INPUT_PULLUP);
  
  Serial.begin(115200);
  Serial.println("ESP32-S3 Image tft Starting...");
  delay(1000);
  
  // Check if PSRAM is available

  
  // Initialize the tft
  tft.init();
  tft.setBrightness(255);  // 0-255, where 255 is brightest
  tft.setRotation(0);  // Adjust rotation as needed (0-3)
  tft.fillScreen(TFT_BLUE);
  tft.setCursor(0, 0);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.setTextWrap(true);  // Wrap on width
  tft.setTextFont(2);     // Set font size
  tft.setTextSize(1);     // Set text size
  tft.print("Connecting to wifi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(500); // Wait 1 second before calling WiFi.begin()

  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  for (int i = 0; i < 7; ++i) {
    sprite[i].createSprite(12, 12);
    sprite[i].setColorDepth(8);
    sprite[i].fillSprite(TFT_TRANSPARENT);
  }
  // 1. Red circle
  sprite[0].fillCircle(6, 6, 6, TFT_RED);

  // 2. Yellow upward equilateral triangle
  {
    int x0 = 6, y0 = 0;
    int x1 = 0, y1 = 11;
    int x2 = 11, y2 = 11;
    sprite[1].fillTriangle(x0, y0, x1, y1, x2, y2, TFT_YELLOW);
  }

  // 3. Green rightward equilateral triangle
  {
    int x0 = 0, y0 = 0;
    int x1 = 11, y1 = 6;
    int x2 = 0, y2 = 11;
    sprite[2].fillTriangle(x0, y0, x1, y1, x2, y2, TFT_GREEN);
  }

  // 4. Purple right-angled triangle (bottom base, left height)
  {
    int x0 = 0, y0 = 6;
    int x1 = 0, y1 = 11;
    int x2 = 11, y2 = 11;
    sprite[3].fillTriangle(x0, y0, x1, y1, x2, y2, TFT_MAGENTA);
  }

  // 5. Pink downward equilateral triangle
  {
    int x0 = 0, y0 = 0;
    int x1 = 11, y1 = 0;
    int x2 = 6, y2 = 11;
    sprite[4].fillTriangle(x0, y0, x1, y1, x2, y2, 0xFC9F);
  }

  // 6. Blue diamond (rotated square)
  {
    int cx = 6, cy = 6;  // center
    int radius = 4;
    int x0 = cx, y0 = cy - radius;      // top
    int x1 = cx + radius, y1 = cy;      // right
    int x2 = cx, y2 = cy + radius;      // bottom  
    int x3 = cx - radius, y3 = cy;      // left
    sprite[5].fillTriangle(x0, y0, x1, y1, x2, y2, TFT_SKYBLUE);
    sprite[5].fillTriangle(x0, y0, x2, y2, x3, y3, TFT_SKYBLUE);
  }

  // 7. Grey square
  sprite[6].fillRect(0, 0, 12, 12, TFT_LIGHTGREY);

  while (WiFi.status() != WL_CONNECTED) {
    tft.print(".");
    pixel.setPixelColor(0, pixel.Color(255, 255, 0)); // Yellow
    pixel.show();
    delay(100);
    pixel.setPixelColor(0, pixel.Color(0, 0, 0)); // Off
    pixel.show();
    delay(100);
  }
  
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(15, 10);
  tft.print("Connected!");
  tft.setCursor(15, 40);
  tft.print(ssid);
  tft.setCursor(15, 65);
  tft.print(WiFi.localIP());
  if (psramFound()) {
    Serial.printf("PSRAM found: %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("PSRAM not found");
  }
  ArduinoOTA.setHostname("Reginald");
  ArduinoOTA.begin();
  initSNTP();
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
  }
  tft.setCursor(15, 90);
  tft.print(&timeinfo, "%Y-%m-%d %H:%M:%S");
  delay(1000);
  
  img.setColorDepth(16);  // Set color depth to 16 bits (RGB565)
  img.setPsram(true); 
  if (!img.createSprite(240, 320)) {
    tft.println("Failed to create sprite");
    while(1){ArduinoOTA.handle();}
  }
  

  //audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  //audio.setVolume(21); // 0...21
  
  if (!digitalRead(BTN_SETCH)) {
    while(1){ArduinoOTA.handle();}
  }
  doMLB();
}

void loop() {
  vTaskDelay(1);
  // audio.loop();
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }  //don't do Blynk unless wifi
  every(30000){
    doMLB();
  }
  

}
