#include <unity.h>
#include "tuner_protocol.h"
#include "voltage_sensor.h"
#include "../../src/tuner_protocol.cpp"

uint32_t testMillis = 0;
unsigned testMutexTakes = 0;
HardwareSerial* HardwareSerial::latest = nullptr;
std::function<void(HardwareSerial&, uint8_t)> HardwareSerial::written;
VoltageSensor voltageSensor;
double VoltageSensor::readVoltage() { return 13.8; }
void setUp() { testMillis = 0; HardwareSerial::written = nullptr; }
void tearDown() {}

void test_tune_response_and_uart_failure_are_not_success() {
    TunerProtocol tuner;
    TEST_ASSERT_TRUE(tuner.begin(35, 22, 38400));
    for (const char response : {'F', 'E', 'P', 'T', 'M'}) {
        HardwareSerial::written = [response](HardwareSerial& serial, uint8_t command) {
            if (command == 'F' || command == 'T') serial.bytes.push_back(response);
        };
        TEST_ASSERT_EQUAL(response == 'T' || response == 'M', tuner.fullTune());
        TEST_ASSERT_EQUAL(response == 'T' || response == 'M', tuner.memoryTune());
    }
    HardwareSerial::latest->failWrites = true;
    TEST_ASSERT_FALSE(tuner.memoryTune());
    TEST_ASSERT_FALSE(tuner.fullTune());
    TEST_ASSERT_FALSE(tuner.toggleAntenna());
}

void test_meter_timeout_and_recovery_survive_clock_wrap() {
    testMillis = 0xfffff000u;
    TunerProtocol tuner;
    TEST_ASSERT_TRUE(tuner.begin(35, 22, 38400));
    TEST_ASSERT_FALSE(tuner.isConnected());
    auto feed = [&]() {
        HardwareSerial::latest->bytes = {0x01, 0x77, 0x00, 0x4b, 0x04, 0x88, 0x3b, 0x3b};
        tuner.process();
    };
    feed();
    TEST_ASSERT_TRUE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
    const unsigned mutexTakes = testMutexTakes;
    tuner.getState();
    tuner.getMode();
    tuner.isConnected();
    TEST_ASSERT_EQUAL_UINT(mutexTakes, testMutexTakes);
    testMillis += 3000;
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
    testMillis += 1;
    TEST_ASSERT_FALSE(tuner.getState().meterValid);
    TEST_ASSERT_FALSE(tuner.isConnected());
    feed();
    TEST_ASSERT_TRUE(tuner.isConnected());
    TEST_ASSERT_TRUE(tuner.getState().meterValid);
}

void test_absolute_selection_resolves_unknown_and_repeated_requests_do_not_toggle() {
    TunerProtocol tuner;
    TEST_ASSERT_TRUE(tuner.begin(35, 22, 38400));
    uint8_t antenna = 1;
    unsigned toggles = 0;
    HardwareSerial::written = [&](HardwareSerial& serial, uint8_t command) {
        if (command == 'A') {
            antenna = antenna == 1 ? 2 : 1;
            ++toggles;
            serial.bytes.push_back(antenna == 1 ? 'A' : 'B');
        }
    };
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_UINT(1, antenna);
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_A));
    TEST_ASSERT_EQUAL_UINT(2, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(2, antenna);
    TEST_ASSERT_EQUAL_UINT(3, toggles);
    TEST_ASSERT_TRUE(tuner.selectAntenna(ANT_B));
    TEST_ASSERT_EQUAL_UINT(3, toggles);
    TEST_ASSERT_FALSE(tuner.selectAntenna(ANT_UNKNOWN));
}

void test_command_specific_acknowledgements() {
    TunerProtocol tuner;
    TEST_ASSERT_TRUE(tuner.begin(35, 22, 38400));
    HardwareSerial::written = [](HardwareSerial& serial, uint8_t command) {
        if (command == 'C') serial.bytes.push_back('P');
        if (command == 'M') serial.bytes.push_back('M');
        if (command == 'P') serial.bytes.push_back('P');
        if (command == 'Z') serial.bytes.push_back('Z');
    };
    TEST_ASSERT_FALSE(tuner.setAutoMode());
    TEST_ASSERT_TRUE(tuner.setManualMode());
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL, tuner.getState().mode);
    TEST_ASSERT_TRUE(tuner.bypass());
    TEST_ASSERT_EQUAL_INT(MODE_BYPASS, tuner.getState().mode);
    TEST_ASSERT_TRUE(tuner.sync());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_tune_response_and_uart_failure_are_not_success);
    RUN_TEST(test_meter_timeout_and_recovery_survive_clock_wrap);
    RUN_TEST(test_absolute_selection_resolves_unknown_and_repeated_requests_do_not_toggle);
    RUN_TEST(test_command_specific_acknowledgements);
    return UNITY_END();
}
