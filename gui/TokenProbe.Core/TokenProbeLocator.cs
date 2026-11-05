namespace TokenProbe.Core;

/// <summary>
/// The CLI ships inside the application. There is deliberately no user-selectable
/// path and no environment override: a missing file means the installation is broken.
/// </summary>
public static class TokenProbeLocator
{
    public const string FileName = "tokenprobe.exe";

    public const string ToolsDirectoryName = "tools";

    public static string DefaultPath(string? baseDirectory = null) =>
        Path.Combine(baseDirectory ?? AppContext.BaseDirectory, ToolsDirectoryName, FileName);

    public static bool Exists(string? baseDirectory = null) => File.Exists(DefaultPath(baseDirectory));
}
