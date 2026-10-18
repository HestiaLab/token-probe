// 下游 C++ 消费方冒烟测试。
//
// 公共头文件必须无需任何包装即可作为 C++ 编译：它带有 `extern "C"` 保护，
// 且暴露的每个类型都必须能在 C++ 中使用。
#include <tokenprobe/probe.h>

#include <cstring>

int main() {
    tokenprobe_decimal_t amount{};
    char text[40]{};

    if (tokenprobe_decimal_parse("7.25", 4, &amount) != TOKENPROBE_OK) {
        return 1;
    }
    if (tokenprobe_decimal_format(amount, text, sizeof(text)) != TOKENPROBE_OK) {
        return 2;
    }
    if (std::strcmp(text, "7.25") != 0) {
        return 3;
    }

    if (tokenprobe_provider_find("deepseek") == nullptr) {
        return 4;
    }

    return 0;
}
