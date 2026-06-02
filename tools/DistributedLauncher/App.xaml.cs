using System.Windows;

namespace DistributedLauncher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // Headless agent mode (remote midware machines) is handled in Milestone 4.
        // It branches here on "--agent" before any window is created.
        if (AgentMode.TryRun(e.Args))
        {
            return;
        }

        var window = new MainWindow();
        window.Show();
    }
}
