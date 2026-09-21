# TransperacyBar
project for transperacy desktopbar


this projet will create a transparent desktop bar at the bottom, instead of that boring plane one we have

## Build

Run `build.bat`. It uses the C# compiler that ships with Windows (.NET Framework 4), so nothing else needs to be installed. The result is `bin\TransperacyBar.exe`.

## Use

Start `bin\TransperacyBar.exe`. It sits in the system tray; right-click the icon to choose:

- **Clear**: fully transparent
- **Tinted**: transparent with a color (set with *Tint color...* and *Tint opacity*)
- **Blur** / **Acrylic**: blurred background
- **Normal**: the default Windows look
- **Normal when a window is maximized**: switches back to the normal look while a window is maximized on that monitor
- **Start with Windows**

Settings are saved in `%APPDATA%\TransperacyBar\settings.ini`. Choosing *Exit* restores the normal taskbar.

## Limitation on Windows 11 22H2 and later

The effect is applied with `SetWindowCompositionAttribute`, which works on Windows 10 and early Windows 11. From Windows 11 22H2, the taskbar is drawn with XAML, and that layer paints a solid background on top of the effect, so no change is visible. TranslucentTB gets around this by injecting a helper DLL into `explorer.exe` to hide that background; TransperacyBar does not do this yet.
