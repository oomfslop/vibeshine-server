/**
 * @file tools/launchables.cpp
 * @brief Lists what can be launched on this machine, as the signed-in user
 *        sees it: every app of the Start menu (the shell's Apps folder: Win32
 *        and Store apps alike) with its icon, and the programs on the PATH.
 *        Vibeshine runs it as the user (the shell answers per user) and reads
 *        the list as JSON from its stdout:
 *
 *        {"apps":[{"name":..,"id":..,"icon":"<PNG, base64>"}..],
 *         "commands":[{"name":..,"path":..}..]}
 *
 *        An app's id is its parsing name in the Apps folder (an AppUserModelID
 *        or a path): "shell:AppsFolder\<id>" launches it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {
  template<class T>
  struct com_ptr {
    T *p = nullptr;

    ~com_ptr() {
      if (p) p->Release();
    }

    T **put() {
      return &p;
    }

    T *operator->() const {
      return p;
    }

    explicit operator bool() const {
      return p != nullptr;
    }
  };

  std::string utf8(const wchar_t *s) {
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
  }

  std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out + "\"";
  }

  std::string base64(const std::vector<unsigned char> &data) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
      const unsigned v = data[i] << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
      out += table[(v >> 18) & 63];
      out += table[(v >> 12) & 63];
      out += i + 1 < data.size() ? table[(v >> 6) & 63] : '=';
      out += i + 2 < data.size() ? table[v & 63] : '=';
    }
    return out;
  }

  /** The item's icon as the Start menu shows it, `size` px, as a PNG */
  std::vector<unsigned char> icon_png(IShellItem *item, IWICImagingFactory *wic, int size) {
    com_ptr<IShellItemImageFactory> factory;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(factory.put())))) return {};
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage(SIZE {size, size}, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &bitmap)) || !bitmap) return {};
    std::vector<unsigned char> png;
    com_ptr<IWICBitmap> source;
    com_ptr<IStream> stream;
    com_ptr<IWICBitmapEncoder> encoder;
    com_ptr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, source.put())) &&
        SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) &&
        SUCCEEDED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(frame.put(), nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(source.p, nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit())) {
      HGLOBAL memory = nullptr;
      if (SUCCEEDED(GetHGlobalFromStream(stream.p, &memory))) {
        const SIZE_T n = GlobalSize(memory);
        if (const auto *data = static_cast<const unsigned char *>(GlobalLock(memory))) {
          png.assign(data, data + n);
          GlobalUnlock(memory);
        }
      }
    }
    DeleteObject(bitmap);
    return png;
  }

  /** Programs on the PATH: name (without .exe) -> full path, first one wins */
  std::vector<std::pair<std::string, std::string>> path_programs() {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::wstring> seen;
    const DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(n, L'\0');
    GetEnvironmentVariableW(L"PATH", path.data(), n);
    size_t start = 0;
    while (start < path.size()) {
      size_t end = path.find(L';', start);
      if (end == std::wstring::npos) end = path.size();
      std::wstring dir = path.substr(start, end - start);
      start = end + 1;
      dir.erase(std::remove(dir.begin(), dir.end(), L'\0'), dir.end());
      if (dir.empty()) continue;
      wchar_t expanded[MAX_PATH * 2];
      if (ExpandEnvironmentStringsW(dir.c_str(), expanded, ARRAYSIZE(expanded)) == 0) continue;
      dir = expanded;
      if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
      WIN32_FIND_DATAW fd;
      HANDLE find = FindFirstFileW((dir + L"*.exe").c_str(), &fd);
      if (find == INVALID_HANDLE_VALUE) continue;
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        name.resize(name.size() - 4);  // ".exe"
        std::wstring key = name;
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
        if (!seen.insert(key).second) continue;
        out.emplace_back(utf8(name.c_str()), utf8((dir + fd.cFileName).c_str()));
      } while (FindNextFileW(find, &fd));
      FindClose(find);
    }
    return out;
  }
}  // namespace

int wmain() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
  com_ptr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));

  std::string out = "{\"apps\":[";
  com_ptr<IShellItem> apps;
  com_ptr<IEnumShellItems> items;
  bool first = true;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(apps.put()))) &&
      SUCCEEDED(apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put())))) {
    for (;;) {
      com_ptr<IShellItem> item;
      if (items->Next(1, item.put(), nullptr) != S_OK) break;
      wchar_t *name = nullptr, *id = nullptr;
      item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
      item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id);
      if (name && id) {
        const auto png = wic ? icon_png(item.p, wic.p, 48) : std::vector<unsigned char> {};
        out += first ? "" : ",";
        first = false;
        out += "{\"name\":" + json_string(utf8(name)) + ",\"id\":" + json_string(utf8(id)) + ",\"icon\":\"" + base64(png) + "\"}";
      }
      CoTaskMemFree(name);
      CoTaskMemFree(id);
    }
  }
  out += "],\"commands\":[";
  first = true;
  for (const auto &[name, path] : path_programs()) {
    out += first ? "" : ",";
    first = false;
    out += "{\"name\":" + json_string(name) + ",\"path\":" + json_string(path) + "}";
  }
  out += "]}";
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
  CoUninitialize();
  return 0;
}
