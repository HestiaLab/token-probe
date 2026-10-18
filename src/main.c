/*
 * tokenprobe 命令行工具。
 *
 * CLI 只是 libtokenprobe 的一层薄封装：解析参数、格式化输出、决定退出码。
 * 它做的每件事都能通过公共 C API 完成，桌面或 Web 宿主无需调用外部进程。
 *
 * 退出码：0 成功，1 探测失败，2 用法错误。
 *
 * 机器可读格式为 XML。只有这份对外文档是 XML：库读取的供应商响应是 JSON，
 * 且保持为 JSON（见 tp_json.c）。
 */
#include "tokenprobe/probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef enum output_format {
    OUTPUT_TEXT = 0, /**< Human-readable; the default. */
    OUTPUT_XML,      /**< One versioned XML document. */
    OUTPUT_JSON      /**< One versioned JSON document. */
} output_format;

typedef struct options {
    const char *provider;
    const char *api_key;
    const char *base_url;
    int timeout_ms;
    output_format format;
    int quiet;
    int show_raw;
    int show_headers;
    int verbose;
    int list;
} options;

static void usage(FILE *out) {
    fprintf(out,
            "tokenprobe %s — probe AI provider credit / spend / quota\n"
            "\n"
            "Usage:\n"
            "  tokenprobe --provider <id> [options]\n"
            "  tokenprobe --list\n"
            "\n"
            "Options:\n"
            "  -p, --provider <id>   provider id (see --list)\n"
            "  -k, --api-key <key>   API key; falls back to the provider's env var\n"
            "  -u, --base-url <url>  override the provider base URL\n"
            "  -t, --timeout <ms>    per-request timeout (default %d)\n"
            "      --json            machine-readable JSON output\n"
            "      --xml             machine-readable XML output\n"
            "      --raw             include the raw response body\n"
            "      --headers         include selected response headers\n"
            "  -q, --quiet           only set the exit code\n"
            "  -v, --verbose         log transport activity to stderr\n"
            "  -l, --list            list known providers\n"
            "  -h, --help            this message\n"
            "      --version         print the version\n"
            "\n"
            "Environment:\n"
            "  Each provider reads its own key variable (e.g. DEEPSEEK_API_KEY).\n"
            "  Keys are never written to disk by this program.\n",
            TOKENPROBE_VERSION_STRING, TOKENPROBE_DEFAULT_TIMEOUT_MS);
}

static int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

/* 两种机器可读格式不能同时占用 stdout。重复同一选项无害；
 * 同时请求两种则按用法错误处理，避免脚本察觉不到 last-wins 行为。 */
static int select_format(output_format *current, output_format want) {
    if (*current != OUTPUT_TEXT && *current != want) {
        return -1;
    }
    *current = want;
    return 0;
}

/* 同时支持 "--opt value" 和 "--opt=value" 两种写法。 */
static const char *take_value(int argc, char **argv, int *i, const char *arg,
                              const char *long_name) {
    size_t n = strlen(long_name);
    if (strncmp(arg, long_name, n) == 0 && arg[n] == '=') {
        return arg + n + 1;
    }
    if ((*i + 1) < argc) {
        return argv[++(*i)];
    }
    return NULL;
}

/*
 * XML 转义。
 *
 * 两条规则保证无论供应商返回什么，文档都格式良好：
 *
 *   - XML 元字符使用字符引用转义；
 *   - XML 1.0 无法表示的码点（除 TAB/LF/CR 外的 C0 控制字符）替换为 U+FFFD。
 *     字符引用也无济于事：XML 1.0 即使转义也禁止这些字符，直接复制损坏响应中
 *     的原始控制字节会生成没有解析器能接受的文档。
 *
 * TAB/LF/CR 一律转为字符引用：在属性值中可避免被属性值规范化折叠，
 * 在文本内容中可避免 CR 被规范化为 LF。
 */
static void xml_escape(FILE *out, const char *text, int in_attribute) {
    const unsigned char *s = (const unsigned char *)text;

    for (; *s; s++) {
        switch (*s) {
        case '&':
            fputs("&amp;", out);
            break;
        case '<':
            fputs("&lt;", out);
            break;
        case '>':
            fputs("&gt;", out);
            break;
        case '"':
            fputs(in_attribute ? "&quot;" : "\"", out);
            break;
        case '\'':
            fputs(in_attribute ? "&apos;" : "'", out);
            break;
        case '\t':
            fputs("&#9;", out);
            break;
        case '\n':
            fputs("&#10;", out);
            break;
        case '\r':
            fputs("&#13;", out);
            break;
        default:
            if (*s < 0x20) {
                fputs("\xEF\xBF\xBD", out); /* U+FFFD */
            } else {
                fputc((int)*s, out);
            }
        }
    }
}

/* 写入 ` name="value"`；值不存在时不写任何内容。 */
static void xml_attr(FILE *out, const char *name, const char *value) {
    if (!value) {
        return;
    }
    fprintf(out, " %s=\"", name);
    xml_escape(out, value, 1);
    fputc('"', out);
}

static void xml_attr_int(FILE *out, const char *name, long long value) {
    fprintf(out, " %s=\"%lld\"", name, value);
}

static void xml_attr_bool(FILE *out, const char *name, int value) {
    fprintf(out, " %s=\"%s\"", name, value ? "true" : "false");
}

/*
 * JSON 转义，用于另一种机器可读格式。
 *
 * JSON 没有无法承载的码点：C0 控制字节会转成 \uXXXX 转义并完整往返，
 * 而 XML 1.0 只能将其丢弃。这一差异正是同时提供两种格式的原因。
 */
static void json_escape(FILE *out, const char *text) {
    const unsigned char *s = (const unsigned char *)text;

    for (; *s; s++) {
        switch (*s) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*s < 0x20) {
                fprintf(out, "\\u%04x", (unsigned)*s);
            } else {
                fputc((int)*s, out);
            }
        }
    }
}

/* 写入 `"name":<string>`；值不存在时写入 `"name":null`。 */
static void json_field(FILE *out, const char *name, const char *value) {
    fprintf(out, "\"%s\":", name);
    if (!value) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    json_escape(out, value);
    fputc('"', out);
}

static const char *metric_kind_name(tokenprobe_metric_kind_t k) {
    switch (k) {
    case TOKENPROBE_METRIC_CREDIT:
        return "credit";
    case TOKENPROBE_METRIC_SPEND:
        return "spend";
    case TOKENPROBE_METRIC_QUOTA:
        return "quota";
    default:
        return "unknown";
    }
}

static void enable_utf8_console(void) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

static int cmd_list(void) {
    size_t i;
    printf("%-12s %-22s %-18s %-7s %-7s %-7s %s\n", "ID", "NAME", "KEY ENV VAR", "CREDIT", "SPEND",
           "QUOTA", "NOTES");
    for (i = 0; i < tokenprobe_provider_count(); i++) {
        const tokenprobe_provider_t *p = tokenprobe_provider_at(i);
        printf("%-12s %-22s %-18s %-7s %-7s %-7s %s\n", p->id, p->display_name,
               p->env_var ? p->env_var : "-", p->supports_credit ? "yes" : "no",
               p->supports_spend ? "yes" : "no", p->supports_quota ? "yes" : "no",
               p->notes ? p->notes : "");
    }
    return 0;
}

static void log_cb(void *user, tokenprobe_log_level_t level, const char *message) {
    static const char *names[] = {"error", "warn", "info", "debug"};
    FILE *out = (FILE *)user;
    fprintf(out, "[%s] %s\n", names[level <= TOKENPROBE_LOG_DEBUG ? level : TOKENPROBE_LOG_DEBUG],
            message);
    fflush(out);
}

static int print_text(const tokenprobe_provider_t *p, const tokenprobe_result_t *r) {
    int i;
    int n = tokenprobe_result_metric_count(r);
    const char *note = tokenprobe_result_note(r);

    printf("%s\n", p->display_name);
    if (p->base_url) {
        printf("  endpoint   %s\n", p->base_url);
    }
    {
        int avail = tokenprobe_result_available(r);
        if (avail >= 0) {
            printf("  available  %s\n", avail ? "yes" : "no");
        }
    }
    printf("  http       %ld\n", tokenprobe_result_http_status(r));
    printf("  latency    %lld ms\n", (long long)tokenprobe_result_latency_ms(r));

    if (n == 0) {
        printf("  metrics    (none reported)\n");
    } else {
        printf("  metrics\n");
        for (i = 0; i < n; i++) {
            const tokenprobe_metric_t *m = tokenprobe_result_metric(r, i);
            char amount[40] = "?";
            (void)tokenprobe_decimal_format(m->amount, amount, sizeof(amount));
            printf("    %-8s %-20s %12s %-4s\n", metric_kind_name(m->kind), m->id ? m->id : "?",
                   amount, m->currency ? m->currency : (m->unit ? m->unit : ""));
        }
    }
    if (note) {
        printf("  note       %s\n", note);
    }
    if (!p->supports_spend) {
        printf("  spend      not available from this provider's API\n");
    }
    return 0;
}

/*
 * 文档格式版本，两种机器可读格式共用。当已解析旧格式的消费方必须改动时才递增：
 * 字段改名、值类型变化、字段删除。新增字段不递增，读取方必须忽略未知字段。
 *
 * 两种格式携带相同的信息和结构，因此版本一起变化。仅涉及编码的改动
 * （字符串如何转义、缺失值是省略还是写 null）不算格式变更：两种格式无法
 * 表示对方的每一个字节，因此编码层面有意不做字节级比较。
 */
#define MACHINE_SCHEMA_VERSION 1

static void xml_header(void) {
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", stdout);
    printf("<tokenprobe schema=\"%d\" version=\"%s\">\n", MACHINE_SCHEMA_VERSION,
           TOKENPROBE_VERSION_STRING);
}

static void print_xml(const tokenprobe_provider_t *p, const tokenprobe_result_t *r, int with_raw) {
    int i;
    int n = tokenprobe_result_metric_count(r);
    const char *note = tokenprobe_result_note(r);

    xml_header();
    fputs("  <result", stdout);
    xml_attr(stdout, "provider", p->id);
    xml_attr(stdout, "display-name", p->display_name);
    xml_attr_bool(stdout, "ok", 1);
    xml_attr_int(stdout, "http-status", (long long)tokenprobe_result_http_status(r));
    xml_attr_int(stdout, "latency-ms", (long long)tokenprobe_result_latency_ms(r));
    xml_attr_int(stdout, "observed-at", (long long)tokenprobe_result_observed_at_ms(r));
    {
        /* 未上报的可用性直接省略，不退化成某个值，
         * 避免消费方把“未上报”渲染为 false。 */
        int avail = tokenprobe_result_available(r);
        if (avail >= 0) {
            xml_attr_bool(stdout, "available", avail);
        }
    }
    fputs(">\n", stdout);

    fputs("    <capabilities", stdout);
    xml_attr_bool(stdout, "credit", p->supports_credit);
    xml_attr_bool(stdout, "spend", p->supports_spend);
    xml_attr_bool(stdout, "quota", p->supports_quota);
    fputs("/>\n", stdout);

    if (n == 0) {
        fputs("    <metrics/>\n", stdout);
    } else {
        fputs("    <metrics>\n", stdout);
        for (i = 0; i < n; i++) {
            const tokenprobe_metric_t *m = tokenprobe_result_metric(r, i);
            char amount[40] = "?";
            (void)tokenprobe_decimal_format(m->amount, amount, sizeof(amount));

            fputs("      <metric", stdout);
            xml_attr(stdout, "kind", metric_kind_name(m->kind));
            xml_attr(stdout, "id", m->id);
            xml_attr(stdout, "label", m->label);
            xml_attr(stdout, "period-start", m->period_start);
            xml_attr(stdout, "period-end", m->period_end);
            fputs(">\n", stdout);

            fputs("        <amount", stdout);
            xml_attr(stdout, "unit", m->unit);
            xml_attr(stdout, "currency", m->currency);
            fputc('>', stdout);
            fputs(amount, stdout);
            fputs("</amount>\n", stdout);

            fputs("      </metric>\n", stdout);
        }
        fputs("    </metrics>\n", stdout);
    }

    if (note) {
        fputs("    <note>", stdout);
        xml_escape(stdout, note, 0);
        fputs("</note>\n", stdout);
    }

    if (with_raw) {
        /* 原始响应体必须放在文档内部，不能追加在文档之后：第二个根节点会导致
         * 输出无法解析，而 --raw 的目的正是查看可能已经损坏的字节。 */
        const char *raw = tokenprobe_result_raw_body(r);
        fputs("    <raw-response>", stdout);
        if (raw) {
            xml_escape(stdout, raw, 0);
        }
        fputs("</raw-response>\n", stdout);
    }

    fputs("  </result>\n</tokenprobe>\n", stdout);
}

static void print_error_xml(const tokenprobe_provider_t *p, const char *provider_id,
                            const tokenprobe_error_t *err) {
    xml_header();
    fputs("  <result", stdout);
    xml_attr(stdout, "provider", p ? p->id : provider_id);
    xml_attr_bool(stdout, "ok", 0);
    fputs(">\n", stdout);

    fputs("    <error", stdout);
    xml_attr(stdout, "code", tokenprobe_status_name(err->code));
    xml_attr_int(stdout, "http-status", (long long)err->http_status);
    fputs(">\n", stdout);

    fputs("      <message>", stdout);
    xml_escape(stdout, err->message, 0);
    fputs("</message>\n", stdout);

    if (err->hint[0]) {
        fputs("      <hint>", stdout);
        xml_escape(stdout, err->hint, 0);
        fputs("</hint>\n", stdout);
    }

    fputs("    </error>\n  </result>\n</tokenprobe>\n", stdout);
}

/*
 * JSON 文档。字段与 XML 一一对应，差异仅由编码限制决定：
 *
 *   - 供应商未上报的字段写为 null 而不是省略：JSON 只能通过省略键来表达
 *     “缺失”，而稳定的键集合对消费方比与 XML 结构逐字符一致更有价值；
 *   - 供应商文本中的 C0 控制字节会被转义并保留，而 XML 1.0 完全无法表示。
 */
static void print_json(const tokenprobe_provider_t *p, const tokenprobe_result_t *r, int with_raw) {
    int i;
    int n = tokenprobe_result_metric_count(r);

    fputs("{\"schema\":", stdout);
    printf("%d", MACHINE_SCHEMA_VERSION);
    fputs(",\"version\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, TOKENPROBE_VERSION_STRING);
    fputc('"', stdout);

    fputs(",\"result\":{\"provider\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, p->id);
    fputc('"', stdout);

    fputs(",\"display_name\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, p->display_name);
    fputc('"', stdout);

    fputs(",\"ok\":true,\"http_status\":", stdout);
    printf("%lld", (long long)tokenprobe_result_http_status(r));
    fputs(",\"latency_ms\":", stdout);
    printf("%lld", (long long)tokenprobe_result_latency_ms(r));
    fputs(",\"observed_at\":", stdout);
    printf("%lld", (long long)tokenprobe_result_observed_at_ms(r));
    fputs(",\"available\":", stdout);
    {
        int avail = tokenprobe_result_available(r);
        fputs(avail < 0 ? "null" : (avail ? "true" : "false"), stdout);
    }

    fputs(",\"capabilities\":{\"credit\":", stdout);
    fputs(p->supports_credit ? "true" : "false", stdout);
    fputs(",\"spend\":", stdout);
    fputs(p->supports_spend ? "true" : "false", stdout);
    fputs(",\"quota\":", stdout);
    fputs(p->supports_quota ? "true" : "false", stdout);
    fputs("},\"metrics\":[", stdout);

    for (i = 0; i < n; i++) {
        const tokenprobe_metric_t *m = tokenprobe_result_metric(r, i);
        char amount[40] = "?";
        (void)tokenprobe_decimal_format(m->amount, amount, sizeof(amount));

        if (i) {
            fputc(',', stdout);
        }
        fputs("{\"kind\":", stdout);
        fputc('"', stdout);
        json_escape(stdout, metric_kind_name(m->kind));
        fputc('"', stdout);

        fputc(',', stdout);
        json_field(stdout, "id", m->id);
        fputc(',', stdout);
        json_field(stdout, "label", m->label);
        fputc(',', stdout);
        json_field(stdout, "period_start", m->period_start);
        fputc(',', stdout);
        json_field(stdout, "period_end", m->period_end);

        /* 金额使用字符串而不是 JSON 数字，以保证十进制值精确往返；
         * 读取方若转成浮点数，那是它自己的选择，而不是本程序替它做的。 */
        fputs(",\"amount\":", stdout);
        fputc('"', stdout);
        fputs(amount, stdout);
        fputc('"', stdout);

        fputc(',', stdout);
        json_field(stdout, "unit", m->unit);
        fputc(',', stdout);
        json_field(stdout, "currency", m->currency);

        fputc('}', stdout);
    }

    fputs("],", stdout);
    json_field(stdout, "note", tokenprobe_result_note(r));

    if (with_raw) {
        fputs(",", stdout);
        json_field(stdout, "raw_response", tokenprobe_result_raw_body(r));
    }

    fputs("}}\n", stdout);
}

static void print_error_json(const tokenprobe_provider_t *p, const char *provider_id,
                             const tokenprobe_error_t *err) {
    fputs("{\"schema\":", stdout);
    printf("%d", MACHINE_SCHEMA_VERSION);
    fputs(",\"version\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, TOKENPROBE_VERSION_STRING);
    fputc('"', stdout);

    fputs(",\"result\":{\"provider\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, p ? p->id : provider_id);
    fputc('"', stdout);
    fputs(",\"ok\":false,\"error\":{\"code\":", stdout);
    fputc('"', stdout);
    json_escape(stdout, tokenprobe_status_name(err->code));
    fputc('"', stdout);
    fputs(",\"http_status\":", stdout);
    printf("%lld", (long long)err->http_status);
    fputc(',', stdout);
    json_field(stdout, "message", err->message);
    fputc(',', stdout);
    json_field(stdout, "hint", err->hint[0] ? err->hint : NULL);

    fputs("}}}\n", stdout);
}

int main(int argc, char **argv) {
    options opt;
    tokenprobe_context_t *ctx = NULL;
    tokenprobe_context_options_t ctx_opts;
    tokenprobe_result_t *result = NULL;
    tokenprobe_error_t err;
    const tokenprobe_provider_t *provider = NULL;
    tokenprobe_status_t st;
    int i;
    int rc = 0;

    enable_utf8_console();
    memset(&opt, 0, sizeof(opt));
    tokenprobe_error_init(&err);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (streq(a, "-h") || streq(a, "--help")) {
            usage(stdout);
            return 0;
        }
        if (streq(a, "--version")) {
            printf("tokenprobe %s\n", TOKENPROBE_VERSION_STRING);
            return 0;
        }
        if (streq(a, "-l") || streq(a, "--list")) {
            opt.list = 1;
            continue;
        }
        if (streq(a, "-p") || streq(a, "--provider") || strncmp(a, "--provider=", 11) == 0) {
            opt.provider = take_value(argc, argv, &i, a, "--provider");
            if (!opt.provider && streq(a, "-p") && (i + 1) < argc) {
                opt.provider = argv[++i];
            }
            if (!opt.provider) {
                fprintf(stderr, "tokenprobe: --provider needs a value\n");
                return 2;
            }
            continue;
        }
        if (streq(a, "-k") || streq(a, "--api-key") || strncmp(a, "--api-key=", 10) == 0) {
            opt.api_key = take_value(argc, argv, &i, a, "--api-key");
            if (!opt.api_key) {
                fprintf(stderr, "tokenprobe: --api-key needs a value\n");
                return 2;
            }
            continue;
        }
        if (streq(a, "-u") || streq(a, "--base-url") || strncmp(a, "--base-url=", 11) == 0) {
            opt.base_url = take_value(argc, argv, &i, a, "--base-url");
            if (!opt.base_url) {
                fprintf(stderr, "tokenprobe: --base-url needs a value\n");
                return 2;
            }
            continue;
        }
        if (streq(a, "-t") || streq(a, "--timeout") || strncmp(a, "--timeout=", 10) == 0) {
            const char *v = take_value(argc, argv, &i, a, "--timeout");
            char *end = NULL;
            long ms;
            if (!v) {
                fprintf(stderr, "tokenprobe: --timeout needs a value\n");
                return 2;
            }
            ms = strtol(v, &end, 10);
            if (!end || *end != '\0' || ms <= 0 || ms > 600000) {
                fprintf(stderr, "tokenprobe: --timeout must be 1..600000 ms\n");
                return 2;
            }
            opt.timeout_ms = (int)ms;
            continue;
        }
        if (streq(a, "--xml") || streq(a, "--json")) {
            output_format want = streq(a, "--xml") ? OUTPUT_XML : OUTPUT_JSON;
            if (select_format(&opt.format, want) != 0) {
                fprintf(stderr, "tokenprobe: --json and --xml are mutually exclusive\n");
                return 2;
            }
            continue;
        }
        if (streq(a, "--raw")) {
            opt.show_raw = 1;
            continue;
        }
        if (streq(a, "--headers")) {
            opt.show_headers = 1;
            continue;
        }
        if (streq(a, "-q") || streq(a, "--quiet")) {
            opt.quiet = 1;
            continue;
        }
        if (streq(a, "-v") || streq(a, "--verbose")) {
            opt.verbose = 1;
            continue;
        }
        fprintf(stderr, "tokenprobe: unknown option '%s'\n", a);
        usage(stderr);
        return 2;
    }

    if (opt.list) {
        return cmd_list();
    }
    if (!opt.provider) {
        fprintf(stderr, "tokenprobe: --provider is required (or use --list)\n");
        return 2;
    }

    provider = tokenprobe_provider_find(opt.provider);
    if (!provider) {
        fprintf(stderr, "tokenprobe: unknown provider '%s'\n", opt.provider);
        fprintf(stderr, "tokenprobe: run --list to see the supported providers\n");
        return 2;
    }

    if (!provider->supports_credit && !provider->supports_spend && !provider->supports_quota) {
        fprintf(stderr, "tokenprobe: %s exposes no credit, spend or quota endpoint\n",
                provider->display_name);
        return 1;
    }

    tokenprobe_context_options_init(&ctx_opts);
    if (opt.timeout_ms > 0) {
        ctx_opts.timeout_ms = opt.timeout_ms;
    }
    if (opt.verbose) {
        ctx_opts.log_fn = log_cb;
        ctx_opts.log_user = stderr;
    }

    ctx = tokenprobe_context_create(&ctx_opts);
    if (!ctx) {
        fprintf(stderr, "tokenprobe: no usable network transport in this build\n");
        return 1;
    }

    st = tokenprobe_probe(provider, ctx, opt.api_key, opt.base_url, &result, &err);

    if (st != TOKENPROBE_OK) {
        rc = 1;
        if (!opt.quiet) {
            if (opt.format == OUTPUT_XML) {
                print_error_xml(provider, opt.provider, &err);
            } else if (opt.format == OUTPUT_JSON) {
                print_error_json(provider, opt.provider, &err);
            } else {
                char buf[720];
                tokenprobe_error_format(buf, sizeof(buf), &err);
                fprintf(stderr, "%s: %s\n", provider->id, buf);
                if (err.hint[0]) {
                    fprintf(stderr, "  hint: %s\n", err.hint);
                }
                if (err.code == TOKENPROBE_ERR_UNSUPPORTED) {
                    fprintf(stderr, "  note: %s\n", provider->notes ? provider->notes : "");
                }
            }
        }
        goto cleanup;
    }

    if (!opt.quiet) {
        if (opt.format == OUTPUT_XML) {
            print_xml(provider, result, opt.show_raw);
        } else if (opt.format == OUTPUT_JSON) {
            print_json(provider, result, opt.show_raw);
        } else {
            print_text(provider, result);
            if (opt.show_raw) {
                const char *raw = tokenprobe_result_raw_body(result);
                printf("\nraw response:\n%s\n", raw ? raw : "(empty)");
            }
        }
        if (opt.show_headers) {
            /* 人类可读的诊断信息，不属于文档本身：机器可读模式下写入 stderr，
             * 保证 stdout 始终是可解析的文档。 */
            int machine = opt.format != OUTPUT_TEXT;
            fprintf(machine ? stderr : stdout,
                    "%s(use --raw for the response body; header capture is transport specific)\n",
                    machine ? "tokenprobe: " : "\n");
        }
    }

cleanup:
    tokenprobe_result_free(result);
    tokenprobe_context_destroy(ctx);
    return rc;
}
