#include <unity.h>
#include "usb_provisioning.h"
#include "connection_config.h"
#include <string>
#include <vector>
static unsigned writes;
static DeviceConfig config;
void setUp() { writes=0;config={};strcpy(config.webUsername,"admin");strcpy(config.webPassword,"secret");config.mqttPort=1883;config.meterPsuVoltage=13.8; }
void tearDown() {}
static void handler(const char* op,JsonVariantConst request,JsonDocument& reply) {
    if (!strcmp(op,"apply")) {
        auto next=config;
        if (!ldg::patchConnectionConfig(request["settings"],next,true)) {reply["status"]=400;return;}
        config=next;++writes;reply["saved"]=true;
    } else if (!strcmp(op,"settings")) {reply["ssid"]=config.wifiSSID;reply["hasPassword"]=bool(config.wifiPassword[0]);}
}
static JsonDocument exchange(ldg::UsbProvisioning& api,const char* text) {
    char output[ldg::UsbProvisioning::Limit+1];TEST_ASSERT_TRUE(api.handle(text,output,sizeof(output)));
    JsonDocument result;TEST_ASSERT_FALSE(deserializeJson(result,output));return result;
}
static void hello(ldg::UsbProvisioning& api) {api.begin("0011223344556677",handler);exchange(api,R"({"ldg":1,"cid":1,"op":"hello","client":"0123456789abcdef0123456789abcdef"})");}
static const char* save=R"({"ldg":1,"cid":2,"boot":"0011223344556677","sid":1,"op":"apply","settings":{"wifiSSID":"Станция","wifiPassword":"test-password","mqttBroker":"rotator.r3v.ru","mqttPort":8883,"mqttUsername":"ldg-test","mqttPassword":"test-token","mqttDiscoveryEnabled":false}})";
void test_save_retry_is_exact_and_wrong_boot_session_or_body_cannot_write() {
    ldg::UsbProvisioning api;hello(api);
    TEST_ASSERT_EQUAL(200,exchange(api,save)["status"]);
    TEST_ASSERT_EQUAL(200,exchange(api,save)["status"]);TEST_ASSERT_EQUAL(1,writes);
    std::string changed=save;changed.replace(changed.find("Станция"),strlen("Станция"),"Other");
    TEST_ASSERT_EQUAL(409,exchange(api,changed.c_str())["status"]);
    changed=save;changed.replace(changed.find("0011223344556677"),16,"1111223344556677");
    TEST_ASSERT_EQUAL(409,exchange(api,changed.c_str())["status"]);
    changed=save;changed.replace(changed.find("\"sid\":1"),7,"\"sid\":2");
    TEST_ASSERT_EQUAL(409,exchange(api,changed.c_str())["status"]);TEST_ASSERT_EQUAL(1,writes);
    TEST_ASSERT_EQUAL_STRING("Станция",config.wifiSSID);
    auto read=exchange(api,R"({"ldg":1,"cid":3,"boot":"0011223344556677","sid":1,"op":"settings"})");
    std::string json;serializeJson(read,json);TEST_ASSERT_NULL(strstr(json.c_str(),"test-password"));
    TEST_ASSERT_TRUE(read["hasPassword"].as<bool>());
    TEST_ASSERT_EQUAL(409,exchange(api,save)["status"]);TEST_ASSERT_EQUAL(1,writes);
}
void test_new_owner_and_reboot_invalidate_old_requests() {
    ldg::UsbProvisioning api;hello(api);exchange(api,save);
    auto owner=exchange(api,R"({"ldg":1,"cid":1,"op":"hello","client":"11111111111111111111111111111111"})");
    TEST_ASSERT_EQUAL(2,owner["sid"]);TEST_ASSERT_EQUAL(409,exchange(api,save)["status"]);
    api.begin("1111223344556677",handler);
    TEST_ASSERT_EQUAL(409,exchange(api,save)["status"]);TEST_ASSERT_EQUAL(1,writes);
}
void test_invalid_json_types_lengths_and_partial_patches_preserve_config() {
    ldg::UsbProvisioning api;hello(api);char output[1600];
    TEST_ASSERT_FALSE(api.handle("boot log",output,sizeof(output)));
    TEST_ASSERT_FALSE(api.handle("{",output,sizeof(output)));
    TEST_ASSERT_FALSE(api.handle(R"({"ldg":true,"cid":1,"op":"hello"})",output,sizeof(output)));
    TEST_ASSERT_FALSE(ldg::validConfigText("\xc0\xaf"));
    TEST_ASSERT_FALSE(ldg::validConfigText("\xed\xa0\x80"));
    TEST_ASSERT_TRUE(ldg::validConfigText("Станция"));
    std::string longLine(1600,'x');TEST_ASSERT_FALSE(api.handle(longLine.c_str(),output,sizeof(output)));
    for (const char* patch : {R"({"mqttPort":0})",R"({"mqttBroker":false})",R"({"mqttDiscoveryEnabled":"false"})",R"({"wifiSSID":"bad\u0000hidden"})",R"({"wifiSSID":"network","wifiPassword":"short"})",R"({"useStaticIP":true,"staticIP":"256.1.1.1"})"}) {
        JsonDocument doc;deserializeJson(doc,patch);auto next=config;
        TEST_ASSERT_FALSE(ldg::patchConnectionConfig(doc.as<JsonVariantConst>(),next,true));
    }
    JsonDocument doc;deserializeJson(doc,R"({"mqttPassword":"","mqttPort":8883})");strcpy(config.mqttPassword,"keep");
    TEST_ASSERT_TRUE(ldg::patchConnectionConfig(doc.as<JsonVariantConst>(),config,true));TEST_ASSERT_EQUAL_STRING("keep",config.mqttPassword);
    doc["clearMqttPassword"]=true;TEST_ASSERT_TRUE(ldg::patchConnectionConfig(doc.as<JsonVariantConst>(),config,true));TEST_ASSERT_EQUAL_STRING("",config.mqttPassword);
}
int main(){UNITY_BEGIN();RUN_TEST(test_save_retry_is_exact_and_wrong_boot_session_or_body_cannot_write);RUN_TEST(test_new_owner_and_reboot_invalidate_old_requests);RUN_TEST(test_invalid_json_types_lengths_and_partial_patches_preserve_config);return UNITY_END();}
