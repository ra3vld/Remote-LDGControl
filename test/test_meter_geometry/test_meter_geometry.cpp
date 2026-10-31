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

void test_rectangular_dial_keeps_zero_endpoints_on_the_opposite_pivot() {
    ldg::MeterGeometry geometry;
    geometry.resize(300, 280);
    TEST_ASSERT_EQUAL_INT(240, geometry.radius);
    pointIs(270, 260, geometry.forwardPivot);
    pointIs(30, 260, geometry.reflectedPivot);
    const ldg::MeterGeometry::Point sizes[] = {{300,280}, {300,300}, {480,320}, {301,281}};
    for (const auto size : sizes) {
        geometry.resize(size.x, size.y);
        TEST_ASSERT_EQUAL_INT(geometry.radius, geometry.forwardPivot.x - geometry.reflectedPivot.x);
        const auto zeroFwd = geometry.forwardNeedle(0);
        const auto zeroRef = geometry.reflectedNeedle(0);
        pointIs(geometry.reflectedPivot.x, geometry.reflectedPivot.y, zeroFwd);
        pointIs(geometry.forwardPivot.x, geometry.forwardPivot.y, zeroRef);
        pointIs(zeroFwd.x, zeroFwd.y, geometry.ticks[0][0].line.from);
        pointIs(zeroRef.x, zeroRef.y, geometry.ticks[1][0].line.from);
        for (unsigned tick = 0; tick <= 8; ++tick) {
            const auto fwd = geometry.forwardNeedle(float(tick) / 8);
            const auto ref = geometry.reflectedNeedle(float(tick) / 8);
            pointIs(fwd.x, fwd.y, geometry.ticks[0][tick].line.from);
            pointIs(ref.x, ref.y, geometry.ticks[1][tick].line.from);
        }
    }
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
    pointIs(183, 103, geometry.forwardNeedle(0.5f));
    pointIs(297, 103, geometry.reflectedNeedle(0.5f));
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
    RUN_TEST(test_rectangular_dial_keeps_zero_endpoints_on_the_opposite_pivot);
    RUN_TEST(test_repeated_strips_reuse_geometry_and_resizing_rebuilds_needles);
    RUN_TEST(test_telemetry_changes_do_not_rebuild_static_swr_curves);
    return UNITY_END();
}
