#include <unity.h>
#include "config_manager.h"
#include "tci_client.h"
#include <WiFi.h>
#include "../../src/tci_client.cpp"

uint32_t testMillis = 0;
FakeSerial Serial;
FakeWiFi WiFi;
FakeConfigManager configManager;
WebSocketsClient* WebSocketsClient::latest = nullptr;
void setUp() {
    testMillis=0; configManager.radio=ldg::RadioSettings{}; WiFi.wifiStatus=WL_CONNECTED;
    configManager.radio.enabled=true; strcpy(configManager.radio.host,"192.0.2.20");
}
void tearDown() {}
static void ready(TciClient& client) {
    client.begin(); client.loop();
    auto& socket=*WebSocketsClient::latest;
    socket.emit(WStype_CONNECTED,"");
    socket.emit(WStype_TEXT,"device:OtherRadio;protocol:OtherServer,1.9;trx_count:1;ready;"
        "vfo:0,0,14074000;split_enable:0,false;xit_enable:0,false;trx:0,false;tune:0,false;");
    client.loop();
}
void test_real_client_lifecycle_queries_only_reads_and_accepts_fragmented_cat_but_not_binary() {
    TciClient client; client.begin(); client.loop(); auto& socket=*WebSocketsClient::latest;
    TEST_ASSERT_EQUAL_STRING("192.0.2.20",socket.host.c_str());
    TEST_ASSERT_EQUAL_UINT(40001,socket.port); TEST_ASSERT_EQUAL_STRING("",socket.protocol.c_str());
    TEST_ASSERT_EQUAL_UINT(1000,socket.ping); TEST_ASSERT_EQUAL_UINT(2000,socket.pong);
    socket.emit(WStype_CONNECTED,""); socket.emit(WStype_TEXT,"trx_count:1;ready;");
    testMillis=1000; socket.emit(WStype_PONG,""); client.loop();
    TEST_ASSERT_EQUAL_UINT(2,socket.sent.size());
    TEST_ASSERT_EQUAL_STRING("trx:0;tune:0;",socket.sent.back().c_str());
    TEST_ASSERT_NULL(strstr(socket.sent.front().c_str(),"true"));
    socket.emit(WStype_FRAGMENT_TEXT_START,"vfo:0,0,140");
    socket.emit(WStype_FRAGMENT_FIN,"74000;split_enable:0,false;xit_enable:0,false;trx:0,false;tune:0,false;");
    client.loop(); TEST_ASSERT_TRUE(client.state().safeRx());
    socket.emit(WStype_BIN,"trx:0,true;"); client.loop(); TEST_ASSERT_TRUE(client.state().safeRx());
    socket.emit(WStype_FRAGMENT_BIN_START,"trx:0,"); socket.emit(WStype_FRAGMENT_FIN,"true;");
    client.loop(); TEST_ASSERT_TRUE(client.state().safeRx());
    socket.emit(WStype_TEXT,"trx:0,true;"); client.loop(); TEST_ASSERT_FALSE(client.state().safeRx());
}
void test_settings_change_and_stalled_network_revoke_authority_without_waiting_for_worker() {
    TciClient client; ready(client); TEST_ASSERT_TRUE(client.state().safeRx());
    strcpy(configManager.radio.host,"new-host");
    TEST_ASSERT_FALSE(client.state().ownsBand());
    WebSocketsClient::latest->emit(WStype_PONG,""); client.loop();
    TEST_ASSERT_FALSE(client.state().safeRx());
    TEST_ASSERT_FALSE(WebSocketsClient::latest->connected);
    client.loop(); TEST_ASSERT_EQUAL_STRING("new-host",WebSocketsClient::latest->host.c_str());
    WebSocketsClient::latest->emit(WStype_CONNECTED,"");
    client.loop(); TEST_ASSERT_FALSE(client.state().frequencyValid);
    ready(client); testMillis=3001;
    TEST_ASSERT_FALSE(client.state().safeRx());
    TEST_ASSERT_FALSE(client.state().ownsBand());
    WebSocketsClient::latest->emit(WStype_PONG,""); client.loop();
    TEST_ASSERT_FALSE(client.state().safeRx());
    TEST_ASSERT_FALSE(WebSocketsClient::latest->connected);
}
void test_heartbeat_keeps_unchanged_frequency_live_and_bad_state_forces_fresh_sync() {
    TciClient client; ready(client); auto& socket=*WebSocketsClient::latest;
    for (unsigned i=1;i<20;++i) { testMillis=i*1000; socket.emit(WStype_PONG,""); client.loop(); TEST_ASSERT_TRUE(client.state().safeRx()); }
    TEST_ASSERT_EQUAL_UINT(120000,client.state().freeHeap);
    TEST_ASSERT_EQUAL_UINT(80000,client.state().largestBlock);
    TEST_ASSERT_EQUAL_UINT(2048,client.state().stackFree);
    socket.emit(WStype_TEXT,"trx:0,invalid;"); client.loop();
    TEST_ASSERT_FALSE(socket.connected); TEST_ASSERT_FALSE(client.state().frequencyValid);
    socket.emit(WStype_CONNECTED,""); socket.emit(WStype_TEXT,"ready;"); client.loop();
    TEST_ASSERT_FALSE(client.state().safeRx());
}
void test_remote_owner_and_disabled_configuration_stop_the_local_client() {
    TciClient client; ready(client); auto& socket=*WebSocketsClient::latest;
    client.loop(false); TEST_ASSERT_FALSE(socket.connected); TEST_ASSERT_FALSE(client.state().ownsBand());
    const unsigned begins=socket.begins; client.loop(false); TEST_ASSERT_EQUAL_UINT(begins,socket.begins);
    configManager.radio.enabled=false; client.loop();
    TEST_ASSERT_EQUAL_INT(int(ldg::RadioConnection::Disabled),int(client.state().connection));
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_real_client_lifecycle_queries_only_reads_and_accepts_fragmented_cat_but_not_binary);
    RUN_TEST(test_settings_change_and_stalled_network_revoke_authority_without_waiting_for_worker);
    RUN_TEST(test_heartbeat_keeps_unchanged_frequency_live_and_bad_state_forces_fresh_sync);
    RUN_TEST(test_remote_owner_and_disabled_configuration_stop_the_local_client);
    return UNITY_END();
}
