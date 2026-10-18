/* 精确十进制契约：解析、精度边界上的舍入，以及余额字段可能触及的 int64 边界。 */
#include <tokenprobe/probe.h>

#include "unity.h"

#include <string.h>

void setUp(void) {}

void tearDown(void) {}

static tokenprobe_decimal_t parse_ok(const char *text) {
    tokenprobe_decimal_t value;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tokenprobe_decimal_parse(text, strlen(text), &value));
    return value;
}

static void expect_parse_failure(const char *text) {
    tokenprobe_decimal_t value;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_PARSE,
                          tokenprobe_decimal_parse(text, strlen(text), &value));
}

static void expect_format(tokenprobe_decimal_t value, const char *expected) {
    char buffer[40];
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tokenprobe_decimal_format(value, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING(expected, buffer);
}

/* DeepSeek 以十进制字符串返回余额，这是真实数据形态。 */
static void test_parse_balance_strings(void) {
    expect_format(parse_ok("110.00"), "110.00");
    expect_format(parse_ok("10.00"), "10.00");
    expect_format(parse_ok("100.00"), "100.00");
    expect_format(parse_ok("0"), "0");
    expect_format(parse_ok("0.000000001"), "0.000000001");
}

static void test_parse_signs_and_exponents(void) {
    expect_format(parse_ok("-12.5"), "-12.5");
    expect_format(parse_ok("+3.25"), "3.25");
    expect_format(parse_ok("1.5e3"), "1500");
    expect_format(parse_ok("15e-1"), "1.5");
    expect_format(parse_ok("2E2"), "200");
    expect_format(parse_ok("1.5E+2"), "150");
}

/* 容忍首尾空白，因为部分供应商会给数字字段补空格。 */
static void test_parse_tolerates_surrounding_blanks(void) {
    tokenprobe_decimal_t value;
    const char *padded = "  42.50\t";
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tokenprobe_decimal_parse(padded, strlen(padded), &value));
    expect_format(value, "42.50");
}

static void test_parse_rejects_malformed_input(void) {
    expect_parse_failure("");
    expect_parse_failure("   ");
    expect_parse_failure(".");
    expect_parse_failure("-");
    expect_parse_failure("+");
    expect_parse_failure("12.");
    expect_parse_failure(".5");
    expect_parse_failure("1.2.3");
    expect_parse_failure("1,5");
    expect_parse_failure("abc");
    expect_parse_failure("12abc");
    expect_parse_failure("1e");
    expect_parse_failure("1e+");
    expect_parse_failure("--1");
    expect_parse_failure("1 2");
}

/* 整数位超出 int64 容量时必须明确失败，不能截断成一个看似合理的错误余额。 */
static void test_parse_rejects_overflow_before_rounding(void) {
    expect_parse_failure("99999999999999999999999999");
    expect_parse_failure("9999999999999999999");
    expect_parse_failure("1e30");
}

/* 位于 int64 边界的值也必须能往返。 */
static void test_parse_round_trips_extremes(void) {
    expect_format(parse_ok("9223372036854775807"), "9223372036854775807");
    expect_format(parse_ok("-9223372036854775808"), "-9223372036854775808");
}

/* scale 限制为 9 位小数，第 10 位按四舍五入（远离零）处理。 */
static void test_parse_rounds_at_precision_boundary(void) {
    expect_format(parse_ok("1.0000000005"), "1.000000001"); /* half, away from zero */
    expect_format(parse_ok("1.0000000004"), "1.000000000");
    expect_format(parse_ok("-1.0000000005"), "-1.000000001");
    expect_format(parse_ok("0.1234567891"), "0.123456789");
    expect_format(parse_ok("0.1234567895"), "0.123456790");
}

/* 略高于半值的数必须进位，即使累加器精度之外的位已被丢弃。
 * 这是 sticky-bit 场景。 */
static void test_parse_rounds_with_dropped_nonzero_digits(void) {
    tokenprobe_decimal_t value = parse_ok("1.0000000005000000001");
    expect_format(value, "1.000000001");

    value = parse_ok("1.0000000004999999999");
    expect_format(value, "1.000000000");
}

static void test_normalises_negative_zero(void) {
    tokenprobe_decimal_t zero = parse_ok("-0.0");
    TEST_ASSERT_EQUAL_INT(0, zero.unscaled);
    TEST_ASSERT_EQUAL_INT(1, tokenprobe_decimal_is_zero(zero));
    expect_format(zero, "0");
}

static void test_format_rejects_short_buffer(void) {
    char small[4];
    tokenprobe_decimal_t value = parse_ok("1234.5678");
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG,
                          tokenprobe_decimal_format(value, small, sizeof(small)));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG, tokenprobe_decimal_format(value, NULL, 0));
}

/* 比较必须按数值进行，而不是直接比较 unscaled：1.5 和 1.50 的 scale 不同，
 * 但数值相等。 */
static void test_compare_aligns_scale(void) {
    tokenprobe_decimal_t a = parse_ok("1.5");
    tokenprobe_decimal_t b = parse_ok("1.50");
    tokenprobe_decimal_t c = parse_ok("1.51");

    TEST_ASSERT_EQUAL_INT(0, tokenprobe_decimal_compare(a, b));
    TEST_ASSERT_EQUAL_INT(-1, tokenprobe_decimal_compare(a, c));
    TEST_ASSERT_EQUAL_INT(1, tokenprobe_decimal_compare(c, a));
    TEST_ASSERT_EQUAL_INT(0, tokenprobe_decimal_compare(a, a));
}

static void test_compare_handles_negative_values(void) {
    tokenprobe_decimal_t minus_two = parse_ok("-2.00");
    tokenprobe_decimal_t minus_one = parse_ok("-1.00");

    TEST_ASSERT_EQUAL_INT(-1, tokenprobe_decimal_compare(minus_two, minus_one));
    TEST_ASSERT_EQUAL_INT(1, tokenprobe_decimal_compare(minus_one, minus_two));
}

static void test_parse_null_arguments(void) {
    tokenprobe_decimal_t value;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG, tokenprobe_decimal_parse(NULL, 0, &value));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG, tokenprobe_decimal_parse("1", 1, NULL));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_balance_strings);
    RUN_TEST(test_parse_signs_and_exponents);
    RUN_TEST(test_parse_tolerates_surrounding_blanks);
    RUN_TEST(test_parse_rejects_malformed_input);
    RUN_TEST(test_parse_rejects_overflow_before_rounding);
    RUN_TEST(test_parse_round_trips_extremes);
    RUN_TEST(test_parse_rounds_at_precision_boundary);
    RUN_TEST(test_parse_rounds_with_dropped_nonzero_digits);
    RUN_TEST(test_normalises_negative_zero);
    RUN_TEST(test_format_rejects_short_buffer);
    RUN_TEST(test_compare_aligns_scale);
    RUN_TEST(test_compare_handles_negative_values);
    RUN_TEST(test_parse_null_arguments);

    return UNITY_END();
}
