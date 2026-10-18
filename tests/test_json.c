/* JSON 解析器的严格性。解析器是内部实现，因此测试包含内部头文件；
 * 它保证的行为可以避免把损坏的余额报告成看似合理的数字。 */
#include <tokenprobe/probe.h>

#include "tp_internal.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}

void tearDown(void) {}

static tokenprobe_error_t err;

static tp_json_doc *parse(const char *text) {
    tp_json_doc *doc = NULL;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tp_json_parse(text, strlen(text), &doc, &err));
    TEST_ASSERT_NOT_NULL(doc);
    return doc;
}

static void expect_failure(const char *text) {
    tp_json_doc *doc = NULL;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_JSON, tp_json_parse(text, strlen(text), &doc, &err));
    TEST_ASSERT_NULL(doc);
}

/*
 * DeepSeek 文档中 GET /user/balance 的原始响应体。金额是字符串，
 * 这正是需要十进制层的原因。
 */
static const char *k_deepseek_balance = "{\n"
                                        "  \"is_available\": true,\n"
                                        "  \"balance_infos\": [\n"
                                        "    {\n"
                                        "      \"currency\": \"CNY\",\n"
                                        "      \"total_balance\": \"110.00\",\n"
                                        "      \"granted_balance\": \"10.00\",\n"
                                        "      \"topped_up_balance\": \"100.00\"\n"
                                        "    }\n"
                                        "  ]\n"
                                        "}\n";

static void test_parses_provider_payload(void) {
    tp_json_doc *doc = parse(k_deepseek_balance);
    const tp_json *root = tp_json_root(doc);
    const tp_json *infos;
    int available = 0;

    TEST_ASSERT_TRUE(tp_json_is_object(root));
    TEST_ASSERT_EQUAL_INT(0, tp_json_bool(tp_json_get(root, "is_available"), &available));
    TEST_ASSERT_EQUAL_INT(1, available);

    infos = tp_json_get(root, "balance_infos");
    TEST_ASSERT_TRUE(tp_json_is_array(infos));
    TEST_ASSERT_EQUAL_size_t(1, tp_json_size(infos));

    {
        const tp_json *entry = tp_json_at(infos, 0);
        tokenprobe_decimal_t amount;
        TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                              tp_json_field_decimal(entry, "total_balance", &amount));
        TEST_ASSERT_EQUAL_INT64(11000, amount.unscaled);
        TEST_ASSERT_EQUAL_INT(2, amount.scale);
    }

    tp_json_free(doc);
}

static void test_object_lookup_and_types(void) {
    tp_json_doc *doc = parse("{\"a\":1,\"b\":\"x\",\"c\":null,\"d\":[1,2,3],\"e\":false}");
    const tp_json *root = tp_json_root(doc);

    TEST_ASSERT_TRUE(tp_json_is_number(tp_json_get(root, "a")));
    TEST_ASSERT_TRUE(tp_json_is_string(tp_json_get(root, "b")));
    TEST_ASSERT_TRUE(tp_json_is_null(tp_json_get(root, "c")));
    TEST_ASSERT_TRUE(tp_json_is_array(tp_json_get(root, "d")));
    TEST_ASSERT_TRUE(tp_json_is_bool(tp_json_get(root, "e")));
    TEST_ASSERT_NULL(tp_json_get(root, "missing"));
    TEST_ASSERT_EQUAL_size_t(3, tp_json_size(tp_json_get(root, "d")));
    TEST_ASSERT_NULL(tp_json_at(tp_json_get(root, "d"), 3));

    tp_json_free(doc);
}

static void test_parses_string_escapes(void) {
    tp_json_doc *doc = parse("{\"s\":\"a\\\"b\\\\c\\/d\\ne\\u0041\\u00e9\"}");
    const tp_json *value = tp_json_get(tp_json_root(doc), "s");
    size_t len = 0;
    const char *text = tp_json_string(value, &len);

    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\neA\xc3\xa9", text);
    TEST_ASSERT_EQUAL_size_t(strlen(text), len);

    tp_json_free(doc);
}

/* 代理对必须正确合并；孤立代理项属于硬错误。 */
static void test_parses_astral_surrogate_pair(void) {
    tp_json_doc *doc = parse("{\"s\":\"\\ud83d\\ude00\"}");
    const char *text = tp_json_string(tp_json_get(tp_json_root(doc), "s"), NULL);
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", text);
    tp_json_free(doc);
}

static void test_rejects_lone_surrogates(void) {
    expect_failure("{\"s\":\"\\ud83d\"}");
    expect_failure("{\"s\":\"\\ude00\"}");
    expect_failure("{\"s\":\"\\ud83dx\"}");
}

static void test_rejects_duplicate_keys(void) {
    /* 后一个键会悄悄覆盖前一个；对余额字段来说，这比解析失败更糟。 */
    expect_failure("{\"total_balance\":\"1.00\",\"total_balance\":\"2.00\"}");
}

static void test_rejects_trailing_data(void) {
    expect_failure("{\"a\":1} garbage");
    expect_failure("[1,2] [3]");
    expect_failure("{} {}");
}

static void test_rejects_malformed_documents(void) {
    expect_failure("");
    expect_failure("{");
    expect_failure("}");
    expect_failure("{\"a\":}");
    expect_failure("{\"a\":1,}");
    expect_failure("{\"a\" 1}");
    expect_failure("{a:1}");
    expect_failure("[1,2");
    expect_failure("[1,]");
    expect_failure("[1 2]");
    expect_failure("'single'");
    expect_failure("nan");
    expect_failure("undefined");
    expect_failure("tru");
}

static void test_rejects_raw_control_characters_in_strings(void) {
    tp_json_doc *doc = NULL;
    const char with_newline[] = "{\"s\":\"a\nb\"}";
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_JSON,
                          tp_json_parse(with_newline, sizeof(with_newline) - 1, &doc, &err));
}

static void test_rejects_bad_numbers(void) {
    expect_failure("[01]");
    expect_failure("[1.]");
    expect_failure("[.1]");
    expect_failure("[1e]");
    expect_failure("[-]");
    expect_failure("[+1]");
}

/* 限制嵌套深度，避免恶意或损坏的响应体耗尽 C 栈。 */
static void test_rejects_excessive_nesting(void) {
    char deep[TOKENPROBE_JSON_MAX_DEPTH * 4 + 16];
    size_t i;
    size_t n = 0;

    for (i = 0; i < TOKENPROBE_JSON_MAX_DEPTH + 4; i++) {
        deep[n++] = '[';
    }
    for (i = 0; i < TOKENPROBE_JSON_MAX_DEPTH + 4; i++) {
        deep[n++] = ']';
    }
    deep[n] = '\0';
    expect_failure(deep);
}

static void test_accepts_nesting_at_the_limit(void) {
    char deep[TOKENPROBE_JSON_MAX_DEPTH * 4 + 16];
    size_t i;
    size_t n = 0;
    tp_json_doc *doc = NULL;

    for (i = 0; i < TOKENPROBE_JSON_MAX_DEPTH; i++) {
        deep[n++] = '[';
    }
    for (i = 0; i < TOKENPROBE_JSON_MAX_DEPTH; i++) {
        deep[n++] = ']';
    }
    deep[n] = '\0';

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tp_json_parse(deep, n, &doc, &err));
    tp_json_free(doc);
}

/* 解析器把数字保留为原始文本，金额从不经过 double；十进制层直接读取文本。 */
static void test_decimal_field_from_number_and_string(void) {
    tp_json_doc *doc = parse("{\"n\":150.25,\"s\":\"150.25\"}");
    tokenprobe_decimal_t from_number;
    tokenprobe_decimal_t from_string;

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tp_json_field_decimal(tp_json_root(doc), "n", &from_number));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tp_json_field_decimal(tp_json_root(doc), "s", &from_string));
    TEST_ASSERT_EQUAL_INT(0, tokenprobe_decimal_compare(from_number, from_string));

    tp_json_free(doc);
}

static void test_decimal_field_reports_schema_errors(void) {
    tp_json_doc *doc = parse("{\"s\":\"abc\",\"o\":{}}");
    tokenprobe_decimal_t value;

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_SCHEMA,
                          tp_json_field_decimal(tp_json_root(doc), "missing", &value));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_SCHEMA,
                          tp_json_field_decimal(tp_json_root(doc), "o", &value));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_PARSE,
                          tp_json_field_decimal(tp_json_root(doc), "s", &value));

    tp_json_free(doc);
}

static void test_reports_line_numbers(void) {
    tp_json_doc *doc = NULL;
    /* 重复键位于第 3 行，错误信息应指向该行。 */
    const char *body = "{\n  \"a\": 1,\n  \"a\": 2\n}";
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_JSON, tp_json_parse(body, strlen(body), &doc, &err));
    TEST_ASSERT_NOT_NULL(strstr(err.message, "line 3"));
}

static void test_null_arguments(void) {
    tp_json_doc *doc = NULL;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG, tp_json_parse(NULL, 0, &doc, &err));
}

int main(void) {
    tokenprobe_error_init(&err);

    UNITY_BEGIN();

    RUN_TEST(test_parses_provider_payload);
    RUN_TEST(test_object_lookup_and_types);
    RUN_TEST(test_parses_string_escapes);
    RUN_TEST(test_parses_astral_surrogate_pair);
    RUN_TEST(test_rejects_lone_surrogates);
    RUN_TEST(test_rejects_duplicate_keys);
    RUN_TEST(test_rejects_trailing_data);
    RUN_TEST(test_rejects_malformed_documents);
    RUN_TEST(test_rejects_raw_control_characters_in_strings);
    RUN_TEST(test_rejects_bad_numbers);
    RUN_TEST(test_rejects_excessive_nesting);
    RUN_TEST(test_accepts_nesting_at_the_limit);
    RUN_TEST(test_decimal_field_from_number_and_string);
    RUN_TEST(test_decimal_field_reports_schema_errors);
    RUN_TEST(test_reports_line_numbers);
    RUN_TEST(test_null_arguments);

    return UNITY_END();
}
