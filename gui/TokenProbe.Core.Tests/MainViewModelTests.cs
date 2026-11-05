using TokenProbe.Core;
using TokenProbe.Wpf.ViewModels;

namespace TokenProbe.Core.Tests;

public sealed class MainViewModelTests : IDisposable
{
    private readonly string _root;
    private readonly string _executablePath;
    private readonly string _keyDirectory;
    private readonly string _logDirectory;
    private readonly ApiKeyStore _keyStore;
    private readonly DiagnosticLog _log;
    private readonly List<TokenProbeSettings> _persisted = [];
    private readonly List<string> _openedExternal = [];
    private readonly List<string> _openedDirectories = [];
    private readonly string? _previousKey;

    public MainViewModelTests()
    {
        _root = Path.Combine(Path.GetTempPath(), "tokenprobe-vm-" + Guid.NewGuid().ToString("N"));
        _executablePath = Path.Combine(_root, "tools", "tokenprobe.exe");
        _keyDirectory = Path.Combine(_root, "keys");
        _logDirectory = Path.Combine(_root, "logs");

        Directory.CreateDirectory(Path.GetDirectoryName(_executablePath)!);
        File.WriteAllText(_executablePath, "stub");

        _keyStore = new ApiKeyStore(_keyDirectory);
        _log = new DiagnosticLog(_logDirectory);

        _previousKey = Environment.GetEnvironmentVariable("DEEPSEEK_API_KEY");
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", null);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", _previousKey);
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    [Fact]
    public void QueryIsDisabledWithoutAKey()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()));

        Assert.False(viewModel.QueryCommand.CanExecute(null));
        Assert.True(viewModel.ShowGoToSettings);
        Assert.Contains("尚未配置密钥", viewModel.QueryKeyHintText);
    }

    [Fact]
    public async Task ValidationSuccessSavesTheKeyAndPublishesTheResult()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()));
        viewModel.ApiKeyDraft = "sk-new";

        await viewModel.ValidateAndSaveCommand.ExecuteAsync(null);

        Assert.Equal("sk-new", _keyStore.Load("deepseek"));
        Assert.Equal(string.Empty, viewModel.ApiKeyDraft);
        Assert.True(viewModel.HasStoredKey);
        Assert.Equal("已验证并保存", viewModel.KeySuccessMessage);
        Assert.Contains("校验成功", viewModel.StatusText);
        Assert.NotNull(viewModel.Result);
        Assert.Equal(2, viewModel.Metrics.Count);
        Assert.True(viewModel.QueryCommand.CanExecute(null));
        Assert.Contains("已配置", viewModel.KeyPlaceholderText);
        Assert.True(viewModel.ShowSavedKeyPlaceholder);
    }

    [Fact]
    public async Task ValidationRejectionKeepsTheOldKey()
    {
        _keyStore.Save("deepseek", "sk-old");
        var viewModel = CreateViewModel(new StubClient(ErrorOutcome("AUTH")));
        viewModel.ApiKeyDraft = "sk-bad";

        await viewModel.ValidateAndSaveCommand.ExecuteAsync(null);

        Assert.Equal("sk-old", _keyStore.Load("deepseek"));
        Assert.True(viewModel.HasStoredKey);
        Assert.Contains("密钥无效", viewModel.KeyErrorMessage);
        Assert.Contains("密钥无效", viewModel.StatusText);
        Assert.Null(viewModel.KeySuccessMessage);
    }

    [Fact]
    public async Task InconclusiveValidationDoesNotSave()
    {
        var viewModel = CreateViewModel(new StubClient(new ProbeOutcome { Failure = "网络不可达" }));
        viewModel.ApiKeyDraft = "sk-untested";

        await viewModel.ValidateAndSaveCommand.ExecuteAsync(null);

        Assert.Null(_keyStore.Load("deepseek"));
        Assert.Contains("未能验证", viewModel.KeyErrorMessage);
        Assert.Contains("网络不可达", viewModel.KeyErrorMessage);
        Assert.False(viewModel.HasStoredKey);
    }

    [Fact]
    public async Task CancelledValidationDoesNotSave()
    {
        var client = new StubClient(SuccessOutcome()) { ThrowOnProbe = new OperationCanceledException() };
        var viewModel = CreateViewModel(client);
        viewModel.ApiKeyDraft = "sk-cancelled";

        await viewModel.ValidateAndSaveCommand.ExecuteAsync(null);

        Assert.Null(_keyStore.Load("deepseek"));
        Assert.Contains("已取消", viewModel.KeyErrorMessage);
        Assert.Equal("已取消", viewModel.StatusText);
    }

    [Fact]
    public async Task QueryUsesTheStoredKeyAndNeverTheDraft()
    {
        _keyStore.Save("deepseek", "sk-stored");
        var client = new StubClient(SuccessOutcome());
        var viewModel = CreateViewModel(client);
        viewModel.ApiKeyDraft = "sk-draft-not-validated";

        await viewModel.QueryCommand.ExecuteAsync(null);

        Assert.Equal("sk-stored", client.LastRequest?.ApiKey);
    }

    [Fact]
    public async Task QueryUsesSystemEnvironmentVariableAsFallback()
    {
        Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", "sk-from-environment");
        try
        {
            var client = new StubClient(SuccessOutcome());
            var viewModel = CreateViewModel(client);

            Assert.True(viewModel.QueryCommand.CanExecute(null));
            Assert.Contains("DEEPSEEK_API_KEY", viewModel.QueryKeyHintText);

            await viewModel.QueryCommand.ExecuteAsync(null);

            // No key is injected by the app; the CLI inherits the process environment.
            Assert.Null(client.LastRequest?.ApiKey);
        }
        finally
        {
            Environment.SetEnvironmentVariable("DEEPSEEK_API_KEY", null);
        }
    }

    [Fact]
    public async Task CredentialErrorOnQuerySuggestsReconfiguration()
    {
        _keyStore.Save("deepseek", "sk-revoked");
        var viewModel = CreateViewModel(new StubClient(ErrorOutcome("AUTH")));

        await viewModel.QueryCommand.ExecuteAsync(null);

        Assert.True(viewModel.HasError);
        Assert.True(viewModel.IsCredentialError);
        Assert.Contains("AUTH", viewModel.ErrorMessage!);
        Assert.Equal("失败", viewModel.StatusText);
    }

    [Fact]
    public void NavigationToggleIsPersisted()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()));
        Assert.Equal(180, viewModel.NavigationWidth);

        viewModel.ToggleNavigationCommand.Execute(null);

        Assert.Equal(48, viewModel.NavigationWidth);
        Assert.False(viewModel.IsNavigationExpanded);
        Assert.True(_persisted[^1].NavigationCollapsed);
    }

    [Fact]
    public void ProviderSelectionIsPersisted()
    {
        CreateViewModel(new StubClient(SuccessOutcome()));

        Assert.Contains(_persisted, settings => settings.LastProvider == "deepseek");
    }

    [Fact]
    public void MissingBundledExecutableDisablesCommands()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()), exePath: Path.Combine(_root, "missing", "tokenprobe.exe"));

        Assert.False(viewModel.ExecutableAvailable);
        Assert.False(viewModel.QueryCommand.CanExecute(null));
        Assert.False(viewModel.ValidateAndSaveCommand.CanExecute(null));
        Assert.Contains("安装不完整", viewModel.QueryKeyHintText);
        Assert.Contains("安装不完整", viewModel.ExecutableStatusText);
    }

    [Fact]
    public void IssueUriCarriesEnvironmentButNoProbeData()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()));

        viewModel.OpenIssueCommand.Execute(null);

        var uri = Assert.Single(_openedExternal);
        Assert.StartsWith(IssueReporter.NewIssueBaseUrl, uri);
        var body = Uri.UnescapeDataString(uri[(uri.IndexOf("body=", StringComparison.Ordinal) + 5)..]);
        Assert.Contains(_logDirectory, body);
        Assert.DoesNotContain("110.00", body);
    }

    [Fact]
    public void OpenLogFolderCreatesAndOpensTheDirectory()
    {
        var viewModel = CreateViewModel(new StubClient(SuccessOutcome()));

        viewModel.OpenLogFolderCommand.Execute(null);

        Assert.Equal(_logDirectory, Assert.Single(_openedDirectories));
        Assert.True(Directory.Exists(_logDirectory));
    }

    private MainViewModel CreateViewModel(StubClient client, TokenProbeSettings? settings = null, string? exePath = null) =>
        new(
            client,
            _keyStore,
            settings ?? new TokenProbeSettings(),
            _log,
            exePath ?? _executablePath,
            _openedExternal.Add,
            _openedDirectories.Add,
            _persisted.Add);

    private static ProbeOutcome SuccessOutcome() => new()
    {
        ExitCode = 0,
        RawJson = "{}",
        Result = new ProbeResult
        {
            Schema = 1,
            CliVersion = "0.2.0",
            Provider = "deepseek",
            DisplayName = "DeepSeek",
            Ok = true,
            HttpStatus = 200,
            LatencyMs = 12,
            ObservedAtMs = 1_760_790_000_000,
            Available = true,
            Capabilities = new Capabilities(true, false, false),
            Metrics =
            [
                new ProbeMetric("credit", "total_balance", "Total balance", null, null, "110.00", "currency", "CNY"),
                new ProbeMetric("credit", "granted_balance", "Granted balance", null, null, "10.00", "currency", "USD"),
            ],
        },
    };

    private static ProbeOutcome ErrorOutcome(string code) => new()
    {
        ExitCode = 1,
        Result = new ProbeResult
        {
            Schema = 1,
            Provider = "deepseek",
            Ok = false,
            Error = new ProbeErrorInfo(code, "provider rejected the key", "check the key", 401),
        },
    };

    private sealed class StubClient(ProbeOutcome outcome) : IProbeClient
    {
        public ProbeRequest? LastRequest { get; private set; }

        public Exception? ThrowOnProbe { get; init; }

        public Task<ProbeOutcome> ProbeAsync(ProbeRequest request, CancellationToken cancellationToken)
        {
            LastRequest = request;
            return ThrowOnProbe is null
                ? Task.FromResult(outcome)
                : Task.FromException<ProbeOutcome>(ThrowOnProbe);
        }

        public Task<string?> TryReadVersionAsync(CancellationToken cancellationToken) =>
            Task.FromResult<string?>("0.2.0");
    }
}
