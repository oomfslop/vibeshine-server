/**
 * @file sunshine_wgc_capture.cpp
 * @brief Windows Graphics Capture helper process for Sunshine.
 *
 * This standalone executable provides Windows Graphics Capture functionality
 * for the main Sunshine streaming process. It runs as a separate process to
 * isolate WGC operations and handle secure desktop scenarios. The helper
 * communicates with the main process via a control pipe, shared D3D11 texture,
 * shared frame metadata, and a frame-ready event.
 */

#define WIN32_LEAN_AND_MEAN

// standard includes
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/app_window_layout.h"
#include "src/platform/windows/ipc/pipes.h"
#include "src/platform/windows/wgc_capture_policy.h"
#include "src/utility.h"  // For RAII utilities

// platform includes
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <inspectable.h>  // For IInspectable
#include <KnownFolders.h>
#include <ShellScalingApi.h>  // For DPI awareness
#include <ShlObj.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.h>
#include <dwmapi.h>
#ifndef PW_RENDERFULLCONTENT
  #define PW_RENDERFULLCONTENT 0x00000002
#endif
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>  // For ApiInformation
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Manual declaration for CreateDirect3D11DeviceFromDXGIDevice if missing
extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
}

/**
 * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
 * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
 * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
 */
struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
  __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
  IDirect3DDxgiInterfaceAccess: ::IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID id, IUnknown **object) = 0;
};

#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
};

template<>
constexpr auto __mingw_uuidof<IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}

static constexpr GUID GUID__IDirect3DSurface = {
  0x0BF4A146,
  0x13C1,
  0x4694,
  {0xBE, 0xE3, 0x7A, 0xBF, 0x15, 0xEA, 0xF5, 0x86}
};

template<>
constexpr auto __mingw_uuidof<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>() -> GUID const & {
  return GUID__IDirect3DSurface;
}
#endif

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace platf::dxgi;

// GPU scheduling priority definitions for optimal capture performance under high GPU load
enum class D3DKMT_SchedulingPriorityClass : LONG {
  IDLE = 0,
  BELOW_NORMAL = 1,
  NORMAL = 2,
  ABOVE_NORMAL = 3,
  HIGH = 4,
  REALTIME = 5
};

using PD3DKMTSetProcessSchedulingPriorityClass = LONG(__stdcall *)(HANDLE, LONG);

/**
 * @brief D3D11 device creation flags for the WGC helper process.
 *
 * NOTE: This constant is NOT shared with the main process. It is defined here separately
 * because including display.h would pull in too many dependencies for this standalone helper.
 * If you change this flag, you MUST update it in both this file and src/platform/windows/display.h.
 */
constexpr UINT D3D11_CREATE_DEVICE_FLAGS = 0;

/**
 * @brief Initial log level for the helper process.
 */
const int INITIAL_LOG_LEVEL = 2;

/**
 * @brief Global configuration data received from the main process.
 */
constexpr uint32_t DEFAULT_WGC_IPC_FLAGS =
  platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST |
  platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE;
static platf::dxgi::config_data_t g_config = {0, 0, 0, L"", {0, 0}, 10000, 60, 1, 2, DEFAULT_WGC_IPC_FLAGS, 120, 0, 0, 0};
static std::mutex g_config_mutex;
static std::condition_variable g_config_cv;
static std::atomic<int32_t> g_activity_admission_fps {120};
static std::atomic<uint32_t> g_activity_admission_generation {0};
static std::atomic<bool> g_cursor_visible {true};

/**
 * @brief Flag indicating whether configuration data has been received from main process.
 */
static bool g_config_received = false;

/**
 * @brief Global communication pipe for sending session closed notifications.
 */
static std::weak_ptr<AsyncNamedPipe> g_communication_pipe_weak;

/**
 * @brief Global Windows event hook for desktop switch detection.
 */
static safe_winevent_hook g_desktop_switch_hook = nullptr;

/**
 * @brief Flag indicating if a secure desktop has been detected.
 */
static bool g_secure_desktop_detected = false;

/**
 * @brief Set when the GraphicsCaptureItem reports it was closed (display mode change,
 * DWM restart, monitor removal). FrameArrived never fires again after this, so the
 * helper must exit and let the main process reinitialize capture.
 */
static std::atomic<bool> g_capture_item_closed {false};

/**
 * @brief Pending verification deadline for desktop switches that were not immediately secure.
 */
static std::optional<std::chrono::steady_clock::time_point> g_pending_secure_desktop_check_deadline;

/**
 * @brief Grace window for confirming secure desktop after a desktop switch event.
 *
 * EVENT_SYSTEM_DESKTOPSWITCH can fire before OpenInputDesktop reflects the final target desktop.
 * Keep a short follow-up probe window so we still notify the main process when the switch settles
 * onto secure desktop, without forcing a reinit for ordinary non-secure desktop churn.
 */
constexpr auto SECURE_DESKTOP_CONFIRMATION_WINDOW = std::chrono::seconds(1);

void notify_main_process_about_secure_desktop() {
  if (g_secure_desktop_detected) {
    return;
  }

  g_secure_desktop_detected = true;
  g_pending_secure_desktop_check_deadline.reset();
  BOOST_LOG(info) << "Secure desktop detected - sending notification to main process";

  if (auto pipe = g_communication_pipe_weak.lock(); pipe && pipe->is_connected()) {
    uint8_t msg = SECURE_DESKTOP_MSG;
    pipe->send(std::span<const uint8_t>(&msg, 1));
    BOOST_LOG(info) << "Queued desktop switch reinit notification to main process";
  } else {
    BOOST_LOG(warning) << "Desktop switch detected, but the main process pipe is not connected";
  }
}

void poll_pending_secure_desktop_transition() {
  if (!g_pending_secure_desktop_check_deadline) {
    return;
  }

  if (platf::dxgi::is_secure_desktop_active()) {
    BOOST_LOG(info) << "Secure desktop became active shortly after desktop switch";
    notify_main_process_about_secure_desktop();
    return;
  }

  if (std::chrono::steady_clock::now() >= *g_pending_secure_desktop_check_deadline) {
    BOOST_LOG(debug) << "Desktop switch settled without entering secure desktop; ignoring reinit";
    g_pending_secure_desktop_check_deadline.reset();
  }
}

/**
 * @brief System initialization class to handle DPI, threading, and MMCSS setup.
 *
 * This class manages critical system-level initialization for optimal capture performance:
 * - Sets DPI awareness to handle high-DPI displays correctly
 * - Elevates thread priority for better capture timing
 * - Configures MMCSS (Multimedia Class Scheduler Service) for audio/video workloads
 * - Initializes WinRT apartment for Windows Graphics Capture APIs
 */
class SystemInitializer {
private:
  safe_mmcss_handle _mmcss_handle = nullptr;  ///< Handle for MMCSS thread characteristics
  bool _dpi_awareness_set = false;  ///< Flag indicating if DPI awareness was successfully set
  bool _thread_priority_set = false;  ///< Flag indicating if thread priority was elevated
  bool _mmcss_characteristics_set = false;  ///< Flag indicating if MMCSS characteristics were set
  bool _gpu_priority_set = false;  ///< Flag indicating if GPU scheduling priority was set

public:
  /**
   * @brief Initializes DPI awareness for the process.
   *
   * Attempts to set per-monitor DPI awareness to handle high-DPI displays correctly.
   * First tries the newer SetProcessDpiAwarenessContext API, then falls back to
   * SetProcessDpiAwareness if the newer API is not available.
   *
   * @return true if DPI awareness was successfully set, false otherwise.
   */
  bool initialize_dpi_awareness() {
    // Try newer API first
    if (HMODULE user32_module = GetModuleHandleA("user32.dll")) {
      using set_process_dpi_awareness_context_fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
      auto set_process_dpi_awareness_context =
        reinterpret_cast<set_process_dpi_awareness_context_fn>(
          GetProcAddress(user32_module, "SetProcessDpiAwarenessContext")
        );

      if (set_process_dpi_awareness_context &&
          set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        _dpi_awareness_set = true;
        return true;
      }
    }

    // Fallback to older API (Win 8.1+)
    if (SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
      _dpi_awareness_set = true;
      return true;
    }

    BOOST_LOG(warning) << "Failed to set DPI awareness, display scaling issues may occur";
    return false;
  }

  /**
   * @brief Elevates the current thread priority for better capture performance.
   *
   * Sets the thread priority to THREAD_PRIORITY_HIGHEST to reduce latency
   * and improve capture frame timing consistency.
   *
   * @return true if thread priority was successfully elevated, false otherwise.
   */
  bool initialize_thread_priority() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
      BOOST_LOG(error) << "Failed to set thread priority: " << GetLastError();
      return false;
    }
    _thread_priority_set = true;
    return true;
  }

  /**
   * @brief Configures MMCSS (Multimedia Class Scheduler Service) characteristics.
   *
   * Registers the thread with MMCSS for multimedia workload scheduling.
   * First attempts "Pro Audio" task profile, then falls back to "Games" profile.
   * This helps ensure consistent timing for capture operations.
   * Additionally sets MMCSS thread relative priority to maximum for best performance.
   *
   * @return true if MMCSS characteristics were successfully set, false otherwise.
   */
  bool initialize_mmcss_characteristics() {
    DWORD task_idx = 0;
    HANDLE raw_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    if (!raw_handle) {
      raw_handle = AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
      if (!raw_handle) {
        BOOST_LOG(error) << "Failed to set MMCSS characteristics: " << GetLastError();
        return false;
      }
    }

    // Set MMCSS thread relative priority to maximum (AVRT_PRIORITY_HIGH = 2)
    if (!AvSetMmThreadPriority(raw_handle, AVRT_PRIORITY_HIGH)) {
      BOOST_LOG(warning) << "Failed to set MMCSS thread priority: " << GetLastError();
      // Don't fail completely as the basic MMCSS registration still works
    }

    _mmcss_handle.reset(raw_handle);
    _mmcss_characteristics_set = true;
    return true;
  }

  /**
   * @brief Sets GPU scheduling priority for optimal capture performance under high GPU load.
   *
   * Tries REALTIME, then HIGH if the first request fails (for example, when the
   * interactive user's token lacks the privilege required for REALTIME).
   * Priority boosts are optional; capture can continue at the existing priority.
   *
   * @return true if GPU priority was successfully set, false otherwise.
   */
  bool initialize_gpu_scheduling_priority() {
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) {
      BOOST_LOG(warning) << "Failed to get gdi32.dll handle for GPU priority adjustment";
      return false;
    }

    auto d3dkmt_set_process_priority = reinterpret_cast<PD3DKMTSetProcessSchedulingPriorityClass>(GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!d3dkmt_set_process_priority) {
      BOOST_LOG(warning) << "D3DKMTSetProcessSchedulingPriorityClass not available, GPU priority not set";
      return false;
    }

    for (const auto priority : {D3DKMT_SchedulingPriorityClass::REALTIME, D3DKMT_SchedulingPriorityClass::HIGH}) {
      const auto priority_name = priority == D3DKMT_SchedulingPriorityClass::REALTIME ? "REALTIME" : "HIGH";
      const LONG status = d3dkmt_set_process_priority(GetCurrentProcess(), static_cast<LONG>(priority));
      if (status >= 0) {
        BOOST_LOG(info) << "GPU scheduling priority set to " << priority_name << " for optimal capture performance";
        _gpu_priority_set = true;
        return true;
      }

      BOOST_LOG(warning) << "Failed to set GPU scheduling priority to " << priority_name << ": " << status;
    }

    BOOST_LOG(warning) << "GPU scheduling priority boost unavailable; continuing capture at the existing priority";
    return false;
  }

  /**
   * @brief Initializes WinRT apartment for Windows Graphics Capture APIs.
   *
   * Sets up the WinRT apartment as multi-threaded to support Windows Graphics
   * Capture operations from background threads.
   *
   * @return true always (WinRT initialization typically succeeds).
   */
  bool initialize_winrt_apartment() const {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    return true;
  }

  /**
   * @brief Initializes all system components for optimal capture performance.
   *
   * Calls all initialization methods in sequence:
   * - DPI awareness configuration
   * - Thread priority elevation
   * - GPU scheduling priority setup
   * - MMCSS characteristics setup
   * - WinRT apartment initialization
   *
   * DPI awareness and GPU scheduling priority are best-effort. Windows may have
   * already selected DPI awareness, and the user's token may deny GPU priority boosts.
   * @return true if all required initialization steps succeeded, false if any required step failed.
   */
  bool initialize_all() {
    bool success = true;
    initialize_dpi_awareness();
    success &= initialize_thread_priority();
    initialize_gpu_scheduling_priority();
    success &= initialize_mmcss_characteristics();
    success &= initialize_winrt_apartment();
    return success;
  }

  /**
   * @brief Checks if DPI awareness was successfully set.
   * @return true if DPI awareness is configured, false otherwise.
   */
  bool is_dpi_awareness_set() const {
    return _dpi_awareness_set;
  }

  /**
   * @brief Checks if thread priority was successfully elevated.
   * @return true if thread priority is elevated, false otherwise.
   */
  bool is_thread_priority_set() const {
    return _thread_priority_set;
  }

  /**
   * @brief Checks if MMCSS characteristics were successfully set.
   * @return true if MMCSS characteristics are configured, false otherwise.
   */
  bool is_mmcss_characteristics_set() const {
    return _mmcss_characteristics_set;
  }

  /**
   * @brief Checks if GPU scheduling priority was successfully set.
   * @return true if GPU scheduling priority is configured, false otherwise.
   */
  bool is_gpu_priority_set() const {
    return _gpu_priority_set;
  }

  /**
   * @brief Destructor for SystemInitializer.
   *
   * RAII automatically releases MMCSS resources.
   */
  ~SystemInitializer() noexcept = default;
};

/**
 * @brief D3D11 device management class to handle device creation and WinRT interop.
 *
 * This class manages D3D11 device and context creation, as well as the WinRT
 * interop device required for Windows Graphics Capture. It handles the complex
 * process of bridging between traditional D3D11 APIs and WinRT capture APIs.
 */
class D3D11DeviceManager {
private:
  winrt::com_ptr<ID3D11Device> _device;  ///< D3D11 device for graphics operations
  winrt::com_ptr<ID3D11DeviceContext> _context;  ///< D3D11 device context for rendering
  D3D_FEATURE_LEVEL _feature_level;  ///< D3D feature level supported by the device
  winrt::com_ptr<IDXGIDevice> _dxgi_device;  ///< DXGI device interface for WinRT interop
  winrt::com_ptr<::IDirect3DDevice> _interop_device;  ///< Intermediate interop device
  IDirect3DDevice _winrt_device = nullptr;  ///< WinRT Direct3D device for capture integration

public:
  /**
   * @brief Creates a D3D11 device and context for graphics operations.
   *
   * Creates a hardware-accelerated D3D11 device using the specified adapter.
   * The device is used for texture operations and WinRT interop.
   * Also sets GPU thread priority to 7 for optimal capture performance.
   * Uses the same D3D11_CREATE_DEVICE_FLAGS as the main Sunshine process.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if device creation succeeded, false otherwise.
   */
  bool create_device(const LUID &adapter_luid) {
    // Feature levels to try, matching the main process
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    // Find the adapter matching the LUID
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0) {
      // Non-zero LUID provided, find the matching adapter
      winrt::com_ptr<IDXGIFactory1> factory;
      HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
      if (FAILED(hr)) {
        BOOST_LOG(error) << "Failed to create DXGI factory for adapter lookup";
        return false;
      }

      IDXGIAdapter1 *raw_adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &raw_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIAdapter1> test_adapter;
        test_adapter.attach(raw_adapter);
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(test_adapter->GetDesc1(&desc))) {
          if (desc.AdapterLuid.HighPart == adapter_luid.HighPart &&
              desc.AdapterLuid.LowPart == adapter_luid.LowPart) {
            adapter = test_adapter;
            BOOST_LOG(info) << "Found matching adapter: " << wide_to_utf8(desc.Description);
            break;
          }
        }
      }

      if (!adapter) {
        BOOST_LOG(warning) << "Could not find adapter with LUID "
                           << std::hex << adapter_luid.HighPart << ":" << adapter_luid.LowPart
                           << std::dec << ", using default adapter";
      }
    } else {
      BOOST_LOG(info) << "Using default adapter (no LUID specified)";
    }

    // Create the D3D11 device using the same flags as the main process
    HRESULT hr = D3D11CreateDevice(
      adapter.get(),  // nullptr if no specific adapter found
      adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      ARRAYSIZE(featureLevels),
      D3D11_SDK_VERSION,
      _device.put(),
      &_feature_level,
      _context.put()
    );

    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create D3D11 device: " << std::hex << hr << std::dec;
      return false;
    }

    // Set GPU thread priority to 7 for optimal capture performance under high GPU load
    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (_dxgi_device) {
      hr = _dxgi_device->SetGPUThreadPriority(7);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to set GPU thread priority to 7: " << hr
                           << " (may require administrator privileges for optimal performance)";
      } else {
        BOOST_LOG(info) << "GPU thread priority set to 7 for optimal capture performance";
      }
    } else {
      BOOST_LOG(warning) << "Failed to query DXGI device for GPU thread priority setting";
    }

    return true;
  }

  /**
   * @brief Creates WinRT interop device from the D3D11 device.
   *
   * Bridges the D3D11 device to WinRT APIs required for Windows Graphics Capture.
   * This involves:
   * - Querying DXGI device interface from D3D11 device
   * - Creating Direct3D interop device using WinRT factory
   * - Converting to WinRT IDirect3DDevice interface
   *
   * @return true if WinRT interop device creation succeeded, false otherwise.
   */
  bool create_winrt_interop() {
    if (!_device) {
      return false;
    }

    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (!_dxgi_device) {
      BOOST_LOG(error) << "Failed to get DXGI device";
      return false;
    }

    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(_dxgi_device.get(), reinterpret_cast<::IInspectable **>(winrt::put_abi(_interop_device)));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create interop device";
      return false;
    }

    _winrt_device = _interop_device.try_as<IDirect3DDevice>();
    if (!_winrt_device) {
      BOOST_LOG(error) << "Failed to query IDirect3DDevice from interop device";
      return false;
    }
    return true;
  }

  /**
   * @brief Initializes the D3D11 device and WinRT interop device.
   *
   * Calls create_device() with the specified adapter LUID, then create_winrt_interop() to create the WinRT interop device.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if both device and interop device are successfully created, false otherwise.
   */
  bool initialize_all(const LUID &adapter_luid) {
    return create_device(adapter_luid) && create_winrt_interop();
  }

  /**
   * @brief Gets the underlying D3D11 device com_ptr.
   *
   * @return Pointer to the managed ID3D11Device, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11Device> &get_device() const {
    return _device;
  }

  /**
   * @brief Gets the underlying D3D11 device context com_ptr.
   *
   * @return Pointer to the managed ID3D11DeviceContext, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11DeviceContext> &get_context() const {
    return _context;
  }

  /**
   * @brief Gets the WinRT IDirect3DDevice for Windows Graphics Capture interop.
   *
   * @return The WinRT IDirect3DDevice, or nullptr if not initialized.
   */
  IDirect3DDevice get_winrt_device() const {
    return _winrt_device;
  }

  /**
   * @brief Destructor for D3D11DeviceManager.
   *
   * RAII automatically releases device and context resources.
   */
  ~D3D11DeviceManager() noexcept = default;
};

/**
 * @brief Monitor and display management class to handle monitor enumeration and selection.
 *
 * This class manages monitor detection, selection, and configuration for capture operations.
 * It handles scenarios with multiple monitors and provides resolution configuration based
 * on monitor capabilities and user preferences.
 */
class DisplayManager {
private:
  HMONITOR _selected_monitor = nullptr;  ///< Handle to the selected monitor for capture
  HWND _target_hwnd = nullptr;  ///< App-window capture target (nullptr = monitor capture)
  MONITORINFO _monitor_info = {sizeof(MONITORINFO)};  ///< Information about the selected monitor
  UINT _width = 0;  ///< Final capture width in pixels
  UINT _height = 0;  ///< Final capture height in pixels

public:
  /**
   * @brief Selects a monitor for capture based on the provided configuration.
   *
   * If a display name is specified, waits briefly for that monitor to appear.
   * Selects the primary monitor only when no display name is provided.
   *
   * @param config The configuration data containing the desired display name (if any).
   * @return true if a monitor was successfully selected; false if monitor selection failed.
   */
  bool select_monitor(const platf::dxgi::config_data_t &config) {
    if (config.target_hwnd) {
      // App-window capture: the window's monitor only matters as a fallback.
      _target_hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(config.target_hwnd));
      if (!IsWindow(_target_hwnd)) {
        BOOST_LOG(error) << "App-window capture target 0x" << std::hex << config.target_hwnd << std::dec << " is not a window";
        return false;
      }
      _selected_monitor = MonitorFromWindow(_target_hwnd, MONITOR_DEFAULTTONEAREST);
      return _selected_monitor != nullptr;
    }
    auto find_requested_monitor = [&]() -> HMONITOR {
      struct EnumData {
        const wchar_t *target_name;
        HMONITOR found_monitor;
      };

      EnumData enum_data = {config.display_name, nullptr};

      auto enum_proc = +[](HMONITOR h_mon, HDC /*hdc*/, RECT * /*rc*/, LPARAM l_param) {
        auto *data = static_cast<EnumData *>(reinterpret_cast<void *>(l_param));
        if (MONITORINFOEXW m_info = {sizeof(MONITORINFOEXW)}; GetMonitorInfoW(h_mon, &m_info) && wcsncmp(m_info.szDevice, data->target_name, 32) == 0) {
          data->found_monitor = h_mon;
          return FALSE;  // Stop enumeration
        }
        return TRUE;
      };

      EnumDisplayMonitors(nullptr, nullptr, enum_proc, static_cast<LPARAM>(reinterpret_cast<std::uintptr_t>(&enum_data)));
      return enum_data.found_monitor;
    };

    // Monitor enumeration can lag during virtual display topology transitions.
    // If the bounded wait expires, let the host reinitialize capture instead of
    // streaming another display with the intended target's input coordinates.
    const bool has_requested_monitor = config.display_name[0] != L'\0';
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    _selected_monitor = platf::dxgi::wgc_policy::select_monitor(
      has_requested_monitor,
      find_requested_monitor,
      [] { return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY); },
      [&] {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
      }
    );

    if (!_selected_monitor) {
      if (has_requested_monitor) {
        BOOST_LOG(warning) << "Could not find requested monitor '" << winrt::to_string(config.display_name)
                           << "'; capture must be reinitialized when the target is available.";
      } else {
        BOOST_LOG(error) << "Failed to get primary monitor";
      }
      return false;
    }
    return true;
  }

  /**
   * @brief Retrieves information about the currently selected monitor.
   *
   * This function queries the Windows API to obtain monitor information for the selected monitor,
   * including its dimensions. The width and height are stored as fallback values for later use.
   *
   * @return true if monitor information was successfully retrieved; false otherwise.
   */
  bool get_monitor_info() {
    if (!_selected_monitor) {
      return false;
    }

    if (!GetMonitorInfo(_selected_monitor, &_monitor_info)) {
      BOOST_LOG(error) << "Failed to get monitor info";
      return false;
    }

    return true;
  }

  /**
   * @brief Creates a Windows Graphics Capture item for the selected monitor.
   *
   * Uses the IGraphicsCaptureItemInterop interface to create a GraphicsCaptureItem
   * corresponding to the currently selected monitor. This item is required for
   * initiating Windows Graphics Capture sessions.
   *
   * @param[out] item Reference to a GraphicsCaptureItem that will be set on success.
   * @return true if the GraphicsCaptureItem was successfully created; false otherwise.
   */
  bool create_graphics_capture_item(GraphicsCaptureItem &item) {
    if (!_selected_monitor) {
      return false;
    }

    auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = activation_factory->CreateForMonitor(_selected_monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem for monitor: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Calculates the final capture resolution based on config, monitor info, and WGC item size.
   *
   * Chooses resolution in this order:
   *
   * - Uses config width/height if valid.
   *
   * - Otherwise uses monitor logical size.
   *
   * - If WGC item size differs significantly (DPI scaling), uses WGC physical size to avoid cropping/zoom.
   *
   * @param config The configuration data with requested width/height.
   * @param config_received True if config data was received.
   * @param item The GraphicsCaptureItem for the selected monitor.
   */
  void configure_capture_resolution(const GraphicsCaptureItem &item) {
    // Get actual WGC item size to ensure we capture full desktop (fixes zoomed display issue)
    auto item_size = item.Size();
    _height = item_size.Height;
    _width = item_size.Width;
  }

  /**
   * @brief Gets the handle to the selected monitor.
   * @return HMONITOR handle of the selected monitor, or nullptr if none selected.
   */
  HMONITOR get_selected_monitor() const {
    return _selected_monitor;
  }

  /**
   * @brief Gets the configured capture width.
   * @return Capture width in pixels.
   */
  UINT get_width() const {
    return _width;
  }

  /**
   * @brief Gets the configured capture height.
   * @return Capture height in pixels.
   */
  UINT get_height() const {
    return _height;
  }
};

/**
 * @brief Shared resource management class to handle texture, memory mapping, and events.
 *
 * This class manages shared D3D11 resources for inter-process communication with the main
 * Sunshine process. It creates a shared texture with a keyed mutex plus a shared metadata
 * view and frame-ready event for realtime cross-process delivery.
 */
class SharedResourceManager {
private:
  winrt::com_ptr<ID3D11Texture2D> _shared_texture;  ///< Shared D3D11 texture for frame data
  winrt::com_ptr<IDXGIKeyedMutex> _keyed_mutex;  ///< Keyed mutex for synchronization
  winrt::handle _shared_handle;  ///< Shared handle for cross-process sharing
  winrt::handle _frame_ready_event;  ///< Auto-reset event signaled after each published frame
  winrt::handle _frame_metadata_mapping;  ///< Shared memory containing latest frame metadata
  platf::dxgi::frame_metadata_t *_frame_metadata = nullptr;  ///< Mapped frame metadata view
  UINT _width = 0;  ///< Texture width in pixels
  UINT _height = 0;  ///< Texture height in pixels

public:
  /**
   * @brief Default constructor for SharedResourceManager.
   */
  SharedResourceManager() = default;

  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  SharedResourceManager(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  SharedResourceManager &operator=(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  SharedResourceManager(SharedResourceManager &&) = delete;

  /**
   * @brief Deleted move assignment operator to prevent resource transfer issues.
   */
  SharedResourceManager &operator=(SharedResourceManager &&) = delete;

  /**
   * @brief Creates a shared D3D11 texture with keyed mutex for inter-process sharing.
   *
   * @param device Pointer to the D3D11 device used for texture creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if the texture was successfully created; false otherwise.
   */
  bool create_shared_texture(const winrt::com_ptr<ID3D11Device> &device, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    _width = texture_width;
    _height = texture_height;

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = _width;
    tex_desc.Height = _height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // Use NT shared handles exclusively
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, _shared_texture.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create NT shared texture: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Acquires a keyed mutex interface from the shared texture for synchronization.
   * @return true if the keyed mutex was successfully acquired; false otherwise.
   */
  bool create_keyed_mutex() {
    if (!_shared_texture) {
      return false;
    }

    _keyed_mutex = _shared_texture.try_as<IDXGIKeyedMutex>();
    if (!_keyed_mutex) {
      BOOST_LOG(error) << "Failed to get keyed mutex";
      return false;
    }
    return true;
  }

  /**
   * @brief Creates a shared handle for the texture resource.
   *
   * @return true if the handle was successfully created; false otherwise.
   */
  bool create_shared_handle() {
    if (!_shared_texture) {
      BOOST_LOG(error) << "Cannot create shared handle - no shared texture available";
      return false;
    }

    winrt::com_ptr<IDXGIResource1> dxgi_resource1 = _shared_texture.try_as<IDXGIResource1>();
    if (!dxgi_resource1) {
      BOOST_LOG(error) << "Failed to query DXGI resource1 interface";
      return false;
    }

    // Create the shared handle
    HRESULT hr = dxgi_resource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, _shared_handle.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create shared handle: " << hr;
      return false;
    }
    return true;
  }

  bool create_frame_signal() {
    _frame_ready_event = winrt::handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!_frame_ready_event) {
      BOOST_LOG(error) << "Failed to create frame-ready event: " << GetLastError();
      return false;
    }

    _frame_metadata_mapping = winrt::handle(CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      nullptr,
      PAGE_READWRITE,
      0,
      static_cast<DWORD>(sizeof(platf::dxgi::frame_metadata_t)),
      nullptr
    ));
    if (!_frame_metadata_mapping) {
      BOOST_LOG(error) << "Failed to create frame metadata mapping: " << GetLastError();
      return false;
    }

    _frame_metadata = static_cast<platf::dxgi::frame_metadata_t *>(MapViewOfFile(
      _frame_metadata_mapping.get(),
      FILE_MAP_READ | FILE_MAP_WRITE,
      0,
      0,
      sizeof(platf::dxgi::frame_metadata_t)
    ));
    if (!_frame_metadata) {
      BOOST_LOG(error) << "Failed to map frame metadata view: " << GetLastError();
      return false;
    }

    _frame_metadata->sequence = 0;
    _frame_metadata->frame_id = 0;
    _frame_metadata->frame_qpc = 0;
    return true;
  }

  /**
   * @brief Initializes all shared resource components: texture, keyed mutex, and handle.
   *
   * @param device Pointer to the D3D11 device used for resource creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if all resources were successfully initialized; false otherwise.
   */
  bool initialize_all(const winrt::com_ptr<ID3D11Device> &device, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    return create_shared_texture(device, texture_width, texture_height, format) &&
           create_keyed_mutex() &&
           create_shared_handle() &&
           create_frame_signal();
  }

  /**
   * @brief Gets the shared handle data for inter-process sharing.
   * @return shared_handle_data_t struct containing the shared handle and texture dimensions.
   */
  platf::dxgi::shared_handle_data_t get_shared_handle_data() const {
    platf::dxgi::shared_handle_data_t data = {};
    data.texture_handle = const_cast<HANDLE>(_shared_handle.get());
    data.frame_event_handle = const_cast<HANDLE>(_frame_ready_event.get());
    data.frame_metadata_handle = const_cast<HANDLE>(_frame_metadata_mapping.get());
    data.width = _width;
    data.height = _height;
    return data;
  }

  void publish_frame_metadata(uint64_t frame_qpc) {
    if (!_frame_metadata) {
      return;
    }

    InterlockedIncrement64(&_frame_metadata->sequence);
    InterlockedExchange64(&_frame_metadata->frame_qpc, static_cast<LONG64>(frame_qpc));
    InterlockedIncrement64(&_frame_metadata->frame_id);
    InterlockedIncrement64(&_frame_metadata->sequence);
  }

  void signal_frame_ready() {
    if (!_frame_ready_event) {
      return;
    }

    SetEvent(_frame_ready_event.get());
  }

  /**
   * @brief Gets the underlying shared D3D11 texture pointer.
   * @return Pointer to the managed ID3D11Texture2D, or nullptr if not initialized.
   */
  const winrt::com_ptr<ID3D11Texture2D> &get_shared_texture() const {
    return _shared_texture;
  }

  /**
   * @brief Gets the keyed mutex interface com_ptr for the shared texture.
   * @return const winrt::com_ptr<IDXGIKeyedMutex>& (may be empty if not initialized).
   */
  const winrt::com_ptr<IDXGIKeyedMutex> &get_keyed_mutex() const {
    return _keyed_mutex;
  }

  /**
   * @brief Destructor for SharedResourceManager.
   * Uses RAII to automatically dispose resources
   */
  ~SharedResourceManager() {
    if (_frame_metadata) {
      UnmapViewOfFile(_frame_metadata);
      _frame_metadata = nullptr;
    }
  }
};

/**
 * @brief WGC capture management class to handle frame pool, capture session, and frame processing.
 *
 * This class manages the core Windows Graphics Capture functionality including:
 * - Frame pool creation and management with dynamic buffer sizing
 * - Capture session lifecycle management
 * - Frame arrival event handling and processing
 * - Frame rate optimization and adaptive buffering
 * - Integration with shared texture resources and event signaling for inter-process communication
 */
struct WgcCaptureDependencies {
  // Required devices/resources
  IDirect3DDevice winrt_device;  // WinRT Direct3D device (value-type COM handle)
  GraphicsCaptureItem graphics_item;  // Target capture item
  SharedResourceManager &resource_manager;  // Shared inter-process texture/mutex manager
  winrt::com_ptr<ID3D11DeviceContext> d3d_context;  // D3D11 context for copies
};

class WgcCaptureManager {
private:
  struct frame_callback_state_t {
    std::mutex mutex;
    std::condition_variable drained;
    WgcCaptureManager *owner = nullptr;
    std::size_t active = 0;
    bool shutting_down = false;
  };

  enum class scratch_state_e {
    free,
    reserved,
    pending,
    delivering
  };

  struct scratch_texture_t {
    winrt::com_ptr<ID3D11Texture2D> texture;
    scratch_state_e state = scratch_state_e::free;
  };

  struct delivery_frame_t {
    winrt::com_ptr<ID3D11Texture2D> texture;
    size_t scratch_index = 0;
    uint64_t frame_qpc = 0;
  };

  static constexpr auto WGC_DIAGNOSTICS_LOG_INTERVAL = std::chrono::seconds(15);
  static constexpr auto WGC_DRAIN_LOG_INTERVAL = std::chrono::seconds(5);

  std::atomic<bool> _shutting_down {false};
  std::shared_ptr<frame_callback_state_t> _frame_callback_state;
  Direct3D11CaptureFramePool _frame_pool = nullptr;  ///< WinRT frame pool for capture operations
  GraphicsCaptureSession _capture_session = nullptr;  ///< WinRT capture session for monitor/window capture
  std::optional<bool> _last_cursor_capture_request;  ///< Avoid repeated WinRT calls/logs for unchanged requests.
  winrt::event_token _frame_arrived_token {};  ///< Event token for frame arrival notifications
  std::optional<WgcCaptureDependencies> _deps;  ///< Dependencies for frame processing

  uint32_t _initial_buffer_size = 1;  ///< Minimum steady-state frame buffer size for this stream
  uint32_t _current_buffer_size = 1;  ///< Current frame buffer size for dynamic adjustment
  uint32_t _max_buffer_size = 4;  ///< Maximum allowed buffer size for adaptive growth
  static constexpr uint32_t ABSOLUTE_MAX_BUFFER_SIZE = 4;  ///< Hard cap to bound latency/VRAM

  std::deque<std::chrono::steady_clock::time_point> _drop_timestamps;  ///< Timestamps of recent frame drops for analysis
  std::atomic<int> _outstanding_frames {0};  ///< Number of frames currently being processed
  std::atomic<int> _peak_outstanding {0};  ///< Peak number of outstanding frames (for monitoring)
  std::atomic<int64_t> _last_frame_pool_pressure_us {0};  ///< Last source drop/drain pressure signal
  std::atomic<int64_t> _last_drain_log_us {0};  ///< Last rate-limited drain log timestamp
  std::atomic<int64_t> _last_diagnostics_log_us {0};  ///< Last aggregate diagnostics log timestamp
  std::chrono::steady_clock::time_point _last_quiet_start = std::chrono::steady_clock::now();  ///< Last time frame processing became quiet
  std::chrono::steady_clock::time_point _last_buffer_check = std::chrono::steady_clock::now();  ///< Last time buffer size was checked
  std::atomic<uint64_t> _captured_frames {0};
  std::atomic<uint64_t> _frame_pool_empty_drops {0};
  std::atomic<uint64_t> _drained_pool_frames {0};
  std::atomic<uint64_t> _slow_context_waits {0};
  std::atomic<uint64_t> _slow_mutex_waits {0};
  std::atomic<uint64_t> _slow_shared_mutex_holds {0};
  std::atomic<uint64_t> _slow_copy_submissions {0};
  std::atomic<uint64_t> _published_frames {0};
  std::atomic<uint64_t> _activity_rate_limited_frames {0};
  uint64_t _last_diagnostics_captured_frames = 0;
  uint64_t _last_diagnostics_published_frames = 0;
  uint64_t _last_diagnostics_empty_drops = 0;
  uint64_t _last_diagnostics_drained_frames = 0;
  uint64_t _last_diagnostics_replaced_frames = 0;
  uint64_t _last_diagnostics_scratch_dropped = 0;
  uint64_t _last_diagnostics_slow_context = 0;
  uint64_t _last_diagnostics_slow_mutex = 0;
  uint64_t _last_diagnostics_slow_hold = 0;
  uint64_t _last_diagnostics_slow_copy = 0;
  uint64_t _last_diagnostics_activity_rate_limited = 0;
  uint32_t _activity_admission_generation = 0;
  platf::dxgi::wgc_policy::source_admission_t _activity_admission;
  std::mutex _delivery_mutex;
  std::condition_variable _delivery_cv;
  std::jthread _delivery_thread;
  std::optional<delivery_frame_t> _pending_delivery_frame;
  bool _delivery_stop = false;
  std::mutex _d3d_context_mutex;
  std::array<scratch_texture_t, 3> _scratch_textures;
  winrt::com_ptr<ID3D11RenderTargetView> _shared_rtv;  ///< app-window canvas clears
  UINT _last_canvas_copy_w = 0, _last_canvas_copy_h = 0;
  std::atomic<uint64_t> _delivery_backpressure_waits {0};
  std::atomic<uint64_t> _delivery_replaced_frames {0};
  std::atomic<uint64_t> _scratch_dropped_frames {0};
  DXGI_FORMAT _capture_format = DXGI_FORMAT_UNKNOWN;  ///< DXGI format for captured frames
  UINT _height = 0;  ///< Capture height in pixels
  UINT _width = 0;  ///< Capture width in pixels

public:
  /**
   * @brief Constructor for WgcCaptureManager.
   *
   * Initializes the capture manager for the given dimensions and capture format and
   * stores the dependencies bundle used during capture operations.
   *
   * @param capture_format DXGI format for captured frames.
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   * @param deps Bundle of dependencies: winrt IDirect3DDevice, GraphicsCaptureItem,
   *             reference to SharedResourceManager and D3D11 context com_ptr.
   */
  WgcCaptureManager(DXGI_FORMAT capture_format, UINT width, UINT height, WgcCaptureDependencies deps):
      _deps(std::move(deps)),
      _capture_format(capture_format),
      _height(height),
      _width(width) {
    _frame_callback_state = std::make_shared<frame_callback_state_t>();
    _frame_callback_state->owner = this;
    _max_buffer_size = std::clamp<uint32_t>(
      g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : ABSOLUTE_MAX_BUFFER_SIZE,
      1,
      ABSOLUTE_MAX_BUFFER_SIZE
    );
    _initial_buffer_size = std::clamp<uint32_t>(
      g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
      1,
      _max_buffer_size
    );
    _current_buffer_size = _initial_buffer_size;

    const auto now = std::chrono::steady_clock::now();
    _last_quiet_start = now;
    _last_buffer_check = now;
    _last_diagnostics_log_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    _delivery_thread = std::jthread([this](std::stop_token stop_token) {
      delivery_thread_proc(stop_token);
    });
  }

  /**
   * @brief Destructor for WgcCaptureManager.
   *
   * Automatically cleans up capture session and frame pool resources.
   */
  ~WgcCaptureManager() noexcept {
    shutdown();
  }

  void shutdown() noexcept {
    if (_shutting_down.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    const auto callback_state = _frame_callback_state;
    {
      std::lock_guard lock(callback_state->mutex);
      callback_state->shutting_down = true;
    }
    revoke_frame_arrived_handler();

    // The handler captures callback_state by shared ownership. A callback that
    // was dispatched before revocation may still enter after this object is
    // destroyed, but it will observe shutting_down without touching owner.
    // Callbacks admitted before shutdown keep owner valid until they leave.
    {
      std::unique_lock lock(callback_state->mutex);
      while (callback_state->active != 0) {
        if (!callback_state->drained.wait_for(lock, std::chrono::seconds(2), [&]() {
              return callback_state->active == 0;
            })) {
          BOOST_LOG(warning) << "Waiting for " << callback_state->active
                             << " in-flight frame(s) to finish before shutdown...";
        }
      }
      callback_state->owner = nullptr;
    }

    cleanup_capture_session();
    cleanup_frame_pool();
    stop_delivery_thread();
  }

private:
  void stop_delivery_thread() noexcept {
    {
      std::lock_guard lock(_delivery_mutex);
      _delivery_stop = true;
      _pending_delivery_frame.reset();
      for (auto &scratch: _scratch_textures) {
        scratch.state = scratch_state_e::free;
      }
    }

    _delivery_cv.notify_all();
    if (_delivery_thread.joinable()) {
      _delivery_thread.request_stop();
      _delivery_thread.join();
    }
  }

  void cleanup_capture_session() noexcept {
    try {
      if (_capture_session) {
        _capture_session.Close();
        _capture_session = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _capture_session.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _capture_session.Close()";
    }
  }

  void revoke_frame_arrived_handler() noexcept {
    try {
      if (_frame_pool && _frame_arrived_token.value != 0) {
        _frame_pool.FrameArrived(_frame_arrived_token);
        _frame_arrived_token.value = 0;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception removing FrameArrived handler: " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception removing FrameArrived handler";
    }
  }

  void cleanup_frame_pool() noexcept {
    try {
      if (_frame_pool) {
        revoke_frame_arrived_handler();
        _frame_pool.Close();
        _frame_pool = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _frame_pool.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _frame_pool.Close()";
    }
  }

public:
  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  WgcCaptureManager(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  WgcCaptureManager &operator=(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  WgcCaptureManager(WgcCaptureManager &&) = delete;
  WgcCaptureManager &operator=(WgcCaptureManager &&) = delete;

  /**
   * @brief Recreates the frame pool with a new buffer size for dynamic adjustment.
   * @param buffer_size The number of frames to buffer.
   * @returns true if the frame pool was recreated successfully, false otherwise.
   */
  bool create_or_adjust_frame_pool(uint32_t buffer_size) {
    if (!_deps || !_deps->winrt_device || _capture_format == DXGI_FORMAT_UNKNOWN) {
      return false;
    }

    if (_frame_pool) {
      // Use the proper Recreate method instead of closing and re-creating
      try {
        _frame_pool.Recreate(
          _deps->winrt_device,
          (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
          buffer_size,
          SizeInt32 {static_cast<int32_t>(_width), static_cast<int32_t>(_height)}
        );

        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool recreated with buffer size: " << buffer_size;
        return true;
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "Failed to recreate frame pool: " << ex.code() << " - " << winrt::to_string(ex.message());
        return false;
      }
    } else {
      // Initial creation case - create new frame pool
      _frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        _deps->winrt_device,
        (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
        buffer_size,
        SizeInt32 {static_cast<int32_t>(_width), static_cast<int32_t>(_height)}
      );

      if (_frame_pool) {
        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool created with buffer size: " << buffer_size;
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Processes a frame when the frame arrived event is triggered.
   * @param sender The frame pool that triggered the event.
   */
  void process_frame(Direct3D11CaptureFramePool const &sender) {
    // Track callback invocations
    static std::atomic<uint64_t> callback_count {0};
    auto cc = ++callback_count;
    if (cc == 1) {
      BOOST_LOG(info) << "First FrameArrived callback invoked";
    }

    Direct3D11CaptureFrame frame = nullptr;
    uint32_t drained_frames = 0;

    try {
      if (drain_to_latest()) {
        while (auto next_frame = sender.TryGetNextFrame()) {
          if (frame) {
            ++drained_frames;
          }
          frame = std::move(next_frame);
        }
      } else {
        frame = sender.TryGetNextFrame();
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "WinRT error retrieving WGC frame: " << ex.code() << " - " << winrt::to_string(ex.message());
      return;
    }

    if (!frame) {
      // Frame drop detected - record timestamp for sliding window analysis
      auto now = std::chrono::steady_clock::now();
      _drop_timestamps.push_back(now);
      record_frame_pool_pressure(now);

      const auto total_empty_drops = _frame_pool_empty_drops.fetch_add(1, std::memory_order_relaxed) + 1;
      if (total_empty_drops <= 5 || total_empty_drops % 120 == 0) {
        BOOST_LOG(info) << "WGC frame pool returned no frame"
                        << " (recent drops in 5s window=" << _drop_timestamps.size()
                        << ", total=" << total_empty_drops << ")";
      }
    } else {
      // Frame successfully retrieved
      try {
        auto surface = frame.Surface();

        // Get frame timing information from the WGC frame. The consumer treats
        // it as QPC ticks, so convert from the TimeSpan's 100 ns units.
        static const int64_t qpc_frequency = [] {
          LARGE_INTEGER frequency {};
          QueryPerformanceFrequency(&frequency);
          return static_cast<int64_t>(frequency.QuadPart);
        }();
        const auto source_time_100ns = frame.SystemRelativeTime().count();
        uint64_t frame_qpc = platf::dxgi::wgc_policy::system_relative_time_to_qpc(
          source_time_100ns,
          qpc_frequency
        );
        record_frame_arrival(drained_frames);
        if (admit_activity_frame(source_time_100ns)) {
          queue_frame_for_delivery(std::move(frame), surface, frame_qpc);
        }
      } catch (const winrt::hresult_error &ex) {
        // Log error
        BOOST_LOG(error) << "WinRT error in frame processing: " << ex.code() << " - " << winrt::to_string(ex.message());
      }
    }

    // Check if we need to adjust frame buffer size
    check_and_adjust_frame_buffer();
  }

private:
  bool drain_to_latest() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) != 0;
  }

  bool allow_buffer_decrease() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) != 0;
  }

  static int64_t steady_time_us(const std::chrono::steady_clock::time_point &time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  }

  void record_frame_pool_pressure(const std::chrono::steady_clock::time_point &now) {
    _last_frame_pool_pressure_us.store(steady_time_us(now), std::memory_order_relaxed);
  }

  static bool should_log_rate_limited(std::atomic<int64_t> &last_log_us, const std::chrono::steady_clock::time_point &now, std::chrono::microseconds interval) {
    const auto now_us = steady_time_us(now);
    auto last_us = last_log_us.load(std::memory_order_relaxed);
    while (last_us == 0 || now_us - last_us >= interval.count()) {
      if (last_log_us.compare_exchange_weak(last_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  double expected_frame_ms() const {
    return 1000.0 / static_cast<double>(std::max(1, g_config.target_fps));
  }

  double approximate_extra_pool_latency_ms(uint32_t buffer_size) const {
    return buffer_size > _initial_buffer_size ?
             static_cast<double>(buffer_size - _initial_buffer_size) * expected_frame_ms() :
             0.0;
  }

  void record_frame_arrival(uint32_t drained_frames) {
    _captured_frames.fetch_add(1, std::memory_order_relaxed);

    if (drained_frames > 0) {
      const auto now = std::chrono::steady_clock::now();
      record_frame_pool_pressure(now);
      const auto total_drained = _drained_pool_frames.fetch_add(drained_frames, std::memory_order_relaxed) + drained_frames;
      if (should_log_rate_limited(_last_drain_log_us, now, std::chrono::duration_cast<std::chrono::microseconds>(WGC_DRAIN_LOG_INTERVAL))) {
        BOOST_LOG(info) << "WGC drained " << drained_frames << " queued frame(s) from frame pool"
                        << " (total_drained=" << total_drained
                        << ", buffer=" << _current_buffer_size << "/" << _max_buffer_size
                        << ", approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size) << ")";
      }
    }
  }

  void maybe_log_capture_diagnostics(const std::chrono::steady_clock::time_point &now) {
    const auto now_us = steady_time_us(now);
    auto last_log_us = _last_diagnostics_log_us.load(std::memory_order_relaxed);
    const auto interval_us = std::chrono::duration_cast<std::chrono::microseconds>(WGC_DIAGNOSTICS_LOG_INTERVAL).count();

    while (now_us - last_log_us >= interval_us) {
      if (!_last_diagnostics_log_us.compare_exchange_weak(last_log_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }

      const double interval_s = static_cast<double>(now_us - last_log_us) / 1000000.0;
      if (interval_s <= 0.0) {
        return;
      }

      const auto captured = _captured_frames.load(std::memory_order_relaxed);
      const auto published = _published_frames.load(std::memory_order_relaxed);
      const auto empty_drops = _frame_pool_empty_drops.load(std::memory_order_relaxed);
      const auto drained = _drained_pool_frames.load(std::memory_order_relaxed);
      const auto replaced = _delivery_replaced_frames.load(std::memory_order_relaxed);
      const auto scratch_dropped = _scratch_dropped_frames.load(std::memory_order_relaxed);
      const auto slow_context = _slow_context_waits.load(std::memory_order_relaxed);
      const auto slow_mutex = _slow_mutex_waits.load(std::memory_order_relaxed);
      const auto slow_hold = _slow_shared_mutex_holds.load(std::memory_order_relaxed);
      const auto slow_copy = _slow_copy_submissions.load(std::memory_order_relaxed);
      const auto activity_rate_limited = _activity_rate_limited_frames.load(std::memory_order_relaxed);

      const auto captured_delta = captured - _last_diagnostics_captured_frames;
      const auto published_delta = published - _last_diagnostics_published_frames;
      const auto empty_drop_delta = empty_drops - _last_diagnostics_empty_drops;
      const auto drained_delta = drained - _last_diagnostics_drained_frames;
      const auto replaced_delta = replaced - _last_diagnostics_replaced_frames;
      const auto scratch_dropped_delta = scratch_dropped - _last_diagnostics_scratch_dropped;
      const auto slow_context_delta = slow_context - _last_diagnostics_slow_context;
      const auto slow_mutex_delta = slow_mutex - _last_diagnostics_slow_mutex;
      const auto slow_hold_delta = slow_hold - _last_diagnostics_slow_hold;
      const auto slow_copy_delta = slow_copy - _last_diagnostics_slow_copy;
      const auto activity_rate_limited_delta = activity_rate_limited - _last_diagnostics_activity_rate_limited;

      _last_diagnostics_captured_frames = captured;
      _last_diagnostics_published_frames = published;
      _last_diagnostics_empty_drops = empty_drops;
      _last_diagnostics_drained_frames = drained;
      _last_diagnostics_replaced_frames = replaced;
      _last_diagnostics_scratch_dropped = scratch_dropped;
      _last_diagnostics_slow_context = slow_context;
      _last_diagnostics_slow_mutex = slow_mutex;
      _last_diagnostics_slow_hold = slow_hold;
      _last_diagnostics_slow_copy = slow_copy;
      _last_diagnostics_activity_rate_limited = activity_rate_limited;

      BOOST_LOG(info) << "WGC capture diagnostics: interval_s=" << interval_s
                      << " buffer=" << _current_buffer_size << "/" << _max_buffer_size
                      << " approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size)
                      << " capture_fps=" << (static_cast<double>(captured_delta) / interval_s)
                      << " publish_fps=" << (static_cast<double>(published_delta) / interval_s)
                      << " drained=" << drained_delta
                      << " activity_rate_limited=" << activity_rate_limited_delta
                      << " empty_drops=" << empty_drop_delta
                      << " delivery_replaced=" << replaced_delta
                      << " scratch_dropped=" << scratch_dropped_delta
                      << " slow_context=" << slow_context_delta
                      << " slow_mutex=" << slow_mutex_delta
                      << " slow_shared_hold=" << slow_hold_delta
                      << " slow_copy=" << slow_copy_delta;
      return;
    }
  }

  bool admit_activity_frame(int64_t source_time_100ns) {
    const auto generation = g_activity_admission_generation.load(std::memory_order_acquire);
    if (generation != _activity_admission_generation) {
      _activity_admission_generation = generation;
      _activity_admission = {};
    }

    const auto admission_fps = g_activity_admission_fps.load(std::memory_order_relaxed);
    if (!_activity_admission.admit(
          admission_fps,
          source_time_100ns,
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        )) {
      _activity_rate_limited_frames.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    return true;
  }

  /**
   * @brief Prunes old frame drop timestamps from the sliding window.
   * @param now Current timestamp for comparison.
   */
  void prune_old_drop_timestamps(const std::chrono::steady_clock::time_point &now) {
    while (!_drop_timestamps.empty() && now - _drop_timestamps.front() > std::chrono::seconds(5)) {
      _drop_timestamps.pop_front();
    }
  }

  /**
   * @brief Checks if buffer size should be increased due to recent frame drops.
   * @param now Current timestamp.
   * @return true if buffer was increased, false otherwise.
   */
  bool try_increase_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (_drop_timestamps.size() >= 2 && _current_buffer_size < _max_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size + 1;
      BOOST_LOG(info) << "Detected " << _drop_timestamps.size() << " frame drops in 5s window, increasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _drop_timestamps.clear();  // Reset after adjustment
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }
    return false;
  }

  /**
   * @brief Checks if buffer size should be decreased due to sustained quiet period.
   * @param now Current timestamp.
   * @return true if buffer was decreased, false otherwise.
   */
  bool try_decrease_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (!allow_buffer_decrease()) {
      return false;
    }

    constexpr auto quiet_period = std::chrono::seconds(30);
    const auto last_pressure_us = _last_frame_pool_pressure_us.load(std::memory_order_relaxed);
    const auto quiet_period_us = std::chrono::duration_cast<std::chrono::microseconds>(quiet_period).count();
    const bool recent_pool_pressure = last_pressure_us > 0 &&
                                      steady_time_us(now) - last_pressure_us < quiet_period_us;
    const bool is_quiet = platf::dxgi::wgc_policy::buffer_pool_is_quiet(
      allow_buffer_decrease(),
      !_drop_timestamps.empty(),
      recent_pool_pressure,
      _peak_outstanding.load(),
      _current_buffer_size
    );

    if (!is_quiet) {
      _last_quiet_start = now;  // Reset quiet timer
      return false;
    }

    // Check if we've been quiet for 30 seconds
    if (now - _last_quiet_start >= quiet_period && _current_buffer_size > _initial_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size - 1;
      BOOST_LOG(info) << "Sustained quiet period (30s) with peak occupancy " << _peak_outstanding.load()
                      << " <= " << (_current_buffer_size - 1) << ", decreasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }

    return false;
  }

  bool ensure_scratch_texture(size_t index) {
    if (!_deps || index >= _scratch_textures.size()) {
      return false;
    }

    auto &scratch = _scratch_textures[index];
    if (scratch.texture) {
      D3D11_TEXTURE2D_DESC have {};
      scratch.texture->GetDesc(&have);
      if (have.Width == _width && have.Height == _height) {
        return true;
      }
      // Window capture resized: drop the stale scratch texture.
      scratch.texture = nullptr;
    }

    winrt::com_ptr<ID3D11Device> device;
    _deps->d3d_context->GetDevice(device.put());
    if (!device) {
      BOOST_LOG(error) << "Failed to get D3D11 device for WGC scratch texture";
      return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = _width;
    desc.Height = _height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = _capture_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, scratch.texture.put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create WGC scratch texture: " << std::format(": 0x{:08X}", hr);
      return false;
    }

    return true;
  }

  std::optional<size_t> reserve_scratch_texture() {
    std::lock_guard lock(_delivery_mutex);

    if (_delivery_stop || _shutting_down.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    for (size_t i = 0; i < _scratch_textures.size(); ++i) {
      if (_scratch_textures[i].state == scratch_state_e::free) {
        _scratch_textures[i].state = scratch_state_e::reserved;
        return i;
      }
    }

    if (_pending_delivery_frame) {
      const auto stale_index = _pending_delivery_frame->scratch_index;
      if (stale_index >= _scratch_textures.size()) {
        _pending_delivery_frame.reset();
        return std::nullopt;
      }

      _pending_delivery_frame.reset();
      _scratch_textures[stale_index].state = scratch_state_e::reserved;

      const auto replaced = _delivery_replaced_frames.fetch_add(1, std::memory_order_relaxed) + 1;
      if (replaced <= 5 || replaced % 120 == 0) {
        BOOST_LOG(debug) << "WGC delivery replaced stale queued scratch frame"
                         << " (count=" << replaced << ")";
      }
      return stale_index;
    }

    const auto dropped = _scratch_dropped_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped <= 5 || dropped % 120 == 0) {
      BOOST_LOG(debug) << "WGC scratch handoff had no free texture; dropping frame"
                       << " (count=" << dropped << ")";
    }
    return std::nullopt;
  }

  void release_reserved_scratch_texture(size_t index) {
    std::lock_guard lock(_delivery_mutex);
    if (index < _scratch_textures.size() && _scratch_textures[index].state == scratch_state_e::reserved) {
      _scratch_textures[index].state = scratch_state_e::free;
    }
  }

  bool enqueue_scratch_texture(size_t index, uint64_t frame_qpc) {
    std::lock_guard lock(_delivery_mutex);

    if (index >= _scratch_textures.size() || !_scratch_textures[index].texture) {
      return false;
    }

    if (_delivery_stop || _shutting_down.load(std::memory_order_acquire)) {
      _scratch_textures[index].state = scratch_state_e::free;
      return false;
    }

    if (_pending_delivery_frame) {
      const auto stale_index = _pending_delivery_frame->scratch_index;
      _pending_delivery_frame.reset();
      if (stale_index < _scratch_textures.size()) {
        _scratch_textures[stale_index].state = scratch_state_e::free;
      }

      const auto replaced = _delivery_replaced_frames.fetch_add(1, std::memory_order_relaxed) + 1;
      if (replaced <= 5 || replaced % 120 == 0) {
        BOOST_LOG(debug) << "WGC delivery replaced stale queued scratch frame"
                         << " (count=" << replaced << ")";
      }
    }

    _scratch_textures[index].state = scratch_state_e::pending;
    _pending_delivery_frame = delivery_frame_t {
      .texture = _scratch_textures[index].texture,
      .scratch_index = index,
      .frame_qpc = frame_qpc
    };
    return true;
  }

  /**
   * @brief Copies the next WGC frame into helper-owned scratch storage and queues it for delivery.
   * @param frame The WGC frame object; it is released before the shared IPC mutex can block.
   * @param surface The captured D3D11 surface.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   */
  void queue_frame_for_delivery(Direct3D11CaptureFrame frame, winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface, uint64_t frame_qpc) {
    if (!_deps) {
      return;
    }

    // Get DXGI access
    winrt::com_ptr<IDirect3DDxgiInterfaceAccess> ia;
    if (FAILED(winrt::get_unknown(surface)->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), ia.put_void()))) {
      BOOST_LOG(error) << "Failed to query IDirect3DDxgiInterfaceAccess";
      return;
    }

    // Get underlying texture
    winrt::com_ptr<ID3D11Texture2D> frame_tex;
    if (FAILED(ia->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<::IUnknown **>(frame_tex.put_void())))) {
      BOOST_LOG(error) << "Failed to get ID3D11Texture2D from interface";
      return;
    }

    auto scratch_index = reserve_scratch_texture();
    if (!scratch_index) {
      return;
    }

    if (!ensure_scratch_texture(*scratch_index)) {
      release_reserved_scratch_texture(*scratch_index);
      return;
    }

    {
      std::lock_guard context_lock(_d3d_context_mutex);
      _deps->d3d_context->CopyResource(_scratch_textures[*scratch_index].texture.get(), frame_tex.get());
    }

    // From here on the delivery thread owns only the helper scratch texture, not
    // the Direct3D11CaptureFrame/WGC frame-pool buffer. The helper can wait for
    // the main process' shared keyed mutex without starving WGC FrameArrived.
    frame = nullptr;

    if (!enqueue_scratch_texture(*scratch_index, frame_qpc)) {
      return;
    }
    _delivery_cv.notify_one();
  }

  void delivery_thread_proc(std::stop_token stop_token) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DWORD task_idx = 0;
    safe_mmcss_handle mmcss_handle = nullptr;
    HANDLE raw_mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    if (!raw_mmcss_handle) {
      raw_mmcss_handle = AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
    }
    if (raw_mmcss_handle) {
      mmcss_handle.reset(raw_mmcss_handle);
      AvSetMmThreadPriority(mmcss_handle.get(), AVRT_PRIORITY_HIGH);
    } else {
      BOOST_LOG(warning) << "Failed to set WGC delivery thread MMCSS characteristics: " << GetLastError();
    }

    for (;;) {
      std::optional<delivery_frame_t> frame;
      {
        std::unique_lock lock(_delivery_mutex);
        _delivery_cv.wait(lock, [&]() {
          return _delivery_stop || stop_token.stop_requested() || _pending_delivery_frame.has_value();
        });

        if (_delivery_stop || stop_token.stop_requested()) {
          break;
        }

        frame = std::move(_pending_delivery_frame);
        _pending_delivery_frame.reset();
        if (frame && frame->scratch_index < _scratch_textures.size()) {
          _scratch_textures[frame->scratch_index].state = scratch_state_e::delivering;
        }
      }
      _delivery_cv.notify_all();

      if (!frame || !frame->texture) {
        continue;
      }

      try {
        copy_frame_to_shared_texture(frame->texture, frame->frame_qpc);
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "WinRT error in WGC delivery thread: " << ex.code() << " - " << winrt::to_string(ex.message());
      } catch (...) {
        BOOST_LOG(error) << "Unknown error in WGC delivery thread";
      }

      {
        std::lock_guard lock(_delivery_mutex);
        if (frame->scratch_index < _scratch_textures.size() &&
            _scratch_textures[frame->scratch_index].state == scratch_state_e::delivering) {
          _scratch_textures[frame->scratch_index].state = scratch_state_e::free;
        }
      }
    }
  }

  /**
   * @brief Copies the captured texture to the shared texture and signals the main process.
   * @param frame_tex The captured D3D11 texture.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   */
  void copy_frame_to_shared_texture(const winrt::com_ptr<ID3D11Texture2D> &frame_tex, uint64_t frame_qpc) {
    if (!_deps || !frame_tex) {
      return;
    }

    // Take the helper-local D3D context lock before the cross-process keyed
    // mutex. The callback thread also uses this context for scratch copies; if
    // GPU load makes that copy slow, waiting here must not block Sunshine from
    // acquiring the shared WGC texture.
    const auto context_wait_start = std::chrono::steady_clock::now();
    std::unique_lock context_lock(_d3d_context_mutex);
    const auto context_wait = std::chrono::steady_clock::now() - context_wait_start;

    const auto mutex_wait_start = std::chrono::steady_clock::now();
    HRESULT hr = _deps->resource_manager.get_keyed_mutex()->AcquireSync(0, 200);
    const auto mutex_wait = std::chrono::steady_clock::now() - mutex_wait_start;
    if (hr == WAIT_TIMEOUT) {
      const auto slow_mutex_count = _slow_mutex_waits.fetch_add(1, std::memory_order_relaxed) + 1;
      if (slow_mutex_count <= 5 || slow_mutex_count % 120 == 0) {
        BOOST_LOG(debug) << "Timed out acquiring keyed mutex for WGC frame copy; dropping frame"
                         << " (count=" << slow_mutex_count << ")";
      }
      return;
    }
    if (hr != S_OK && hr != WAIT_ABANDONED) {
      BOOST_LOG(error) << "Failed to acquire mutex key 0: " << std::format(": 0x{:08X}", hr);
      return;
    }
    if (hr == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Keyed mutex was abandoned; continuing with lock held";
    }

    const auto shared_mutex_hold_start = std::chrono::steady_clock::now();
    auto release_shared_mutex = util::fail_guard([&]() {
      const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex()->ReleaseSync(0);
      if (FAILED(rel_hr)) {
        BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      }
    });

    // Copy frame data while holding the keyed mutex. The main process acquires
    // the same mutex before snapshotting this shared texture into a pool-owned frame.
    const auto copy_start = std::chrono::steady_clock::now();
    {
      auto &shared = _deps->resource_manager.get_shared_texture();
      D3D11_TEXTURE2D_DESC src {}, dst {};
      frame_tex->GetDesc(&src);
      shared->GetDesc(&dst);
      if (src.Width == dst.Width && src.Height == dst.Height) {
        _deps->d3d_context->CopyResource(shared.get(), frame_tex.get());
      } else {
        // App-window canvas: window frame into the top-left; clear first when
        // the window got smaller so stale pixels don't linger beyond its edge.
        if (src.Width < _last_canvas_copy_w || src.Height < _last_canvas_copy_h || !_shared_rtv) {
          if (!_shared_rtv) {
            winrt::com_ptr<ID3D11Device> device;
            _deps->d3d_context->GetDevice(device.put());
            if (device) {
              device->CreateRenderTargetView(shared.get(), nullptr, _shared_rtv.put());
            }
          }
          if (_shared_rtv) {
            const float black[4] {0.f, 0.f, 0.f, 1.f};
            _deps->d3d_context->ClearRenderTargetView(_shared_rtv.get(), black);
          }
        }
        D3D11_BOX box {0, 0, 0, std::min(src.Width, dst.Width), std::min(src.Height, dst.Height), 1};
        _deps->d3d_context->CopySubresourceRegion(shared.get(), 0, 0, 0, 0, frame_tex.get(), 0, &box);
        _last_canvas_copy_w = src.Width;
        _last_canvas_copy_h = src.Height;
      }
    }
    const auto copy_submit = std::chrono::steady_clock::now() - copy_start;

    // Publish the metadata while still holding the shared keyed mutex so the
    // consumer can never lock a newer texture while reading an older frame id.
    _deps->resource_manager.publish_frame_metadata(frame_qpc);

    const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex()->ReleaseSync(0);
    release_shared_mutex.disable();
    if (FAILED(rel_hr)) {
      BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      return;
    }
    const auto shared_mutex_hold = std::chrono::steady_clock::now() - shared_mutex_hold_start;
    context_lock.unlock();

    // Signal only after releasing the mutex so a woken consumer can acquire the
    // frame without waiting on the producer's normal release path.
    _deps->resource_manager.signal_frame_ready();
    _published_frames.fetch_add(1, std::memory_order_relaxed);

    const auto context_wait_ms = std::chrono::duration<double, std::milli>(context_wait).count();
    const auto mutex_wait_ms = std::chrono::duration<double, std::milli>(mutex_wait).count();
    const auto shared_mutex_hold_ms = std::chrono::duration<double, std::milli>(shared_mutex_hold).count();
    const auto copy_submit_ms = std::chrono::duration<double, std::milli>(copy_submit).count();
    const bool slow_context = context_wait_ms > 1.0;
    const bool slow_mutex = mutex_wait_ms > 1.0;
    const bool slow_shared_hold = shared_mutex_hold_ms > 1.0;
    const bool slow_copy = copy_submit_ms > 1.0;
    if (slow_context || slow_mutex || slow_shared_hold || slow_copy) {
      const auto slow_context_count = slow_context ? (_slow_context_waits.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_context_waits.load(std::memory_order_relaxed);
      const auto slow_mutex_count = slow_mutex ? (_slow_mutex_waits.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_mutex_waits.load(std::memory_order_relaxed);
      const auto slow_hold_count = slow_shared_hold ? (_slow_shared_mutex_holds.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_shared_mutex_holds.load(std::memory_order_relaxed);
      const auto slow_copy_count = slow_copy ? (_slow_copy_submissions.fetch_add(1, std::memory_order_relaxed) + 1) : _slow_copy_submissions.load(std::memory_order_relaxed);
      const bool should_log_slow_context = slow_context && (slow_context_count <= 5 || slow_context_count % 120 == 0);
      const bool should_log_slow_mutex = slow_mutex && (slow_mutex_count <= 5 || slow_mutex_count % 120 == 0);
      const bool should_log_slow_hold = slow_shared_hold && (slow_hold_count <= 5 || slow_hold_count % 120 == 0);
      const bool should_log_slow_copy = slow_copy && (slow_copy_count <= 5 || slow_copy_count % 120 == 0);
      if (should_log_slow_context || should_log_slow_mutex || should_log_slow_hold || should_log_slow_copy) {
        BOOST_LOG(debug) << "WGC helper copy timing: context_wait_ms=" << context_wait_ms
                         << " mutex_wait_ms=" << mutex_wait_ms
                         << " shared_mutex_hold_ms=" << shared_mutex_hold_ms
                         << " copy_submit_ms=" << copy_submit_ms
                         << " slow_context_count=" << slow_context_count
                         << " slow_mutex_count=" << slow_mutex_count
                         << " slow_shared_hold_count=" << slow_hold_count
                         << " slow_copy_count=" << slow_copy_count;
      }
    }

    // Log first frame and frame 100 for quick sanity checks, but avoid long-running spam.
    static std::atomic<uint64_t> frame_count {0};
    const auto count = ++frame_count;
    if (count == 1 || count == 100) {
      BOOST_LOG(info) << "Published frame " << count << " to main process";
    }
  }

public:
  /**
   * @brief Creates a capture session for the specified capture item.
   * @returns true if the session was created successfully, false otherwise.
   */
  bool create_capture_session() {
    if (!_frame_pool) {
      return false;
    }

    if (!_deps || !_deps->graphics_item) {
      BOOST_LOG(error) << "Cannot create capture session: missing capture item";
      return false;
    }

    const auto callback_state = _frame_callback_state;
    _frame_arrived_token = _frame_pool.FrameArrived([callback_state](Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &) {
      WgcCaptureManager *owner = nullptr;
      {
        std::lock_guard lock(callback_state->mutex);
        if (callback_state->shutting_down || !callback_state->owner) {
          return;
        }
        ++callback_state->active;
        owner = callback_state->owner;
      }

      const int value = owner->_outstanding_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
      auto outstanding_guard = util::fail_guard([callback_state, owner]() {
        owner->_outstanding_frames.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard lock(callback_state->mutex);
        if (--callback_state->active == 0) {
          callback_state->drained.notify_all();
        }
      });

      int prev_peak = owner->_peak_outstanding.load(std::memory_order_relaxed);
      while (value > prev_peak &&
             !owner->_peak_outstanding.compare_exchange_weak(prev_peak, value, std::memory_order_release, std::memory_order_relaxed)) {
      }

      owner->process_frame(sender);
    });

    try {
      _capture_session = _frame_pool.CreateCaptureSession(_deps->graphics_item);
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "CreateCaptureSession threw: " << ex.code() << " - " << winrt::to_string(ex.message());
      return false;
    } catch (...) {
      BOOST_LOG(error) << "CreateCaptureSession threw an unknown exception";
      return false;
    }

    if (!_capture_session) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureSession (returned null)";
      return false;
    }

    _last_cursor_capture_request.reset();
    update_cursor_capture();

    auto session3 = _capture_session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
    if (!session3) {
      BOOST_LOG(warning) << "IGraphicsCaptureSession3 not available; skipping IsBorderRequired(false)";
    } else {
      try {
        session3.IsBorderRequired(false);
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(warning) << "IsBorderRequired(false) failed (continuing without it): " << ex.code() << " - " << winrt::to_string(ex.message());
      } catch (...) {
        BOOST_LOG(warning) << "IsBorderRequired(false) threw an unknown exception (continuing without it)";
      }
    }

    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
      if (g_config.min_update_interval_100ns > 0) {
        _capture_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan {g_config.min_update_interval_100ns});
        BOOST_LOG(info) << "WGC MinUpdateInterval set to " << g_config.min_update_interval_100ns << " ticks";
      } else {
        BOOST_LOG(info) << "WGC MinUpdateInterval left at system default";
      }
    }

    return true;
  }

  /**
   * @brief Apply cursor inclusion on the helper owner thread, before startup or during capture.
   */
  void update_cursor_capture() {
    if (!_capture_session) {
      return;
    }
    const bool visible = g_cursor_visible.load(std::memory_order_acquire);
    if (_last_cursor_capture_request == visible) {
      return;
    }
    _last_cursor_capture_request = visible;
    try {
      auto session2 = _capture_session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>();
      if (!session2) {
        BOOST_LOG(warning) << "IGraphicsCaptureSession2 not available; keeping WGC cursor inclusion at its system default";
        return;
      }
      session2.IsCursorCaptureEnabled(visible);
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(warning) << "IsCursorCaptureEnabled failed (continuing capture): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(warning) << "IsCursorCaptureEnabled threw an unknown exception (continuing capture)";
    }
  }

  /**
   * @brief Starts the capture session if available.
   */
  void start_capture() const {
    if (_capture_session) {
      _capture_session.StartCapture();
      BOOST_LOG(info) << "Helper process started. Capturing frames using WGC...";
    }
  }

private:
  void check_and_adjust_frame_buffer() {
    auto now = std::chrono::steady_clock::now();

    // Check every 1 second for buffer adjustments
    if (auto time_since_last_check = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_buffer_check);
        time_since_last_check.count() < 1000) {
      return;
    }

    _last_buffer_check = now;

    // 1) Prune old drop timestamps (older than 5 seconds)
    prune_old_drop_timestamps(now);

    // 2) Try to increase buffer count if we have recent drops
    if (try_increase_buffer_size(now)) {
      maybe_log_capture_diagnostics(now);
      return;
    }

    // 3) Try to decrease buffer count if we've been quiet
    try_decrease_buffer_size(now);
    maybe_log_capture_diagnostics(now);
  }
};

/**
 * @brief Callback procedure for desktop switch events.
 *
 * This function handles EVENT_SYSTEM_DESKTOPSWITCH events to detect when the system
 * transitions to or from secure desktop mode (such as UAC prompts or lock screens).
 * If secure desktop is not active yet, it starts a short confirmation window so we can
 * still catch the secure-desktop transition after the event settles.
 *
 * @param h_win_event_hook Handle to the event hook (unused).
 * @param event The event type that occurred.
 * @param hwnd Handle to the window (unused).
 * @param id_object Object identifier (unused).
 * @param id_child Child object identifier (unused).
 * @param dw_event_thread Thread that generated the event (unused).
 * @param dwms_event_time Time the event occurred (unused).
 */
void CALLBACK desktop_switch_hook_proc(HWINEVENTHOOK /*h_win_event_hook*/, DWORD event, HWND /*hwnd*/, LONG /*id_object*/, LONG /*id_child*/, DWORD /*dw_event_thread*/, DWORD /*dwms_event_time*/) {
  if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
    BOOST_LOG(info) << "Desktop switch detected!";

    const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
    BOOST_LOG(info) << "Desktop switch - Secure desktop: " << (secure_desktop_active ? "YES" : "NO");

    if (secure_desktop_active) {
      notify_main_process_about_secure_desktop();
    } else if (!secure_desktop_active && g_secure_desktop_detected) {
      BOOST_LOG(info) << "Returned to normal desktop";
      g_secure_desktop_detected = false;
      g_pending_secure_desktop_check_deadline.reset();
    } else {
      g_pending_secure_desktop_check_deadline = std::chrono::steady_clock::now() + SECURE_DESKTOP_CONFIRMATION_WINDOW;
      BOOST_LOG(debug) << "Desktop switch was not immediately secure; waiting briefly for confirmation";
    }
  }
}

std::filesystem::path get_roaming_log_directory() {
  PWSTR roaming_path = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming_path);
  if (FAILED(hr) || roaming_path == nullptr) {
    if (roaming_path) {
      CoTaskMemFree(roaming_path);
    }
    return {};
  }

  std::filesystem::path result(roaming_path);
  CoTaskMemFree(roaming_path);
  result /= L"Sunshine";

  std::error_code ec;
  std::filesystem::create_directories(result, ec);
  if (ec) {
    return {};
  }

  return result;
}

/**
 * @brief Helper function to get the WGC helper log path.
 *
 * Logs live under Roaming/Sunshine so they sit with the other Sunshine logs.
 * When session-mode logging is enabled, the returned file path is used to derive
 * the base name and output directory (typically Roaming/Sunshine/logs).
 * Falls back to the system temporary directory (or current directory) if the roaming
 * folder cannot be resolved.
 *
 * @return Path to the log file as a UTF-8 string.
 */
std::string get_temp_log_path() {
  if (auto roaming_dir = get_roaming_log_directory(); !roaming_dir.empty()) {
    auto log_file = roaming_dir / L"sunshine_wgc_helper.log";
    return wide_to_utf8(log_file.wstring());
  }

  std::wstring temp_path(MAX_PATH, L'\0');
  if (auto len = GetTempPathW(MAX_PATH, temp_path.data()); len == 0 || len > MAX_PATH) {
    return "sunshine_wgc_helper.log";
  }
  temp_path.resize(wcslen(temp_path.data()));
  std::wstring wlog = temp_path + L"sunshine_wgc_helper.log";
  return wide_to_utf8(wlog);
}

/**
 * @brief Helper function to handle IPC messages from the main process.
 *
 * Processes incoming messages from the main Sunshine process via named pipe:
 *
 * - Configuration messages: Receives and stores config_data_t structure with display settings
 *
 * @param message The received message bytes from the named pipe.
 *
 */
void handle_ipc_message(std::span<const uint8_t> message) {
  if (message.size() == sizeof(platf::dxgi::cursor_visibility_data_t)) {
    platf::dxgi::cursor_visibility_data_t update {};
    memcpy(&update, message.data(), sizeof(update));
    if (update.magic == platf::dxgi::WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC) {
      if (update.visible > 1) {
        BOOST_LOG(warning) << "Ignoring invalid WGC cursor visibility update";
        return;
      }
      g_cursor_visible.store(update.visible != 0, std::memory_order_release);
      return;
    }
  }

  if (message.size() == sizeof(platf::dxgi::activity_admission_data_t)) {
    platf::dxgi::activity_admission_data_t update {};
    memcpy(&update, message.data(), sizeof(update));
    if (update.magic != platf::dxgi::WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC || update.admission_fps <= 0) {
      BOOST_LOG(warning) << "Ignoring invalid WGC activity admission update";
      return;
    }

    g_activity_admission_fps.store(update.admission_fps, std::memory_order_release);
    g_activity_admission_generation.fetch_add(1, std::memory_order_acq_rel);
    BOOST_LOG(info) << "WGC activity admission updated to " << update.admission_fps << "fps";
    return;
  }

  // Handle config data message
  if (message.size() == sizeof(platf::dxgi::config_data_t)) {
    std::lock_guard lock(g_config_mutex);
    if (g_config_received) {
      return;
    }

    memcpy(&g_config, message.data(), sizeof(platf::dxgi::config_data_t));
    g_config_received = true;
    g_cursor_visible.store((g_config.flags & platf::dxgi::WGC_IPC_FLAG_HIDE_CURSOR) == 0, std::memory_order_release);
    g_activity_admission_fps.store(g_config.activity_admission_fps, std::memory_order_release);
    g_activity_admission_generation.fetch_add(1, std::memory_order_acq_rel);
    // If log_level in config differs from current, update log filter
    if (INITIAL_LOG_LEVEL != g_config.log_level) {
      // Update log filter to new log level
      boost::log::core::get()->set_filter(
        severity >= g_config.log_level
      );
      BOOST_LOG(info) << "Log level updated from config: " << g_config.log_level;
    }
    BOOST_LOG(info) << "Received config data: hdr: " << g_config.dynamic_range
                    << ", display: '" << winrt::to_string(g_config.display_name) << "'"
                    << ", adapter LUID: " << std::hex << g_config.adapter_luid.HighPart
                    << ":" << g_config.adapter_luid.LowPart << std::dec
                    << ", target_fps: " << g_config.target_fps
                    << ", activity_admission_fps: " << g_config.activity_admission_fps
                    << ", min_update_interval_100ns: " << g_config.min_update_interval_100ns
                    << ", initial_buffers: " << g_config.initial_frame_buffer_size
                    << ", max_buffers: " << g_config.max_frame_buffer_size
                    << ", force_sdr_capture: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) ? "yes" : "no")
                    << ", drain_to_latest: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) ? "yes" : "no")
                    << ", allow_buffer_decrease: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) ? "yes" : "no");
    g_config_cv.notify_all();
  }
}

/**
 * @brief Reconstruct WGC control records from arbitrary byte-pipe reads.
 */
class WgcControlStream {
public:
  // AnonymousPipeFactory supplies a byte stream, not message boundaries. Keep
  // the existing wire format: one config followed by fixed-size live updates.
  void receive(std::span<const uint8_t> bytes) {
    static_assert(sizeof(platf::dxgi::cursor_visibility_data_t) == sizeof(platf::dxgi::activity_admission_data_t));
    while (!bytes.empty()) {
      const auto message_size = _configured ? sizeof(platf::dxgi::activity_admission_data_t) : sizeof(platf::dxgi::config_data_t);
      const auto count = std::min(bytes.size(), message_size - _buffered);
      std::copy_n(bytes.data(), count, _buffer.data() + _buffered);
      _buffered += count;
      bytes = bytes.subspan(count);
      if (_buffered == message_size) {
        handle_ipc_message(std::span<const uint8_t>(_buffer.data(), message_size));
        _configured = true;
        _buffered = 0;
      }
    }
  }

private:
  std::array<uint8_t, sizeof(platf::dxgi::config_data_t)> _buffer {};
  size_t _buffered = 0;
  bool _configured = false;
};

/**
 * @brief Helper function to setup the communication pipe with the main process.
 *
 * Configures the AsyncNamedPipe with callback functions for message handling:
 * - on_message: Delegates to handle_ipc_message() for processing received data
 * - on_error: Handles pipe communication errors (currently empty handler)
 *
 * @param pipe Reference to the AsyncNamedPipe to configure.
 *
 * @return true if the pipe was successfully configured and started, false otherwise.
 */
bool setup_pipe_callbacks(AsyncNamedPipe &pipe) {
  // Each connection owns its accumulator. Retiring the pipe discards an
  // incomplete tail instead of carrying it into the next helper generation.
  auto on_message = [stream = WgcControlStream {}](std::span<const uint8_t> message) mutable {
    stream.receive(message);
  };

  auto on_error = [](std::string_view /*err*/) {
    // Error handler, intentionally left empty or log as needed
  };

  return pipe.start(on_message, on_error);
}

/**
 * @brief Helper function to process window messages for desktop hooks.
 *
 * @param shutdown_requested Reference to shutdown flag that may be set if WM_QUIT is received.
 * @return true if messages were processed, false if shutdown was requested.
 */
bool process_window_messages(bool &shutdown_requested) {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (msg.message == WM_QUIT) {
      shutdown_requested = true;
      return false;
    }
  }
  return true;
}

/**
 * @brief Helper function to setup desktop switch hook for secure desktop detection.
 *
 * @return true if the hook was successfully installed, false otherwise.
 */
bool setup_desktop_switch_hook() {
  BOOST_LOG(info) << "Setting up desktop switch hook...";

  HWINEVENTHOOK raw_hook = SetWinEventHook(
    EVENT_SYSTEM_DESKTOPSWITCH,
    EVENT_SYSTEM_DESKTOPSWITCH,
    nullptr,
    desktop_switch_hook_proc,
    0,
    0,
    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
  );

  if (!raw_hook) {
    BOOST_LOG(error) << "Failed to set up desktop switch hook: " << GetLastError();
    return false;
  }

  g_desktop_switch_hook.reset(raw_hook);
  BOOST_LOG(info) << "Desktop switch hook installed successfully";
  return true;
}

/**
 * @brief Main application entry point for the Windows Graphics Capture helper process.
 *
 * This standalone executable serves as a helper process for Sunshine's Windows Graphics
 * Capture functionality. The main function performs these key operations:
 *
 * 1. **System Initialization**: Sets up logging, DPI awareness, thread priority, and MMCSS
 * 2. **IPC Setup**: Establishes named pipe communication with the main Sunshine process
 * 3. **D3D11 Device Creation**: Creates hardware-accelerated D3D11 device and WinRT interop
 * 4. **Monitor Selection**: Identifies and configures the target monitor for capture
 * 5. **Shared Resource Creation**: Sets up shared D3D11 texture for inter-process frame sharing
 * 6. **WGC Setup**: Initializes Windows Graphics Capture frame pool and capture session
 * 7. **Desktop Monitoring**: Sets up hooks to detect secure desktop transitions
 * 8. **Main Loop**: Processes window messages and handles capture events until shutdown
 *
 * @param argc Number of command line arguments. Expects at least 2 arguments.
 * @param argv Array of command line argument strings. argv[1] should contain the pipe name GUID.
 * @return 0 on successful completion, 1 on initialization failure.
 */
// ---------------------------------------------------------------------------
// App-window sessions: one WGC capture per window that belongs to the streamed
// application window (the window itself plus its menus, drop-downs, tooltips
// and owned dialogs), composited onto a fixed canvas the size of the encoder
// frame. Per-window WGC items are unaffected by whatever overlaps the window
// on the host, so other apps, the Start menu or the taskbar never bleed in.
// ---------------------------------------------------------------------------
class WindowCompositor {
public:
  struct capture_t {
    HWND hwnd = nullptr;
    bool is_target = false;
    RECT rect {};  ///< outer rect, sampled by the layout thread
    GraphicsCaptureItem item = nullptr;
    Direct3D11CaptureFramePool pool = nullptr;
    GraphicsCaptureSession session = nullptr;
    winrt::event_token frame_token {};
    winrt::event_token closed_token {};
    winrt::Windows::Graphics::SizeInt32 pool_size {0, 0};
    winrt::com_ptr<ID3D11Texture2D> latest;  ///< helper-owned copy of the newest frame (guarded by _ctx_mutex)
    winrt::com_ptr<ID3D11ShaderResourceView> latest_srv;
    UINT latest_w = 0, latest_h = 0;
    uint64_t latest_qpc = 0;
    std::optional<bool> cursor_applied;
    std::atomic<bool> closed {false};
    bool gdi = false;  ///< not captured by WGC: PrintWindow, or a screen copy for layered windows
    bool screen_copy = false;  ///< WS_EX_LAYERED: only the screen has its pixels (WGC/PrintWindow give blanks)
    bool dialog = false;  ///< owned window with a caption
    bool inline_part = false;  ///< drawn in place over the parent, no slot
    bool seeded = false;  ///< first picture taken with PrintWindow because WGC delivered nothing yet
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point gdi_last {};
  };

  WindowCompositor(IDirect3DDevice winrt_device, winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context,
                   SharedResourceManager &shared, DXGI_FORMAT format, HWND target, UINT canvas_w, UINT canvas_h, int fps,
                   std::shared_ptr<AsyncNamedPipe> pipe):
      _winrt_device(std::move(winrt_device)),
      _device(std::move(device)),
      _context(std::move(context)),
      _shared(shared),
      _format(format),
      _target(target),
      _canvas_w(canvas_w),
      _canvas_h(canvas_h),
      _bands(app_window_layout::frame_bands(static_cast<int>(canvas_h))),
      _color_h(static_cast<UINT>(_bands.canvas_h)),
      _fps(std::clamp(fps, 10, 240)),
      _pipe(std::move(pipe)) {
  }

  ~WindowCompositor() {
    stop();
  }

  bool start() {
    {
      std::lock_guard lock(_captures_mutex);
      auto cap = create_capture(_target, true);
      if (!cap) {
        BOOST_LOG(error) << "App-window capture: cannot capture the target window";
        return false;
      }
      _target_capture = cap;
      _order = {cap};
    }
    _stop.store(false, std::memory_order_release);
    _layout_thread = std::thread([this] {
      layout_thread_proc();
    });
    _composite_thread = std::thread([this] {
      composite_thread_proc();
    });
    BOOST_LOG(info) << "App-window capture: window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(_target) << std::dec
                    << " on a " << _canvas_w << 'x' << _color_h << " canvas (window band " << _bands.window_h << ", popup band " << (_bands.canvas_h - _bands.popup_y) << ", + half-res alpha band)";
    return true;
  }

  void stop() {
    if (_stop.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(_dirty_mutex);
      _dirty = true;
    }
    _dirty_cv.notify_all();
    if (_layout_thread.joinable()) {
      _layout_thread.join();
    }
    if (_composite_thread.joinable()) {
      _composite_thread.join();
    }
    std::lock_guard lock(_captures_mutex);
    for (auto &cap : _order) {
      destroy_capture(*cap);
    }
    _order.clear();
    _target_capture.reset();
  }

  /** Apply the main process' cursor inclusion request to every capture (main thread, ~1 kHz). */
  void update_cursor_capture() {
    const bool visible = g_cursor_visible.load(std::memory_order_acquire);
    std::lock_guard lock(_captures_mutex);
    for (auto &cap : _order) {
      if (!cap->session || cap->cursor_applied == visible) {
        continue;
      }
      cap->cursor_applied = visible;
      try {
        if (auto session2 = cap->session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>()) {
          session2.IsCursorCaptureEnabled(visible);
        }
      } catch (...) {
      }
    }
  }

private:
  static DirectXPixelFormat pixel_format_for(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized;
  }

  static void init_thread_apartment() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
  }

  void mark_dirty() {
    {
      std::lock_guard lock(_dirty_mutex);
      _dirty = true;
    }
    _dirty_cv.notify_one();
  }

  bool ensure_latest_texture(capture_t &cap, UINT w, UINT h) {
    if (cap.latest && cap.latest_w == w && cap.latest_h == h) {
      return true;
    }
    cap.latest = nullptr;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = _format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(_device->CreateTexture2D(&desc, nullptr, cap.latest.put()))) {
      BOOST_LOG(error) << "App-window capture: failed to create a " << w << 'x' << h << " frame texture";
      cap.latest = nullptr;
      return false;
    }
    cap.latest_srv = nullptr;
    if (FAILED(_device->CreateShaderResourceView(cap.latest.get(), nullptr, cap.latest_srv.put()))) {
      BOOST_LOG(error) << "App-window capture: failed to create a frame texture view";
      cap.latest = nullptr;
      return false;
    }
    cap.latest_w = w;
    cap.latest_h = h;
    return true;
  }

  struct quad_cb_t {
    float dst[4];  ///< NDC x0, y0 (top), x1, y1 (bottom)
    float uv[4];  ///< u0, v0, u1, v1
    float mode[4];  ///< x: write alpha as grey, y: treat the source as opaque
  };

  bool ensure_pipeline() {
    if (_vs && _ps) {
      return true;
    }
    if (_pipeline_failed) {
      return false;
    }
    static const char *source = R"(
cbuffer cb : register(b0) { float4 dst; float4 uv; float4 mode; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
  float2 p = float2(id & 1, id >> 1);
  VSOut o;
  o.pos = float4(lerp(dst.x, dst.z, p.x), lerp(dst.y, dst.w, p.y), 0.0, 1.0);
  o.uv = float2(lerp(uv.x, uv.z, p.x), lerp(uv.y, uv.w, p.y));
  return o;
}
float4 ps(VSOut i) : SV_Target {
  float4 c = tex.Sample(smp, i.uv);
  if (mode.y > 0.5) c = float4(c.rgb, 1.0);
  if (mode.x > 0.5) return float4(c.a, c.a, c.a, c.a);
  return c;
}
)";
    winrt::com_ptr<ID3DBlob> vs_blob, ps_blob, err;
    if (FAILED(D3DCompile(source, strlen(source), "aw_composite", nullptr, nullptr, "vs", "vs_5_0", 0, 0, vs_blob.put(), err.put())) ||
        FAILED(D3DCompile(source, strlen(source), "aw_composite", nullptr, nullptr, "ps", "ps_5_0", 0, 0, ps_blob.put(), err.put()))) {
      BOOST_LOG(error) << "App-window composite: shader compilation failed: " << (err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
      _pipeline_failed = true;
      return false;
    }
    if (FAILED(_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, _vs.put())) ||
        FAILED(_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, _ps.put()))) {
      BOOST_LOG(error) << "App-window composite: shader creation failed";
      _pipeline_failed = true;
      return false;
    }
    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth = sizeof(quad_cb_t);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // sources are premultiplied
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(_device->CreateBuffer(&cbd, nullptr, _cb.put())) || FAILED(_device->CreateSamplerState(&sd, _sampler.put())) ||
        FAILED(_device->CreateBlendState(&bd, _blend.put())) || FAILED(_device->CreateRasterizerState(&rd, _raster.put()))) {
      BOOST_LOG(error) << "App-window composite: pipeline state creation failed";
      _pipeline_failed = true;
      return false;
    }
    return true;
  }

  /** Caller holds _captures_mutex. First-fit slot in the popup band for a popup
   *  of w x h; sticky per window while it lives. Returns false when nothing fits. */
  bool assign_slot(HWND hwnd, LONG w, LONG h, RECT &out) {
    if (auto it = _slots.find(hwnd); it != _slots.end()) {
      if (it->second.right - it->second.left == w && it->second.bottom - it->second.top == h) {
        out = it->second;
        return true;
      }
      _slots.erase(it);  // resized: place again
    }
    // The whole colour canvas is available except the window's own rectangle
    // (its real size, which the window band only bounds): first the popup band
    // below the window band, then free space right of and below the window.
    const LONG canvas_right = static_cast<LONG>(_canvas_w), canvas_bottom = _bands.canvas_h;
    RECT window_rect {0, 0, _target_capture ? std::min<LONG>(_target_capture->rect.right - _target_capture->rect.left, canvas_right) : canvas_right,
                      _target_capture ? std::min<LONG>(_target_capture->rect.bottom - _target_capture->rect.top, _bands.window_h) : _bands.window_h};
    w = std::min(w, canvas_right);
    h = std::min(h, canvas_bottom);
    if (w <= 0 || h <= 0) return false;
    // Candidate positions: band origin, the window's right/bottom edges, and
    // the right/bottom edges of existing slots (+8).
    std::vector<POINT> candidates {{0, _bands.popup_y}, {window_rect.right + 8, 0}, {0, window_rect.bottom + 8}};
    for (const auto &[other, r] : _slots) {
      candidates.push_back({r.right + 8, r.top});
      candidates.push_back({r.left, r.bottom + 8});
      candidates.push_back({0, r.bottom + 8});
      candidates.push_back({window_rect.right + 8, r.bottom + 8});
    }
    std::sort(candidates.begin(), candidates.end(), [](const POINT &a, const POINT &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    for (const POINT &c : candidates) {
      RECT r {c.x, c.y, c.x + w, c.y + h};
      if (r.right > canvas_right || r.bottom > canvas_bottom || r.left < 0 || r.top < 0) continue;
      RECT x {};
      if (IntersectRect(&x, &r, &window_rect)) continue;
      bool free = true;
      for (const auto &[other, o] : _slots) {
        if (IntersectRect(&x, &r, &o)) {
          free = false;
          break;
        }
      }
      if (free) {
        _slots[hwnd] = r;
        out = r;
        return true;
      }
    }
    return false;
  }

  /** Caller holds _captures_mutex. Slots the window grew into are given up (reassigned next tick). */
  void evict_slots_under_window() {
    if (!_target_capture) return;
    RECT window_rect {0, 0, std::min<LONG>(_target_capture->rect.right - _target_capture->rect.left, static_cast<LONG>(_canvas_w)),
                      std::min<LONG>(_target_capture->rect.bottom - _target_capture->rect.top, _bands.window_h)};
    for (auto it = _slots.begin(); it != _slots.end();) {
      RECT x {};
      if (IntersectRect(&x, &it->second, &window_rect)) {
        it = _slots.erase(it);
      } else {
        ++it;
      }
    }
  }

  /** Tell the main process where every popup sits (caller holds _captures_mutex). */
  void publish_slots() {
    std::vector<platf::dxgi::popup_slot_t> now;
    for (auto &cap : _order) {
      if (cap->is_target) continue;
      auto it = _slots.find(cap->hwnd);
      if (it == _slots.end()) continue;
      platf::dxgi::popup_slot_t sl {};
      sl.hwnd = reinterpret_cast<std::uintptr_t>(cap->hwnd);
      sl.host_x = cap->rect.left;
      sl.host_y = cap->rect.top;
      sl.w = it->second.right - it->second.left;
      sl.h = it->second.bottom - it->second.top;
      sl.canvas_x = it->second.left;
      sl.canvas_y = it->second.top;
      sl.dialog = cap->dialog ? 1 : 0;
      now.push_back(sl);
      if (now.size() >= platf::dxgi::WGC_POPUP_SLOTS_MAX) break;
    }
    if (now.size() == _published.size() && std::equal(now.begin(), now.end(), _published.begin(), [](const auto &a, const auto &b) {
          return std::memcmp(&a, &b, sizeof(a)) == 0;
        })) {
      return;
    }
    _published = now;
    if (!_pipe || !_pipe->is_connected()) return;
    platf::dxgi::popup_slots_t msg {};
    msg.magic = platf::dxgi::WGC_POPUP_SLOTS_MESSAGE_MAGIC;
    msg.count = static_cast<uint32_t>(now.size());
    std::copy(now.begin(), now.end(), msg.slots);
    _pipe->send(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&msg), sizeof(msg)));
  }

  /** Draws src[sx, sx+w) x [sy, sy+h) of `cap` at canvas (dx, dy); alpha band when `alpha_band`. */
  void draw_quad(const capture_t &cap, UINT sx, UINT sy, UINT w, UINT h, UINT dx, UINT dy, bool alpha_band) {
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(_context->Map(_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return;
    }
    auto *cb = static_cast<quad_cb_t *>(mapped.pData);
    const float W = static_cast<float>(_canvas_w), H = static_cast<float>(_canvas_h);
    // The alpha band holds the canvas' alpha at half resolution below it.
    const float k = alpha_band ? 0.5f : 1.f;
    const float x0 = dx * k, y0 = (alpha_band ? static_cast<float>(_bands.alpha_y) : 0.f) + dy * k;
    cb->dst[0] = x0 / W * 2.f - 1.f;
    cb->dst[1] = 1.f - y0 / H * 2.f;
    cb->dst[2] = (x0 + w * k) / W * 2.f - 1.f;
    cb->dst[3] = 1.f - (y0 + h * k) / H * 2.f;
    cb->uv[0] = static_cast<float>(sx) / cap.latest_w;
    cb->uv[1] = static_cast<float>(sy) / cap.latest_h;
    cb->uv[2] = static_cast<float>(sx + w) / cap.latest_w;
    cb->uv[3] = static_cast<float>(sy + h) / cap.latest_h;
    cb->mode[0] = alpha_band ? 1.f : 0.f;
    cb->mode[1] = cap.gdi ? 1.f : 0.f;
    cb->mode[2] = cb->mode[3] = 0.f;
    _context->Unmap(_cb.get(), 0);
    ID3D11ShaderResourceView *srv = cap.latest_srv.get();
    _context->PSSetShaderResources(0, 1, &srv);
    _context->Draw(4, 0);
  }

  /** Called on WGC's free-threaded callback thread. */
  void on_frame(const std::shared_ptr<capture_t> &cap, Direct3D11CaptureFramePool const &sender) {
    if (_stop.load(std::memory_order_acquire)) {
      return;
    }
    try {
      Direct3D11CaptureFrame frame = nullptr;
      while (auto next = sender.TryGetNextFrame()) {
        frame = std::move(next);
      }
      if (!frame) {
        return;
      }
      const auto content = frame.ContentSize();
      if (content.Width <= 0 || content.Height <= 0) {
        return;
      }
      if (content.Width != cap->pool_size.Width || content.Height != cap->pool_size.Height) {
        // Resized: the pool follows, the next frame lands at the new size.
        cap->pool_size = content;
        frame = nullptr;
        sender.Recreate(_winrt_device, pixel_format_for(_format), 2, content);
        return;
      }
      auto surface = frame.Surface();
      winrt::com_ptr<IDirect3DDxgiInterfaceAccess> access;
      if (FAILED(winrt::get_unknown(surface)->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), access.put_void()))) {
        return;
      }
      winrt::com_ptr<ID3D11Texture2D> frame_tex;
      if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<::IUnknown **>(frame_tex.put_void())))) {
        return;
      }
      static const int64_t qpc_frequency = [] {
        LARGE_INTEGER frequency {};
        QueryPerformanceFrequency(&frequency);
        return static_cast<int64_t>(frequency.QuadPart);
      }();
      const uint64_t qpc = platf::dxgi::wgc_policy::system_relative_time_to_qpc(frame.SystemRelativeTime().count(), qpc_frequency);
      {
        std::lock_guard ctx_lock(_ctx_mutex);
        if (ensure_latest_texture(*cap, static_cast<UINT>(content.Width), static_cast<UINT>(content.Height))) {
          D3D11_BOX box {0, 0, 0, static_cast<UINT>(content.Width), static_cast<UINT>(content.Height), 1};
          _context->CopySubresourceRegion(cap->latest.get(), 0, 0, 0, 0, frame_tex.get(), 0, &box);
          cap->latest_qpc = qpc;
        }
      }
      frame = nullptr;
      _frames_in.fetch_add(1, std::memory_order_relaxed);
      mark_dirty();
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(debug) << "App-window capture: frame callback failed: " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
    }
  }

  /** Caller holds _captures_mutex. */
  std::shared_ptr<capture_t> create_capture(HWND hwnd, bool is_target) {
    auto cap = std::make_shared<capture_t>();
    cap->hwnd = hwnd;
    cap->is_target = is_target;
    app_window_layout::frame_rect(hwnd, cap->rect);
    if (!is_target && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) && _format == DXGI_FORMAT_B8G8R8A8_UNORM) {
      // Layered popups (UpdateLayeredWindow skins, some tooltips) have no
      // redirection surface: WGC and PrintWindow return blanks for them.
      // Their pixels exist only on screen, so copy them from there.
      cap->gdi = true;
      cap->screen_copy = true;
      BOOST_LOG(info) << "App-window capture: popup 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << " is layered; copying it from the screen";
      return cap;
    }
    try {
      auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      GraphicsCaptureItem item = nullptr;
      const HRESULT hr = factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
      if (FAILED(hr) || !item) {
        throw winrt::hresult_error(FAILED(hr) ? hr : E_FAIL);
      }
      const auto size = item.Size();
      if (size.Width <= 0 || size.Height <= 0) {
        throw winrt::hresult_error(E_INVALIDARG);
      }
      cap->item = item;
      cap->pool_size = size;
      cap->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(_winrt_device, pixel_format_for(_format), 2, size);
      std::weak_ptr<capture_t> weak = cap;
      cap->frame_token = cap->pool.FrameArrived([this, weak](Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &) {
        if (auto c = weak.lock()) {
          on_frame(c, sender);
        }
      });
      cap->closed_token = item.Closed([weak, is_target](GraphicsCaptureItem const &, winrt::Windows::Foundation::IInspectable const &) {
        if (auto c = weak.lock()) {
          c->closed.store(true, std::memory_order_release);
        }
        if (is_target) {
          BOOST_LOG(warning) << "App-window capture: the window's capture item closed";
          g_capture_item_closed.store(true, std::memory_order_release);
        }
      });
      cap->session = cap->pool.CreateCaptureSession(item);
      try {
        if (auto session3 = cap->session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>()) {
          session3.IsBorderRequired(false);
        }
      } catch (...) {
      }
      // WGC paces frame delivery at ~60 Hz unless told otherwise.
      try {
        if (g_config.min_update_interval_100ns > 0 &&
            winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
          cap->session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan {g_config.min_update_interval_100ns});
        }
      } catch (...) {
      }
      try {
        if (auto session2 = cap->session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>()) {
          const bool visible = g_cursor_visible.load(std::memory_order_acquire);
          session2.IsCursorCaptureEnabled(visible);
          cap->cursor_applied = visible;
        }
      } catch (...) {
      }
      cap->session.StartCapture();
      return cap;
    } catch (const winrt::hresult_error &ex) {
      destroy_capture(*cap);
      if (is_target || _format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        BOOST_LOG(error) << "App-window capture: CreateForWindow(0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec
                         << ") failed: " << ex.code() << " - " << winrt::to_string(ex.message());
        return nullptr;
      }
      // Popups WGC will not capture (rare) are printed through GDI instead.
      BOOST_LOG(info) << "App-window capture: popup 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec
                      << " refused by WGC (" << ex.code() << "), using PrintWindow";
      cap->gdi = true;
      return cap;
    }
  }

  void destroy_capture(capture_t &cap) noexcept {
    try {
      if (cap.pool && cap.frame_token.value != 0) {
        cap.pool.FrameArrived(cap.frame_token);
      }
      if (cap.item && cap.closed_token.value != 0) {
        cap.item.Closed(cap.closed_token);
      }
      if (cap.session) {
        cap.session.Close();
      }
      if (cap.pool) {
        cap.pool.Close();
      }
    } catch (...) {
    }
    cap.frame_token = {};
    cap.closed_token = {};
    cap.session = nullptr;
    cap.pool = nullptr;
    cap.item = nullptr;
  }

  /** PrintWindow a GDI-fallback popup into its frame texture (layout thread). */
  void refresh_gdi_capture(capture_t &cap) {
    const auto now = std::chrono::steady_clock::now();
    if (cap.gdi && now - cap.gdi_last < std::chrono::milliseconds(33)) {
      return;
    }
    cap.gdi_last = now;
    const int w = cap.rect.right - cap.rect.left, h = cap.rect.bottom - cap.rect.top;
    if (w <= 0 || h <= 0) {
      return;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
      HGDIOBJ old = SelectObject(mem, dib);
      if (cap.gdi && !cap.screen_copy) {
        ok = PrintWindow(cap.hwnd, mem, PW_RENDERFULLCONTENT) != 0;
      } else {
        // Seeding a window WGC has not delivered for yet: layered windows
        // (UpdateLayeredWindow) render nothing through PrintWindow, so copy
        // what is on the host screen at its rectangle instead.
        ok = BitBlt(mem, 0, 0, w, h, screen, cap.rect.left, cap.rect.top, SRCCOPY | CAPTUREBLT) != 0;
      }
      GdiFlush();
      if (ok) {
        std::lock_guard ctx_lock(_ctx_mutex);
        if (!cap.gdi && cap.latest) {
          ok = false;  // WGC got there first
        } else if (ensure_latest_texture(cap, static_cast<UINT>(w), static_cast<UINT>(h))) {
          _context->UpdateSubresource(cap.latest.get(), 0, nullptr, bits, static_cast<UINT>(w) * 4, 0);
          LARGE_INTEGER qpc {};
          QueryPerformanceCounter(&qpc);
          cap.latest_qpc = static_cast<uint64_t>(qpc.QuadPart);
        }
      }
      SelectObject(mem, old);
    }
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (ok) {
      mark_dirty();
    }
  }

  /** Follows the window and its popups (which ones exist, where they are). */
  void layout_thread_proc() {
    init_thread_apartment();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::vector<std::pair<HWND, RECT>> last_layout;
    while (!_stop.load(std::memory_order_acquire)) {
      if (!IsWindow(_target)) {
        BOOST_LOG(warning) << "App-window capture: the window is gone";
        g_capture_item_closed.store(true, std::memory_order_release);
        break;
      }
      if (IsIconic(_target)) {
        // Minimized: keep the last picture, nothing to follow.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      auto popups = app_window_layout::popup_windows(_target);
      std::vector<std::pair<HWND, RECT>> layout;
      std::vector<std::shared_ptr<capture_t>> gdi_refresh;
      {
        std::lock_guard lock(_captures_mutex);
        RECT target_rect {};
        if (app_window_layout::frame_rect(_target, target_rect)) {
          _target_capture->rect = target_rect;
        }
        evict_slots_under_window();
        layout.emplace_back(_target, _target_capture->rect);
        std::vector<std::shared_ptr<capture_t>> order;
        order.push_back(_target_capture);
        for (const auto &p : popups) {
          std::shared_ptr<capture_t> cap;
          for (auto &existing : _order) {
            if (!existing->is_target && existing->hwnd == p.hwnd && !existing->closed.load(std::memory_order_acquire)) {
              cap = existing;
              break;
            }
          }
          if (!cap) {
            const auto failed = _failed.find(p.hwnd);
            if (failed != _failed.end() && std::chrono::steady_clock::now() - failed->second < std::chrono::seconds(1)) {
              continue;
            }
            cap = create_capture(p.hwnd, false);
            if (!cap) {
              _failed[p.hwnd] = std::chrono::steady_clock::now();
              continue;
            }
            _failed.erase(p.hwnd);
          }
          cap->rect = p.rect;
          cap->dialog = p.dialog;
          cap->inline_part = app_window_layout::is_inline_part(_target_capture->rect, p);
          RECT slot {};
          if (cap->inline_part) {
            _slots.erase(p.hwnd);
          } else if (!assign_slot(p.hwnd, p.rect.right - p.rect.left, p.rect.bottom - p.rect.top, slot)) {
            continue;  // no room in the popup band right now
          }
          order.push_back(cap);
          layout.emplace_back(p.hwnd, p.rect);
          if (cap->gdi) {
            gdi_refresh.push_back(cap);
          } else if (!cap->seeded && !cap->latest &&
                     std::chrono::steady_clock::now() - cap->created > std::chrono::milliseconds(150)) {
            // WGC only delivers once the window presents something new; a
            // popup painted before we started capturing (layered skin parts,
            // static bars) would stay blank until it repaints. Take its
            // current picture once; WGC takes over from the next repaint.
            cap->seeded = true;
            gdi_refresh.push_back(cap);
          }
        }
        // Whatever is no longer part of the picture goes away.
        for (auto &old : _order) {
          if (old->is_target) {
            continue;
          }
          bool kept = false;
          for (auto &cap : order) {
            if (cap == old) {
              kept = true;
              break;
            }
          }
          if (!kept) {
            destroy_capture(*old);
            _slots.erase(old->hwnd);
          }
        }
        _order = std::move(order);
        publish_slots();
      }
      bool changed = layout.size() != last_layout.size();
      for (size_t i = 0; !changed && i < layout.size(); ++i) {
        changed = layout[i].first != last_layout[i].first || !EqualRect(&layout[i].second, &last_layout[i].second);
      }
      if (changed) {
        last_layout = std::move(layout);
        mark_dirty();
      }
      for (auto &cap : gdi_refresh) {
        refresh_gdi_capture(*cap);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  }

  void composite_thread_proc() {
    init_thread_apartment();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD task_idx = 0;
    if (HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task_idx)) {
      AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    }
    const auto min_gap = std::chrono::microseconds(1000000 / (2 * _fps));
    auto last = std::chrono::steady_clock::now() - min_gap;
    auto stats_since = last;
    unsigned composites = 0;
    while (!_stop.load(std::memory_order_acquire)) {
      {
        std::unique_lock lock(_dirty_mutex);
        _dirty_cv.wait(lock, [this] {
          return _dirty || _stop.load(std::memory_order_acquire);
        });
        _dirty = false;
      }
      if (_stop.load(std::memory_order_acquire)) {
        break;
      }
      // Several windows repaint for one vsync; publish at most two canvases per frame interval.
      const auto now = std::chrono::steady_clock::now();
      if (now - last < min_gap) {
        std::this_thread::sleep_for(min_gap - (now - last));
      }
      try {
        composite();
        ++composites;
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "App-window composite failed: " << ex.code() << " - " << winrt::to_string(ex.message());
      } catch (...) {
        BOOST_LOG(error) << "App-window composite failed";
      }
      last = std::chrono::steady_clock::now();
      if (last - stats_since >= std::chrono::seconds(2)) {
        const double secs = std::chrono::duration<double>(last - stats_since).count();
        BOOST_LOG(info) << "App-window composite rate: " << std::fixed << std::setprecision(1) << (composites / secs) << " canvases/s, "
                        << (_frames_in.exchange(0) / secs) << " window frames/s";
        composites = 0;
        stats_since = last;
      }
    }
  }

  void composite() {
    std::vector<std::shared_ptr<capture_t>> order;
    RECT target_rect {};
    {
      std::lock_guard lock(_captures_mutex);
      order = _order;
      target_rect = _target_capture->rect;
    }
    if (IsIconic(_target)) {
      return;
    }
    std::map<HWND, RECT> slots;
    {
      std::lock_guard lock(_captures_mutex);
      slots = _slots;
    }

    std::unique_lock ctx_lock(_ctx_mutex);
    if (!ensure_pipeline()) {
      return;
    }
    auto &keyed = _shared.get_keyed_mutex();
    const HRESULT hr = keyed->AcquireSync(0, 200);
    if (hr == WAIT_TIMEOUT) {
      mark_dirty();  // try again
      return;
    }
    if (hr != S_OK && hr != WAIT_ABANDONED) {
      BOOST_LOG(error) << "App-window composite: failed to acquire the shared mutex: " << std::format("0x{:08X}", static_cast<uint32_t>(hr));
      return;
    }
    auto release = util::fail_guard([&]() {
      keyed->ReleaseSync(0);
    });
    auto &canvas = _shared.get_shared_texture();
    if (!_canvas_rtv && FAILED(_device->CreateRenderTargetView(canvas.get(), nullptr, _canvas_rtv.put()))) {
      BOOST_LOG(error) << "App-window composite: cannot create the canvas render target view";
      return;
    }
    // Transparent black everywhere: the picture band ends up premultiplied
    // by coverage, the alpha band holds the coverage itself.
    const float clear[4] {0.f, 0.f, 0.f, 0.f};
    _context->ClearRenderTargetView(_canvas_rtv.get(), clear);
    ID3D11RenderTargetView *rtv = _canvas_rtv.get();
    _context->OMSetRenderTargets(1, &rtv, nullptr);
    const float blend_factor[4] {0.f, 0.f, 0.f, 0.f};
    _context->OMSetBlendState(_blend.get(), blend_factor, 0xffffffff);
    _context->RSSetState(_raster.get());
    D3D11_VIEWPORT vp {0.f, 0.f, static_cast<float>(_canvas_w), static_cast<float>(_canvas_h), 0.f, 1.f};
    _context->RSSetViewports(1, &vp);
    _context->IASetInputLayout(nullptr);
    _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    _context->VSSetShader(_vs.get(), nullptr, 0);
    _context->PSSetShader(_ps.get(), nullptr, 0);
    ID3D11Buffer *cb = _cb.get();
    _context->VSSetConstantBuffers(0, 1, &cb);
    _context->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *sampler = _sampler.get();
    _context->PSSetSamplers(0, 1, &sampler);
    uint64_t qpc = 0;
    for (auto &cap : order) {
      if (!cap->latest || !cap->latest_srv || cap->latest_w == 0 || cap->latest_h == 0) {
        continue;
      }
      // The window sits at the canvas origin (clipped to the window band);
      // every popup has a slot of its own in the popup band.
      LONG dx = 0, dy = 0;
      LONG band_bottom = _bands.window_h;
      if (cap->inline_part) {
        // A piece of the app's UI: in place over the parent
        dx = cap->rect.left - target_rect.left;
        dy = cap->rect.top - target_rect.top;
      } else if (!cap->is_target) {
        auto it = slots.find(cap->hwnd);
        if (it == slots.end()) continue;
        dx = it->second.left;
        dy = it->second.top;
        band_bottom = _bands.canvas_h;
      }
      UINT sx = 0, sy = 0, w = cap->latest_w, h = cap->latest_h;
      // A maximized window's capture covers GetWindowRect, whose invisible
      // borders hang off-screen; the picture (and the input mapping) use the
      // DWM frame, so skip the border part of the texture.
      {
        RECT raw {};
        const LONG frame_w = cap->rect.right - cap->rect.left, frame_h = cap->rect.bottom - cap->rect.top;
        if (GetWindowRect(cap->hwnd, &raw) && (raw.right - raw.left) == static_cast<LONG>(w) && (raw.bottom - raw.top) == static_cast<LONG>(h) &&
            (frame_w != static_cast<LONG>(w) || frame_h != static_cast<LONG>(h))) {
          const LONG bx = cap->rect.left - raw.left, by = cap->rect.top - raw.top;
          if (bx > 0 && bx < static_cast<LONG>(w)) {
            sx = static_cast<UINT>(bx);
            w = static_cast<UINT>(std::min<LONG>(frame_w, static_cast<LONG>(w) - bx));
          }
          if (by > 0 && by < static_cast<LONG>(h)) {
            sy = static_cast<UINT>(by);
            h = static_cast<UINT>(std::min<LONG>(frame_h, static_cast<LONG>(h) - by));
          }
        }
      }
      if (dx < 0) {
        if (static_cast<UINT>(-dx) >= w) continue;
        sx += static_cast<UINT>(-dx);
        w -= static_cast<UINT>(-dx);
        dx = 0;
      }
      if (dy < 0) {
        if (static_cast<UINT>(-dy) >= h) continue;
        sy += static_cast<UINT>(-dy);
        h -= static_cast<UINT>(-dy);
        dy = 0;
      }
      if (w == 0 || h == 0 || dx >= static_cast<LONG>(_canvas_w) || dy >= band_bottom) {
        continue;
      }
      w = std::min<UINT>(w, _canvas_w - static_cast<UINT>(dx));
      h = std::min<UINT>(h, static_cast<UINT>(band_bottom - dy));
      draw_quad(*cap, sx, sy, w, h, static_cast<UINT>(dx), static_cast<UINT>(dy), false);
      draw_quad(*cap, sx, sy, w, h, static_cast<UINT>(dx), static_cast<UINT>(dy), true);
      qpc = std::max(qpc, cap->latest_qpc);
    }
    ID3D11ShaderResourceView *null_srv = nullptr;
    _context->PSSetShaderResources(0, 1, &null_srv);
    ID3D11RenderTargetView *null_rtv = nullptr;
    _context->OMSetRenderTargets(1, &null_rtv, nullptr);
    if (qpc == 0) {
      LARGE_INTEGER now {};
      QueryPerformanceCounter(&now);
      qpc = static_cast<uint64_t>(now.QuadPart);
    }
    _shared.publish_frame_metadata(qpc);
    release.disable();
    keyed->ReleaseSync(0);
    ctx_lock.unlock();
    _shared.signal_frame_ready();
  }

  IDirect3DDevice _winrt_device;
  winrt::com_ptr<ID3D11Device> _device;
  winrt::com_ptr<ID3D11DeviceContext> _context;
  SharedResourceManager &_shared;
  DXGI_FORMAT _format;
  HWND _target;
  UINT _canvas_w, _canvas_h;  ///< encoder frame (colour canvas + half-resolution alpha band)
  app_window_layout::frame_bands_t _bands;
  UINT _color_h;  ///< colour canvas height (window band + popup band)
  int _fps;
  std::shared_ptr<AsyncNamedPipe> _pipe;
  std::map<HWND, RECT> _slots;  ///< popup -> its slot on the canvas (sticky while the popup lives)
  std::vector<platf::dxgi::popup_slot_t> _published;  ///< last slot message sent
  // Compositing pipeline (alpha-blended quads; the alpha band gets alpha as grey)
  winrt::com_ptr<ID3D11VertexShader> _vs;
  winrt::com_ptr<ID3D11PixelShader> _ps;
  winrt::com_ptr<ID3D11Buffer> _cb;
  winrt::com_ptr<ID3D11SamplerState> _sampler;
  winrt::com_ptr<ID3D11BlendState> _blend;
  winrt::com_ptr<ID3D11RasterizerState> _raster;
  bool _pipeline_failed = false;

  std::mutex _ctx_mutex;  ///< serializes every use of the immediate context
  winrt::com_ptr<ID3D11RenderTargetView> _canvas_rtv;

  std::mutex _captures_mutex;
  std::shared_ptr<capture_t> _target_capture;
  std::vector<std::shared_ptr<capture_t>> _order;  ///< composition order, target first
  std::map<HWND, std::chrono::steady_clock::time_point> _failed;

  std::mutex _dirty_mutex;
  std::condition_variable _dirty_cv;
  bool _dirty = false;
  std::atomic<unsigned> _frames_in {0};
  std::atomic<bool> _stop {true};
  std::thread _layout_thread;
  std::thread _composite_thread;
};

/**
 * @brief Runs an app-window session: handshake, compositor, message loop.
 */
int run_window_session(std::shared_ptr<AsyncNamedPipe> pipe_shared, D3D11DeviceManager &d3d11_manager) {
  const bool force_sdr_capture = g_config_received && ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) != 0);
  const auto capture_surface_format = platf::dxgi::wgc_policy::select_capture_surface_format(
    g_config_received,
    force_sdr_capture,
    g_config.dynamic_range,
    g_config.advanced_color_capture
  );
  const DXGI_FORMAT capture_format =
    capture_surface_format == platf::dxgi::wgc_policy::capture_surface_format::rgba16_float ?
      DXGI_FORMAT_R16G16B16A16_FLOAT :
      DXGI_FORMAT_B8G8R8A8_UNORM;

  SharedResourceManager shared_resource_manager;
  if (!shared_resource_manager.initialize_all(d3d11_manager.get_device(), g_config.canvas_width, g_config.canvas_height, capture_format)) {
    return 1;
  }
  platf::dxgi::shared_handle_data_t handle_data = shared_resource_manager.get_shared_handle_data();
  std::span<const uint8_t> handle_message(reinterpret_cast<const uint8_t *>(&handle_data), sizeof(handle_data));
  {
    int waited_ms = 0;
    while (!pipe_shared->is_connected() && waited_ms < 5000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      waited_ms += 50;
    }
    if (!pipe_shared->is_connected()) {
      BOOST_LOG(error) << "Timed out waiting for main process to connect (5000ms)";
      return 1;
    }
  }
  pipe_shared->send(handle_message);

  auto target = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(g_config.target_hwnd));
  WindowCompositor compositor {d3d11_manager.get_winrt_device(), d3d11_manager.get_device(), d3d11_manager.get_context(),
                               shared_resource_manager, capture_format, target, g_config.canvas_width, g_config.canvas_height,
                               g_config.target_fps > 0 ? g_config.target_fps : 60, pipe_shared};
  if (!compositor.start()) {
    return 1;
  }
  setup_desktop_switch_hook();

  bool shutdown_requested = false;
  while (pipe_shared->is_connected() && !shutdown_requested && !g_capture_item_closed.load(std::memory_order_acquire)) {
    if (!process_window_messages(shutdown_requested)) {
      break;
    }
    poll_pending_secure_desktop_transition();
    compositor.update_cursor_capture();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  compositor.stop();
  BOOST_LOG(info) << (g_capture_item_closed.load(std::memory_order_acquire) ? "App-window capture ended, shutting down..." : "Main process disconnected, shutting down...");
  pipe_shared->stop();
  boost::log::core::get()->flush();
  BOOST_LOG(info) << "WGC Helper process terminated";
  return 0;
}

int main(int argc, char *argv[]) {
  // Set up default config and log level
  // Use session-mode logs so rare startup deadlocks don't get overwritten by the next helper run.
  auto log_deinit = logging::init(2, get_temp_log_path());

  // Check command line arguments for pipe names
  if (argc < 2) {
    BOOST_LOG(error) << "Usage: " << argv[0] << " <pipe_name_guid>";
    return 1;
  }

  std::string pipe_name = argv[1];
  BOOST_LOG(info) << "Using pipe name: " << pipe_name;

  // Initialize system settings (DPI awareness, thread priority, MMCSS)
  SystemInitializer system_initializer;
  if (!system_initializer.initialize_all()) {
    BOOST_LOG(error) << "System initialization failed, exiting...";
    return 1;
  }

  // Debug: Verify system settings
  BOOST_LOG(info) << "DPI awareness set: " << (system_initializer.is_dpi_awareness_set() ? "YES" : "NO");
  BOOST_LOG(info) << "Thread priority set: " << (system_initializer.is_thread_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "GPU scheduling priority set: " << (system_initializer.is_gpu_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "MMCSS characteristics set: " << (system_initializer.is_mmcss_characteristics_set() ? "YES" : "NO");

  BOOST_LOG(info) << "Starting Windows Graphics Capture helper process...";

  // Create named pipe for communication with main process using provided pipe name
  AnonymousPipeFactory pipe_factory;

  auto comm_pipe = pipe_factory.create_client(pipe_name);
  auto pipe_shared = std::make_shared<AsyncNamedPipe>(std::move(comm_pipe));
  g_communication_pipe_weak = pipe_shared;  // Store weak reference for desktop hook callback

  if (!setup_pipe_callbacks(*pipe_shared)) {
    BOOST_LOG(error) << "Failed to start communication pipe";
    return 1;
  }

  constexpr auto max_wait = std::chrono::milliseconds(5000);
  {
    std::unique_lock lock(g_config_mutex);
    if (!g_config_cv.wait_for(lock, max_wait, []() {
          return g_config_received;
        })) {
      BOOST_LOG(error) << "Timed out waiting for config data from main process (" << max_wait.count() << "ms)";
      return 1;
    }
  }

  // Create D3D11 device and context using the same adapter as the main process
  D3D11DeviceManager d3d11_manager;
  if (!d3d11_manager.initialize_all(g_config.adapter_luid)) {
    BOOST_LOG(error) << "D3D11 device initialization failed, exiting...";
    return 1;
  }

  // App-window sessions have their own capture path: one WGC item per window
  // of the streamed application, composited onto a fixed canvas.
  if (g_config.target_hwnd && g_config.canvas_width > 0 && g_config.canvas_height > 0) {
    return run_window_session(pipe_shared, d3d11_manager);
  }

  // Monitor management
  DisplayManager display_manager;
  if (!display_manager.select_monitor(g_config)) {
    BOOST_LOG(error) << "Monitor selection failed, exiting...";
    return 1;
  }

  if (!display_manager.get_monitor_info()) {
    BOOST_LOG(error) << "Failed to get monitor info, exiting...";
    return 1;
  }

  // Create GraphicsCaptureItem for monitor using interop.
  // Virtual display topology can churn briefly while Sunshine is applying display settings,
  // so retry monitor selection/item creation before treating startup as fatal.
  GraphicsCaptureItem item = nullptr;
  if (!display_manager.create_graphics_capture_item(item)) {
    constexpr int kMaxCreateItemAttempts = 8;
    bool created = false;
    for (int attempt = 1; attempt <= kMaxCreateItemAttempts && !created; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (!display_manager.select_monitor(g_config)) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": monitor selection failed";
        continue;
      }
      if (!display_manager.get_monitor_info()) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": failed to query monitor info";
        continue;
      }
      if (display_manager.create_graphics_capture_item(item)) {
        BOOST_LOG(info) << "GraphicsCaptureItem created after retry " << attempt
                        << "/" << kMaxCreateItemAttempts;
        created = true;
        break;
      }
    }
    if (!created) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem after retries";
      return 1;
    }
  }

  // Calculate final resolution based on config and monitor info
  display_manager.configure_capture_resolution(item);

  // WGC silently stops delivering frames once the capture item closes; display mode
  // changes, DWM restarts and monitor removals all do this (especially on Windows 10).
  // Exit so the main process notices the broken pipe and reinitializes capture instead
  // of freezing on the last delivered frame.
  item.Closed([](GraphicsCaptureItem const &, winrt::Windows::Foundation::IInspectable const &) {
    BOOST_LOG(warning) << "GraphicsCaptureItem closed; shutting down so capture can be reinitialized";
    g_capture_item_closed.store(true, std::memory_order_release);
  });

  // Use FP16 whenever the stream is HDR or the target output is already in
  // Advanced Color, except when the main process asks for SDR-compatible
  // capture so RTX HDR/TrueHDR can synthesize the HDR frame itself.
  const bool force_sdr_capture =
    g_config_received &&
    ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) != 0);
  const auto capture_surface_format = platf::dxgi::wgc_policy::select_capture_surface_format(
    g_config_received,
    force_sdr_capture,
    g_config.dynamic_range,
    g_config.advanced_color_capture
  );
  const DXGI_FORMAT capture_format =
    capture_surface_format == platf::dxgi::wgc_policy::capture_surface_format::rgba16_float ?
      DXGI_FORMAT_R16G16B16A16_FLOAT :
      DXGI_FORMAT_B8G8R8A8_UNORM;

  // Create shared resource manager for texture, keyed mutex, and metadata
  SharedResourceManager shared_resource_manager;
  if (!shared_resource_manager.initialize_all(d3d11_manager.get_device(), display_manager.get_width(), display_manager.get_height(), capture_format)) {
    return 1;
  }

  // Send shared handle data via named pipe to main process
  platf::dxgi::shared_handle_data_t handle_data = shared_resource_manager.get_shared_handle_data();
  BOOST_LOG(info) << "Prepared shared handle message - Size: " << sizeof(handle_data) << " bytes, Handle: 0x" << std::hex << reinterpret_cast<uintptr_t>(handle_data.texture_handle) << std::dec;
  std::span<const uint8_t> handle_message(reinterpret_cast<const uint8_t *>(&handle_data), sizeof(handle_data));

  // Wait for connection and send the handle data
  BOOST_LOG(info) << "Waiting for main process to connect...";
  {
    constexpr int connect_timeout_ms = 5000;
    int connect_waited_ms = 0;
    while (!pipe_shared->is_connected() && connect_waited_ms < connect_timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      connect_waited_ms += 50;
    }

    if (!pipe_shared->is_connected()) {
      BOOST_LOG(error) << "Timed out waiting for main process to connect (" << connect_timeout_ms << "ms)";
      return 1;
    }
  }
  BOOST_LOG(info) << "Connected! Sending duplicated handle data...";
  pipe_shared->send(handle_message);
  BOOST_LOG(info) << "Duplicated handle data sent successfully to main process";

  // Create dependencies for capture manager
  WgcCaptureDependencies deps {
    d3d11_manager.get_winrt_device(),
    item,
    shared_resource_manager,
    d3d11_manager.get_context()
  };

  // Create WGC capture manager
  WgcCaptureManager wgc_capture_manager {capture_format, display_manager.get_width(), display_manager.get_height(), std::move(deps)};
  const auto initial_frame_buffer_size = std::clamp<uint32_t>(
    g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
    1,
    std::max<uint32_t>(1, g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : 1)
  );
  if (!wgc_capture_manager.create_or_adjust_frame_pool(initial_frame_buffer_size)) {
    BOOST_LOG(error) << "Failed to create frame pool";
    return 1;
  }

  if (!wgc_capture_manager.create_capture_session()) {
    BOOST_LOG(error) << "Failed to create capture session";
    return 1;
  }

  // Set up desktop switch hook for secure desktop detection
  setup_desktop_switch_hook();

  wgc_capture_manager.start_capture();

  // Main message loop
  bool shutdown_requested = false;
  while (pipe_shared->is_connected() && !shutdown_requested && !g_capture_item_closed.load(std::memory_order_acquire)) {
    // Process window messages and check for shutdown
    if (!process_window_messages(shutdown_requested)) {
      break;
    }

    poll_pending_secure_desktop_transition();
    wgc_capture_manager.update_cursor_capture();

    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Reduced from 5ms for lower IPC jitter
  }

  if (g_capture_item_closed.load(std::memory_order_acquire)) {
    BOOST_LOG(info) << "Capture item closed, shutting down...";
  } else {
    BOOST_LOG(info) << "Main process disconnected, shutting down...";
  }

  pipe_shared->stop();

  // Flush logs before exit
  boost::log::core::get()->flush();

  BOOST_LOG(info) << "WGC Helper process terminated";

  return 0;
}
