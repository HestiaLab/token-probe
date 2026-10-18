/* URL 拆分。解析器不在公共路径上，tp_url_parse_into() 属于内部实现，
 * 因此测试通过内部头文件驱动它，与各驱动的用法一致。 */
#include <tokenprobe/probe.h>

#include "tp_internal.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}

void tearDown(void) {}

static struct {
    tp_arena *arena;
} fixture;

static tokenprobe_error_t err;

static tokenprobe_status_t split(const char *url, tp_url *out) {
    tokenprobe_status_t status = tp_url_parse_into(fixture.arena, url, out, &err);
    return status;
}

static void test_parses_full_url(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://api.deepseek.com/user/balance", &url));
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("api.deepseek.com", url.host);
    TEST_ASSERT_EQUAL_INT(443, url.port);
    TEST_ASSERT_EQUAL_STRING("/user/balance", url.path);
    TEST_ASSERT_NULL(url.query);
    TEST_ASSERT_TRUE(url.is_secure);
}

static void test_parses_explicit_port_and_query(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          split("http://localhost:8080/v1/costs?start=1&limit=10", &url));
    TEST_ASSERT_EQUAL_STRING("http", url.scheme);
    TEST_ASSERT_EQUAL_STRING("localhost", url.host);
    TEST_ASSERT_EQUAL_INT(8080, url.port);
    TEST_ASSERT_EQUAL_STRING("/v1/costs", url.path);
    TEST_ASSERT_EQUAL_STRING("start=1&limit=10", url.query);
    TEST_ASSERT_FALSE(url.is_secure);
}

static void test_defaults_path_and_port(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://example.com", &url));
    TEST_ASSERT_EQUAL_STRING("/", url.path);
    TEST_ASSERT_EQUAL_INT(443, url.port);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("http://example.com", &url));
    TEST_ASSERT_EQUAL_INT(80, url.port);
}

/* 有路径无查询、有查询无路径都是合法的。 */
static void test_query_without_path(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://example.com?a=1", &url));
    TEST_ASSERT_EQUAL_STRING("/", url.path);
    TEST_ASSERT_EQUAL_STRING("a=1", url.query);
}

static void test_fragment_is_ignored(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://example.com/v1/x#frag", &url));
    TEST_ASSERT_EQUAL_STRING("/v1/x", url.path);
    TEST_ASSERT_NULL(url.query);
}

/* base URL 可能指向带版本前缀的 API 根；驱动复用的是主机和端口。 */
static void test_versioned_base_url(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://api.openai.com/v1", &url));
    TEST_ASSERT_EQUAL_STRING("api.openai.com", url.host);
    TEST_ASSERT_EQUAL_STRING("/v1", url.path);
}

static void test_rejects_missing_scheme(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("api.deepseek.com/user/balance", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("://example.com", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("example.com", &url));
}

static void test_rejects_unsupported_scheme(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("ftp://example.com/x", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("file:///etc/passwd", &url));
}

/*
 * authority 中的凭据会进入日志和传输层目标字符串，因此直接拒绝，
 * 而不是悄悄转发。
 */
static void test_rejects_userinfo(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL,
                          split("https://user:secret@example.com/v1", &url));
}

static void test_rejects_bad_port(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("https://example.com:0/x", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("https://example.com:99999/x", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("https://example.com:/x", &url));
}

static void test_rejects_empty_host(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("https:///path", &url));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split("https://", &url));
}

static void test_accepts_ipv6_literal(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("https://[::1]:8443/v1/balance", &url));
    TEST_ASSERT_EQUAL_STRING("[::1]", url.host);
    TEST_ASSERT_EQUAL_INT(8443, url.port);
}

static void test_scheme_is_case_insensitive(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, split("HTTPS://Example.COM/V1", &url));
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("Example.COM", url.host);
}

static void test_null_arguments(void) {
    tp_url url;
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, split(NULL, &url));
}

int main(void) {
    fixture.arena = tp_arena_create(2048);
    if (!fixture.arena) {
        return 1;
    }
    tokenprobe_error_init(&err);

    UNITY_BEGIN();

    RUN_TEST(test_parses_full_url);
    RUN_TEST(test_parses_explicit_port_and_query);
    RUN_TEST(test_defaults_path_and_port);
    RUN_TEST(test_query_without_path);
    RUN_TEST(test_fragment_is_ignored);
    RUN_TEST(test_versioned_base_url);
    RUN_TEST(test_rejects_missing_scheme);
    RUN_TEST(test_rejects_unsupported_scheme);
    RUN_TEST(test_rejects_userinfo);
    RUN_TEST(test_rejects_bad_port);
    RUN_TEST(test_rejects_empty_host);
    RUN_TEST(test_accepts_ipv6_literal);
    RUN_TEST(test_scheme_is_case_insensitive);
    RUN_TEST(test_null_arguments);

    int result = UNITY_END();
    tp_arena_destroy(fixture.arena);
    return result;
}
