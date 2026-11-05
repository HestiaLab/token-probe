namespace TokenProbe.Core;

/// <summary>
/// Declaration of which quantities a provider can report. A missing quantity is
/// distinct from a quantity that happens to be zero.
/// </summary>
public sealed record Capabilities(bool Credit, bool Spend, bool Quota)
{
    public static Capabilities None { get; } = new(false, false, false);
}

/// <summary>
/// One reported quantity. <see cref="Amount"/> stays a string on purpose: the CLI
/// emits exact decimal text and converting it to a floating point type would
/// silently corrupt the value.
/// </summary>
public sealed record ProbeMetric(
    string Kind,
    string? Id,
    string? Label,
    string? PeriodStart,
    string? PeriodEnd,
    string? Amount,
    string? Unit,
    string? Currency);

public sealed record ProbeErrorInfo(string Code, string Message, string? Hint, int HttpStatus);

/// <summary>
/// Parsed form of one <c>--json</c> document. Field names follow the CLI schema.
/// </summary>
public sealed record ProbeResult
{
    public required int Schema { get; init; }

    public string? CliVersion { get; init; }

    public required string Provider { get; init; }

    public string? DisplayName { get; init; }

    public bool Ok { get; init; }

    public int HttpStatus { get; init; }

    public long LatencyMs { get; init; }

    public long ObservedAtMs { get; init; }

    public bool? Available { get; init; }

    public Capabilities Capabilities { get; init; } = Capabilities.None;

    public IReadOnlyList<ProbeMetric> Metrics { get; init; } = [];

    public string? Note { get; init; }

    public string? Account { get; init; }

    public ProbeErrorInfo? Error { get; init; }

    public string? RawResponse { get; init; }

    public DateTimeOffset? ObservedAt =>
        ObservedAtMs > 0 ? DateTimeOffset.FromUnixTimeMilliseconds(ObservedAtMs).ToLocalTime() : null;
}

public sealed record ProbeRequest(string Provider, string? ApiKey, int TimeoutMs = TokenProbeProcessClient.DefaultProbeTimeoutMs);

/// <summary>
/// Everything one CLI invocation produced. <see cref="Failure"/> is populated only
/// when the caller cannot get a usable document back from the CLI.
/// </summary>
public sealed record ProbeOutcome
{
    public ProbeResult? Result { get; init; }

    public string RawJson { get; init; } = string.Empty;

    public string StandardError { get; init; } = string.Empty;

    public int ExitCode { get; init; }

    public bool TimedOut { get; init; }

    public string? Failure { get; init; }

    public string? Warning { get; init; }

    public bool Ok => Result is { Ok: true };
}
