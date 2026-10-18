#ifndef TOKENPROBE_PROBE_H
#define TOKENPROBE_PROBE_H

/**
 * @file probe.h
 * @brief 与供应商无关的 AI API 额度、支出和配额探测接口。
 *
 * 本头文件即完整的公共 ABI。它不包含任何第三方头文件，因此库可以嵌入
 * C、C++、C#、Rust 或 Python（FFI）宿主，而不会把依赖带给宿主。
 *
 * 三种额度分别建模，因为它们不可互换：
 *
 * - @b credit（余额）——某一时刻的剩余金额，例如 DeepSeek 的
 *   `GET /user/balance`。
 * - @b spend（支出）——某一时段内的消费金额，例如 OpenAI 的组织费用。
 * - @b quota（配额）——剩余限流额度，只能从真实推理请求的响应头中观察。
 *
 * 无法上报某项数据的供应商不会生成对应指标，因此调用方总能区分“不支持”和
 * “零”。tokenprobe_provider_t 中声明的能力可让 UI 提前得知。
 *
 * @par 金额表示
 * 金额使用精确十进制（tokenprobe_decimal_t），绝不用 `double`。供应商以
 * `"110.00"` 这类十进制字符串返回余额，二进制浮点数无法可靠往返。
 *
 * @par 命名
 * 公共符号使用 `tokenprobe_` 前缀。`src/` 下的内部符号使用较短的 `tp_` 前缀，
 * 不属于本 ABI。
 */

#include <stddef.h>
#include <stdint.h>

#include <tokenprobe/export.h>
#include <tokenprobe/version.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 状态 */

/** 所有可能失败接口的返回码。 */
typedef enum tokenprobe_status_t {
    TOKENPROBE_OK = 0, /**< 成功。 */

    /* -1 .. -99 —— 调用方与输入错误。 */
    TOKENPROBE_ERR_INVALID_ARG = -1, /**< 必需指针为 NULL 或超出范围。 */
    TOKENPROBE_ERR_INVALID_KEY = -2, /**< 未提供 API key，且供应商的环境变量未设置。 */
    TOKENPROBE_ERR_INVALID_URL = -3, /**< 无法解析 base URL。 */

    /* -100 .. -199 —— 传输层。 */
    TOKENPROBE_ERR_HTTP = -100,    /**< HTTP 状态异常。 */
    TOKENPROBE_ERR_TLS = -101,     /**< TLS 握手或证书校验失败。 */
    TOKENPROBE_ERR_TIMEOUT = -102, /**< 请求超时。 */
    TOKENPROBE_ERR_DNS = -103,     /**< 无法解析主机名。 */
    TOKENPROBE_ERR_CONNECT = -104, /**< TCP 连接失败。 */

    /* -200 .. -299 —— 响应内容。 */
    TOKENPROBE_ERR_JSON = -200,     /**< 响应体不是合法 JSON。 */
    TOKENPROBE_ERR_PARSE = -201,    /**< 无法解析某个数值或文本字段。 */
    TOKENPROBE_ERR_SCHEMA = -202,   /**< 响应体缺少该供应商要求的字段。 */
    TOKENPROBE_ERR_ENCODING = -203, /**< 不支持的传输编码或内容编码。 */

    /* -300 .. -399 —— 语义。 */
    TOKENPROBE_ERR_AUTH = -300,        /**< API key 被拒绝（401）。 */
    TOKENPROBE_ERR_FORBIDDEN = -301,   /**< API key 缺少所需权限（403）。 */
    TOKENPROBE_ERR_NOT_FOUND = -302,   /**< 该账号不存在此接口（404）。 */
    TOKENPROBE_ERR_RATE_LIMIT = -303,  /**< 供应商正在对该 key 限流（429）。 */
    TOKENPROBE_ERR_SERVER = -304,      /**< 供应商返回 5xx。 */
    TOKENPROBE_ERR_UNSUPPORTED = -305, /**< 该供应商没有公开接口提供这项数据。 */

    /* -900 .. —— 本地错误。 */
    TOKENPROBE_ERR_OOM = -900,     /**< 内存不足。 */
    TOKENPROBE_ERR_IO = -901,      /**< 本地 I/O 操作失败。 */
    TOKENPROBE_ERR_INTERNAL = -999 /**< 内部不变量被破坏。 */
} tokenprobe_status_t;

/**
 * @brief 状态码的稳定大写名称，例如 `"AUTH"`。
 * @param status 要命名的状态。
 * @return 静态字符串；永不为 NULL，调用方无需释放。
 */
TOKENPROBE_API const char *tokenprobe_status_name(tokenprobe_status_t status);

/**
 * @brief 状态码的可读描述。
 * @param status 要描述的状态。
 * @return 静态字符串；永不为 NULL，调用方无需释放。
 */
TOKENPROBE_API const char *tokenprobe_status_message(tokenprobe_status_t status);

/* ------------------------------------------------------------------ 错误 */

/**
 * @brief 调用失败时的诊断信息。
 *
 * 由调用方提供输出参数，不在内部分配：先用 tokenprobe_error_init() 或清零
 * 初始化，再传给任何可能失败的调用。不需要详情时，所有失败接口也都接受 NULL。
 */
typedef struct tokenprobe_error_t {
    tokenprobe_status_t code; /**< 失败码；未使用时为 TOKENPROBE_OK。 */
    int http_status;          /**< HTTP 状态；非 HTTP 层失败时为 0。 */
    char message[320];        /**< 单行摘要，始终以 NUL 结尾。 */
    char hint[256];           /**< 可执行的下一步建议，或为空。 */
} tokenprobe_error_t;

/**
 * @brief 把错误结构重置为“无错误”状态。
 * @param err 要重置的错误。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_error_init(tokenprobe_error_t *err);

/**
 * @brief 把错误渲染为一行文本，适合日志或 UI 显示。
 * @param dst 目标缓冲区，不能为 NULL。
 * @param dst_len @p dst 的字节容量；大于 0 时保证以 NUL 结尾。
 * @param err 要渲染的错误。可为 NULL，此时输出空字符串。
 */
TOKENPROBE_API void tokenprobe_error_format(char *dst, size_t dst_len,
                                            const tokenprobe_error_t *err);

/* ----------------------------------------------------------------- 十进制 */

/** tokenprobe_decimal_t 保留的最大小数位数。 */
#define TOKENPROBE_DECIMAL_MAX_SCALE 9

/**
 * @brief 精确十进制，值为 `unscaled / 10^scale`。
 *
 * 金额绝不用二进制浮点类型保存。用 `double` 解析 `"110.00"` 再格式化可能
 * 改变最后一位，而余额显示差 0.01 就是缺陷。
 */
typedef struct tokenprobe_decimal_t {
    int64_t unscaled; /**< 小数点移位前的有符号值。 */
    int scale;        /**< 小数位数，限制在 [0, TOKENPROBE_DECIMAL_MAX_SCALE]。 */
} tokenprobe_decimal_t;

/**
 * @brief 精确解析十进制字符串或 JSON 数字字面量。
 *
 * 接受可选符号、整数部分、可选小数和可选指数。超过
 * TOKENPROBE_DECIMAL_MAX_SCALE 的位数按四舍五入（远离零）处理。
 *
 * @param text 要解析的文本；不要求以 NUL 结尾。
 * @param len @p text 的字节数。
 * @param[out] out 接收解析结果，不能为 NULL；失败时清零。
 * @return TOKENPROBE_OK；当 @p text 不是完整十进制数或幅值超出 int64 时返回
 *         TOKENPROBE_ERR_PARSE。
 */
TOKENPROBE_API tokenprobe_status_t tokenprobe_decimal_parse(const char *text, size_t len,
                                                            tokenprobe_decimal_t *out);

/**
 * @brief 把十进制值格式化为规范文本，例如 `"-12.34"`。
 * @param value 要格式化的值。
 * @param dst 目标缓冲区，不能为 NULL。
 * @param dst_len @p dst 的容量；40 字节总是足够。
 * @return TOKENPROBE_OK；缓冲区太小时返回 TOKENPROBE_ERR_INVALID_ARG。
 */
TOKENPROBE_API tokenprobe_status_t tokenprobe_decimal_format(tokenprobe_decimal_t value, char *dst,
                                                             size_t dst_len);

/**
 * @brief 按数值比较两个十进制数，忽略存储的 scale。
 * @param a 左操作数。
 * @param b 右操作数。
 * @return a < b 返回 -1，相等返回 0，a > b 返回 1。
 */
TOKENPROBE_API int tokenprobe_decimal_compare(tokenprobe_decimal_t a, tokenprobe_decimal_t b);

/**
 * @brief 判断十进制值是否恰好为零。
 * @param value 要判断的值。
 * @return 为零返回 1，否则返回 0。
 */
TOKENPROBE_API int tokenprobe_decimal_is_zero(tokenprobe_decimal_t value);

/* ------------------------------------------------------------------ 指标 */

/** 指标对应的三种数据之一。 */
typedef enum tokenprobe_metric_kind_t {
    TOKENPROBE_METRIC_CREDIT = 0, /**< 某一时刻的剩余余额。 */
    TOKENPROBE_METRIC_SPEND = 1,  /**< 某一时段内的消费金额。 */
    TOKENPROBE_METRIC_QUOTA = 2   /**< 剩余限流额度。 */
} tokenprobe_metric_kind_t;

/**
 * @brief 一项上报的数据。
 *
 * 所有字符串成员都由所属的 tokenprobe_result_t 持有，在调用
 * tokenprobe_result_free() 之前保持有效。不要单独释放它们。
 */
typedef struct tokenprobe_metric_t {
    tokenprobe_metric_kind_t kind; /**< 数据种类。 */
    const char *id;              /**< 稳定键名，例如 `"total_balance"`。永不为 NULL。 */
    const char *label;           /**< 显示文本，或 NULL。 */
    const char *currency;        /**< ISO 代码，例如 `"CNY"`；token 类指标为 NULL。 */
    const char *unit;            /**< `"currency"`、`"token"` 或 `"request"`。 */
    tokenprobe_decimal_t amount; /**< 数值。 */
    const char *period_start;    /**< 统计时段的 ISO-8601 起点，或 NULL。 */
    const char *period_end;      /**< 统计时段的 ISO-8601 终点，或 NULL。 */
} tokenprobe_metric_t;

/* ------------------------------------------------------------------ 结果 */

/**
 * @brief 不透明的堆分配探测结果。
 *
 * 用 tokenprobe_result_create() 创建，用 tokenprobe_result_free() 释放。
 * 访问器返回的每个字符串都由结果持有，因此调用方可以跨帧保留结果，
 * 而不必固定解析器或传输层状态。
 */
typedef struct tokenprobe_result_t tokenprobe_result_t;

/**
 * @brief 分配一个空结果。
 * @return 新结果；分配失败返回 NULL。
 */
TOKENPROBE_API tokenprobe_result_t *tokenprobe_result_create(void);

/**
 * @brief 释放结果及其持有的所有字符串。
 * @param result 要释放的结果。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_result_free(tokenprobe_result_t *result);

/**
 * @brief 生成本结果的供应商 id。
 * @param result 要读取的结果。
 * @return 由 @p result 持有的借用字符串，或 NULL。
 */
TOKENPROBE_API const char *tokenprobe_result_provider(const tokenprobe_result_t *result);

/**
 * @brief 账号或组织标识，供应商上报时才有。
 * @param result 要读取的结果。
 * @return 由 @p result 持有的借用字符串，或 NULL。
 */
TOKENPROBE_API const char *tokenprobe_result_account(const tokenprobe_result_t *result);

/**
 * @brief 供应商提供的注意事项，适合在 UI 中显示。
 * @param result 要读取的结果。
 * @return 由 @p result 持有的借用字符串，或 NULL。
 */
TOKENPROBE_API const char *tokenprobe_result_note(const tokenprobe_result_t *result);

/**
 * @brief 截断后的原始响应体，用于诊断。
 * @param result 要读取的结果。
 * @return 由 @p result 持有的借用字符串，或 NULL。
 */
TOKENPROBE_API const char *tokenprobe_result_raw_body(const tokenprobe_result_t *result);

/**
 * @brief 底层请求的 HTTP 状态。
 * @param result 要读取的结果。
 * @return HTTP 状态；未发起请求时为 0。
 */
TOKENPROBE_API long tokenprobe_result_http_status(const tokenprobe_result_t *result);

/**
 * @brief 请求的实际耗时。
 * @param result 要读取的结果。
 * @return 耗时，单位毫秒。
 */
TOKENPROBE_API int64_t tokenprobe_result_latency_ms(const tokenprobe_result_t *result);

/**
 * @brief 成功探测完成时的观测时间。
 * @param result 要读取的结果。
 * @return Unix 纪元起的毫秒数；未完成探测或 @p result 为 NULL 时返回 0。
 */
TOKENPROBE_API int64_t tokenprobe_result_observed_at_ms(const tokenprobe_result_t *result);

/**
 * @brief 供应商是否报告该账号可用。
 * @param result 要读取的结果。
 * @return 可用返回 1，不可用返回 0，供应商未上报返回 -1。
 */
TOKENPROBE_API int tokenprobe_result_available(const tokenprobe_result_t *result);

/**
 * @brief 结果中的指标数量。
 * @param result 要读取的结果。
 * @return 指标数量；@p result 为 NULL 时返回 0。
 */
TOKENPROBE_API int tokenprobe_result_metric_count(const tokenprobe_result_t *result);

/**
 * @brief 按索引访问指标。
 * @param result 要读取的结果。
 * @param index 从 0 开始的索引。
 * @return 由 @p result 持有的借用指针；越界时返回 NULL。
 */
TOKENPROBE_API const tokenprobe_metric_t *
tokenprobe_result_metric(const tokenprobe_result_t *result, int index);

/**
 * @brief 设置供应商 id，字符串会被复制到结果中。
 * @param result 要修改的结果。可为 NULL，此时不做任何事。
 * @param provider_id 要复制的值；传 NULL 表示清空。
 */
TOKENPROBE_API void tokenprobe_result_set_provider(tokenprobe_result_t *result,
                                                   const char *provider_id);

/**
 * @brief 设置账号标识，字符串会被复制到结果中。
 * @param result 要修改的结果。可为 NULL。
 * @param account 要复制的值；传 NULL 表示清空。
 */
TOKENPROBE_API void tokenprobe_result_set_account(tokenprobe_result_t *result, const char *account);

/**
 * @brief 设置显示用备注，字符串会被复制到结果中。
 * @param result 要修改的结果。可为 NULL。
 * @param note 要复制的值；传 NULL 表示清空。
 */
TOKENPROBE_API void tokenprobe_result_set_note(tokenprobe_result_t *result, const char *note);

/**
 * @brief 设置诊断用原始响应体，字符串会被复制到结果中。
 * @param result 要修改的结果。可为 NULL。
 * @param body 要复制的值；传 NULL 表示清空。
 */
TOKENPROBE_API void tokenprobe_result_set_raw_body(tokenprobe_result_t *result, const char *body);

/**
 * @brief 记录供应商是否报告该账号可用。
 * @param result 要修改的结果。可为 NULL。
 * @param available 非零表示可用，零表示不可用。
 */
TOKENPROBE_API void tokenprobe_result_set_available(tokenprobe_result_t *result, int available);

/**
 * @brief 记录底层请求的状态和耗时。
 * @param result 要修改的结果。可为 NULL。
 * @param http_status HTTP 状态码。
 * @param latency_ms 耗时，单位毫秒。
 */
TOKENPROBE_API void tokenprobe_result_set_timing(tokenprobe_result_t *result, long http_status,
                                                 int64_t latency_ms);

/**
 * @brief 追加一个指标，并深拷贝 @p metric 中的所有字符串。
 * @param result 要修改的结果。不能为 NULL。
 * @param metric 要复制的指标；@c id 不能为 NULL。
 * @return TOKENPROBE_OK、TOKENPROBE_ERR_INVALID_ARG 或 TOKENPROBE_ERR_OOM。
 */
TOKENPROBE_API tokenprobe_status_t tokenprobe_result_add_metric(tokenprobe_result_t *result,
                                                                const tokenprobe_metric_t *metric);

/* -------------------------------------------------------------- 传输层 */

/** 一个请求头或响应头。两个字符串都由其容器持有。 */
typedef struct tokenprobe_http_header_t {
    const char *name;  /**< 头名称。 */
    const char *value; /**< 头值。 */
} tokenprobe_http_header_t;

/** 交给传输层执行的 HTTP 请求。 */
typedef struct tokenprobe_http_request_t {
    const char *method;                      /**< `"GET"`、`"POST"` 等。永不为 NULL。 */
    const char *url;                         /**< 绝对 URL。永不为 NULL。 */
    const tokenprobe_http_header_t *headers; /**< 附加请求头，或 NULL。 */
    size_t header_count;                     /**< @c headers 的元素个数。 */
    const char *body;                        /**< 请求体，或 NULL。 */
    size_t body_len;                         /**< @c body 的字节数。 */
    int timeout_ms;                          /**< 请求超时，单位毫秒。 */
} tokenprobe_http_request_t;

/**
 * @brief 传输层生成的 HTTP 响应。
 *
 * @c body 和头部字符串由传输层持有。自定义传输层必须用 malloc/realloc 分配，
 * 以便可以用 free() 释放；可使用 tokenprobe_strdup()。
 */
typedef struct tokenprobe_http_response_t {
    long status;                       /**< HTTP 状态；未收到响应时为 0。 */
    char *body;                        /**< malloc 分配的响应体，或 NULL。 */
    size_t body_len;                   /**< @c body 的字节数。 */
    tokenprobe_http_header_t *headers; /**< malloc 分配的响应头数组，或 NULL。 */
    size_t header_count;               /**< @c headers 的元素个数。 */
} tokenprobe_http_response_t;

/**
 * @brief 交给传输层之前把响应清零。
 * @param response 要初始化的响应。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_http_response_init(tokenprobe_http_response_t *response);

/**
 * @brief 释放响应持有的内存。
 * @param response 要释放的响应。可为 NULL；重复调用是安全的。
 */
TOKENPROBE_API void tokenprobe_http_response_free(tokenprobe_http_response_t *response);

/**
 * @brief 按名称查找响应头，大小写不敏感。
 * @param response 要搜索的响应。
 * @param name 要查找的头名称。
 * @return 由 @p response 持有的借用指针；不存在时返回 NULL。
 */
TOKENPROBE_API const char *tokenprobe_http_header_get(const tokenprobe_http_response_t *response,
                                                      const char *name);

/**
 * @brief 用 malloc 复制一个以 NUL 结尾的字符串。
 * @param text 要复制的字符串。传 NULL 返回 NULL。
 * @return malloc 分配的副本；分配失败返回 NULL。
 */
TOKENPROBE_API char *tokenprobe_strdup(const char *text);

/**
 * @brief 用 malloc 复制字符串的最多 @p len 个字节。
 * @param text 要复制的字符串。传 NULL 返回 NULL。
 * @param len 最多复制的字节数。
 * @return malloc 分配且以 NUL 结尾的副本；分配失败返回 NULL。
 */
TOKENPROBE_API char *tokenprobe_strndup(const char *text, size_t len);

/**
 * @brief 传输层请求函数的签名。
 *
 * 单独命名而不是内联书写，便于第三方实现传输层时无需重复签名，也让
 * `scripts/check_decl_def.py` 中的架构门禁能把它识别为类型，而不会误读为
 * 函数声明。
 *
 * @param impl tokenprobe_transport_t::impl 的值。
 * @param request 要执行的请求。永不为 NULL。
 * @param[out] response 接收响应，传入前已初始化。
 * @param[out] err 接收失败详情。可为 NULL。
 * @return 成功返回 TOKENPROBE_OK；其他值原样传给调用方。
 */
typedef tokenprobe_status_t (*tokenprobe_transport_perform_fn)(
    void *impl, const tokenprobe_http_request_t *request, tokenprobe_http_response_t *response,
    tokenprobe_error_t *err);

/** 传输层清理函数的签名。 */
typedef void (*tokenprobe_transport_destroy_fn)(void *impl);

/**
 * @brief 让核心库可测试、可嵌入的接缝。
 *
 * 替换传输层即可让整个链路——URL 构造、认证头、状态处理、JSON 解析、指标
 * 提取——针对预置响应运行，无需网络，也不产生 API 费用。
 */
typedef struct tokenprobe_transport_t {
    const char *name; /**< 短标识，例如 `"winhttp"`。 */

    /** @brief 执行一次请求。 */
    tokenprobe_transport_perform_fn perform;

    /** @brief 释放 @c impl。可为 NULL。 */
    tokenprobe_transport_destroy_fn destroy;

    void *impl; /**< 传回给 @c perform 的实现状态。 */
} tokenprobe_transport_t;

/* ---------------------------------------------------------- 离线传输层 */

/**
 * @brief mock 传输层表中的一条预置响应。
 *
 * tokenprobe_transport_mock_create() 会复制所有字符串，因此该表可以是栈上变量。
 */
typedef struct tokenprobe_mock_entry_t {
    const char *url_prefix;          /**< 最长匹配前缀优先。 */
    long status;                     /**< 返回的 HTTP 状态；0 表示 200。 */
    const char *body;                /**< 响应体，或 NULL。 */
    const char *const *header_lines; /**< `"Name: Value"` 形式的行，或 NULL。 */
    size_t header_count;             /**< @c header_lines 的元素个数。 */
} tokenprobe_mock_entry_t;

/**
 * @brief 根据响应表创建确定性的离线传输层。
 *
 * 不访问网络，也不产生费用。本项目测试使用它，也供希望用录制数据渲染 UI 的
 * 宿主使用。
 *
 * @param entries 响应表。@p entry_count 为 0 时可为 NULL。
 * @param entry_count @p entries 的元素个数。
 * @param[out] out_transport 接收传输层，不能为 NULL。
 * @return 成功返回 0，分配失败返回 -1。
 */
TOKENPROBE_API int tokenprobe_transport_mock_create(const tokenprobe_mock_entry_t *entries,
                                                    size_t entry_count,
                                                    tokenprobe_transport_t *out_transport);

/**
 * @brief 释放由 tokenprobe_transport_mock_create() 创建的传输层。
 * @param transport 要释放的传输层。可为 NULL；释放后会被清零。
 */
TOKENPROBE_API void tokenprobe_transport_mock_free(tokenprobe_transport_t *transport);

/**
 * @brief mock 传输层收到的请求数量。
 * @param transport 要查询的传输层。
 * @return 请求数量；@p transport 为 NULL 时返回 0。
 */
TOKENPROBE_API size_t tokenprobe_mock_request_count(const tokenprobe_transport_t *transport);

/**
 * @brief mock 传输层收到的第 N 个请求的 URL。
 * @param transport 要查询的传输层。
 * @param index 从 0 开始的请求索引。
 * @return 由 @p transport 持有的借用字符串；越界时返回 NULL。
 */
TOKENPROBE_API const char *tokenprobe_mock_request_url(const tokenprobe_transport_t *transport,
                                                       size_t index);

/**
 * @brief mock 传输层收到的第 N 个请求中指定头的值。
 * @param transport 要查询的传输层。
 * @param index 从 0 开始的请求索引。
 * @param name 头名称，大小写不敏感。
 * @return 由 @p transport 持有的借用字符串；不存在时返回 NULL。
 */
TOKENPROBE_API const char *tokenprobe_mock_request_header(const tokenprobe_transport_t *transport,
                                                          size_t index, const char *name);

/* ----------------------------------------------------------------- 上下文 */

/** 传给 tokenprobe_log_fn 的严重级别。 */
typedef enum tokenprobe_log_level_t {
    TOKENPROBE_LOG_ERROR = 0, /**< 调用失败。 */
    TOKENPROBE_LOG_WARN = 1,  /**< 出现可恢复的问题。 */
    TOKENPROBE_LOG_INFO = 2,  /**< 进度信息。 */
    TOKENPROBE_LOG_DEBUG = 3  /**< 单请求细节；可能包含 URL。 */
} tokenprobe_log_level_t;

/**
 * @brief 由宿主提供的日志接收函数。
 * @param user tokenprobe_context_options_t::log_user 的值。
 * @param level 消息严重级别。
 * @param message 以 NUL 结尾，仅在本次调用期间有效。
 */
typedef void (*tokenprobe_log_fn)(void *user, tokenprobe_log_level_t level, const char *message);

/** 默认单请求超时，单位毫秒。 */
#define TOKENPROBE_DEFAULT_TIMEOUT_MS 15000

/**
 * @brief 上下文的构造选项。
 *
 * 设置字段前务必先用 tokenprobe_context_options_init() 初始化；结构体带有
 * @c struct_size，因此后续增加字段不会破坏既有调用方。
 */
typedef struct tokenprobe_context_options_t {
    size_t struct_size;                      /**< 由 tokenprobe_context_options_init() 设置。 */
    const tokenprobe_transport_t *transport; /**< 传 NULL 表示使用平台传输层。 */
    int timeout_ms;                          /**< <= 0 时使用 TOKENPROBE_DEFAULT_TIMEOUT_MS。 */
    const char *user_agent;                  /**< 传 NULL 时使用 `tokenprobe/<version>`。 */
    tokenprobe_log_fn log_fn;                /**< 可选日志接收函数，或 NULL。 */
    void *log_user;                          /**< 原样传回给 @c log_fn。 */
} tokenprobe_context_options_t;

/**
 * @brief 不透明的探测上下文。
 *
 * 持有一个传输层、超时时间、User-Agent 和日志接收函数。库内没有全局可变状态，
 * 因此每个线程一个上下文是安全的，也是并发探测的推荐方式。
 */
typedef struct tokenprobe_context_t tokenprobe_context_t;

/**
 * @brief 把选项初始化为文档所述的默认值。
 * @param options 要初始化的选项。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_context_options_init(tokenprobe_context_options_t *options);

/**
 * @brief 创建上下文。
 * @param options 选项；传 NULL 表示全部使用默认值。
 * @return 新上下文；没有可用传输层或分配失败时返回 NULL。
 */
TOKENPROBE_API tokenprobe_context_t *
tokenprobe_context_create(const tokenprobe_context_options_t *options);

/**
 * @brief 销毁上下文。
 *
 * 调用方提供的传输层不受影响；平台传输层是进程级共享实例，同样不会在此销毁。
 *
 * @param context 要销毁的上下文。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_context_destroy(tokenprobe_context_t *context);

/**
 * @brief 向上下文的日志接收函数发送一条格式化消息。
 * @param context 提供日志接收函数的上下文。可为 NULL，此时直接丢弃。
 * @param level 严重级别。
 * @param fmt printf 风格格式串。
 */
TOKENPROBE_API void tokenprobe_context_log(tokenprobe_context_t *context,
                                           tokenprobe_log_level_t level, const char *fmt, ...);

/**
 * @brief 上下文将使用的传输层。
 * @param context 要检查的上下文。
 * @return 由 @p context 持有的借用指针；@p context 为 NULL 时返回 NULL。
 */
TOKENPROBE_API const tokenprobe_transport_t *
tokenprobe_context_transport(const tokenprobe_context_t *context);

/**
 * @brief 平台默认传输层，例如 `"winhttp"` 或 `"openssl"`。
 * @return 进程级的借用指针；本构建未编译平台传输层时返回 NULL。
 *         调用方不得销毁返回的传输层。
 */
TOKENPROBE_API const tokenprobe_transport_t *tokenprobe_transport_platform(void);

/* ---------------------------------------------------------------- 供应商 */

/**
 * @brief 单次探测的可选参数。
 *
 * 先用 tokenprobe_probe_options_init() 初始化；结构体带有 @c struct_size，
 * 便于后续增加字段。字段留空时使用供应商默认行为。
 */
typedef struct tokenprobe_probe_options_t {
    size_t struct_size;                      /**< 由 tokenprobe_probe_options_init() 设置。 */
    const char *period_start;                /**< 统计时段起点（ISO-8601），或 NULL。 */
    const char *period_end;                  /**< 统计时段终点（ISO-8601），或 NULL。 */
    const tokenprobe_http_header_t *headers; /**< 附加请求头，或 NULL。 */
    size_t header_count;                     /**< @c headers 的元素个数。 */
    const char *method;                      /**< HTTP 方法；NULL 表示 GET。 */
    const char *body;                        /**< 请求体，或 NULL。 */
    size_t body_len;                         /**< @c body 的字节数。 */
} tokenprobe_probe_options_t;

/** 供应商探测的入参。 */
typedef struct tokenprobe_provider_request_t {
    const char *api_key;      /**< 要出示的凭据。探测时永不为 NULL。 */
    const char *base_url;     /**< 覆盖 base URL；传 NULL 使用供应商默认值。 */
    const char *period_start; /**< 统计时段起点，或 NULL。 */
    const char *period_end;   /**< 统计时段终点，或 NULL。 */
    const tokenprobe_http_header_t *headers; /**< 附加请求头，或 NULL。 */
    size_t header_count;                     /**< @c headers 的元素个数。 */
    const char *method;                      /**< HTTP 方法，或 NULL。 */
    const char *body;                        /**< 请求体，或 NULL。 */
    size_t body_len;                         /**< @c body 的字节数。 */
} tokenprobe_provider_request_t;

/** @brief 受支持的供应商。完整定义见下文。 */
typedef struct tokenprobe_provider_t tokenprobe_provider_t;

/**
 * @brief 供应商驱动探测函数的签名。
 *
 * 单独命名而不是内联书写，便于第三方新增供应商时无需重复签名，也让
 * `scripts/check_decl_def.py` 中的架构门禁能识别为类型，而不会误读为函数声明。
 *
 * @param self 要探测的供应商。
 * @param context 用于探测的上下文。
 * @param request 凭据和接口覆盖。
 * @param[out] out 要填充的结果。永不为 NULL。
 * @param[out] err 接收失败详情。可为 NULL。
 * @return TOKENPROBE_OK 或失败码。
 */
typedef tokenprobe_status_t (*tokenprobe_provider_probe_fn)(
    const tokenprobe_provider_t *self, tokenprobe_context_t *context,
    const tokenprobe_provider_request_t *request, tokenprobe_result_t *out,
    tokenprobe_error_t *err);

/**
 * @brief 受支持的供应商及其能力。
 *
 * 能力提前声明，调用方无需把“没有指标”解释为零值。
 */
struct tokenprobe_provider_t {
    const char *id;           /**< 稳定 id，例如 `"deepseek"`。 */
    const char *display_name; /**< 显示名称，例如 `"DeepSeek"`。 */
    const char *base_url;     /**< 默认 API 根地址。 */
    const char *env_var;      /**< 保存 API key 的环境变量。 */
    const char *doc_url;      /**< 本驱动所用接口的文档地址。 */
    int supports_credit;      /**< 非零表示可查询剩余余额。 */
    int supports_spend;       /**< 非零表示可查询时段支出。 */
    int supports_quota;       /**< 非零表示可查询限流额度。 */
    const char *notes;        /**< 适合在 UI 中提示的注意事项，或 NULL。 */

    /** @brief 驱动入口点。由 tokenprobe_probe()/tokenprobe_probe_ex() 调用，调用方不直接调用。 */
    tokenprobe_provider_probe_fn probe;
};

/**
 * @brief 编译进本库的供应商数量。
 * @return 供应商数量；正常构建下不为零。
 */
TOKENPROBE_API size_t tokenprobe_provider_count(void);

/**
 * @brief 按索引访问供应商。
 * @param index 从 0 开始的索引。
 * @return 静态借用指针；越界时返回 NULL。
 */
TOKENPROBE_API const tokenprobe_provider_t *tokenprobe_provider_at(size_t index);

/**
 * @brief 按 id、显示名或别名查找供应商，大小写不敏感。
 * @param id 要查找的标识。
 * @return 静态借用指针；未知时返回 NULL。
 */
TOKENPROBE_API const tokenprobe_provider_t *tokenprobe_provider_find(const char *id);

/**
 * @brief 把探测选项初始化为默认值。
 * @param options 要初始化的选项。可为 NULL。
 */
TOKENPROBE_API void tokenprobe_probe_options_init(tokenprobe_probe_options_t *options);

/**
 * @brief 探测一个供应商，并传入可选参数。
 *
 * @param provider 要探测的供应商。不能为 NULL。
 * @param context 用于探测的上下文。不能为 NULL。
 * @param api_key API key；传 NULL 则读取供应商的环境变量。
 * @param base_url 覆盖 base URL；传 NULL 使用供应商默认值。
 * @param options 可选参数，或 NULL。传入时必须由
 *                tokenprobe_probe_options_init() 初始化。
 * @param[out] out 成功时接收新分配的结果。不能为 NULL；失败时置为 NULL。
 * @param[out] err 接收失败详情。可为 NULL。
 * @return 成功返回 TOKENPROBE_OK，否则返回失败码。调用方拥有返回的结果，
 *         并通过 tokenprobe_result_free() 释放。
 */
TOKENPROBE_API tokenprobe_status_t tokenprobe_probe_ex(const tokenprobe_provider_t *provider,
                                                       tokenprobe_context_t *context,
                                                       const char *api_key, const char *base_url,
                                                       const tokenprobe_probe_options_t *options,
                                                       tokenprobe_result_t **out,
                                                       tokenprobe_error_t *err);

/**
 * @brief 探测一个供应商（无额外参数）。
 *
 * 等价于以 NULL 选项调用 tokenprobe_probe_ex()。
 *
 * @param provider 要探测的供应商。不能为 NULL。
 * @param context 用于探测的上下文。不能为 NULL。
 * @param api_key API key；传 NULL 则读取供应商的环境变量。
 * @param base_url 覆盖 base URL；传 NULL 使用供应商默认值。
 * @param[out] out 成功时接收新分配的结果。不能为 NULL；失败时置为 NULL。
 * @param[out] err 接收失败详情。可为 NULL。
 * @return 成功返回 TOKENPROBE_OK，否则返回失败码。调用方拥有返回的结果，
 *         并通过 tokenprobe_result_free() 释放。
 */
TOKENPROBE_API tokenprobe_status_t tokenprobe_probe(const tokenprobe_provider_t *provider,
                                                    tokenprobe_context_t *context,
                                                    const char *api_key, const char *base_url,
                                                    tokenprobe_result_t **out,
                                                    tokenprobe_error_t *err);

#ifdef __cplusplus
}
#endif

#endif /* TOKENPROBE_PROBE_H */
