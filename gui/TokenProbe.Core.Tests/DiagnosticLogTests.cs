using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class DiagnosticLogTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "tokenprobe-log-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }

    [Fact]
    public void WritesAnEntryToADailyFile()
    {
        var log = new DiagnosticLog(_directory);

        log.Info("hello diagnostics");

        var file = Assert.Single(log.GetLogFiles());
        Assert.StartsWith("tokenprobe-", Path.GetFileName(file));
        Assert.EndsWith(".log", file);
        Assert.Contains("INFO hello diagnostics", File.ReadAllText(file));
    }

    [Fact]
    public void RedactsApiKeysBeforeTheyReachTheDisk()
    {
        var log = new DiagnosticLog(_directory);

        log.Info("using key sk-abcdef123456 and api_key=super-secret-value");

        var content = File.ReadAllText(Assert.Single(log.GetLogFiles()));
        Assert.DoesNotContain("sk-abcdef123456", content);
        Assert.DoesNotContain("super-secret-value", content);
        Assert.Contains("sk-***", content);
    }

    [Fact]
    public void RecordProbeCapturesMetadataAndRedactedOutput()
    {
        var log = new DiagnosticLog(_directory);
        var outcome = new ProbeOutcome
        {
            ExitCode = 1,
            RawJson = """{"schema":1,"result":{"provider":"deepseek","ok":false,"error":{"code":"AUTH","message":"bad key sk-abcdef123456"}}}""",
            StandardError = "tokenprobe: boom",
            Result = new ProbeResult
            {
                Schema = 1,
                Provider = "deepseek",
                Ok = false,
                Error = new ProbeErrorInfo("AUTH", "bad key", null, 401),
            },
        };

        log.RecordProbe("deepseek", outcome, TimeSpan.FromMilliseconds(42), "0.2.0");

        var content = File.ReadAllText(Assert.Single(log.GetLogFiles()));
        Assert.Contains("provider=deepseek", content);
        Assert.Contains("exit=1", content);
        Assert.Contains("elapsed_ms=42", content);
        Assert.Contains("error=AUTH", content);
        Assert.Contains("stderr=tokenprobe: boom", content);
        Assert.Contains("stdout=", content);
        Assert.DoesNotContain("sk-abcdef123456", content);
    }

    [Fact]
    public void KeepsOnlyTheConfiguredNumberOfFiles()
    {
        Directory.CreateDirectory(_directory);
        var baseTime = DateTime.UtcNow.AddDays(-10);
        for (var index = 1; index <= 5; index++)
        {
            var path = Path.Combine(_directory, $"tokenprobe-2020010{index}.log");
            File.WriteAllText(path, $"old {index}");
            File.SetLastWriteTimeUtc(path, baseTime.AddMinutes(index));
        }

        var log = new DiagnosticLog(_directory, maxFiles: 2);
        log.Info("newest");

        var files = log.GetLogFiles();
        Assert.Equal(2, files.Count);
        Assert.DoesNotContain(files, path => Path.GetFileName(path) == "tokenprobe-20200101.log");
        Assert.DoesNotContain(files, path => Path.GetFileName(path) == "tokenprobe-20200102.log");
        Assert.Contains(files, path => Path.GetFileName(path) == "tokenprobe-20200105.log");
    }

    [Fact]
    public void SurvivesAnUnusableDirectory()
    {
        var filePath = Path.Combine(_directory, "not-a-directory");
        Directory.CreateDirectory(_directory);
        File.WriteAllText(filePath, "x");

        var log = new DiagnosticLog(filePath);

        log.Info("must not throw");
    }
}
