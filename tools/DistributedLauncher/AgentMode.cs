namespace DistributedLauncher;

// Headless remote-midware agent. Fully implemented in Milestone 4; for now it is
// a no-op so the controller (GUI) path is unaffected.
internal static class AgentMode
{
    // Returns true if the process was started in agent mode (and therefore the GUI
    // should not be shown). Milestone 4 will listen on a TCP control channel here.
    public static bool TryRun(string[] args)
    {
        return false;
    }
}
