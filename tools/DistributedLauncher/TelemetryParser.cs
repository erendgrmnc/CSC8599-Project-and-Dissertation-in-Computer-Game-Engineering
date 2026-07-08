using System.Globalization;

namespace DistributedLauncher;

// One parsed stdout line from a role (or a forwarded game-server line).
public sealed class ParsedLine
{
    public int? ServerId;              // N from a "[server N] " prefix (midware-forwarded), else null
    public string Text = "";           // the line, minus a recognised "[server N] " prefix
    public bool IsStat;                // true when the (de-tagged) line is "@@STAT ..."
    public string Role = "";           // STAT role= field
    public string Id = "";             // STAT id= field
    public Dictionary<string, string> Fields = new();
}

public static class TelemetryParser
{
    private const string StatMarker = "@@STAT ";
    private const string ServerTagPrefix = "[server ";

    public static ParsedLine Parse(string raw)
    {
        var p = new ParsedLine();
        string s = raw;

        // Strip ONLY a well-formed "[server N] " prefix, which the midware prepends to
        // each game-server line it forwards. Any other bracketed text (e.g. a launcher
        // "[Manager] " tag, or a log line that happens to start with '[') is left intact
        // so it cannot be mistaken for a server tag and mis-routed.
        if (s.StartsWith(ServerTagPrefix, StringComparison.Ordinal))
        {
            int end = s.IndexOf("] ", ServerTagPrefix.Length, StringComparison.Ordinal);
            if (end > 0)
            {
                string idStr = s.Substring(ServerTagPrefix.Length, end - ServerTagPrefix.Length);
                if (int.TryParse(idStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out int id))
                {
                    p.ServerId = id;
                    s = s[(end + 2)..];
                }
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
