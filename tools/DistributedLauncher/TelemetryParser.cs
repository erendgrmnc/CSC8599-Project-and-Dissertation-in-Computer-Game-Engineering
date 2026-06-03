namespace DistributedLauncher;

// One parsed stdout line from a role (or a forwarded game-server line).
public sealed class ParsedLine
{
    public string? Tag;                // e.g. "server 0" from a "[server 0] " prefix, else null
    public string Text = "";           // the line with any "[tag] " prefix stripped
    public bool IsStat;                // true when the (de-tagged) line is "@@STAT ..."
    public string Role = "";           // STAT role= field
    public string Id = "";             // STAT id= field
    public Dictionary<string, string> Fields = new();
}

public static class TelemetryParser
{
    private const string StatMarker = "@@STAT ";

    public static ParsedLine Parse(string raw)
    {
        var p = new ParsedLine();
        string s = raw;

        // Strip an optional "[tag] " prefix (the midware tags forwarded server logs).
        if (s.StartsWith("["))
        {
            int end = s.IndexOf("] ", StringComparison.Ordinal);
            if (end > 0)
            {
                p.Tag = s.Substring(1, end - 1);
                s = s[(end + 2)..];
            }
        }
        p.Text = s;

        if (s.StartsWith(StatMarker, StringComparison.Ordinal))
        {
            p.IsStat = true;
            foreach (var tok in s[StatMarker.Length..].Split(' ', StringSplitOptions.RemoveEmptyEntries))
            {
                int eq = tok.IndexOf('=');
                if (eq > 0)
                {
                    p.Fields[tok[..eq]] = tok[(eq + 1)..];
                }
            }
            p.Fields.TryGetValue("role", out var role);
            p.Fields.TryGetValue("id", out var id);
            p.Role = role ?? "";
            p.Id = id ?? "";
        }
        return p;
    }

    // Human-readable metric string for the dashboard (drops role/id).
    public static string MetricsText(ParsedLine p) =>
        string.Join("   ", p.Fields
            .Where(kv => kv.Key is not ("role" or "id"))
            .Select(kv => $"{kv.Key}={kv.Value}"));
}
