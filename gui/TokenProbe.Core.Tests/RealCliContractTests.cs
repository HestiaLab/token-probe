using System.Text.RegularExpressions;
using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

/// <summary>
/// Runs against the real CLI when a C build exists in this checkout, so the parsing
/// and process contract are validated against the shipped binary rather than only
/// the stand-in. Returns silently when the CLI has not been built.
/// </summary>
public sealed class RealCliContractTests
{
    [Fact]
    public async Task RealCliReportsStructuredInvalidKeyError()
    {
        var executable = FindRealExecutable();
        if (executable is null)
        {
            return;
        }

        var previousKey = Environment.GetEnvironmentVariable("DEEPSEEK_API_KEY");
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", null);
        try
        {
            var client = new TokenProbeProcessClient(executable);

            var outcome = await client.ProbeAsync(new ProbeRequest("deepseek", ApiKey: null), CancellationToken.None);

            Assert.Null(outcome.Failure);
            Assert.Equal(1, outcome.ExitCode);
            var result = Assert.IsType<ProbeResult>(outcome.Result);
            Assert.False(result.Ok);
            var error = Assert.IsType<ProbeErrorInfo>(result.Error);
            Assert.Equal("INVALID_KEY", error.Code);
            Assert.Contains("DEEPSEEK_API_KEY", error.Hint);
        }
        finally
        {
            Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", previousKey);
        }
    }

    [Fact]
    public async Task RealCliReportsItsVersion()
    {
        var executable = FindRealExecutable();
        if (executable is null)
        {
            return;
        }

        var version = await new TokenProbeProcessClient(executable).TryReadVersionAsync(CancellationToken.None);

        Assert.NotNull(version);
        Assert.Matches(new Regex(@"^\d+\.\d+\.\d+$"), version);
    }

    private static string? FindRealExecutable()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for (var level = 0; level < 8 && directory is not null; level++, directory = directory.Parent)
        {
            foreach (var preset in new[] { "ninja-release", "ninja-debug" })
            {
                var candidate = Path.Combine(directory.FullName, "build", preset, "bin", "tokenprobe.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }

        return null;
    }
}
