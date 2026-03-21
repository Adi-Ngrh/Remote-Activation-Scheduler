/**
 * Tests for ReceiveData (mirrors main.cpp logic, no ESP32 headers needed):
 *   1. Single-chunk: full JSON arrives in one HTTP body chunk
 *   2. Multi-chunk:  JSON split across two TCP segments
 *   3. Oversized:    payload >= BUFFER_SIZE is rejected, buffer stays zeroed
 */

#include <unity.h>
#include <string.h>
#include <stdint.h>

// ─── Constants (mirror main.cpp) ────────────────────────────────────────────

#define BUFFER_SIZE 200

// Sample schedule JSON as sent by JSON.stringify() in index.html
#define SCHEDULE_A \
    "{\"id\":\"1773964800\",\"startTime\":\"1774051200\",\"duration\":\"3600\",\"interval\":\"2\",\"intervalUnit\":\"1\"}"

// SCHEDULE_A split at byte 43 (after startTime value), simulating a slow TCP link
#define SCHEDULE_A_CHUNK1 \
    "{\"id\":\"1773964800\",\"startTime\":\"1774051200\""
#define SCHEDULE_A_CHUNK2 \
    ",\"duration\":\"3600\",\"interval\":\"2\",\"intervalUnit\":\"1\"}"

// ─── System under test (ReceiveData logic copied verbatim from main.cpp) ────

char receivedData[BUFFER_SIZE];

void ReceiveData_logic(const uint8_t* pData, size_t len, size_t index, size_t total)
{
    if (total >= BUFFER_SIZE) {
        return;
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

// ─── Unity setUp / tearDown ──────────────────────────────────────────────────

void setUp(void)    { memset(receivedData, 0, BUFFER_SIZE); }
void tearDown(void) {}

// ─── Tests ───────────────────────────────────────────────────────────────────

// 1. Full JSON arrives in a single chunk
void test_receive_single_chunk()
{
    const char* payload = SCHEDULE_A;
    size_t len = strlen(payload);
    ReceiveData_logic((const uint8_t*)payload, len, 0, len);
    TEST_ASSERT_EQUAL_STRING(SCHEDULE_A, receivedData);
}

// 2. JSON split across two chunks (simulates slow TCP link)
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

// 3. Payload >= BUFFER_SIZE is silently rejected, buffer stays zeroed
void test_receive_oversized_rejected()
{
    char bigPayload[BUFFER_SIZE + 10];
    memset(bigPayload, 'B', sizeof(bigPayload) - 1);
    bigPayload[sizeof(bigPayload) - 1] = '\0';

    size_t len = strlen(bigPayload);
    ReceiveData_logic((const uint8_t*)bigPayload, len, 0, len);

    char zeroBuf[BUFFER_SIZE];
    memset(zeroBuf, 0, BUFFER_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(zeroBuf, receivedData, BUFFER_SIZE);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_receive_single_chunk);
    RUN_TEST(test_receive_multi_chunk);
    RUN_TEST(test_receive_oversized_rejected);
    return UNITY_END();
}
