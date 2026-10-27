#include <unity.h>
#include <map>
#include <vector>
#include "gt911_touch.h"

// A register-backed I2C device, including NACK, short reads and write failure.
class FakeBus {
public:
    uint8_t deviceAddress = 0x5d;
    bool connected = true;
    bool failAck = false;
    uint16_t shortRead = 0;
    unsigned acknowledgements = 0;
    std::map<uint16_t, uint8_t> registers;

    FakeBus() {
        registers[0x8140] = '9';
        registers[0x8141] = '1';
        registers[0x8142] = '1';
    }

    void report(uint8_t count, uint16_t x = 0, uint16_t y = 0) {
        registers[0x814e] = 0x80 | count;
        registers[0x8150] = x & 0xff;
        registers[0x8151] = x >> 8;
        registers[0x8152] = y & 0xff;
        registers[0x8153] = y >> 8;
    }

    void beginTransmission(uint8_t address) {
        m_address = address;
        m_tx.clear();
    }
    void write(uint8_t value) { m_tx.push_back(value); }
    uint8_t endTransmission(bool stop = true) {
        if (!connected || m_address != deviceAddress) return 2;
        m_reg = (uint16_t(m_tx.at(0)) << 8) | m_tx.at(1);
        if (m_tx.size() == 3) {
            TEST_ASSERT_TRUE(stop);
            TEST_ASSERT_EQUAL_HEX16(0x814e, m_reg);
            TEST_ASSERT_EQUAL_UINT8(0, m_tx[2]);
            if (failAck) return 2;
            ++acknowledgements;
            registers[m_reg] = m_tx[2];
        } else {
            TEST_ASSERT_FALSE(stop); // Register reads use a repeated start.
        }
        return 0;
    }
    size_t requestFrom(uint8_t address, size_t count) {
        TEST_ASSERT_EQUAL_UINT8(deviceAddress, address);
        if (shortRead == m_reg) return count - 1;
        return count;
    }
    uint8_t read() { return registers[m_reg++]; }

private:
    uint8_t m_address = 0;
    uint16_t m_reg = 0;
    std::vector<uint8_t> m_tx;
};

void setUp() {}
void tearDown() {}

void test_address_probe_and_product_identity() {
    for (uint8_t address : {0x5d, 0x14}) {
        FakeBus bus;
        bus.deviceAddress = address;
        Gt911Touch<FakeBus> touch(bus);
        TEST_ASSERT_TRUE(touch.begin());
        TEST_ASSERT_EQUAL_UINT8(address, touch.address());
    }
    FakeBus otherDevice;
    otherDevice.registers[0x8140] = '8';
    Gt911Touch<FakeBus> touch(otherDevice);
    TEST_ASSERT_FALSE(touch.begin());
    TEST_ASSERT_EQUAL_UINT8(0, touch.address());
}

void test_landscape_corners_and_centre() {
    FakeBus bus;
    Gt911Touch<FakeBus> touch(bus);
    TEST_ASSERT_TRUE(touch.begin());
    const uint16_t cases[][4] = {
        {0, 0, 479, 0}, {319, 0, 479, 319},
        {0, 479, 0, 0}, {319, 479, 0, 319}, {160, 240, 239, 160}
    };
    for (const auto& c : cases) {
        bus.report(1, c[0], c[1]);
        const auto point = touch.poll(20);
        TEST_ASSERT_TRUE(point.pressed);
        TEST_ASSERT_EQUAL_UINT16(c[2], point.x);
        TEST_ASSERT_EQUAL_UINT16(c[3], point.y);
        TEST_ASSERT_EQUAL_UINT8(0, bus.registers[0x814e]);
    }
    TEST_ASSERT_EQUAL_UINT(5, bus.acknowledgements);
}

void test_release_and_multitouch_do_not_press() {
    FakeBus bus;
    Gt911Touch<FakeBus> touch(bus);
    touch.begin();
    for (uint8_t count : {0, 2, 5, 15}) {
        bus.report(1, 100, 200);
        TEST_ASSERT_TRUE(touch.poll(20).pressed);
        bus.report(count);
        const auto point = touch.poll(40);
        TEST_ASSERT_FALSE(point.pressed);
        TEST_ASSERT_EQUAL_UINT16(279, point.x);
        TEST_ASSERT_EQUAL_UINT16(100, point.y);
        TEST_ASSERT_EQUAL_UINT8(0, bus.registers[0x814e]);
    }
}

void test_out_of_range_coordinates_are_released() {
    FakeBus bus;
    Gt911Touch<FakeBus> touch(bus);
    touch.begin();
    for (const auto& c : {std::vector<uint16_t>{320, 0}, {0, 480}, {65535, 65535}}) {
        bus.report(1, c[0], c[1]);
        TEST_ASSERT_FALSE(touch.poll(20).pressed);
        TEST_ASSERT_EQUAL_UINT8(0, bus.registers[0x814e]);
    }
}

void test_no_report_preserves_press_briefly_then_releases() {
    FakeBus bus;
    Gt911Touch<FakeBus> touch(bus);
    touch.begin();
    bus.report(1, 50, 60);
    TEST_ASSERT_TRUE(touch.poll(100).pressed);
    TEST_ASSERT_TRUE(touch.poll(130).pressed);
    TEST_ASSERT_FALSE(touch.poll(201).pressed);
    TEST_ASSERT_EQUAL_UINT(1, bus.acknowledgements);
}

void test_i2c_failures_release_held_input() {
    for (unsigned failure = 0; failure < 4; ++failure) {
        FakeBus bus;
        Gt911Touch<FakeBus> touch(bus);
        touch.begin();
        bus.report(1, 50, 60);
        TEST_ASSERT_TRUE(touch.poll(100).pressed);
        bus.report(1, 50, 60);
        if (failure == 0) bus.connected = false;
        if (failure == 1) bus.shortRead = 0x814e;
        if (failure == 2) bus.shortRead = 0x814f;
        if (failure == 3) bus.failAck = true;
        TEST_ASSERT_FALSE(touch.poll(130).pressed);
    }
}

void test_late_device_is_probed_again() {
    FakeBus bus;
    bus.connected = false;
    Gt911Touch<FakeBus> touch(bus);
    TEST_ASSERT_FALSE(touch.begin());
    bus.connected = true;
    bus.report(1, 10, 20);
    TEST_ASSERT_FALSE(touch.poll(999).pressed);
    TEST_ASSERT_TRUE(touch.poll(1000).pressed);
}

void test_report_age_handles_millis_rollover() {
    FakeBus bus;
    Gt911Touch<FakeBus> touch(bus);
    touch.begin();
    bus.report(1, 10, 20);
    TEST_ASSERT_TRUE(touch.poll(UINT32_MAX - 30).pressed);
    TEST_ASSERT_TRUE(touch.poll(20).pressed);
    TEST_ASSERT_FALSE(touch.poll(80).pressed);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_address_probe_and_product_identity);
    RUN_TEST(test_landscape_corners_and_centre);
    RUN_TEST(test_release_and_multitouch_do_not_press);
    RUN_TEST(test_out_of_range_coordinates_are_released);
    RUN_TEST(test_no_report_preserves_press_briefly_then_releases);
    RUN_TEST(test_i2c_failures_release_held_input);
    RUN_TEST(test_late_device_is_probed_again);
    RUN_TEST(test_report_age_handles_millis_rollover);
    return UNITY_END();
}
