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
#include <limits>
#include <locale>
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
const wchar_t *kWorkerHostLifetimeEnv = L"STEMTEX_WORKER_HOST_LIFETIME_HANDLE";
const char *kUnknownInternalException = "Unknown internal exception";
#ifndef STEMTEX_RENDERER_VERSION
#define STEMTEX_RENDERER_VERSION "0.0.0-dev"
#endif
const char *kRendererVersion = STEMTEX_RENDERER_VERSION;
const char *kRendererAbiVersion = STEMTEX_RENDERER_VERSION;

char *alloc_c_string(const std::string &s) noexcept;

struct ApiException : std::runtime_error {
  ApiException(StemTeXErrorCode c, const std::string &message) : std::runtime_error(message), code(c) {}
  StemTeXErrorCode code;
};

StemTeXErrorCode exception_code(const std::exception &e) {
  const auto *api = dynamic_cast<const ApiException *>(&e);
  return api ? api->code : STEMTEX_ERROR_INTERNAL;
}

StemTeXRenderOutcomeCode outcome_from_error(StemTeXErrorCode code) {
  switch (code) {
    case STEMTEX_OK: return STEMTEX_RENDER_OUTCOME_OK;
    case STEMTEX_ERROR_INVALID_ARGUMENT: return STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT;
    case STEMTEX_ERROR_BAD_CONFIG: return STEMTEX_RENDER_OUTCOME_BAD_CONFIG;
    case STEMTEX_ERROR_WORKER_STARTUP: return STEMTEX_RENDER_OUTCOME_WORKER_STARTUP;
    case STEMTEX_ERROR_WORKER_TIMEOUT: return STEMTEX_RENDER_OUTCOME_WORKER_TIMEOUT;
    case STEMTEX_ERROR_WORKER_RESTARTING: return STEMTEX_RENDER_OUTCOME_WORKER_RESTARTING;
    case STEMTEX_ERROR_WORKER_BUSY: return STEMTEX_RENDER_OUTCOME_WORKER_BUSY;
    case STEMTEX_ERROR_TEX_SNIPPET: return STEMTEX_RENDER_OUTCOME_TEX_SNIPPET;
    case STEMTEX_ERROR_XDVIPDFMX: return STEMTEX_RENDER_OUTCOME_XDVIPDFMX;
    case STEMTEX_ERROR_CANCELLED: return STEMTEX_RENDER_OUTCOME_CANCELLED;
    case STEMTEX_ERROR_FILESYSTEM: return STEMTEX_RENDER_OUTCOME_FILESYSTEM;
    case STEMTEX_ERROR_DVISVGM: return STEMTEX_RENDER_OUTCOME_DVISVGM;
    case STEMTEX_ERROR_INTERNAL:
    default: return STEMTEX_RENDER_OUTCOME_INTERNAL;
  }
}

void set_error_outputs(StemTeXErrorCode code, const std::string &message, StemTeXErrorCode *error_code,
                       char **error_utf8) noexcept {
  if (error_code) *error_code = code;
  if (error_utf8) *error_utf8 = alloc_c_string(message);
}

void set_unknown_error_outputs(StemTeXErrorCode *error_code, char **error_utf8) noexcept {
  set_error_outputs(STEMTEX_ERROR_INTERNAL, kUnknownInternalException, error_code, error_utf8);
}

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string dup_to_c_string(const std::string &s) {
  return s;
}

char *alloc_c_string(const std::string &s) noexcept {
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

bool env_name_equal_ascii_ci(const std::wstring &a, const std::wstring &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    wchar_t ca = a[i];
    wchar_t cb = b[i];
    if (ca >= L'a' && ca <= L'z') ca = ca - L'a' + L'A';
    if (cb >= L'a' && cb <= L'z') cb = cb - L'a' + L'A';
    if (ca != cb) return false;
  }
  return true;
}

void erase_env_name(std::map<std::wstring, std::wstring> &env, const std::wstring &name) {
  for (auto it = env.begin(); it != env.end();) {
    if (env_name_equal_ascii_ci(it->first, name)) {
      it = env.erase(it);
    } else {
      ++it;
    }
  }
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

  const std::wstring tex_names[] = {
      L"PATH",
      L"TEXMFROOT",
      L"TEXMFCNF",
      L"TEXFORMATS",
      L"XE_FONTCONFIG_PATH",
      L"FONTCONFIG_PATH",
      L"XE_FC_CACHEDIR",
      L"FC_CACHEDIR",
      L"TEXMFDIST",
      L"TEXMFSYSVAR",
      L"TEXMFSYSCONFIG",
      L"TEXMFVAR",
      L"TEXMFCONFIG",
      L"TEXMF",
      L"TEXMFDBS",
      L"SYSTEXMF",
      L"TEXMFCACHE",
      L"VARTEXFONTS",
      L"TEXPOOL",
      L"TEXINPUTS",
      L"TEXFONTMAPS",
      L"WEB2C",
      L"W32TEX",
      L"OSFONTDIR",
      L"SELFAUTOLOC",
      L"SELFAUTODIR",
      L"SELFAUTOPARENT",
      L"SELFAUTOGRANDPARENT",
      L"ICU_DATA",
      L"command_line_encoding",
      kWorkerHostLifetimeEnv,
  };
  for (const auto &name : tex_names) erase_env_name(env, name);
  for (const auto &kv : overrides) {
    erase_env_name(env, kv.first);
    env[kv.first] = kv.second;
  }

  std::vector<wchar_t> block;
  for (const auto &kv : env) {
    std::wstring entry = kv.first + L"=" + kv.second;
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

std::vector<wchar_t> environment_with_lifetime_handle(const std::vector<wchar_t> &environment, HANDLE lifetime_read) {
  std::vector<wchar_t> out = environment;
  if (out.empty()) out.push_back(L'\0');
  if (!out.empty() && out.back() == L'\0') out.pop_back();
  std::wstring entry =
      std::wstring(kWorkerHostLifetimeEnv) + L"=" + std::to_wstring(reinterpret_cast<uintptr_t>(lifetime_read));
  out.insert(out.end(), entry.begin(), entry.end());
  out.push_back(L'\0');
  out.push_back(L'\0');
  return out;
}

std::string slash_path(const fs::path &p) {
  return p.generic_string();
}

std::string path_utf8(const fs::path &p) {
  return narrow_utf8(path_to_wstring(p));
}

fs::path local_app_data_root() {
  DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (needed > 1) {
    std::wstring value(needed - 1, L'\0');
    GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), needed);
    if (!value.empty()) return fs::path(value);
  }
  return fs::temp_directory_path();
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

void replace_all(std::string &text, const std::string &from, const std::string &to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
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

uint32_t read_be_uint(const std::vector<uint8_t> &bytes, size_t pos, int width) {
  if (pos + (size_t)width > bytes.size()) throw std::runtime_error("Malformed XDV font definition");
  uint32_t value = 0;
  for (int i = 0; i < width; ++i) value = (value << 8) | bytes[pos + (size_t)i];
  return value;
}

struct XdvParts {
  std::vector<uint8_t> fontdefs;
};

using FontDefMap = std::map<uint32_t, std::vector<uint8_t>>;

void collect_fontdefs_from_bytes(const std::vector<uint8_t> &xdv, FontDefMap &fontdefs) {
  constexpr uint8_t SET1 = 128;
  constexpr uint8_t SET_RULE = 132;
  constexpr uint8_t PUT1 = 133;
  constexpr uint8_t PUT_RULE = 137;
  constexpr uint8_t BOP = 139;
  constexpr uint8_t RIGHT1 = 143;
  constexpr uint8_t W0 = 147;
  constexpr uint8_t W1 = 148;
  constexpr uint8_t X0 = 152;
  constexpr uint8_t X1 = 153;
  constexpr uint8_t DOWN1 = 157;
  constexpr uint8_t Y0 = 161;
  constexpr uint8_t Y1 = 162;
  constexpr uint8_t Z0 = 166;
  constexpr uint8_t Z1 = 167;
  constexpr uint8_t FNT1 = 235;
  constexpr uint8_t XXX1 = 239;
  constexpr uint8_t FNT_DEF1 = 243;
  constexpr uint8_t PRE = 247;
  constexpr uint8_t POST = 248;
  constexpr uint8_t POST_POST = 249;
  constexpr uint8_t XDV_NATIVE_FONT_DEF = 252;
  constexpr uint8_t XDV_GLYPHS = 253;
  constexpr uint8_t XDV_TEXT_AND_GLYPHS = 254;
  constexpr uint8_t PTEXDIR = 255;
  constexpr uint16_t XDV_FLAG_COLORED = 0x0200;
  constexpr uint16_t XDV_FLAG_EXTEND = 0x1000;
  constexpr uint16_t XDV_FLAG_SLANT = 0x2000;
  constexpr uint16_t XDV_FLAG_EMBOLDEN = 0x4000;

  size_t pos = 0;
  while (pos < xdv.size()) {
    size_t start = pos;
    uint8_t op = xdv[pos++];
    if (op <= 127 || (op >= 171 && op <= 234) || op == 138 || op == 140 || op == 141 || op == 142 ||
        op == W0 || op == X0 || op == Y0 || op == Z0 || op == 250 || op == 251) {
      continue;
    }
    if (op >= SET1 && op <= 131) {
      pos += (size_t)(op - SET1 + 1);
    } else if (op == SET_RULE || op == PUT_RULE) {
      pos += 8;
    } else if (op >= PUT1 && op <= 136) {
      pos += (size_t)(op - PUT1 + 1);
    } else if (op == BOP) {
      pos += 44;
    } else if (op >= RIGHT1 && op <= 146) {
      pos += (size_t)(op - RIGHT1 + 1);
    } else if (op >= W1 && op <= 151) {
      pos += (size_t)(op - W1 + 1);
    } else if (op >= X1 && op <= 156) {
      pos += (size_t)(op - X1 + 1);
    } else if (op >= DOWN1 && op <= 160) {
      pos += (size_t)(op - DOWN1 + 1);
    } else if (op >= Y1 && op <= 165) {
      pos += (size_t)(op - Y1 + 1);
    } else if (op >= Z1 && op <= 170) {
      pos += (size_t)(op - Z1 + 1);
    } else if (op >= FNT1 && op <= 238) {
      pos += (size_t)(op - FNT1 + 1);
    } else if (op >= XXX1 && op <= 242) {
      int width = op - XXX1 + 1;
      uint32_t len = read_be_uint(xdv, pos, width);
      pos += (size_t)width + len;
    } else if (op >= FNT_DEF1 && op <= 246) {
      int id_width = op - FNT_DEF1 + 1;
      uint32_t id = read_be_uint(xdv, pos, id_width);
      pos += (size_t)id_width + 12;
      if (pos + 2 > xdv.size()) throw std::runtime_error("Malformed XDV font definition");
      uint32_t area_len = xdv[pos++];
      uint32_t name_len = xdv[pos++];
      pos += area_len + name_len;
      if (pos > xdv.size()) throw std::runtime_error("Malformed XDV font definition");
      fontdefs[id] = std::vector<uint8_t>(xdv.begin() + (std::ptrdiff_t)start, xdv.begin() + (std::ptrdiff_t)pos);
    } else if (op == PRE) {
      if (pos + 14 > xdv.size()) throw std::runtime_error("Malformed XDV preamble");
      uint32_t comment_len = xdv[pos + 13];
      pos += 14 + comment_len;
    } else if (op == POST || op == POST_POST) {
      break;
    } else if (op == XDV_NATIVE_FONT_DEF) {
      uint32_t id = read_be_uint(xdv, pos, 4);
      pos += 8;
      uint16_t flags = (uint16_t)read_be_uint(xdv, pos, 2);
      pos += 2;
      if (pos >= xdv.size()) throw std::runtime_error("Malformed XDV native font definition");
      uint32_t name_len = xdv[pos++];
      pos += name_len + 4;
      if (flags & XDV_FLAG_COLORED) pos += 4;
      if (flags & XDV_FLAG_EXTEND) pos += 4;
      if (flags & XDV_FLAG_SLANT) pos += 4;
      if (flags & XDV_FLAG_EMBOLDEN) pos += 4;
      if (pos > xdv.size()) throw std::runtime_error("Malformed XDV native font definition");
      fontdefs[id] = std::vector<uint8_t>(xdv.begin() + (std::ptrdiff_t)start, xdv.begin() + (std::ptrdiff_t)pos);
    } else if (op == XDV_GLYPHS) {
      pos += 4;
      uint32_t len = read_be_uint(xdv, pos, 2);
      pos += 2 + len * 10;
    } else if (op == XDV_TEXT_AND_GLYPHS) {
      uint32_t text_len = read_be_uint(xdv, pos, 2);
      pos += 2 + text_len * 2 + 4;
      uint32_t glyph_len = read_be_uint(xdv, pos, 2);
      pos += 2 + glyph_len * 10;
    } else if (op == PTEXDIR) {
      pos += 1;
    } else {
      throw std::runtime_error("Unsupported XDV opcode while scanning font definitions");
    }
    if (pos > xdv.size()) throw std::runtime_error("Malformed XDV while scanning font definitions");
  }
}

std::vector<uint8_t> merged_fontdefs(const XdvParts &parts, const std::vector<uint8_t> &xdv_body) {
  FontDefMap fontdefs;
  collect_fontdefs_from_bytes(parts.fontdefs, fontdefs);
  collect_fontdefs_from_bytes(xdv_body, fontdefs);
  std::vector<uint8_t> merged;
  for (const auto &entry : fontdefs) merged.insert(merged.end(), entry.second.begin(), entry.second.end());
  return merged;
}

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
                         const XdvParts &parts, int page_count, uint64_t last_bop) {
  if (xdv_body.empty() || xdv_body[0] != 247) throw std::runtime_error("Expected full XDV body");
  std::vector<uint8_t> out = xdv_body;
  if (page_count < 0 || page_count > std::numeric_limits<uint16_t>::max()) {
    throw std::runtime_error("XDV page count exceeds 16-bit postamble limit");
  }
  if (last_bop > (uint64_t)std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("XDV BOP offset exceeds 32-bit postamble limit");
  }
  if (out.size() > (size_t)std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("XDV postamble offset exceeds 32-bit postamble limit");
  }
  int32_t post_offset = static_cast<int32_t>(out.size());
  out.push_back(248);
  append_be32(out, static_cast<int32_t>(last_bop));
  append_be32(out, 25400000);
  append_be32(out, 473628672);
  append_be32(out, 1000);
  append_be32(out, 0x1d200000);
  append_be32(out, 0x1d200000);
  append_be16(out, 20);
  append_be16(out, static_cast<uint16_t>(page_count));
  std::vector<uint8_t> fontdefs = merged_fontdefs(parts, xdv_body);
  out.insert(out.end(), fontdefs.begin(), fontdefs.end());
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
    close_handles();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    HANDLE lifetime_read = nullptr, lifetime_write = nullptr;
    auto close_pipe_handles = [&]() {
      close_handle(stdin_read);
      close_handle(stdin_write);
      close_handle(stdout_read);
      close_handle(stdout_write);
      close_handle(stderr_read);
      close_handle(stderr_write);
      close_handle(lifetime_read);
      close_handle(lifetime_write);
    };
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
      throw std::runtime_error("CreatePipe stdin failed");
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
      close_pipe_handles();
      throw std::runtime_error("CreatePipe stdout failed");
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
      close_pipe_handles();
      throw std::runtime_error("CreatePipe stderr failed");
    }
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&lifetime_read, &lifetime_write, &sa, 0)) {
      close_pipe_handles();
      throw std::runtime_error("CreatePipe lifetime failed");
    }
    SetHandleInformation(lifetime_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    std::wstring cwdw = cwd.wstring();
    std::vector<wchar_t> child_environment = environment_with_lifetime_handle(environment, lifetime_read);
    LPVOID env = child_environment.empty() ? nullptr : child_environment.data();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env, cwdw.c_str(), &si, &pi);
    DWORD create_err = ok ? 0 : GetLastError();
    CloseHandle(stdin_read);
    stdin_read = nullptr;
    CloseHandle(stdout_write);
    stdout_write = nullptr;
    CloseHandle(stderr_write);
    stderr_write = nullptr;
    CloseHandle(lifetime_read);
    lifetime_read = nullptr;
    if (!ok) {
      close_pipe_handles();
      throw std::runtime_error("CreateProcess failed: " + std::to_string(create_err));
    }

    pi_ = pi;
    stdin_ = stdin_write;
    stdout_ = stdout_read;
    stderr_ = stderr_read;
    lifetime_ = lifetime_write;
    lifetime_write = nullptr;
    try {
      stdout_thread_ = std::thread([this, on_data]() noexcept { read_loop(stdout_, on_data); });
      stderr_thread_ = std::thread([this, on_data]() noexcept { read_loop(stderr_, on_data); });
    } catch (...) {
      stop();
      throw;
    }
  }

  void write_stdin(const std::string &text) {
    DWORD written = 0;
    if (!WriteFile(stdin_, text.data(), (DWORD)text.size(), &written, nullptr)) {
      DWORD err = GetLastError();
      throw std::runtime_error("WriteFile stdin failed: " + std::to_string(err));
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
      close_handle(lifetime_);
      close_handle(stdin_);
      DWORD wait = WaitForSingleObject(pi_.hProcess, 5000);
      if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi_.hProcess, 1);
        WaitForSingleObject(pi_.hProcess, 5000);
      }
    }
    join_readers();
    close_handles();
  }

  bool is_running() const {
    if (!pi_.hProcess) return false;
    return WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT;
  }

 private:
  void read_loop(HANDLE h, DataCallback on_data) noexcept {
    try {
      char buf[4096];
      DWORD n = 0;
      while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        on_data(std::string(buf, buf + n));
      }
    } catch (...) {
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
    close_handle(lifetime_);
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
  HANDLE lifetime_ = nullptr;
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
  fs::path texmf_root;
  fs::path profile_root;
  fs::path state_root;
  fs::path renders_root;
  fs::path fontconfig_conf_root;
  fs::path fontconfig_cache_root;
  fs::path warmup_tex;
  fs::path worker_template;
  fs::path preamble_tex;
  int request_timeout_ms = 90000;
  int xdvipdfmx_timeout_ms = 90000;
  double min_width_pt = 0.0;
  double max_width_pt = 0.0;
  double default_width_pt = 360.0;
  int spare_worker_count = 0;
  bool auto_restart = true;
  bool delete_intermediates = false;
};

enum class RenderFormat {
  Pdf,
  Svg,
};

struct RenderOutput {
  std::string request_id;
  fs::path output_path;
  std::string output_format;
  std::string summary_json;
  StemTeXRenderOutcomeCode outcome_code = STEMTEX_RENDER_OUTCOME_OK;
  int issue_flags = 0;
  std::string outcome_message;
};

constexpr int kWorkerMaxRenderableRequests = 60000;
constexpr uint64_t kWorkerMaxCumulativeXdvBytes = 1536ull * 1024ull * 1024ull;
constexpr double kDefaultFontSizePt = 10.0;
constexpr double kMinFontSizePt = 1.0;
constexpr double kMaxFontSizePt = 200.0;

constexpr const char *kDefaultWorkerTemplate = R"STEMTEX_WORKER(\input{@@STEMTEX_PREAMBLE@@}
\newcount\snippetcount
\newdimen\stemtexfontsize
\newdimen\stemtexbaselineskip
\newif\ifstemtexcheckpointed
\def\workerstopline{\workerstop}
\def\stemtexemptyline{}
\pagestyle{empty}
\makeatletter
\def\stemtexresetfragmentstate{%
  \setcounter{page}{1}%
  \setcounter{equation}{0}%
  \setcounter{footnote}{0}%
  \setcounter{enumi}{0}%
  \setcounter{enumii}{0}%
  \setcounter{enumiii}{0}%
  \setcounter{enumiv}{0}%
  \global\@listdepth=\z@
  \@itemdepth=\z@
  \@enumdepth=\z@
  \global\@inlabelfalse
  \global\@newlistfalse
  \global\@noparitemfalse
  \global\@noparlistfalse
  \@noitemargfalse
  \@nmbrlistfalse
  \global\setbox\@labels=\box\voidb@x
}%
\makeatother
\begin{document}
\typeout{WORKER_READY}
\def\workerloop{%
  \advance\snippetcount by 1
  \typeout{WORKER_WAIT:\the\snippetcount}%
  \read16 to\snippetHsize
  \ifx\snippetHsize\stemtexemptyline
    \read16 to\snippetHsize
  \fi
  \ifx\snippetHsize\workerstopline
    \typeout{WORKER_STOPPED}%
  \else
    \read16 to\stemtexfontsizeline
    \read16 to\requestfile
    \scrollmode
    \begin{preview}%
      \begin{minipage}{\snippetHsize}%
        \hsize=\snippetHsize
        \parindent=0pt
        \begingroup
        \stemtexresetfragmentstate
        \stemtexfontsize=\stemtexfontsizeline
        \stemtexbaselineskip=1.2\stemtexfontsize
        \normalfont\fontsize{\the\stemtexfontsize}{\the\stemtexbaselineskip}\selectfont\normalcolor
        \input\requestfile
        \par
        \endgroup
      \end{minipage}%
    \end{preview}%
    \typeout{WORKER_DONE:\the\snippetcount}%
    \ifstemtexcheckpointed\else
      \stemtexcheckpointedtrue
      \special{stemtex:checkpoint}%
      \typeout{WORKER_BASELINE_READY}%
    \fi
    \errorstopmode
    \workerloop
  \fi
}
\workerloop
\end{document}
)STEMTEX_WORKER";

std::string installed_warmup_body(const RendererConfig &cfg) {
  std::string warmup = read_text_file(cfg.warmup_tex);
  auto strip_environment = [](std::string text, const std::string &name) {
    const std::string begin = "\\begin{" + name + "}";
    const std::string end = "\\end{" + name + "}";
    size_t body_start = text.find(begin);
    if (body_start == std::string::npos) return text;
    body_start += begin.size();
    size_t body_end = text.find(end, body_start);
    return text.substr(body_start, body_end == std::string::npos ? std::string::npos : body_end - body_start);
  };
  warmup = strip_environment(warmup, "document");
  warmup = strip_environment(warmup, "preview");
  return warmup;
}

fs::path worker_host_exe(const RendererConfig &cfg) {
  return cfg.runtime_root / "bin" / "windows" / "stemtex-worker-host.exe";
}

fs::path xetexdaemon_exe(const RendererConfig &cfg) {
  return cfg.runtime_root / "bin" / "windows" / "xetexdaemon.exe";
}

std::string hosted_xetex_command(const RendererConfig &cfg, const std::string &xetex_command) {
  return quote_cmd_arg(worker_host_exe(cfg).string()) + " -- " + xetex_command;
}

std::wstring worker_command(const RendererConfig &cfg, const fs::path &out_dir) {
  fs::path host = worker_host_exe(cfg);
  fs::path exe = xetexdaemon_exe(cfg);
  fs::path worker = out_dir / "worker-template.tex";
  std::string worker_arg = slash_path(worker);
  std::wostringstream cmd;
  cmd << quote_cmd_arg_w(path_to_wstring(host))
      << L" -- "
      << quote_cmd_arg_w(path_to_wstring(exe))
      << L" -fmt=xelatexdaemon --no-font-cache-refresh"
      << L" -jobname=worker-template -interaction=errorstopmode -no-pdf -flush-output-on-shipout"
      << L" -output-directory=" << quote_cmd_arg_w(path_to_wstring(out_dir))
      << L" " << quote_cmd_arg_w(widen_utf8(worker_arg));
  return cmd.str();
}

void materialize_worker_template(const RendererConfig &cfg, const fs::path &out_dir) {
  std::string text = cfg.worker_template.empty() ? std::string(kDefaultWorkerTemplate) : read_text_file(cfg.worker_template);
  replace_all(text, "@@STEMTEX_PREAMBLE@@", slash_path(cfg.preamble_tex));
  write_text_file(out_dir / "worker-template.tex", text);
}

void write_fontconfig_config(const RendererConfig &cfg) {
  fs::path conf_dir = cfg.fontconfig_conf_root;
  fs::path cache_dir = cfg.fontconfig_cache_root;
  fs::create_directories(conf_dir / "conf.d");
  fs::create_directories(cache_dir);

  std::ostringstream fonts;
  fonts << "<?xml version=\"1.0\"?>\n"
        << "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
        << "<fontconfig>\n"
        << "  <dir>C:/Windows/fonts</dir>\n"
        << "  <dir>" << slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "opentype") << "</dir>\n"
        << "  <dir>" << slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "truetype") << "</dir>\n"
        << "  <cachedir>" << slash_path(cache_dir) << "</cachedir>\n"
        << "  <include ignore_missing=\"yes\">conf.d</include>\n"
        << "  <config><rescan><int>30</int></rescan></config>\n"
        << "</fontconfig>\n";
  write_text_file(conf_dir / "fonts.conf", fonts.str());
  write_text_file(conf_dir / "conf.d" / "51-local.conf",
                  "<?xml version=\"1.0\"?>\n"
                  "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
                  "<fontconfig></fontconfig>\n");
}

std::vector<wchar_t> worker_environment(const RendererConfig &cfg) {
  fs::path bin = cfg.runtime_root / "bin" / "windows";
  fs::path texmfcnf = cfg.texmf_root / "texmf-dist" / "web2c";
  fs::path fmt = cfg.runtime_root / "texmf-var" / "web2c" / "xetex";
  fs::path fontconf = cfg.fontconfig_conf_root;
  fs::path fontcache = cfg.fontconfig_cache_root;
  std::wstring fontmaps =
      widen_utf8(slash_path(cfg.texmf_root / "texmf-var" / "fonts" / "map" / "pdftex" / "updmap")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-var" / "fonts" / "map" / "dvipdfmx" / "updmap")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "map" / "dvipdfmx")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "map")) + L"//";
  std::wstring system_root;
  wchar_t sysroot[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
  if (n > 0 && n < MAX_PATH) system_root = sysroot;
  std::wstring path = path_to_wstring(bin);
  if (!system_root.empty()) path += L";" + system_root + L"\\System32";

  std::map<std::wstring, std::wstring> env = {
      {L"PATH", path},
      {L"TEXMFROOT", path_to_wstring(cfg.texmf_root)},
      {L"TEXMFCNF", path_to_wstring(texmfcnf)},
      {L"TEXFORMATS", path_to_wstring(fmt) + L";" + path_to_wstring(fmt) + L"\\"},
      {L"XE_FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"XE_FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"TEXMFDIST", path_to_wstring(cfg.texmf_root / "texmf-dist")},
      {L"TEXMFSYSVAR", path_to_wstring(cfg.texmf_root / "texmf-var")},
      {L"TEXMFSYSCONFIG", path_to_wstring(cfg.texmf_root / "texmf-config")},
      {L"TEXMFVAR", path_to_wstring(cfg.texmf_root / "texmf-var")},
      {L"TEXMFCONFIG", path_to_wstring(cfg.texmf_root / "texmf-config")},
      {L"TEXFONTMAPS", fontmaps},
      {L"WEB2C", path_to_wstring(cfg.texmf_root / "texmf-dist" / "web2c")},
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
      _wputenv_s(kv.first.c_str(), kv.second.c_str());
    }
  }

  ~ScopedEnvironment() {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if (it->had_value) {
        SetEnvironmentVariableW(it->name.c_str(), it->value.c_str());
        _wputenv_s(it->name.c_str(), it->value.c_str());
      } else {
        SetEnvironmentVariableW(it->name.c_str(), nullptr);
        _wputenv_s(it->name.c_str(), L"");
      }
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
  fs::path texmfcnf = cfg.texmf_root / "texmf-dist" / "web2c";
  fs::path fmt = cfg.runtime_root / "texmf-var" / "web2c" / "xetex";
  fs::path fontconf = cfg.fontconfig_conf_root;
  fs::path fontcache = cfg.fontconfig_cache_root;
  std::wstring fontmaps =
      widen_utf8(slash_path(cfg.texmf_root / "texmf-var" / "fonts" / "map" / "pdftex" / "updmap")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-var" / "fonts" / "map" / "dvipdfmx" / "updmap")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "map" / "dvipdfmx")) + L";" +
      widen_utf8(slash_path(cfg.texmf_root / "texmf-dist" / "fonts" / "map")) + L"//";
  std::wstring system_root;
  wchar_t sysroot[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
  if (n > 0 && n < MAX_PATH) system_root = sysroot;
  std::wstring path = path_to_wstring(bin);
  if (!system_root.empty()) path += L";" + system_root + L"\\System32";

  std::map<std::wstring, std::wstring> env = {
      {L"PATH", path},
      {L"TEXMFROOT", path_to_wstring(cfg.texmf_root)},
      {L"TEXMFCNF", path_to_wstring(texmfcnf)},
      {L"TEXFORMATS", path_to_wstring(fmt) + L";" + path_to_wstring(fmt) + L"\\"},
      {L"XE_FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"FONTCONFIG_PATH", path_to_wstring(fontconf)},
      {L"XE_FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"FC_CACHEDIR", path_to_wstring(fontcache)},
      {L"TEXMFDIST", path_to_wstring(cfg.texmf_root / "texmf-dist")},
      {L"TEXMFSYSVAR", path_to_wstring(cfg.texmf_root / "texmf-var")},
      {L"TEXMFSYSCONFIG", path_to_wstring(cfg.texmf_root / "texmf-config")},
      {L"TEXMFVAR", path_to_wstring(cfg.texmf_root / "texmf-var")},
      {L"TEXMFCONFIG", path_to_wstring(cfg.texmf_root / "texmf-config")},
      {L"TEXFONTMAPS", fontmaps},
      {L"WEB2C", path_to_wstring(cfg.texmf_root / "texmf-dist" / "web2c")},
      {L"W32TEX", path_to_wstring(cfg.runtime_root)},
      {L"command_line_encoding", L""},
  };
  fs::path icu = bin / "icu-data";
  if (has_file_with_prefix_suffix(icu, L"icudt", L"l.dat")) env[L"ICU_DATA"] = path_to_wstring(icu);
  return env;
}

class DvipdfmxDaemon {
 public:
  using ApiFn = int(__cdecl *)(int, char **);
  using ShutdownFn = int(__cdecl *)(void);
  using IssueFlagsFn = int(__cdecl *)(void);
  using IssueMessageFn = const char *(__cdecl *)(void);

  struct ConvertResult {
    std::string mode;
    int return_code = 0;
    int issue_flags = 0;
    std::string issue_message;
  };

  explicit DvipdfmxDaemon(const RendererConfig &cfg)
      : program_arg_(slash_path(cfg.runtime_root / "bin" / "windows" / "xdvipdfmxdaemon.exe")),
        env_(runtime_environment_overrides(cfg)) {
    fs::path init_trace_path = cfg.state_root / "xdvipdfmx-init-trace.log";
    env_[L"STEMTEX_XDVIPDFMX_TRACE"] = path_to_wstring(init_trace_path);
    fs::path dll_path = cfg.runtime_root / "bin" / "windows" / "dvipdfmxdaemon.dll";
    if (!fs::exists(dll_path)) throw std::runtime_error("dvipdfmxdaemon.dll missing");
    env_scope_ = std::make_unique<ScopedEnvironment>(env_);
    dll_ = LoadLibraryW(dll_path.wstring().c_str());
    if (!dll_) throw std::runtime_error("LoadLibrary dvipdfmxdaemon.dll failed: " + std::to_string(GetLastError()));
    init_ = reinterpret_cast<ApiFn>(GetProcAddress(dll_, "dvipdfmxdaemon_init"));
    convert_ = reinterpret_cast<ApiFn>(GetProcAddress(dll_, "dvipdfmxdaemon_convert"));
    shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(dll_, "dvipdfmxdaemon_shutdown"));
    last_issue_flags_ = reinterpret_cast<IssueFlagsFn>(GetProcAddress(dll_, "dvipdfmxdaemon_last_issue_flags"));
    last_issue_message_ =
        reinterpret_cast<IssueMessageFn>(GetProcAddress(dll_, "dvipdfmxdaemon_last_issue_message"));
    if (!init_ || !convert_ || !shutdown_) {
      FreeLibrary(dll_);
      dll_ = nullptr;
      throw std::runtime_error("dvipdfmxdaemon.dll does not export hot-start API");
    }

    std::vector<std::string> args = {program_arg_};
    std::vector<char *> av;
    for (auto &arg : args) av.push_back(arg.data());
    int code = init_((int)av.size(), av.data());
    if (code != 0) throw std::runtime_error("dvipdfmxdaemon_init returned " + std::to_string(code));
  }

  DvipdfmxDaemon(const DvipdfmxDaemon &) = delete;
  DvipdfmxDaemon &operator=(const DvipdfmxDaemon &) = delete;

  ~DvipdfmxDaemon() noexcept {
    try {
      if (dll_) {
        if (shutdown_) {
          shutdown_();
        }
        FreeLibrary(dll_);
      }
      env_scope_.reset();
    } catch (...) {
    }
  }

  ConvertResult convert(const fs::path &final_path, const fs::path &pdf_path, const std::string &page_range) {
    fs::path trace_path = pdf_path.parent_path() / "xdvipdfmx-trace.log";
    std::map<std::wstring, std::wstring> trace_env = {
        {L"STEMTEX_XDVIPDFMX_TRACE", path_to_wstring(trace_path)},
    };
    ScopedEnvironment trace_scope(trace_env);
    std::vector<std::string> args = {
        program_arg_,
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
    int code = convert_((int)av.size(), av.data());
    if (code != 0 && code != 2) throw std::runtime_error("dvipdfmxdaemon_convert returned " + std::to_string(code));
    if (!fs::exists(pdf_path)) throw std::runtime_error("dvipdfmxdaemon.dll did not write PDF: " + pdf_path.string());
    ConvertResult result;
    result.mode = "daemon-dll";
    result.return_code = code;
    result.issue_flags = last_issue_flags_ ? last_issue_flags_() : 0;
    if (last_issue_message_) {
      const char *message = last_issue_message_();
      if (message) result.issue_message = message;
    }
    return result;
  }

 private:
  std::string program_arg_;
  std::map<std::wstring, std::wstring> env_;
  std::unique_ptr<ScopedEnvironment> env_scope_;
  HMODULE dll_ = nullptr;
  ApiFn init_ = nullptr;
  ApiFn convert_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
  IssueFlagsFn last_issue_flags_ = nullptr;
  IssueMessageFn last_issue_message_ = nullptr;
};

class DvisvgmDaemon {
 public:
  using ApiFn = int(__cdecl *)(int, char **);
  using ShutdownFn = int(__cdecl *)(void);
  using LastErrorFn = const char *(__cdecl *)(void);

  struct ConvertResult {
    std::string mode;
    int return_code = 0;
    std::string error_message;
  };

  explicit DvisvgmDaemon(const RendererConfig &cfg)
      : program_arg_(slash_path(cfg.runtime_root / "bin" / "windows" / "dvisvgmdaemon.exe")),
        env_(runtime_environment_overrides(cfg)) {
    fs::path dll_path = cfg.runtime_root / "bin" / "windows" / "dvisvgmdaemon.dll";
    if (!fs::exists(dll_path)) throw std::runtime_error("dvisvgmdaemon.dll missing");
    env_scope_ = std::make_unique<ScopedEnvironment>(env_);
    dll_ = LoadLibraryW(dll_path.wstring().c_str());
    if (!dll_) throw std::runtime_error("LoadLibrary dvisvgmdaemon.dll failed: " + std::to_string(GetLastError()));
    init_ = reinterpret_cast<ApiFn>(GetProcAddress(dll_, "dvisvgmdaemon_init"));
    convert_ = reinterpret_cast<ApiFn>(GetProcAddress(dll_, "dvisvgmdaemon_convert"));
    shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(dll_, "dvisvgmdaemon_shutdown"));
    last_error_ = reinterpret_cast<LastErrorFn>(GetProcAddress(dll_, "dvisvgmdaemon_last_error_message"));
    if (!init_ || !convert_ || !shutdown_ || !last_error_) {
      FreeLibrary(dll_);
      dll_ = nullptr;
      throw std::runtime_error("dvisvgmdaemon.dll does not export hot-start API");
    }

    std::vector<std::string> args = {program_arg_};
    std::vector<char *> av;
    for (auto &arg : args) av.push_back(arg.data());
    int code = init_((int)av.size(), av.data());
    if (code != 0) {
      const char *message = last_error_();
      FreeLibrary(dll_);
      dll_ = nullptr;
      throw std::runtime_error(std::string("dvisvgmdaemon_init returned ") + std::to_string(code) +
                               (message && *message ? ": " + std::string(message) : ""));
    }
  }

  DvisvgmDaemon(const DvisvgmDaemon &) = delete;
  DvisvgmDaemon &operator=(const DvisvgmDaemon &) = delete;

  ~DvisvgmDaemon() noexcept {
    try {
      if (dll_) {
        if (shutdown_) {
          shutdown_();
        }
        FreeLibrary(dll_);
      }
      env_scope_.reset();
    } catch (...) {
    }
  }

  ConvertResult convert(const fs::path &final_path, const fs::path &svg_path, const std::string &page) {
    std::vector<std::string> args = {
        program_arg_,
        "--page",
        page,
        "--bbox",
        "papersize",
        "--exact-bbox",
        "--no-fonts",
        "--output",
        slash_path(svg_path),
        slash_path(final_path),
    };
    std::vector<char *> av;
    for (auto &arg : args) av.push_back(arg.data());
    int code = convert_((int)av.size(), av.data());
    ConvertResult result;
    result.mode = "daemon-dll";
    result.return_code = code;
    if (last_error_) {
      const char *message = last_error_();
      if (message) result.error_message = message;
    }
    if (code != 0) {
      throw std::runtime_error("dvisvgmdaemon_convert returned " + std::to_string(code) +
                               (result.error_message.empty() ? "" : ": " + result.error_message));
    }
    if (!fs::exists(svg_path)) throw std::runtime_error("dvisvgmdaemon.dll did not write SVG: " + svg_path.string());
    return result;
  }

 private:
  std::string program_arg_;
  std::map<std::wstring, std::wstring> env_;
  std::unique_ptr<ScopedEnvironment> env_scope_;
  HMODULE dll_ = nullptr;
  ApiFn init_ = nullptr;
  ApiFn convert_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
  LastErrorFn last_error_ = nullptr;
};

void run_sync(const std::string &command, const fs::path &cwd, const std::vector<wchar_t> &environment,
              DWORD timeout_ms = 10000, bool attach_lifetime = false) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE stdin_read = nullptr, stdin_write = nullptr;
  HANDLE stdout_read = nullptr, stdout_write = nullptr;
  HANDLE stderr_read = nullptr, stderr_write = nullptr;
  HANDLE lifetime_read = nullptr, lifetime_write = nullptr;
  auto close_handle = [](HANDLE &h) {
    if (h) {
      CloseHandle(h);
      h = nullptr;
    }
  };
  auto close_local_handles = [&]() {
    close_handle(stdin_read);
    close_handle(stdin_write);
    close_handle(stdout_read);
    close_handle(stdout_write);
    close_handle(stderr_read);
    close_handle(stderr_write);
    close_handle(lifetime_read);
    close_handle(lifetime_write);
  };
  if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) throw std::runtime_error("CreatePipe stdin failed");
  SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
    close_local_handles();
    throw std::runtime_error("CreatePipe stdout failed");
  }
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
    close_local_handles();
    throw std::runtime_error("CreatePipe stderr failed");
  }
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  if (attach_lifetime) {
    if (!CreatePipe(&lifetime_read, &lifetime_write, &sa, 0)) {
      close_local_handles();
      throw std::runtime_error("CreatePipe lifetime failed");
    }
    SetHandleInformation(lifetime_write, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_read;
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_write;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = widen_utf8(command);
  std::wstring cwdw = cwd.wstring();
  std::vector<wchar_t> child_environment;
  LPVOID env = environment.empty() ? nullptr : const_cast<wchar_t *>(environment.data());
  if (attach_lifetime) {
    child_environment = environment_with_lifetime_handle(environment, lifetime_read);
    env = child_environment.empty() ? nullptr : child_environment.data();
  }
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                           env, cwdw.c_str(), &si, &pi);
  DWORD create_err = ok ? 0 : GetLastError();
  close_handle(stdin_read);
  close_handle(stdin_write);
  close_handle(stdout_write);
  close_handle(stderr_write);
  close_handle(lifetime_read);
  if (!ok) {
    close_local_handles();
    throw std::runtime_error("CreateProcess failed: " + std::to_string(create_err) + ": " + command);
  }

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
  std::thread stdout_thread;
  std::thread stderr_thread;
  try {
    stdout_thread = std::thread([&]() noexcept {
      try {
        stdout_text = read_pipe(stdout_read);
      } catch (...) {
      }
    });
    stderr_thread = std::thread([&]() noexcept {
      try {
        stderr_text = read_pipe(stderr_read);
      } catch (...) {
      }
    });
  } catch (...) {
    close_handle(lifetime_write);
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    if (stdout_thread.joinable()) stdout_thread.join();
    if (stderr_thread.joinable()) stderr_thread.join();
    close_local_handles();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw;
  }

  DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    close_handle(lifetime_write);
    wait = WaitForSingleObject(pi.hProcess, 5000);
    if (wait == WAIT_TIMEOUT) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 5000);
    }
    if (stdout_thread.joinable()) stdout_thread.join();
    if (stderr_thread.joinable()) stderr_thread.join();
    close_local_handles();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    throw std::runtime_error("Command timed out: " + command);
  }
  close_handle(lifetime_write);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  if (stdout_thread.joinable()) stdout_thread.join();
  if (stderr_thread.joinable()) stderr_thread.join();
  close_local_handles();
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    throw std::runtime_error("Command failed with code " + std::to_string(code) + ": " + command + "\nstdout:\n" +
                             stdout_text + "\nstderr:\n" + stderr_text);
  }
}

std::optional<XdvParts> try_read_xdv_parts(const fs::path &path) {
  if (!fs::exists(path)) return std::nullopt;
  try {
    return read_xdv_parts(path);
  } catch (...) {
    return std::nullopt;
  }
}

XdvParts run_warmup(const RendererConfig &cfg) {
  fs::path warmup_dir = cfg.state_root / "warmup";
  std::error_code cleanup_ec;
  fs::remove_all(warmup_dir, cleanup_ec);
  fs::create_directories(warmup_dir);
  fs::path exe = xetexdaemon_exe(cfg);
  auto env = worker_environment(cfg);
  try {
    std::ostringstream cmd;
    cmd << quote_cmd_arg(exe.string()) << " -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error"
        << " -output-directory=" << quote_cmd_arg(warmup_dir.string()) << " " << quote_cmd_arg(cfg.warmup_tex.string());
    run_sync(hosted_xetex_command(cfg, cmd.str()), cfg.profile_root, env, (DWORD)cfg.request_timeout_ms, true);
    fs::path temp_xdv = warmup_dir / "warmup.xdv";
    if (!fs::exists(temp_xdv)) throw std::runtime_error("Warmup XDV was not written: " + temp_xdv.string());
    XdvParts parts = read_xdv_parts(temp_xdv);
    return parts;
  } catch (...) {
    std::error_code ec;
    fs::remove_all(warmup_dir, ec);
    throw;
  }
}

XdvParts load_or_run_warmup(const RendererConfig &cfg) {
  fs::path profile_xdv = cfg.profile_root / "warmup.xdv";
  for (const fs::path &candidate : {
           profile_xdv,
           cfg.profile_root / "worker-template.xdv",
       }) {
    if (auto parts = try_read_xdv_parts(candidate)) return *parts;
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

bool tex_output_has_error(const std::string &text) {
  bool line_start = true;
  for (char c : text) {
    if (line_start && c == '!') return true;
    line_start = (c == '\n' || c == '\r');
  }
  return false;
}

int parse_worker_marker_number(const std::string &line, const char *marker) {
  const char *pos = std::strstr(line.c_str(), marker);
  if (!pos) return -1;
  pos += std::strlen(marker);
  if (*pos < '0' || *pos > '9') return -1;
  return std::atoi(pos);
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

double normalize_default_width(double value) {
  return std::isfinite(value) && value > 0.0 ? value : 360.0;
}

double effective_width(const RendererConfig &cfg, double width) {
  if (std::isfinite(width) && width > 0.0) return width;
  return normalize_default_width(cfg.default_width_pt);
}

double effective_font_size(double font_size_pt) {
  if (!std::isfinite(font_size_pt) || font_size_pt <= 0.0) return kDefaultFontSizePt;
  if (font_size_pt < kMinFontSizePt) return kMinFontSizePt;
  if (font_size_pt > kMaxFontSizePt) return kMaxFontSizePt;
  return font_size_pt;
}

std::string format_decimal(double value) {
  if (!std::isfinite(value)) return "0";
  std::ostringstream s;
  s.imbue(std::locale::classic());
  s << std::fixed << std::setprecision(6) << value;
  std::string out = s.str();
  while (out.size() > 1 && out.back() == '0') out.pop_back();
  if (!out.empty() && out.back() == '.') out.pop_back();
  if (out == "-0") return "0";
  return out;
}

int normalize_spare_worker_count(int count) {
  if (count <= 0) return 0;
  return std::min(4, count);
}

RendererConfig config_from_api_common(const char *repo_root_utf8, const char *runtime_root_utf8,
                                      const char *texmf_root_utf8, const char *profile_root_utf8,
                                      const char *state_root_utf8, const char *renders_root_utf8,
                                      const char *worker_template_utf8) {
  RendererConfig cfg;
  if (!runtime_root_utf8 || !*runtime_root_utf8) {
    throw ApiException(STEMTEX_ERROR_BAD_CONFIG, "runtime_root_utf8 is required");
  }
  cfg.runtime_root = fs::absolute(runtime_root_utf8);
  cfg.texmf_root = texmf_root_utf8 && *texmf_root_utf8
                       ? fs::absolute(texmf_root_utf8)
                       : cfg.runtime_root;
  if (!profile_root_utf8 || !*profile_root_utf8) {
    throw ApiException(STEMTEX_ERROR_BAD_CONFIG, "profile_root_utf8 is required");
  }
  cfg.profile_root = fs::absolute(profile_root_utf8);
  cfg.repo_root = repo_root_utf8 && *repo_root_utf8
                      ? fs::absolute(repo_root_utf8)
                      : cfg.profile_root;
  std::string instance_id = random_id();
  fs::path default_work_root = fs::temp_directory_path() / "stemtex-renderer" / instance_id;
  fs::path state_base_root = state_root_utf8 && *state_root_utf8
                                 ? fs::absolute(state_root_utf8)
                                 : default_work_root / "state";
  cfg.state_root = state_base_root / ("instance-" + instance_id);
  cfg.fontconfig_conf_root = cfg.state_root / "fontconfig" / "conf";
  cfg.fontconfig_cache_root = state_base_root / "fontconfig" / "cache";
  cfg.renders_root = renders_root_utf8 && *renders_root_utf8
                          ? fs::absolute(renders_root_utf8)
                          : default_work_root / "renders";
  cfg.warmup_tex = cfg.profile_root / "warmup.tex";
  if (worker_template_utf8 && *worker_template_utf8) {
    cfg.worker_template = fs::absolute(worker_template_utf8);
  }
  cfg.preamble_tex = cfg.profile_root / "preamble.tex";
  return cfg;
}

RendererConfig config_from_api(const StemTeXConfig *config) {
  if (!config) throw ApiException(STEMTEX_ERROR_BAD_CONFIG, "config is required");
  RendererConfig cfg = config_from_api_common(config->repo_root_utf8, config->runtime_root_utf8,
                                             config->texmf_root_utf8, config->profile_root_utf8,
                                             config->state_root_utf8, config->renders_root_utf8,
                                             config->worker_template_utf8);
  if (config) {
    cfg.request_timeout_ms = normalize_timeout_ms(config->request_timeout_ms);
    cfg.xdvipdfmx_timeout_ms = normalize_timeout_ms(config->xdvipdfmx_timeout_ms);
    cfg.min_width_pt = std::isfinite(config->min_width_pt) ? config->min_width_pt : 0.0;
    cfg.max_width_pt = std::isfinite(config->max_width_pt) ? config->max_width_pt : 0.0;
    cfg.default_width_pt = normalize_default_width(config->default_width_pt);
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
  require_dir(cfg.texmf_root, "texmf root");
  require_dir(cfg.profile_root, "profile root");
  require_file(cfg.runtime_root / "run-xelatexdaemon.bat", "runtime launcher");
  require_file(cfg.runtime_root / "bin" / "windows" / "stemtex-worker-host.exe", "stemtex-worker-host.exe");
  require_file(cfg.runtime_root / "bin" / "windows" / "xetexdaemon.exe", "xetexdaemon.exe");
  require_file(cfg.runtime_root / "bin" / "windows" / "xdvipdfmxdaemon.exe", "xdvipdfmxdaemon.exe");
  require_file(cfg.runtime_root / "bin" / "windows" / "dvipdfmxdaemon.dll", "dvipdfmxdaemon.dll");
  require_file(cfg.runtime_root / "bin" / "windows" / "dvisvgmdaemon.exe", "dvisvgmdaemon.exe");
  require_file(cfg.runtime_root / "bin" / "windows" / "dvisvgmdaemon.dll", "dvisvgmdaemon.dll");
  require_file(cfg.runtime_root / "texmf-var" / "web2c" / "xetex" / "xelatexdaemon.fmt", "xelatexdaemon.fmt");
  require_dir(cfg.texmf_root / "texmf-dist", "texmf-dist");
  require_dir(cfg.texmf_root / "texmf-dist" / "web2c", "texmf-dist web2c");
  require_file(cfg.warmup_tex, "warmup tex");
  if (!cfg.worker_template.empty()) require_file(cfg.worker_template, "worker template");
  require_file(cfg.preamble_tex, "preamble tex");
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
    snapshot_spare_target.store(cfg.spare_worker_count);
    publish_status(STEMTEX_STATUS_STARTING, STEMTEX_STAGE_REBUILDING);
    worker_env = worker_environment(cfg);
    converter = std::make_unique<DvipdfmxDaemon>(cfg);
    append_log("dvipdfmx hot-start DLL initialized\n");
    svg_converter = std::make_unique<DvisvgmDaemon>(cfg);
    append_log("dvisvgm hot-start DLL initialized\n");
    primary = create_ready_worker("primary");
    schedule_spare_rebuild_locked();
    publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
  }

  struct WorkerSlot {
    std::string name;
    fs::path live_out;
    fs::path root;
    ChildProcess child;
    LineWatcher lines;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    bool done = false;
    std::string output_tail;
    std::string request_output;
    uint64_t last_xdv_offset = 0;
    int last_wait_request = 0;
    int last_done_request = 0;
    int next_request = 0;
    int xdv_page_count = 0;
    bool restored = false;
    bool wait_after_restore = false;
    bool baseline_ready = false;
  };

  void append_log(const std::string &text) {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    log_tail += text;
    if (log_tail.size() > 32768) log_tail.erase(0, log_tail.size() - 32768);
  }

  void set_last_error(StemTeXErrorCode code, const std::string &message) noexcept {
    try {
      std::lock_guard<std::mutex> lock(diagnostic_mu);
      last_error = code;
      if (!message.empty()) {
        log_tail += message;
        log_tail += "\n";
        if (log_tail.size() > 32768) log_tail.erase(0, log_tail.size() - 32768);
      }
    } catch (...) {
    }
  }

  void set_last_outcome(StemTeXRenderOutcomeCode code, int issue_flags, const std::string &message) noexcept {
    try {
      std::lock_guard<std::mutex> lock(diagnostic_mu);
      last_outcome = code;
      last_issue_flags = issue_flags;
      last_outcome_message = message;
    } catch (...) {
    }
  }

  StemTeXRenderOutcomeCode get_last_outcome_code() {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    return last_outcome;
  }

  int get_last_issue_flags() {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    return last_issue_flags;
  }

  std::string get_last_outcome_message() {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    return last_outcome_message;
  }

  std::string get_log_tail(int max_bytes) {
    std::lock_guard<std::mutex> lock(diagnostic_mu);
    if (max_bytes <= 0 || (size_t)max_bytes >= log_tail.size()) return log_tail;
    return log_tail.substr(log_tail.size() - (size_t)max_bytes);
  }

  void publish_status(StemTeXRendererStatus next_status, StemTeXRenderStage next_stage) {
    status.store(next_status);
    snapshot_stage.store(next_stage);
  }

  void publish_counts_locked() {
    snapshot_primary_ready.store(primary && primary->child.is_running() ? 1 : 0);
    snapshot_spare_ready.store(spare_ready_count_locked());
    snapshot_spare_target.store(cfg.spare_worker_count);
    snapshot_spare_rebuilding.store(spare_rebuilding ? 1 : 0);
  }

  void publish_status_and_counts_locked(StemTeXRendererStatus next_status, StemTeXRenderStage next_stage) {
    publish_status(next_status, next_stage);
    publish_counts_locked();
  }

  std::unique_ptr<WorkerSlot> create_ready_worker(const std::string &name, bool prime = true) {
    auto slot = std::make_unique<WorkerSlot>();
    slot->name = name;
    slot->root = cfg.state_root / "workers" / (name + "-" + random_id());
    slot->live_out = slot->root / "live";
    fs::create_directories(slot->live_out);
    materialize_worker_template(cfg, slot->live_out);
    slot->ready = false;
    slot->done = false;
    slot->output_tail.clear();
    slot->request_output.clear();
    slot->last_xdv_offset = 0;
    slot->last_wait_request = 0;
    slot->last_done_request = 0;
    slot->next_request = 0;
    slot->xdv_page_count = 0;
    slot->baseline_ready = false;
    WorkerSlot *raw = slot.get();
    raw->child.start(worker_command(cfg, raw->live_out), raw->live_out, worker_env, [this, raw](const std::string &text) {
      append_log(text);
      {
        std::lock_guard<std::mutex> lock(raw->mu);
        raw->output_tail += text;
        if (raw->output_tail.size() > 8192) {
          raw->output_tail.erase(0, raw->output_tail.size() - 8192);
        }
        raw->request_output += text;
        if (raw->request_output.size() > 32768) {
          raw->request_output.erase(0, raw->request_output.size() - 32768);
        }
      }
      raw->lines.feed(text, [raw](const std::string &line) {
        std::lock_guard<std::mutex> lock(raw->mu);
        if (line.find("WORKER_READY") != std::string::npos) {
          raw->ready = true;
          raw->cv.notify_all();
        }
        if (line.find("STEMTEX_RESTORED") != std::string::npos) {
          raw->restored = true;
          raw->cv.notify_all();
        }
        if (line.find("WORKER_BASELINE_READY") != std::string::npos) {
          raw->baseline_ready = true;
          raw->cv.notify_all();
        }
        int wait_request = parse_worker_marker_number(line, "WORKER_WAIT:");
        if (wait_request >= 0) {
          raw->last_wait_request = wait_request;
          if (raw->restored) raw->wait_after_restore = true;
          raw->cv.notify_all();
        }
        int done_request = parse_worker_marker_number(line, "WORKER_DONE:");
        if (done_request >= 0) {
          raw->done = true;
          raw->last_done_request = done_request;
          raw->cv.notify_all();
        }
      });
    });
    std::unique_lock<std::mutex> lock(raw->mu);
    auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.request_timeout_ms);
    while (!raw->ready && !shutting_down.load() && std::chrono::steady_clock::now() < startup_deadline) {
      raw->cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    if (!raw->ready) {
      std::string tail = raw->output_tail;
      lock.unlock();
      raw->child.stop();
      if (shutting_down.load()) {
        throw ApiException(STEMTEX_ERROR_CANCELLED, "Live worker startup cancelled");
      }
      throw ApiException(STEMTEX_ERROR_WORKER_STARTUP,
                         "Live worker did not become ready: " + name + ". TeX output tail:\n" + tail);
    }
    if (shutting_down.load()) {
      lock.unlock();
      raw->child.stop();
      throw ApiException(STEMTEX_ERROR_CANCELLED, "Live worker startup cancelled");
    }
    lock.unlock();
    if (prime) prime_worker(*raw);
    return slot;
  }

  void prime_worker(WorkerSlot &slot) {
    fs::path req_path = slot.root / "warmup-request" / "req1.tex";
    write_text_file(req_path, installed_warmup_body(cfg));
    {
      std::lock_guard<std::mutex> lock(slot.mu);
      slot.done = false;
      slot.restored = false;
      slot.wait_after_restore = false;
      slot.baseline_ready = false;
      slot.request_output.clear();
    }
    try {
      slot.child.write_stdin("360pt\n");
      slot.child.write_stdin(format_decimal(kDefaultFontSizePt) + "pt\n");
      slot.child.write_stdin(slash_path(req_path) + "\n");
    } catch (...) {
      std::string tail;
      {
        std::lock_guard<std::mutex> lock(slot.mu);
        tail = slot.output_tail;
      }
      slot.child.stop();
      throw ApiException(STEMTEX_ERROR_WORKER_STARTUP,
                         "Live worker closed during warmup: " + slot.name + ". TeX output tail:\n" + tail);
    }
    std::unique_lock<std::mutex> lock(slot.mu);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.request_timeout_ms);
    bool completed = false;
    while (!shutting_down.load() && std::chrono::steady_clock::now() < deadline) {
      if ((slot.done && slot.baseline_ready) || !slot.child.is_running()) {
        completed = true;
        break;
      }
      slot.cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    if (!completed || !slot.done || !slot.baseline_ready) {
      std::string tail = slot.output_tail;
      lock.unlock();
      slot.child.stop();
      if (shutting_down.load()) {
        throw ApiException(STEMTEX_ERROR_CANCELLED, "Live worker warmup cancelled");
      }
      throw ApiException(STEMTEX_ERROR_WORKER_STARTUP,
                         "Live worker warmup did not reach checkpoint baseline: " + slot.name + ". TeX output tail:\n" + tail);
    }
    lock.unlock();
    fs::path xdv_path = slot.live_out / "worker-template.xdv";
    slot.last_xdv_offset = fs::file_size(xdv_path);
    slot.last_done_request = std::max(slot.last_done_request, 1);
    slot.next_request = 1;
    slot.xdv_page_count = 1;
  }

  void write_render_request_or_recover(WorkerSlot &slot, double width_pt, double font_size_pt, const fs::path &req_path) {
    try {
      slot.child.write_stdin(format_decimal(width_pt) + "pt\n");
      slot.child.write_stdin(format_decimal(font_size_pt) + "pt\n");
      slot.child.write_stdin(slash_path(req_path) + "\n");
    } catch (...) {
      std::string tail;
      std::string request_output;
      {
        std::lock_guard<std::mutex> lock(slot.mu);
        tail = slot.output_tail;
        request_output = slot.request_output;
      }
      {
        std::lock_guard<std::mutex> control_lock(control_mu);
        active_slot = nullptr;
        cancel_requested = false;
      }
      promote_spare_locked("stdin-closed");
      update_status_after_worker_loss_locked();
      if (tex_output_has_error(request_output)) {
        throw ApiException(STEMTEX_ERROR_TEX_SNIPPET,
                           std::string("TeX snippet failed before worker returned WORKER_DONE. TeX output tail:\n") +
                               request_output);
      }
      throw ApiException(STEMTEX_ERROR_WORKER_RESTARTING,
                         "Live worker closed while sending the request; a replacement worker is being prepared.\n"
                         "TeX output tail:\n" +
                             tail);
    }
  }

  ~StemTeXRenderer() noexcept {
    try {
      shutting_down = true;
      publish_status(STEMTEX_STATUS_DEAD, STEMTEX_STAGE_STOPPING);
      stop_async_worker();
      join_spare_builder();
      if (primary) primary->child.stop();
      for (auto &slot : spares) {
        if (slot) slot->child.stop();
      }
      converter.reset();
      svg_converter.reset();
      std::error_code ec;
      fs::remove_all(cfg.state_root, ec);
    } catch (...) {
    }
  }

  void join_spare_builder() {
    if (spare_builder.joinable()) spare_builder.join();
    spare_builder_finished.store(true);
  }

  void schedule_spare_rebuild_locked() {
    if (spare_builder.joinable() && spare_builder_finished.load()) spare_builder.join();
    if (shutting_down.load() || spare_rebuilding || (int)spares.size() >= cfg.spare_worker_count) return;
    if (spare_builder.joinable()) return;
    spare_rebuilding = true;
    spare_builder_finished.store(false);
    publish_counts_locked();
    try {
      spare_builder = std::thread([this]() {
        struct FinishFlag {
          StemTeXRenderer *self;
          ~FinishFlag() { self->spare_builder_finished.store(true); }
        } finish{this};
        try {
          while (true) {
            int slot_index = 0;
            {
              std::lock_guard<std::mutex> lock(render_mu);
              if (shutting_down.load() || (int)spares.size() >= cfg.spare_worker_count) {
                spare_rebuilding = false;
                publish_counts_locked();
                return;
              }
              slot_index = next_spare_index++;
            }

            std::unique_ptr<WorkerSlot> built;
            try {
              built = create_ready_worker("spare-" + std::to_string(slot_index));
            } catch (...) {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::lock_guard<std::mutex> lock(render_mu);
            if (shutting_down.load()) {
              spare_rebuilding = false;
              publish_counts_locked();
              return;
            }
            if (built) {
              if (!primary || !primary->child.is_running()) {
                if (primary) primary->child.stop();
                primary = std::move(built);
                primary->name = "primary";
                publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
              } else if ((int)spares.size() < cfg.spare_worker_count) {
                spares.push_back(std::move(built));
                publish_counts_locked();
              }
            }
          }
        } catch (...) {
          try {
            std::lock_guard<std::mutex> lock(render_mu);
            spare_rebuilding = false;
            publish_counts_locked();
          } catch (...) {
          }
        }
      });
    } catch (...) {
      spare_rebuilding = false;
      spare_builder_finished.store(true);
      publish_counts_locked();
    }
  }

  void promote_spare_locked(const char *reason) {
    if (primary) {
      append_log(std::string("[stemtex] retiring live worker: ") + reason + "\n");
      primary->child.stop();
    }
    while (!spares.empty()) {
      auto candidate = std::move(spares.back());
      spares.pop_back();
      if (candidate && candidate->child.is_running()) {
        primary = std::move(candidate);
        primary->name = "primary";
        schedule_spare_rebuild_locked();
        publish_counts_locked();
        return;
      }
    }
    primary.reset();
    schedule_spare_rebuild_locked();
    publish_counts_locked();
  }

  bool promote_if_available_locked() {
    while (!spares.empty()) {
      auto candidate = std::move(spares.back());
      spares.pop_back();
      if (candidate && candidate->child.is_running()) {
        primary = std::move(candidate);
        primary->name = "primary";
        schedule_spare_rebuild_locked();
        publish_counts_locked();
        return true;
      }
    }
    publish_counts_locked();
    return false;
  }

  bool worker_needs_rotation_locked(const WorkerSlot &slot) const {
    return slot.xdv_page_count >= kWorkerMaxRenderableRequests ||
           slot.last_xdv_offset >= kWorkerMaxCumulativeXdvBytes;
  }

  void ensure_primary_ready_locked() {
    if (primary && primary->child.is_running()) return;
    if (!promote_if_available_locked()) {
      publish_status_and_counts_locked(STEMTEX_STATUS_RESTARTING, STEMTEX_STAGE_REBUILDING);
      if (primary) primary->child.stop();
      primary.reset();
      try {
        primary = create_ready_worker("primary");
        schedule_spare_rebuild_locked();
      } catch (const std::exception &e) {
        schedule_spare_rebuild_locked();
        throw ApiException(STEMTEX_ERROR_WORKER_STARTUP, std::string("Cannot start XeTeX worker: ") + e.what());
      }
    }
  }

  void update_status_after_worker_loss_locked() {
    publish_status_and_counts_locked(primary && primary->child.is_running() ? STEMTEX_STATUS_READY : STEMTEX_STATUS_RESTARTING,
                                     primary && primary->child.is_running() ? STEMTEX_STAGE_IDLE : STEMTEX_STAGE_REBUILDING);
  }

  void update_status_after_render_exception() {
    if (status.load() == STEMTEX_STATUS_DEAD) return;
    std::lock_guard<std::mutex> lock(render_mu);
    update_status_after_worker_loss_locked();
  }

  void update_status_after_api_exception(StemTeXErrorCode code) noexcept {
    if (code == STEMTEX_ERROR_TEX_SNIPPET || code == STEMTEX_ERROR_XDVIPDFMX || code == STEMTEX_ERROR_DVISVGM) return;
    try {
      update_status_after_render_exception();
    } catch (...) {
    }
  }

  bool clear_active_request() {
    std::lock_guard<std::mutex> control_lock(control_mu);
    bool was_cancelled = cancel_requested;
    active_slot = nullptr;
    cancel_requested = false;
    return was_cancelled;
  }

  void keep_worker_after_recoverable_tex_error_locked(WorkerSlot &slot, int last_wait_request, const char *reason) {
    fs::path xdv_path = slot.live_out / "worker-template.xdv";
    if (fs::exists(xdv_path)) slot.last_xdv_offset = fs::file_size(xdv_path);
    if (last_wait_request > 0) {
      int old_next = slot.next_request;
      slot.next_request = std::max(0, last_wait_request - 1);
      if (slot.next_request != old_next) {
        append_log(std::string("[stemtex] resynced worker request counter after TeX error recovery: ") +
                   std::to_string(old_next) + " -> " + std::to_string(slot.next_request) + "\n");
      }
    }
    append_log(std::string("[stemtex] TeX error returned worker to request loop; keeping worker: ") + reason + "\n");
    publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
  }

  int spare_ready_count_locked() const {
    int count = 0;
    for (const auto &slot : spares) {
      if (slot && slot->child.is_running()) ++count;
    }
    return count;
  }

  void restart() {
    publish_status(STEMTEX_STATUS_RESTARTING, STEMTEX_STAGE_REBUILDING);
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
    publish_counts_locked();
    shutting_down = false;
    parts = load_or_run_warmup(cfg);
    worker_env = worker_environment(cfg);
    primary = create_ready_worker("primary");
    schedule_spare_rebuild_locked();
    publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
    set_last_error(STEMTEX_OK, "");
    set_last_outcome(STEMTEX_RENDER_OUTCOME_OK, 0, "");
  }

  bool cancel_current() {
    std::lock_guard<std::mutex> lock(control_mu);
    if (!active_slot || !active_slot->child.is_running()) return false;
    cancel_requested = true;
    publish_status(STEMTEX_STATUS_RESTARTING, STEMTEX_STAGE_REBUILDING);
    snapshot_primary_ready.store(0);
    active_slot->child.stop();
    return true;
  }

  struct AsyncJob {
    uint64_t id = 0;
    std::string snippet;
    double width_pt = 0.0;
    double font_size_pt = kDefaultFontSizePt;
    StemTeXRenderCallback callback = nullptr;
    void *user_data = nullptr;
  };

  uint64_t submit_async(std::string snippet, double width_pt, double font_size_pt, uint64_t *job_id,
                        StemTeXRenderCallback callback, void *user_data) {
    std::lock_guard<std::mutex> lock(async_mu);
    uint64_t id = ++next_async_job_id;
    if (job_id) *job_id = id;
    if (async_pending) {
      async_cancelled.push_back(std::move(*async_pending));
    }
    async_pending = AsyncJob{id, std::move(snippet), width_pt, effective_font_size(font_size_pt), callback, user_data};
    snapshot_async_pending.store(1);
    snapshot_pending_job_id.store(id);
    if (status.load() == STEMTEX_STATUS_READY && !snapshot_async_running.load()) {
      snapshot_stage.store(STEMTEX_STAGE_QUEUED);
    }
    if (!async_worker.joinable()) {
      try {
        async_worker = std::thread([this]() noexcept { async_loop(); });
      } catch (...) {
        if (async_pending && async_pending->id == id) async_pending.reset();
        snapshot_async_pending.store(0);
        snapshot_pending_job_id.store(0);
        snapshot_stage.store(status.load() == STEMTEX_STATUS_READY ? STEMTEX_STAGE_IDLE : STEMTEX_STAGE_REBUILDING);
        throw;
      }
    }
    async_cv.notify_all();
    return id;
  }

  void stop_async_worker() {
    {
      std::lock_guard<std::mutex> lock(async_mu);
      async_stop = true;
      if (async_pending) {
        async_cancelled.push_back(std::move(*async_pending));
        async_pending.reset();
      }
      snapshot_async_pending.store(0);
      snapshot_pending_job_id.store(0);
    }
    async_cv.notify_all();
    cancel_current();
    if (async_worker.joinable()) async_worker.join();
  }

  void callback_cancelled(const AsyncJob &job, const char *message) {
    invoke_callback_safely(job, 0, nullptr, STEMTEX_ERROR_CANCELLED, message);
  }

  void append_log_noexcept(const char *text) noexcept {
    try {
      append_log(text ? std::string(text) : std::string());
    } catch (...) {
    }
  }

  void append_callback_exception_noexcept(const char *kind, const char *message = nullptr) noexcept {
    try {
      std::string text = "[stemtex] async callback ";
      text += kind ? kind : "threw";
      if (message && *message) {
        text += ": ";
        text += message;
      }
      text += "\n";
      append_log(text);
    } catch (...) {
    }
  }

  void invoke_callback_safely(const AsyncJob &job, int ok, const StemTeXRenderResult *result, StemTeXErrorCode code,
                              const char *error) noexcept {
    if (!job.callback) return;
    try {
      job.callback(job.id, ok, result, code, error, job.user_data);
    } catch (const std::exception &e) {
      append_callback_exception_noexcept("threw", e.what());
    } catch (...) {
      append_callback_exception_noexcept("threw an unknown exception");
    }
  }

  void async_loop() noexcept {
    try {
      while (true) {
      std::optional<AsyncJob> job;
      std::optional<AsyncJob> cancelled;
      {
        std::unique_lock<std::mutex> lock(async_mu);
        async_cv.wait(lock, [this]() { return async_stop || async_pending || !async_cancelled.empty(); });
        if (!async_cancelled.empty()) {
          cancelled = std::move(async_cancelled.front());
          async_cancelled.erase(async_cancelled.begin());
        } else if (async_pending) {
          job = std::move(*async_pending);
          async_pending.reset();
          snapshot_async_pending.store(0);
          snapshot_pending_job_id.store(0);
          snapshot_async_running.store(1);
          snapshot_running_job_id.store(job->id);
          if (status.load() == STEMTEX_STATUS_READY) snapshot_stage.store(STEMTEX_STAGE_QUEUED);
        } else if (async_stop) {
          return;
        }
      }

      if (cancelled) {
        callback_cancelled(*cancelled, "Async render superseded by a newer request");
        continue;
      }
      if (!job) continue;

      StemTeXRenderResult result{};
      StemTeXErrorCode code = STEMTEX_OK;
      std::string error;
      int ok = 0;
      try {
        result = render(job->snippet, job->width_pt, job->font_size_pt);
        ok = 1;
      } catch (const std::exception &e) {
        code = exception_code(e);
        set_last_error(code, e.what());
        set_last_outcome(outcome_from_error(code), 0, e.what());
        update_status_after_api_exception(code);
        error = e.what();
      } catch (...) {
        code = STEMTEX_ERROR_INTERNAL;
        set_last_error(code, kUnknownInternalException);
        set_last_outcome(outcome_from_error(code), 0, kUnknownInternalException);
        update_status_after_api_exception(code);
        error = kUnknownInternalException;
      }
      invoke_callback_safely(*job, ok, ok ? &result : nullptr, code, error.c_str());
      stemtex_renderer_free_result(&result);
      snapshot_async_running.store(0);
      snapshot_running_job_id.store(0);
      if (!snapshot_async_pending.load() && status.load() == STEMTEX_STATUS_READY) snapshot_stage.store(STEMTEX_STAGE_IDLE);
    }
    } catch (const std::exception &e) {
      append_callback_exception_noexcept("thread stopped after internal exception", e.what());
      snapshot_async_running.store(0);
      snapshot_running_job_id.store(0);
      if (status.load() != STEMTEX_STATUS_DEAD) snapshot_stage.store(STEMTEX_STAGE_IDLE);
    } catch (...) {
      append_log_noexcept("[stemtex] async thread stopped after unknown internal exception\n");
      snapshot_async_running.store(0);
      snapshot_running_job_id.store(0);
      if (status.load() != STEMTEX_STATUS_DEAD) snapshot_stage.store(STEMTEX_STAGE_IDLE);
    }
  }

  RenderOutput render_output(const std::string &snippet, double width_pt, double font_size_pt, RenderFormat format) {
    std::lock_guard<std::mutex> render_lock(render_mu);
    publish_status_and_counts_locked(STEMTEX_STATUS_RENDERING, STEMTEX_STAGE_TYPESETTING);
    ensure_primary_ready_locked();
    if (primary && worker_needs_rotation_locked(*primary)) {
      promote_spare_locked("worker-numeric-limit");
      ensure_primary_ready_locked();
    }
    publish_status_and_counts_locked(STEMTEX_STATUS_RENDERING, STEMTEX_STAGE_TYPESETTING);
    WorkerSlot &slot = *primary;
    int64_t start = now_ms();
    std::string id = random_id();
    fs::path render_dir = cfg.renders_root / id;
    fs::path req_path = render_dir / "requests" / "req1.tex";
    write_text_file(req_path, snippet);

    {
      std::lock_guard<std::mutex> lock(slot.mu);
      slot.done = false;
      slot.request_output.clear();
      slot.restored = false;
      slot.wait_after_restore = false;
    }
    {
      std::lock_guard<std::mutex> lock(control_mu);
      active_slot = &slot;
      cancel_requested = false;
    }
    double resolved_width_pt = effective_width(cfg, width_pt);
    double resolved_font_size_pt = effective_font_size(font_size_pt);
    write_render_request_or_recover(slot, resolved_width_pt, resolved_font_size_pt, req_path);

    {
      std::unique_lock<std::mutex> lock(slot.mu);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.request_timeout_ms);
      bool completed = false;
      while (std::chrono::steady_clock::now() < deadline) {
        bool has_tex_error = tex_output_has_error(slot.request_output);
        bool request_done = slot.done;
        bool returned_to_loop = !has_tex_error || slot.wait_after_restore;
        if ((request_done && returned_to_loop) || (has_tex_error && slot.wait_after_restore) ||
            !slot.child.is_running()) {
          completed = true;
          break;
        }
        slot.cv.wait_for(lock, std::chrono::milliseconds(25));
      }
      if (!completed) {
        bool snippet_recovery_stuck = tex_output_has_error(slot.request_output);
        std::string request_output = slot.request_output;
        lock.unlock();
        clear_active_request();
        promote_spare_locked(snippet_recovery_stuck ? "snippet-error-stuck" : "request-timeout");
        update_status_after_worker_loss_locked();
        if (snippet_recovery_stuck) {
          throw ApiException(STEMTEX_ERROR_TEX_SNIPPET,
                             "TeX snippet failed and the live worker did not return to the request loop. TeX output tail:\n" +
                                 request_output);
        }
        throw ApiException(STEMTEX_ERROR_WORKER_TIMEOUT, "Worker request timed out");
      }
      if (!slot.child.is_running()) {
        std::string request_output = slot.request_output;
        std::string tail = slot.output_tail;
        bool has_tex_error = tex_output_has_error(request_output);
        bool was_cancelled = false;
        lock.unlock();
        was_cancelled = clear_active_request();
        promote_spare_locked(was_cancelled ? "cancelled" : (has_tex_error ? "snippet-error-worker-exited" : "exited-before-done"));
        update_status_after_worker_loss_locked();
        if (was_cancelled) throw ApiException(STEMTEX_ERROR_CANCELLED, "Render cancelled");
        if (has_tex_error) {
          throw ApiException(STEMTEX_ERROR_TEX_SNIPPET,
                             "TeX snippet failed and the live worker exited before returning to the request loop. TeX output tail:\n" +
                                 request_output);
        }
        throw ApiException(STEMTEX_ERROR_TEX_SNIPPET, "Worker exited before WORKER_DONE. TeX output tail:\n" + tail);
      }
      if (tex_output_has_error(slot.request_output)) {
        std::string request_output = slot.request_output;
        bool returned_to_loop = slot.wait_after_restore;
        int last_wait_request = slot.last_wait_request;
        lock.unlock();
        clear_active_request();
        if (returned_to_loop) {
          keep_worker_after_recoverable_tex_error_locked(slot, last_wait_request, "snippet-error-restored");
          throw ApiException(STEMTEX_ERROR_TEX_SNIPPET, "TeX snippet failed. TeX output tail:\n" + request_output);
        }
        promote_spare_locked("snippet-error-no-request-loop");
        update_status_after_worker_loss_locked();
        throw ApiException(STEMTEX_ERROR_TEX_SNIPPET,
                           "TeX snippet failed and the live worker did not return to the request loop. TeX output tail:\n" +
                               request_output);
      }
      if (!slot.done) {
        std::string tail = slot.output_tail;
        bool was_cancelled = false;
        was_cancelled = clear_active_request();
        lock.unlock();
        promote_spare_locked(was_cancelled ? "cancelled" : "exited-before-done");
        update_status_after_worker_loss_locked();
        if (was_cancelled) throw ApiException(STEMTEX_ERROR_CANCELLED, "Render cancelled");
        throw ApiException(STEMTEX_ERROR_TEX_SNIPPET, "Worker exited before WORKER_DONE. TeX output tail:\n" + tail);
      }
    }
    clear_active_request();

    fs::path xdv_path = slot.live_out / "worker-template.xdv";
    uint64_t current_size = fs::file_size(xdv_path);
    if (current_size > (uint64_t)std::numeric_limits<int32_t>::max()) {
      promote_spare_locked("xdv-postamble-offset-limit");
      update_status_after_worker_loss_locked();
      throw ApiException(STEMTEX_ERROR_WORKER_RESTARTING,
                         "Live worker cumulative XDV exceeded the postamble offset limit; a replacement worker is being prepared");
    }
    auto cumulative = read_file_range(xdv_path, 0, current_size);
    auto delta = read_file_range(xdv_path, slot.last_xdv_offset, current_size);
    uint64_t last_bop = slot.last_xdv_offset + find_first_bop(delta, xdv_path.string() + " delta");
    if (last_bop > (uint64_t)std::numeric_limits<int32_t>::max()) {
      promote_spare_locked("xdv-bop-offset-limit");
      update_status_after_worker_loss_locked();
      throw ApiException(STEMTEX_ERROR_WORKER_RESTARTING,
                         "Live worker XDV BOP offset exceeded the postamble limit; a replacement worker is being prepared");
    }
    slot.last_xdv_offset = current_size;

    int request_no = ++slot.next_request;
    int output_page_no = ++slot.xdv_page_count;
    fs::path out_dir = render_dir / "out" / "live";
    fs::create_directories(out_dir);
    fs::path cumulative_path = out_dir / "snippet-1-cumulative.xdv";
    fs::path final_path = out_dir / "snippet-1-final.xdv";
    fs::path pdf_path = out_dir / "snippet-1.pdf";
    fs::path svg_path = out_dir / "snippet-1.svg";
    write_file(cumulative_path, cumulative);

    int64_t finalize_start = now_ms();
    size_t final_bytes = finalize_xdv_body(cumulative, final_path, parts, output_page_no, last_bop);
    int64_t convert_start = now_ms();
    publish_status_and_counts_locked(STEMTEX_STATUS_RENDERING, STEMTEX_STAGE_CONVERTING);
    std::string page_range = std::to_string(output_page_no) + "-" + std::to_string(output_page_no);
    std::string page_number = std::to_string(output_page_no);
    std::string output_format = format == RenderFormat::Svg ? "svg" : "pdf";
    fs::path output_path = format == RenderFormat::Svg ? svg_path : pdf_path;
    std::string pdf_options = "-q -z 1 -C 64";
    std::string svg_options = "--bbox=papersize --exact-bbox --no-fonts";
    std::string converter_mode = "daemon-dll";
    int xdvipdfmx_return_code = 0;
    int xdvipdfmx_issue_flags = 0;
    std::string xdvipdfmx_issue_message;
    int dvisvgm_return_code = 0;
    std::string dvisvgm_error_message;
    try {
      if (format == RenderFormat::Svg) {
        auto convert_result = svg_converter->convert(final_path, svg_path, page_number);
        converter_mode = convert_result.mode;
        dvisvgm_return_code = convert_result.return_code;
        dvisvgm_error_message = convert_result.error_message;
      } else {
        auto convert_result = converter->convert(final_path, pdf_path, page_range);
        converter_mode = convert_result.mode;
        xdvipdfmx_return_code = convert_result.return_code;
        xdvipdfmx_issue_flags = convert_result.issue_flags;
        xdvipdfmx_issue_message = convert_result.issue_message;
      }
    } catch (const std::exception &e) {
      publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
      throw ApiException(format == RenderFormat::Svg ? STEMTEX_ERROR_DVISVGM : STEMTEX_ERROR_XDVIPDFMX, e.what());
    }
    int64_t end = now_ms();

    std::ostringstream summary;
    summary << "{"
            << "\"outputFormat\":\"" << output_format << "\","
            << "\"outputPath\":\"" << json_escape(output_path.string()) << "\","
            << "\"renderMode\":\"cpp-dll-live-worker-latest-page\","
            << "\"pdfMode\":\"cpp-dll-live-worker-latest-page\","
            << "\"svgMode\":\"cpp-dll-live-worker-latest-page\","
            << "\"backend\":\"" << (format == RenderFormat::Svg ? "dvisvgmdaemon" : "xdvipdfmxdaemon") << "\","
            << "\"converterMode\":\"" << json_escape(converter_mode) << "\","
            << "\"xdvipdfmxOptions\":\"" << json_escape(pdf_options) << "\","
            << "\"xdvipdfmxMode\":\"" << json_escape(format == RenderFormat::Svg ? "" : converter_mode) << "\","
            << "\"xdvipdfmxReturnCode\":" << xdvipdfmx_return_code << ","
            << "\"xdvipdfmxIssueFlags\":" << xdvipdfmx_issue_flags << ","
            << "\"xdvipdfmxIssueMessage\":\"" << json_escape(xdvipdfmx_issue_message) << "\","
            << "\"xdvipdfmxWarning\":" << (xdvipdfmx_issue_flags ? "true" : "false") << ","
            << "\"dvisvgmOptions\":\"" << json_escape(svg_options) << "\","
            << "\"dvisvgmMode\":\"" << json_escape(format == RenderFormat::Svg ? converter_mode : "") << "\","
            << "\"dvisvgmReturnCode\":" << dvisvgm_return_code << ","
            << "\"dvisvgmErrorMessage\":\"" << json_escape(dvisvgm_error_message) << "\","
            << "\"outcomeCode\":" << (xdvipdfmx_issue_flags ? STEMTEX_RENDER_OUTCOME_RECOVERABLE
                                                            : STEMTEX_RENDER_OUTCOME_OK) << ","
            << "\"issueFlags\":" << xdvipdfmx_issue_flags << ","
            << "\"outcomeMessage\":\"" << json_escape(xdvipdfmx_issue_message) << "\","
            << "\"widthPt\":" << format_decimal(resolved_width_pt) << ","
            << "\"fontSizePt\":" << format_decimal(resolved_font_size_pt) << ","
            << "\"requestToOutputMs\":" << (end - start) << ","
            << "\"requestToPdfMs\":" << (format == RenderFormat::Svg ? 0 : (end - start)) << ","
            << "\"requestToSvgMs\":" << (format == RenderFormat::Svg ? (end - start) : 0) << ","
            << "\"finalizeXdvMs\":" << (convert_start - finalize_start) << ","
            << "\"convertMs\":" << (end - convert_start) << ","
            << "\"xdvipdfmxMs\":" << (format == RenderFormat::Svg ? 0 : (end - convert_start)) << ","
            << "\"dvisvgmMs\":" << (format == RenderFormat::Svg ? (end - convert_start) : 0) << ","
            << "\"cumulativeXdvBytes\":" << cumulative.size() << ","
            << "\"newXdvBytes\":" << delta.size() << ","
            << "\"finalXdvBytes\":" << final_bytes << ","
            << "\"outputBytes\":" << fs::file_size(output_path) << ","
            << "\"pdfBytes\":" << (format == RenderFormat::Svg ? 0 : fs::file_size(pdf_path)) << ","
            << "\"svgBytes\":" << (format == RenderFormat::Svg ? fs::file_size(svg_path) : 0) << ","
            << "\"workerRequest\":" << request_no << ","
            << "\"workerPage\":" << output_page_no << ","
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

    RenderOutput result;
    result.request_id = id;
    result.output_path = output_path;
    result.output_format = output_format;
    result.summary_json = summary.str();
    result.outcome_code = xdvipdfmx_issue_flags ? STEMTEX_RENDER_OUTCOME_RECOVERABLE : STEMTEX_RENDER_OUTCOME_OK;
    result.issue_flags = xdvipdfmx_issue_flags;
    result.outcome_message = xdvipdfmx_issue_message;
    publish_status_and_counts_locked(STEMTEX_STATUS_READY, STEMTEX_STAGE_IDLE);
    set_last_error(STEMTEX_OK, "");
    set_last_outcome(result.outcome_code, result.issue_flags, result.outcome_message);
    return result;
  }

  StemTeXRenderResult render(const std::string &snippet, double width_pt, double font_size_pt = kDefaultFontSizePt) {
    RenderOutput output = render_output(snippet, width_pt, font_size_pt, RenderFormat::Pdf);
    StemTeXRenderResult result{};
    result.request_id_utf8 = alloc_c_string(output.request_id);
    result.pdf_path_utf8 = alloc_c_string(output.output_path.string());
    result.summary_json_utf8 = alloc_c_string(output.summary_json);
    result.outcome_code = output.outcome_code;
    result.issue_flags = output.issue_flags;
    result.outcome_message_utf8 = alloc_c_string(output.outcome_message);
    return result;
  }

  RendererConfig cfg;
  XdvParts parts;
  std::vector<wchar_t> worker_env;
  std::mutex render_mu;
  std::mutex control_mu;
  std::mutex diagnostic_mu;
  std::atomic<StemTeXRendererStatus> status{STEMTEX_STATUS_STARTING};
  std::atomic<StemTeXRenderStage> snapshot_stage{STEMTEX_STAGE_IDLE};
  std::atomic<int> snapshot_primary_ready{0};
  std::atomic<int> snapshot_spare_ready{0};
  std::atomic<int> snapshot_spare_target{0};
  std::atomic<int> snapshot_spare_rebuilding{0};
  std::atomic<int> snapshot_async_running{0};
  std::atomic<int> snapshot_async_pending{0};
  std::atomic<uint64_t> snapshot_running_job_id{0};
  std::atomic<uint64_t> snapshot_pending_job_id{0};
  StemTeXErrorCode last_error = STEMTEX_OK;
  StemTeXRenderOutcomeCode last_outcome = STEMTEX_RENDER_OUTCOME_OK;
  int last_issue_flags = 0;
  std::string last_outcome_message;
  std::string log_tail;
  std::unique_ptr<DvipdfmxDaemon> converter;
  std::unique_ptr<DvisvgmDaemon> svg_converter;
  WorkerSlot *active_slot = nullptr;
  bool cancel_requested = false;
  std::unique_ptr<WorkerSlot> primary;
  std::vector<std::unique_ptr<WorkerSlot>> spares;
  std::thread spare_builder;
  std::atomic<bool> spare_builder_finished{true};
  std::mutex async_mu;
  std::condition_variable async_cv;
  std::thread async_worker;
  std::optional<AsyncJob> async_pending;
  std::vector<AsyncJob> async_cancelled;
  bool async_stop = false;
  uint64_t next_async_job_id = 0;
  bool spare_rebuilding = false;
  std::atomic<bool> shutting_down{false};
  int next_spare_index = 0;
};

RenderFormat render_format_from_api(StemTeXOutputFormat format) {
  switch (format) {
    case STEMTEX_OUTPUT_PDF: return RenderFormat::Pdf;
    case STEMTEX_OUTPUT_SVG: return RenderFormat::Svg;
    default: throw ApiException(STEMTEX_ERROR_INVALID_ARGUMENT, "Unknown output format");
  }
}

StemTeXRenderOutputResult output_result_to_api(const RenderOutput &output) {
  StemTeXRenderOutputResult result{};
  result.request_id_utf8 = alloc_c_string(output.request_id);
  result.output_path_utf8 = alloc_c_string(output.output_path.string());
  result.output_format_utf8 = alloc_c_string(output.output_format);
  result.summary_json_utf8 = alloc_c_string(output.summary_json);
  result.outcome_code = output.outcome_code;
  result.issue_flags = output.issue_flags;
  result.outcome_message_utf8 = alloc_c_string(output.outcome_message);
  return result;
}

extern "C" {

STEMTEX_API StemTeXRenderer *stemtex_renderer_create(const StemTeXConfig *config, StemTeXErrorCode *error_code,
                                                     char **error_utf8) {
  try {
    RendererConfig cfg = config_from_api(config);
    write_fontconfig_config(cfg);
    validate_or_throw(cfg);
    fs::create_directories(cfg.state_root);
    fs::create_directories(cfg.renders_root);
    if (error_code) *error_code = STEMTEX_OK;
    return new StemTeXRenderer(std::move(cfg));
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return nullptr;
  } catch (...) {
    set_unknown_error_outputs(error_code, error_utf8);
    return nullptr;
  }
}

STEMTEX_API int stemtex_renderer_render(StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt,
                                        StemTeXRenderResult *result, StemTeXErrorCode *error_code,
                                        char **error_utf8) {
  return stemtex_renderer_render_with_font_size(renderer, snippet_utf8, width_pt, kDefaultFontSizePt, result,
                                                error_code, error_utf8);
}

STEMTEX_API int stemtex_renderer_render_with_font_size(StemTeXRenderer *renderer, const char *snippet_utf8,
                                                       double width_pt, double font_size_pt,
                                                       StemTeXRenderResult *result,
                                                       StemTeXErrorCode *error_code, char **error_utf8) {
  if (!renderer || !snippet_utf8 || !result) {
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT, 0, "Invalid argument");
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    *result = renderer->render(snippet_utf8, width_pt, font_size_pt);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    renderer->set_last_outcome(outcome_from_error(code), 0, e.what());
    renderer->update_status_after_api_exception(code);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    renderer->set_last_error(STEMTEX_ERROR_INTERNAL, kUnknownInternalException);
    renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, kUnknownInternalException);
    renderer->update_status_after_api_exception(STEMTEX_ERROR_INTERNAL);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_pdf_bytes(StemTeXRenderer *renderer, const char *snippet_utf8,
                                                  double width_pt, StemTeXPdfBytes *pdf,
                                                  StemTeXRenderResult *result, StemTeXErrorCode *error_code,
                                                  char **error_utf8) {
  return stemtex_renderer_render_pdf_bytes_with_font_size(renderer, snippet_utf8, width_pt, kDefaultFontSizePt, pdf,
                                                          result, error_code, error_utf8);
}

STEMTEX_API int stemtex_renderer_render_pdf_bytes_with_font_size(
    StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt, double font_size_pt, StemTeXPdfBytes *pdf,
    StemTeXRenderResult *result, StemTeXErrorCode *error_code, char **error_utf8) {
  if (!pdf) {
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT, 0, "Invalid argument");
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  pdf->data = nullptr;
  pdf->size = 0;
  StemTeXRenderResult local_result{};
  StemTeXRenderResult *target = result ? result : &local_result;
  if (!stemtex_renderer_render_with_font_size(renderer, snippet_utf8, width_pt, font_size_pt, target, error_code,
                                              error_utf8)) return 0;
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
    if (renderer) renderer->set_last_outcome(outcome_from_error(code), 0, e.what());
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    if (!result) stemtex_renderer_free_result(&local_result);
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, kUnknownInternalException);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_output(StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt,
                                               StemTeXOutputFormat format, StemTeXRenderOutputResult *result,
                                               StemTeXErrorCode *error_code, char **error_utf8) {
  return stemtex_renderer_render_output_with_font_size(renderer, snippet_utf8, width_pt, kDefaultFontSizePt, format,
                                                       result, error_code, error_utf8);
}

STEMTEX_API int stemtex_renderer_render_output_with_font_size(
    StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt, double font_size_pt,
    StemTeXOutputFormat format, StemTeXRenderOutputResult *result, StemTeXErrorCode *error_code, char **error_utf8) {
  if (!renderer || !snippet_utf8 || !result) {
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT, 0, "Invalid argument");
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    RenderOutput output = renderer->render_output(snippet_utf8, width_pt, font_size_pt, render_format_from_api(format));
    *result = output_result_to_api(output);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    renderer->set_last_outcome(outcome_from_error(code), 0, e.what());
    renderer->update_status_after_api_exception(code);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    renderer->set_last_error(STEMTEX_ERROR_INTERNAL, kUnknownInternalException);
    renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, kUnknownInternalException);
    renderer->update_status_after_api_exception(STEMTEX_ERROR_INTERNAL);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_output_bytes(StemTeXRenderer *renderer, const char *snippet_utf8,
                                                     double width_pt, StemTeXOutputFormat format,
                                                     StemTeXOutputBytes *bytes, StemTeXRenderOutputResult *result,
                                                     StemTeXErrorCode *error_code, char **error_utf8) {
  return stemtex_renderer_render_output_bytes_with_font_size(renderer, snippet_utf8, width_pt, kDefaultFontSizePt,
                                                             format, bytes, result, error_code, error_utf8);
}

STEMTEX_API int stemtex_renderer_render_output_bytes_with_font_size(
    StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt, double font_size_pt,
    StemTeXOutputFormat format, StemTeXOutputBytes *bytes, StemTeXRenderOutputResult *result,
    StemTeXErrorCode *error_code, char **error_utf8) {
  if (!bytes) {
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT, 0, "Invalid argument");
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  bytes->data = nullptr;
  bytes->size = 0;
  StemTeXRenderOutputResult local_result{};
  StemTeXRenderOutputResult *target = result ? result : &local_result;
  if (!stemtex_renderer_render_output_with_font_size(renderer, snippet_utf8, width_pt, font_size_pt, format, target,
                                                     error_code, error_utf8)) return 0;
  try {
    auto file_bytes = read_file(target->output_path_utf8);
    bytes->data = static_cast<unsigned char *>(CoTaskMemAlloc(file_bytes.size()));
    if (!bytes->data && !file_bytes.empty()) throw ApiException(STEMTEX_ERROR_INTERNAL, "Cannot allocate output bytes");
    if (!file_bytes.empty()) std::memcpy(bytes->data, file_bytes.data(), file_bytes.size());
    bytes->size = file_bytes.size();
    if (!result) stemtex_renderer_free_output_result(&local_result);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    if (!result) stemtex_renderer_free_output_result(&local_result);
    StemTeXErrorCode code = exception_code(e);
    if (renderer) renderer->set_last_outcome(outcome_from_error(code), 0, e.what());
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    if (!result) stemtex_renderer_free_output_result(&local_result);
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, kUnknownInternalException);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_render_async(StemTeXRenderer *renderer, const char *snippet_utf8, double width_pt,
                                              uint64_t *job_id, StemTeXRenderCallback callback, void *user_data,
                                              StemTeXErrorCode *error_code, char **error_utf8) {
  return stemtex_renderer_render_async_with_font_size(renderer, snippet_utf8, width_pt, kDefaultFontSizePt, job_id,
                                                      callback, user_data, error_code, error_utf8);
}

STEMTEX_API int stemtex_renderer_render_async_with_font_size(StemTeXRenderer *renderer, const char *snippet_utf8,
                                                             double width_pt, double font_size_pt, uint64_t *job_id,
                                                             StemTeXRenderCallback callback, void *user_data,
                                                             StemTeXErrorCode *error_code, char **error_utf8) {
  if (!renderer || !snippet_utf8 || !callback) {
    if (renderer) renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT, 0, "Invalid argument");
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    renderer->submit_async(snippet_utf8, width_pt, font_size_pt, job_id, callback, user_data);
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    renderer->set_last_outcome(outcome_from_error(code), 0, e.what());
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    renderer->set_last_error(STEMTEX_ERROR_INTERNAL, kUnknownInternalException);
    renderer->set_last_outcome(STEMTEX_RENDER_OUTCOME_INTERNAL, 0, kUnknownInternalException);
    set_unknown_error_outputs(error_code, error_utf8);
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
    renderer->publish_status(STEMTEX_STATUS_DEAD, STEMTEX_STAGE_STOPPING);
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    renderer->set_last_error(STEMTEX_ERROR_INTERNAL, kUnknownInternalException);
    renderer->publish_status(STEMTEX_STATUS_DEAD, STEMTEX_STAGE_STOPPING);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_renderer_cancel_current(StemTeXRenderer *renderer, StemTeXErrorCode *error_code,
                                                char **error_utf8) {
  if (!renderer) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "Invalid argument", error_code, error_utf8);
    return 0;
  }
  try {
    renderer->cancel_current();
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    StemTeXErrorCode code = exception_code(e);
    renderer->set_last_error(code, e.what());
    set_error_outputs(code, e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    renderer->set_last_error(STEMTEX_ERROR_INTERNAL, kUnknownInternalException);
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API StemTeXRendererStatus stemtex_renderer_status(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return STEMTEX_STATUS_DEAD;
    return renderer->status.load();
  } catch (...) {
    return STEMTEX_STATUS_DEAD;
  }
}

STEMTEX_API int stemtex_renderer_engine_snapshot(StemTeXRenderer *renderer, StemTeXEngineSnapshot *snapshot) {
  try {
    if (!renderer || !snapshot) return 0;
    snapshot->status = renderer->status.load();
    snapshot->stage = renderer->snapshot_stage.load();
    snapshot->last_error = STEMTEX_OK;
    snapshot->primary_ready = renderer->snapshot_primary_ready.load();
    snapshot->spare_ready = renderer->snapshot_spare_ready.load();
    snapshot->spare_target = renderer->snapshot_spare_target.load();
    snapshot->spare_rebuilding = renderer->snapshot_spare_rebuilding.load();
    snapshot->async_running = renderer->snapshot_async_running.load();
    snapshot->async_pending = renderer->snapshot_async_pending.load();
    snapshot->running_job_id = renderer->snapshot_running_job_id.load();
    snapshot->pending_job_id = renderer->snapshot_pending_job_id.load();
    {
      std::lock_guard<std::mutex> lock(renderer->diagnostic_mu);
      snapshot->last_error = renderer->last_error;
    }
    return 1;
  } catch (...) {
    return 0;
  }
}

STEMTEX_API StemTeXErrorCode stemtex_renderer_last_error_code(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return STEMTEX_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(renderer->diagnostic_mu);
    return renderer->last_error;
  } catch (...) {
    return STEMTEX_ERROR_INTERNAL;
  }
}

STEMTEX_API StemTeXRenderOutcomeCode stemtex_renderer_last_outcome_code(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT;
    return renderer->get_last_outcome_code();
  } catch (...) {
    return STEMTEX_RENDER_OUTCOME_INTERNAL;
  }
}

STEMTEX_API int stemtex_renderer_last_issue_flags(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return 0;
    return renderer->get_last_issue_flags();
  } catch (...) {
    return 0;
  }
}

STEMTEX_API char *stemtex_renderer_last_outcome_message(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return alloc_c_string("Invalid argument");
    return alloc_c_string(renderer->get_last_outcome_message());
  } catch (...) {
    return alloc_c_string(kUnknownInternalException);
  }
}

STEMTEX_API char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes) {
  try {
    if (!renderer) return alloc_c_string("");
    return alloc_c_string(renderer->get_log_tail(max_bytes));
  } catch (...) {
    return alloc_c_string("");
  }
}

STEMTEX_API const char *stemtex_renderer_version(void) {
  return kRendererVersion;
}

STEMTEX_API const char *stemtex_renderer_abi_version(void) {
  return kRendererAbiVersion;
}

STEMTEX_API char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer) {
  try {
    if (!renderer) return alloc_c_string("");
    fs::path version = renderer->cfg.runtime_root / "VERSION";
    if (fs::exists(version)) return alloc_c_string(read_text_file(version));
    return alloc_c_string(renderer->cfg.runtime_root.string());
  } catch (...) {
    return alloc_c_string("");
  }
}

STEMTEX_API char *stemtex_renderer_profile_info_json(const char *profile_root_utf8, StemTeXErrorCode *error_code,
                                                     char **error_utf8) {
  if (!profile_root_utf8 || !*profile_root_utf8) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "profile_root_utf8 is required", error_code, error_utf8);
    return nullptr;
  }
  try {
    fs::path profile = fs::absolute(profile_root_utf8).lexically_normal();
    bool is_dir = fs::exists(profile) && fs::is_directory(profile);
    bool has_preamble = fs::exists(profile / "preamble.tex");
    bool has_warmup = fs::exists(profile / "warmup.tex");
    bool has_warmup_xdv = fs::exists(profile / "warmup.xdv");
    std::ostringstream json;
    json << "{\"name\":\"" << json_escape(path_utf8(profile.filename())) << "\","
         << "\"path\":\"" << json_escape(path_utf8(profile)) << "\","
         << "\"valid\":" << (is_dir && has_preamble && has_warmup ? "true" : "false") << ","
         << "\"hasPreamble\":" << (has_preamble ? "true" : "false") << ","
         << "\"hasWarmup\":" << (has_warmup ? "true" : "false") << ","
         << "\"hasWarmupXdv\":" << (has_warmup_xdv ? "true" : "false") << "}";
    if (error_code) *error_code = STEMTEX_OK;
    if (error_utf8) *error_utf8 = nullptr;
    return alloc_c_string(json.str());
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return nullptr;
  } catch (...) {
    set_unknown_error_outputs(error_code, error_utf8);
    return nullptr;
  }
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
  } catch (...) {
    set_unknown_error_outputs(error_code, diagnostics_utf8);
    return 0;
  }
}

STEMTEX_API int stemtex_refresh_font_cache(const char *runtime_root_utf8, const char *profile_root_utf8,
                                           StemTeXErrorCode *error_code, char **error_utf8) {
  if (!runtime_root_utf8 || !*runtime_root_utf8) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "runtime_root_utf8 is required", error_code, error_utf8);
    return 0;
  }
  if (!profile_root_utf8 || !*profile_root_utf8) {
    set_error_outputs(STEMTEX_ERROR_INVALID_ARGUMENT, "profile_root_utf8 is required", error_code, error_utf8);
    return 0;
  }
  try {
    RendererConfig cfg;
    cfg.runtime_root = fs::absolute(runtime_root_utf8);
    cfg.texmf_root = cfg.runtime_root;
    cfg.profile_root = fs::absolute(profile_root_utf8);
    cfg.repo_root = cfg.profile_root;
    fs::path stemtex_user_root = local_app_data_root() / "StemTeX";
    fs::path refresh_root = stemtex_user_root / "refresh";
    cfg.state_root = refresh_root / "state" / ("instance-" + random_id());
    cfg.renders_root = refresh_root / "renders";
    cfg.fontconfig_conf_root = cfg.state_root / "fontconfig" / "conf";
    cfg.fontconfig_cache_root = stemtex_user_root / "fontconfig" / "cache";
    cfg.warmup_tex = cfg.profile_root / "warmup.tex";
    cfg.preamble_tex = cfg.profile_root / "preamble.tex";
    cfg.request_timeout_ms = 90000;
    cfg.xdvipdfmx_timeout_ms = 90000;
    write_fontconfig_config(cfg);
    if (!fs::exists(cfg.warmup_tex)) throw ApiException(STEMTEX_ERROR_BAD_CONFIG, "Warmup tex missing: " + cfg.warmup_tex.string());
    fs::path output_dir = cfg.profile_root;
    fs::create_directories(output_dir);
    auto env = worker_environment(cfg);
    fs::path exe = xetexdaemon_exe(cfg);
    std::ostringstream cmd;
    cmd << quote_cmd_arg(exe.string()) << " -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error"
        << " -output-directory=" << quote_cmd_arg(output_dir.string()) << " " << quote_cmd_arg(cfg.warmup_tex.string());
    run_sync(hosted_xetex_command(cfg, cmd.str()), cfg.warmup_tex.parent_path(), env, 90000, true);
    fs::path warmup_xdv = output_dir / (cfg.warmup_tex.stem().string() + ".xdv");
    if (!fs::exists(warmup_xdv)) throw ApiException(STEMTEX_ERROR_FILESYSTEM, "Warmup XDV was not written: " + warmup_xdv.string());
    if (error_code) *error_code = STEMTEX_OK;
    return 1;
  } catch (const std::exception &e) {
    set_error_outputs(exception_code(e), e.what(), error_code, error_utf8);
    return 0;
  } catch (...) {
    set_unknown_error_outputs(error_code, error_utf8);
    return 0;
  }
}

STEMTEX_API void stemtex_renderer_free_result(StemTeXRenderResult *result) {
  if (!result) return;
  stemtex_renderer_free_string(result->request_id_utf8);
  stemtex_renderer_free_string(result->pdf_path_utf8);
  stemtex_renderer_free_string(result->summary_json_utf8);
  stemtex_renderer_free_string(result->outcome_message_utf8);
  result->request_id_utf8 = nullptr;
  result->pdf_path_utf8 = nullptr;
  result->summary_json_utf8 = nullptr;
  result->outcome_code = STEMTEX_RENDER_OUTCOME_OK;
  result->issue_flags = 0;
  result->outcome_message_utf8 = nullptr;
}

STEMTEX_API void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf) {
  if (!pdf) return;
  if (pdf->data) CoTaskMemFree(pdf->data);
  pdf->data = nullptr;
  pdf->size = 0;
}

STEMTEX_API void stemtex_renderer_free_output_result(StemTeXRenderOutputResult *result) {
  if (!result) return;
  stemtex_renderer_free_string(result->request_id_utf8);
  stemtex_renderer_free_string(result->output_path_utf8);
  stemtex_renderer_free_string(result->output_format_utf8);
  stemtex_renderer_free_string(result->summary_json_utf8);
  stemtex_renderer_free_string(result->outcome_message_utf8);
  result->request_id_utf8 = nullptr;
  result->output_path_utf8 = nullptr;
  result->output_format_utf8 = nullptr;
  result->summary_json_utf8 = nullptr;
  result->outcome_code = STEMTEX_RENDER_OUTCOME_OK;
  result->issue_flags = 0;
  result->outcome_message_utf8 = nullptr;
}

STEMTEX_API void stemtex_renderer_free_output_bytes(StemTeXOutputBytes *bytes) {
  if (!bytes) return;
  if (bytes->data) CoTaskMemFree(bytes->data);
  bytes->data = nullptr;
  bytes->size = 0;
}

STEMTEX_API void stemtex_renderer_free_string(char *value) {
  if (value) CoTaskMemFree(value);
}

STEMTEX_API void stemtex_renderer_destroy(StemTeXRenderer *renderer) {
  try {
    delete renderer;
  } catch (...) {
  }
}

}  // extern "C"
