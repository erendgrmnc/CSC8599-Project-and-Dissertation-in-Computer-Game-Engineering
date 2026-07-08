using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

namespace DistributedLauncher;

// Wraps a spawned role process (Manager / Midware / Client). Surfaces a live
// status + PID for binding and raises LogLine for each line of stdout/stderr.
public sealed class RoleProcess : INotifyPropertyChanged
{
    private Process? _process;
    private string _status = "idle";

    public RoleProcess(string label)
    {
        Label = label;
    }

    public string Label { get; }

    public int Pid => _process is { HasExited: false } ? _process.Id : 0;

    public string Status
    {
        get => _status;
        private set { _status = value; OnPropertyChanged(); OnPropertyChanged(nameof(Pid)); }
    }

    public bool IsRunning => _process is { HasExited: false };

    public event Action<string>? LogLine;
    public event PropertyChangedEventHandler? PropertyChanged;

    public bool Start(string exePath, string args, string workingDir)
    {
        if (!File.Exists(exePath))
        {
            Status = "missing exe";
            LogLine?.Invoke($"[{Label}] ERROR: executable not found: {exePath}");
            return false;
        }

        var psi = new ProcessStartInfo
        {
            FileName = exePath,
            Arguments = args,
            WorkingDirectory = workingDir,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        _process.OutputDataReceived += (_, e) => { if (e.Data != null) LogLine?.Invoke($"[{Label}] {e.Data}"); };
        _process.ErrorDataReceived += (_, e) => { if (e.Data != null) LogLine?.Invoke($"[{Label}] STDERR: {e.Data}"); };

        // Every role's main loop is `while (true)` (see DistributedSystemCommonFiles/
        // HeadlessRunner), so ANY exit is abnormal. Report the code: 0xC0000005 is an
        // access violation, 0xC0000409 a stack buffer overrun, 3 an unhandled C++
        // exception. Without it a crash looks identical to a clean shutdown.
        _process.Exited += (_, _) =>
        {
            int code = _process.ExitCode;
            Status = "exited";
            LogLine?.Invoke($"[{Label}] process exited (code {code} / 0x{code:X8}).");
        };

        try
        {
            _process.Start();
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
            Status = "running";
            LogLine?.Invoke($"[{Label}] started (pid {_process.Id}): {Path.GetFileName(exePath)} {args}");
            OnPropertyChanged(nameof(Pid));
            return true;
        }
        catch (Exception ex)
        {
            Status = "failed";
            LogLine?.Invoke($"[{Label}] ERROR starting process: {ex.Message}");
            return false;
        }
    }

    // Kills the whole process tree (taskkill /T) so the game-server consoles the
    // midware spawned as children are torn down too.
    public void Stop()
    {
        if (_process == null || _process.HasExited)
        {
            Status = "stopped";
            return;
        }

        try
        {
            var kill = new ProcessStartInfo("taskkill", $"/PID {_process.Id} /T /F")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            Process.Start(kill)?.WaitForExit(3000);
        }
        catch (Exception ex)
        {
            LogLine?.Invoke($"[{Label}] ERROR stopping: {ex.Message}");
        }
        Status = "stopped";
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
