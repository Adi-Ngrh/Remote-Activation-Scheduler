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


// to do :
//  - implement activation behaviour for repeat mode and always active schedule
//  - implement month and year checking for the schedule.
//  - evaluate wether state manager task is needed or should be completely replaced by schedule task
// bugs : 
// Global variables & definitions
#define SCHEDULE_ARRAY_SIZE 50
TaskHandle_t StateManagerTaskHandle = NULL;
TaskHandle_t ScheduleTaskHandle = NULL;

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
typedef enum{
  NOTIFY_SCHEDULE_READ = 0b0001,
  NOTIFY_SCHEDULE_SET_ALARM = 0b0010,
  ALARM_TRIGGERED = 0b0100  // important: due to bitwise accumulation, make sure each value has unique bit that is set (ex: dont use 0b0011 when 0b0001 and 0b0010 exist)
} scheduleTask_enum;

struct Schedule{
  int mode;
  int intervalUnit;
  time_t startTime;
  uint16_t duration;
  uint16_t interval;
};
struct Request{
  event_enum eventCategory;
  std::optional<Schedule> schedule;
};

QueueHandle_t requestQueue;
QueueHandle_t responseQueue;
Schedule scheduleArray[SCHEDULE_ARRAY_SIZE];


// WiFi related functions
void InitWifi();
// WebServer related functions
void StoreSchedule(AsyncWebServerRequest* pRequest);
void DeleteSchedule(AsyncWebServerRequest* pRequest);
int RemoveSchedule(String idToDelete);
void ReceiveData(AsyncWebServerRequest* pRequest, uint8_t* pData, size_t len, size_t index, size_t total);
void InitWebServer();
// LittleFS related functions
void InitLittleFS();
void ReadSchedule();
time_t ExtractStartTime(JsonDocument& doc);
// NTP related functions
void InitNTP();
// DS3231 related functions
void InitRTC();
void SetAlarm();
void IRAM_ATTR onAlarmISR();
// Device related functions
void TurnOnDevice();
void TurnOffDevice();
void StateManagerTask(void* pvParams);
void ScheduleTask(void* pvParams);




void setup() 
{
  Serial.begin(115200);
  InitWifi();
  InitLittleFS();
  InitNTP();
  InitRTC();
  InitWebServer();
  pinMode(devicePin, OUTPUT);
  pinMode(sqw_pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(sqw_pin), onAlarmISR, FALLING);

  requestQueue = xQueueCreate(1, sizeof(Request));
  if (requestQueue == NULL)
  {
    Serial.println("Request Queue Failed!");
  }

  responseQueue = xQueueCreate(1, sizeof(state_enum));
  if (responseQueue == NULL)
  {
    Serial.println("Response Queue Failed!");
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
    ScheduleTask,
    "Schedule Task",
    4096,
    NULL,
    1,
    &ScheduleTaskHandle,
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
void StoreSchedule(AsyncWebServerRequest* pRequest)
{
  File scheduleFile = LittleFS.open("/schedule.txt", "a");
  if (!scheduleFile)
  {
    Serial.println("Schedule File Missing Or Can't Be Opened");
    pRequest->send(404, "text/plain", "schedule file is missing!");
    return;
  }
  scheduleFile.println(receivedData);
  scheduleFile.close();

  // confirmation
  scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule File Missing Or Can't Be Opened");
    return;
  }
  Serial.println("FILE START (store)");
  while (scheduleFile.available())
  {
    Serial.println(scheduleFile.readStringUntil('\n'));
  }
  Serial.println("FILE END (store)");
  scheduleFile.close();
  pRequest->send(200, "text/plain", "schedule is sent and stored!");
  xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_READ, eSetBits);
  xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_SET_ALARM, eSetBits);
}

void DeleteSchedule(AsyncWebServerRequest* pRequest)
{
  receivedData.replace("\"", ""); // get rid of literal quote (\") being sent by JSON.stringify()
  int status = RemoveSchedule(receivedData);
  switch (status)
  {
    case 1:
      pRequest->send(404, "text/plain", "file cant be opened!");
      break;
    default:
      break;
  }

  // confirmation
  File scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule File Missing Or Can't Be Opened");
    return;
  }
  Serial.println("FILE START (delete)");
  while (scheduleFile.available())
  {
    Serial.println(scheduleFile.readStringUntil('\n'));
  }
  Serial.println("FILE END (delete)");
  scheduleFile.close();
  pRequest->send(200, "text/plain", "schedule is deleted!");
  xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_READ, eSetBits);
  xTaskNotify(ScheduleTaskHandle, NOTIFY_SCHEDULE_SET_ALARM, eSetBits);
}

int RemoveSchedule(String idToDelete)
{
  File original = LittleFS.open("/schedule.txt", "r");
  if (!original)
  {
    Serial.println("Schedule File Cant Be Opened!");
    return 1;
  }
  File temp = LittleFS.open("/schedule_temp.txt", "w");
  if (!temp)
  {
    Serial.println("Temp File Cant Be Opened!");
    return 1;
  }
  while (original.available())
  {
    JsonDocument scheduleDoc;
    DeserializationError error = deserializeJson(scheduleDoc, original);
    if (error)
    {
      break;  
    }
    if (scheduleDoc["id"].as<String>() != idToDelete)
    {
      String scheduleString;
      serializeJson(scheduleDoc, scheduleString);
      temp.println(scheduleString);
    }
  }
  original.close();
  temp.close();
  LittleFS.remove("/schedule.txt");
  LittleFS.rename("/schedule_temp.txt", "/schedule.txt");
  Serial.println("Schedule : " + idToDelete + " successfully removed!");
  return 0;
}

void ReceiveData(AsyncWebServerRequest* pRequest, uint8_t* pData, size_t len, size_t index, size_t total)
{
  if (index == 0)
  {
    receivedData = "";
    receivedData.reserve(total);
  }
  for (size_t i = 0; i < len; i++)
  {
    receivedData += (char)pData[i];
  }
}

void InitWebServer()
{
  // GET / : client ask to display web page, esp32 respond by sending html file
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* pRequest) {
    if (!LittleFS.exists("/index.html"))
    {
      pRequest->send(404, "text/plain");
      return; 
    }
    pRequest->send(LittleFS, "/index.html", "text/html");
  });
  // POST /upload : client send schedule data, esp32 respond with handlers to store it
  webServer.on("/upload", HTTP_POST, StoreSchedule, NULL, ReceiveData); // onRequest run after data fully transferred, onUpload and onBody run during transfer
  // GET /update : client ask to get all schedules, esp32 respond by sending schedult.txt
  webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest* pRequest) {
    if (!LittleFS.exists("/schedule.txt"))
    {
      pRequest->send(404, "application/json", "[]");
      return;
    }
    pRequest->send(LittleFS, "/schedule.txt", "text/plain");
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
    Serial.println("LittleFS Failed To Initialized");
    delay(1000);
  } 
  Serial.println("LittleFS Initialized");
  delay(1000);

  // sanity check
  File testFile = LittleFS.open("/test.txt", "r");
  if (!testFile)
  {
    Serial.println("Test File Missing Or Can't Be Opened");
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
  memset(scheduleArray, 0, sizeof(scheduleArray));
  File scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule File Missing Or Can't Be Opened");
  }
  while (scheduleFile.available() && c < SCHEDULE_ARRAY_SIZE)
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
    c++;
  }
  scheduleFile.close();

  // confirmation
  for (int i = 0; i < SCHEDULE_ARRAY_SIZE; i++)
  {
    if (scheduleArray[i].startTime)
    {
      Serial.println(String(scheduleArray[i].mode) + "===" + String(scheduleArray[i].startTime) + "===" + String(scheduleArray[i].duration));
    }
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
  epoch += gmt_offset;  // mktime() assuming the struct tm is localized. dont forget to add the result with the timezone offset
  return epoch;
}


// NTP related functions
void InitNTP()
{
  configTime(gmt_offset, daylight_offset, ntp_server);
  Serial.println("NTP Initializdd, Using GMT+" + String(gmt_offset / 3600));
  delay(1000);

  // sanity check
  struct tm dateTime;
  getLocalTime(&dateTime);
  Serial.println(
    String("From NTP : ") + 
    dateTime.tm_year + String(":") + 
    dateTime.tm_mon + String(":") +
    dateTime.tm_mday + String(":") +
    dateTime.tm_wday + String(" ; ") +
    dateTime.tm_hour + String(":") + 
    dateTime.tm_min
  );
  delay(1000);
}


// DS3231 related functions
void InitRTC()
{
  while (!Wire.begin(sda_pin, scl_pin, 100000))
  {
    Serial.println("I2C Failed To Initialized");
    delay(1000);
  }
  Serial.println("I2C Initialized");

  while (!rtc.begin())
  {
    Serial.println("RTC Not Found");
    delay(1000);
  }
  Serial.println("RTC Initialized");
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
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disable32K();
  delay(1000);
}

void SetAlarm()
{
  int closestSchedule = 0;
  if (scheduleArray != NULL)
  {
    for (int i = 0; i < SCHEDULE_ARRAY_SIZE; i++)
    {
      if (scheduleArray[i].startTime != NULL && scheduleArray[i].startTime < scheduleArray[closestSchedule].startTime)      
      {
        closestSchedule = i;
      }
    }
    Serial.println("Closest Schedule : ");
    Serial.println(String(scheduleArray[closestSchedule].mode) + "|||" + String(scheduleArray[closestSchedule].startTime) + "|||" + String(scheduleArray[closestSchedule].duration));
    // set start time to alarm2
    DateTime startTime = DateTime(scheduleArray[closestSchedule].startTime);
    char buffer[] = "YYYY/MM/DD hh:mm:ss";
    Serial.println(startTime.toString(buffer));
    rtc.setAlarm2(startTime, DS3231_A2_Date);
    // set duration to alarm1
    time_t finishTimeEpoch = scheduleArray[closestSchedule].startTime + scheduleArray[closestSchedule].duration;
    DateTime finishTime = DateTime(finishTimeEpoch);
    char buffer2[] = "YYYY/MM/DD hh:mm:ss";
    Serial.println(finishTime.toString(buffer2));
    rtc.setAlarm1(finishTime, DS3231_A1_Date);
    delay(10);
    rtc.clearAlarm(2);
    rtc.clearAlarm(1);
  }
  else
  {
    Serial.println("No Schedule");
  }
}

void IRAM_ATTR onAlarmISR()
{
  if(digitalRead(sqw_pin) == LOW)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    // important: eSetBits will append multiple bits into one atomic state (bitwise accumulation, ex: 0b0001 and 0b0010 sent back-to-back is the same as 0b0011 sent alone)
    xTaskNotifyFromISR(ScheduleTaskHandle, ALARM_TRIGGERED, eSetBits, &xHigherPriorityTaskWoken); 
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


// Handle program state transition based on event value on Request struct
void StateManagerTask(void* pvParams)
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
  }
}


// Handle RTC alarm setting and time keeping
void ScheduleTask(void* pvParams)
{
  uint32_t notifValue;
  while(true)
  {
    if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notifValue, portMAX_DELAY) == pdPASS)
    {
      if (notifValue & NOTIFY_SCHEDULE_READ) 
      {
        ReadSchedule();
        Serial.println("Read Schedules");
      }
      
      if (notifValue & NOTIFY_SCHEDULE_SET_ALARM)
      {
        SetAlarm();
        Serial.println("Set Alarm");
      }
      
      if (notifValue & ALARM_TRIGGERED)
      {
        if (rtc.alarmFired(2))
        {
          TurnOnDevice();
          Serial.println("Device Turned On");
          rtc.clearAlarm(2);
        }
        else if (rtc.alarmFired(1))
        {
          TurnOffDevice();
          Serial.println("Device Turned Off");
          rtc.clearAlarm(1);
        }
        SetAlarm();
      }
    }
  }
}


// Test Functions =====================================================

// Test Functions End =====================================================