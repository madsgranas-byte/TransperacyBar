// ExplorerHook - loaded into explorer.exe through the XAML diagnostics API
// (InitializeXamlDiagnosticsEx, called by TransperacyBar.exe).
//
// On Windows 11 22H2 and later the taskbar is drawn with XAML: a rectangle named
// "BackgroundFill" paints its background and "BackgroundStroke" the border line on top.
// Blur/acrylic accents applied to the taskbar window from outside show up as solid black
// there, so this DLL does the whole appearance in XAML instead: it watches the visual tree,
// finds those rectangles inside Taskbar.TaskbarBackground, and replaces or hides their brush.
//
// TransperacyBar.exe controls it through a message-only window of class
// "TransperacyBarHook": send WM_APP_SETAPPEARANCE with the mode (see Mode) in wParam and
// the tint color as 0xAARRGGBB in lParam.

#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime // clashes with a XAML method name in the C++/WinRT headers

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    namespace wux = winrt::Windows::UI::Xaml;

    // {7C8D2E61-4B3A-4F5E-9A21-6E0B5C3D8F14} - must match TransperacyBar.cs
    constexpr CLSID CLSID_Hook = { 0x7c8d2e61, 0x4b3a, 0x4f5e, { 0x9a, 0x21, 0x6e, 0x0b, 0x5c, 0x3d, 0x8f, 0x14 } };

    constexpr wchar_t WindowClass[] = L"TransperacyBarHook";
    constexpr UINT WM_APP_SETAPPEARANCE = WM_APP + 1;

    // Values of wParam; must match TransperacyBar.cs.
    enum class Mode : int { Normal = 0, Clear = 1, Tinted = 2, Blur = 3, Acrylic = 4 };

    struct Appearance
    {
        Mode mode;
        winrt::Windows::UI::Color color;
    };

    struct ElementInfo
    {
        InstanceHandle parent;
        std::wstring type;
    };

    struct Target
    {
        wux::Shapes::Shape shape{ nullptr };
        wux::Media::Brush original{ nullptr }; // explorer's own brush, put back for Normal
        bool isFill;                           // BackgroundFill, as opposed to BackgroundStroke
        DWORD threadId;
    };

    std::mutex g_lock;
    std::unordered_map<InstanceHandle, ElementInfo> g_elements; // every XAML element we've seen, for walking up to ancestors
    std::unordered_map<InstanceHandle, Target> g_targets;       // the taskbar background rectangles
    Appearance g_appearance{ Mode::Clear, { 0, 0, 0, 0 } };
    HWND g_window = nullptr;
    HMODULE g_module = nullptr;
    thread_local bool t_applying = false; // set while we change a Fill ourselves

    wux::Media::Brush MakeBrush(const Appearance& appearance, const wux::Media::Brush& original)
    {
        switch (appearance.mode)
        {
        case Mode::Tinted:
            return wux::Media::SolidColorBrush(appearance.color);
        case Mode::Blur:
        case Mode::Acrylic:
        {
            // Backdrop (not HostBackdrop, which explorer's own brush uses) samples what's behind the
            // XAML. The taskbar window is see-through, so that's the desktop and windows behind it,
            // and unlike HostBackdrop it keeps working while the taskbar isn't the active window.
            wux::Media::AcrylicBrush brush;
            brush.BackgroundSource(wux::Media::AcrylicBackgroundSource::Backdrop);
            auto tint = appearance.color;
            tint.A = 255;
            brush.TintColor(tint);
            brush.TintOpacity(appearance.color.A / 255.0);
            // Without a luminosity layer it's a plain blur; the default gives the frosted acrylic look.
            if (appearance.mode == Mode::Blur)
                brush.TintLuminosityOpacity(winrt::Windows::Foundation::IReference<double>(0.0));
            brush.FallbackColor(appearance.color);
            return brush;
        }
        default:
            return original;
        }
    }

    // Must run on the target's UI thread.
    void ApplyNow(const Target& target, const Appearance& appearance)
    {
        t_applying = true;
        try
        {
            if (!target.isFill)
            {
                target.shape.Opacity(appearance.mode == Mode::Normal ? 1.0 : 0.0);
            }
            else
            {
                target.shape.Fill(MakeBrush(appearance, target.original));
                target.shape.Opacity(appearance.mode == Mode::Clear ? 0.0 : 1.0);
            }
        }
        catch (...)
        {
            // The element may be going away; never let an exception escape into explorer.
        }
        t_applying = false;
    }

    void ApplyTo(const Target& target, const Appearance& appearance)
    {
        try
        {
            if (target.threadId == GetCurrentThreadId())
            {
                ApplyNow(target, appearance);
            }
            else
            {
                Target copy = target;
                target.shape.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                    [copy, appearance] { ApplyNow(copy, appearance); });
            }
        }
        catch (...)
        {
        }
    }

    void ApplyAll()
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (auto& pair : g_targets)
            ApplyTo(pair.second, g_appearance);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_APP_SETAPPEARANCE)
        {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                const auto argb = static_cast<uint32_t>(lParam);
                g_appearance.mode = static_cast<Mode>(wParam);
                g_appearance.color = { static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16),
                                       static_cast<uint8_t>(argb >> 8), static_cast<uint8_t>(argb) };
            }
            ApplyAll();
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // Created on the XAML UI thread so its messages are processed by that thread's message loop.
    void EnsureWindow()
    {
        if (g_window)
            return;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = g_module;
        wc.lpszClassName = WindowClass;
        RegisterClassW(&wc);
        g_window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, nullptr);
    }

    // Caller holds g_lock.
    bool HasAncestor(InstanceHandle handle, const wchar_t* type)
    {
        for (int depth = 0; handle != 0 && depth < 64; ++depth)
        {
            auto it = g_elements.find(handle);
            if (it == g_elements.end())
                return false;
            if (it->second.type == type)
                return true;
            handle = it->second.parent;
        }
        return false;
    }

    struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2>
    {
        explicit VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diagnostics) : m_diagnostics(std::move(diagnostics)) {}

        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutation) noexcept override
        {
            try
            {
                if (mutation == Add)
                    OnAdd(relation.Parent, element);
                else if (mutation == Remove)
                    OnRemove(element.Handle);
            }
            catch (...)
            {
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
        {
            return S_OK;
        }

    private:
        void OnAdd(InstanceHandle parent, const VisualElement& element)
        {
            std::wstring type = element.Type ? element.Type : L"";
            std::wstring name = element.Name ? element.Name : L"";

            std::lock_guard<std::mutex> guard(g_lock);
            g_elements[element.Handle] = ElementInfo{ parent, type };

            if (type != L"Windows.UI.Xaml.Shapes.Rectangle" || (name != L"BackgroundFill" && name != L"BackgroundStroke"))
                return;
            if (!HasAncestor(parent, L"Taskbar.TaskbarBackground"))
                return;

            winrt::com_ptr<::IInspectable> inspectable;
            if (FAILED(m_diagnostics->GetIInspectableFromHandle(element.Handle, inspectable.put())))
                return;
            auto shape = inspectable.try_as<wux::Shapes::Shape>();
            if (!shape)
                return;

            const bool isFill = name == L"BackgroundFill";
            Target target{ shape, shape.Fill(), isFill, GetCurrentThreadId() };

            g_targets[element.Handle] = target;
            EnsureWindow();

            if (isFill)
            {
                // Explorer swaps the brush on theme changes; remember its new one and put ours back.
                const InstanceHandle handle = element.Handle;
                shape.RegisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(),
                    [handle](const wux::DependencyObject&, const wux::DependencyProperty&)
                    {
                        if (t_applying)
                            return;
                        std::lock_guard<std::mutex> guard(g_lock);
                        auto it = g_targets.find(handle);
                        if (it == g_targets.end())
                            return;
                        it->second.original = it->second.shape.Fill();
                        if (g_appearance.mode != Mode::Normal && g_appearance.mode != Mode::Clear)
                            ApplyNow(it->second, g_appearance);
                    });
            }

            ApplyTo(target, g_appearance);
        }

        void OnRemove(InstanceHandle handle)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_elements.erase(handle);
            g_targets.erase(handle);
        }

        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
    };

    winrt::com_ptr<VisualTreeWatcher> g_watcher;

    struct Hook : winrt::implements<Hook, IObjectWithSite>
    {
        HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
        {
            m_site.copy_from(site);
            if (!site)
                return S_OK;

            auto diagnostics = m_site.try_as<IXamlDiagnostics>();
            auto service = m_site.try_as<IVisualTreeService3>();
            if (!diagnostics || !service)
                return E_NOINTERFACE;

            // AdviseVisualTreeChange blocks until the current tree has been reported, which
            // deadlocks if called from inside SetSite, so do it from another thread.
            std::thread([diagnostics, service]
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                g_watcher = winrt::make_self<VisualTreeWatcher>(diagnostics);
                service->AdviseVisualTreeChange(g_watcher.get());
            }).detach();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** result) noexcept override
        {
            if (!m_site)
            {
                *result = nullptr;
                return E_FAIL;
            }
            return m_site->QueryInterface(riid, result);
        }

    private:
        winrt::com_ptr<IUnknown> m_site;
    };

    struct HookFactory : winrt::implements<HookFactory, IClassFactory>
    {
        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** result) noexcept override
        {
            *result = nullptr;
            if (outer)
                return CLASS_E_NOAGGREGATION;
            try
            {
                return winrt::make<Hook>().as<IUnknown>()->QueryInterface(riid, result);
            }
            catch (...)
            {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
        {
            return S_OK;
        }
    };
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* result)
{
    *result = nullptr;
    if (rclsid != CLSID_Hook)
        return CLASS_E_CLASSNOTAVAILABLE;
    try
    {
        return winrt::make<HookFactory>().as<IUnknown>()->QueryInterface(riid, result);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// The visual tree callbacks stay registered for the lifetime of explorer, so never unload.
__control_entrypoint(DllExport) STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}
