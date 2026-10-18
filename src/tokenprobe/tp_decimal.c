/*
 * 精确定点数。
 *
 * 供应商以十进制字符串返回余额（如 "110.00"）。用 double 解析再格式化可能
 * 改变最后一位，而余额差 0.01 就是缺陷。这里全部采用整数运算，
 * 值为 unscaled / 10^scale。
 *
 * 表示规则：
 *
 *   1. scale 始终在 [0, TOKENPROBE_DECIMAL_MAX_SCALE] 内，值为
 *      unscaled / 10^scale。零的 scale 固定为 0，因此 is_zero() 和
 *      compare() 的行为与“没有余额”的语义一致。
 *
 *   2. 只在解析时舍入一次，最多保留 TOKENPROBE_DECIMAL_MAX_SCALE 位小数，
 *      采用四舍五入（远离零）。库内其他位置不再舍入，因此格式化结果与解析
 *      结果完全一致；比较时把较小的 scale 放大，该过程是精确的。
 *
 *   3. 累加器使用无符号幅值，因此舍入路径不需要处理符号。符号只在最后应用，
 *      只有 |INT64_MIN| 这一个幅值允许超过 INT64_MAX，从而让整个 int64 范围
 *      都能往返转换。
 *
 *   4. 如果输入精度超出累加器的可表示范围，直接拒绝而不是截断。悄悄丢掉
 *      有效数字会让余额显示在无人察觉的情况下出错，这比返回一个可见的错误更糟。
 */
#include "tp_internal.h"

#include <string.h>

/* 累加器保存幅值，因此上限是 |INT64_MIN|。 */
#define TP_ACC_MAG_LIMIT ((uint64_t)INT64_MAX + 1u)

/* 累加器可表示的十进制位数；int64 最大值为 19 位。 */
#define TP_ACC_DIGITS 19

/* 限制指数上限，避免解析被恶意构造的巨量计算拖垮。
 * 真实余额远达不到这个量级。 */
#define TP_MAX_EXPONENT 40

static uint64_t pow10_u64(int n) {
    static const uint64_t table[20] = {1ull,
                                       10ull,
                                       100ull,
                                       1000ull,
                                       10000ull,
                                       100000ull,
                                       1000000ull,
                                       10000000ull,
                                       100000000ull,
                                       1000000000ull,
                                       10000000000ull,
                                       100000000000ull,
                                       1000000000000ull,
                                       10000000000000ull,
                                       100000000000000ull,
                                       1000000000000000ull,
                                       10000000000000000ull,
                                       100000000000000000ull,
                                       1000000000000000000ull,
                                       10000000000000000000ull};
    return (n >= 0 && n < 20) ? table[n] : 0;
}

/* 放大一个幅值，超出可表示范围时返回错误。 */
static int scale_up_mag(uint64_t *mag, int by) {
    uint64_t factor;

    if (by <= 0 || *mag == 0) {
        return 0;
    }
    factor = pow10_u64(by);
    if (factor == 0 || *mag > TP_ACC_MAG_LIMIT / factor) {
        return -1;
    }
    *mag *= factor;
    return 0;
}

/* 在幅值尺度上放大有符号值，溢出时返回错误。 */
static int scale_up_signed(int64_t *value, int by) {
    uint64_t mag;

    if (by <= 0 || *value == 0) {
        return 0;
    }
    mag = *value < 0 ? (uint64_t)(-(*value + 1)) + 1u : (uint64_t)*value;
    if (scale_up_mag(&mag, by) != 0) {
        return -1;
    }
    if (*value < 0) {
        *value = (mag == TP_ACC_MAG_LIMIT) ? INT64_MIN : -(int64_t)mag;
    } else {
        *value = (int64_t)mag;
    }
    return 0;
}

/*
 * 将幅值除以 10^drop，并按四舍五入（远离零）处理。
 *
 * 比较式必须写成 `2 * remainder >= divisor`，不能写成
 * `remainder >= divisor / 2`：奇数除数被整除会截断，把“向上舍入”变成“向下舍入”。
 *
 * 四舍五入（远离零）意味着正好一半时总是进位，因此只需一次 `>=` 判断。
 * 这里不需要额外的“有数字被丢弃”标志：被丢弃的数字小于保留末位的 1 个单位，
 * 不会让结果越过保留位已经确定的半值边界。
 */
static void scale_down_mag(uint64_t *mag, int drop) {
    uint64_t divisor;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t twice;

    if (drop <= 0 || *mag == 0) {
        return;
    }
    if (drop >= 20) {
        /* 所有有效数字都在小数点后 19 位以外，远细于本程序输出的货币字段。 */
        *mag = 0;
        return;
    }

    divisor = pow10_u64(drop);
    quotient = *mag / divisor;
    remainder = *mag % divisor;
    twice = remainder * 2;

    if (twice >= divisor) {
        quotient += 1;
    }
    *mag = quotient;
}

/*
 * 接受可选符号、整数部分、可选小数和可选指数。指数路径让 1.5e3 这类 JSON
 * 数字可以直接解析，无需经过有精度损失的 double。
 *
 * 必须消费所有字节。尾部多余内容按错误处理，而不是做前缀匹配：供应商改变
 * 字段格式时应当报错，而不是得到一个看起来合理、实际错误的数字。
 */
static tokenprobe_status_t parse_digits(const char *text, size_t len, tokenprobe_decimal_t *out) {
    size_t i;
    int neg = 0;
    int in_frac = 0;
    int seen_digit = 0;
    int digits_kept = 0;
    int frac_kept = 0;
    int frac_len = 0;
    int frac_dropped = 0;
    long exponent = 0;
    uint64_t mag = 0;

    i = 0;
    if (text[i] == '+' || text[i] == '-') {
        neg = text[i] == '-';
        i++;
    }

    for (; i < len; i++) {
        char c = text[i];

        if (c >= '0' && c <= '9') {
            unsigned digit = (unsigned)(c - '0');

            seen_digit = 1;
            if (in_frac) {
                frac_len++;
            }

            /*
             * 整数部分的前导零没有意义，不能占用累加器的位数预算：
             * "000000000000000000001.5" 是合法输入。
             */
            if (!in_frac && digits_kept == 0 && digit == 0) {
                continue;
            }
            if (digits_kept >= TP_ACC_DIGITS || mag > (TP_ACC_MAG_LIMIT - digit) / 10u) {
                if (!in_frac) {
                    return TOKENPROBE_ERR_PARSE; /* integer magnitude exceeds int64 */
                }
                /* 后续小数位只会让数值更小，不进累加器，但也不能丢：
                 * 位数决定 scale，是否非零决定舍入。 */
                frac_dropped++;
                continue;
            }
            mag = mag * 10u + digit;
            digits_kept++;
            if (in_frac) {
                frac_kept++;
            }
            continue;
        }

        if (c == '.' && !in_frac) {
            if (!seen_digit) {
                return TOKENPROBE_ERR_PARSE;
            }
            in_frac = 1;
            continue;
        }
        break;
    }

    if (!seen_digit) {
        return TOKENPROBE_ERR_PARSE;
    }
    /* "12." 不是合法数字：JSON 禁止这种写法，供应商返回它说明格式已变。 */
    if (in_frac && frac_len == 0) {
        return TOKENPROBE_ERR_PARSE;
    }

    if (i < len && (text[i] == 'e' || text[i] == 'E')) {
        size_t exp_start;
        size_t exp_digits_start;
        long exp_mag = 0;
        size_t k;

        i++;
        exp_start = i;
        if (i < len && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        exp_digits_start = i;
        for (k = i; k < len && text[k] >= '0' && text[k] <= '9'; k++) {
            if (exp_mag <= TP_MAX_EXPONENT) {
                exp_mag = exp_mag * 10 + (text[k] - '0');
            }
        }
        if (k == exp_digits_start) {
            return TOKENPROBE_ERR_PARSE;
        }
        if (exp_mag > TP_MAX_EXPONENT) {
            return TOKENPROBE_ERR_PARSE;
        }
        i = k;
        exponent = (text[exp_start] == '-') ? -exp_mag : exp_mag;
    }

    if (i != len) {
        return TOKENPROBE_ERR_PARSE; /* trailing garbage */
    }

    if (mag == 0) {
        out->unscaled = 0;
        out->scale = 0;
        return TOKENPROBE_OK;
    }

    /*
     * E = frac_kept - exponent 精确描述了缩放后的值。
     *
     * 被丢弃的小数位不计入 E，因为它们不在幅值中：它们严格位于末位之下，
     * 只影响舍入。比较 2*remainder 与除数已经覆盖了“四舍五入 + 被丢弃位
     * 非零则超过半值”的规则，因此这里不需要 sticky 标志。
     */
    {
        long e = (long)frac_kept - exponent;

        if (e < -TP_MAX_EXPONENT || e > TP_MAX_EXPONENT + TP_ACC_DIGITS) {
            return TOKENPROBE_ERR_PARSE;
        }

        if (e < 0) {
            if (frac_dropped > 0) {
                /* 有效数字跨越了类型无法表示的幅值；截断会得到错误的数字。 */
                return TOKENPROBE_ERR_PARSE;
            }
            if (scale_up_mag(&mag, (int)(-e)) != 0) {
                return TOKENPROBE_ERR_PARSE;
            }
            e = 0;
        }

        if (e > TOKENPROBE_DECIMAL_MAX_SCALE) {
            scale_down_mag(&mag, (int)(e - TOKENPROBE_DECIMAL_MAX_SCALE));
            e = TOKENPROBE_DECIMAL_MAX_SCALE;
        }

        if (mag == 0) {
            out->unscaled = 0;
            out->scale = 0;
            return TOKENPROBE_OK;
        }

        if (mag > (uint64_t)INT64_MAX) {
            /* 只有恰好 |INT64_MIN| 可表示，且只能作为负数。 */
            if (!(neg && mag == TP_ACC_MAG_LIMIT)) {
                return TOKENPROBE_ERR_PARSE;
            }
            out->unscaled = INT64_MIN;
            out->scale = (int)e;
            return TOKENPROBE_OK;
        }

        out->unscaled = neg ? -(int64_t)mag : (int64_t)mag;
        out->scale = (int)e;
    }
    return TOKENPROBE_OK;
}

tokenprobe_status_t tokenprobe_decimal_parse(const char *text, size_t len,
                                             tokenprobe_decimal_t *out) {
    tokenprobe_status_t status;
    size_t start;

    if (!out) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    out->unscaled = 0;
    out->scale = 0;
    if (!text) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    /* 容忍首尾空白：部分供应商会给数字字段补空格。 */
    start = 0;
    while (start < len && (text[start] == ' ' || text[start] == '\t')) {
        start++;
    }
    while (len > start && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    if (start >= len) {
        return TOKENPROBE_ERR_PARSE;
    }

    status = parse_digits(text + start, len - start, out);
    if (status != TOKENPROBE_OK) {
        out->unscaled = 0;
        out->scale = 0;
    }
    return status;
}

tokenprobe_status_t tokenprobe_decimal_format(tokenprobe_decimal_t value, char *dst,
                                              size_t dst_len) {
    char buf[40];
    int n = 0;
    int i;
    int64_t u = value.unscaled;
    int neg = u < 0;
    int scale = value.scale;
    uint64_t mag;

    if (!dst || dst_len < 2) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    if (u == 0 || scale < 0) {
        scale = 0;
    }
    if (scale > TOKENPROBE_DECIMAL_MAX_SCALE) {
        scale = TOKENPROBE_DECIMAL_MAX_SCALE;
    }

    /* 使用幅值运算，因此 INT64_MIN 不需要特判。 */
    mag = neg ? (uint64_t)(-(u + 1)) + 1u : (uint64_t)u;

    {
        char digits[24];
        char frac[TOKENPROBE_DECIMAL_MAX_SCALE];
        int d = 0;

        if (scale > 0) {
            for (i = 0; i < scale; i++) {
                frac[scale - 1 - i] = (char)('0' + (int)(mag % 10u));
                mag /= 10u;
            }
        }
        if (mag == 0) {
            digits[d++] = '0';
        }
        while (mag > 0) {
            digits[d++] = (char)('0' + (int)(mag % 10u));
            mag /= 10u;
        }
        if (neg) {
            buf[n++] = '-';
        }
        for (i = d - 1; i >= 0; i--) {
            buf[n++] = digits[i];
        }
        if (scale > 0) {
            buf[n++] = '.';
            for (i = 0; i < scale; i++) {
                buf[n++] = frac[i];
            }
        }
    }
    buf[n] = '\0';

    if ((size_t)n + 1 > dst_len) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    memcpy(dst, buf, (size_t)n + 1);
    return TOKENPROBE_OK;
}

int tokenprobe_decimal_compare(tokenprobe_decimal_t a, tokenprobe_decimal_t b) {
    tokenprobe_decimal_t left = a;
    tokenprobe_decimal_t right = b;
    int target;

    if (left.unscaled == 0) {
        left.scale = 0;
    }
    if (right.unscaled == 0) {
        right.scale = 0;
    }

    target = left.scale > right.scale ? left.scale : right.scale;
    if (target > TOKENPROBE_DECIMAL_MAX_SCALE) {
        target = TOKENPROBE_DECIMAL_MAX_SCALE;
    }

    /*
     * 放大是精确的：解析时的舍入已把两个 scale 限制在
     * TOKENPROBE_DECIMAL_MAX_SCALE 内，较小的一方只会被放大。无法放大的值
     * 按构造必然幅值更大。
     */
    if (left.scale != target && scale_up_signed(&left.unscaled, target - left.scale) != 0) {
        return a.unscaled < 0 ? -1 : 1;
    }
    if (right.scale != target && scale_up_signed(&right.unscaled, target - right.scale) != 0) {
        return b.unscaled < 0 ? 1 : -1;
    }

    if (left.unscaled < right.unscaled) {
        return -1;
    }
    if (left.unscaled > right.unscaled) {
        return 1;
    }
    return 0;
}

int tokenprobe_decimal_is_zero(tokenprobe_decimal_t value) {
    return value.unscaled == 0;
}
