# TransperacyBar
project for transperacy desktopbar


this projet will create a transparent desktop bar at the bottom, instead of that boring plane one we have

## Build

Run `build.bat`. It produces:

- `bin\TransperacyBar.exe`, the tray app. It's built with the C# compiler that ships with Windows (.NET Framework 4), so it needs nothing extra.
- `bin\ExplorerHook.dll`, needed on Windows 11 22H2 and later. It's built with the Visual Studio C++ tools (`winget install Microsoft.VisualStudio.2022.BuildTools` with the "Desktop development with C++" workload). If those tools aren't installed, this step is skipped.

Once the hook has been loaded, explorer keeps `ExplorerHook.dll` locked. To rebuild it, restart explorer first.

## Install (start when you log in)

After building, run `install.bat`. It:

- copies the program to `%LOCALAPPDATA%\Programs\TransperacyBar`
- adds it to your startup apps (you can turn it off under Settings > Apps > Startup, or with *Start with Windows* in the tray menu)
- starts it

To install a newer build, run `install.bat` again. If explorer is using the old `ExplorerHook.dll`, restart explorer first. To remove the program, run `uninstall.bat`.

## Use

Start `bin\TransperacyBar.exe`. It sits in the system tray; right-click the icon to choose:

- **Clear**: fully transparent
- **Tinted**: transparent with a color (set with *Tint color...* and *Tint opacity*)
- **Blur** / **Acrylic**: blurred background
- **Normal**: the default Windows look
- **Normal when a window is maximized**: switches back to the normal look while a window is maximized on that monitor
- **Start with Windows**

Settings are saved in `%APPDATA%\TransperacyBar\settings.ini`. Choosing *Exit* restores the normal taskbar.

## How it works

The effect is applied to the taskbar windows (`Shell_TrayWnd`, `Shell_SecondaryTrayWnd`) with the undocumented `SetWindowCompositionAttribute` accent policy. Explorer resets the effect often, so the app re-applies it every 50 ms.

From Windows 11 22H2, the taskbar is drawn with XAML, and a rectangle named `BackgroundFill` paints a solid background on top of that effect. As TranslucentTB does, TransperacyBar loads `ExplorerHook.dll` into `explorer.exe` with the XAML diagnostics API (`InitializeXamlDiagnosticsEx`). The hook finds that rectangle and the `BackgroundStroke` border line, and sets their opacity to 0. The tray app controls the hook through a hidden message window, so choosing *Normal* or *Exit* brings the standard background back.

The hook is loaded again automatically if explorer restarts. It hides the background on all monitors at once, so with *Normal when a window is maximized* and several monitors, it only returns when every taskbar is set to normal.
