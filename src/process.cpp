#include "acp/process.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace acp {
    namespace {
        std::wstring toWide(const std::string& value) {
            if (value.empty()) {
                return {};
            }
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                 static_cast<int>(value.size()), nullptr, 0);
            if (size <= 0) {
                return {};
            }
            std::wstring out(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), out.data(), size);
            return out;
        }

        std::wstring quoteArg(const std::wstring& value) {
            if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
                return value;
            }
            std::wstring out = L"\"";
            size_t backslashes = 0;
            for (wchar_t c : value) {
                if (c == L'\\') {
                    ++backslashes;
                } else if (c == L'\"') {
                    out.append(backslashes * 2 + 1, L'\\');
                    out += c;
                    backslashes = 0;
                } else {
                    out.append(backslashes, L'\\');
                    out += c;
                    backslashes = 0;
                }
            }
            out.append(backslashes * 2, L'\\');
            out += L'\"';
            return out;
        }

        std::string lastError() {
            return "Windows error " + std::to_string(GetLastError());
        }
    } // namespace

    RunResult run(const std::vector<std::string>& argv) {
        RunResult r;
        if (argv.empty()) {
            r.error = "empty command";
            return r;
        }

        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE childOut = nullptr;
        HANDLE parentOut = nullptr;
        HANDLE childIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable, OPEN_EXISTING, 0, nullptr);
        if (childIn == INVALID_HANDLE_VALUE) {
            r.error = lastError();
            return r;
        }
        if (!CreatePipe(&parentOut, &childOut, &inheritable, 0) ||
            !SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0)) {
            r.error = lastError();
            CloseHandle(childIn);
            if (childOut) {
                CloseHandle(childOut);
            }
            if (parentOut) {
                CloseHandle(parentOut);
            }
            return r;
        }

        std::wstring commandLine;
        for (const auto& arg : argv) {
            if (!commandLine.empty()) {
                commandLine += L' ';
            }
            const std::wstring wide = toWide(arg);
            if (wide.empty() && !arg.empty()) {
                CloseHandle(childIn);
                CloseHandle(childOut);
                CloseHandle(parentOut);
                r.error = "command contains invalid UTF-8";
                return r;
            }
            commandLine += quoteArg(wide);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childIn;
        startup.hStdOutput = childOut;
        startup.hStdError = childOut;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                            nullptr, &startup, &process)) {
            r.error = lastError();
            CloseHandle(childIn);
            CloseHandle(childOut);
            CloseHandle(parentOut);
            return r;
        }
        CloseHandle(childIn);
        CloseHandle(childOut);
        CloseHandle(process.hThread);

        char chunk[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(parentOut, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
                break;
            }
            r.output.append(chunk, static_cast<size_t>(read));
        }
        CloseHandle(parentOut);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 0;
        if (GetExitCodeProcess(process.hProcess, &exitCode)) {
            r.ok = true;
            r.exitCode = static_cast<int>(exitCode);
        } else {
            r.error = lastError();
        }
        CloseHandle(process.hProcess);
        return r;
    }
} // namespace acp

#else

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace acp {

    RunResult run(const std::vector<std::string>& argv) {
        RunResult result;
        if (argv.empty()) {
            result.error = "empty command";
            return result;
        }

        int out[2];
        if (pipe(out) != 0) {
            result.error = std::strerror(errno);
            return result;
        }

        std::vector<const char*> cargv;
        for (const auto& a : argv) {
            cargv.push_back(a.c_str());
        }
        cargv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, out[0]);

        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr,
                                    const_cast<char* const*>(cargv.data()), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        if (rc != 0) {
            close(out[0]);
            result.error = std::strerror(rc);
            return result;
        }

        char chunk[4096];
        for (;;) {
            const ssize_t n = read(out[0], chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            result.output.append(chunk, static_cast<size_t>(n));
        }
        close(out[0]);

        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        result.ok = WIFEXITED(status);
        result.exitCode = result.ok ? WEXITSTATUS(status) : -1;
        return result;
    }

} // namespace acp

#endif
