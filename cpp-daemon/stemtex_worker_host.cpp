#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>

namespace {

constexpr const wchar_t *kLifetimeEnv = L"STEMTEX_WORKER_HOST_LIFETIME_HANDLE";

std::wstring quote_cmd_arg(const std::wstring &arg) {
  if (arg.empty()) return L"\"\"";
  bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) return arg;

  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
    } else if (ch == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(ch);
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(ch);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring join_command_line(int argc, wchar_t **argv, int first) {
  std::wstring cmd;
  for (int i = first; i < argc; ++i) {
    if (!cmd.empty()) cmd.push_back(L' ');
    cmd += quote_cmd_arg(argv[i] ? argv[i] : L"");
  }
  return cmd;
}

void print_win_error(const wchar_t *context, DWORD err) {
  std::fwprintf(stderr, L"stemtex-worker-host: %ls failed: %lu\n", context, err);
}

HANDLE parse_lifetime_handle() {
  wchar_t buf[64]{};
  DWORD n = GetEnvironmentVariableW(kLifetimeEnv, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return nullptr;
  wchar_t *end = nullptr;
  unsigned long long value = std::wcstoull(buf, &end, 10);
  if (!end || *end != L'\0' || value == 0) return nullptr;
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

HANDLE duplicate_inheritable(HANDLE h, const wchar_t *label) {
  if (!h || h == INVALID_HANDLE_VALUE) return h;
  HANDLE dup = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    print_win_error(label, GetLastError());
    return h;
  }
  return dup;
}

void close_if_duplicated(HANDLE original, HANDLE duplicated) {
  if (duplicated && duplicated != INVALID_HANDLE_VALUE && duplicated != original) CloseHandle(duplicated);
}

void lifetime_watch(HANDLE lifetime, HANDLE child_process) {
  char byte = 0;
  DWORD read = 0;
  ReadFile(lifetime, &byte, 1, &read, nullptr);

  DWORD code = 0;
  if (GetExitCodeProcess(child_process, &code) && code == STILL_ACTIVE) {
    TerminateProcess(child_process, 1);
  }
  CloseHandle(child_process);
  CloseHandle(lifetime);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
  int split = -1;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::wcscmp(argv[i], L"--") == 0) {
      split = i;
      break;
    }
  }
  if (split < 0 || split + 1 >= argc) {
    std::fwprintf(stderr, L"Usage: stemtex-worker-host -- xetexdaemon.exe [args...]\n");
    return 2;
  }

  std::wstring child_cmd = join_command_line(argc, argv, split + 1);
  HANDLE lifetime = parse_lifetime_handle();

  HANDLE stdin_original = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE stdout_original = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE stderr_original = GetStdHandle(STD_ERROR_HANDLE);
  HANDLE stdin_child = duplicate_inheritable(stdin_original, L"DuplicateHandle(stdin)");
  HANDLE stdout_child = duplicate_inheritable(stdout_original, L"DuplicateHandle(stdout)");
  HANDLE stderr_child = duplicate_inheritable(stderr_original, L"DuplicateHandle(stderr)");

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_child;
  si.hStdOutput = stdout_child;
  si.hStdError = stderr_child;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, child_cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
  DWORD create_err = ok ? 0 : GetLastError();

  close_if_duplicated(stdin_original, stdin_child);
  close_if_duplicated(stdout_original, stdout_child);
  close_if_duplicated(stderr_original, stderr_child);

  if (!ok) {
    if (lifetime) CloseHandle(lifetime);
    print_win_error(L"CreateProcessW(xetexdaemon)", create_err);
    return 127;
  }

  if (lifetime) {
    HANDLE child_for_watch = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &child_for_watch, 0, FALSE,
                        DUPLICATE_SAME_ACCESS)) {
      std::thread(lifetime_watch, lifetime, child_for_watch).detach();
    } else {
      CloseHandle(lifetime);
      print_win_error(L"DuplicateHandle(child)", GetLastError());
    }
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(exit_code);
}
