/**
 * test_storing.cpp
 *
 * Tests for ReceiveData and StoreSchedule logic (the ESP32 side of /upload).
 *
 * Why not test webServer.on("/upload", ...) directly?
 *   ESPAsyncWebServer is ESP32-specific and cannot compile/run on native (PC).
 *   Testing the library's routing wiring is not our responsibility.
 *   What matters is: given a valid HTTP body, does ESP32 buffer it correctly?
 *   And given a buffered payload, does it get written to the file correctly?
 *   These are tested here in isolation with minimal fakes.
 *
 * Payload format (from OnSubmitForm() in index.html):
 *   JSON.stringify() is called on an object where ALL values are strings:
 *     { id, startTime, duration, interval, intervalUnit }
 *
 *   id          – epoch seconds when the form was submitted
 *   startTime   – epoch seconds of chosen start date+time (browser local → UTC)
 *   duration    – total seconds: ((days*24+hours)*60+minutes)*60+seconds
 *   interval    – repeat count (numeric string from <input type="number">)
 *   intervalUnit– "0"=minutes, "1"=hours, "2"=days
 *
 *   Example (user schedules March 21 2026 00:00 UTC, 1-hour ON, repeat every 2 hours):
 *     {"id":"1773964800","startTime":"1774051200","duration":"3600","interval":"2","intervalUnit":"1"}
 *
 * Test cases:
 *   ReceiveData:
 *     1. Single-chunk: complete schedule JSON arrives in one HTTP body chunk
 *     2. Multi-chunk: JSON is split across two TCP segments (realistic on slow links)
 *     3. Max legal size: exactly 199-byte payload (BUFFER_SIZE-1) is accepted
 *     4. Oversized: payload >= BUFFER_SIZE is silently rejected, buffer untouched
 *     5. New request clears leftovers: second schedule replaces first correctly
 *
 *   StoreSchedule:
 *     6. Valid schedule JSON is appended to the schedule file
 *     7. Responds 404 and writes nothing when file cannot be opened
 */

#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// ─── Constants (mirror main.cpp) ───────────────────────────────────────────

#define BUFFER_SIZE 200

// ─── Realistic test payload constants ───────────────────────────────────────
// These are the exact bytes the browser sends via JSON.stringify(scheduleObj).
//
// SCHEDULE_A: submitted on March 20 2026 (~epoch 1773964800),
//             starts  March 21 2026 00:00 UTC (epoch 1774051200),
//             ON for 1 hour (3600 s), repeats every 2 hours.
//             Total JSON length: 96 bytes — well within BUFFER_SIZE 200.
#define SCHEDULE_A \
    "{\"id\":\"1773964800\",\"startTime\":\"1774051200\",\"duration\":\"3600\",\"interval\":\"2\",\"intervalUnit\":\"1\"}"

// SCHEDULE_B: a second schedule for the buffer-clear test.
//             Starts March 22 2026 00:00 UTC (epoch 1774137600),
//             ON for 2 hours (7200 s), repeats every 1 day.
#define SCHEDULE_B \
    "{\"id\":\"1773964900\",\"startTime\":\"1774137600\",\"duration\":\"7200\",\"interval\":\"1\",\"intervalUnit\":\"2\"}"

// Multi-chunk split of SCHEDULE_A: cut after the closing quote of startTime's
// value (byte index 43 of 96). This mirrors how a slow TCP connection might
// split an HTTP body across two recv() calls.
#define SCHEDULE_A_CHUNK1 \
    "{\"id\":\"1773964800\",\"startTime\":\"1774051200\""
#define SCHEDULE_A_CHUNK2 \
    ",\"duration\":\"3600\",\"interval\":\"2\",\"intervalUnit\":\"1\"}"

// ─── Minimal fakes (no real Arduino / LittleFS / AsyncWebServer needed) ────

// Fake File — backed by an in-memory string
struct FakeFile {
    char  content[1024];
    int   pos;
    bool  open;

    void reset() { memset(content, 0, sizeof(content)); pos = 0; open = false; }
    bool available() const { return open && content[pos] != '\0'; }
    // mimic File::println — append text + newline
    void println(const char* s) { int n = strlen(s); strcat(content, s); strcat(content, "\n"); }
};

FakeFile g_scheduleFile;
bool     g_scheduleFileOpenFails = false;   // toggle to simulate open failure

// Fake LittleFS — only the subset used by StoreSchedule
struct FakeLittleFS_t {
    FakeFile* open(const char* /*path*/, const char* mode) {
        if (g_scheduleFileOpenFails) return nullptr;
        if (mode[0] == 'a' || mode[0] == 'w') {
            g_scheduleFile.open = true;
        } else if (mode[0] == 'r') {
            g_scheduleFile.pos  = 0;
            g_scheduleFile.open = true;
        }
        return &g_scheduleFile;
    }
} FakeLittleFS;

// Fake AsyncWebServerRequest — records the last send() call
struct FakeRequest {
    int         lastCode    = 0;
    const char* lastBody    = nullptr;

    void send(int code, const char* /*contentType*/, const char* body = "") {
        lastCode = code;
        lastBody = body;
    }
};

// ─── System under test: logic copied verbatim from main.cpp ────────────────
// We duplicate only the two functions so:
//   a) No ESP32 headers are needed
//   b) We can swap LittleFS / request for fakes via the pointers below

// Shared buffer that ReceiveData writes into (mirrors main.cpp global)
char receivedData[BUFFER_SIZE];

// ---- ReceiveData logic (identical to main.cpp) ----------------------------
void ReceiveData_logic(const uint8_t* pData, size_t len, size_t index, size_t total)
{
    if (total >= BUFFER_SIZE) {
        return;                         // reject oversized payload
    }
    if (index == 0) {
        memset(receivedData, 0, BUFFER_SIZE);
    }
    if (index + len < BUFFER_SIZE) {
        memcpy(receivedData + index, pData, len);
        if (index + len == total) {
            receivedData[total] = '\0';
        }
    }
}

// ---- StoreSchedule logic (identical to main.cpp, fakes injected) ----------
int StoreSchedule_logic(FakeRequest* req)
{
    FakeFile* f = FakeLittleFS.open("/schedule.txt", "a");
    if (!f) {
        req->send(404, "text/plain", "schedule file is missing!");
        return 404;
    }
    f->println(receivedData);
    f->open = false;            // mimic close()

    req->send(200, "text/plain", "schedule is sent and stored!");
    return 200;
}

// ─── Unity setUp / tearDown ─────────────────────────────────────────────────

void setUp(void)
{
    memset(receivedData, 0, BUFFER_SIZE);
    g_scheduleFile.reset();
    g_scheduleFileOpenFails = false;
}

void tearDown(void) {}

// ─── Tests ──────────────────────────────────────────────────────────────────

// 1. Single-chunk: full schedule JSON arrives in one piece (common case)
void test_receive_single_chunk()
{
    const char* payload = SCHEDULE_A;
    size_t len = strlen(payload);
    ReceiveData_logic((const uint8_t*)payload, len, 0, len);
    TEST_ASSERT_EQUAL_STRING(SCHEDULE_A, receivedData);
}

// 2. Multi-chunk: JSON split across two TCP segments
//    chunk1 = everything up to and including the startTime value
//    chunk2 = duration, interval, intervalUnit and closing brace
void test_receive_multi_chunk()
{
    const char* chunk1 = SCHEDULE_A_CHUNK1;
    const char* chunk2 = SCHEDULE_A_CHUNK2;
    size_t len1  = strlen(chunk1);
    size_t len2  = strlen(chunk2);
    size_t total = len1 + len2;

    ReceiveData_logic((const uint8_t*)chunk1, len1, 0,    total);
    ReceiveData_logic((const uint8_t*)chunk2, len2, len1, total);

    TEST_ASSERT_EQUAL_STRING(SCHEDULE_A, receivedData);
}

// 3. Max legal size: a 199-byte payload (one byte below BUFFER_SIZE) is accepted.
//    Normal schedules are ~96 bytes; this tests the upper safety boundary.
//    Such a payload could appear if fields are expanded in a future protocol version.
void test_receive_max_legal_size()
{
    char bigPayload[BUFFER_SIZE];           // 200-element array
    memset(bigPayload, 'A', BUFFER_SIZE - 1);
    bigPayload[BUFFER_SIZE - 1] = '\0';     // 199 'A' chars + null terminator

    size_t len   = BUFFER_SIZE - 1;         // 199 — passes the total >= BUFFER_SIZE guard
    size_t total = len;
    ReceiveData_logic((const uint8_t*)bigPayload, len, 0, total);
    TEST_ASSERT_EQUAL_STRING(bigPayload, receivedData);
}

// 4. Oversized: payload >= BUFFER_SIZE is silently rejected, buffer stays zeroed.
//    This guards against injection attacks or future protocol mismatches.
void test_receive_oversized_rejected()
{
    char bigPayload[BUFFER_SIZE + 10];
    memset(bigPayload, 'B', sizeof(bigPayload) - 1);
    bigPayload[sizeof(bigPayload) - 1] = '\0';

    size_t len   = strlen(bigPayload);      // > BUFFER_SIZE → must be rejected
    size_t total = len;
    ReceiveData_logic((const uint8_t*)bigPayload, len, 0, total);

    char zeroBuf[BUFFER_SIZE];
    memset(zeroBuf, 0, BUFFER_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(zeroBuf, receivedData, BUFFER_SIZE);
}

// 5. New request clears previous schedule: user submits SCHEDULE_A, then
//    immediately submits SCHEDULE_B. Buffer must contain only SCHEDULE_B.
void test_receive_clears_buffer_on_new_transfer()
{
    // First request: SCHEDULE_A is fully received
    const char* first  = SCHEDULE_A;
    size_t len1 = strlen(first);
    ReceiveData_logic((const uint8_t*)first, len1, 0, len1);
    TEST_ASSERT_EQUAL_STRING(SCHEDULE_A, receivedData);     // sanity check

    // Second request (index==0 signals start of new body): SCHEDULE_B arrives
    const char* second = SCHEDULE_B;
    size_t len2 = strlen(second);
    ReceiveData_logic((const uint8_t*)second, len2, 0, len2);

    // Buffer must hold SCHEDULE_B exactly
    TEST_ASSERT_EQUAL_STRING(SCHEDULE_B, receivedData);
    // SCHEDULE_A's id must not be anywhere in the buffer
    TEST_ASSERT_NULL(strstr(receivedData, "\"id\":\"1773964800\""));
}

// 6. StoreSchedule appends the schedule JSON into the schedule file
void test_store_schedule_writes_to_file()
{
    strcpy(receivedData, SCHEDULE_A);

    FakeRequest req;
    int result = StoreSchedule_logic(&req);

    TEST_ASSERT_EQUAL(200, result);
    TEST_ASSERT_EQUAL(200, req.lastCode);
    // File content must contain every key field from the schedule
    TEST_ASSERT_NOT_NULL(strstr(g_scheduleFile.content, "\"id\":\"1773964800\""));
    TEST_ASSERT_NOT_NULL(strstr(g_scheduleFile.content, "\"startTime\":\"1774051200\""));
    TEST_ASSERT_NOT_NULL(strstr(g_scheduleFile.content, "\"duration\":\"3600\""));
    TEST_ASSERT_NOT_NULL(strstr(g_scheduleFile.content, "\"interval\":\"2\""));
    TEST_ASSERT_NOT_NULL(strstr(g_scheduleFile.content, "\"intervalUnit\":\"1\""));
}

// 7. StoreSchedule responds 404 and writes nothing when file cannot be opened
void test_store_schedule_file_open_fail()
{
    strcpy(receivedData, SCHEDULE_A);
    g_scheduleFileOpenFails = true;

    FakeRequest req;
    int result = StoreSchedule_logic(&req);

    TEST_ASSERT_EQUAL(404, result);
    TEST_ASSERT_EQUAL(404, req.lastCode);
    TEST_ASSERT_EQUAL(0, (int)g_scheduleFile.content[0]);  // nothing written
}

// ─── main ───────────────────────────────────────────────────────────────────

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_receive_single_chunk);
    RUN_TEST(test_receive_multi_chunk);
    RUN_TEST(test_receive_max_legal_size);
    RUN_TEST(test_receive_oversized_rejected);
    RUN_TEST(test_receive_clears_buffer_on_new_transfer);
    RUN_TEST(test_store_schedule_writes_to_file);
    RUN_TEST(test_store_schedule_file_open_fail);
    return UNITY_END();
}
