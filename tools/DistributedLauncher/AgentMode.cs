using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace DistributedLauncher;

// Headless remote-midware agent. Runs on a physics machine and, on command from
// the controller GUI, spawns the local midware (which in turn spawns the game
// servers) and streams its log lines back over a TCP/JSON control channel.
//
// Usage on a remote machine (after copying the deploy/ folder + this exe):
//     DistributedLauncher.exe --agent --port 5099
internal static class AgentMode
{
    [DllImport("kernel32.dll")] private static extern bool AllocConsole();

    private static readonly object WriteLock = new();
    private static RoleProcess? _midware;

    // Returns true if started in agent mode (so the GUI is not shown). Blocks
    // serving the control channel until the process is terminated.
    public static bool TryRun(string[] args)
    {
        if (!args.Contains("--agent"))
        {
            return false;
        }

        AllocConsole();
        int port = ReadPort(args);

        var deploy = LaunchProfile.TryLocateDeployFolder();
        Console.WriteLine("==== Distributed Launcher — AGENT MODE ====");
        Console.WriteLine($"Listening on port {port}");
        Console.WriteLine($"Deploy folder: {deploy ?? "(not found — place this exe next to deploy/)"}");

        var listener = new TcpListener(IPAddress.Any, port);
        listener.Start();

        while (true)
        {
            try
            {
                using TcpClient client = listener.AcceptTcpClient();
                Console.WriteLine($"Controller connected: {client.Client.RemoteEndPoint}");
                ServeClient(client, deploy);
                Console.WriteLine("Controller disconnected.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Agent error: {ex.Message}");
            }
        }
    }

    private static int ReadPort(string[] args)
    {
        int idx = Array.IndexOf(args, "--port");
        if (idx >= 0 && idx + 1 < args.Length && int.TryParse(args[idx + 1], out int p))
        {
            return p;
        }
        return AgentProtocol.DefaultPort;
    }

    private static void ServeClient(TcpClient client, string? deploy)
    {
        using NetworkStream stream = client.GetStream();
        using var reader = new StreamReader(stream);
        using var writer = new StreamWriter(stream) { AutoFlush = true };

        string? line;
        while ((line = reader.ReadLine()) != null)
        {
            AgentCommand? cmd;
            try { cmd = AgentProtocol.Decode<AgentCommand>(line); }
            catch { Send(writer, new AgentMessage { Type = "error", Message = "bad command json" }); continue; }
            if (cmd == null) { continue; }

            switch (cmd.Cmd)
            {
                case "start-midware":
                    StartMidware(writer, deploy, cmd);
                    break;
                case "stop":
                    StopMidware();
                    Send(writer, new AgentMessage { Type = "ack", Message = "stopped" });
                    break;
                case "status":
                    Send(writer, new AgentMessage { Type = "status", Running = _midware?.IsRunning ?? false, Pid = _midware?.Pid ?? 0 });
                    break;
                default:
                    Send(writer, new AgentMessage { Type = "error", Message = $"unknown cmd '{cmd.Cmd}'" });
                    break;
            }
        }
    }

    private static void StartMidware(StreamWriter writer, string? deploy, AgentCommand cmd)
    {
        if (deploy == null)
        {
            Send(writer, new AgentMessage { Type = "error", Message = "deploy folder not found on agent machine" });
            return;
        }

        StopMidware();

        string midwareExe = Path.Combine(deploy, "Midware", "EntryPoint.exe");
        string serverExe = Path.Combine(deploy, "DistributedPhysicsServer", "EntryPoint.exe");
        string args = $"--manager-ip {cmd.ManagerIp} --manager-port {cmd.ManagerPort} --server-exe \"{serverExe}\"";

        var rp = new RoleProcess("Midware (remote)");
        rp.LogLine += l =>
        {
            Console.WriteLine(l);
            Send(writer, new AgentMessage { Type = "log", Line = l });
        };
        _midware = rp;

        bool ok = rp.Start(midwareExe, args, Path.GetDirectoryName(midwareExe) ?? deploy);
        Send(writer, ok
            ? new AgentMessage { Type = "ack", Pid = rp.Pid, Message = "midware started" }
            : new AgentMessage { Type = "error", Message = "failed to start midware" });
    }

    private static void StopMidware()
    {
        _midware?.Stop();
        _midware = null;
    }

    private static void Send(StreamWriter writer, AgentMessage msg)
    {
        lock (WriteLock)
        {
            try { writer.WriteLine(AgentProtocol.Encode(msg)); }
            catch { /* controller dropped the connection */ }
        }
    }
}
