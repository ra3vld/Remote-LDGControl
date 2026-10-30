#include <unity.h>
#include <string.h>
#include "meter_geometry.h"

void setUp() {}
void tearDown() {}

static void pointIs(int x, int y, ldg::MeterGeometry::Point point) {
    TEST_ASSERT_EQUAL_INT(x, point.x);
    TEST_ASSERT_EQUAL_INT(y, point.y);
}

void test_meter_geometry_keeps_existing_pivots_and_scale_endpoints() {
    ldg::MeterGeometry geometry;
    TEST_ASSERT_TRUE(geometry.resize(300, 300));
    TEST_ASSERT_EQUAL_INT(260, geometry.radius);
    pointIs(280, 280, geometry.forwardPivot);
    pointIs(20, 280, geometry.reflectedPivot);
    pointIs(20, 280, geometry.ticks[0][0].line.from);
    pointIs(280, 20, geometry.ticks[0][8].line.from);
    pointIs(280, 280, geometry.ticks[1][0].line.from);
    pointIs(20, 20, geometry.ticks[1][8].line.from);
}

void test_needle_positions_match_zero_half_and_full_scale() {
    ldg::MeterGeometry geometry;
    geometry.resize(300, 300);
    pointIs(20, 280, geometry.forwardNeedle(0));
    pointIs(280, 280, geometry.reflectedNeedle(0));
    pointIs(97, 97, geometry.forwardNeedle(0.5f));
    pointIs(203, 97, geometry.reflectedNeedle(0.5f));
    pointIs(280, 20, geometry.forwardNeedle(1));
    pointIs(20, 20, geometry.reflectedNeedle(1));
}

void test_repeated_strips_reuse_geometry_and_resizing_rebuilds_needles() {
    ldg::MeterGeometry geometry;
    geometry.resize(300, 300);
    geometry.forwardNeedle(0.5f);
    geometry.reflectedNeedle(0.5f);
    for (int strip = 0; strip < 64; ++strip) {
        TEST_ASSERT_FALSE(geometry.resize(300, 300));
        pointIs(97, 97, geometry.forwardNeedle(0.5f));
        pointIs(203, 97, geometry.reflectedNeedle(0.5f));
    }
    TEST_ASSERT_TRUE(geometry.resize(480, 320));
    pointIs(263, 103, geometry.forwardNeedle(0.5f));
    pointIs(217, 103, geometry.reflectedNeedle(0.5f));
}

void test_telemetry_changes_do_not_rebuild_static_swr_curves() {
    ldg::MeterGeometry geometry;
    geometry.resize(300, 300);
    unsigned char saved[sizeof(geometry.curves)];
    memcpy(saved, geometry.curves, sizeof(saved));
    for (int reading = 0; reading < 100; ++reading) {
        geometry.forwardNeedle(float(reading) / 100);
        geometry.reflectedNeedle(float(100 - reading) / 100);
    }
    TEST_ASSERT_EQUAL_MEMORY(saved, geometry.curves, sizeof(saved));
    for (const auto& curve : geometry.curves) {
        TEST_ASSERT_GREATER_THAN_UINT8(0, curve.count);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(39, curve.count);
    }
    TEST_ASSERT_LESS_THAN_UINT32(2048, sizeof(geometry));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_meter_geometry_keeps_existing_pivots_and_scale_endpoints);
    RUN_TEST(test_needle_positions_match_zero_half_and_full_scale);
    RUN_TEST(test_repeated_strips_reuse_geometry_and_resizing_rebuilds_needles);
    RUN_TEST(test_telemetry_changes_do_not_rebuild_static_swr_curves);
    return UNITY_END();
}
