namespace TokenProbe.Core;

public enum CredentialStatus
{
    /// <summary>The probe succeeded; the key is usable.</summary>
    Valid,

    /// <summary>The provider rejected the key.</summary>
    Invalid,

    /// <summary>The key could not be judged (network, timeout, unexpected response).</summary>
    Inconclusive,
}

public static class ProbeFailureClassifier
{
    private static readonly HashSet<string> CredentialErrorCodes = new(StringComparer.Ordinal)
    {
        "INVALID_KEY",
        "AUTH",
        "FORBIDDEN",
    };

    public static CredentialStatus Classify(ProbeOutcome outcome)
    {
        ArgumentNullException.ThrowIfNull(outcome);

        if (outcome.Failure is not null || outcome.Result is null)
        {
            return CredentialStatus.Inconclusive;
        }

        if (outcome.Result.Ok)
        {
            return CredentialStatus.Valid;
        }

        var code = outcome.Result.Error?.Code;
        return code is not null && CredentialErrorCodes.Contains(code)
            ? CredentialStatus.Invalid
            : CredentialStatus.Inconclusive;
    }
}
