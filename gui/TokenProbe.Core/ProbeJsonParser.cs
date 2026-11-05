using System.Text.Json;

namespace TokenProbe.Core;

public sealed record ProbeParseResult(ProbeResult? Result, string? Error, string? Warning)
{
    public static ProbeParseResult Failed(string error) => new(null, error, null);
}

/// <summary>
/// Reads the documented <c>--json</c> contract. Unknown fields are ignored so a
/// newer CLI that only adds fields keeps working; a higher <c>schema</c> number is
/// surfaced as a warning instead of being treated as a failure.
/// </summary>
public static class ProbeJsonParser
{
    public const int SupportedSchema = 1;

    public static ProbeParseResult Parse(string json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return ProbeParseResult.Failed("tokenprobe 没有输出任何内容。");
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                AllowTrailingCommas = true,
                CommentHandling = JsonCommentHandling.Skip,
            });
        }
        catch (JsonException ex)
        {
            return ProbeParseResult.Failed($"无法解析 tokenprobe 的 JSON 输出：{ex.Message}");
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return ProbeParseResult.Failed("tokenprobe 的输出不是 JSON 对象。");
            }

            var schema = GetInt(root, "schema") ?? SupportedSchema;
            var warning = schema > SupportedSchema
                ? $"输出 schema 为 {schema}，本界面按 schema {SupportedSchema} 解析，部分字段可能未显示。"
                : null;

            if (!root.TryGetProperty("result", out var resultElement) ||
                resultElement.ValueKind != JsonValueKind.Object)
            {
                return ProbeParseResult.Failed("tokenprobe 的输出缺少 result 对象。");
            }

            var provider = GetString(resultElement, "provider") ?? string.Empty;
            var ok = GetBool(resultElement, "ok") ?? false;

            ProbeErrorInfo? error = null;
            if (resultElement.TryGetProperty("error", out var errorElement) &&
                errorElement.ValueKind == JsonValueKind.Object)
            {
                error = new ProbeErrorInfo(
                    GetString(errorElement, "code") ?? "UNKNOWN",
                    GetString(errorElement, "message") ?? string.Empty,
                    GetString(errorElement, "hint"),
                    GetInt(errorElement, "http_status") ?? 0);
            }

            var result = new ProbeResult
            {
                Schema = schema,
                CliVersion = GetString(root, "version"),
                Provider = provider,
                DisplayName = GetString(resultElement, "display_name"),
                Ok = ok,
                HttpStatus = GetInt(resultElement, "http_status") ?? 0,
                LatencyMs = GetLong(resultElement, "latency_ms") ?? 0,
                ObservedAtMs = GetLong(resultElement, "observed_at") ?? 0,
                Available = GetBool(resultElement, "available"),
                Capabilities = ReadCapabilities(resultElement),
                Metrics = ReadMetrics(resultElement),
                Note = GetString(resultElement, "note"),
                Account = GetString(resultElement, "account"),
                Error = error,
                RawResponse = GetString(resultElement, "raw_response"),
            };

            return new ProbeParseResult(result, null, warning);
        }
    }

    private static Capabilities ReadCapabilities(JsonElement result)
    {
        if (!result.TryGetProperty("capabilities", out var capabilities) ||
            capabilities.ValueKind != JsonValueKind.Object)
        {
            return Capabilities.None;
        }

        return new Capabilities(
            GetBool(capabilities, "credit") ?? false,
            GetBool(capabilities, "spend") ?? false,
            GetBool(capabilities, "quota") ?? false);
    }

    private static IReadOnlyList<ProbeMetric> ReadMetrics(JsonElement result)
    {
        if (!result.TryGetProperty("metrics", out var metrics) ||
            metrics.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        var list = new List<ProbeMetric>();
        foreach (var metric in metrics.EnumerateArray())
        {
            if (metric.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            list.Add(new ProbeMetric(
                GetString(metric, "kind") ?? "unknown",
                GetString(metric, "id"),
                GetString(metric, "label"),
                GetString(metric, "period_start"),
                GetString(metric, "period_end"),
                GetScalarText(metric, "amount"),
                GetString(metric, "unit"),
                GetString(metric, "currency")));
        }

        return list;
    }

    private static string? GetString(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        return value.ValueKind switch
        {
            JsonValueKind.String => value.GetString(),
            JsonValueKind.Null or JsonValueKind.Undefined => null,
            _ => value.GetRawText(),
        };
    }

    /// <summary>
    /// Accepts a number as well as a string. The contract says amounts are strings,
    /// but a reader that throws on a number would be brittle for no benefit.
    /// </summary>
    private static string? GetScalarText(JsonElement element, string name) => GetString(element, name);

    private static int? GetInt(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number))
        {
            return number;
        }

        return value.ValueKind == JsonValueKind.String &&
               int.TryParse(value.GetString(), out var parsed)
            ? parsed
            : null;
    }

    private static long? GetLong(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number))
        {
            return number;
        }

        return value.ValueKind == JsonValueKind.String &&
               long.TryParse(value.GetString(), out var parsed)
            ? parsed
            : null;
    }

    private static bool? GetBool(JsonElement element, string name)
    {
        if (!element.TryGetProperty(name, out var value))
        {
            return null;
        }

        return value.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
    }
}
