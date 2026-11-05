using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TokenProbe.Core;

namespace TokenProbe.Wpf.ViewModels;

public enum AppPage
{
    Quota,
    Settings,
}

public sealed record MetricRow(string KindText, string Label, string AmountText, string PeriodText, string Id);

public sealed partial class MainViewModel : ObservableObject
{
    private readonly ApiKeyStore _keyStore;
    private readonly DiagnosticLog _log;
    private readonly IProbeClient _client;
    private readonly Action<string> _openExternal;
    private readonly Action<string> _openDirectory;
    private readonly Dictionary<string, string> _drafts = new(StringComparer.OrdinalIgnoreCase);
    private readonly string _executablePath;
    private readonly Action<TokenProbeSettings> _persistSettings;

    private TokenProbeSettings _settings;
    private string? _cliVersion;

    public MainViewModel(
        IProbeClient client,
        ApiKeyStore keyStore,
        TokenProbeSettings settings,
        DiagnosticLog log,
        string? executablePath = null,
        Action<string>? openExternal = null,
        Action<string>? openDirectory = null,
        Action<TokenProbeSettings>? persistSettings = null)
    {
        _client = client;
        _keyStore = keyStore;
        _log = log;
        _settings = settings;
        _executablePath = executablePath ?? TokenProbeLocator.DefaultPath();
        _openExternal = openExternal ?? (uri => Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true }));
        _openDirectory = openDirectory ?? (path => Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true }));
        _persistSettings = persistSettings ?? (value => value.Save());

        _navigationCollapsed = settings.NavigationCollapsed;
        SelectedProvider = ProviderCatalog.Find(settings.LastProvider) ?? Providers[0];
        ConfiguredProvider = SelectedProvider;

        RefreshExecutableState();
        RefreshKeyState();
        _ = RefreshVersionAsync();
    }

    public IReadOnlyList<ProviderInfo> Providers { get; } = ProviderCatalog.All;

    public ObservableCollection<MetricRow> Metrics { get; } = [];

    public string AppVersion { get; } = ResolveAppVersion();

    public string LogDirectory => _log.DirectoryPath;

    // ---------------------------------------------------------------- navigation

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(NavigationWidth))]
    [NotifyPropertyChangedFor(nameof(IsNavigationExpanded))]
    private bool _navigationCollapsed;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsQuotaPage))]
    [NotifyPropertyChangedFor(nameof(IsSettingsPage))]
    private AppPage _currentPage;

    public double NavigationWidth => NavigationCollapsed ? 48 : 180;

    public bool IsNavigationExpanded => !NavigationCollapsed;

    public bool IsQuotaPage => CurrentPage == AppPage.Quota;

    public bool IsSettingsPage => CurrentPage == AppPage.Settings;

    // ----------------------------------------------------------------- providers

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(QueryCommand))]
    [NotifyPropertyChangedFor(nameof(QueryKeyHintText))]
    [NotifyPropertyChangedFor(nameof(ShowGoToSettings))]
    private ProviderInfo? _selectedProvider;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(ValidateAndSaveCommand))]
    private ProviderInfo? _configuredProvider;

    // --------------------------------------------------------------------- state

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(QueryCommand))]
    [NotifyCanExecuteChangedFor(nameof(ValidateAndSaveCommand))]
    private bool _isBusy;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(QueryCommand))]
    [NotifyCanExecuteChangedFor(nameof(ValidateAndSaveCommand))]
    private bool _isValidating;

    [ObservableProperty]
    private string _statusText = "就绪";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasError))]
    private string? _errorMessage;

    [ObservableProperty]
    private bool _isCredentialError;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    private string? _warningMessage;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasKeyError))]
    private string? _keyErrorMessage;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasKeySuccess))]
    private string? _keySuccessMessage;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasResult))]
    [NotifyPropertyChangedFor(nameof(ResultTitle))]
    [NotifyPropertyChangedFor(nameof(MetaText))]
    [NotifyPropertyChangedFor(nameof(CapabilitiesText))]
    private ProbeResult? _result;

    [ObservableProperty]
    private string _cliVersionText = "—";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(QueryCommand))]
    [NotifyCanExecuteChangedFor(nameof(ValidateAndSaveCommand))]
    private bool _executableAvailable;

    [ObservableProperty]
    private string _executableStatusText = string.Empty;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowSavedKeyPlaceholder))]
    [NotifyPropertyChangedFor(nameof(KeyPlaceholderText))]
    [NotifyCanExecuteChangedFor(nameof(ValidateAndSaveCommand))]
    private string _apiKeyDraft = string.Empty;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowSavedKeyPlaceholder))]
    [NotifyPropertyChangedFor(nameof(KeyPlaceholderText))]
    [NotifyCanExecuteChangedFor(nameof(QueryCommand))]
    private bool _hasStoredKey;

    [ObservableProperty]
    private string _keyStatusText = string.Empty;

    public bool HasResult => Result is not null;

    public bool HasError => !string.IsNullOrWhiteSpace(ErrorMessage);

    public bool HasWarning => !string.IsNullOrWhiteSpace(WarningMessage);

    public bool HasKeyError => !string.IsNullOrWhiteSpace(KeyErrorMessage);

    public bool HasKeySuccess => !string.IsNullOrWhiteSpace(KeySuccessMessage);

    public bool ShowSavedKeyPlaceholder => HasStoredKey && string.IsNullOrEmpty(ApiKeyDraft);

    public string KeyPlaceholderText => HasStoredKey ? "●●●●●● 已配置（输入新密钥以覆盖）" : "输入 API Key";

    public string QueryKeyHintText
    {
        get
        {
            if (!ExecutableAvailable)
            {
                return $"安装不完整：缺少 {TokenProbeLocator.ToolsDirectoryName}\\{TokenProbeLocator.FileName}";
            }

            if (SelectedProvider is null)
            {
                return string.Empty;
            }

            if (_keyStore.Has(SelectedProvider.Id))
            {
                return "使用已保存的密钥";
            }

            return HasSystemKey(SelectedProvider.Id)
                ? $"使用系统环境变量 {ProviderEnvironment.KeyVariable(SelectedProvider.Id)}"
                : "尚未配置密钥，请前往设置页校验并保存";
        }
    }

    public bool ShowGoToSettings => ExecutableAvailable && SelectedProvider is not null && !HasUsableKey(SelectedProvider);

    public string ResultTitle => Result is null
        ? string.Empty
        : $"{Result.DisplayName ?? Result.Provider} · {(Result.Ok ? "成功" : "失败")}";

    public string CapabilitiesText
    {
        get
        {
            if (Result is null)
            {
                return string.Empty;
            }

            var capabilities = Result.Capabilities;
            return $"能力：余额 {(capabilities.Credit ? "✓" : "✗")}　花费 {(capabilities.Spend ? "✓" : "✗")}　配额 {(capabilities.Quota ? "✓" : "✗")}";
        }
    }

    public string MetaText
    {
        get
        {
            if (Result is null)
            {
                return string.Empty;
            }

            var observed = Result.ObservedAt?.ToString("yyyy-MM-dd HH:mm:ss") ?? "—";
            var availability = Result.Available switch
            {
                true => "可用",
                false => "不可用",
                null => "未报告",
            };

            return $"HTTP {Result.HttpStatus}　延迟 {Result.LatencyMs} ms　观测时间 {observed}　状态 {availability}";
        }
    }

    // ------------------------------------------------------------------ commands

    [RelayCommand(CanExecute = nameof(CanQuery), IncludeCancelCommand = true)]
    private async Task QueryAsync(CancellationToken cancellationToken)
    {
        if (SelectedProvider is null)
        {
            return;
        }

        var provider = SelectedProvider;
        IsBusy = true;
        StatusText = "查询中…";
        ErrorMessage = null;
        IsCredentialError = false;
        WarningMessage = null;
        Result = null;
        Metrics.Clear();

        try
        {
            var storedKey = _keyStore.Load(provider.Id);
            var stopwatch = Stopwatch.StartNew();
            var outcome = await _client
                .ProbeAsync(new ProbeRequest(provider.Id, string.IsNullOrEmpty(storedKey) ? null : storedKey), cancellationToken)
                .ConfigureAwait(true);
            stopwatch.Stop();

            _log.RecordProbe(provider.Id, outcome, stopwatch.Elapsed, _cliVersion);
            WarningMessage = outcome.Warning;

            if (outcome.Failure is not null)
            {
                ErrorMessage = outcome.Failure;
                StatusText = outcome.TimedOut ? "超时" : "失败";
                return;
            }

            ApplyResult(outcome.Result);

            if (outcome.Result is { Ok: true })
            {
                StatusText = "成功";
                return;
            }

            ShowStructuredError(outcome.Result);
            StatusText = "失败";
        }
        catch (OperationCanceledException)
        {
            StatusText = "已取消";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool CanQuery() =>
        !IsBusy && !IsValidating && ExecutableAvailable && SelectedProvider is not null && HasUsableKey(SelectedProvider);

    [RelayCommand(CanExecute = nameof(CanValidate), IncludeCancelCommand = true)]
    private async Task ValidateAndSaveAsync(CancellationToken cancellationToken)
    {
        if (ConfiguredProvider is null || string.IsNullOrWhiteSpace(ApiKeyDraft))
        {
            return;
        }

        var provider = ConfiguredProvider;
        var candidateKey = ApiKeyDraft.Trim();
        IsValidating = true;
        KeyErrorMessage = null;
        KeySuccessMessage = null;
        StatusText = "校验中…";

        try
        {
            var stopwatch = Stopwatch.StartNew();
            var outcome = await _client
                .ProbeAsync(new ProbeRequest(provider.Id, candidateKey), cancellationToken)
                .ConfigureAwait(true);
            stopwatch.Stop();

            _log.RecordProbe(provider.Id, outcome, stopwatch.Elapsed, _cliVersion);

            switch (ProbeFailureClassifier.Classify(outcome))
            {
                case CredentialStatus.Valid:
                    _keyStore.Save(provider.Id, candidateKey);
                    _drafts.Remove(provider.Id);
                    ApiKeyDraft = string.Empty;
                    RefreshKeyState();
                    KeySuccessMessage = "已验证并保存";

                    if (SelectedProvider?.Id == provider.Id)
                    {
                        ApplyResult(outcome.Result);
                    }

                    StatusText = "校验成功，密钥已保存";
                    break;

                case CredentialStatus.Invalid:
                    KeyErrorMessage = DescribeCredentialFailure(outcome);
                    StatusText = "密钥无效，未保存";
                    break;

                default:
                    KeyErrorMessage = $"未能验证，密钥未保存。{DescribeInconclusiveFailure(outcome)}";
                    StatusText = "未能验证";
                    break;
            }
        }
        catch (OperationCanceledException)
        {
            KeyErrorMessage = "已取消，密钥未保存。";
            StatusText = "已取消";
        }
        finally
        {
            IsValidating = false;
        }
    }

    private bool CanValidate() =>
        !IsBusy && !IsValidating && ExecutableAvailable && ConfiguredProvider is not null && !string.IsNullOrWhiteSpace(ApiKeyDraft);

    [RelayCommand]
    private void ShowQuotaPage() => CurrentPage = AppPage.Quota;

    [RelayCommand]
    private void ShowSettingsPage() => CurrentPage = AppPage.Settings;

    [RelayCommand]
    private void ToggleNavigation() => NavigationCollapsed = !NavigationCollapsed;

    [RelayCommand]
    private void OpenIssue()
    {
        var uri = IssueReporter.BuildUri(
            AppVersion,
            _cliVersion ?? "未知",
            Environment.OSVersion.VersionString,
            _log.DirectoryPath);

        try
        {
            _openExternal(uri);
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            StatusText = $"无法打开浏览器：{ex.Message}";
        }
    }

    [RelayCommand]
    private void OpenLogFolder()
    {
        try
        {
            Directory.CreateDirectory(_log.DirectoryPath);
            _openDirectory(_log.DirectoryPath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            StatusText = $"无法打开日志目录：{ex.Message}";
        }
    }

    // ------------------------------------------------------------------- helpers

    partial void OnSelectedProviderChanged(ProviderInfo? value)
    {
        if (value is not null && _settings.LastProvider != value.Id)
        {
            _settings = _settings with { LastProvider = value.Id };
            _persistSettings(_settings);
        }

        OnPropertyChanged(nameof(QueryKeyHintText));
        OnPropertyChanged(nameof(ShowGoToSettings));
    }

    partial void OnConfiguredProviderChanged(ProviderInfo? value)
    {
        ApiKeyDraft = value is not null && _drafts.TryGetValue(value.Id, out var draft) ? draft : string.Empty;
        RefreshKeyState();
    }

    partial void OnApiKeyDraftChanged(string value)
    {
        if (ConfiguredProvider is not null)
        {
            _drafts[ConfiguredProvider.Id] = value;
        }
    }

    partial void OnNavigationCollapsedChanged(bool value)
    {
        _settings = _settings with { NavigationCollapsed = value };
        _persistSettings(_settings);
    }

    partial void OnExecutableAvailableChanged(bool value)
    {
        OnPropertyChanged(nameof(QueryKeyHintText));
        OnPropertyChanged(nameof(ShowGoToSettings));
    }

    private void ApplyResult(ProbeResult? result)
    {
        Result = result;
        Metrics.Clear();

        if (result is null)
        {
            return;
        }

        foreach (var metric in result.Metrics)
        {
            Metrics.Add(new MetricRow(
                KindText(metric.Kind),
                metric.Label ?? metric.Id ?? "—",
                AmountText(metric),
                PeriodText(metric),
                metric.Id ?? string.Empty));
        }
    }

    private void ShowStructuredError(ProbeResult? result)
    {
        var error = result?.Error;
        if (error is null)
        {
            ErrorMessage = "探测失败，tokenprobe 未给出错误详情。";
            return;
        }

        ErrorMessage = string.IsNullOrWhiteSpace(error.Hint)
            ? $"{error.Code}：{error.Message}"
            : $"{error.Code}：{error.Message}\n提示：{error.Hint}";

        IsCredentialError = error.Code is "INVALID_KEY" or "AUTH" or "FORBIDDEN";
    }

    private static string DescribeCredentialFailure(ProbeOutcome outcome)
    {
        var error = outcome.Result?.Error;
        return error is null
            ? "密钥无效。"
            : string.IsNullOrWhiteSpace(error.Hint)
                ? $"密钥无效：{error.Code}：{error.Message}"
                : $"密钥无效：{error.Code}：{error.Message}\n提示：{error.Hint}";
    }

    private static string DescribeInconclusiveFailure(ProbeOutcome outcome)
    {
        if (outcome.Failure is not null)
        {
            return outcome.Failure;
        }

        var error = outcome.Result?.Error;
        if (error is not null)
        {
            return $"{error.Code}：{error.Message}";
        }

        return "未收到可判定的响应。";
    }

    private void RefreshExecutableState()
    {
        ExecutableAvailable = File.Exists(_executablePath);
        ExecutableStatusText = ExecutableAvailable
            ? _executablePath
            : $"安装不完整：缺少 {TokenProbeLocator.ToolsDirectoryName}\\{TokenProbeLocator.FileName}";
    }

    private void RefreshKeyState()
    {
        HasStoredKey = ConfiguredProvider is not null && _keyStore.Has(ConfiguredProvider.Id);
        KeyStatusText = BuildKeyStatusText();
        OnPropertyChanged(nameof(QueryKeyHintText));
        OnPropertyChanged(nameof(ShowGoToSettings));
        QueryCommand.NotifyCanExecuteChanged();
    }

    private string BuildKeyStatusText()
    {
        if (ConfiguredProvider is null)
        {
            return string.Empty;
        }

        if (_keyStore.Has(ConfiguredProvider.Id))
        {
            return "已保存并验证。如需更换，输入新密钥并再次校验。";
        }

        return HasSystemKey(ConfiguredProvider.Id)
            ? $"尚未保存密钥；查询时会回退到系统环境变量 {ProviderEnvironment.KeyVariable(ConfiguredProvider.Id)}。"
            : "尚未保存密钥。输入后点击「校验并保存」。";
    }

    private bool HasUsableKey(ProviderInfo provider) =>
        _keyStore.Has(provider.Id) || HasSystemKey(provider.Id);

    private static bool HasSystemKey(string providerId) =>
        !string.IsNullOrEmpty(Environment.GetEnvironmentVariable(ProviderEnvironment.KeyVariable(providerId)));

    private static string KindText(string kind) => kind switch
    {
        "credit" => "余额",
        "spend" => "花费",
        "quota" => "配额",
        _ => kind,
    };

    /// <summary>Amount stays the exact text the provider returned; it is never re-formatted or summed.</summary>
    private static string AmountText(ProbeMetric metric)
    {
        var amount = metric.Amount ?? "—";
        if (!string.IsNullOrWhiteSpace(metric.Currency))
        {
            return $"{amount} {metric.Currency}";
        }

        return string.IsNullOrWhiteSpace(metric.Unit) ? amount : $"{amount} {metric.Unit}";
    }

    private static string PeriodText(ProbeMetric metric)
    {
        if (metric.PeriodStart is null && metric.PeriodEnd is null)
        {
            return "—";
        }

        return $"{metric.PeriodStart ?? "…"} → {metric.PeriodEnd ?? "…"}";
    }

    private async Task RefreshVersionAsync()
    {
        try
        {
            var version = await _client.TryReadVersionAsync(CancellationToken.None).ConfigureAwait(true);
            _cliVersion = version;
            CliVersionText = version ?? "未知";

            if (version is null && ExecutableAvailable)
            {
                ExecutableStatusText = $"无法运行：{_executablePath}";
            }
        }
        catch (Exception ex)
        {
            CliVersionText = "未知";
            ExecutableStatusText = $"读取版本失败：{ex.Message}";
        }
    }

    private static string ResolveAppVersion() =>
        typeof(MainViewModel).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? typeof(MainViewModel).Assembly.GetName().Version?.ToString()
        ?? "未知";
}
