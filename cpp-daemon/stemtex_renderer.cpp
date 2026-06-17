#include "stemtex_renderer.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char *kWorkerStop = "\\workerstop";
const char *kRendererVersion = "0.1.0";
const char *kRendererAbiVersion = "0.2.0";

char *alloc_c_string(const std::string &s);

struct ApiException : std::runtime_error {
  ApiException(StemTeXErrorCode c, const std::string &message) : std::runtime_error(message), code(c) {}
  StemTeXErrorCode code;
};

StemTeXErrorCode exception_code(const std::exception &e) {
  const auto *api = dynamic_cast<const ApiException *>(&e);
  return api ? api->code : STEMTEX_ERROR_INTERNAL;
}

void set_error_outputs(StemTeXErrorCode code, const std::string &message, StemTeXErrorCode *error_code,
                       char **error_utf8) {
  if (error_code) *error_code = code;
  if (error_utf8) *error_utf8 = alloc_c_string(message);
}

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string dup_to_c_string(const std::string &s) {
  return s;
}

char *alloc_c_string(const std::string &s) {
  char *p = static_cast<char *>(CoTaskMemAlloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.data(), s.size());
  p[s.size()] = '\0';
  return p;
}

std::wstring widen_utf8(const std::string &s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) throw std::runtime_error("MultiByteToWideChar failed");
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string narrow_utf8(const std::wstring &s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) throw std::runtime_error("WideCharToMultiByte failed");
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::string quote_cmd_arg(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += "\"";
  return out;
}

std::wstring quote_cmd_arg_w(const std::wstring &s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"') out += L"\\\"";
    else out += c;
  }
  out += L"\"";
  return out;
}

std::wstring path_to_wstring(const fs::path &p) {
  return p.wstring();
}

std::vector<wchar_t> build_environment_block(const std::map<std::wstring, std::wstring> &overrides) {
  std::map<std::wstring, std::wstring> env;
  LPWCH raw = GetEnvironmentStringsW();
  if (!raw) throw std::runtime_error("GetEnvironmentStringsW failed");
  for (LPWCH p = raw; *p; p += wcslen(p) + 1) {
    std::wstring entry = p;
    size_t eq = entry.find(L'=');
    if (eq == std::wstring::npos || eq == 0) continue;
    env[entry.substr(0, eq)] = entry.substr(eq + 1);
  }
  FreeEnvironmentStringsW(raw);
  for (const auto &kv : overrides) env[kv.first] = kv.second;

  std::vector<wchar_t> block;
  for (const auto &kv : env) {
    std::wstring entry = kv.first + L"=" + kv.second;
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

std::string slash_path(const fs::path &p) {
  return p.generic_string();
}

bool has_file_with_prefix_suffix(const fs::path &dir, const std::wstring &prefix, const std::wstring &suffix) {
  if (!fs::exists(dir)) return false;
  for (const auto &entry : fs::directory_iterator(dir)) {
    std::wstring name = entry.path().filename().wstring();
    if (name.size() >= prefix.size() + suffix.size() && name.rfind(prefix, 0) == 0 &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

void write_text_file(const fs::path &p, const std::string &text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot write file: " + p.string());
  f.write(text.data(), (std::streamsize)text.size());
}

std::vector<uint8_t> read_file(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read file: " + p.string());
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string read_text_file(const fs::path &p) {
  auto bytes = read_file(p);
  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

void write_file(const fs::path &p, const std::vector<uint8_t> &bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot write file: " + p.string());
  f.write(reinterpret_cast<const char *>(bytes.data()), (std::streamsize)bytes.size());
}

std::vector<uint8_t> read_file_range(const fs::path &p, uint64_t start, uint64_t end) {
  if (end <= start) throw std::runtime_error("No new XDV bytes available: " + p.string());
  std::vector<uint8_t> out((size_t)(end - start));
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read file: " + p.string());
  f.seekg((std::streamoff)start, std::ios::beg);
  f.read(reinterpret_cast<char *>(out.data()), (std::streamsize)out.size());
  if ((size_t)f.gcount() != out.size()) throw std::runtime_error("Short read: " + p.string());
  return out;
}

size_t find_byte(const std::vector<uint8_t> &b, uint8_t value, size_t start = 0) {
  auto it = std::find(b.begin() + (std::ptrdiff_t)start, b.end(), value);
  if (it == b.end()) return std::string::npos;
  return (size_t)std::distance(b.begin(), it);
}

size_t rfind_byte(const std::vector<uint8_t> &b, uint8_t value) {
  for (size_t i = b.size(); i > 0; --i) {
    if (b[i - 1] == value) return i - 1;
  }
  return std::string::npos;
}

void append_be32(std::vector<uint8_t> &out, int32_t v) {
  out.push_back((uint8_t)((v >> 24) & 0xff));
  out.push_back((uint8_t)((v >> 16) & 0xff));
  out.push_back((uint8_t)((v >> 8) & 0xff));
  out.push_back((uint8_t)(v & 0xff));
}

void append_be16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back((uint8_t)((v >> 8) & 0xff));
  out.push_back((uint8_t)(v & 0xff));
}

struct XdvParts {
  std::vector<uint8_t> fontdefs;
};

XdvParts read_xdv_parts(const fs::path &xdv_path) {
  auto xdv = read_file(xdv_path);
  size_t post = rfind_byte(xdv, 248);
  if (post == std::string::npos) throw std::runtime_error("No XDV postamble: " + xdv_path.string());
  size_t postpost = find_byte(xdv, 249, post);
  if (postpost == std::string::npos) throw std::runtime_error("No XDV post_post: " + xdv_path.string());
  if (post + 29 > postpost) throw std::runtime_error("Malformed XDV postamble: " + xdv_path.string());
  XdvParts parts;
  parts.fontdefs.assign(xdv.begin() + (std::ptrdiff_t)(post + 29), xdv.begin() + (std::ptrdiff_t)postpost);
  return parts;
}

size_t find_first_bop(const std::vector<uint8_t> &b, const std::string &label) {
  size_t pos = find_byte(b, 139);
  if (pos == std::string::npos) throw std::runtime_error("No BOP found in " + label);
  return pos;
}

size_t finalize_xdv_body(const std::vector<uint8_t> &xdv_body, const fs::path &out_path,
                         const XdvParts &parts, uint16_t page_count, int32_t last_bop) {
  if (xdv_body.empty() || xdv_body[0] != 247) throw std::runtime_error("Expected full XDV body");
  std::vector<uint8_t> out = xdv_body;
  int32_t post_offset = (int32_t)out.size();
  out.push_back(248);
  append_be32(out, last_bop);
  append_be32(out, 25400000);
  append_be32(out, 473628672);
  append_be32(out, 1000);
  append_be32(out, 0x1d200000);
  append_be32(out, 0x1d200000);
  append_be16(out, 20);
  append_be16(out, page_count);
  out.insert(out.end(), parts.fontdefs.begin(), parts.fontdefs.end());
  out.push_back(249);
  append_be32(out, post_offset);
  out.push_back(7);
  out.insert(out.end(), {223, 223, 223, 223});
  while (out.size() % 4 != 0) out.push_back(223);
  write_file(out_path, out);
  return out.size();
}

class ChildProcess {
 public:
  using DataCallback = std::function<void(const std::string &)>;

  ChildProcess() = default;
  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;
  ~ChildProcess() { stop(); }

  void start(const std::wstring &command_line, const fs::path &cwd, const std::vector<wchar_t> &environment,
             DataCallback on_data) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) throw std::runtime_error("CreatePipe stdin failed");
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) throw std::runtime_error("CreatePipe stdout failed");
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) throw std::runtime_error("CreatePipe stderr failed");
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    std::wstring cwdw = cwd.wstring();
    LPVOID env = environment.empty() ? nullptr : const_cast<wchar_t *>(environment.data());
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env, cwdw.c_str(), &si, &pi);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!ok) {
      CloseHandle(stdin_write);
      CloseHandle(stdout_read);
      CloseHandle(stderr_read);
      throw std::runtime_error("CreateProcess failed");
    }

    pi_ = pi;
    stdin_ = stdin_write;
    stdout_ = stdout_read;
    stderr_ = stderr_read;
    stdout_thread_ = std::thread([this, on_data]() { read_loop(stdout_, on_data); });
    stderr_thread_ = std::thread([this, on_data]() { read_loop(stderr_, on_data); });
  }

  void write_stdin(const std::string &text) {
    DWORD written = 0;
    if (!WriteFile(stdin_, text.data(), (DWORD)text.size(), &written, nullptr)) {
      throw std::runtime_error("WriteFile stdin failed");
    }
  }

  DWORD wait() {
    if (!pi_.hProcess) return 0;
    WaitForSingleObject(pi_.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi_.hProcess, &code);
    join_readers();
    close_handles();
    return code;
  }

  void stop() {
    if (pi_.hProcess) {
      TerminateProcess(pi_.hProcess, 1);
      WaitForSingleObject(pi_.hProcess, 5000);
    }
    join_readers();
    close_handles();
  }

  bool is_running() const {
    if (!pi_.hProcess) return false;
    return WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT;
  }

 private:
  void read_loop(HANDLE h, DataCallback on_data) {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
      on_data(std::string(buf, buf + n));
    }
  }

  void join_readers() {
    if (stdout_thread_.joinable()) stdout_thread_.join();
    if (stderr_thread_.joinable()) stderr_thread_.join();
  }

  void close_handle(HANDLE &h) {
    if (h) {
      CloseHandle(h);
      h = nullptr;
    }
  }

  void close_handles() {
    close_handle(stdin_);
    close_handle(stdout_);
    close_handle(stderr_);
    close_handle(pi_.hThread);
    close_handle(pi_.hProcess);
  }

  PROCESS_INFORMATION pi_{};
  HANDLE stdin_ = nullptr;
  HANDLE stdout_ = nullptr;
  HANDLE stderr_ = nullptr;
  std::thread stdout_thread_;
  std::thread stderr_thread_;
};

class LineWatcher {
 public:
  void feed(const std::string &text, const std::function<void(const std::string &)> &line_cb) {
    std::lock_guard<std::mutex> lock(mu_);
    buffer_ += text;
    size_t start = 0;
    while (true) {
      size_t pos = buffer_.find('\n', start);
      if (pos == std::string::npos) break;
      std::string line = buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      line_cb(line);
      start = pos + 1;
    }
    buffer_.erase(0, start);
  }

 private:
  std::mutex mu_;
  std::string buffer_;
};

struct RendererConfig {
  fs::path repo_root;
  fs::path runtime_root;
  fs::path state_root;
  fs::path renders_root;
  fs::path warmup_tex;
  fs::path worker_template;
  fs::path preamble_tex;
  int request_timeout_ms = 90000;
  int xdvipdfmx_timeout_ms = 90000;
  int min_width_pt = 180;
  int max_width_pt = 430;
  int default_width_pt = 360;
  int spare_worker_count = 1;
  bool auto_restart = true;
  bool delete_intermediates = false;
};

std::string installed_warmup_body(const RendererConfig &cfg) {
  fs::path installed_warmup = cfg.warmup_tex.empty() ? cfg.runtime_root / "cache-warmup" / "warmup.tex" : cfg.warmup_tex;
  if (!fs::exists(installed_warmup)) {
    return u8"中文 warmup $E=mc^2$ \\textcolor{blue}{blue} \\[\\int_0^1 x^2\\,dx=\\frac13\\] \\ce{H2O} $\\ip{1}{0}$\n";
  }
  std::string warmup = read_text_file(installed_warmup);
  const std::string begin = "\\begin{document}";
  const std::string end = "\\end{document}";
  size_t body_start = warmup.find(begin);
  if (body_start != std::string::npos) {
    body_start += begin.size();
    size_t body_end = warmup.find(end, body_start);
    warmup = warmup.substr(body_start, body_end == std::string::npos ? std::string::npos : body_end - body_start);
  }
  return warmup;
}

fs::path default_runtime(const fs::path &repo_root) {
  const char *env = std::getenv("XETEX_RUNTIME");
  if (env && *env) return fs::absolute(env);
  if (fs::exists("C:\\StemTeX\\run-xelatexdaemon.bat")) return "C:\\StemTeX";
  for (const char *candidate : {"stemtex", "runtime", "../stemtex"}) {
    fs::path p = fs::absolute(repo_root / candidate);
    if (fs::exists(p / "run-xelatexdaemon.bat")) return p;
  }
  return repo_root / "runtime";
}

std::wstring worker_command(const RendererConfig &cfg, const fs::path &out_dir) {
  fs::path exe = cfg.runtime_root / "bin" / "windows" / "xetexdaemon.exe";
  fs::path worker = cfg.worker_template.empty() ? cfg.repo_root / "webapp" / "worker-webapp.tex" : cfg.worker_template;
  std::string worker_arg;
  try {
    worker_arg = slash_path(fs::relative(worker, cfg.repo_root));
  } catch (...) {
    worker_arg = slash_path(worker);
  }
  std::wostringstream cmd;
  cmd << quote_cmd_arg_w(path_to_wstring(exe))
      << L" -fmt=xelatex --no-font-cache-refresh"
      << L" -interaction=errorstopmode -halt-on-error -no-pdf -flush-output-on-shipout"
      << L" -output-directory=" << quote_cmd_arg_w(path_to_wstring(out_dir))
      << L" " << quote_cmd_arg_w(widen_utf8(worker_arg));
  return cmd.str();
}

std::vector<wchar_t> worker_environment(const RendererConfig &cfg) {
  fs::path bin = cfg.runtime_root / "bin" / "windows";
  fs::path texmfcnf = cfg.runtime_root / "texmf-dist" / "web2c";
  fs::path fmt = cfg.runtime_root / "texmf-var" / "web2c" / "xetex";
  fs::path fontconf = cfg.runtime_root / "texmf-var" / "fonts" / "conf";
  fs::path fontcache = cfg.runtime_root / "texmf-var" / "fonts" / "cache";
  std::wstring system_root;
  wchar_t sysroot[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
  if (n > 0 && n < MAX_PATH) system_root = sysroot;
  std::wstring path = path_to_wstring(bin);
  if (!system_root.empty()) path += L";" + system_root + L"\\System32";

  std::map<std::wstring, std::wstring> env = {
      {L"PATH", path},
      {L"TEXMFROOT", path_to_wstring(cfg.runtime_root)},
      {L"TEXMFCNF", path_to_wstring(texmfcnf)},
      {L"TEXFORMATS", path_to_wstring(fmt) + L";" + path_to_wstring(fmt) + L"\\"},
      {L"XE_FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"XE_FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"TEXMF", L""},
      {L"TEXMFDIST", path_to_wstring(cfg.runtime_root / "texmf-dist")},
      {L"TEXMFLOCAL", L""},
      {L"TEXMFSYSVAR", path_to_wstring(cfg.runtime_root / "texmf-var")},
      {L"TEXMFSYSCONFIG", path_to_wstring(cfg.runtime_root / "texmf-config")},
      {L"TEXMFVAR", path_to_wstring(cfg.runtime_root / "texmf-var")},
      {L"TEXMFCONFIG", path_to_wstring(cfg.runtime_root / "texmf-config")},
      {L"TEXMFHOME", L""},
      {L"TEXINPUTS", L""},
      {L"LUAINPUTS", L""},
      {L"BIBINPUTS", L""},
      {L"BSTINPUTS", L""},
      {L"MFINPUTS", L""},
      {L"MPINPUTS", L""},
      {L"TFMFONTS", L""},
      {L"T1FONTS", L""},
      {L"OPENTYPEFONTS", L""},
      {L"TTFONTS", L""},
      {L"TEXFONTMAPS", L""},
      {L"ENCFONTS", L""},
      {L"VFFONTS", L""},
      {L"WEB2C", path_to_wstring(cfg.runtime_root / "texmf-dist" / "web2c")},
      {L"W32TEX", path_to_wstring(cfg.runtime_root)},
  };
  fs::path icu = bin / "icu-data";
  if (has_file_with_prefix_suffix(icu, L"icudt", L"l.dat")) env[L"ICU_DATA"] = path_to_wstring(icu);
  return build_environment_block(env);
}

struct ScopedEnvironment {
  explicit ScopedEnvironment(const std::map<std::wstring, std::wstring> &overrides) {
    for (const auto &kv : overrides) {
      Entry entry;
      entry.name = kv.first;
      DWORD needed = GetEnvironmentVariableW(kv.first.c_str(), nullptr, 0);
      if (needed > 0) {
        entry.had_value = true;
        entry.value.resize(needed - 1);
        GetEnvironmentVariableW(kv.first.c_str(), entry.value.data(), needed);
      }
      entries.push_back(std::move(entry));
      SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());
    }
  }

  ~ScopedEnvironment() {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      SetEnvironmentVariableW(it->name.c_str(), it->had_value ? it->value.c_str() : nullptr);
    }
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

  struct Entry {
    std::wstring name;
    bool had_value = false;
    std::wstring value;
  };
  std::vector<Entry> entries;
};

std::map<std::wstring, std::wstring> runtime_environment_overrides(const RendererConfig &cfg) {
  fs::path bin = cfg.runtime_root / "bin" / "windows";
  fs::path texmfcnf = cfg.runtime_root / "texmf-dist" / "web2c";
  fs::path fmt = cfg.runtime_root / "texmf-var" / "web2c" / "xetex";
  fs::path fontconf = cfg.runtime_root / "texmf-var" / "fonts" / "conf";
  fs::path fontcache = cfg.runtime_root / "texmf-var" / "fonts" / "cache";
  std::wstring system_root;
  wchar_t sysroot[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
  if (n > 0 && n < MAX_PATH) system_root = sysroot;
  std::wstring path = path_to_wstring(bin);
  if (!system_root.empty()) path += L";" + system_root + L"\\System32";

  std::map<std::wstring, std::wstring> env = {
      {L"PATH", path},
      {L"TEXMFROOT", path_to_wstring(cfg.runtime_root)},
      {L"TEXMFCNF", path_to_wstring(texmfcnf)},
      {L"TEXFORMATS", path_to_wstring(fmt) + L";" + path_to_wstring(fmt) + L"\\"},
      {L"XE_FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"XE_FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"TEXMF", L""},
      {L"TEXMFDIST", path_to_wstring(cfg.runtime_root / "texmf-dist")},
      {L"TEXMFLOCAL", L""},
      {L"TEXMFSYSVAR", path_to_wstring(cfg.runtime_root / "texmf-var")},
      {L"TEXMFSYSCONFIG", path_to_wstring(cfg.runtime_root / "texmf-config")},
      {L"TEXMFVAR", path_to_wstring(cfg.runtime_root / "texmf-var")},
      {L"TEXMFCONFIG", path_to_wstring(cfg.runtime_root / "texmf-config")},
      {L"TEXMFHOME", L""},
      {L"TEXINPUTS", L""},
      {L"LUAINPUTS", L""},
      {L"BIBINPUTS", L""},
      {L"BSTINPUTS", L""},
      {L"MFINPUTS", L""},
      {L"MPINPUTS", L""},
      {L"TFMFONTS", L""},
      {L"T1FONTS", L""},
      {L"OPENTYPEFONTS", L""},
      {L"TTFONTS", L""},
      {L"TEXFONTMAPS", L""},
      {L"ENCFONTS", L""},
      {L"VFFONTS", L""},
      {L"WEB2C", path_to_wstring(cfg.runtime_root / "texmf-dist" / "web2c")},
      {L"W32TEX", path_to_wstring(cfg.runtime_root)},
      {L"command_line_encoding", L""},
  };
  fs::path icu = bin / "icu-data";
  if (has_file_with_prefix_suffix(icu, L"icudt", L"l.dat")) env[L"ICU_DATA"] = path_to_wstring(icu);
  return env;
}

std::string run_dvipdfmx_dll(const RendererConfig &cfg, const fs::path &final_path, const fs::path &pdf_path,
                             const std::string &page_range) {
  fs::path dll_path = cfg.runtime_root / "bin" / "windows" / "dvipdfmx.dll";
  if (!fs::exists(dll_path)) throw std::runtime_error("dvipdfmx.dll missing");

  ScopedEnvironment env(runtime_environment_overrides(cfg));
  using MainFn = int(__cdecl *)(int, char **);
  HMODULE dll = LoadLibraryW(dll_path.wstring().c_str());
  if (!dll) throw std::runtime_error("LoadLibrary dvipdfmx.dll failed: " + std::to_string(GetLastError()));
  auto free_dll = std::unique_ptr<std::remove_pointer<HMODULE>::type, decltype(&FreeLibrary)>(dll, FreeLibrary);
  auto fn = reinterpret_cast<MainFn>(GetProcAddress(dll, "dlldvipdfmxmain"));
  if (!fn) throw std::runtime_error("GetProcAddress dlldvipdfmxmain failed: " + std::to_string(GetLastError()));

  std::vector<std::string> args = {
      "xdvipdfmx",
      "-q",
      "-z",
      "1",
      "-C",
      "64",
      "-s",
      page_range,
      "-o",
      slash_path(pdf_path),
      slash_path(final_path),
  };
  std::vector<char *> av;
  for (auto &arg : args) av.push_back(arg.data());
  int code = fn((int)av.size(), av.data());
  if (code != 0) throw std::runtime_error("dvipdfmx.dll returned " + std::to_string(code));
  if (!fs::exists(pdf_path)) throw std::runtime_error("dvipdfmx.dll did not write PDF: " + pdf_path.string());
  return "dll";
}

void run_sync(const std::string &command, const fs::path &cwd, const std::vector<wchar_t> &environment,
              DWORD timeout_ms = 10000) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE stdout_read = nullptr, stdout_write = nullptr;
  HANDLE stderr_read = nullptr, stderr_write = nullptr;
  if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) throw std::runtime_error("CreatePipe stdout failed");
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) throw std::runtime_error("CreatePipe stderr failed");
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_write;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = widen_utf8(command);
  std::wstring cwdw = cwd.wstring();
  LPVOID env = environment.empty() ? nullptr : const_cast<wchar_t *>(environment.data());
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                      env, cwdw.c_str(), &si, &pi)) {
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_read);
    CloseHandle(stderr_write);
    throw std::runtime_error("CreateProcess failed: " + command);
  }
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);

  auto read_pipe = [](HANDLE h) {
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
      out.append(buf, buf + n);
      if (out.size() > 8192) out.erase(0, out.size() - 8192);
    }
    return out;
  };
  std::string stdout_text;
  std::string stderr_text;
  std::thread stdout_thread([&]() { stdout_text = read_pipe(stdout_read); });
  std::thread stderr_thread([&]() { stderr_text = read_pipe(stderr_read); });

  DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    if (stdout_thread.joinable()) stdout_thread.join();
    if (stderr_thread.joinable()) stderr_thread.join();
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw std::runtime_error("Command timed out: " + command);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  if (stdout_thread.joinable()) stdout_thread.join();
  if (stderr_thread.joinable()) stderr_thread.join();
  CloseHandle(stdout_read);
  CloseHandle(stderr_read);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    throw std::runtime_error("Command failed with code " + std::to_string(code) + ": " + command + "\nstdout:\n" +
                             stdout_text + "\nstderr:\n" + stderr_text);
  }
}

XdvParts run_warmup(const RendererConfig &cfg) {
  fs::path out_dir = cfg.state_root / "fontdefs-warmup";
  fs::path req_dir = cfg.state_root / "warmup-request";
  fs::remove_all(out_dir);
  fs::remove_all(req_dir);
  fs::create_directories(out_dir);
  fs::create_directories(req_dir);
  fs::path req_path = req_dir / "req1.tex";
  write_text_file(req_path, installed_warmup_body(cfg));

  std::mutex mu;
  std::condition_variable cv;
  bool ready = false;
  bool done = false;
  std::string log;
  LineWatcher lines;
  ChildProcess child;
  auto env = worker_environment(cfg);
  child.start(worker_command(cfg, out_dir), cfg.repo_root, env, [&](const std::string &text) {
    log += text;
    lines.feed(text, [&](const std::string &line) {
      if (line.find("WORKER_READY") != std::string::npos) {
        std::lock_guard<std::mutex> lk(mu);
        ready = true;
        cv.notify_all();
      }
      if (line.find("WORKER_DONE:") != std::string::npos) {
        std::lock_guard<std::mutex> lk(mu);
        done = true;
        cv.notify_all();
      }
    });
  });

  std::unique_lock<std::mutex> lock(mu);
  if (!cv.wait_for(lock, std::chrono::seconds(30), [&]() { return ready; })) {
    child.stop();
    throw std::runtime_error("Warmup worker did not become ready");
  }
  lock.unlock();
  child.write_stdin("360pt\n");
  child.write_stdin(slash_path(fs::relative(req_path, cfg.repo_root)) + "\n");
  lock.lock();
  if (!cv.wait_for(lock, std::chrono::seconds(30), [&]() { return done; })) {
    child.stop();
    throw std::runtime_error("Warmup worker did not finish");
  }
  lock.unlock();
  child.write_stdin(std::string(kWorkerStop) + "\n");
  DWORD code = child.wait();
  if (code != 0) throw std::runtime_error("Warmup worker exited with code " + std::to_string(code) + "\n" + log);
  return read_xdv_parts(out_dir / "worker-webapp.xdv");
}

XdvParts load_or_run_warmup(const RendererConfig &cfg) {
  for (const fs::path &candidate : {
           cfg.runtime_root / "texmf-var" / "cache-warmup" / "warmup.xdv",
           cfg.runtime_root / "texmf-var" / "cache-warmup" / "worker-webapp.xdv",
       }) {
    if (fs::exists(candidate)) {
      try {
        return read_xdv_parts(candidate);
      } catch (...) {
      }
    }
  }
  return run_warmup(cfg);
}

std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string random_id() {
  auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
  static std::atomic<uint32_t> seq{0};
  std::ostringstream s;
  s << t << "-" << GetCurrentProcessId() << "-" << seq.fetch_add(1);
  return s.str();
}

int normalize_timeout_ms(int value) {
  return value > 0 ? value : 90000;
}

int normalize_width_bound(int value, int fallback) {
  return value > 0 ? value : fallback;
}

int clamp_width(const RendererConfig &cfg, int width) {
  int min_width = normalize_width_bound(cfg.min_width_pt, 180);
  int max_width = normalize_width_bound(cfg.max_width_pt, 430);
  if (max_width < min_width) std::swap(max_width, min_width);
  int default_width = normalize_width_bound(cfg.default_width_pt, 360);
  default_width = std::max(min_width, std::min(max_width, default_width));
  if (width <= 0) return default_width;
  return std::max(min_width, std::min(max_width, width));
}

int normalize_spare_worker_count(int count) {
  if (count <= 0) return 1;
  return std::min(4, count);
}

RendererConfig config_from_api(const StemTeXConfig *config) {
  RendererConfig cfg;
  cfg.repo_root = config && config->repo_root_utf8 && *config->repo_root_utf8
                      ? fs::absolute(config->repo_root_utf8)
                      : fs::current_path();
  cfg.runtime_root = config && config->runtime_root_utf8 && *config->runtime_root_utf8
                         ? fs::absolute(config->runtime_root_utf8)
                         : default_runtime(cfg.repo_root);
  cfg.state_root = config && config->state_root_utf8 && *config->state_root_utf8
                       ? fs::absolute(config->state_root_utf8)
                       : cfg.repo_root / "out" / "cpp-renderer-state";
  cfg.renders_root = config && config->renders_root_utf8 && *config->renders_root_utf8
                         ? fs::absolute(config->renders_root_utf8)
                         : cfg.repo_root / "out" / "cpp-renderer-renders";
  cfg.warmup_tex = config && config->warmup_tex_utf8 && *config->warmup_tex_utf8
                       ? fs::absolute(config->warmup_tex_utf8)
                       : cfg.runtime_root / "cache-warmup" / "warmup.tex";
  cfg.worker_template = config && config->worker_template_utf8 && *config->worker_template_utf8
                            ? fs::absolute(config->worker_template_utf8)
                            : cfg.repo_root / "webapp" / "worker-webapp.tex";
  cfg.preamble_tex = config && config->preamble_tex_utf8 && *config->preamble_tex_utf8
                         ? fs::absolute(config->preamble_tex_utf8)
                         : cfg.repo_root / "test" / "preamble.tex";
  if (config) {
    cfg.request_timeout_ms = normalize_timeout_ms(config->request_timeout_ms);
    cfg.xdvipdfmx_timeout_ms = normalize_timeout_ms(config->xdvipdfmx_timeout_ms);
    cfg.min_width_pt = normalize_width_bound(config->min_width_pt, 180);
    cfg.max_width_pt = normalize_width_bound(config->max_width_pt, 430);
    cfg.default_width_pt = normalize_width_bound(config->default_width_pt, 360);
    cfg.spare_worker_count = normalize_spare_worker_count(config->spare_worker_count);
    cfg.auto_restart = config->auto_restart == 0 ? true : config->auto_restart != 0;
    cfg.delete_intermediates = config->delete_intermediates != 0;
  }
  return cfg;
}

std::string validate_config_text(const RendererConfig &cfg) {
  std::ostringstream out;
  auto require_file = [&](const fs::path &p, const char *label) {
    if (!fs::exists(p)) out << label << " missing: " << p.string() << "\n";
  };
  auto require_dir = [&](const fs::path &p, const char *label) {
    if (!fs::exists(p) || !fs::is_directory(p)) out << label << " missing: " << p.string() << "\n";
  };
  require_dir(cfg.runtime_root, "runtime root");
  require_file(cfg.runtime_root / "run-xelatexdaemon.bat", "runtime launcher");
  require_file(cfg.runtime_root / "bin" / "windows" / "xetexdaemon.exe", "xetexdaemon.exe");
  require_file(cfg.runtime_root / "bin" / "windows" / "xdvipdfmx.exe", "xdvipdfmx.exe");
  require_file(cfg.runtime_root / "texmf-var" / "web2c" / "xetex" / "xelatex.fmt", "xelatex.fmt");
  require_file(cfg.warmup_tex, "warmup tex");
  require_file(cfg.worker_template, "worker template");
  require_file(cfg.preamble_tex, "preamble tex");
  require_dir(cfg.runtime_root / "texmf-var" / "fonts" / "conf", "fontconfig conf");
  require_dir(cfg.runtime_root / "texmf-var" / "fonts" / "cache", "fontconfig cache");
  fs::path icu = cfg.runtime_root / "bin" / "windows" / "icu-data";
  if (!has_file_with_prefix_suffix(icu, L"icudt", L"l.dat")) out << "ICU data missing: " << icu.string() << "\n";
  return out.str();
}

void validate_or_throw(const RendererConfig &cfg) {
  std::string diagnostics = validate_config_text(cfg);
  if (!diagnostics.empty()) throw ApiException(STEMTEX_ERROR_BAD_CONFIG, diagnostics);
}

}  // namespace

struct StemTeXRenderer {
  explicit StemTeXRenderer(RendererConfig c) : cfg(std::move(c)), parts(load_or_run_warmup(cfg)) {
    status.store(STEMTEX_STATUS_STARTING);
    worker_env = worker_environment(cfg);
    primary = create_ready_worker("primary");
    schedule_spare_rebuild_locked();
    status.store(STEMTEX_STATUS_READY);
  }

  struct WorkerSlot {
    std::string name;
    fs::path live_out;
    ChildProcess child;
    LineWatcher lines;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    bool done = false;
    std::string output_tail;
    uint64_t last_xdv_offset = 0;
    int next_request = 0;
  };

  void append_log(const std::string &text) {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    log_tail += text;
    if (log_tail.size() > 32768) log_tail.erase(0, log_tail.size() - 32768);
  }

  void set_last_error(StemTeXErrorCode code, const std::string &message) {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    last_error = code;
    if (!message.empty()) {
      log_tail += message;
      log_tail += "\n";
      if (log_tail.size() > 32768) log_tail.erase(0, log_tail.size() - 32768);
    }
  }

  std::string get_log_tail(int max_bytes) {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    if (max_bytes <= 0 || (size_t)max_bytes >= log_tail.size()) return log_tail;
    return log_tail.substr(log_tail.size() - (size_t)max_bytes);
  }

  std::unique_ptr<WorkerSlot> create_ready_worker(const std::string &name, bool prime = true) {
    auto slot = std::make_unique<WorkerSlot>();
    slot->name = name;
    slot->live_out = cfg.state_root / "workers" / name / "live";
    fs::remove_all(cfg.state_root / "workers" / name);
    fs::create_directories(slot->live_out);
    slot->ready = false;
    slot->done = false;
    slot->output_tail.clear();
    slot->last_xdv_offset = 0;
    slot->next_request = 0;
    WorkerSlot *raw = slot.get();
    raw->child.start(worker_command(cfg, raw->live_out), cfg.repo_root, worker_env, [this, raw](const std::string &text) {
      append_log(text);
      {
        std::lock_guard<std::mutex> lock(raw->mu);
        raw->output_tail += text;
        if (raw->output_tail.size() > 8192) {
          raw->output_tail.erase(0, raw->output_tail.size() - 8192);
        }
      }
      raw->lines.feed(text, [raw](const std::string &line) {
        std::lock_guard<std::mutex> lock(raw->mu);
        if (line.find("WORKER_READY") != std::string::npos) {
          raw->ready = true;
          raw->cv.notify_all();
        }
        if (line.find("WORKER_DONE:") != std::string::npos) {
          raw->done = true;
          raw->cv.notify_all();
        }
      });
    });
    std::unique_lock<std::mutex> lock(raw->mu);
    if (!raw->cv.wait_for(lock, std::chrono::milliseconds(cfg.request_timeout_ms), [&]() { return raw->ready; })) {
      raw->child.stop();
      throw ApiException(STEMTEX_ERROR_WORKER_STARTUP, "Live worker did not become ready: " + name);
    }
    lock.unlock();
    if (prime) prime_worker(*raw);
    return slot;
  }

  void prime_worker(WorkerSlot &slot) {
    fs::path req_path = cfg.state_root / "workers" / slot.name / "warmup-request" / "req1.tex";
    write_text_file(req_path, installed_warmup_body(cfg));
    {
      std::lock_guard<std::mutex> lock(slot.mu);
      slot.done = false;
    }
    slot.child.write_stdin("360pt\n");
    slot.child.write_stdin(slash_path(fs::relative(req_path, cfg.repo_root)) + "\n");
    std::unique_lock<std::mutex> lock(slot.mu);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.request_timeout_ms);
    bool completed = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (slot.done || !slot.child.is_running()) {
        completed = true;
        break;
      }
      slot.cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    if (!completed || !slot.done) {
      std::string tail = slot.output_tail;
      lock.unlock();
      slot.child.stop();
      throw ApiException(STEMTEX_ERROR_WORKER_STARTUP, "Live worker warmup failed: " + slot.name + ". TeX output tail:\n" + tail);
    }
    lock.unlock();
    fs::path xdv_path = slot.live_out / "worker-webapp.xdv";
    slot.last_xdv_offset = fs::file_size(xdv_path);
    slot.next_request = 1;
  }

  ~StemTeXRenderer() {
    shutting_down = true;
    join_spare_builder();
    if (primary) primary->child.stop();
    for (auto &slot : spares) {
      if (slot) slot->child.stop();
    }
  }

  void join_spare_builder() {
    if (spare_builder.joinable()) spare_builder.join();
  }

  void schedule_spare_rebuild_locked() {
    if (shutting_down || spare_rebuilding || (int)spares.size() >= cfg.spare_worker_count) return;
    if (spare_builder.joinable()) spare_builder.join();
    spare_rebuilding = true;
    spare_builder = std::thread([this]() {
      while (true) {
        int slot_index = 0;
        {
          std::lock_guard<std::mutex> lock(render_mu);
          if (shutting_down || (int)spares.size() >= cfg.spare_worker_count) {
            spare_rebuilding = false;
            return;
          }
          slot_index = next_spare_index++;
        }

        std::unique_ptr<WorkerSlot> built;
        try {
          built = create_ready_worker("spare-" + std::to_string(slot_index));
        } catch (...) {
        }

        std::lock_guard<std::mutex> lock(render_mu);
        if (shutting_down) {
          spare_rebuilding = false;
          return;
        }
        if (built) {
          if (!primary || !primary->child.is_running()) {
            if (primary) primary->child.stop();
            primary = std::move(built);
            primary->name = "primary";
          } else if ((int)spares.size() < cfg.spare_worker_count) {
            spares.push_back(std::move(built));
          }
        }
      }
    });
  }

  void promote_spare_locked() {
    if (primary) primary->child.stop();
    while (!spares.empty()) {
      auto candidate = std::move(spares.back());
      spares.pop_back();
      if (candidate && candidate->child.is_running()) {
        primary = std::move(candidate);
        primary->name = "primary";
        schedule_spare_rebuild_locked();
        return;
      }
    }
    primary.reset();
    schedule_spare_rebuild_locked();
  }

  bool promote_if_available_locked() {
    while (!spares.empty()) {
      auto candidate = std::move(spares.back());
      spares.pop_back();
      if (candidate && candidate->child.is_running()) {
        primary = std::move(candidate);
        primary->name = "primary";
        schedule_spare_rebuild_locked();
        return true;
      }
    }
    return false;
  }

  int spare_ready_count_locked() const {
    int count = 0;
    for (const auto &slot : spares) {
      if (slot && slot->child.is_running()) ++count;
    }
    return count;
  }

  void restart() {
    status.store(STEMTEX_STATUS_RESTARTING);
    {
      std::lock_guard<std::mutex> lock(control_mu);
      active_slot = nullptr;
      cancel_requested = false;
    }
    {
      std::lock_guard<std::mutex> lock(render_mu);
      shutting_down = true;
    }
    join_spare_builder();
    std::lock_guard<std::mutex> render_lock(render_mu);
    if (primary) primary->child.stop();
    for (auto &slot : spares) {
      if (slot) slot->child.stop();
    }
    primary.reset();
    spares.clear();
    spare_rebuilding = false;
    shutting_down = false;
    parts = load_or_run_warmup(cfg);
    worker_env = worker_environment(cfg);
    primary = create_ready_worker("primary");
    schedule_spare_rebuild_locked();
    status.store(STEMTEX_STATUS_READY);
    set_last_error(STEMTEX_OK, "");
  }

  bool cancel_current() {
    std::lock_guard<std::mutex> lock(control_mu);
    if (!active_slot || !active_slot->child.is_running()) return false;
    cancel_requested = true;
    status.store(STEMTEX_STATUS_RESTARTING);
    active_slot->child.stop();
    return true;
  }

  StemTeXRenderResult render(const std::string &snippet, int width_pt) {
    std::lock_guard<std::mutex> render_lock(render_mu);
    status.store(STEMTEX_STATUS_RENDERING);
    if (!primary || !primary->child.is_running()) {
      if (!promote_if_available_locked()) {
        schedule_spare_rebuild_locked();
        status.store(STEMTEX_STATUS_RESTARTING);
        throw ApiException(STEMTEX_ERROR_WORKER_RESTARTING, "No ready XeTeX worker; engine is rebuilding");
      }
    }
    WorkerSlot &slot = *primary;
    int64_t start = now_ms();
    std::string id = random_id();
    fs::path render_dir = cfg.renders_root / id;
    fs::path req_path = render_dir / "requests" / "req1.tex";
    write_text_file(req_path, snippet);

    {
      std::lock_guard<std::mutex> lock(slot.mu);
      slot.done = false;
    }
    {
      std::lock_guard<std::mutex> lock(control_mu);
      active_slot = &slot;
      cancel_requested = false;
    }
    slot.child.write_stdin(std::to_string(clamp_width(cfg, width_pt)) + "pt\n");
    slot.child.write_stdin(slash_path(fs::relative(req_path, cfg.repo_root)) + "\n");

    {
      std::unique_lock<std::mutex> lock(slot.mu);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.request_timeout_ms);
      bool completed = false;
      while (std::chrono::steady_clock::now() < deadline) {
        if (slot.done || !slot.child.is_running()) {
          completed = true;
          break;
        }
        slot.cv.wait_for(lock, std::chrono::milliseconds(25));
      }
      if (!completed) {
        lock.unlock();
        {
          std::lock_guard<std::mutex> control_lock(control_mu);
          active_slot = nullptr;
        }
        promote_spare_locked();
        status.store(STEMTEX_STATUS_READY);
        throw ApiException(STEMTEX_ERROR_WORKER_TIMEOUT, "Worker request timed out");
      }
      if (!slot.done) {
        std::string tail = slot.output_tail;
        bool was_cancelled = false;
        {
          std::lock_guard<std::mutex> control_lock(control_mu);
          was_cancelled = cancel_requested;
          active_slot = nullptr;
          cancel_requested = false;
        }
        lock.unlock();
        promote_spare_locked();
        status.store(STEMTEX_STATUS_READY);
        if (was_cancelled) throw ApiException(STEMTEX_ERROR_CANCELLED, "Render cancelled");
        throw ApiException(STEMTEX_ERROR_TEX_SNIPPET, "Worker exited before WORKER_DONE. TeX output tail:\n" + tail);
      }
    }
    {
      std::lock_guard<std::mutex> control_lock(control_mu);
      active_slot = nullptr;
      cancel_requested = false;
    }

    fs::path xdv_path = slot.live_out / "worker-webapp.xdv";
    uint64_t current_size = fs::file_size(xdv_path);
    auto cumulative = read_file_range(xdv_path, 0, current_size);
    auto delta = read_file_range(xdv_path, slot.last_xdv_offset, current_size);
    int32_t last_bop = (int32_t)(slot.last_xdv_offset + find_first_bop(delta, xdv_path.string() + " delta"));
    slot.last_xdv_offset = current_size;

    int request_no = ++slot.next_request;
    fs::path out_dir = render_dir / "out" / "live";
    fs::create_directories(out_dir);
    fs::path cumulative_path = out_dir / "snippet-1-cumulative.xdv";
    fs::path final_path = out_dir / "snippet-1-final.xdv";
    fs::path pdf_path = out_dir / "snippet-1.pdf";
    write_file(cumulative_path, cumulative);

    int64_t finalize_start = now_ms();
    size_t final_bytes = finalize_xdv_body(cumulative, final_path, parts, (uint16_t)request_no, last_bop);
    int64_t convert_start = now_ms();
    fs::path xdvipdfmx = cfg.runtime_root / "bin" / "windows" / "xdvipdfmx.exe";
    std::string xdvipdfmx_options = "-q -z 1 -C 64";
    std::string page_range = std::to_string(request_no) + "-" + std::to_string(request_no);
    std::string xdvipdfmx_mode = "dll";
    try {
      try {
        xdvipdfmx_mode = run_dvipdfmx_dll(cfg, final_path, pdf_path, page_range);
      } catch (const std::exception &) {
        xdvipdfmx_mode = "exe-fallback";
        std::ostringstream cmd;
        cmd << quote_cmd_arg(xdvipdfmx.string()) << " " << xdvipdfmx_options << " -s " << page_range
            << " -o " << quote_cmd_arg(pdf_path.string()) << " " << quote_cmd_arg(final_path.string());
        run_sync(cmd.str(), cfg.repo_root, worker_env, (DWORD)cfg.xdvipdfmx_timeout_ms);
      }
    } catch (const std::exception &e) {
      status.store(STEMTEX_STATUS_READY);
      throw ApiException(STEMTEX_ERROR_XDVIPDFMX, e.what());
    }
    int64_t end = now_ms();

    std::ostringstream summary;
    summary << "{"
            << "\"pdfMode\":\"cpp-dll-live-worker-latest-page\","
            << "\"xdvipdfmxOptions\":\"" << json_escape(xdvipdfmx_options) << "\","
            << "\"xdvipdfmxMode\":\"" << json_escape(xdvipdfmx_mode) << "\","
            << "\"widthPt\":" << clamp_width(cfg, width_pt) << ","
            << "\"requestToPdfMs\":" << (end - start) << ","
            << "\"finalizeXdvMs\":" << (convert_start - finalize_start) << ","
            << "\"xdvipdfmxMs\":" << (end - convert_start) << ","
            << "\"cumulativeXdvBytes\":" << cumulative.size() << ","
            << "\"newXdvBytes\":" << delta.size() << ","
            << "\"finalXdvBytes\":" << final_bytes << ","
            << "\"pdfBytes\":" << fs::file_size(pdf_path) << ","
            << "\"workerRequest\":" << request_no << ","
            << "\"workerSlot\":\"" << json_escape(slot.name) << "\","
            << "\"spareReady\":" << spare_ready_count_locked() << ","
            << "\"spareTarget\":" << cfg.spare_worker_count << ","
            << "\"spareRebuilding\":" << (spare_rebuilding ? "true" : "false")
            << "}";
    write_text_file(render_dir / "out" / "summary.json", summary.str() + "\n");
    if (cfg.delete_intermediates) {
      std::error_code ec;
      fs::remove(cumulative_path, ec);
      fs::remove(final_path, ec);
      fs::remove_all(render_dir / "requests", ec);
    }

    StemTeXRenderResult result{};
    result.request_id_utf8 = alloc_c_string(id);
    result.pdf_path_utf8 = alloc_c_string(pdf_path.string());
    result.summary_json_utf8 = alloc_c_string(summary.str());
    status.store(STEMTEX_STATUS_READY);
    set_last_error(STEMTEX_OK, "");
    return result;
  }

  RendererConfig cfg;
  XdvParts parts;
  std::vector<wchar_t> worker_env;
  std::mutex render_mu;
  std::mutex control_mu;
  std::mutex diagnostic_mu;
  std::atomic<StemTeXRendererStatus> status{STEMTEX_STATUS_STARTING};
  StemTeXErrorCode last_error = STEMTEX_OK;
  std::string log_tail;
  WorkerSlot *active_slot = nullptr;
  bool cancel_requested = false;
  std::unique_ptr<WorkerSlot> primary;
  std::vector<std::unique_ptr<WorkerSlot>> spares;
  std::thread spare_builder;
  bool spare_rebuilding = false;
  bool shutting_down = false;
  int next_spare_index = 0;
};

extern "C" {

STEMTEX_API StemTeXRenderer *stemtex_renderer_create(const StemTeXConfig *config, StemTeXErrorCode *error_code,
                                                     char **error_utf8) {
  try {
    RendererConfig cfg = config_from_api(config);
    validate_or_throw(cfg);
    fs::create_directories(cfg.state_root);
    fs::create_directories(cfg.renders_root);
    if (error_code) *error_code = STEMTEX_OK;
    return new StemTeXRenderer(std::move(cfg));
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return nullptr;
  }
}

STEMTEX_API int stemtex_renderer_render(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                        StemTeXRenderResult *result, StemTeXErrorCode *error_code,
                                        char **error_utf8) {
  if (!renderer || !snippet_utf8 || !result) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    *result = renderer->render(snippet_utf8, width_pt);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    if (renderer->status.load() != STEMTEX_STATUS_DEAD) renderer->status.store(STEMTEX_STATUS_READY);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_pdf_bytes(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                                  StemTeXPdfBytes *pdf, StemTeXRenderResult *result,
                                                  StemTeXErrorCode *error_code, char **error_utf8) {
  if (!pdf) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  pdf->data = nullptr;
  pdf->size = 0;
  StemTeXRenderResult local_result{};
  StemTeXRenderResult *target = result ? result : &local_result;
  if (!stemtex_renderer_render(renderer, snippet_utf8, width_pt, target, error_code, error_utf8)) return 0;
  try {
    auto bytes = read_file(target->pdf_path_utf8);
    pdf->data = static_cast<unsigned char *>(CoTaskMemAlloc(bytes.size()));
    if (!pdf->data && !bytes.empty()) throw ApiException(STEMTEX_ERROR_INTERNAL, "Cannot allocate PDF bytes");
    if (!bytes.empty()) std::memcpy(pdf->data, bytes.data(), bytes.size());
    pdf->size = bytes.size();
    if (!result) stemtex_renderer_free_result(&local_result);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    if (!result) stemtex_renderer_free_result(&local_result);
    StemTeXErrorCode code = exception_code(e);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_async(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                              StemTeXRenderCallback callback, void *user_data,
                                              StemTeXErrorCode *error_code, char **error_utf8) {
  if (!renderer || !snippet_utf8 || !callback) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    std::string snippet = snippet_utf8;
    std::thread([renderer, snippet, width_pt, callback, user_data]() {
      StemTeXRenderResult result{};
      StemTeXErrorCode code = STEMTEX_OK;
      char *error = nullptr;
      int ok = stemtex_renderer_render(renderer, snippet.c_str(), width_pt, &result, &code, &error);
      callback(ok, ok ? &result : nullptr, code, error, user_data);
      stemtex_renderer_free_result(&result);
      stemtex_renderer_free_string(error);
    }).detach();
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_restart(StemTeXRenderer *renderer, StemTeXErrorCode *error_code, char **error_utf8) {
  if (!renderer) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    renderer->restart();
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    renderer->status.store(STEMTEX_STATUS_DEAD);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_cancel_current(StemTeXRenderer *renderer, StemTeXErrorCode *error_code,
                                                char **error_utf8) {
  if (!renderer) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  renderer->cancel_current();
  if (error_code) *error_code = STEMTEX_OK;
  return 1;
}

STEMTEX_API StemTeXRendererStatus stemtex_renderer_status(StemTeXRenderer *renderer) {
  if (!renderer) return STEMTEX_STATUS_DEAD;
  return renderer->status.load();
}

STEMTEX_API int stemtex_renderer_engine_snapshot(StemTeXRenderer *renderer, StemTeXEngineSnapshot *snapshot) {
  if (!renderer || !snapshot) return 0;
  snapshot->status = renderer->status.load();
  snapshot->last_error = STEMTEX_OK;
  {
    std::lock_guard<std::mutex> lock(renderer->diagnostic_mu);
    snapshot->last_error = renderer->last_error;
  }
  {
    std::lock_guard<std::mutex> lock(renderer->render_mu);
    snapshot->primary_ready = renderer->primary && renderer->primary->child.is_running() ? 1 : 0;
    snapshot->spare_ready = renderer->spare_ready_count_locked();
    snapshot->spare_target = renderer->cfg.spare_worker_count;
    snapshot->spare_rebuilding = renderer->spare_rebuilding ? 1 : 0;
  }
  return 1;
}

STEMTEX_API StemTeXErrorCode stemtex_renderer_last_error_code(StemTeXRenderer *renderer) {
  if (!renderer) return STEMTEX_ERROR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(renderer->diagnostic_mu);
  return renderer->last_error;
}

STEMTEX_API char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes) {
  if (!renderer) return alloc_c_string("");
  return alloc_c_string(renderer->get_log_tail(max_bytes));
}

STEMTEX_API const char *stemtex_renderer_version(void) {
  return kRendererVersion;
}

STEMTEX_API const char *stemtex_renderer_abi_version(void) {
  return kRendererAbiVersion;
}

STEMTEX_API char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer) {
  if (!renderer) return alloc_c_string("");
  fs::path version = renderer->cfg.runtime_root / "VERSION";
  if (fs::exists(version)) return alloc_c_string(read_text_file(version));
  return alloc_c_string(renderer->cfg.runtime_root.string());
}

STEMTEX_API int stemtex_renderer_validate_config(const StemTeXConfig *config, StemTeXErrorCode *error_code,
                                                 char **diagnostics_utf8) {
  try {
    RendererConfig cfg = config_from_api(config);
    std::string diagnostics = validate_config_text(cfg);
    if (!diagnostics.empty()) {
      set_error_outputs(STEMTEX_ERROR_BAD_CONFIG, diagnostics, error_code, diagnostics_utf8);
      return 0;
    }
    set_error_outputs(STEMTEX_OK, "OK", error_code, diagnostics_utf8);
    return 1;
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, diagnostics_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_refresh_font_cache(const char *runtime_root_utf8, const char *warmup_tex_utf8,
                                           StemTeXErrorCode *error_code, char **error_utf8) {
  if (!runtime_root_utf8 || !*runtime_root_utf8) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "runtime_root_utf8 is required", error_code, error_utf8);
    return 0;
  }
  try {
    RendererConfig cfg;
    cfg.runtime_root = fs::absolute(runtime_root_utf8);
    cfg.repo_root = warmup_tex_utf8 && *warmup_tex_utf8 ? fs::absolute(fs::path(warmup_tex_utf8)).parent_path()
                                                        : cfg.runtime_root;
    cfg.state_root = cfg.runtime_root / "texmf-var" / "cache-warmup-state";
    cfg.renders_root = cfg.runtime_root / "texmf-var" / "cache-warmup-renders";
    cfg.warmup_tex = warmup_tex_utf8 && *warmup_tex_utf8 ? fs::absolute(warmup_tex_utf8)
                                                         : cfg.runtime_root / "cache-warmup" / "warmup.tex";
    cfg.worker_template = cfg.repo_root / "webapp" / "worker-webapp.tex";
    cfg.preamble_tex = cfg.repo_root / "test" / "preamble.tex";
    cfg.request_timeout_ms = 90000;
    cfg.xdvipdfmx_timeout_ms = 90000;
    if (!fs::exists(cfg.warmup_tex)) throw ApiException(STEMTEX_ERROR_BAD_CONFIG, "Warmup tex missing: " + cfg.warmup_tex.string());
    fs::path output_dir = cfg.runtime_root / "texmf-var" / "cache-warmup";
    fs::create_directories(output_dir);
    auto env = worker_environment(cfg);
    fs::path exe = cfg.runtime_root / "bin" / "windows" / "xetexdaemon.exe";
    std::ostringstream cmd;
    cmd << quote_cmd_arg(exe.string()) << " -fmt=xelatex -no-pdf -interaction=nonstopmode -halt-on-error"
        << " -output-directory=" << quote_cmd_arg(output_dir.string()) << " " << quote_cmd_arg(cfg.warmup_tex.string());
    run_sync(cmd.str(), cfg.warmup_tex.parent_path(), env, 90000);
    fs::path warmup_xdv = output_dir / (cfg.warmup_tex.stem().string() + ".xdv");
    if (!fs::exists(warmup_xdv)) throw ApiException(STEMTEX_ERROR_FILESYSTEM, "Warmup XDV was not written: " + warmup_xdv.string());
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API void stemtex_renderer_free_result(StemTeXRenderResult *result) {
  if (!result) return;
  stemtex_renderer_free_string(result->request_id_utf8);
  stemtex_renderer_free_string(result->pdf_path_utf8);
  stemtex_renderer_free_string(result->summary_json_utf8);
  result->request_id_utf8 = nullptr;
  result->pdf_path_utf8 = nullptr;
  result->summary_json_utf8 = nullptr;
}

STEMTEX_API void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf) {
  if (!pdf) return;
  if (pdf->data) CoTaskMemFree(pdf->data);
  pdf->data = nullptr;
  pdf->size = 0;
}

STEMTEX_API void stemtex_renderer_free_string(char *value) {
  if (value) CoTaskMemFree(value);
}

STEMTEX_API void stemtex_renderer_destroy(StemTeXRenderer *renderer) {
  delete renderer;
}

}  // extern "C"
