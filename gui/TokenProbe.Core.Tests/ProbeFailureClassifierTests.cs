using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class ProbeFailureClassifierTests
{
    private static ProbeOutcome Error(string code) => new()
    {
        ExitCode = 1,
        Result = new ProbeResult
        {
            Schema = 1,
            Provider = "deepseek",
            Ok = false,
            Error = new ProbeErrorInfo(code, "message", null, 0),
        },
    };

    [Fact]
    public void SuccessfulProbeMeansTheKeyIsValid()
    {
        var outcome = new ProbeOutcome
        {
            Result = new ProbeResult { Schema = 1, Provider = "deepseek", Ok = true },
        };

        Assert.Equal(CredentialStatus.Valid, ProbeFailureClassifier.Classify(outcome));
    }

    [Theory]
    [InlineData("INVALID_KEY")]
    [InlineData("AUTH")]
    [InlineData("FORBIDDEN")]
    public void CredentialErrorCodesMeanTheKeyIsInvalid(string code)
    {
        Assert.Equal(CredentialStatus.Invalid, ProbeFailureClassifier.Classify(Error(code)));
    }

    [Theory]
    [InlineData("TIMEOUT")]
    [InlineData("DNS")]
    [InlineData("RATE_LIMIT")]
    [InlineData("SERVER")]
    [InlineData("HTTP")]
    public void EverythingElseIsInconclusive(string code)
    {
        Assert.Equal(CredentialStatus.Inconclusive, ProbeFailureClassifier.Classify(Error(code)));
    }

    [Fact]
    public void TransportFailureIsInconclusive()
    {
        var outcome = new ProbeOutcome { Failure = "找不到 tokenprobe.exe" };

        Assert.Equal(CredentialStatus.Inconclusive, ProbeFailureClassifier.Classify(outcome));
    }

    [Fact]
    public void UnparseableOutputIsInconclusive()
    {
        var outcome = new ProbeOutcome { Failure = "无法解析 tokenprobe 的 JSON 输出" };

        Assert.Equal(CredentialStatus.Inconclusive, ProbeFailureClassifier.Classify(outcome));
    }
}
