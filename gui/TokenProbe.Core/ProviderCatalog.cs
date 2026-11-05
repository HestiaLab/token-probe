namespace TokenProbe.Core;

/// <summary>
/// Mirrors the provider registry compiled into the CLI. <c>--list</c> only prints a
/// fixed-width table meant for humans, so parsing it would be brittle; the catalog is
/// small and changes only when a provider is added to the CLI.
/// </summary>
public sealed record ProviderInfo(
    string Id,
    string DisplayName,
    string KeyVariable,
    Capabilities Capabilities,
    string? Notes);

public static class ProviderCatalog
{
    public static IReadOnlyList<ProviderInfo> All { get; } =
    [
        new ProviderInfo(
            "deepseek",
            "DeepSeek",
            "DEEPSEEK_API_KEY",
            new Capabilities(Credit: true, Spend: false, Quota: false),
            "仅提供余额；DeepSeek 没有用量/费用查询接口。"),
    ];

    public static ProviderInfo? Find(string? id) =>
        id is null ? null : All.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.OrdinalIgnoreCase));
}
