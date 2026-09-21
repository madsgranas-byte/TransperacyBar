// ExplorerHook - loaded into explorer.exe through the XAML diagnostics API
// (InitializeXamlDiagnosticsEx, called by TransperacyBar.exe).
//
// On Windows 11 22H2 and later the taskbar is drawn with XAML, and a rectangle named
// "BackgroundFill" paints a solid background over the taskbar window. This DLL watches
// the XAML visual tree, finds that rectangle (and the "BackgroundStroke" border line)
// inside Taskbar.TaskbarBackground, and makes them invisible so the accent effect that
// TransperacyBar.exe applies to the taskbar window shows through.
//
// TransperacyBar.exe controls it through a message-only window of class
// "TransperacyBarHook": send WM_APP_SETTRANSPARENT with wParam 1 to hide the XAML
// background or 0 to show it again.

#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime // clashes with a XAML method name in the C++/WinRT headers

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    // {7C8D2E61-4B3A-4F5E-9A21-6E0B5C3D8F14} - must match TransperacyBar.cs
    constexpr CLSID CLSID_Hook = { 0x7c8d2e61, 0x4b3a, 0x4f5e, { 0x9a, 0x21, 0x6e, 0x0b, 0x5c, 0x3d, 0x8f, 0x14 } };

    constexpr wchar_t WindowClass[] = L"TransperacyBarHook";
    constexpr UINT WM_APP_SETTRANSPARENT = WM_APP + 1;

    struct ElementInfo
    {
        InstanceHandle parent;
        std::wstring type;
    };

    struct Target
    {
        winrt::Windows::UI::Xaml::UIElement element{ nullptr };
        DWORD threadId;
    };

    std::mutex g_lock;
    std::unordered_map<InstanceHandle, ElementInfo> g_elements; // every XAML element we've seen, for walking up to ancestors
    std::unordered_map<InstanceHandle, Target> g_targets;       // the taskbar background rectangles
    bool g_transparent = true;
    HWND g_window = nullptr;
    HMODULE g_module = nullptr;

    void ApplyTo(const Target& target, bool transparent)
    {
        const double opacity = transparent ? 0.0 : 1.0;
        try
        {
            if (target.threadId == GetCurrentThreadId())
            {
                target.element.Opacity(opacity);
            }
            else
            {
                auto element = target.element;
                target.element.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                    [element, opacity] { element.Opacity(opacity); });
            }
        }
        catch (...)
        {
            // The element may be going away; never let an exception escape into explorer.
        }
    }

    void ApplyAll()
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (auto& pair : g_targets)
            ApplyTo(pair.second, g_transparent);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_APP_SETTRANSPARENT)
        {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_transparent = wParam != 0;
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
            auto uiElement = inspectable.try_as<winrt::Windows::UI::Xaml::UIElement>();
            if (!uiElement)
                return;

            Target target{ uiElement, GetCurrentThreadId() };
            g_targets[element.Handle] = target;
            EnsureWindow();
            ApplyTo(target, g_transparent);
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
