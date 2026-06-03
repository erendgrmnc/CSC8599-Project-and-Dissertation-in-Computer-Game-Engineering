using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace DistributedLauncher;

// One row in the live status dashboard, representing a role or a discovered game
// server. Bound to the UI via INotifyPropertyChanged.
public sealed class EntityStatus : INotifyPropertyChanged
{
    public EntityStatus(string key, string name)
    {
        Key = key;
        Name = name;
    }

    public string Key { get; }
    public string Name { get; }

    private string _state = "starting";
    public string State
    {
        get => _state;
        set { _state = value; OnPropertyChanged(); }
    }

    private string _metrics = "";
    public string Metrics
    {
        get => _metrics;
        set { _metrics = value; OnPropertyChanged(); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
