using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace TokenProbe.Core;

public interface IProbeClient
{
    Task<ProbeOutcome> ProbeAsync(ProbeRequest request, CancellationToken cancellationToken);

    Task<string?> TryReadVersionAsync(CancellationToken cancellationToken);
}

/// <summary>
/// Builds the CLI invocation. Kept separate from process handling so the argument
/// and environment rules can be asserted without starting anything.
/// </summary>
public static class TokenProbeCommand
{
    public static ProcessStartInfo CreateStartInfo(string executablePath, ProbeRequest request)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);
        ArgumentNullException.ThrowIfNull(request);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.Provider);

        var startInfo = new ProcessStartInfo(executablePath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
            WorkingDirectory = Path.GetDirectoryName(executablePath) ?? Environment.CurrentDirectory,
        };

        startInfo.ArgumentList.Add("--provider");
        startInfo.ArgumentList.Add(request.Provider);
        startInfo.ArgumentList.Add("--json");
        startInfo.ArgumentList.Add("--timeout");
        startInfo.ArgumentList.Add(request.TimeoutMs.ToString(CultureInfo.InvariantCulture));

        // The key never goes on the command line: process command lines are readable
        // by other processes of the same user. The CLI already honours this variable.
        if (!string.IsNullOrEmpty(request.ApiKey))
        {
            startInfo.Environment[ProviderEnvironment.KeyVariable(request.Provider)] = request.ApiKey;
        }

        return startInfo;
    }
}

public static class ProviderEnvironment
{
    public static string KeyVariable(string providerId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(providerId);

        return providerId.Trim().ToLowerInvariant() switch
        {
            "deepseek" => "DEEPSEEK_API_KEY",
            _ => providerId.Trim().ToUpperInvariant().Replace('-', '_') + "_API_KEY",
        };
    }
}

public sealed class TokenProbeProcessClient : IProbeClient
{
    public const int DefaultProbeTimeoutMs = 15_000;

    /// <summary>Slack on top of the CLI's own per-request timeout before the process is killed.</summary>
    public static readonly TimeSpan DefaultProcessGrace = TimeSpan.FromSeconds(5);

    private readonly TimeSpan _processGrace;

    public TokenProbeProcessClient(string executablePath, TimeSpan? processGrace = null)
    {
        ExecutablePath = executablePath;
        _processGrace = processGrace ?? DefaultProcessGrace;
    }

    public string ExecutablePath { get; }

    public async Task<ProbeOutcome> ProbeAsync(ProbeRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);

        if (!File.Exists(ExecutablePath))
        {
            return new ProbeOutcome { Failure = $"找不到 tokenprobe.exe：{ExecutablePath}" };
        }

        var startInfo = TokenProbeCommand.CreateStartInfo(ExecutablePath, request);
        using var process = new Process { StartInfo = startInfo };

        try
        {
            if (!process.Start())
            {
                return new ProbeOutcome { Failure = "tokenprobe 进程未能启动。" };
            }
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return new ProbeOutcome { Failure = $"无法启动 tokenprobe.exe：{ex.Message}" };
        }

        var stdoutTask = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
        var stderrTask = process.StandardError.ReadToEndAsync(CancellationToken.None);

        var timeout = TimeSpan.FromMilliseconds(Math.Max(request.TimeoutMs, 1_000)) + _processGrace;
        using var timeoutSource = new CancellationTokenSource(timeout);
        using var linkedSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, timeoutSource.Token);

        try
        {
            await process.WaitForExitAsync(linkedSource.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            KillTree(process);

            var partialStandardError = await SafeReadAsync(stderrTask).ConfigureAwait(false);
            var partialStandardOutput = await SafeReadAsync(stdoutTask).ConfigureAwait(false);

            // A caller-initiated cancel is not a result; do not dress it up as one.
            cancellationToken.ThrowIfCancellationRequested();

            return new ProbeOutcome
            {
                TimedOut = true,
                ExitCode = -1,
                RawJson = partialStandardOutput,
                StandardError = partialStandardError,
                Failure = $"tokenprobe 在 {timeout.TotalSeconds:0.#} 秒内没有返回，进程已终止。",
            };
        }

        var standardOutput = await stdoutTask.ConfigureAwait(false);
        var standardError = await stderrTask.ConfigureAwait(false);
        var exitCode = process.ExitCode;

        if (string.IsNullOrWhiteSpace(standardOutput))
        {
            return new ProbeOutcome
            {
                ExitCode = exitCode,
                StandardError = standardError,
                Failure = DescribeEmptyOutput(exitCode, standardError),
            };
        }

        var parsed = ProbeJsonParser.Parse(standardOutput);
        if (parsed.Result is null)
        {
            var detail = string.IsNullOrWhiteSpace(standardError) ? string.Empty : $"\n{standardError.Trim()}";
            return new ProbeOutcome
            {
                ExitCode = exitCode,
                RawJson = standardOutput,
                StandardError = standardError,
                Failure = $"{parsed.Error}（退出码 {exitCode}）{detail}",
            };
        }

        return new ProbeOutcome
        {
            Result = parsed.Result,
            RawJson = standardOutput,
            StandardError = standardError,
            ExitCode = exitCode,
            Warning = parsed.Warning,
        };
    }

    public async Task<string?> TryReadVersionAsync(CancellationToken cancellationToken)
    {
        if (!File.Exists(ExecutablePath))
        {
            return null;
        }

        var startInfo = new ProcessStartInfo(ExecutablePath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
            WorkingDirectory = Path.GetDirectoryName(ExecutablePath) ?? Environment.CurrentDirectory,
        };
        startInfo.ArgumentList.Add("--version");

        using var process = new Process { StartInfo = startInfo };
        try
        {
            if (!process.Start())
            {
                return null;
            }
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return null;
        }

        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(TimeSpan.FromSeconds(10));

        try
        {
            await process.WaitForExitAsync(timeoutSource.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            KillTree(process);
            return null;
        }

        var output = await process.StandardOutput.ReadToEndAsync(CancellationToken.None).ConfigureAwait(false);
        _ = await process.StandardError.ReadToEndAsync(CancellationToken.None).ConfigureAwait(false);

        // Expected shape: "tokenprobe 0.2.0 — probe AI provider credit / spend / quota"
        var firstLine = output.Split('\n', 2)[0].Trim();
        var parts = firstLine.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length >= 2 ? parts[1] : firstLine;
    }

    private static string DescribeEmptyOutput(int exitCode, string standardError)
    {
        var reason = exitCode switch
        {
            0 => "tokenprobe 正常退出，但没有输出任何内容。",
            2 => "tokenprobe 报告用法错误（退出码 2）。",
            _ => $"tokenprobe 以退出码 {exitCode} 结束，但没有输出任何内容。",
        };

        return string.IsNullOrWhiteSpace(standardError) ? reason : $"{reason}\n{standardError.Trim()}";
    }

    private static async Task<string> SafeReadAsync(Task<string> task)
    {
        try
        {
            return await task.ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            return string.Empty;
        }
    }

    private static void KillTree(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (Exception ex) when (ex is InvalidOperationException or NotSupportedException or System.ComponentModel.Win32Exception)
        {
            // The process already exited or cannot be killed; nothing useful left to do.
        }
    }
}
