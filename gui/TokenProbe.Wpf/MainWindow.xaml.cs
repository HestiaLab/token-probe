using System.ComponentModel;
using System.Windows;
using TokenProbe.Wpf.ViewModels;

namespace TokenProbe.Wpf;

public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel;

    public MainWindow(MainViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;
        _viewModel.PropertyChanged += OnViewModelPropertyChanged;
    }

    private void KeyBox_OnPasswordChanged(object sender, RoutedEventArgs e)
    {
        // PasswordBox.Password is deliberately not bindable; the draft lives only in
        // memory and never reaches the command line.
        _viewModel.ApiKeyDraft = KeyBox.Password;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(MainViewModel.ApiKeyDraft))
        {
            return;
        }

        // The view model resets the draft after a successful validation; mirror that
        // into the control without re-entering the PasswordChanged handler.
        if (!string.Equals(KeyBox.Password, _viewModel.ApiKeyDraft, StringComparison.Ordinal))
        {
            KeyBox.Password = _viewModel.ApiKeyDraft;
        }
    }
}
