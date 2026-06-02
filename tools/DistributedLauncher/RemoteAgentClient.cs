using System.IO;
using System.Net.Sockets;

namespace DistributedLauncher;

// Controller-side connection to one remote midware agent. Sends commands and
// surfaces the agent's streamed log/ack lines via LogLine.
public sealed class RemoteAgentClient
{
    private readonly string _host;
    private readonly int _port;
    private TcpClient? _client;
    private StreamWriter? _writer;
    private Thread? _readThread;
    private volatile bool _closed;

    public RemoteAgentClient(string endpoint)
    {
        Endpoint = endpoint;
        var parts = endpoint.Split(':', 2);
        _host = parts[0];
        _port = parts.Length > 1 && int.TryParse(parts[1], out int p) ? p : AgentProtocol.DefaultPort;
    }

    public string Endpoint { get; }

    public event Action<string>? LogLine;

    // Connects and asks the agent to start its local midware pointed at the manager.
    public bool StartMidware(string managerIp, int managerPort)
    {
        try
        {
            _client = new TcpClient();
            _client.Connect(_host, _port);
            var stream = _client.GetStream();
            _writer = new StreamWriter(stream) { AutoFlush = true };

            _readThread = new Thread(() => ReadLoop(stream)) { IsBackground = true };
            _readThread.Start();

            Send(new AgentCommand { Cmd = "start-midware", ManagerIp = managerIp, ManagerPort = managerPort });
            LogLine?.Invoke($"[Agent {Endpoint}] connected, requested midware start.");
            return true;
        }
        catch (Exception ex)
        {
            LogLine?.Invoke($"[Agent {Endpoint}] ERROR: {ex.Message}");
            return false;
        }
    }

    public void Stop()
    {
        _closed = true;
        try { if (_writer != null) Send(new AgentCommand { Cmd = "stop" }); } catch { /* ignore */ }
        try { _client?.Close(); } catch { /* ignore */ }
    }

    private void Send(AgentCommand cmd)
    {
        _writer?.WriteLine(AgentProtocol.Encode(cmd));
    }

    private void ReadLoop(NetworkStream stream)
    {
        try
        {
            using var reader = new StreamReader(stream);
            string? line;
            while (!_closed && (line = reader.ReadLine()) != null)
            {
                var msg = AgentProtocol.Decode<AgentMessage>(line);
                if (msg == null) { continue; }
                switch (msg.Type)
                {
                    case "log":
                        LogLine?.Invoke(msg.Line ?? "");
                        break;
                    case "ack":
                        LogLine?.Invoke($"[Agent {Endpoint}] ack: {msg.Message} (pid {msg.Pid})");
                        break;
                    case "error":
                        LogLine?.Invoke($"[Agent {Endpoint}] error: {msg.Message}");
                        break;
                    case "status":
                        LogLine?.Invoke($"[Agent {Endpoint}] status: running={msg.Running} pid={msg.Pid}");
                        break;
                }
            }
        }
        catch (Exception ex)
        {
            if (!_closed) { LogLine?.Invoke($"[Agent {Endpoint}] connection closed: {ex.Message}"); }
        }
    }
}
