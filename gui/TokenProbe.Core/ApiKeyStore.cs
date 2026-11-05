using System.Security.Cryptography;
using System.Text;

namespace TokenProbe.Core;

/// <summary>
/// Stores API keys encrypted with DPAPI under the current user. The file is only
/// readable by the same Windows account on the same machine; a copied file is useless.
/// </summary>
public sealed class ApiKeyStore
{
    private readonly string _directory;

    public ApiKeyStore(string? directory = null)
    {
        _directory = directory ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "TokenProbe",
            "keys");
    }

    public string? Load(string provider)
    {
        var path = PathFor(provider);
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            var protectedBytes = File.ReadAllBytes(path);
            var plainBytes = ProtectedData.Unprotect(protectedBytes, optionalEntropy: null, DataProtectionScope.CurrentUser);
            var key = Encoding.UTF8.GetString(plainBytes).Trim();
            return key.Length == 0 ? null : key;
        }
        catch (Exception ex) when (ex is CryptographicException or IOException or UnauthorizedAccessException)
        {
            // A key from another user/machine simply cannot be read: treat it as absent.
            return null;
        }
    }

    public void Save(string provider, string apiKey)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(apiKey);

        Directory.CreateDirectory(_directory);
        var plainBytes = Encoding.UTF8.GetBytes(apiKey.Trim());
        var protectedBytes = ProtectedData.Protect(plainBytes, optionalEntropy: null, DataProtectionScope.CurrentUser);
        var path = PathFor(provider);

        var temporaryPath = path + ".tmp";
        File.WriteAllBytes(temporaryPath, protectedBytes);
        File.Move(temporaryPath, path, overwrite: true);
    }

    public bool Delete(string provider)
    {
        var path = PathFor(provider);
        if (!File.Exists(path))
        {
            return false;
        }

        File.Delete(path);
        return true;
    }

    public bool Has(string provider) => File.Exists(PathFor(provider));

    private string PathFor(string provider)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(provider);

        var safe = new StringBuilder(provider.Length);
        foreach (var character in provider.Trim().ToLowerInvariant())
        {
            safe.Append(char.IsAsciiLetterOrDigit(character) || character is '-' or '_' ? character : '_');
        }

        return Path.Combine(_directory, safe + ".bin");
    }
}
