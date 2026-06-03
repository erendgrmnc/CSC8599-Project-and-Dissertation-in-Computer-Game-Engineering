using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DistributedLauncher;

// Serializable configuration profile for a distributed-system run. Mirrors the
// flags the C++ roles parse in LaunchConfig (CSC8503CoreClasses/DistributedSystemCommonFiles).
public sealed class LaunchProfile
{
    public int Servers { get; set; } = 2;
    public int Clients { get; set; } = 1;
    public int ObjectsPerPlayer { get; set; } = 1;

    public double WorldMinX { get; set; } = -150;
    public double WorldMaxX { get; set; } = 150;
    public double WorldMinZ { get; set; } = -150;
    public double WorldMaxZ { get; set; } = 150;

    public string ManagerIp { get; set; } = "127.0.0.1";
    public int ManagerPort { get; set; } = 1234;

    // Headless = roles run windowless and stream telemetry/logs to this launcher
    // (the single pane of glass). Uncheck for the per-role OpenGL profiler windows
    // used in dissertation evaluation screenshots.
    public bool Headless { get; set; } = true;

    public string DeployFolder { get; set; } = "";

    // Remote midware agent endpoints ("host:port"). Empty => local-only (one midware).
    // Used by Milestone 4; the local midware is always launched in addition.
    public List<string> RemoteMidwareAgents { get; set; } = new();

    [JsonIgnore]
    public int TotalMidwareCount => 1 + RemoteMidwareAgents.Count;

    // --- Deployed exe paths --------------------------------------------------
    [JsonIgnore] public string ManagerExe => Path.Combine(DeployFolder, "Manager", "EntryPoint.exe");
    [JsonIgnore] public string MidwareExe => Path.Combine(DeployFolder, "Midware", "EntryPoint.exe");
    [JsonIgnore] public string ClientExe => Path.Combine(DeployFolder, "Client", "EntryPoint.exe");
    [JsonIgnore] public string GameServerExe => Path.Combine(DeployFolder, "DistributedPhysicsServer", "EntryPoint.exe");

    private string HeadlessFlag => Headless ? " --headless" : "";

    // --- Command-line builders (match the C++ LaunchConfig flags) ------------
    public string BuildManagerArgs() =>
        $"--servers {Servers} --clients {Clients} --objects {ObjectsPerPlayer} " +
        $"--port {ManagerPort} --world {Fmt(WorldMinX)},{Fmt(WorldMaxX)},{Fmt(WorldMinZ)},{Fmt(WorldMaxZ)} " +
        $"--midwares {TotalMidwareCount} --autostart{HeadlessFlag}";

    public string BuildMidwareArgs() =>
        $"--manager-ip {ManagerIp} --manager-port {ManagerPort} --server-exe \"{GameServerExe}\"{HeadlessFlag}";

    // Clients always run windowed (with a head) so the operator can see them;
    // only the infrastructure roles (manager / midware / game servers) honour the
    // headless toggle.
    public string BuildClientArgs() =>
        $"--manager-ip {ManagerIp} --manager-port {ManagerPort}";

    private static string Fmt(double d) =>
        d.ToString(System.Globalization.CultureInfo.InvariantCulture);

    // --- Persistence ---------------------------------------------------------
    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };

    public void Save(string path) =>
        File.WriteAllText(path, JsonSerializer.Serialize(this, JsonOpts));

    public static LaunchProfile Load(string path) =>
        JsonSerializer.Deserialize<LaunchProfile>(File.ReadAllText(path)) ?? new LaunchProfile();

    // Best-effort auto-discovery of the deploy/ folder produced by build-deploy.ps1,
    // walking up from the running exe location.
    public static string? TryLocateDeployFolder()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var candidate = Path.Combine(dir.FullName, "deploy");
            if (File.Exists(Path.Combine(candidate, "Manager", "EntryPoint.exe")))
            {
                return candidate;
            }
            dir = dir.Parent;
        }
        return null;
    }
}
