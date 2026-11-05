using System.Windows;
using System.Windows.Threading;
using TokenProbe.Core;
using TokenProbe.Wpf.ViewModels;

namespace TokenProbe.Wpf;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        DispatcherUnhandledException += OnDispatcherUnhandledException;

        var settings = TokenProbeSettings.Load();
        var log = new DiagnosticLog();
        log.Info(
            $"app start; app={typeof(App).Assembly.GetName().Version}; " +
            $"os={Environment.OSVersion.VersionString}; runtime={Environment.Version}; " +
            $"exe={TokenProbeLocator.DefaultPath()}");

        var client = new TokenProbeProcessClient(TokenProbeLocator.DefaultPath());
        var viewModel = new MainViewModel(client, new ApiKeyStore(), settings, log);
        var window = new MainWindow(viewModel);
        MainWindow = window;
        window.Show();
    }

    private static void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        MessageBox.Show(
            $"发生未处理的错误：\n{e.Exception.Message}",
            "TokenProbe",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
        e.Handled = true;
    }
}
