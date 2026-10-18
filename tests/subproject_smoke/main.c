/* 下游 add_subdirectory() 消费方冒烟测试。 */
#include <tokenprobe/probe.h>

#include <string.h>

int main(void) {
    tokenprobe_decimal_t a;
    tokenprobe_decimal_t b;
    tokenprobe_result_t *result;

    if (tokenprobe_decimal_parse("1.50", 4, &a) != TOKENPROBE_OK) {
        return 1;
    }
    if (tokenprobe_decimal_parse("1.5", 3, &b) != TOKENPROBE_OK) {
        return 2;
    }
    /* scale 不同但数值相等：比较时必须对齐。 */
    if (tokenprobe_decimal_compare(a, b) != 0) {
        return 3;
    }

    result = tokenprobe_result_create();
    if (result == NULL) {
        return 4;
    }
    if (tokenprobe_result_metric_count(result) != 0) {
        return 5;
    }
    tokenprobe_result_free(result);

    if (strncmp(tokenprobe_status_name(TOKENPROBE_ERR_AUTH), "AUTH", 4) != 0) {
        return 6;
    }
    return 0;
}
