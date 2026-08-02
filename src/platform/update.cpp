#include "platform/update.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "platform/paths.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace hr::platform::update {
namespace {

// The one host and the one path. Compiled in, not configurable: an updater that
// can be pointed somewhere else is a way to run somebody else's code.
constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/Anonymous-Floof/Hollowreach/releases/latest";
constexpr wchar_t kAgent[] = L"Hollowreach-Updater";

std::mutex gMutex;
State gState;
std::atomic<bool> gBusy {false};

void setState(Stage stage, const std::string& message, float progress = -1.0f) {
  std::lock_guard<std::mutex> lock(gMutex);
  gState.stage = stage;
  gState.message = message;
  if (progress >= 0.0f) gState.progress = progress;
}

void setLatest(const std::string& version) {
  std::lock_guard<std::mutex> lock(gMutex);
  gState.latest = version;
}

// --- version numbers ---------------------------------------------------------

struct Version {
  int major = 0, minor = 0, patch = 0;
};

Version parseVersion(const std::string& s) {
  Version v;
  const char* p = s.c_str();
  if (*p == 'v' || *p == 'V') ++p;
  v.major = std::atoi(p);
  const char* dot = std::strchr(p, '.');
  if (!dot) return v;
  v.minor = std::atoi(dot + 1);
  dot = std::strchr(dot + 1, '.');
  if (dot) v.patch = std::atoi(dot + 1);
  return v;
}

bool newerThan(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  return a.patch > b.patch;
}

// --- the smallest JSON reach-in that does the job ----------------------------
//
// A release document is large and deeply nested and we want two strings out of
// it. A parser for the whole thing would be a hundred times the code and would
// still have to be told which two, so this finds them by name and validates what
// it finds — which is the check that actually matters.

std::string stringField(const std::string& src, const std::string& key,
                        std::size_t from = 0) {
  const std::string needle = "\"" + key + "\"";
  std::size_t i = src.find(needle, from);
  if (i == std::string::npos) return {};
  i = src.find(':', i + needle.size());
  if (i == std::string::npos) return {};
  ++i;
  while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) ++i;
  if (i >= src.size() || src[i] != '"') return {};
  ++i;
  std::string out;
  while (i < src.size() && src[i] != '"') {
    if (src[i] == '\\' && i + 1 < src.size()) ++i;
    out.push_back(src[i++]);
  }
  return out;
}

// The asset we will accept, and only it.
bool assetNameOk(const std::string& name, const std::string& version) {
  return name == "Hollowreach-v" + version + "-Windows.zip";
}

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

// One GET. `toFile` empty means "into memory". Follows redirects only while they
// stay on HTTPS, which is what WinHTTP does by default with the secure-redirect
// policy left alone.
bool httpGet(const std::wstring& host, const std::wstring& path, std::string* intoMemory,
             const std::string& toFile, const std::function<void(float)>& onProgress,
             std::string& error) {
  HINTERNET session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = "no network session";
    return false;
  }
  // Twenty seconds is long enough for a slow link and short enough that a dead
  // one does not leave the button spinning forever.
  WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    error = "could not reach " + narrow(host);
    return false;
  }
  HINTERNET request =
      WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    error = "could not open the request";
    return false;
  }

  const bool ok = WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n",
                                     static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);
  if (!ok) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    error = "the request failed";
    return false;
  }

  DWORD status = 0, statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    error = "the server answered " + std::to_string(status);
    return false;
  }

  long long expected = 0;
  wchar_t lengthText[32] = {};
  DWORD lengthSize = sizeof(lengthText);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                          lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
    expected = _wtoi64(lengthText);
  }

  std::ofstream out;
  if (!toFile.empty()) {
    out.open(toFile, std::ios::binary | std::ios::trunc);
    if (!out) {
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      error = "could not write " + toFile;
      return false;
    }
  }

  std::vector<char> buffer(64 * 1024);
  long long total = 0;
  bool good = true;
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
      good = false;
      error = "the download was interrupted";
      break;
    }
    if (got == 0) break;
    total += got;
    // A release zip is a couple of megabytes. Anything claiming to be a hundred
    // is not our artifact and is not worth filling a disk to find out.
    if (total > 256LL * 1024 * 1024) {
      good = false;
      error = "the download was implausibly large";
      break;
    }
    if (intoMemory) intoMemory->append(buffer.data(), got);
    if (out) out.write(buffer.data(), got);
    if (onProgress && expected > 0) {
      onProgress(static_cast<float>(static_cast<double>(total) / static_cast<double>(expected)));
    }
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return good;
}

// Splits "https://host/path" into the two halves WinHttp wants, and refuses
// anything that is not https.
bool splitHttpsUrl(const std::string& url, std::wstring& host, std::wstring& path,
                   std::string& error) {
  const std::string prefix = "https://";
  if (url.rfind(prefix, 0) != 0) {
    error = "the download link was not https";
    return false;
  }
  const std::size_t slash = url.find('/', prefix.size());
  if (slash == std::string::npos) {
    error = "the download link had no path";
    return false;
  }
  host = widen(url.substr(prefix.size(), slash - prefix.size()));
  path = widen(url.substr(slash));
  return true;
}

std::string stageDir() { return paths::join(paths::dataDir(), "update"); }

// Runs a command and waits. Used for `tar`, which has shipped in Windows since
// 1803 and unpacks a zip without this project taking on a compression library
// for one button.
bool runQuiet(const std::wstring& commandLine, DWORD& exitCode) {
  STARTUPINFOW si {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi {};
  std::wstring mutableLine = commandLine;
  if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
}

#endif  // _WIN32

std::string gAssetUrl;
std::string gAssetName;

}  // namespace

bool supported() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

State state() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gState;
}

void check(const std::string& asIfVersion) {
#if !defined(_WIN32)
  (void)asIfVersion;
  setState(Stage::Unsupported, "Updating is Windows-only for now");
#else
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true)) return;
  setState(Stage::Checking, "Checking for updates\xE2\x80\xA6", 0.0f);

  std::thread([asIfVersion] {
    std::string body;
    std::string error;
    if (!httpGet(kApiHost, kApiPath, &body, {}, {}, error)) {
      setState(Stage::Failed, error);
      gBusy = false;
      return;
    }

    const std::string tag = stringField(body, "tag_name");
    if (tag.empty()) {
      setState(Stage::Failed, "The release listing could not be read");
      gBusy = false;
      return;
    }
    const Version latest = parseVersion(tag);
    const Version mine = parseVersion(asIfVersion.empty() ? HR_VERSION : asIfVersion);
    const std::string latestText = std::to_string(latest.major) + "." +
                                   std::to_string(latest.minor) + "." +
                                   std::to_string(latest.patch);
    setLatest(latestText);

    if (!newerThan(latest, mine)) {
      setState(Stage::UpToDate, "You are on the latest version (" HR_VERSION ")");
      gBusy = false;
      return;
    }

    // Find OUR asset, by exact name. Walking the assets array by hand rather than
    // taking the first browser_download_url in the document, because a release can
    // carry anything and the first one is not a promise.
    gAssetUrl.clear();
    gAssetName.clear();
    std::size_t at = 0;
    for (;;) {
      const std::size_t next = body.find("\"browser_download_url\"", at);
      if (next == std::string::npos) break;
      // The name field of the same asset object sits before its url.
      const std::size_t nameAt = body.rfind("\"name\"", next);
      const std::string name = nameAt == std::string::npos ? "" : stringField(body, "name", nameAt);
      const std::string url = stringField(body, "browser_download_url", next);
      if (assetNameOk(name, latestText)) {
        gAssetName = name;
        gAssetUrl = url;
        break;
      }
      at = next + 1;
    }
    if (gAssetUrl.empty()) {
      setState(Stage::Failed, "Version " + latestText + " has no Windows download yet");
      gBusy = false;
      return;
    }
    setState(Stage::Available, "Version " + latestText + " is available");
    gBusy = false;
  }).detach();
#endif
}

void download() {
#if !defined(_WIN32)
  setState(Stage::Unsupported, "Updating is Windows-only for now");
#else
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gState.stage != Stage::Available) return;
  }
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true)) return;
  setState(Stage::Downloading, "Downloading\xE2\x80\xA6", 0.0f);

  std::thread([] {
    const std::string dir = stageDir();
    paths::ensureDir(dir);
    const std::string zip = paths::join(dir, gAssetName);

    std::wstring host, path;
    std::string error;
    if (!splitHttpsUrl(gAssetUrl, host, path, error)) {
      setState(Stage::Failed, error);
      gBusy = false;
      return;
    }
    if (!httpGet(host, path, nullptr, zip, [](float p) { setState(Stage::Downloading,
                                                                 "Downloading\xE2\x80\xA6", p); },
                 error)) {
      setState(Stage::Failed, error);
      gBusy = false;
      return;
    }

    setState(Stage::Staging, "Unpacking\xE2\x80\xA6", 1.0f);
    const std::string staged = paths::join(dir, "staged");
    paths::removeTree(staged);
    paths::ensureDir(staged);
    DWORD code = 1;
    const std::wstring cmd = L"tar.exe -xf \"" + widen(zip) + L"\" -C \"" + widen(staged) + L"\"";
    if (!runQuiet(cmd, code) || code != 0) {
      setState(Stage::Failed, "The download could not be unpacked");
      gBusy = false;
      return;
    }
    // The zip unpacks to a folder of its own name; the files are inside it.
    setState(Stage::ReadyToApply, "Ready to install \xE2\x80\x94 the game will restart");
    gBusy = false;
  }).detach();
#endif
}

bool apply() {
#if !defined(_WIN32)
  return false;
#else
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gState.stage != Stage::ReadyToApply) return false;
  }

  wchar_t exePathW[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) == 0) return false;
  const std::string exePath = narrow(exePathW);
  const std::size_t slash = exePath.find_last_of("/\\");
  const std::string installDir = slash == std::string::npos ? "." : exePath.substr(0, slash);

  const std::string dir = stageDir();
  const std::string staged = paths::join(dir, "staged");
  const std::string script = paths::join(dir, "apply.cmd");

  // The handover. It has to be a separate process because Windows will not let a
  // running executable be overwritten, and it has to wait because we are still
  // that running executable until a moment from now.
  //
  // robocopy /E copies the tree IN and never removes anything that is not in the
  // source, which is the whole safety property: data/, resource packs and any
  // other files beside the game are not in the zip, so nothing here can touch
  // them. There is deliberately no /PURGE and no delete of the install directory.
  // Every path written into the script has to be back-slashed. paths::join
  // normalises to forward slashes, which most of cmd tolerates and `del` does
  // not — it reads a leading `/` as the start of a switch, so the zip was left
  // behind after an otherwise clean update.
  const auto win = [](std::string p) {
    for (char& c : p) {
      if (c == '/') c = '\\';
    }
    return p;
  };

  const std::string pid = std::to_string(GetCurrentProcessId());
  std::ofstream out(script, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "@echo off\r\n"
      << "rem Written by Hollowreach's updater. Safe to delete.\r\n"
      // Wait for THIS process to exit, by pid. Deliberately not by trying to open
      // the executable: the usual trick for that opens it for append, and an
      // append handle on a running binary is not something to point at the file
      // the whole game lives in. Sixty seconds, then give up and leave everything
      // where it is rather than copying over a program that is still running.
      << "set /a tries=0\r\n"
      << ":wait\r\n"
      << "set /a tries+=1\r\n"
      << "if %tries% gtr 60 goto done\r\n"
      << "tasklist /fi \"PID eq " << pid << "\" /nh 2>nul | find \"" << pid << "\" >nul\r\n"
      << "if not errorlevel 1 (ping -n 2 127.0.0.1 >nul & goto wait)\r\n"
      // Everything the zip contained, over the top. The zip unpacks to a folder of
      // its own name, hence the /d loop. /E copies the tree in and REMOVES NOTHING
      // that is not in the source — there is deliberately no /PURGE and no delete
      // of the install directory, which is what makes data/, resource packs and
      // anything else beside the game safe without a list of exclusions.
      << "for /d %%D in (\"" << win(staged) << "\\*\") do robocopy \"%%~fD\" \""
      << win(installDir) << "\" /E /R:2 /W:1 /NFL /NDL /NJH /NJS >nul\r\n"
      << ":done\r\n"
      << "rmdir /s /q \"" << win(staged) << "\" 2>nul\r\n"
      << "del /q \"" << win(paths::join(dir, gAssetName)) << "\" 2>nul\r\n"
      << "start \"\" \"" << win(exePath) << "\"\r\n"
      // Deleted last, by itself. cmd has already read the line it is running.
      << "del /q \"%~f0\"\r\n";
  out.close();

  STARTUPINFOW si {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi {};
  std::wstring cmd = L"cmd.exe /c \"" + widen(win(script)) + L"\"";
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      widen(dir).c_str(), &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  log::info("update: handing over to %s", script.c_str());
  return true;
#endif
}

void cleanup() {
  if (gBusy) return;
  paths::removeTree(stageDir());
}

}  // namespace hr::platform::update
