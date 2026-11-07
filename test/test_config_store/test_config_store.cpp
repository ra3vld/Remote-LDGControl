#include <unity.h>
#include <vector>
#include "config_store.h"
#include "remote_protocol.h"
#include "command_feedback.h"
#include "display_orientation.h"
#include <cstddef>

void setUp() {}
void tearDown() {}
struct Storage {
    std::vector<uint8_t> bytes;
    bool fail = false;
    size_t putBytes(const char*, const void* data, size_t size) {
        if (fail) return 0;
        bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return size;
    }
    size_t getBytesLength(const char*) { return bytes.size(); }
    size_t getBytes(const char*, void* data, size_t size) {
        if (bytes.size() != size) return 0;
        memcpy(data, bytes.data(), size);
        return size;
    }
};
static DeviceConfig config() {
    DeviceConfig value{};
    strcpy(value.webUsername, "admin");
    strcpy(value.webPassword, "test-password");
    value.mqttPort = 1883;
    value.meterPsuVoltage = 13.8;
    return value;
}

void test_failed_config_write_preserves_previous_snapshot() {
    Storage store;
    auto original = config();
    TEST_ASSERT_TRUE(ldg::writeConfig(store, original));
    store.fail = true;
    auto candidate = original;
    strcpy(candidate.webPassword, "changed");
    TEST_ASSERT_FALSE(ldg::writeConfig(store, candidate));
    DeviceConfig loaded{};
    TEST_ASSERT_TRUE(ldg::readConfig(store, loaded));
    TEST_ASSERT_EQUAL_STRING(original.webPassword, loaded.webPassword);
    store.bytes.pop_back();
    TEST_ASSERT_FALSE(ldg::readConfig(store, loaded));
}

void test_display_orientation_uses_padding_and_migrates_v1_without_reading_padding() {
    TEST_ASSERT_EQUAL_UINT(560, sizeof(DeviceConfigV2));
    TEST_ASSERT_EQUAL_UINT(558, offsetof(DeviceConfigV2, displayOrientation));
    TEST_ASSERT_EQUAL_UINT(564, sizeof(ldg::StoredConfigV2));
    Storage store;
    auto original = config();
    original.displayOrientation = 15;
    TEST_ASSERT_TRUE(ldg::writeConfig(store, original));
    DeviceConfig loaded{};
    TEST_ASSERT_TRUE(ldg::readConfig(store, loaded));
    TEST_ASSERT_EQUAL_UINT8(15, loaded.displayOrientation);
    TEST_ASSERT_FALSE(ldg::networkSettingsChanged(config(), original));
    TEST_ASSERT_FALSE(ldg::mqttSettingsChanged(config(), original));
    original.displayOrientation = 16;
    TEST_ASSERT_FALSE(ldg::writeConfig(store, original));
    TEST_ASSERT_TRUE(ldg::readConfig(store, loaded));
    TEST_ASSERT_EQUAL_UINT8(15, loaded.displayOrientation);
    // Legacy record: bytes at the new field offset were arbitrary padding.
    ldg::StoredConfigV2 legacy{};
    legacy.version = 1;
    strcpy(legacy.value.webUsername, "admin");
    strcpy(legacy.value.webPassword, "test-password");
    legacy.value.mqttPort = 1883;
    legacy.value.meterPsuVoltage = 13.8;
    legacy.value.displayOrientation = 0xff;
    store.putBytes("configV1", &legacy, sizeof(legacy));
    TEST_ASSERT_TRUE(ldg::readConfig(store, loaded));
    TEST_ASSERT_EQUAL_UINT8(0, loaded.displayOrientation);
    TEST_ASSERT_EQUAL_STRING(config().webPassword, loaded.webPassword);
    store.bytes[0] = 99;
    TEST_ASSERT_FALSE(ldg::readConfig(store, loaded));
}

void test_v2_migration_preserves_network_auth_and_settings_then_saves_long_mqtt_credentials() {
    Storage store;
    ldg::StoredConfigV2 old{};
    old.version = 2;
    auto& v = old.value;
    strcpy(v.wifiSSID, "test-network"); strcpy(v.wifiPassword, "test-network-secret");
    strcpy(v.webUsername, "operator"); strcpy(v.webPassword, "test-web-secret");
    strcpy(v.mqttBroker, "mqtt.example.org"); strcpy(v.mqttUsername, "old-user");
    strcpy(v.mqttPassword, "old-secret"); v.mqttPort = 8883; v.mqttDiscoveryEnabled = true;
    v.meterPsuVoltage = 12.5; v.configured = true; v.remoteUnitId = 7;
    strcpy(v.ant1Name, "DIPOLE"); strcpy(v.ant2Name, "YAGI");
    v.useStaticIP = true; strcpy(v.staticIP, "192.0.2.10"); strcpy(v.staticGateway, "192.0.2.1");
    strcpy(v.staticNetmask, "255.255.255.0"); strcpy(v.staticDNS, "192.0.2.2");
    strcpy(v.remoteHost, "remote.example.org"); v.displayOrientation = 15;
    store.putBytes("configV1", &old, sizeof(old));
    DeviceConfig loaded{};
    TEST_ASSERT_TRUE(ldg::readConfig(store, loaded));
    TEST_ASSERT_EQUAL_STRING(v.wifiSSID, loaded.wifiSSID);
    TEST_ASSERT_EQUAL_STRING(v.wifiPassword, loaded.wifiPassword);
    TEST_ASSERT_EQUAL_STRING(v.webUsername, loaded.webUsername);
    TEST_ASSERT_EQUAL_STRING(v.webPassword, loaded.webPassword);
    TEST_ASSERT_EQUAL_STRING(v.mqttBroker, loaded.mqttBroker);
    TEST_ASSERT_EQUAL_STRING(v.mqttUsername, loaded.mqttUsername);
    TEST_ASSERT_EQUAL_STRING(v.mqttPassword, loaded.mqttPassword);
    TEST_ASSERT_EQUAL_UINT(v.mqttPort, loaded.mqttPort);
    TEST_ASSERT_TRUE(loaded.mqttDiscoveryEnabled && loaded.configured && loaded.useStaticIP);
    TEST_ASSERT_EQUAL_DOUBLE(v.meterPsuVoltage, loaded.meterPsuVoltage);
    TEST_ASSERT_EQUAL_UINT(v.remoteUnitId, loaded.remoteUnitId);
    TEST_ASSERT_EQUAL_UINT(v.displayOrientation, loaded.displayOrientation);
    TEST_ASSERT_EQUAL_STRING(v.ant1Name, loaded.ant1Name); TEST_ASSERT_EQUAL_STRING(v.ant2Name, loaded.ant2Name);
    TEST_ASSERT_EQUAL_STRING(v.staticIP, loaded.staticIP); TEST_ASSERT_EQUAL_STRING(v.staticGateway, loaded.staticGateway);
    TEST_ASSERT_EQUAL_STRING(v.staticNetmask, loaded.staticNetmask); TEST_ASSERT_EQUAL_STRING(v.staticDNS, loaded.staticDNS);
    TEST_ASSERT_EQUAL_STRING(v.remoteHost, loaded.remoteHost);
    memset(loaded.mqttUsername, 'u', 64); loaded.mqttUsername[64] = 0;
    memset(loaded.mqttPassword, 'p', 128); loaded.mqttPassword[128] = 0;
    TEST_ASSERT_TRUE(ldg::writeConfig(store, loaded));
    DeviceConfig roundtrip{};
    TEST_ASSERT_TRUE(ldg::readConfig(store, roundtrip));
    TEST_ASSERT_EQUAL_STRING(loaded.mqttUsername, roundtrip.mqttUsername);
    TEST_ASSERT_EQUAL_STRING(loaded.mqttPassword, roundtrip.mqttPassword);
    TEST_ASSERT_EQUAL_STRING(v.wifiPassword, roundtrip.wifiPassword);
    TEST_ASSERT_EQUAL_UINT(3, store.bytes[0]);
    // Old/corrupt records must never partially replace an active configuration.
    old.value.mqttPassword[0] = 'x'; memset(old.value.mqttUsername, 'x', sizeof(old.value.mqttUsername));
    store.putBytes("configV1", &old, sizeof(old));
    TEST_ASSERT_FALSE(ldg::readConfig(store, roundtrip));
    TEST_ASSERT_EQUAL_STRING(loaded.mqttUsername, roundtrip.mqttUsername);
}

void test_all_display_touch_transforms_invert_the_hardware_address_mapping() {
    for (unsigned mode = 0; mode < 16; ++mode) {
        const auto driver = ldg::displayDriverRotation(mode);
        const auto madctl = ldg::displayMadctl(driver);
        for (int16_t x : {int16_t(0), int16_t(173), int16_t(479)}) {
            for (int16_t y : {int16_t(0), int16_t(111), int16_t(319)}) {
                int16_t uiX = x, uiY = y;
                ldg::orientDisplayTouch(uiX, uiY, driver);
                TEST_ASSERT_GREATER_OR_EQUAL_INT(0, uiX);
                TEST_ASSERT_GREATER_OR_EQUAL_INT(0, uiY);
                TEST_ASSERT_LESS_THAN_INT((driver&1) ? 480 : 320, uiX);
                TEST_ASSERT_LESS_THAN_INT((driver&1) ? 320 : 480, uiY);
                // Independent model of MADCTL: exchange native column/row,
                // then reverse the selected native axes.
                int column = (madctl & 0x20) ? uiY : uiX;
                int row = (madctl & 0x20) ? uiX : uiY;
                if (madctl & 0x40) column = 319 - column;
                if (madctl & 0x80) row = 479 - row;
                TEST_ASSERT_EQUAL_INT(x, row);
                TEST_ASSERT_EQUAL_INT(y, column);
            }
        }
    }
}

void test_invalid_config_never_replaces_saved_credentials() {
    Storage store;
    auto value = config();
    TEST_ASSERT_TRUE(ldg::writeConfig(store, value));
    value.meterPsuVoltage = NAN;
    TEST_ASSERT_FALSE(ldg::writeConfig(store, value));
    value = config();
    value.useStaticIP = true;
    strcpy(value.staticIP, "192.168.1.999");
    strcpy(value.staticNetmask, "255.255.255.0");
    strcpy(value.staticGateway, "192.168.1.1");
    TEST_ASSERT_FALSE(ldg::writeConfig(store, value));
    strcpy(value.staticIP, "192.168.1.50");
    TEST_ASSERT_TRUE(ldg::writeConfig(store, value));
    value = config();
    value.webUsername[0] = 0;
    TEST_ASSERT_FALSE(ldg::writeConfig(store, value));
    value = config();
    memset(value.remoteHost, 'x', sizeof(value.remoteHost));
    TEST_ASSERT_FALSE(ldg::writeConfig(store, value));
}

void test_remote_results_and_source_contract() {
    JsonDocument response;
    response["result"] = "failed";
    TEST_ASSERT_FALSE(ldg::remoteCommandSucceeded(200, response.as<JsonVariantConst>()));
    response["result"] = "success";
    TEST_ASSERT_FALSE(ldg::remoteCommandSucceeded(401, response.as<JsonVariantConst>()));
    TEST_ASSERT_TRUE(ldg::remoteCommandSucceeded(200, response.as<JsonVariantConst>()));
    TEST_ASSERT_FALSE(ldg::validRemoteStatus(response.as<JsonVariantConst>()));
    response["fwd_power"] = nullptr;
    response["ref_power"] = nullptr;
    response["source"] = "local";
    TEST_ASSERT_TRUE(ldg::validRemoteStatus(response.as<JsonVariantConst>()));
    response["source"] = "remote";
    TEST_ASSERT_FALSE(ldg::validRemoteStatus(response.as<JsonVariantConst>()));
    TEST_ASSERT_EQUAL_STRING("/api/command/antenna_1", ldg::commandEndpoint(TUNER_CMD_SELECT_ANT_A));
    TEST_ASSERT_NULL(ldg::commandEndpoint(0));
}

void test_remote_deadlines_allow_tuning_recovery_and_antenna_selection() {
    TEST_ASSERT_TRUE(ldg::remoteRequestTimeout('F') >= 23000);
    TEST_ASSERT_TRUE(ldg::remoteRequestTimeout('T') >= 23000);
    TEST_ASSERT_TRUE(ldg::remoteRequestTimeout(TUNER_CMD_SELECT_ANT_A) >= 10600);
    TEST_ASSERT_TRUE(ldg::remoteRequestTimeout(TUNER_CMD_SELECT_ANT_B) >= 10600);
    TEST_ASSERT_TRUE(ldg::remoteRequestTimeout(TUNER_CMD_TOGGLE_ANT) >= 7400);
    TEST_ASSERT_EQUAL_UINT(3000, ldg::remoteRequestTimeout(0));
}

void test_runtime_settings_and_dhcp_defaults_do_not_require_a_network_restart() {
    const auto original = config();
    auto candidate = original;
    strcpy(candidate.mqttBroker, "other-broker");
    candidate.meterPsuVoltage = 12.5;
    strcpy(candidate.ant1Name, "DIPOLE");
    strcpy(candidate.staticIP, "192.0.2.10"); // Browser fills DHCP info in static fields.
    TEST_ASSERT_FALSE(ldg::networkSettingsChanged(original, candidate));
    TEST_ASSERT_TRUE(ldg::mqttSettingsChanged(original, candidate));
    candidate.useStaticIP = true;
    TEST_ASSERT_TRUE(ldg::networkSettingsChanged(original, candidate));
    candidate = original;
    strcpy(candidate.wifiSSID, "different-wifi");
    TEST_ASSERT_TRUE(ldg::networkSettingsChanged(original, candidate));
    TEST_ASSERT_FALSE(ldg::mqttSettingsChanged(original, candidate));
    auto fixed = original;
    fixed.useStaticIP = true;
    strcpy(fixed.staticIP, "192.0.2.1");
    candidate = fixed;
    strcpy(candidate.staticIP, "192.0.2.2");
    TEST_ASSERT_TRUE(ldg::networkSettingsChanged(fixed, candidate));
}

void test_command_feedback_waits_for_real_completion_and_ignores_older_requests() {
    ldg::CommandFeedback feedback;
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Idle);
    const uint32_t first = feedback.start('T');
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Pending);
    const uint32_t second = feedback.start('F');
    feedback.finish(first, true);
    TEST_ASSERT_EQUAL_UINT(second, feedback.snapshot().token);
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Pending);
    feedback.finish(second, false);
    TEST_ASSERT_EQUAL_UINT8('F', feedback.snapshot().command);
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Failed);
    const uint32_t third = feedback.start('C');
    feedback.finish(third, true);
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Success);
    feedback.finish(third, false);
    TEST_ASSERT_TRUE(feedback.snapshot().phase == ldg::CommandFeedback::Phase::Success);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_failed_config_write_preserves_previous_snapshot);
    RUN_TEST(test_display_orientation_uses_padding_and_migrates_v1_without_reading_padding);
    RUN_TEST(test_v2_migration_preserves_network_auth_and_settings_then_saves_long_mqtt_credentials);
    RUN_TEST(test_all_display_touch_transforms_invert_the_hardware_address_mapping);
    RUN_TEST(test_invalid_config_never_replaces_saved_credentials);
    RUN_TEST(test_remote_results_and_source_contract);
    RUN_TEST(test_remote_deadlines_allow_tuning_recovery_and_antenna_selection);
    RUN_TEST(test_runtime_settings_and_dhcp_defaults_do_not_require_a_network_restart);
    RUN_TEST(test_command_feedback_waits_for_real_completion_and_ignores_older_requests);
    return UNITY_END();
}
