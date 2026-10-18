/*
 * DeepSeek 驱动基于离线传输层的端到端测试。
 *
 * 这正是整套设计的目标：URL 构造、认证头、状态处理、JSON 解析和指标提取
 * 都真实执行，但不访问网络、不需要 API key，也不产生费用。只有 socket 被替换。
 */
#include <tokenprobe/probe.h>

#include "unity.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}

void tearDown(void) {}

/* DeepSeek 文档中 GET /user/balance 的响应体。 */
static const char *k_balance_body = "{"
                                    "  \"is_available\": true,"
                                    "  \"balance_infos\": ["
                                    "    {"
                                    "      \"currency\": \"CNY\","
                                    "      \"total_balance\": \"110.00\","
                                    "      \"granted_balance\": \"10.00\","
                                    "      \"topped_up_balance\": \"100.00\""
                                    "    }"
                                    "  ]"
                                    "}";

static const char *k_multi_currency_body =
    "{"
    "  \"is_available\": true,"
    "  \"balance_infos\": ["
    "    {\"currency\": \"CNY\", \"total_balance\": \"110.00\"},"
    "    {\"currency\": \"USD\", \"total_balance\": \"15.50\"}"
    "  ]"
    "}";

static const char *k_unavailable_body =
    "{\"is_available\": false, \"balance_infos\": [{\"currency\": \"CNY\", "
    "\"total_balance\": \"0.00\"}]}";

static tokenprobe_context_t *context_from(const tokenprobe_transport_t *transport) {
    tokenprobe_context_options_t options;
    tokenprobe_context_options_init(&options);
    options.transport = transport;
    return tokenprobe_context_create(&options);
}

/* 为 `body` 构建单条目传输层，并返回使用它的探测上下文。 */
static tokenprobe_context_t *context_serving(const char *body, long status,
                                             tokenprobe_transport_t *out_transport) {
    tokenprobe_mock_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.url_prefix = "https://api.deepseek.com/user/balance";
    entry.status = status;
    entry.body = body;

    TEST_ASSERT_EQUAL_INT(0, tokenprobe_transport_mock_create(&entry, 1, out_transport));
    return context_from(out_transport);
}

static const tokenprobe_metric_t *metric_by_id(const tokenprobe_result_t *result, const char *id) {
    int count = tokenprobe_result_metric_count(result);
    int i;
    for (i = 0; i < count; i++) {
        const tokenprobe_metric_t *metric = tokenprobe_result_metric(result, i);
        if (metric->id && strcmp(metric->id, id) == 0) {
            return metric;
        }
    }
    return NULL;
}

static void test_provider_is_registered(void) {
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");

    TEST_ASSERT_NOT_NULL(provider);
    TEST_ASSERT_EQUAL_STRING("DeepSeek", provider->display_name);
    TEST_ASSERT_EQUAL_STRING("DEEPSEEK_API_KEY", provider->env_var);
    TEST_ASSERT_EQUAL_INT(1, provider->supports_credit);
    /* DeepSeek 没有用量或费用接口。若声称支持，调用方会显示一个虚假的 0。 */
    TEST_ASSERT_EQUAL_INT(0, provider->supports_spend);
    TEST_ASSERT_NOT_NULL(strstr(provider->doc_url, "deepseek.com"));
}

static void test_provider_lookup_is_case_insensitive(void) {
    TEST_ASSERT_NOT_NULL(tokenprobe_provider_find("DeepSeek"));
    TEST_ASSERT_NOT_NULL(tokenprobe_provider_find("DEEPSEEK"));
    TEST_ASSERT_NULL(tokenprobe_provider_find("no-such-vendor"));
    TEST_ASSERT_NULL(tokenprobe_provider_find(NULL));
}

static void test_probe_reports_credit_metrics(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;
    tokenprobe_status_t status;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    status = tokenprobe_probe(provider, context, "sk-test", NULL, &result, &err);
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, status);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, err.code);

    TEST_ASSERT_EQUAL_STRING("deepseek", tokenprobe_result_provider(result));
    TEST_ASSERT_EQUAL_INT(1, tokenprobe_result_available(result));
    TEST_ASSERT_TRUE(tokenprobe_result_observed_at_ms(result) > 0);
    TEST_ASSERT_EQUAL_INT(200, (int)tokenprobe_result_http_status(result));
    TEST_ASSERT_EQUAL_INT(3, tokenprobe_result_metric_count(result));

    {
        const tokenprobe_metric_t *total = metric_by_id(result, "total_balance");
        char text[40];

        TEST_ASSERT_NOT_NULL(total);
        TEST_ASSERT_EQUAL_INT(TOKENPROBE_METRIC_CREDIT, total->kind);
        TEST_ASSERT_EQUAL_STRING("CNY", total->currency);
        TEST_ASSERT_EQUAL_STRING("currency", total->unit);
        TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                              tokenprobe_decimal_format(total->amount, text, sizeof(text)));
        TEST_ASSERT_EQUAL_STRING("110.00", text);
    }

    {
        const tokenprobe_metric_t *granted = metric_by_id(result, "granted_balance");
        char text[40];
        TEST_ASSERT_NOT_NULL(granted);
        TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                              tokenprobe_decimal_format(granted->amount, text, sizeof(text)));
        TEST_ASSERT_EQUAL_STRING("10.00", text);
    }

    /* 请求确实发往文档中的接口。 */
    TEST_ASSERT_EQUAL_size_t(1, tokenprobe_mock_request_count(&transport));
    TEST_ASSERT_EQUAL_STRING("https://api.deepseek.com/user/balance",
                             tokenprobe_mock_request_url(&transport, 0));
    TEST_ASSERT_EQUAL_STRING("Bearer sk-test",
                             tokenprobe_mock_request_header(&transport, 0, "Authorization"));
    /* 显式请求 identity：OpenSSL 传输层无法解码 gzip，驱动不得请求它。 */
    TEST_ASSERT_EQUAL_STRING("identity",
                             tokenprobe_mock_request_header(&transport, 0, "Accept-Encoding"));

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_probe_reads_key_from_environment(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    const char *header;

    TEST_ASSERT_NOT_NULL(context);

#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("DEEPSEEK_API_KEY", "from-env"));
#else
    TEST_ASSERT_EQUAL_INT(0, setenv("DEEPSEEK_API_KEY", "from-env", 1));
#endif

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, NULL, NULL, &result, NULL));
    header = tokenprobe_mock_request_header(&transport, 0, "Authorization");
    TEST_ASSERT_EQUAL_STRING("Bearer from-env", header);

#if defined(_WIN32)
    (void)_putenv_s("DEEPSEEK_API_KEY", "");
#else
    (void)unsetenv("DEEPSEEK_API_KEY");
#endif

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

/*
 * 显式传入的 key 必须优先于环境变量：否则调用方在账号之间切换时，
 * 会悄悄探测错误的账号。
 */
static void test_explicit_key_overrides_environment(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;

#if defined(_WIN32)
    (void)_putenv_s("DEEPSEEK_API_KEY", "from-env");
#else
    (void)setenv("DEEPSEEK_API_KEY", "from-env", 1);
#endif

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, "explicit", NULL, &result, NULL));
    TEST_ASSERT_EQUAL_STRING("Bearer explicit",
                             tokenprobe_mock_request_header(&transport, 0, "Authorization"));

#if defined(_WIN32)
    (void)_putenv_s("DEEPSEEK_API_KEY", "");
#else
    (void)unsetenv("DEEPSEEK_API_KEY");
#endif

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_probe_without_key_fails_before_any_request(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = (tokenprobe_result_t *)0x1;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

#if defined(_WIN32)
    (void)_putenv_s("DEEPSEEK_API_KEY", "");
#else
    (void)unsetenv("DEEPSEEK_API_KEY");
#endif

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_KEY,
                          tokenprobe_probe(provider, context, NULL, NULL, &result, &err));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_KEY, err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.hint, "DEEPSEEK_API_KEY"));
    /* 没有发出任何请求，凭据不会泄漏到非预期主机。 */
    TEST_ASSERT_EQUAL_size_t(0, tokenprobe_mock_request_count(&transport));

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_unauthorized_maps_to_auth_error(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving("{\"error\":\"bad key\"}", 401, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_AUTH,
                          tokenprobe_probe(provider, context, "sk-bad", NULL, &result, &err));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(401, err.http_status);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_server_error_maps_to_server_status(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving("upstream unavailable", 503, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_SERVER,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, &err));
    TEST_ASSERT_NULL(result);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_malformed_body_fails_without_partial_result(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving("{\"is_available\": tru", 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_JSON,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, &err));
    /* 失败的探测不能返回填写了一半的结果。 */
    TEST_ASSERT_NULL(result);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_missing_fields_report_schema_error(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context =
        context_serving("{\"is_available\": true, \"balance\": []}", 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_SCHEMA,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, &err));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "balance_infos"));

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_empty_balance_list_is_a_schema_error(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context =
        context_serving("{\"is_available\": true, \"balance_infos\": []}", 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_SCHEMA,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, &err));
    TEST_ASSERT_NULL(result);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

/* 缺少币种时不能报告为零，也不能套用其他币种。 */
static void test_missing_optional_amounts_are_skipped(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context =
        context_serving("{\"is_available\": true, \"balance_infos\": [{\"currency\": \"USD\", "
                        "\"total_balance\": \"7.25\"}]}",
                        200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;

    TEST_ASSERT_NOT_NULL(context);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, NULL));
    TEST_ASSERT_EQUAL_INT(1, tokenprobe_result_metric_count(result));
    TEST_ASSERT_EQUAL_STRING("USD", tokenprobe_result_metric(result, 0)->currency);

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

/* 多币种账号不能被悄悄求和；结果备注会说明这一点。 */
static void test_multi_currency_adds_a_note(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_multi_currency_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;

    TEST_ASSERT_NOT_NULL(context);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, NULL));
    TEST_ASSERT_EQUAL_INT(2, tokenprobe_result_metric_count(result));
    TEST_ASSERT_NOT_NULL(tokenprobe_result_note(result));
    TEST_ASSERT_NOT_NULL(strstr(tokenprobe_result_note(result), "must not be summed"));

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_unavailable_account_is_reported_as_not_available(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_unavailable_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;

    TEST_ASSERT_NOT_NULL(context);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, "sk-test", NULL, &result, NULL));
    TEST_ASSERT_EQUAL_INT(0, tokenprobe_result_available(result));

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

/* 覆盖 base URL 时，主机改变，但请求路径必须保持文档规定。 */
static void test_base_url_override_keeps_endpoint_path(void) {
    tokenprobe_mock_entry_t entry;
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context;
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;

    memset(&entry, 0, sizeof(entry));
    entry.url_prefix = "https://proxy.internal:8443/user/balance";
    entry.status = 200;
    entry.body = k_balance_body;

    TEST_ASSERT_EQUAL_INT(0, tokenprobe_transport_mock_create(&entry, 1, &transport));
    context = context_from(&transport);
    TEST_ASSERT_NOT_NULL(context);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK,
                          tokenprobe_probe(provider, context, "sk-test",
                                           "https://proxy.internal:8443/v1", &result, NULL));
    TEST_ASSERT_EQUAL_STRING("https://proxy.internal:8443/user/balance",
                             tokenprobe_mock_request_url(&transport, 0));

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_invalid_base_url_is_rejected(void) {
    tokenprobe_mock_entry_t entry;
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context;
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    memset(&entry, 0, sizeof(entry));
    entry.url_prefix = "https://api.deepseek.com";
    entry.status = 200;
    entry.body = k_balance_body;

    TEST_ASSERT_EQUAL_INT(0, tokenprobe_transport_mock_create(&entry, 1, &transport));
    context = context_from(&transport);
    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, tokenprobe_probe(provider, context, "sk-test",
                                                                       "not-a-url", &result, &err));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_URL, err.code);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_probe_ex_forwards_extra_headers(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_probe_options_t options;
    tokenprobe_http_header_t extra;
    tokenprobe_result_t *result = NULL;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_probe_options_init(&options);
    extra.name = "X-Trace";
    extra.value = "probe-test";
    options.headers = &extra;
    options.header_count = 1;

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_OK, tokenprobe_probe_ex(provider, context, "sk-test", NULL,
                                                             &options, &result, NULL));
    TEST_ASSERT_EQUAL_STRING("probe-test",
                             tokenprobe_mock_request_header(&transport, 0, "X-Trace"));

    tokenprobe_result_free(result);
    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_probe_ex_rejects_uninitialized_options(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_probe_options_t options;
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);
    memset(&options, 0, sizeof(options));

    TEST_ASSERT_EQUAL_INT(
        TOKENPROBE_ERR_INVALID_ARG,
        tokenprobe_probe_ex(provider, context, "sk-test", NULL, &options, &result, &err));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG, err.code);

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

static void test_null_arguments_are_rejected(void) {
    tokenprobe_transport_t transport;
    tokenprobe_context_t *context = context_serving(k_balance_body, 200, &transport);
    const tokenprobe_provider_t *provider = tokenprobe_provider_find("deepseek");
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;

    TEST_ASSERT_NOT_NULL(context);
    tokenprobe_error_init(&err);

    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG,
                          tokenprobe_probe(NULL, context, "k", NULL, &result, &err));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG,
                          tokenprobe_probe(provider, NULL, "k", NULL, &result, &err));
    TEST_ASSERT_EQUAL_INT(TOKENPROBE_ERR_INVALID_ARG,
                          tokenprobe_probe(provider, context, "k", NULL, NULL, &err));

    tokenprobe_context_destroy(context);
    tokenprobe_transport_mock_free(&transport);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_provider_is_registered);
    RUN_TEST(test_provider_lookup_is_case_insensitive);
    RUN_TEST(test_probe_reports_credit_metrics);
    RUN_TEST(test_probe_reads_key_from_environment);
    RUN_TEST(test_explicit_key_overrides_environment);
    RUN_TEST(test_probe_without_key_fails_before_any_request);
    RUN_TEST(test_unauthorized_maps_to_auth_error);
    RUN_TEST(test_server_error_maps_to_server_status);
    RUN_TEST(test_malformed_body_fails_without_partial_result);
    RUN_TEST(test_missing_fields_report_schema_error);
    RUN_TEST(test_empty_balance_list_is_a_schema_error);
    RUN_TEST(test_missing_optional_amounts_are_skipped);
    RUN_TEST(test_multi_currency_adds_a_note);
    RUN_TEST(test_unavailable_account_is_reported_as_not_available);
    RUN_TEST(test_base_url_override_keeps_endpoint_path);
    RUN_TEST(test_invalid_base_url_is_rejected);
    RUN_TEST(test_probe_ex_forwards_extra_headers);
    RUN_TEST(test_probe_ex_rejects_uninitialized_options);
    RUN_TEST(test_null_arguments_are_rejected);

    return UNITY_END();
}
