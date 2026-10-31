/**
 * @file src/keyboard_language.cpp
 * @brief See keyboard_language.h.
 */
#include "keyboard_language.h"

#include "logging.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <sddl.h>

  #include <atomic>
  #include <cwchar>
  #include <chrono>
  #include <mutex>
  #include <thread>
  #include <vector>

  #include "platform/windows/misc.h"
  #include "platform/windows/utf_utils.h"
#endif

namespace keyboard_language {

#ifdef _WIN32
  namespace {
    std::mutex g_mutex;
    HKL g_wanted = nullptr;  ///< what the client asked for (null: nothing)
    std::atomic<bool> g_watching {false};

    /** The signed-in user's keyboard layouts, in their order (the
     *  "Keyboard Layout\Preload" list of their hive, substitutes applied) */
    std::vector<HKL> user_layouts() {
      std::vector<HKL> out;
      HANDLE token = platf::retrieve_users_token(false);
      if (!token) return out;
      std::wstring sid;
      {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        LPWSTR text = nullptr;
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
          sid = text;
          LocalFree(text);
        }
      }
      CloseHandle(token);
      if (sid.empty()) return out;
      HKEY preload = nullptr, substitutes = nullptr;
      if (RegOpenKeyExW(HKEY_USERS, (sid + L"\\Keyboard Layout\\Preload").c_str(), 0, KEY_READ, &preload) != ERROR_SUCCESS) return out;
      RegOpenKeyExW(HKEY_USERS, (sid + L"\\Keyboard Layout\\Substitutes").c_str(), 0, KEY_READ, &substitutes);
      for (int i = 1; i < 64; ++i) {
        wchar_t klid[KL_NAMELENGTH] {};
        DWORD size = sizeof(klid);
        if (RegGetValueW(preload, nullptr, std::to_wstring(i).c_str(), RRF_RT_REG_SZ, nullptr, klid, &size) != ERROR_SUCCESS) break;
        wchar_t substitute[KL_NAMELENGTH] {};
        size = sizeof(substitute);
        // The layout (low word: the language; high word: which layout of it)
        const wchar_t *layout = substitutes && RegGetValueW(substitutes, nullptr, klid, RRF_RT_REG_SZ, nullptr, substitute, &size) == ERROR_SUCCESS ? substitute : klid;
        const auto id = static_cast<DWORD>(std::wcstoul(layout, nullptr, 16));
        const WORD lang = static_cast<WORD>(std::wcstoul(klid, nullptr, 16) & 0xFFFF);
        WORD device = static_cast<WORD>(id & 0xFFFF);
        if (id >> 16) {
          // A variant layout: its "Layout Id" names it in the HKL
          wchar_t value[16] {};
          size = sizeof(value);
          const std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\") + layout;
          if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Layout Id", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
            device = static_cast<WORD>(0xF000 | (std::wcstoul(value, nullptr, 16) & 0x0FFF));
          }
        }
        out.push_back(reinterpret_cast<HKL>(static_cast<std::uintptr_t>(MAKELONG(lang, device))));
      }
      if (substitutes) RegCloseKey(substitutes);
      RegCloseKey(preload);
      return out;
    }

    HKL layout_of(HWND hwnd) {
      return GetKeyboardLayout(GetWindowThreadProcessId(hwnd, nullptr));
    }

    /** Asks the window to take the layout (as the language bar does) */
    void apply(HWND hwnd, HKL hkl) {
      if (hwnd && layout_of(hwnd) != hkl) {
        PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hkl));
      }
    }

    /** Windows that come to the foreground take it too (Windows can keep a
     *  layout per app); a few times a second while a language is wanted */
    void watch() {
      if (g_watching.exchange(true)) return;
      std::thread([] {
        HWND last = nullptr;
        for (;;) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          HKL wanted;
          {
            std::lock_guard lock(g_mutex);
            wanted = g_wanted;
            if (!wanted) {
              g_watching = false;
              return;
            }
          }
          const HWND fg = GetForegroundWindow();
          if (fg != last) {
            last = fg;
            apply(fg, wanted);
          }
        }
      }).detach();
    }
  }  // namespace

  bool set(const std::string &language, std::string &error) {
    const auto name = utf_utils::from_utf8(language);
    const LCID lcid = LocaleNameToLCID(name.c_str(), LOCALE_ALLOW_NEUTRAL_NAMES);
    if (!lcid) {
      error = "unknown language";
      return false;
    }
    const WORD lang = LANGIDFROMLCID(lcid);
    const auto layouts = user_layouts();
    // The user's layout of that very language, else of that language at all
    // ("en" also takes English (United Kingdom))
    HKL chosen = nullptr;
    for (HKL hkl : layouts) {
      const WORD l = LOWORD(reinterpret_cast<std::uintptr_t>(hkl));
      if (l == lang) {
        chosen = hkl;
        break;
      }
      if (!chosen && PRIMARYLANGID(l) == PRIMARYLANGID(lang)) chosen = hkl;
    }
    if (!chosen) {
      error = "no keyboard layout for " + language + " on the host";
      BOOST_LOG(info) << "Keyboard language: " << error;
      return false;
    }
    {
      std::lock_guard lock(g_mutex);
      if (g_wanted != chosen) {
        BOOST_LOG(info) << "Keyboard language: " << language << " -> layout " << std::hex << reinterpret_cast<std::uintptr_t>(chosen) << std::dec;
      }
      g_wanted = chosen;
    }
    apply(GetForegroundWindow(), chosen);
    watch();
    return true;
  }

  void clear() {
    std::lock_guard lock(g_mutex);
    g_wanted = nullptr;
  }
#else
  bool set(const std::string &, std::string &error) {
    error = "not supported on this platform";
    return false;
  }

  void clear() {
  }
#endif

}  // namespace keyboard_language
