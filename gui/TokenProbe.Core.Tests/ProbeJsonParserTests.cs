using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class ProbeJsonParserTests
{
    private const string SuccessJson = """
        {"schema":1,"version":"0.2.0","result":{"provider":"deepseek","display_name":"DeepSeek","ok":true,"http_status":200,"latency_ms":12,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":false,"quota":false},"metrics":[{"kind":"credit","id":"total_balance","label":"Total balance","period_start":null,"period_end":null,"amount":"110.00","unit":"currency","currency":"CNY"},{"kind":"credit","id":"granted_balance","label":"Granted balance","period_start":null,"period_end":null,"amount":"10.00","unit":"currency","currency":"USD"}],"note":"multiple currencies reported; each currency is listed separately and must not be summed"}}
        """;

    [Fact]
    public void ParsesSuccessfulDocument()
    {
        var parsed = ProbeJsonParser.Parse(SuccessJson);

        Assert.Null(parsed.Error);
        Assert.Null(parsed.Warning);
        var result = Assert.IsType<ProbeResult>(parsed.Result);
        Assert.True(result.Ok);
        Assert.Equal("deepseek", result.Provider);
        Assert.Equal("DeepSeek", result.DisplayName);
        Assert.Equal(200, result.HttpStatus);
        Assert.Equal(12, result.LatencyMs);
        Assert.True(result.Available);
        Assert.True(result.Capabilities.Credit);
        Assert.False(result.Capabilities.Spend);
        Assert.False(result.Capabilities.Quota);
        Assert.Equal(2, result.Metrics.Count);
        Assert.Equal("110.00", result.Metrics[0].Amount);
        Assert.Equal("CNY", result.Metrics[0].Currency);
        Assert.Equal("USD", result.Metrics[1].Currency);
        Assert.NotNull(result.ObservedAt);
    }

    [Fact]
    public void ParsesErrorDocument()
    {
        const string json = """
            {"schema":1,"version":"0.2.0","result":{"provider":"deepseek","ok":false,"error":{"code":"INVALID_KEY","http_status":0,"message":"no API key for DeepSeek","hint":"set DEEPSEEK_API_KEY or pass --api-key"}}}
            """;

        var parsed = ProbeJsonParser.Parse(json);
        var result = Assert.IsType<ProbeResult>(parsed.Result);

        Assert.False(result.Ok);
        var error = Assert.IsType<ProbeErrorInfo>(result.Error);
        Assert.Equal("INVALID_KEY", error.Code);
        Assert.Equal("no API key for DeepSeek", error.Message);
        Assert.Equal("set DEEPSEEK_API_KEY or pass --api-key", error.Hint);
        Assert.Empty(result.Metrics);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public void RejectsEmptyOutput(string json)
    {
        var parsed = ProbeJsonParser.Parse(json);

        Assert.Null(parsed.Result);
        Assert.NotNull(parsed.Error);
    }

    [Fact]
    public void RejectsNonJsonOutput()
    {
        var parsed = ProbeJsonParser.Parse("tokenprobe: something went wrong");

        Assert.Null(parsed.Result);
        Assert.Contains("无法解析", parsed.Error);
    }

    [Fact]
    public void RejectsDocumentWithoutResultObject()
    {
        var parsed = ProbeJsonParser.Parse("""{"schema":1,"version":"0.2.0"}""");

        Assert.Null(parsed.Result);
        Assert.Contains("result", parsed.Error);
    }

    [Fact]
    public void WarnsButStillParsesNewerSchema()
    {
        const string json = """
            {"schema":2,"version":"0.3.0","result":{"provider":"deepseek","ok":true,"http_status":200,"latency_ms":5,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":true,"quota":false},"metrics":[],"brand_new_field":{"nested":true}}}
            """;

        var parsed = ProbeJsonParser.Parse(json);
        var result = Assert.IsType<ProbeResult>(parsed.Result);

        Assert.NotNull(parsed.Warning);
        Assert.Contains("输出 schema 为 2", parsed.Warning);
        Assert.Equal(2, result.Schema);
        Assert.True(result.Capabilities.Spend);
    }

    [Fact]
    public void KeepsAmountTextWhenProviderSendsNumber()
    {
        const string json = """
            {"schema":1,"result":{"provider":"deepseek","ok":true,"metrics":[{"kind":"credit","id":"x","label":"X","amount":110.5,"unit":"currency","currency":"CNY"}]}}
            """;

        var parsed = ProbeJsonParser.Parse(json);
        var result = Assert.IsType<ProbeResult>(parsed.Result);

        Assert.Equal("110.5", result.Metrics[0].Amount);
    }

    [Fact]
    public void ToleratesNullAvailabilityAndMissingOptionalFields()
    {
        const string json = """
            {"schema":1,"result":{"provider":"deepseek","ok":true,"available":null,"metrics":[{"kind":"credit","id":"x","label":"X","period_start":null,"period_end":null,"amount":"1.00","unit":null,"currency":null}]}}
            """;

        var parsed = ProbeJsonParser.Parse(json);
        var result = Assert.IsType<ProbeResult>(parsed.Result);

        Assert.Null(result.Available);
        Assert.Null(result.Metrics[0].Currency);
        Assert.Null(result.Metrics[0].Unit);
        Assert.Null(result.Note);
    }

    [Fact]
    public void KeepsRawResponseWhenRequested()
    {
        const string json = """
            {"schema":1,"result":{"provider":"deepseek","ok":true,"metrics":[],"raw_response":"{\"is_available\":true}"}}
            """;

        var parsed = ProbeJsonParser.Parse(json);
        var result = Assert.IsType<ProbeResult>(parsed.Result);

        Assert.Equal("{\"is_available\":true}", result.RawResponse);
    }
}
