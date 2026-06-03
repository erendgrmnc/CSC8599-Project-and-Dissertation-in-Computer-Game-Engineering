using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;

namespace DistributedLauncher;

public partial class MainWindow : Window
{
    private sealed class EntityView
    {
        public required EntityStatus Status;
        public required TextBox Log;
        public required TabItem Tab;
    }

    private readonly List<RoleProcess> _processes = new();
    private readonly List<RemoteAgentClient> _agents = new();
    private readonly Dictionary<string, EntityView> _entities = new();
    private readonly ObservableCollection<EntityStatus> _dashboard = new();

    // Spawn ordering delays: the manager must be listening before the midware
    // connects, and the midware connected before clients try to join.
    private const int ManagerToMidwareDelayMs = 1200;
    private const int MidwareToClientDelayMs = 1000;
    private const int ClientStaggerMs = 500;

    private static readonly Brush LogBackground = new SolidColorBrush(Color.FromRgb(0x0C, 0x0C, 0x0C));
    private static readonly Brush LogForeground = new SolidColorBrush(Color.FromRgb(0xCC, 0xCC, 0xCC));

    public MainWindow()
    {
        InitializeComponent();
        Dashboard.ItemsSource = _dashboard;

        var profile = new LaunchProfile { DeployFolder = LaunchProfile.TryLocateDeployFolder() ?? "" };
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
        HeadlessCheck.IsChecked = p.Headless;
        MinXBox.Text = Fmt(p.WorldMinX);
        MaxXBox.Text = Fmt(p.WorldMaxX);
        MinZBox.Text = Fmt(p.WorldMinZ);
        MaxZBox.Text = Fmt(p.WorldMaxZ);
        RemoteAgentsBox.Text = string.Join(Environment.NewLine, p.RemoteMidwareAgents);
    }

    private LaunchProfile ReadProfileFromForm() => new()
    {
        DeployFolder = DeployBox.Text.Trim(),
        Servers = ParseInt(ServersBox.Text, 1),
        Clients = ParseInt(ClientsBox.Text, 1),
        ObjectsPerPlayer = ParseInt(ObjectsBox.Text, 1),
        ManagerIp = string.IsNullOrWhiteSpace(ManagerIpBox.Text) ? "127.0.0.1" : ManagerIpBox.Text.Trim(),
        ManagerPort = ParseInt(ManagerPortBox.Text, 1234),
        Headless = HeadlessCheck.IsChecked == true,
        WorldMinX = ParseDouble(MinXBox.Text, -150),
        WorldMaxX = ParseDouble(MaxXBox.Text, 150),
        WorldMinZ = ParseDouble(MinZBox.Text, -150),
        WorldMaxZ = ParseDouble(MaxZBox.Text, 150),
        RemoteMidwareAgents = RemoteAgentsBox.Text
            .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .ToList(),
    };

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

        StopAllProcesses();
        ClearAllEntities();

        LaunchButton.IsEnabled = false;
        Log($"Launching: {profile.Servers} server(s), {profile.Clients} client(s), " +
            $"headless={profile.Headless}, world [{Fmt(profile.WorldMinX)}..{Fmt(profile.WorldMaxX)} / " +
            $"{Fmt(profile.WorldMinZ)}..{Fmt(profile.WorldMaxZ)}]");

        // 1) Manager (auto-creates the instance once midwares connect).
        StartRole("Manager", profile.ManagerExe, profile.BuildManagerArgs());
        await Task.Delay(ManagerToMidwareDelayMs);

        // 2) Local midware (spawns the game-server processes; forwards their logs).
        StartRole("Midware", profile.MidwareExe, profile.BuildMidwareArgs());

        // 2b) Remote midware agents.
        foreach (var endpoint in profile.RemoteMidwareAgents)
        {
            string akey = "agent:" + endpoint, aname = "Agent " + endpoint;
            GetOrCreateEntity(akey, aname);
            var agent = new RemoteAgentClient(endpoint);
            agent.LogLine += line => Dispatcher.Invoke(() => Ingest(akey, aname, line));
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
        Log("Launch sequence complete.");
    }

    private void StartRole(string label, string exe, string args)
    {
        var entity = GetOrCreateEntity(label, label);
        var rp = new RoleProcess(label);
        rp.LogLine += line => Dispatcher.Invoke(() => Ingest(label, label, line));
        rp.PropertyChanged += (_, ev) =>
        {
            if (ev.PropertyName == nameof(RoleProcess.Status))
            {
                Dispatcher.Invoke(() => entity.Status.State = rp.Status);
            }
        };
        _processes.Add(rp);
        rp.Start(exe, args, Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory);
    }

    private void OnStopAll(object sender, RoutedEventArgs e) => StopAllProcesses();

    private void StopAllProcesses()
    {
        foreach (var agent in _agents) agent.Stop();
        _agents.Clear();

        foreach (var p in _processes) p.Stop();
        if (_processes.Count > 0) Log("Stopped all processes.");
        _processes.Clear();

        foreach (var s in _dashboard)
        {
            if (s.Key != "launcher") s.State = "stopped";
        }
    }

    // --- Entity routing ------------------------------------------------------
    private EntityView GetOrCreateEntity(string key, string name)
    {
        if (_entities.TryGetValue(key, out var existing))
        {
            return existing;
        }

        var log = new TextBox
        {
            IsReadOnly = true,
            Background = LogBackground,
            Foreground = LogForeground,
            FontFamily = new FontFamily("Consolas"),
            FontSize = 12,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            TextWrapping = TextWrapping.NoWrap,
            AcceptsReturn = true,
            BorderThickness = new Thickness(0),
        };
        var tab = new TabItem { Header = name, Content = log };
        LogTabs.Items.Add(tab);
        if (LogTabs.SelectedIndex < 0) LogTabs.SelectedIndex = 0;

        var status = new EntityStatus(key, name);
        _dashboard.Add(status);

        var view = new EntityView { Status = status, Log = log, Tab = tab };
        _entities[key] = view;
        return view;
    }

    // Routes one raw stdout line to the right log tab + dashboard row. Forwarded
    // game-server lines carry a "[server N]" tag and are split out automatically.
    private void Ingest(string sourceKey, string sourceName, string raw)
    {
        var p = TelemetryParser.Parse(raw);

        string key, name;
        if (p.Tag != null && p.Tag.StartsWith("server", StringComparison.OrdinalIgnoreCase))
        {
            string id = p.Tag.Length > 6 ? p.Tag[6..].Trim() : "?";
            key = "server:" + id;
            name = "Server " + id;
        }
        else
        {
            key = sourceKey;
            name = sourceName;
        }

        var ev = GetOrCreateEntity(key, name);
        ev.Log.AppendText(p.Text + Environment.NewLine);
        ev.Log.ScrollToEnd();

        if (p.IsStat)
        {
            ev.Status.Metrics = TelemetryParser.MetricsText(p);
            if (ev.Status.State is "starting" or "stopped")
            {
                ev.Status.State = "running";
            }
        }
    }

    private void ClearAllEntities()
    {
        LogTabs.Items.Clear();
        _dashboard.Clear();
        _entities.Clear();
    }

    private void Log(string line) => Ingest("launcher", "Launcher", line);

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
                Log($"Saved profile to {dlg.FileName}");
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
                Log($"Loaded profile from {dlg.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "Load failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void OnClearLog(object sender, RoutedEventArgs e) => ClearAllEntities();

    protected override void OnClosed(EventArgs e)
    {
        StopAllProcesses();
        base.OnClosed(e);
    }

    // --- Helpers -------------------------------------------------------------
    private static string Fmt(double d) => d.ToString(CultureInfo.InvariantCulture);
    private static int ParseInt(string s, int fallback) => int.TryParse(s, out var v) ? v : fallback;
    private static double ParseDouble(string s, double fallback) =>
        double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;
}
