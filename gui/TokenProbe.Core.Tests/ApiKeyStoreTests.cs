using System.Text;
using TokenProbe.Core;

namespace TokenProbe.Core.Tests;

public sealed class ApiKeyStoreTests : IDisposable
{
    private readonly string _directory;
    private readonly ApiKeyStore _store;

    public ApiKeyStoreTests()
    {
        _directory = Path.Combine(Path.GetTempPath(), "tokenprobe-keys-" + Guid.NewGuid().ToString("N"));
        _store = new ApiKeyStore(_directory);
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }

    [Fact]
    public void RoundTripsAKeyThroughDpapi()
    {
        _store.Save("deepseek", "sk-round-trip");

        Assert.True(_store.Has("deepseek"));
        Assert.Equal("sk-round-trip", _store.Load("deepseek"));

        // What lands on disk must not contain the plaintext key.
        var bytes = File.ReadAllBytes(Directory.GetFiles(_directory).Single());
        Assert.DoesNotContain("sk-round-trip", Encoding.UTF8.GetString(bytes));
    }

    [Fact]
    public void OverwritesAnExistingKey()
    {
        _store.Save("deepseek", "first");
        _store.Save("deepseek", "second");

        Assert.Equal("second", _store.Load("deepseek"));
        Assert.Single(Directory.GetFiles(_directory));
    }

    [Fact]
    public void DeletesAKey()
    {
        _store.Save("deepseek", "sk-delete-me");

        Assert.True(_store.Delete("deepseek"));
        Assert.False(_store.Has("deepseek"));
        Assert.Null(_store.Load("deepseek"));
        Assert.False(_store.Delete("deepseek"));
    }

    [Fact]
    public void TreatsCorruptFilesAsMissing()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllBytes(Path.Combine(_directory, "deepseek.bin"), [1, 2, 3, 4]);

        Assert.Null(_store.Load("deepseek"));
    }

    [Fact]
    public void KeepsProviderIdsInsideTheStoreDirectory()
    {
        _store.Save("../evil", "sk-evil");

        var files = Directory.GetFiles(_directory);
        Assert.Single(files);
        Assert.Equal(_directory, Path.GetDirectoryName(files[0]));
    }

    [Fact]
    public void ReturnsNullForUnknownProvider()
    {
        Assert.Null(_store.Load("unknown"));
    }
}
