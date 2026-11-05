#include <unity.h>
#include <vector>
#include <cstring>
#include "brightness_control.h"
void setUp() {}
void tearDown() {}
void test_manual_control_and_missing_sensor_use_manual_brightness() {
    ldg::BrightnessSettings s; s.manual=70;
    ldg::BrightnessControl control;
    TEST_ASSERT_EQUAL_UINT8(70,control.update(s,0));
    s.automatic=1;
    TEST_ASSERT_EQUAL_UINT8(70,control.update(s,-1));
    TEST_ASSERT_EQUAL_UINT8(70,control.update(s,4096));
}
void test_auto_brightness_fades_within_limits_and_supports_reversed_calibration() {
    for (const bool reversed : {false,true}) {
        ldg::BrightnessSettings s; s.automatic=1;
        if (reversed) { s.darkAdc=300; s.brightAdc=3500; }
        ldg::BrightnessControl control;
        unsigned previous=100;
        for (int i=0;i<100;++i) {
            unsigned actual=control.update(s,s.darkAdc);
            TEST_ASSERT_LESS_OR_EQUAL_UINT(2,previous-actual);
            TEST_ASSERT_GREATER_OR_EQUAL_UINT(s.minimum,actual);
            previous=actual;
        }
        TEST_ASSERT_EQUAL_UINT(s.minimum,previous);
        for (int i=0;i<100;++i) {
            unsigned actual=control.update(s,s.brightAdc);
            TEST_ASSERT_LESS_OR_EQUAL_UINT(2,actual-previous);
            TEST_ASSERT_LESS_OR_EQUAL_UINT(s.maximum,actual);
            previous=actual;
        }
        TEST_ASSERT_EQUAL_UINT(s.maximum,previous);
    }
}
void test_sensor_noise_does_not_modulate_backlight_and_bad_settings_are_bright() {
    ldg::BrightnessSettings s; s.automatic=1;
    ldg::BrightnessControl control;
    unsigned stable=0;
    for (int i=0;i<100;++i) stable=control.update(s,1900);
    for (int i=0;i<100;++i) TEST_ASSERT_EQUAL_UINT(stable,control.update(s,1900+(i%3)-1));
    s.darkAdc=s.brightAdc;
    TEST_ASSERT_FALSE(ldg::validBrightnessSettings(s));
    TEST_ASSERT_EQUAL_UINT8(100,control.update(s,0));
}
struct Storage {
    std::vector<uint8_t> blob;
    bool fail=false;
    size_t putBytes(const char*,const void* data,size_t size) {
        if(fail)return 0;
        const auto* p=static_cast<const uint8_t*>(data);blob.assign(p,p+size);return size;
    }
    size_t getBytesLength(const char*) { return blob.size(); }
    size_t getBytes(const char*,void* data,size_t size) {
        if(size!=blob.size())return 0;memcpy(data,blob.data(),size);return size;
    }
};
void test_settings_json_and_persistence_reject_invalid_or_partial_configuration() {
    ldg::BrightnessSettings s; s.automatic=1; s.manual=45;
    Storage storage;
    TEST_ASSERT_TRUE(ldg::writeBrightnessSettings(storage,s));
    ldg::BrightnessSettings loaded;
    TEST_ASSERT_TRUE(ldg::readBrightnessSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT8(45,loaded.manual);
    JsonDocument doc;ldg::writeBrightnessJson(doc,loaded);
    TEST_ASSERT_TRUE(ldg::parseBrightnessSettings(doc.as<JsonVariantConst>(),s));
    doc["minimum"]=0;TEST_ASSERT_FALSE(ldg::parseBrightnessSettings(doc.as<JsonVariantConst>(),s));
    doc["minimum"]=90;doc["maximum"]=20;
    TEST_ASSERT_FALSE(ldg::parseBrightnessSettings(doc.as<JsonVariantConst>(),s));
    doc.clear();doc["automatic"]=true;
    TEST_ASSERT_FALSE(ldg::parseBrightnessSettings(doc.as<JsonVariantConst>(),s));
    storage.fail=true;TEST_ASSERT_FALSE(ldg::writeBrightnessSettings(storage,s));
    TEST_ASSERT_TRUE(ldg::readBrightnessSettings(storage,loaded));
    TEST_ASSERT_EQUAL_UINT8(45,loaded.manual);
    storage.blob[0]=2;TEST_ASSERT_FALSE(ldg::readBrightnessSettings(storage,loaded));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_manual_control_and_missing_sensor_use_manual_brightness);
    RUN_TEST(test_auto_brightness_fades_within_limits_and_supports_reversed_calibration);
    RUN_TEST(test_sensor_noise_does_not_modulate_backlight_and_bad_settings_are_bright);
    RUN_TEST(test_settings_json_and_persistence_reject_invalid_or_partial_configuration);
    return UNITY_END();
}
