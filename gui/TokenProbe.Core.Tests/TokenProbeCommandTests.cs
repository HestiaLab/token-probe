using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class TokenProbeCommandTests
{
    [Fact]
    public void BuildsJsonArgumentsWithoutTheKey()
    {
        var startInfo = TokenProbeCommand.CreateStartInfo(
            @"C:\tools\tokenprobe.exe",
            new ProbeRequest("deepseek", "sk-secret-value", TimeoutMs: 12_345));

        Assert.Equal(@"C:\tools\tokenprobe.exe", startInfo.FileName);
        Assert.False(startInfo.UseShellExecute);
        Assert.True(startInfo.RedirectStandardOutput);
        Assert.True(startInfo.RedirectStandardError);
        Assert.Equal(
            ["--provider", "deepseek", "--json", "--timeout", "12345"],
            startInfo.ArgumentList);

        // The key must never be reachable from the process command line.
        Assert.DoesNotContain(startInfo.ArgumentList, argument => argument.Contains("sk-secret-value", StringComparison.Ordinal));
        Assert.DoesNotContain("--api-key", startInfo.ArgumentList);
    }

    [Fact]
    public void PassesTheKeyThroughTheChildEnvironmentOnly()
    {
        var startInfo = TokenProbeCommand.CreateStartInfo(
            @"C:\tools\tokenprobe.exe",
            new ProbeRequest("deepseek", "sk-secret-value"));

        Assert.Equal("sk-secret-value", startInfo.Environment["DEEPSEEK_API_KEY"]);
    }

    [Fact]
    public void OmitsTheKeyVariableWhenNoKeyIsGiven()
    {
        var startInfo = TokenProbeCommand.CreateStartInfo(
            @"C:\tools\tokenprobe.exe",
            new ProbeRequest("deepseek", ApiKey: null));

        Assert.DoesNotContain("--api-key", startInfo.ArgumentList);
        Assert.DoesNotContain(startInfo.ArgumentList, argument => argument.StartsWith("sk-", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("deepseek", "DEEPSEEK_API_KEY")]
    [InlineData("DeepSeek", "DEEPSEEK_API_KEY")]
    [InlineData("open-ai", "OPEN_AI_API_KEY")]
    public void MapsProviderIdsToKeyVariables(string provider, string expected)
    {
        Assert.Equal(expected, ProviderEnvironment.KeyVariable(provider));
    }

    [Fact]
    public void RejectsMissingProvider()
    {
        Assert.ThrowsAny<ArgumentException>(() =>
            TokenProbeCommand.CreateStartInfo(@"C:\tools\tokenprobe.exe", new ProbeRequest(" ", null)));
    }
}
