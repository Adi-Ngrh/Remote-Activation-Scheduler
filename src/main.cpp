#include <Arduino.h>
#include "optional"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "LittleFS.h"       // for using littleFS file system on esp32
#include "WiFi.h"  
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "HTTPClient.h"
#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "time.h"           //for connecting to NTP server
#include "Wire.h"           // for I2C
#include "RTClib.h"         // for interacting with RS3231
static_assert(__cplusplus >= 201703L, "C++17 not enabled"); // confirm the use of c++17



// to do : handle deserializeJson() explicitly for important errors
// Global variables & definitions
#define SCHEDULE_ARRAY_SIZE 50
xTaskHandle StateManagerTaskHandle = NULL;
xTaskHandle CheckTimeTaskHandle = NULL;
typedef enum {
  STATE_IDLE,
  STATE_ACTIVE,
  STATE_CONFIG,
  STATE_ERROR
} state_enum;
typedef enum{
  EVENT_DEVICE_ON,
  EVENT_DEVICE_OFF,
  EVENT_DEVICE_FAIL,
  EVENT_WIFI_DISCONNECT,
  EVENT_NTP_FAIL,
  EVENT_RTC_FAIL,
  EVENT_ASK_STATE
} event_enum;
struct Schedule{
  int mode;
  time_t startTime;
  uint16_t duration;
};
struct Request{
  event_enum eventCategory;
  std::optional<Schedule> extra;
};
QueueHandle_t requestQueue;
QueueHandle_t responseQueue;
Schedule scheduleArray[SCHEDULE_ARRAY_SIZE];

// WiFi & website related variables
// ensure WiFi use 2,4 GHz and use WPA2 for compatibility
const char* wifi_ssid = "punya orang";
const char* wifi_password = "b57aigqs";
AsyncWebServer webServer(80);
String receivedData = "";
HTTPClient http;

// NTP related variables
const char* ntp_server = "pool.ntp.org";
const long gmt_offset = 25200; // GMT+7 (3600 x 7)
const int daylight_offset = 0;

// RS3231 related variables
RTC_DS3231 rtc;
DateTime currentDateTime;
const uint8_t sqw_pin = 7;
const uint8_t sda_pin = 8;
const uint8_t scl_pin = 9;
const uint8_t rtc_addr = 0x68;

// Device related variables
const uint8_t devicePin = 16;
bool isDeviceActive = false;



// WiFi related functions
void InitWifi();
// WebServer related functions
void StoreSchedule(AsyncWebServerRequest* request);
void DeleteSchedule(AsyncWebServerRequest* request);
void ReceiveData(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
void InitWebServer();
// LittleFS related functions
void InitLittleFS();
void ReadSchedule();
time_t ExtractStartTime(JsonDocument& doc);
// NTP related functions
void InitNTP();
// RS3231 related functions
void InitRTC();
// Device related functions
void TurnOnDevice();
void TurnOffDevice();
void StateManagerTask(void* params);



void setup() 
{
  Serial.begin(115200);
  pinMode(16, OUTPUT);
  InitWifi();
  InitLittleFS();
  InitNTP();
  InitRTC();
  InitWebServer();

  requestQueue = xQueueCreate(1, sizeof(Request));
  if (requestQueue == NULL)
  {
    Serial.println("request queue failed!");
  }

  responseQueue = xQueueCreate(1, sizeof(state_enum));
  if (responseQueue == NULL)
  {
    Serial.println("response queue failed!");
  }

  xTaskCreatePinnedToCore(
    StateManagerTask,
    "State Manager Task",
    2048,
    NULL,
    1,
    &StateManagerTaskHandle,
    0
  );
  xTaskCreatePinnedToCore(
    CheckTimeTask,
    "Check Time Task",
    2048,
    NULL,
    1,
    &CheckTimeTaskHandle,
    1
  );
}

void loop() 
{ 
}



// WiFi related functions
void InitWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println(String(WiFi.status()));
    delay(1000);
  }
  Serial.println("Connected to : " + String(wifi_ssid));
  delay(1000);
}


// WebServer related functions
void StoreSchedule(AsyncWebServerRequest* request)
{
  File scheduleFile = LittleFS.open("/schedule.txt", "a");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
    request->send(404, "text/plain", "schedule file is missing!");
    return;
  }
  scheduleFile.println(receivedData);
  scheduleFile.close();

  // confirmation
  scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
    return;
  }
  Serial.println("FILE START");
  while (scheduleFile.available())
  {
    Serial.println(scheduleFile.readStringUntil('\n'));
  }
  Serial.println("FILE END");
  scheduleFile.close();
  request->send(200, "text/plain", "schedule is sent and stored!");
}

void DeleteSchedule(AsyncWebServerRequest* request)
{
  File scheduleFileOriginal = LittleFS.open("/schedule.txt", "r");
  File scheduleFileTemp = LittleFS.open("/schedule_temp.txt", "w");
  if (!scheduleFileOriginal)
  {
    Serial.println("Schedule file missing or can't be opened");
    request->send(404, "text/plain", "schedule file is missing!");
    return;
  }
  while (scheduleFileOriginal.available())
  {
    JsonDocument scheduleDoc;
    DeserializationError error = deserializeJson(scheduleDoc, scheduleFileOriginal);
    if (error)
    {
      break;  
    }
    receivedData.replace("\"", "");   // get rid of literal quote (\") being sent by JSON.stringify()
    if (scheduleDoc["id"].as<String>() != receivedData)
    {
      String scheduleString;
      serializeJson(scheduleDoc, scheduleString);
      scheduleFileTemp.println(scheduleString);
    }
  }
  scheduleFileOriginal.close();
  scheduleFileTemp.close();
  LittleFS.remove("/schedule.txt");
  LittleFS.rename("/schedule_temp.txt", "/schedule.txt");

  // confirmation
  File scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
    return;
  }
  Serial.println("FILE START");
  while (scheduleFile.available())
  {
    Serial.println(scheduleFile.readStringUntil('\n'));
  }
  Serial.println("FILE END");
  scheduleFile.close();
  request->send(200, "text/plain", "schedule is deleted!");
}

void ReceiveData(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total)
{
  if (index == 0)
  {
    receivedData = "";
    receivedData.reserve(total);
  }
  for (size_t i = 0; i < len; i++)
  {
    receivedData += (char)data[i];
  }
}

void InitWebServer()
{
  // GET / : client ask to display web page, esp32 respond by sending html file
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!LittleFS.exists("/index.html"))
    {
      request->send(404, "text/plain");
      return; 
    }
    request->send(LittleFS, "/index.html", "text/html");
  });
  // POST /upload : client send schedule data, esp32 respond with handlers to store it
  webServer.on("/upload", HTTP_POST, StoreSchedule, NULL, ReceiveData); // onRequest run after data fully transferred, onUpload and onBody run during transfer
  // GET /update : client ask to get all schedules, esp32 respond by sending schedult.txt
  webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!LittleFS.exists("/schedule.txt"))
    {
      request->send(404, "application/json", "[]");
      return;
    }
    request->send(LittleFS, "/schedule.txt", "text/plain");
  });
  // POST /delete : client send schedule id to be deleted, esp32 delete that schedule and send confirmation
  webServer.on("/delete", HTTP_POST, DeleteSchedule, NULL, ReceiveData);
  webServer.begin();
  Serial.println("IP Address : " + WiFi.localIP().toString());
  delay(1000);
}


// LittleFS related functions
void InitLittleFS()
{
  while (!LittleFS.begin(false))  // do not immedietly format upon mounting fail
  {
    Serial.println("LittleFS failed to initialized");
    delay(1000);
  } 
  Serial.println("LittleFS initialized");
  delay(1000);

  // sanity check
  File testFile = LittleFS.open("/test.txt", "r");
  if (!testFile)
  {
    Serial.println("Test file missing or can't be opened");
  }
  Serial.print("Test file : ");
  while (testFile.available())
  {
    Serial.write(testFile.read());  // file.read() get raw byte one at time, serial.write() display the byte (work with byte only)
  }
  Serial.print("\n");
  testFile.close();
  delay(1000);
}

void ReadSchedule()
{
  int c = 0;
  File scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
  }
  while (scheduleFile.available())
  {
    JsonDocument scheduleDoc;
    DeserializationError error = deserializeJson(scheduleDoc, scheduleFile);
    if(error)
    {
      break;
    }
    scheduleArray[c].mode = scheduleDoc["mode"];
    scheduleArray[c].startTime = ExtractStartTime(scheduleDoc);
    scheduleArray[c].duration = scheduleDoc["duration"];
  }
}

time_t ExtractStartTime(JsonDocument& doc)
{
  const char* dateStr = doc["date"];
  const char* timeStr = doc["time"];
  // extract start date
  int year, month, mday;
  sscanf(dateStr, "%d-%d-%d", &year, &month, &mday);
  // extract start time
  int hour, minute;
  sscanf(timeStr, "%d:%d", &hour, &minute);
  // store result
  struct tm t = {0};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = mday;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  // convert to epoch for easier processing
  time_t epoch = mktime(&t);
  return epoch;
}


// NTP related functions
void InitNTP()
{
  configTime(gmt_offset, daylight_offset, ntp_server);
  Serial.println("NTP initializdd, using GMT+" + String(gmt_offset / 3600));
  delay(1000);

  // sanity check
  struct tm dateTime;
  getLocalTime(&dateTime);
  Serial.println(
    String("from NTP : ") + 
    dateTime.tm_year + String(":") + 
    dateTime.tm_mon + String(":") +
    dateTime.tm_mday + String(":") +
    dateTime.tm_wday + String(" ; ") +
    dateTime.tm_hour + String(":") + 
    dateTime.tm_min
  );
  delay(1000);
}


// RS3231 related functions
void InitRTC()
{
  while (!Wire.begin(sda_pin, scl_pin, 100000))
  {
    Serial.println("I2C failed to initialized");
    delay(1000);
  }
  Serial.println("I2C initialized");

  while (!rtc.begin())
  {
    Serial.println("RTC not found");
    delay(1000);
  }
  Serial.println("RTC initialized");
  rtc.disable32K();

  struct tm dateTime;
  getLocalTime(&dateTime);
  time_t dateTimeEpoch = mktime(&dateTime); // convert to absolute time in seconds
  dateTimeEpoch += gmt_offset;    // applied back offset
  rtc.adjust(DateTime(dateTimeEpoch));
  delay(1000);

  // sanity check
  currentDateTime = rtc.now();
  Serial.println(
    String("from RTC : ") + 
    currentDateTime.year() + String(":") + 
    currentDateTime.month() + String(":") +
    currentDateTime.day() + String(":") +
    currentDateTime.dayOfTheWeek() + String(" ; ") +
    currentDateTime.hour() + String(":") + 
    currentDateTime.minute()
  );
  delay(1000);
}

void CheckTimeTask(void* params)
{
  while(true)
  {
    rtc.now();
  }
}


// Device related functions
void TurnOnDevice()
{
  isDeviceActive = true;
  digitalWrite(16, HIGH);
}

void TurnOffDevice()
{
  isDeviceActive = false;
  digitalWrite(16, LOW);
} 


// State manager
void StateManagerTask(void* params)
{
  state_enum currentState = STATE_IDLE;
  Request request;

  while (true)
  {
    if (xQueueReceive(requestQueue, &request, portMAX_DELAY))
    {
      switch (request.eventCategory)
      {
        case EVENT_DEVICE_ON :
          // call function to turn on device
          TurnOnDevice();
          currentState = STATE_ACTIVE;
          break;

        case EVENT_DEVICE_OFF :
          // call function to turn off device
          TurnOffDevice();
          currentState = STATE_IDLE;
          break;

        case EVENT_DEVICE_FAIL :
          // call function to handle runtime device failure
          currentState = STATE_ERROR;
          break;

        case EVENT_WIFI_DISCONNECT :
          // call function to handle runtime wifi disconnect
          currentState = STATE_ERROR;
          break;

        case EVENT_NTP_FAIL :
          // call function to handle runtime NTP failure
          currentState = STATE_ERROR;
          break;

        case EVENT_RTC_FAIL :
          // call function to handle runtime RTC failure
          currentState = STATE_ERROR;
          break;

        case EVENT_ASK_STATE :
          xQueueSend(responseQueue, &currentState, pdMS_TO_TICKS(500));
          break;

        default :
          currentState = STATE_IDLE;
      }
    }
    vTaskDelay(pdTICKS_TO_MS(500));
  }
}


// Test Functions =====================================================

// Test Functions End =====================================================