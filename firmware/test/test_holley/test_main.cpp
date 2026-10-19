// Host tests for src/holley.h. Run: pio test -e native
#include <unity.h>
#include "holley.h"
using namespace holley;

void setUp(){}
void tearDown(){}

void test_input_ids(){
  TEST_ASSERT_EQUAL_HEX32(0x1E2027C1,inputId(1));
  TEST_ASSERT_EQUAL_HEX32(0x1E2067C1,inputId(2));
  // Bench A candidates, documented in docs/CAN_PROTOCOL.md.
  TEST_ASSERT_EQUAL_HEX32(0x1E20A7C1,inputId(3));
  TEST_ASSERT_EQUAL_HEX32(0x1E20E7C1,inputId(4));
  TEST_ASSERT_EQUAL_HEX32(0x1E2127C1,inputId(5));
  TEST_ASSERT_EQUAL_HEX32(0x1E2167C1,inputId(6));
}

void test_output_rows_round_trip(){
  for(unsigned row=1;row<=OUTPUT_ROWS;++row) TEST_ASSERT_EQUAL_UINT(row,outputRow(outputRowId(row,0x150),0x150));
  TEST_ASSERT_EQUAL_HEX32(0x1E6DD150,outputRowId(62,0x150)); // DBC IO_OUTPUT62
  TEST_ASSERT_EQUAL_UINT(0,outputRow(outputRowId(5,0x150),0x151));            // Other ECU serial
  TEST_ASSERT_EQUAL_UINT(0,outputRow(0x1E5E5150,0x150));                       // IO_INPUT80
  TEST_ASSERT_EQUAL_UINT(0,outputRow(outputRowId(62,0x150)+0x4000,0x150));     // Past row 62
}

void test_float_big_endian(){
  uint8_t d[4];
  putFloatBE(1.0f,d);
  TEST_ASSERT_EQUAL_HEX8(0x3F,d[0]);TEST_ASSERT_EQUAL_HEX8(0x80,d[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00,d[2]);TEST_ASSERT_EQUAL_HEX8(0x00,d[3]);
  putFloatBE(123.25f,d);
  TEST_ASSERT_EQUAL_FLOAT(123.25f,bitsToFloat(getU32BE(d)));
}

void test_flex_math(){
  TEST_ASSERT_TRUE(flexFrequencyOk(49.5f));TEST_ASSERT_FALSE(flexFrequencyOk(49.4f));
  TEST_ASSERT_TRUE(flexFrequencyOk(150.5f));TEST_ASSERT_FALSE(flexFrequencyOk(150.6f));
  TEST_ASSERT_EQUAL_FLOAT(0,ethanolPercent(49.6f));
  TEST_ASSERT_EQUAL_FLOAT(30,ethanolPercent(80));
  TEST_ASSERT_EQUAL_FLOAT(100,ethanolPercent(150.4f));
  TEST_ASSERT_EQUAL_FLOAT(50,flexInputHz(49.6f));
  TEST_ASSERT_EQUAL_FLOAT(150,flexInputHz(150.4f));
  TEST_ASSERT_TRUE(flexWidthOk(1000,10000));TEST_ASSERT_FALSE(flexWidthOk(999,10000));
  TEST_ASSERT_TRUE(flexWidthOk(5000,10000));TEST_ASSERT_FALSE(flexWidthOk(5001,10000));
  TEST_ASSERT_FALSE(flexWidthOk(5000,5000));
  TEST_ASSERT_EQUAL_FLOAT(-40,flexTempC(1));
  TEST_ASSERT_EQUAL_FLOAT(125,flexTempC(5));
}

void test_gm_cts_curve(){
  TEST_ASSERT_EQUAL_FLOAT(100700,gmCtsOhms(-50));
  TEST_ASSERT_EQUAL_FLOAT(62,gmCtsOhms(150));
  TEST_ASSERT_FLOAT_WITHIN(1,3400,gmCtsOhms(20));
  // Monotonic falling between table points.
  for(float c=-39;c<140;c+=1) TEST_ASSERT_TRUE(gmCtsOhms(c)<gmCtsOhms(c-1));
}

int main(){
  UNITY_BEGIN();
  RUN_TEST(test_input_ids);
  RUN_TEST(test_output_rows_round_trip);
  RUN_TEST(test_float_big_endian);
  RUN_TEST(test_flex_math);
  RUN_TEST(test_gm_cts_curve);
  return UNITY_END();
}
