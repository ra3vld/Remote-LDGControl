#include <unity.h>
#include <vector>
#include "needle_motion.h"
#include "meter_face_cache.h"
void setUp() {}
void tearDown() {}
void test_motion_is_time_based_bounded_and_reaches_the_latest_target() {
    ldg::NeedleMotion a,b;
    a.snap(0,0);b.snap(0,0);a.target(1);b.target(1);
    for(uint32_t now=10;now<=100;now+=10)a.advance(now);
    b.advance(100);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f,a.value(),b.value());
    TEST_ASSERT_GREATER_THAN_FLOAT(0.9f,a.value());
    TEST_ASSERT_LESS_THAN_FLOAT(1.f,a.value());
    a.target(0.25f);float previous=a.value();
    for(uint32_t now=130;now<1000;now+=30) {
        const float value=a.advance(now);
        TEST_ASSERT_TRUE(value<=previous && value>=0.25f);previous=value;
    }
    TEST_ASSERT_EQUAL_FLOAT(0.25f,a.value());
}
void test_motion_handles_stalls_wrap_scale_changes_and_missing_data_without_replay() {
    ldg::NeedleMotion motion;
    motion.snap(0,0xfffffff0u);motion.target(1);
    TEST_ASSERT_GREATER_THAN_FLOAT(0,motion.advance(20));
    motion.target(0.4f);TEST_ASSERT_EQUAL_FLOAT(0.4f,motion.advance(1000));
    motion.snap(0.9f,1001);TEST_ASSERT_EQUAL_FLOAT(0.9f,motion.value());
    motion.snap(NAN,1002);TEST_ASSERT_EQUAL_FLOAT(0,motion.value());
}
void test_sparse_face_roundtrips_exact_pixels_clips_and_long_runs() {
    ldg::MeterFaceCache cache;
    const int width=300,height=3;
    std::vector<uint16_t> pixels(width*height,0x1234);
    for(int x=127;x<170;++x)pixels[width+x]=uint16_t(x*123);
    for(int x=0;x<width;++x)pixels[2*width+x]=uint16_t(x);
    TEST_ASSERT_TRUE(cache.count(width,height,0x1234));
    for(int y=0;y<height;++y)TEST_ASSERT_TRUE(cache.append(pixels.data()+y*width));
    TEST_ASSERT_TRUE(cache.allocate());
    for(int y=0;y<height;++y)TEST_ASSERT_TRUE(cache.append(pixels.data()+y*width));
    TEST_ASSERT_TRUE(cache.finish());
    std::vector<uint16_t> actual(width);
    for(int y=0;y<height;++y) {
        TEST_ASSERT_TRUE(cache.copyRow(actual.data(),y,0,width-1));
        TEST_ASSERT_EQUAL_MEMORY(pixels.data()+y*width,actual.data(),width*2);
        TEST_ASSERT_TRUE(cache.copyRow(actual.data(),y,126,173));
        TEST_ASSERT_EQUAL_MEMORY(pixels.data()+y*width+126,actual.data(),48*2);
    }
    TEST_ASSERT_FALSE(cache.copyRow(actual.data(),3,0,1));
    TEST_ASSERT_FALSE(cache.count(481,320,0));
    TEST_ASSERT_FALSE(cache.valid());
}
void test_oversized_background_fails_closed_to_vector_fallback() {
    ldg::MeterFaceCache cache;
    TEST_ASSERT_TRUE(cache.count(480,320,0));
    std::vector<uint16_t> noisy(480);
    for(int i=0;i<480;++i)noisy[i]=uint16_t(i+1);
    bool success=true;
    for(int row=0;row<320 && success;++row)success=cache.append(noisy.data());
    TEST_ASSERT_FALSE(success);TEST_ASSERT_FALSE(cache.valid());
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_motion_is_time_based_bounded_and_reaches_the_latest_target);
    RUN_TEST(test_motion_handles_stalls_wrap_scale_changes_and_missing_data_without_replay);
    RUN_TEST(test_sparse_face_roundtrips_exact_pixels_clips_and_long_runs);
    RUN_TEST(test_oversized_background_fails_closed_to_vector_fallback);
    return UNITY_END();
}
