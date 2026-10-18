/* 上下文与平台传输层生命周期测试。 */
#include <tokenprobe/probe.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

/* 平台传输层是进程级借用实例：多次获取返回同一指针，且不提供 destroy。 */
static void test_platform_transport_is_process_wide(void) {
    const tokenprobe_transport_t *first = tokenprobe_transport_platform();
    const tokenprobe_transport_t *second = tokenprobe_transport_platform();

    if (!first) {
        TEST_IGNORE_MESSAGE("本构建没有平台传输层");
        return;
    }
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_NOT_NULL(first->perform);
    TEST_ASSERT_NULL(first->destroy);
}

/* 销毁一个默认 context 不能影响下一个 context 使用的平台传输层。 */
static void test_contexts_do_not_own_platform_transport(void) {
    const tokenprobe_transport_t *platform = tokenprobe_transport_platform();
    tokenprobe_context_options_t options;
    tokenprobe_context_t *first;
    tokenprobe_context_t *second;

    if (!platform) {
        TEST_IGNORE_MESSAGE("本构建没有平台传输层");
        return;
    }

    tokenprobe_context_options_init(&options);

    first = tokenprobe_context_create(&options);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_PTR(platform->perform, tokenprobe_context_transport(first)->perform);
    TEST_ASSERT_EQUAL_PTR(platform->impl, tokenprobe_context_transport(first)->impl);
    tokenprobe_context_destroy(first);

    second = tokenprobe_context_create(&options);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_PTR(platform->perform, tokenprobe_context_transport(second)->perform);
    TEST_ASSERT_EQUAL_PTR(platform->impl, tokenprobe_context_transport(second)->impl);
    TEST_ASSERT_NOT_NULL(tokenprobe_context_transport(second)->impl);
    tokenprobe_context_destroy(second);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_platform_transport_is_process_wide);
    RUN_TEST(test_contexts_do_not_own_platform_transport);

    return UNITY_END();
}
