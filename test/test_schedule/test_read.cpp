/**
 * Tests for ReadSchedule, exercised via ScheduleTask with NOTIFY_SCHEDULE_READ:
 *   1. Empty file: scheduleArray stays zeroed
 *   2. Multiple schedules: all entries parsed and stored correctly
 */

#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

// ─── Constants and types (mirror main.cpp) ───────────────────────────────────

#define SCHEDULE_ARRAY_SIZE 50
#define NOTIFY_SCHEDULE_READ 0b0001

const long gmt_offset = 25200;  // GMT+7

struct Schedule {
    long long id;
    time_t    startTime;
    uint16_t  duration;
    uint16_t  interval;
    int       intervalUnit;
};

Schedule scheduleArray[SCHEDULE_ARRAY_SIZE];

// ─── Minimal ArduinoJson fake ─────────────────────────────────────────────────
// Supports only the exact usage pattern in ReadSchedule: string-valued keys,
// accessed via doc["key"].as<T>()

struct JsonValue {
    char raw[64] = {};
    template<typename T> T as() const;
};
template<> long long JsonValue::as<long long>() const { return strtoll(raw, nullptr, 10); }
template<> time_t    JsonValue::as<time_t>()    const { return (time_t)strtoll(raw, nullptr, 10); }
template<> uint16_t  JsonValue::as<uint16_t>()  const { return (uint16_t)atoi(raw); }
template<> int       JsonValue::as<int>()       const { return atoi(raw); }

struct JsonDocument {
    struct Pair { char key[32]; char val[64]; };
    Pair pairs[8];
    int  count = 0;
    void clear() { count = 0; }
    JsonValue operator[](const char* key) const {
        JsonValue v = {};
        for (int i = 0; i < count; i++)
            if (strcmp(pairs[i].key, key) == 0) { strncpy(v.raw, pairs[i].val, 63); break; }
        return v;
    }
};

struct DeserializationError {
    bool failed = false;
    explicit operator bool() const { return failed; }
};

// ─── In-memory file fake ──────────────────────────────────────────────────────

struct FakeFile {
    const char* content = "";
    int         pos     = 0;
    bool available() const { return content[pos] != '\0'; }
};

FakeFile g_scheduleFile;

struct FakeLittleFS_t {
    FakeFile* open(const char* /*path*/, const char* /*mode*/) {
        g_scheduleFile.pos = 0;
        return &g_scheduleFile;
    }
} FakeLittleFS;

// Reads one JSON object per line from FakeFile (matches the one-object-per-line
// format written by AddSchedule in main.cpp)
DeserializationError deserializeJson(JsonDocument& doc, FakeFile& file) {
    DeserializationError err;
    doc.clear();

    while (file.available() && (file.content[file.pos] == '\n' || file.content[file.pos] == '\r'))
        file.pos++;
    if (!file.available() || file.content[file.pos] != '{') { err.failed = true; return err; }

    const char* lineStart = file.content + file.pos;
    while (file.available() && file.content[file.pos] != '\n') file.pos++;

    char line[256] = {};
    int  lineLen   = (int)((file.content + file.pos) - lineStart);
    strncpy(line, lineStart, lineLen < 255 ? lineLen : 255);

    const char* p = line + 1;  // skip '{'
    while (*p && *p != '}') {
        while (*p == ',' || *p == ' ') p++;
        if (*p != '"') break;
        p++;
        char key[32] = {};
        int  ki = 0;
        while (*p && *p != '"' && ki < 31) key[ki++] = *p++;
        if (*p == '"') p++;
        if (*p != ':') break;
        p++;
        char val[64] = {};
        if (*p == '"') {
            p++;
            int vi = 0;
            while (*p && *p != '"' && vi < 63) val[vi++] = *p++;
            if (*p == '"') p++;
        }
        if (doc.count < 8) {
            strncpy(doc.pairs[doc.count].key, key, 31);
            strncpy(doc.pairs[doc.count].val, val, 63);
            doc.count++;
        }
    }
    if (doc.count == 0) err.failed = true;
    return err;
}

// ─── System under test (logic copied from main.cpp) ──────────────────────────

void ReadSchedule_logic()
{
    int c = 0;
    memset(scheduleArray, 0, sizeof(scheduleArray));
    FakeFile* f = FakeLittleFS.open("/schedule.txt", "r");
    if (!f) return;
    while (f->available() && c < SCHEDULE_ARRAY_SIZE) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *f);
        if (error) break;
        scheduleArray[c].id           = doc["id"].as<long long>();
        scheduleArray[c].startTime    = doc["startTime"].as<time_t>() + gmt_offset;
        scheduleArray[c].duration     = doc["duration"].as<uint16_t>();
        scheduleArray[c].interval     = doc["interval"].as<uint16_t>();
        scheduleArray[c].intervalUnit = doc["intervalUnit"].as<int>();
        c++;
    }
}

// Mirrors the relevant dispatch branch inside ScheduleTask
void ScheduleTask_logic(uint32_t notifValue)
{
    if (notifValue & NOTIFY_SCHEDULE_READ)
        ReadSchedule_logic();
}

// ─── Unity setUp / tearDown ───────────────────────────────────────────────────

void setUp(void)
{
    memset(scheduleArray, 0, sizeof(scheduleArray));
    g_scheduleFile.content = "";
    g_scheduleFile.pos     = 0;
}

void tearDown(void) {}

// ─── Tests ────────────────────────────────────────────────────────────────────

// 1. Empty file: scheduleArray stays fully zeroed after the task runs
void test_read_empty_file()
{
    g_scheduleFile.content = "";
    ScheduleTask_logic(NOTIFY_SCHEDULE_READ);
    for (int i = 0; i < SCHEDULE_ARRAY_SIZE; i++)
        TEST_ASSERT_EQUAL(0, scheduleArray[i].startTime);
}

// 2. Multiple schedules: each entry is fully parsed and stored in order
void test_read_multiple_schedules()
{
    g_scheduleFile.content =
        "{\"id\":\"1773964800\",\"startTime\":\"1774051200\",\"duration\":\"3600\",\"interval\":\"2\",\"intervalUnit\":\"1\"}\n"
        "{\"id\":\"1773964900\",\"startTime\":\"1774137600\",\"duration\":\"7200\",\"interval\":\"1\",\"intervalUnit\":\"2\"}\n";
    ScheduleTask_logic(NOTIFY_SCHEDULE_READ);

    TEST_ASSERT_EQUAL_INT64(1773964800LL,                      scheduleArray[0].id);
    TEST_ASSERT_EQUAL      (1774051200 + gmt_offset,           scheduleArray[0].startTime);
    TEST_ASSERT_EQUAL      (3600,                              scheduleArray[0].duration);
    TEST_ASSERT_EQUAL      (2,                                 scheduleArray[0].interval);
    TEST_ASSERT_EQUAL      (1,                                 scheduleArray[0].intervalUnit);

    TEST_ASSERT_EQUAL_INT64(1773964900LL,                      scheduleArray[1].id);
    TEST_ASSERT_EQUAL      (1774137600 + gmt_offset,           scheduleArray[1].startTime);
    TEST_ASSERT_EQUAL      (7200,                              scheduleArray[1].duration);
    TEST_ASSERT_EQUAL      (1,                                 scheduleArray[1].interval);
    TEST_ASSERT_EQUAL      (2,                                 scheduleArray[1].intervalUnit);

    TEST_ASSERT_EQUAL      (0,                                 scheduleArray[2].startTime);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_read_empty_file);
    RUN_TEST(test_read_multiple_schedules);
    return UNITY_END();
}
