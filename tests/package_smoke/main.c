/* 下游 C 消费方冒烟测试。
 *
 * 通过 find_package 针对*已安装*的 TokenProbe 包运行，验证真实消费方看到的内容：
 * 导出的目标、安装的头文件、C 链接方式，以及库不会传递依赖 OpenSSL 或 winhttp。
 */
#include <tokenprobe/probe.h>

#include <string.h>

int main(void) {
    tokenprobe_decimal_t amount;
    char text[40];
    tokenprobe_result_t *result;
    size_t count;
    size_t i;
    int saw_deepseek = 0;

    /* 版本来自生成的头部，而不是这里的字面量。 */
    if (TOKENPROBE_VERSION_MAJOR != 0 || TOKENPROBE_VERSION_STRING[0] == '\0') {
        return 1;
    }

    /* 精确十进制属于公共契约的一部分。 */
    if (tokenprobe_decimal_parse("110.00", 6, &amount) != TOKENPROBE_OK) {
        return 2;
    }
    if (amount.unscaled != 11000 || amount.scale != 2) {
        return 3;
    }
    if (tokenprobe_decimal_format(amount, text, sizeof(text)) != TOKENPROBE_OK) {
        return 4;
    }
    if (strcmp(text, "110.00") != 0) {
        return 5;
    }

    /* 无需构造上下文即可访问供应商注册表，
     * 因此宿主可以在完全离线的情况下渲染供应商选择器。 */
    count = tokenprobe_provider_count();
    if (count == 0) {
        return 6;
    }
    for (i = 0; i < count; i++) {
        const tokenprobe_provider_t *provider = tokenprobe_provider_at(i);
        if (provider == NULL || provider->id == NULL) {
            return 7;
        }
        if (strcmp(provider->id, "deepseek") == 0) {
            saw_deepseek = 1;
        }
    }
    if (!saw_deepseek) {
        return 8;
    }
    if (tokenprobe_provider_find("DEEPSEEK") == NULL) {
        return 9;
    }

    /* 结果是堆对象，生命周期有明确文档。 */
    result = tokenprobe_result_create();
    if (result == NULL) {
        return 10;
    }
    if (tokenprobe_result_metric_count(result) != 0) {
        return 11;
    }
    tokenprobe_result_free(result);

    return 0;
}
