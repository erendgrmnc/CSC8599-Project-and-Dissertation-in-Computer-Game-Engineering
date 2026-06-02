using System.Text.Json;

namespace DistributedLauncher;

// Newline-delimited JSON control protocol between the controller (GUI) and a
// remote launcher running in --agent mode. One JSON object per line.

// Controller -> agent.
public sealed class AgentCommand
{
    public string Cmd { get; set; } = "";          // "start-midware" | "stop" | "status"
    public string ManagerIp { get; set; } = "127.0.0.1";
    public int ManagerPort { get; set; } = 1234;
}

// Agent -> controller.
public sealed class AgentMessage
{
    public string Type { get; set; } = "";          // "log" | "ack" | "error" | "status"
    public string? Line { get; set; }
    public int Pid { get; set; }
    public bool Running { get; set; }
    public string? Message { get; set; }
}

internal static class AgentProtocol
{
    public const int DefaultPort = 5099;

    private static readonly JsonSerializerOptions Opts = new() { IncludeFields = false };

    public static string Encode<T>(T value) => JsonSerializer.Serialize(value, Opts);

    public static T? Decode<T>(string line) => JsonSerializer.Deserialize<T>(line, Opts);
}
