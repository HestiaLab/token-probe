using System.Diagnostics;
using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

/// <summary>
/// Exercises the real process plumbing against <c>TokenProbe.FakeProbe.exe</c>, which
/// mirrors the CLI's observable contract. No network, no API keys.
/// </summary>
public sealed class TokenProbeProcessClientTests : IDisposable
{
    private static readonly string FakeExecutable =
        Path.Combine(AppContext.BaseDirectory, "fake", "TokenProbe.FakeProbe.exe");

    private static readonly TimeSpan ShortGrace = TimeSpan.FromMilliseconds(500);

    private readonly string? _previousMode;
    private readonly string? _previousKey;

    public TokenProbeProcessClientTests()
    {
        _previousMode = Environment.GetEnvironmentVariable("FAKE_PROBE_MODE");
        _previousKey = Environment.GetEnvironmentVariable("DEEPSEEK_API_KEY");
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", null);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("FAKE_PROBE_MODE", _previousMode);
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", _previousKey);
    }

    [Fact]
    public async Task ReturnsParsedResultAndInjectsKey()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("success", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        Assert.Null(outcome.Failure);
        Assert.Equal(0, outcome.ExitCode);
        Assert.True(outcome.Ok);
        var result = Assert.IsType<ProbeResult>(outcome.Result);
        Assert.Equal(2, result.Metrics.Count);
        Assert.Equal("key-present", result.Account);
        Assert.Equal("110.00", result.Metrics[0].Amount);
        Assert.Equal(string.Empty, outcome.StandardError);
    }

    [Fact]
    public async Task OmitsKeyWhenCallerHasNone()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("success", () => client.ProbeAsync(new ProbeRequest("deepseek", ApiKey: null), CancellationToken.None));

        var result = Assert.IsType<ProbeResult>(outcome.Result);
        Assert.Equal("no-key", result.Account);
    }

    [Fact]
    public async Task SurfacesStructuredError()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("error", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-bad"), CancellationToken.None));

        Assert.Null(outcome.Failure);
        Assert.Equal(1, outcome.ExitCode);
        Assert.False(outcome.Ok);
        var result = Assert.IsType<ProbeResult>(outcome.Result);
        var error = Assert.IsType<ProbeErrorInfo>(result.Error);
        Assert.Equal("INVALID_KEY", error.Code);
        Assert.False(string.IsNullOrWhiteSpace(error.Hint));
    }

    [Fact]
    public async Task ReportsUnparseableOutputWithStandardError()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("garbage", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        Assert.NotNull(outcome.Failure);
        Assert.Contains("无法解析", outcome.Failure);
        Assert.Contains("internal failure", outcome.Failure);
        Assert.Equal("this is not json", outcome.RawJson.Trim());
    }

    [Fact]
    public async Task ReportsEmptyOutputWithExitCode()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("empty", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        Assert.NotNull(outcome.Failure);
        Assert.Contains("退出码 3", outcome.Failure);
        Assert.Contains("silent failure", outcome.Failure);
    }

    [Fact]
    public async Task ReportsStandardErrorOnlyFailure()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("stderr-only", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        Assert.NotNull(outcome.Failure);
        Assert.Contains("cannot reach provider", outcome.Failure);
    }

    [Fact]
    public async Task TimesOutAndTerminatesTheProcess()
    {
        var client = CreateClient();
        var stopwatch = Stopwatch.StartNew();

        var outcome = await WithModeAsync("hang", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test", TimeoutMs: 1_000), CancellationToken.None));

        stopwatch.Stop();
        Assert.True(outcome.TimedOut);
        Assert.NotNull(outcome.Failure);
        Assert.Contains("没有返回", outcome.Failure);
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(15), $"超时处理耗时过长：{stopwatch.Elapsed}");
    }

    [Fact]
    public async Task PropagatesCallerCancellation()
    {
        var client = CreateClient();
        using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));

        var stopwatch = Stopwatch.StartNew();
        await WithModeAsync("hang", async () =>
        {
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), cancellation.Token));
        });

        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(10), $"取消耗时过长：{stopwatch.Elapsed}");
    }

    [Fact]
    public async Task DecodesUtf8Output()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("utf8", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        var result = Assert.IsType<ProbeResult>(outcome.Result);
        Assert.Equal("深度求索", result.DisplayName);
        Assert.Equal("总余额", result.Metrics[0].Label);
        Assert.Contains("币种", result.Note);
    }

    [Fact]
    public async Task WarnsWhenSchemaIsNewer()
    {
        var client = CreateClient();

        var outcome = await WithModeAsync("future-schema", () => client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None));

        Assert.NotNull(outcome.Warning);
        Assert.Equal(2, outcome.Result!.Schema);
    }

    [Fact]
    public async Task ReadsCliVersion()
    {
        var client = CreateClient();

        var version = await client.TryReadVersionAsync(CancellationToken.None);

        Assert.Equal("0.2.0", version);
    }

    [Fact]
    public async Task ReportsMissingExecutableWithoutThrowing()
    {
        var client = new TokenProbeProcessClient(Path.Combine(Path.GetTempPath(), "definitely-missing-tokenprobe.exe"));

        var outcome = await client.ProbeAsync(new ProbeRequest("deepseek", "sk-test"), CancellationToken.None);

        Assert.Null(outcome.Result);
        Assert.Contains("找不到 tokenprobe.exe", outcome.Failure);
    }

    private static TokenProbeProcessClient CreateClient()
    {
        Assert.True(File.Exists(FakeExecutable), $"测试用假 CLI 不存在：{FakeExecutable}");
        return new TokenProbeProcessClient(FakeExecutable, ShortGrace);
    }

    private static async Task WithModeAsync(string mode, Func<Task> action)
    {
        var previous = Environment.GetEnvironmentVariable("FAKE_PROBE_MODE");
        Environment.SetEnvironmentVariable("FAKE_PROBE_MODE", mode);
        try
        {
            await action();
        }
        finally
        {
            Environment.SetEnvironmentVariable("FAKE_PROBE_MODE", previous);
        }
    }

    private static async Task<T> WithModeAsync<T>(string mode, Func<Task<T>> action)
    {
        var previous = Environment.GetEnvironmentVariable("FAKE_PROBE_MODE");
        Environment.SetEnvironmentVariable("FAKE_PROBE_MODE", mode);
        try
        {
            return await action();
        }
        finally
        {
            Environment.SetEnvironmentVariable("FAKE_PROBE_MODE", previous);
        }
    }
}
