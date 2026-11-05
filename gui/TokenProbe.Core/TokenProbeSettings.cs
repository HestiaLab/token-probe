using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace TokenProbe.Core;

public sealed record TokenProbeSettings
{
    public string? LastProvider { get; init; }

    public bool NavigationCollapsed { get; init; }

    public static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "TokenProbe",
        "settings.json");

    public static TokenProbeSettings Load(string? path = null)
    {
        path ??= DefaultPath;
        if (!File.Exists(path))
        {
            return new TokenProbeSettings();
        }

        try
        {
            return JsonSerializer.Deserialize<TokenProbeSettings>(File.ReadAllText(path)) ?? new TokenProbeSettings();
        }
        catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException)
        {
            return new TokenProbeSettings();
        }
    }

    public void Save(string? path = null)
    {
        path ??= DefaultPath;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}

/// <summary>
/// Local diagnostic log used when a user files a GitHub issue. Only the last few
/// files are kept, entries are bounded in size, and anything resembling an API key
/// is redacted before it reaches the disk.
/// </summary>
public sealed partial class DiagnosticLog
{
    public const int DefaultMaxFiles = 5;
    public const long DefaultMaxBytes = 512 * 1024;

    private readonly object _gate = new();
    private readonly string _directory;
    private readonly int _maxFiles;
    private readonly long _maxBytes;
    private readonly TimeProvider _timeProvider;

    public DiagnosticLog(string? directory = null, int maxFiles = DefaultMaxFiles, long maxBytes = DefaultMaxBytes, TimeProvider? timeProvider = null)
    {
        _directory = directory ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "TokenProbe",
            "logs");
        _maxFiles = Math.Max(1, maxFiles);
        _maxBytes = Math.Max(1024, maxBytes);
        _timeProvider = timeProvider ?? TimeProvider.System;
    }

    public string DirectoryPath => _directory;

    public string? CurrentFilePath
    {
        get
        {
            var files = GetLogFiles();
            return files.Count == 0 ? null : files[^1];
        }
    }

    public void Info(string message)
    {
        Write("INFO", message);
    }

    public void RecordProbe(string provider, ProbeOutcome outcome, TimeSpan duration, string? cliVersion)
    {
        ArgumentNullException.ThrowIfNull(outcome);

        var builder = new StringBuilder();
        builder.Append(CultureInfo.InvariantCulture, $"provider={provider} exit={outcome.ExitCode} elapsed_ms={(long)duration.TotalMilliseconds}");
        builder.Append(CultureInfo.InvariantCulture, $" timed_out={outcome.TimedOut}");
        builder.Append(CultureInfo.InvariantCulture, $" cli={cliVersion ?? "unknown"}");

        if (outcome.Result?.Error is { } error)
        {
            builder.Append(CultureInfo.InvariantCulture, $" error={error.Code} http={error.HttpStatus}");
        }

        if (outcome.Failure is not null)
        {
            builder.Append(CultureInfo.InvariantCulture, $" failure={outcome.Failure}");
        }

        if (!string.IsNullOrWhiteSpace(outcome.StandardError))
        {
            builder.Append(CultureInfo.InvariantCulture, $" stderr={outcome.StandardError.Trim()}");
        }

        if (!string.IsNullOrWhiteSpace(outcome.RawJson))
        {
            builder.Append(CultureInfo.InvariantCulture, $" stdout={outcome.RawJson.Trim()}");
        }

        Write("PROBE", builder.ToString());
    }

    public IReadOnlyList<string> GetLogFiles()
    {
        if (!Directory.Exists(_directory))
        {
            return [];
        }

        return Directory.GetFiles(_directory, "tokenprobe-*.log")
            .OrderBy(File.GetLastWriteTimeUtc)
            .ThenBy(path => path, StringComparer.Ordinal)
            .ToArray();
    }

    /// <summary>
    /// Defensive redaction: nothing should ever put a key in a log message, but a
    /// single slip must not turn into a credential leak on disk.
    /// </summary>
    public static string Redact(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        var redacted = ApiKeyPattern().Replace(text, "sk-***");
        return SecretValuePattern().Replace(redacted, "$1***");
    }

    private void Write(string level, string message)
    {
        var line = Redact(
            $"{_timeProvider.GetUtcNow().ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture)} {level} {message}{Environment.NewLine}");

        lock (_gate)
        {
            try
            {
                Directory.CreateDirectory(_directory);
                var path = PathForToday();
                if (File.Exists(path) && new FileInfo(path).Length >= _maxBytes)
                {
                    path = RotateOverflowFile(path);
                }

                File.AppendAllText(path, line, new UTF8Encoding(false));
                TrimOldFiles();
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
            {
                // Diagnostics must never take the application down.
            }
        }
    }

    private string PathForToday() =>
        Path.Combine(_directory, $"tokenprobe-{_timeProvider.GetUtcNow():yyyyMMdd}.log");

    private string RotateOverflowFile(string path)
    {
        var index = 1;
        string candidate;
        do
        {
            candidate = Path.Combine(_directory, $"{Path.GetFileNameWithoutExtension(path)}-{index}{Path.GetExtension(path)}");
            index++;
        }
        while (File.Exists(candidate) && new FileInfo(candidate).Length >= _maxBytes && index < 1000);

        return candidate;
    }

    private void TrimOldFiles()
    {
        var files = GetLogFiles();
        var excess = files.Count - _maxFiles;
        for (var i = 0; i < excess; i++)
        {
            try
            {
                File.Delete(files[i]);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                return;
            }
        }
    }

    [GeneratedRegex(@"sk-[A-Za-z0-9_\-]{4,}")]
    private static partial Regex ApiKeyPattern();

    [GeneratedRegex(@"(?i)(api[-_ ]?key\s*[=:]\s*)\S+")]
    private static partial Regex SecretValuePattern();
}

/// <summary>
/// Builds the prefilled GitHub issue URL. The body carries environment facts only:
/// logs and balances stay on the machine and are attached by the user.
/// </summary>
public static class IssueReporter
{
    public const string NewIssueBaseUrl = "https://github.com/HestiaLab/token-probe/issues/new";

    public static string BuildUri(
        string appVersion,
        string cliVersion,
        string operatingSystem,
        string logDirectory)
    {
        var body = string.Join('\n',
        [
            "### 环境",
            string.Empty,
            $"- TokenProbe WPF：{appVersion}",
            $"- tokenprobe CLI：{cliVersion}",
            $"- 操作系统：{operatingSystem}",
            $"- 日志目录：{logDirectory}",
            string.Empty,
            "### 问题描述",
            string.Empty,
            "（请在此描述你遇到的问题）",
            string.Empty,
            "> 请将日志目录中最新的 `tokenprobe-*.log` 文件附加到此 issue。",
        ]);

        return $"{NewIssueBaseUrl}?title={Uri.EscapeDataString("bug: ")}&body={Uri.EscapeDataString(body)}";
    }
}
