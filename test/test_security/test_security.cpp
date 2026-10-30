#include <unity.h>
#include "../support/mqtt/config_manager.h"
#include "security.h"
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rate_limit_at_boot_and_after_clock_wrap);
    RUN_TEST(test_authorization_enforces_lockout_then_recovers);
    RUN_TEST(test_login_lockout_survives_clock_wrap);
    return UNITY_END();
}
