using System.Security.Principal;
using NetConditioner.Forms;
using NetConditioner.Services;

namespace NetConditioner;

internal static class Program
{
    private static Mutex? _instanceMutex;

    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Any(a => a.Equals("--self-test", StringComparison.OrdinalIgnoreCase)))
        {
            Environment.ExitCode = SelfTest.Run();
            return;
        }

        _instanceMutex = new Mutex(true, @"Global\NetConditioner.SingleInstance", out var createdNew);
        if (!createdNew)
        {
            MessageBox.Show("弱网模拟器已经在运行。", "NetConditioner",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        if (!IsAdministrator())
        {
            MessageBox.Show("WinDivert 需要管理员权限。请右键选择“以管理员身份运行”。",
                "NetConditioner", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        ApplicationConfiguration.Initialize();
        Application.ThreadException += (_, e) => ShowUnexpectedError(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            ShowUnexpectedError(e.ExceptionObject as Exception);

        Application.Run(new MainForm());
        GC.KeepAlive(_instanceMutex);
    }

    private static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static void ShowUnexpectedError(Exception? ex)
    {
        MessageBox.Show(ex?.ToString() ?? "发生未知错误。", "NetConditioner",
            MessageBoxButtons.OK, MessageBoxIcon.Error);
    }
}
