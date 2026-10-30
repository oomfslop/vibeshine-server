/**
 * @file src/app_launcher.cpp
 * @brief See app_launcher.h.
 */
#include "app_launcher.h"

#include "logging.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <userenv.h>

  #include <chrono>
  #include <cstdio>
  #include <map>
  #include <mutex>

  #include <nlohmann/json.hpp>

  #include "platform/common.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/utf_utils.h"
#endif

namespace app_launcher {

#ifdef _WIN32
  namespace {
    std::mutex g_mutex;
    list_t g_list;
    std::chrono::steady_clock::time_point g_listed_at;
    std::map<std::string, std::string> g_icons;  ///< key -> PNG

    constexpr auto list_ttl = std::chrono::minutes(10);

    std::wstring module_dir() {
      wchar_t path[MAX_PATH];
      const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
      std::wstring dir(path, n);
      const auto slash = dir.find_last_of(L"\\/");
      return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    }

    /** The signed-in user's home folder ("" when there is no user session) */
    std::wstring user_home() {
      HANDLE token = platf::retrieve_users_token(false);
      if (!token) return {};
      wchar_t path[MAX_PATH];
      DWORD size = MAX_PATH;
      std::wstring home;
      if (GetUserProfileDirectoryW(token, path, &size)) home = path;
      CloseHandle(token);
      return home;
    }

    std::string decode_base64(const std::string &in) {
      static const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
      };
      std::string out;
      out.reserve(in.size() * 3 / 4);
      int bits = 0, acc = 0;
      for (char c : in) {
        const int v = value(c);
        if (v < 0) continue;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out += static_cast<char>((acc >> bits) & 0xff);
        }
      }
      return out;
    }

    std::string key_of(const std::string &png) {
      std::uint64_t h = 1469598103934665603ull;
      for (unsigned char c : png) h = (h ^ c) * 1099511628211ull;
      char buf[17];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
      return buf;
    }

    /** Run the launchables tool as the user; its JSON, or "" */
    std::string run_tool() {
      const std::wstring tool = module_dir() + L"\\tools\\vibeshine_launchables.exe";
      if (GetFileAttributesW(tool.c_str()) == INVALID_FILE_ATTRIBUTES) {
        BOOST_LOG(warning) << "App launcher: " << utf_utils::to_utf8(tool) << " is missing";
        return {};
      }
      // Its stdout goes to a file of ours (the handle is inherited)
      wchar_t temp_dir[MAX_PATH], temp_file[MAX_PATH];
      GetTempPathW(MAX_PATH, temp_dir);
      if (!GetTempFileNameW(temp_dir, L"vsl", 0, temp_file)) return {};
      FILE *file = _wfopen(temp_file, L"w+b");
      if (!file) return {};
      std::string out;
      {
        std::error_code ec;
        boost::filesystem::path working_dir;
        platf::bp::environment env = platf::bp::this_process::env();
        auto child = platf::run_command(false, false, "\"" + utf_utils::to_utf8(tool) + "\"", working_dir, env, file, ec, nullptr);
        if (ec) {
          BOOST_LOG(warning) << "App launcher: couldn't run the launchables tool: " << ec.message();
        } else {
          // At most 30 s (the shell can hang on a broken shortcut)
          if (HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(child.id()))) {
            if (WaitForSingleObject(process, 30000) == WAIT_TIMEOUT) {
              BOOST_LOG(warning) << "App launcher: the launchables tool took too long";
              TerminateProcess(process, 1);
            }
            CloseHandle(process);
          }
          child.wait(ec);
        }
      }
      std::fflush(file);
      std::fseek(file, 0, SEEK_END);
      const long size = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (size > 0) {
        out.resize(static_cast<size_t>(size));
        out.resize(std::fread(out.data(), 1, out.size(), file));
      }
      std::fclose(file);
      DeleteFileW(temp_file);
      return out;
    }

    bool start_as_user(const std::string &cmd, bool console, std::string &error) {
      std::error_code ec;
      boost::filesystem::path working_dir(user_home());
      platf::bp::environment env = platf::bp::this_process::env();
      auto child = platf::run_command(false, console, cmd, working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        error = ec.message();
        BOOST_LOG(warning) << "App launcher: couldn't start [" << cmd << "]: " << error;
        return false;
      }
      child.detach();
      BOOST_LOG(info) << "App launcher: started [" << cmd << ']';
      return true;
    }
  }  // namespace

  list_t list(bool refresh) {
    std::lock_guard lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_list.ok && !refresh && now - g_listed_at < list_ttl) return g_list;
    const auto json_text = run_tool();
    list_t result;
    try {
      const auto doc = nlohmann::json::parse(json_text);
      std::map<std::string, std::string> icons;
      for (const auto &a : doc.at("apps")) {
        app_t app {a.value("name", ""), a.value("id", ""), ""};
        if (app.name.empty() || app.id.empty()) continue;
        const auto png = decode_base64(a.value("icon", ""));
        if (!png.empty()) {
          app.icon_key = key_of(png);
          icons[app.icon_key] = png;
        }
        result.apps.push_back(std::move(app));
      }
      for (const auto &c : doc.at("commands")) {
        command_t command {c.value("name", ""), c.value("path", "")};
        if (!command.name.empty()) result.commands.push_back(std::move(command));
      }
      result.ok = true;
      g_icons = std::move(icons);
      g_list = result;
      g_listed_at = now;
      BOOST_LOG(info) << "App launcher: " << result.apps.size() << " apps, " << result.commands.size() << " programs on the PATH";
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "App launcher: unreadable list from the launchables tool: " << e.what();
      if (g_list.ok) return g_list;
    }
    return result;
  }

  std::optional<std::string> icon_png(const std::string &key) {
    std::lock_guard lock(g_mutex);
    const auto it = g_icons.find(key);
    if (it == g_icons.end()) return std::nullopt;
    return it->second;
  }

  bool launch_app(const std::string &id, std::string &error) {
    if (id.empty() || id.find('"') != std::string::npos) {
      error = "invalid app id";
      return false;
    }
    // The shell starts it as from the Start menu (Store and desktop apps alike)
    return start_as_user("explorer.exe \"shell:AppsFolder\\" + id + "\"", false, error);
  }

  bool launch_command(const std::string &command_line, std::string &error) {
    if (command_line.find_first_not_of(" \t") == std::string::npos) {
      error = "empty command";
      return false;
    }
    // (a GUI program gets no console from this; a console program its own)
    return start_as_user(command_line, true, error);
  }
#else
  list_t list(bool) {
    return {};
  }

  std::optional<std::string> icon_png(const std::string &) {
    return std::nullopt;
  }

  bool launch_app(const std::string &, std::string &error) {
    error = "not supported on this platform";
    return false;
  }

  bool launch_command(const std::string &, std::string &error) {
    error = "not supported on this platform";
    return false;
  }
#endif

}  // namespace app_launcher
