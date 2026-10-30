#include <unity.h>
#include <vector>
#include "config_store.h"
#include "remote_protocol.h"

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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_failed_config_write_preserves_previous_snapshot);
    RUN_TEST(test_invalid_config_never_replaces_saved_credentials);
    RUN_TEST(test_remote_results_and_source_contract);
    return UNITY_END();
}
