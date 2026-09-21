// TransperacyBar - makes the Windows taskbar transparent, similar to TranslucentTB.
// Built with the .NET Framework compiler that ships with Windows (see build.bat),
// so the code sticks to C# 5 features.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace TransperacyBar
{
    enum Mode { Clear, Tinted, Blur, Acrylic, Normal }

    static class Native
    {
        public enum AccentState
        {
            Disabled = 0,
            EnableGradient = 1,
            EnableTransparentGradient = 2,
            EnableBlurBehind = 3,
            EnableAcrylicBlurBehind = 4,
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct AccentPolicy
        {
            public AccentState AccentState;
            public int AccentFlags;
            public uint GradientColor; // AABBGGRR
            public int AnimationId;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct WindowCompositionAttributeData
        {
            public int Attribute;
            public IntPtr Data;
            public int SizeOfData;
        }

        public const int WCA_ACCENT_POLICY = 19;

        [DllImport("user32.dll")]
        public static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr FindWindow(string className, string windowName);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr childAfter, string className, string windowName);

        public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern bool IsZoomed(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern bool IsIconic(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

        public const uint MONITOR_DEFAULTTONEAREST = 2;

        [DllImport("dwmapi.dll")]
        public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);

        public const int DWMWA_CLOAKED = 14;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int maxCount);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

        [DllImport("user32.dll")]
        public static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam,
            uint flags, uint timeout, out IntPtr result);

        public static readonly IntPtr HWND_MESSAGE = new IntPtr(-3);
        public const uint SMTO_ABORTIFHUNG = 2;

        [DllImport("Windows.UI.Xaml.dll", CharSet = CharSet.Unicode)]
        public static extern int InitializeXamlDiagnosticsEx(string endPointName, uint pid, string xamlDiagnosticsDll,
            string tapDll, Guid tapClsid, string initializationData);

        public static void SetAccent(IntPtr hwnd, AccentState state, uint color)
        {
            var accent = new AccentPolicy
            {
                AccentState = state,
                AccentFlags = 2, // makes GradientColor apply
                GradientColor = color,
            };

            int size = Marshal.SizeOf(accent);
            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(accent, ptr, false);
                var data = new WindowCompositionAttributeData
                {
                    Attribute = WCA_ACCENT_POLICY,
                    Data = ptr,
                    SizeOfData = size,
                };
                SetWindowCompositionAttribute(hwnd, ref data);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }
    }

    class Settings
    {
        public Mode Mode = Mode.Clear;
        public Color Color = Color.Black;
        public int Opacity = 0; // 0-255, used by Tinted/Blur/Acrylic
        public bool NormalWhenMaximized = false;

        static string FilePath
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "TransperacyBar", "settings.ini");
            }
        }

        public static Settings Load()
        {
            var s = new Settings();
            try
            {
                if (!File.Exists(FilePath))
                    return s;
                foreach (string line in File.ReadAllLines(FilePath))
                {
                    int eq = line.IndexOf('=');
                    if (eq < 0)
                        continue;
                    string key = line.Substring(0, eq).Trim();
                    string value = line.Substring(eq + 1).Trim();
                    switch (key)
                    {
                        case "Mode": s.Mode = (Mode)Enum.Parse(typeof(Mode), value); break;
                        case "Color": s.Color = ColorTranslator.FromHtml(value); break;
                        case "Opacity": s.Opacity = Math.Max(0, Math.Min(255, int.Parse(value))); break;
                        case "NormalWhenMaximized": s.NormalWhenMaximized = bool.Parse(value); break;
                    }
                }
            }
            catch
            {
                // A broken settings file falls back to defaults.
            }
            return s;
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
                File.WriteAllLines(FilePath, new[]
                {
                    "Mode=" + Mode,
                    "Color=" + ColorTranslator.ToHtml(Color.FromArgb(Color.R, Color.G, Color.B)),
                    "Opacity=" + Opacity,
                    "NormalWhenMaximized=" + NormalWhenMaximized,
                });
            }
            catch
            {
            }
        }

        // AABBGGRR, as expected by the accent policy.
        public uint AccentColor
        {
            get { return ((uint)Opacity << 24) | ((uint)Color.B << 16) | ((uint)Color.G << 8) | Color.R; }
        }

        // AARRGGBB, as expected by ExplorerHook.dll.
        public uint Argb
        {
            get { return ((uint)Opacity << 24) | ((uint)Color.R << 16) | ((uint)Color.G << 8) | Color.B; }
        }
    }

    // On Windows 11 22H2+ the taskbar's XAML draws a solid background over the window the
    // accent is applied to, and blur/acrylic accents come out black. ExplorerHook.dll (see hook/)
    // is loaded into explorer and draws the chosen appearance in the taskbar's XAML instead.
    static class ExplorerHook
    {
        const string WindowClass = "TransperacyBarHook";
        const uint WM_APP_SETAPPEARANCE = 0x8000 + 1;
        static readonly Guid Clsid = new Guid("7C8D2E61-4B3A-4F5E-9A21-6E0B5C3D8F14"); // must match ExplorerHook.cpp

        static uint injectedPid;     // explorer process the hook was successfully loaded into
        static int injecting;        // 1 while an injection attempt is running
        static int lastAttempt = Environment.TickCount - 10000;
        static IntPtr lastWindow = IntPtr.Zero;
        static int lastMode = -1;
        static uint lastColor;

        // wParam values understood by the hook; must match Mode in ExplorerHook.cpp.
        static int HookMode(Mode mode)
        {
            switch (mode)
            {
                case Mode.Clear: return 1;
                case Mode.Tinted: return 2;
                case Mode.Blur: return 3;
                case Mode.Acrylic: return 4;
                default: return 0;
            }
        }

        static string DllPath
        {
            get { return Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "ExplorerHook.dll"); }
        }

        // Environment.OSVersion reports Windows 8 to apps without a manifest, so read the real build.
        static readonly int WindowsBuild = ReadWindowsBuild();

        static int ReadWindowsBuild()
        {
            try
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                    return int.Parse((string)key.GetValue("CurrentBuildNumber"));
            }
            catch
            {
                return 0;
            }
        }

        static bool XamlTaskbar
        {
            get { return WindowsBuild >= 22621 && File.Exists(DllPath); }
        }

        static IntPtr FindHookWindow(uint explorerPid)
        {
            IntPtr hwnd = IntPtr.Zero;
            while ((hwnd = Native.FindWindowEx(Native.HWND_MESSAGE, hwnd, WindowClass, null)) != IntPtr.Zero)
            {
                uint pid;
                Native.GetWindowThreadProcessId(hwnd, out pid);
                if (pid == explorerPid)
                    return hwnd;
            }
            return IntPtr.Zero;
        }

        // Called on every timer tick. Injects the hook once per explorer process and keeps it in
        // sync with the chosen appearance (color is 0xAARRGGBB). Returns whether the hook is
        // drawing the taskbar.
        public static bool Update(IntPtr mainTaskbar, Mode mode, uint color)
        {
            if (!XamlTaskbar || mainTaskbar == IntPtr.Zero)
                return false;

            uint pid;
            Native.GetWindowThreadProcessId(mainTaskbar, out pid);
            IntPtr hookWindow = FindHookWindow(pid);

            if (hookWindow == IntPtr.Zero)
            {
                // Right after explorer starts, its XAML isn't up yet and injection fails, so retry
                // every couple of seconds until it succeeds.
                if (mode != Mode.Normal && pid != injectedPid && Environment.TickCount - lastAttempt > 2000 &&
                    Interlocked.CompareExchange(ref injecting, 1, 0) == 0)
                {
                    lastAttempt = Environment.TickCount;
                    string dll = DllPath;
                    ThreadPool.QueueUserWorkItem(delegate
                    {
                        if (Inject(pid, dll))
                            injectedPid = pid;
                        injecting = 0;
                    });
                }
                return false;
            }

            int hookMode = HookMode(mode);
            if (hookWindow != lastWindow || hookMode != lastMode || color != lastColor)
            {
                IntPtr result;
                if (Native.SendMessageTimeout(hookWindow, WM_APP_SETAPPEARANCE, new IntPtr(hookMode),
                        new IntPtr(unchecked((int)color)), Native.SMTO_ABORTIFHUNG, 1000, out result) == IntPtr.Zero)
                    return false;
                lastWindow = hookWindow;
                lastMode = hookMode;
                lastColor = color;
            }
            return true;
        }

        static bool Inject(uint pid, string dll)
        {
            // Each diagnostics connection name can only be used once per process.
            for (int i = 1; i <= 10; i++)
            {
                try
                {
                    if (Native.InitializeXamlDiagnosticsEx("VisualDiagConnection" + i, pid, null, dll, Clsid, null) >= 0)
                        return true;
                }
                catch
                {
                    return false;
                }
            }
            return false;
        }

        public static void Restore(IntPtr mainTaskbar)
        {
            if (!XamlTaskbar || mainTaskbar == IntPtr.Zero)
                return;
            uint pid;
            Native.GetWindowThreadProcessId(mainTaskbar, out pid);
            IntPtr hookWindow = FindHookWindow(pid);
            if (hookWindow == IntPtr.Zero)
                return;
            IntPtr result;
            Native.SendMessageTimeout(hookWindow, WM_APP_SETAPPEARANCE, IntPtr.Zero, IntPtr.Zero,
                Native.SMTO_ABORTIFHUNG, 1000, out result);
        }
    }

    class TrayApp : ApplicationContext
    {
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunValue = "TransperacyBar";

        readonly Settings settings = Settings.Load();
        readonly NotifyIcon trayIcon;
        readonly System.Windows.Forms.Timer timer;
        readonly Dictionary<Mode, ToolStripMenuItem> modeItems = new Dictionary<Mode, ToolStripMenuItem>();
        readonly Dictionary<int, ToolStripMenuItem> opacityItems = new Dictionary<int, ToolStripMenuItem>();
        ToolStripMenuItem maximizedItem;
        ToolStripMenuItem startupItem;

        public TrayApp()
        {
            trayIcon = new NotifyIcon
            {
                Icon = CreateIcon(),
                Text = "TransperacyBar",
                ContextMenuStrip = BuildMenu(),
                Visible = true,
            };
            UpdateChecks();

            // Explorer resets the taskbar's accent whenever it repaints (start menu, hover,
            // theme changes, explorer restart), so it has to be re-applied continuously.
            timer = new System.Windows.Forms.Timer { Interval = 50 };
            timer.Tick += delegate { Apply(); };
            timer.Start();
            Apply();
        }

        ContextMenuStrip BuildMenu()
        {
            var menu = new ContextMenuStrip();

            foreach (Mode mode in Enum.GetValues(typeof(Mode)))
            {
                Mode m = mode;
                var item = new ToolStripMenuItem(mode.ToString(), null, delegate { settings.Mode = m; Changed(); });
                modeItems[mode] = item;
                menu.Items.Add(item);
            }

            menu.Items.Add(new ToolStripSeparator());

            menu.Items.Add(new ToolStripMenuItem("Tint color...", null, delegate { PickColor(); }));

            var opacityMenu = new ToolStripMenuItem("Tint opacity");
            foreach (int percent in new[] { 0, 10, 25, 50, 75, 100 })
            {
                int alpha = percent * 255 / 100;
                var item = new ToolStripMenuItem(percent + "%", null, delegate { settings.Opacity = alpha; Changed(); });
                opacityItems[alpha] = item;
                opacityMenu.DropDownItems.Add(item);
            }
            menu.Items.Add(opacityMenu);

            maximizedItem = new ToolStripMenuItem("Normal when a window is maximized", null, delegate
            {
                settings.NormalWhenMaximized = !settings.NormalWhenMaximized;
                Changed();
            });
            menu.Items.Add(maximizedItem);

            menu.Items.Add(new ToolStripSeparator());

            startupItem = new ToolStripMenuItem("Start with Windows", null, delegate
            {
                SetStartup(!IsStartupEnabled());
                UpdateChecks();
            });
            menu.Items.Add(startupItem);

            menu.Items.Add(new ToolStripMenuItem("Exit", null, delegate { Exit(); }));
            return menu;
        }

        void PickColor()
        {
            using (var dialog = new ColorDialog { Color = settings.Color, FullOpen = true })
            {
                if (dialog.ShowDialog() != DialogResult.OK)
                    return;
                settings.Color = dialog.Color;
                // Picking a color with 0% opacity would show nothing, so give it some.
                if (settings.Opacity == 0)
                    settings.Opacity = 50 * 255 / 100;
                if (settings.Mode == Mode.Clear || settings.Mode == Mode.Normal)
                    settings.Mode = Mode.Tinted;
                Changed();
            }
        }

        void Changed()
        {
            settings.Save();
            UpdateChecks();
            Apply();
        }

        void UpdateChecks()
        {
            foreach (var pair in modeItems)
                pair.Value.Checked = pair.Key == settings.Mode;
            foreach (var pair in opacityItems)
                pair.Value.Checked = pair.Key == settings.Opacity;
            maximizedItem.Checked = settings.NormalWhenMaximized;
            startupItem.Checked = IsStartupEnabled();
        }

        static List<IntPtr> FindTaskbars()
        {
            var result = new List<IntPtr>();
            IntPtr main = Native.FindWindow("Shell_TrayWnd", null);
            if (main != IntPtr.Zero)
                result.Add(main);

            // One Shell_SecondaryTrayWnd per additional monitor.
            IntPtr secondary = IntPtr.Zero;
            while ((secondary = Native.FindWindowEx(IntPtr.Zero, secondary, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero)
                result.Add(secondary);
            return result;
        }

        void Apply()
        {
            HashSet<IntPtr> maximizedMonitors = settings.NormalWhenMaximized ? FindMaximizedMonitors() : null;

            List<IntPtr> taskbars = FindTaskbars();
            var modes = new List<Mode>();
            foreach (IntPtr taskbar in taskbars)
            {
                Mode mode = settings.Mode;
                if (maximizedMonitors != null &&
                    maximizedMonitors.Contains(Native.MonitorFromWindow(taskbar, Native.MONITOR_DEFAULTTONEAREST)))
                    mode = Mode.Normal;
                modes.Add(mode);
            }

            // The hook styles every taskbar at once, so it keeps the effect as long as any
            // taskbar wants one.
            bool anyEffect = modes.Exists(delegate(Mode m) { return m != Mode.Normal; });
            bool hooked = ExplorerHook.Update(taskbars.Count > 0 ? taskbars[0] : IntPtr.Zero,
                anyEffect ? settings.Mode : Mode.Normal, settings.Argb);

            for (int i = 0; i < taskbars.Count; i++)
            {
                // When the hook draws the effect in XAML, the window underneath just has to be see-through.
                if (hooked)
                    ApplyMode(taskbars[i], modes[i] == Mode.Normal ? Mode.Normal : Mode.Clear);
                else
                    ApplyMode(taskbars[i], modes[i]);
            }
        }

        void ApplyMode(IntPtr taskbar, Mode mode)
        {
            switch (mode)
            {
                case Mode.Clear:
                    Native.SetAccent(taskbar, Native.AccentState.EnableTransparentGradient, 0);
                    break;
                case Mode.Tinted:
                    Native.SetAccent(taskbar, Native.AccentState.EnableTransparentGradient, settings.AccentColor);
                    break;
                case Mode.Blur:
                    Native.SetAccent(taskbar, Native.AccentState.EnableBlurBehind, settings.AccentColor);
                    break;
                case Mode.Acrylic:
                    // Acrylic with a fully transparent tint makes the taskbar lag, so keep alpha >= 1.
                    uint color = settings.AccentColor;
                    if ((color >> 24) == 0)
                        color |= 0x01000000;
                    Native.SetAccent(taskbar, Native.AccentState.EnableAcrylicBlurBehind, color);
                    break;
                case Mode.Normal:
                    Native.SetAccent(taskbar, Native.AccentState.Disabled, 0);
                    break;
            }
        }

        static HashSet<IntPtr> FindMaximizedMonitors()
        {
            var monitors = new HashSet<IntPtr>();
            var className = new StringBuilder(256);
            Native.EnumWindows(delegate(IntPtr hwnd, IntPtr lParam)
            {
                if (!Native.IsWindowVisible(hwnd) || Native.IsIconic(hwnd) || !Native.IsZoomed(hwnd))
                    return true;

                // Suspended UWP apps and windows on other virtual desktops are cloaked but still "visible".
                int cloaked;
                if (Native.DwmGetWindowAttribute(hwnd, Native.DWMWA_CLOAKED, out cloaked, sizeof(int)) == 0 && cloaked != 0)
                    return true;

                className.Clear();
                Native.GetClassName(hwnd, className, className.Capacity);
                string name = className.ToString();
                if (name == "Shell_TrayWnd" || name == "Shell_SecondaryTrayWnd" || name == "Progman" || name == "WorkerW")
                    return true;

                monitors.Add(Native.MonitorFromWindow(hwnd, Native.MONITOR_DEFAULTTONEAREST));
                return true;
            }, IntPtr.Zero);
            return monitors;
        }

        static bool IsStartupEnabled()
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey))
                return key != null && key.GetValue(RunValue) != null;
        }

        static void SetStartup(bool enable)
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (enable)
                    key.SetValue(RunValue, "\"" + Application.ExecutablePath + "\"");
                else
                    key.DeleteValue(RunValue, false);
            }
        }

        static Icon CreateIcon()
        {
            using (var bitmap = new Bitmap(32, 32))
            {
                using (Graphics g = Graphics.FromImage(bitmap))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.Clear(Color.Transparent);
                    using (var frame = new Pen(Color.White, 2))
                        g.DrawRectangle(frame, 2, 5, 27, 21);
                    using (var bar = new SolidBrush(Color.FromArgb(140, 90, 170, 255)))
                        g.FillRectangle(bar, 3, 19, 26, 7);
                    using (var dot = new SolidBrush(Color.White))
                    {
                        g.FillRectangle(dot, 6, 21, 3, 3);
                        g.FillRectangle(dot, 11, 21, 3, 3);
                        g.FillRectangle(dot, 16, 21, 3, 3);
                    }
                }
                return Icon.FromHandle(bitmap.GetHicon());
            }
        }

        void Exit()
        {
            timer.Stop();
            List<IntPtr> taskbars = FindTaskbars();
            foreach (IntPtr taskbar in taskbars)
                Native.SetAccent(taskbar, Native.AccentState.Disabled, 0);
            ExplorerHook.Restore(taskbars.Count > 0 ? taskbars[0] : IntPtr.Zero);
            trayIcon.Visible = false;
            trayIcon.Dispose();
            ExitThread();
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            bool createdNew;
            using (var mutex = new Mutex(true, "TransperacyBar-SingleInstance", out createdNew))
            {
                if (!createdNew)
                {
                    MessageBox.Show("TransperacyBar is already running. Look for its icon in the system tray.",
                        "TransperacyBar", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new TrayApp());
            }
        }
    }
}
