namespace TokenProbe.FakeProbe;

/// <summary>
/// Stand-in for tokenprobe.exe used by integration tests. It reproduces the CLI's
/// observable contract (arguments, exit codes, stdout/stderr split, UTF-8) without
/// touching the network. Behaviour is selected with FAKE_PROBE_MODE.
/// </summary>
internal static class Program
{
    private const string ErrorTemplate = """
        {"schema":1,"version":"0.2.0","result":{"provider":"__PROVIDER__","ok":false,"error":{"code":"INVALID_KEY","http_status":0,"message":"no API key for DeepSeek","hint":"set DEEPSEEK_API_KEY or pass --api-key"}}}
        """;

    private const string Utf8Template = """
        {"schema":1,"version":"0.2.0","result":{"provider":"__PROVIDER__","display_name":"深度求索","ok":true,"http_status":200,"latency_ms":12,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":false,"quota":false},"metrics":[{"kind":"credit","id":"total_balance","label":"总余额","period_start":null,"period_end":null,"amount":"110.00","unit":"currency","currency":"CNY"}],"note":"多个币种分别列出，不应相加"}}
        """;

    private const string FutureSchemaTemplate = """
        {"schema":2,"version":"0.3.0","result":{"provider":"__PROVIDER__","ok":true,"http_status":200,"latency_ms":5,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":true,"quota":false},"metrics":[],"unknown_field":42}}
        """;

    private const string SuccessTemplate = """
        {"schema":1,"version":"0.2.0","result":{"provider":"__PROVIDER__","display_name":"DeepSeek","ok":true,"http_status":200,"latency_ms":12,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":false,"quota":false},"metrics":[{"kind":"credit","id":"total_balance","label":"Total balance","period_start":null,"period_end":null,"amount":"110.00","unit":"currency","currency":"CNY"},{"kind":"credit","id":"granted_balance","label":"Granted balance","period_start":null,"period_end":null,"amount":"10.00","unit":"currency","currency":"USD"}],"account":"__KEY_STATE__","note":"multiple currencies reported"}}
        """;

    private static async Task<int> Main(string[] args)
    {
        // Mirror the real CLI, which switches the console to UTF-8 before writing.
        Console.OutputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

        var mode = Environment.GetEnvironmentVariable("FAKE_PROBE_MODE") ?? "success";

        if (args.Contains("--version"))
        {
            Console.Out.WriteLine("tokenprobe 0.2.0 — probe AI provider credit / spend / quota");
            return 0;
        }

        var provider = ReadOption(args, "--provider") ?? "deepseek";
        var usesJson = args.Contains("--json");
        var keyVariable = provider.ToUpperInvariant() + "_API_KEY";
        var hasKey = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable(keyVariable));

        if (!usesJson)
        {
            Console.Out.WriteLine($"{provider}: success (human readable)");
            return 0;
        }

        switch (mode)
        {
            case "error":
                Console.Out.WriteLine(ErrorTemplate.Replace("__PROVIDER__", provider));
                return 1;

            case "garbage":
                Console.Out.WriteLine("this is not json");
                Console.Error.WriteLine("tokenprobe: internal failure");
                return 1;

            case "empty":
                Console.Error.WriteLine("tokenprobe: silent failure");
                return 3;

            case "stderr-only":
                Console.Error.WriteLine("tokenprobe: cannot reach provider");
                return 1;

            case "hang":
                await Task.Delay(TimeSpan.FromMinutes(5));
                return 0;

            case "utf8":
                Console.Out.WriteLine(Utf8Template.Replace("__PROVIDER__", provider));
                return 0;

            case "future-schema":
                Console.Out.WriteLine(FutureSchemaTemplate.Replace("__PROVIDER__", provider));
                return 0;

            case "success":
            default:
                Console.Out.WriteLine(
                    SuccessTemplate
                        .Replace("__PROVIDER__", provider)
                        .Replace("__KEY_STATE__", hasKey ? "key-present" : "no-key"));
                return 0;
        }
    }

    private static string? ReadOption(string[] args, string name)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
    }
}
