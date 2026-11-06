#include <unity.h>
#include "../support/mqtt/config_manager.h"
#include "security.h"
#include "web_asset_request.h"
#include "../../src/security.cpp"

uint32_t testMillis = 0;
FakeSerial Serial;
FakeConfigManager configManager;
void setUp() {
    testMillis = 0;
    configManager.config = DeviceConfig{};
    Security::begin();
    Security::getRateLimiter().reset();
    Security::getLoginTracker().reset();
}
void tearDown() {}

void test_rate_limit_at_boot_and_after_clock_wrap() {
    RateLimiter limiter(10, 1000);
    IPAddress ip(1);
    for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(limiter.allowRequest(ip));
    TEST_ASSERT_FALSE(limiter.allowRequest(ip));
    TEST_ASSERT_TRUE(limiter.allowRequest(IPAddress(2)));
    testMillis = 1000;
    TEST_ASSERT_TRUE(limiter.allowRequest(ip));
    limiter.reset();
    testMillis = 0xffffff00u;
    for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(limiter.allowRequest(ip));
    TEST_ASSERT_FALSE(limiter.allowRequest(ip));
    testMillis += 1000;
    TEST_ASSERT_TRUE(limiter.allowRequest(ip));
}

void test_authorization_enforces_lockout_then_recovers() {
    IPAddress ip(1);
    for (int i = 0; i < 5; ++i)
        TEST_ASSERT_TRUE(Security::authorize(ip, "admin", "wrong") == Security::Authorization::Unauthorized);
    TEST_ASSERT_TRUE(Security::authorize(ip, "admin", "test-password") == Security::Authorization::Locked);
    testMillis += 300000;
    TEST_ASSERT_TRUE(Security::authorize(ip, "admin", "test-password") == Security::Authorization::Allowed);
    for (int i = 0; i < 9; ++i)
        TEST_ASSERT_TRUE(Security::authorize(ip, "admin", "test-password") == Security::Authorization::Allowed);
    TEST_ASSERT_TRUE(Security::authorize(ip, "admin", "test-password") == Security::Authorization::RateLimited);
}

void test_login_lockout_survives_clock_wrap() {
    LoginTracker tracker(5, 300000);
    IPAddress ip(3);
    testMillis = 0xffffff00u;
    for (int i = 0; i < 5; ++i) tracker.recordFailure(ip);
    TEST_ASSERT_FALSE(tracker.canAttempt(ip));
    testMillis += 299999;
    TEST_ASSERT_FALSE(tracker.canAttempt(ip));
    testMillis += 1;
    TEST_ASSERT_TRUE(tracker.canAttempt(ip));
}

void test_web_asset_paths_are_restricted() {
    TEST_ASSERT_TRUE(ldg::validWebAsset("widgets.0123456789ab.js"));
    TEST_ASSERT_TRUE(ldg::validWebAsset("settings.abcdefabcdef.js"));
    TEST_ASSERT_TRUE(ldg::validWebAsset("ru.000000000000.js"));
    for (const char* name : {"", "../config", "widgets.0123456789ab.js.gz", "ru.ABCDEF123456.js",
            "settings.0123456789a.js", "config.0123456789ab.js", "ru.0123456789ab.js/../cert.pem"})
        TEST_ASSERT_FALSE(ldg::validWebAsset(name));
}
void test_web_encoding_negotiation() {
    TEST_ASSERT_FALSE(ldg::webEncoding("").gzip);
    TEST_ASSERT_TRUE(ldg::webEncoding("").identity);
    TEST_ASSERT_TRUE(ldg::webEncoding("br, GZip;q=0.5").gzip);
    TEST_ASSERT_FALSE(ldg::webEncoding("gzip;q=0, br").gzip);
    TEST_ASSERT_FALSE(ldg::webEncoding("xgzip, deflate").gzip);
    TEST_ASSERT_TRUE(ldg::webEncoding("*;q=0.5").gzip);
    TEST_ASSERT_FALSE(ldg::webEncoding("*;q=0").identity);
    TEST_ASSERT_FALSE(ldg::webEncoding("gzip, identity;q=0").identity);
    TEST_ASSERT_FALSE(ldg::webEncoding("gzip;q=invalid").gzip);
    TEST_ASSERT_FALSE(ldg::webEncoding("gzip;q=1.1").gzip);
    TEST_ASSERT_TRUE(ldg::webEncoding("*;q=0,gzip;q=1,identity;q=1").identity);
    TEST_ASSERT_TRUE(ldg::webEncoding("*;q=0,gzip;q=1").gzip);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rate_limit_at_boot_and_after_clock_wrap);
    RUN_TEST(test_authorization_enforces_lockout_then_recovers);
    RUN_TEST(test_login_lockout_survives_clock_wrap);
    RUN_TEST(test_web_asset_paths_are_restricted);
    RUN_TEST(test_web_encoding_negotiation);
    return UNITY_END();
}
