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
#include <vector>

namespace fs = std::filesystem;

namespace {

const char *kWorkerStop = "\\workerstop";

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
  int spare_worker_count = 1;
};

std::string installed_warmup_body(const RendererConfig &cfg) {
  fs::path installed_warmup = cfg.runtime_root / "cache-warmup" / "warmup.tex";
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

std::string light_prime_body() {
  return R"(hot spare prime $E=mc^2$ {\color{blue}blue}
)";
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
  std::wostringstream cmd;
  cmd << quote_cmd_arg_w(path_to_wstring(exe))
      << L" -fmt=xelatex --no-font-cache-refresh"
      << L" -interaction=errorstopmode -halt-on-error -no-pdf -flush-output-on-shipout"
      << L" -output-directory=" << quote_cmd_arg_w(path_to_wstring(out_dir))
      << L" webapp/worker-webapp.tex";
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

int clamp_width(int width) {
  if (width <= 0) return 360;
  return std::max(180, std::min(430, width));
}

int normalize_spare_worker_count(int count) {
  if (count <= 0) return 1;
  return std::min(4, count);
}

}  // namespace

struct StemTeXRenderer {
  explicit StemTeXRenderer(RendererConfig c) : cfg(std::move(c)), parts(load_or_run_warmup(cfg)) {
    worker_env = worker_environment(cfg);
    primary = create_ready_worker("primary");
    schedule_spare_rebuild_locked();
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
    raw->child.start(worker_command(cfg, raw->live_out), cfg.repo_root, worker_env, [raw](const std::string &text) {
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
    if (!raw->cv.wait_for(lock, std::chrono::seconds(30), [&]() { return raw->ready; })) {
      raw->child.stop();
      throw std::runtime_error("Live worker did not become ready: " + name);
    }
    lock.unlock();
    if (prime) prime_worker(*raw, false);
    return slot;
  }

  void prime_worker(WorkerSlot &slot, bool full_warmup) {
    fs::path req_path = cfg.state_root / "workers" / slot.name / "warmup-request" / "req1.tex";
    write_text_file(req_path, full_warmup ? installed_warmup_body(cfg) : light_prime_body());
    {
      std::lock_guard<std::mutex> lock(slot.mu);
      slot.done = false;
    }
    slot.child.write_stdin("360pt\n");
    slot.child.write_stdin(slash_path(fs::relative(req_path, cfg.repo_root)) + "\n");
    std::unique_lock<std::mutex> lock(slot.mu);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
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
      throw std::runtime_error("Live worker warmup failed: " + slot.name + ". TeX output tail:\n" + tail);
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
        if (built && (int)spares.size() < cfg.spare_worker_count) {
          spares.push_back(std::move(built));
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

  StemTeXRenderResult render(const std::string &snippet, int width_pt) {
    std::lock_guard<std::mutex> render_lock(render_mu);
    if (!primary || !primary->child.is_running()) {
      if (!promote_if_available_locked()) {
        primary = create_ready_worker("primary");
        schedule_spare_rebuild_locked();
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
    slot.child.write_stdin(std::to_string(clamp_width(width_pt)) + "pt\n");
    slot.child.write_stdin(slash_path(fs::relative(req_path, cfg.repo_root)) + "\n");

    {
      std::unique_lock<std::mutex> lock(slot.mu);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
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
        promote_spare_locked();
        throw std::runtime_error("Worker request timed out");
      }
      if (!slot.done) {
        std::string tail = slot.output_tail;
        lock.unlock();
        promote_spare_locked();
        throw std::runtime_error("Worker exited before WORKER_DONE. TeX output tail:\n" + tail);
      }
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
    std::ostringstream cmd;
    cmd << quote_cmd_arg(xdvipdfmx.string()) << " -q -s " << request_no << "-" << request_no
        << " -o " << quote_cmd_arg(pdf_path.string()) << " " << quote_cmd_arg(final_path.string());
    run_sync(cmd.str(), cfg.repo_root, worker_env);
    int64_t end = now_ms();

    std::ostringstream summary;
    summary << "{"
            << "\"pdfMode\":\"cpp-dll-live-worker-latest-page\","
            << "\"widthPt\":" << clamp_width(width_pt) << ","
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

    StemTeXRenderResult result{};
    result.request_id_utf8 = alloc_c_string(id);
    result.pdf_path_utf8 = alloc_c_string(pdf_path.string());
    result.summary_json_utf8 = alloc_c_string(summary.str());
    return result;
  }

  RendererConfig cfg;
  XdvParts parts;
  std::vector<wchar_t> worker_env;
  std::mutex render_mu;
  std::unique_ptr<WorkerSlot> primary;
  std::vector<std::unique_ptr<WorkerSlot>> spares;
  std::thread spare_builder;
  bool spare_rebuilding = false;
  bool shutting_down = false;
  int next_spare_index = 0;
};

extern "C" {

STEMTEX_API StemTeXRenderer *stemtex_renderer_create(const StemTeXConfig *config, char **error_utf8) {
  try {
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
    cfg.spare_worker_count = config ? normalize_spare_worker_count(config->spare_worker_count) : 1;
    fs::create_directories(cfg.state_root);
    fs::create_directories(cfg.renders_root);
    if (!fs::exists(cfg.runtime_root / "run-xelatexdaemon.bat")) {
      throw std::runtime_error("Runtime missing run-xelatexdaemon.bat: " + cfg.runtime_root.string());
    }
    return new StemTeXRenderer(std::move(cfg));
  } catch (const std::exception &e) {
    if (error_utf8) *error_utf8 = alloc_c_string(e.what());
    return nullptr;
  }
}

STEMTEX_API int stemtex_renderer_render(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                        StemTeXRenderResult *result, char **error_utf8) {
  if (!renderer || !snippet_utf8 || !result) {
    if (error_utf8) *error_utf8 = alloc_c_string("Invalid argument");
    return 0;
  }
  try {
    *result = renderer->render(snippet_utf8, width_pt);
    return 1;
  } catch (const std::exception &e) {
    if (error_utf8) *error_utf8 = alloc_c_string(e.what());
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

STEMTEX_API void stemtex_renderer_free_string(char *value) {
  if (value) CoTaskMemFree(value);
}

STEMTEX_API void stemtex_renderer_destroy(StemTeXRenderer *renderer) {
  delete renderer;
}

}  // extern "C"
