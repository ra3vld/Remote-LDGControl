#include <unity.h>
#include "../support/mqtt/config_manager.h"
#include "voltage_sensor.h"
#include "../../src/voltage_sensor.cpp"

uint32_t testMillis = 0;
int testAnalogValue = 2000;
int testAnalogReads = 0;
FakeConfigManager configManager;
void setUp() { testAnalogReads = 0; configManager.config = DeviceConfig{}; }
void tearDown() {}

void test_external_adc_is_initialized_and_sampled() {
    VoltageSensor sensor;
    sensor.begin(34);
    TEST_ASSERT_TRUE(sensor.isValid());
    TEST_ASSERT_DOUBLE_WITHIN(0.001, (2000.0 / 4095) * 3.3 * VOLTAGE_DIVIDER_RATIO, sensor.readVoltage());
    TEST_ASSERT_EQUAL_INT(VOLTAGE_SAMPLES, testAnalogReads);
}

void test_no_adc_uses_configured_voltage_and_checks_invalid_values() {
    VoltageSensor sensor;
    sensor.begin(-1);
    TEST_ASSERT_FALSE(sensor.isValid());
    configManager.config.meterPsuVoltage = 12.5;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 12.5, sensor.readVoltage());
    TEST_ASSERT_EQUAL_INT(0, testAnalogReads);
    configManager.config.meterPsuVoltage = NAN;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, METER_PSU_VOLTAGE, sensor.readVoltage());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_external_adc_is_initialized_and_sampled);
    RUN_TEST(test_no_adc_uses_configured_voltage_and_checks_invalid_values);
    return UNITY_END();
}
