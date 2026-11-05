using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class TokenProbeLocatorTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), "tokenprobe-locator-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    [Fact]
    public void ResolvesTheBundledToolsDirectory()
    {
        var expected = Path.Combine(_root, "tools", "tokenprobe.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(expected)!);
        File.WriteAllText(expected, "stub");

        Assert.Equal(expected, TokenProbeLocator.DefaultPath(_root));
        Assert.True(TokenProbeLocator.Exists(_root));
    }

    [Fact]
    public void ReportsMissingBundledExecutable()
    {
        Assert.False(TokenProbeLocator.Exists(_root));
    }

    [Fact]
    public void DoesNotHonourAnEnvironmentOverride()
    {
        var overridePath = Path.Combine(_root, "elsewhere", "tokenprobe.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(overridePath)!);
        File.WriteAllText(overridePath, "stub");

        var previous = Environment.GetEnvironmentVariable("TOKENPROBE_EXE_PATH");
        Environment.SetEnvironmentVariable("TOKENPROBE_EXE_PATH", overridePath);
        try
        {
            Assert.False(TokenProbeLocator.Exists(_root));
            Assert.Equal(Path.Combine(_root, "tools", "tokenprobe.exe"), TokenProbeLocator.DefaultPath(_root));
        }
        finally
        {
            Environment.SetEnvironmentVariable("TOKENPROBE_EXE_PATH", previous);
        }
    }
}
