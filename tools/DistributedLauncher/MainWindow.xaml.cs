using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows;
using Microsoft.Win32;

namespace DistributedLauncher;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<RoleProcess> _processes = new();
    private readonly List<RemoteAgentClient> _agents = new();

    // Spawn ordering delays: the manager must be listening before the midware
    // connects, and the midware connected before clients try to join.
    private const int ManagerToMidwareDelayMs = 1200;
    private const int MidwareToClientDelayMs = 1000;
    private const int ClientStaggerMs = 500;

    public MainWindow()
    {
        InitializeComponent();
        StatusList.ItemsSource = _processes;

        var profile = new LaunchProfile();
        profile.DeployFolder = LaunchProfile.TryLocateDeployFolder() ?? "";
        WriteProfileToForm(profile);
    }

    // --- Form <-> profile ----------------------------------------------------
    private void WriteProfileToForm(LaunchProfile p)
    {
        DeployBox.Text = p.DeployFolder;
        ServersBox.Text = p.Servers.ToString();
        ClientsBox.Text = p.Clients.ToString();
        ObjectsBox.Text = p.ObjectsPerPlayer.ToString();
        ManagerIpBox.Text = p.ManagerIp;
        ManagerPortBox.Text = p.ManagerPort.ToString();
        MinXBox.Text = Fmt(p.WorldMinX);
        MaxXBox.Text = Fmt(p.WorldMaxX);
        MinZBox.Text = Fmt(p.WorldMinZ);
        MaxZBox.Text = Fmt(p.WorldMaxZ);
        RemoteAgentsBox.Text = string.Join(Environment.NewLine, p.RemoteMidwareAgents);
    }

    private LaunchProfile ReadProfileFromForm()
    {
        return new LaunchProfile
        {
            DeployFolder = DeployBox.Text.Trim(),
            Servers = ParseInt(ServersBox.Text, 1),
            Clients = ParseInt(ClientsBox.Text, 1),
            ObjectsPerPlayer = ParseInt(ObjectsBox.Text, 1),
            ManagerIp = string.IsNullOrWhiteSpace(ManagerIpBox.Text) ? "127.0.0.1" : ManagerIpBox.Text.Trim(),
            ManagerPort = ParseInt(ManagerPortBox.Text, 1234),
            WorldMinX = ParseDouble(MinXBox.Text, -150),
            WorldMaxX = ParseDouble(MaxXBox.Text, 150),
            WorldMinZ = ParseDouble(MinZBox.Text, -150),
            WorldMaxZ = ParseDouble(MaxZBox.Text, 150),
            RemoteMidwareAgents = RemoteAgentsBox.Text
                .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .ToList(),
        };
    }

    // --- Launch / stop -------------------------------------------------------
    private async void OnLaunch(object sender, RoutedEventArgs e)
    {
        var profile = ReadProfileFromForm();

        if (string.IsNullOrWhiteSpace(profile.DeployFolder) || !Directory.Exists(profile.DeployFolder))
        {
            MessageBox.Show("Pick a valid deploy folder (run tools\\build-deploy.ps1 first).",
                "Deploy folder missing", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        // Tear down any previous run before starting a fresh one.
        StopAllProcesses();

        LaunchButton.IsEnabled = false;
        Log($"=== Launching: {profile.Servers} server(s), {profile.Clients} client(s), " +
            $"world [{Fmt(profile.WorldMinX)}..{Fmt(profile.WorldMaxX)} / {Fmt(profile.WorldMinZ)}..{Fmt(profile.WorldMaxZ)}] ===");

        // 1) Manager (auto-creates the instance once midwares connect).
        StartRole("Manager", profile.ManagerExe, profile.BuildManagerArgs());
        await Task.Delay(ManagerToMidwareDelayMs);

        // 2) Local midware (spawns the game-server processes).
        StartRole("Midware (local)", profile.MidwareExe, profile.BuildMidwareArgs());

        // 2b) Remote midware agents: connect to each and ask it to start its
        //     local midware pointed at this manager.
        foreach (var endpoint in profile.RemoteMidwareAgents)
        {
            var agent = new RemoteAgentClient(endpoint);
            agent.LogLine += line => Dispatcher.Invoke(() => Log(line));
            _agents.Add(agent);
            agent.StartMidware(profile.ManagerIp, profile.ManagerPort);
        }

        await Task.Delay(MidwareToClientDelayMs);

        // 3) Clients.
        for (int i = 0; i < profile.Clients; i++)
        {
            StartRole($"Client {i + 1}", profile.ClientExe, profile.BuildClientArgs());
            await Task.Delay(ClientStaggerMs);
        }

        LaunchButton.IsEnabled = true;
        Log("=== Launch sequence complete ===");
    }

    private void StartRole(string label, string exe, string args)
    {
        var rp = new RoleProcess(label);
        rp.LogLine += line => Dispatcher.Invoke(() => Log(line));
        _processes.Add(rp);
        rp.Start(exe, args, Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory);
    }

    private void OnStopAll(object sender, RoutedEventArgs e) => StopAllProcesses();

    private void StopAllProcesses()
    {
        foreach (var agent in _agents)
        {
            agent.Stop();
        }
        _agents.Clear();

        foreach (var p in _processes)
        {
            p.Stop();
        }
        if (_processes.Count > 0)
        {
            Log("=== Stopped all processes ===");
        }
        _processes.Clear();
    }

    // --- Browse / profiles ---------------------------------------------------
    private void OnBrowse(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Select the deploy folder" };
        if (!string.IsNullOrWhiteSpace(DeployBox.Text) && Directory.Exists(DeployBox.Text))
        {
            dlg.InitialDirectory = DeployBox.Text;
        }
        if (dlg.ShowDialog() == true)
        {
            DeployBox.Text = dlg.FolderName;
        }
    }

    private void OnSaveProfile(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog { Filter = "JSON profile (*.json)|*.json", FileName = "launch-profile.json" };
        if (dlg.ShowDialog() == true)
        {
            try
            {
                ReadProfileFromForm().Save(dlg.FileName);
                Log($"[Launcher] Saved profile to {dlg.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "Save failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void OnLoadProfile(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "JSON profile (*.json)|*.json" };
        if (dlg.ShowDialog() == true)
        {
            try
            {
                WriteProfileToForm(LaunchProfile.Load(dlg.FileName));
                Log($"[Launcher] Loaded profile from {dlg.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "Load failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void OnClearLog(object sender, RoutedEventArgs e) => LogBox.Clear();

    protected override void OnClosed(EventArgs e)
    {
        StopAllProcesses();
        base.OnClosed(e);
    }

    // --- Helpers -------------------------------------------------------------
    private void Log(string line)
    {
        LogBox.AppendText(line + Environment.NewLine);
        LogBox.ScrollToEnd();
    }

    private static string Fmt(double d) => d.ToString(CultureInfo.InvariantCulture);
    private static int ParseInt(string s, int fallback) => int.TryParse(s, out var v) ? v : fallback;
    private static double ParseDouble(string s, double fallback) =>
        double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;
}
