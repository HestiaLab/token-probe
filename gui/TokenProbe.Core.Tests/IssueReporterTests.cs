using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class IssueReporterTests
{
    [Fact]
    public void BuildsAPrefilledIssueUrl()
    {
        var uri = IssueReporter.BuildUri(
            appVersion: "1.0.0",
            cliVersion: "0.2.0",
            operatingSystem: "Microsoft Windows NT 10.0",
            logDirectory: @"C:\Users\someone\AppData\Local\TokenProbe\logs");

        Assert.StartsWith(IssueReporter.NewIssueBaseUrl, uri);
        Assert.Contains("title=bug%3A%20", uri);
        Assert.Contains("body=", uri);
    }

    [Fact]
    public void BodyCarriesEnvironmentButNoProbeData()
    {
        var uri = IssueReporter.BuildUri("1.0.0", "0.2.0", "Windows 11", @"C:\logs");

        var bodyParameter = uri[(uri.IndexOf("body=", StringComparison.Ordinal) + "body=".Length)..];
        var body = Uri.UnescapeDataString(bodyParameter);

        Assert.Contains("TokenProbe WPF：1.0.0", body);
        Assert.Contains("tokenprobe CLI：0.2.0", body);
        Assert.Contains("Windows 11", body);
        Assert.Contains(@"C:\logs", body);
        Assert.Contains("日志", body);
        Assert.DoesNotContain("110.00", body);
        Assert.DoesNotContain("sk-", body);
    }
}
