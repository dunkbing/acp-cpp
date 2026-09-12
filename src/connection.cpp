#include "acp/connection.hpp"
#include "acp/log.hpp"

#include <algorithm>

namespace acp {

    namespace {
        Response closedResponse(const std::string& reason) {
            Response r;
            r.error = RpcError{-32000, reason, json()};
            return r;
        }
    } // namespace

    // ---------------------------------------------------------------- shared

    Connection::~Connection() {
        stop();
    }

    bool Connection::isRunning() const {
        return pid_ > 0 && !exited_;
    }

    std::future<Response> Connection::request(const std::string& method, json params,
                                              ResponseCallback callback) {
        std::promise<Response> promise;
        std::future<Response> future = promise.get_future();
        long long id = 0;
        {
            std::lock_guard lock(pendingMutex_);
            id = nextId_++;
            pending_[id] = Pending{std::move(promise), std::move(callback)};
        }
        if (!isRunning()) {
            failPending("agent is not running");
            return future;
        }
        writeLine(
            json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
        return future;
    }

    void Connection::notify(const std::string& method, json params) {
        writeLine(json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump());
    }

    void Connection::respond(const json& id, json result) {
        writeLine(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump());
    }

    void Connection::respondError(const json& id, int code, const std::string& message) {
        writeLine(
            json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}
                .dump());
    }

    std::future<Response> Connection::initialize(const ClientCapabilities& capabilities,
                                                 int protocolVersion, ResponseCallback callback) {
        return request(
            "initialize",
            {{"protocolVersion", protocolVersion}, {"clientCapabilities", capabilities.toJson()}},
            std::move(callback));
    }

    std::future<Response> Connection::authenticate(const std::string& methodId,
                                                   ResponseCallback callback) {
        return request("authenticate", {{"methodId", methodId}}, std::move(callback));
    }

    std::future<Response> Connection::newSession(const std::string& cwd, const json& mcpServers,
                                                 ResponseCallback callback) {
        return request("session/new", {{"cwd", cwd}, {"mcpServers", mcpServers}},
                       std::move(callback));
    }

    std::future<Response> Connection::loadSession(const std::string& sessionId,
                                                  const std::string& cwd, const json& mcpServers,
                                                  ResponseCallback callback) {
        return request("session/load",
                       {{"sessionId", sessionId}, {"cwd", cwd}, {"mcpServers", mcpServers}},
                       std::move(callback));
    }

    std::future<Response> Connection::prompt(const std::string& sessionId,
                                             const json& contentBlocks, ResponseCallback callback) {
        return request("session/prompt", {{"sessionId", sessionId}, {"prompt", contentBlocks}},
                       std::move(callback));
    }

    std::future<Response> Connection::setSessionMode(const std::string& sessionId,
                                                     const std::string& modeId,
                                                     ResponseCallback callback) {
        return request("session/set_mode", {{"sessionId", sessionId}, {"modeId", modeId}},
                       std::move(callback));
    }

    std::future<Response> Connection::setSessionConfigOption(const std::string& sessionId,
                                                              const std::string& configId, json value,
                                                              ResponseCallback callback) {
        return request("session/set_config_option",
                       {{"sessionId", sessionId}, {"configId", configId}, {"value", std::move(value)}},
                       std::move(callback));
    }

    void Connection::cancel(const std::string& sessionId) {
        notify("session/cancel", {{"sessionId", sessionId}});
    }

    void Connection::respondPermission(const json& rpcId, const std::string& optionId) {
        const json outcome = optionId.empty()
                                 ? json{{"outcome", "cancelled"}}
                                 : json{{"outcome", "selected"}, {"optionId", optionId}};
        respond(rpcId, {{"outcome", outcome}});
    }

    void Connection::failPending(const std::string& reason) {
        std::map<long long, Pending> pending;
        {
            std::lock_guard lock(pendingMutex_);
            pending.swap(pending_);
        }
        for (auto& [id, p] : pending) {
            Response r = closedResponse(reason);
            if (p.callback) {
                p.callback(r);
            }
            p.promise.set_value(std::move(r));
        }
    }

    void Connection::handleMessage(const json& msg) {
        const bool hasMethod = msg.contains("method");
        const bool hasId = msg.contains("id");
        if (hasMethod && hasId) {
            handleRequest(msg);
        } else if (hasMethod) {
            const std::string method = msg["method"];
            const json params = msg.value("params", json::object());
            if (method == "session/update") {
                SessionNotification n;
                n.sessionId = getString(params, "sessionId");
                n.update = SessionUpdate::fromJson(params.value("update", json::object()));
                client_->sessionUpdate(n);
            } else {
                client_->extNotification(method, params);
            }
        } else if (hasId) {
            handleResponse(msg);
        }
    }

    void Connection::handleRequest(const json& msg) {
        const std::string method = msg["method"];
        const json params = msg.value("params", json::object());
        const json& id = msg["id"];

        if (method == "session/request_permission") {
            PermissionRequest req;
            req.rpcId = id;
            req.sessionId = getString(params, "sessionId");
            const json tc = params.value("toolCall", json::object());
            req.toolCall.id = getString(tc, "toolCallId");
            req.toolCall.title = getString(tc, "title");
            req.toolCall.kind = getString(tc, "kind");
            req.toolCall.status = getString(tc, "status");
            req.toolCall.content = tc.value("content", json::array());
            for (const auto& opt : params.value("options", json::array())) {
                req.options.push_back(
                    {getString(opt, "optionId"), getString(opt, "name"), getString(opt, "kind")});
            }
            client_->requestPermission(req);
            return;
        }

        std::optional<Response> reply;
        if (method == "fs/read_text_file") {
            reply = client_->readTextFile(params);
        } else if (method == "fs/write_text_file") {
            reply = client_->writeTextFile(params);
        } else {
            reply = client_->extRequest(method, params);
        }
        if (!reply) {
            respondError(id, -32601, "Method not supported: " + method);
        } else if (reply->error) {
            respondError(id, reply->error->code, reply->error->message);
        } else {
            respond(id, reply->result);
        }
    }

    void Connection::handleResponse(const json& msg) {
        Pending pending;
        bool found = false;
        {
            std::lock_guard lock(pendingMutex_);
            if (msg["id"].is_number_integer()) {
                auto it = pending_.find(msg["id"].get<long long>());
                if (it != pending_.end()) {
                    pending = std::move(it->second);
                    pending_.erase(it);
                    found = true;
                }
            }
        }
        if (!found) {
            log(LogLevel::Warn, "response for unknown request id: " + msg["id"].dump());
            return;
        }
        Response r;
        if (msg.contains("error")) {
            const json& err = msg["error"];
            r.error = RpcError{getInt(err, "code"),
                               (getString(err, "message").empty() ? std::string("Agent error")
                                                                  : getString(err, "message")),
                               err.value("data", json())};
        } else {
            r.result = msg.value("result", json::object());
        }
        if (pending.callback) {
            pending.callback(r);
        }
        pending.promise.set_value(std::move(r));
    }

} // namespace acp

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

        std::string lastError(const std::string& what) {
            const DWORD code = GetLastError();
            LPWSTR buffer = nullptr;
            const DWORD size = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
            std::string detail;
            if (size > 0 && buffer) {
                const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size),
                                                         nullptr, 0, nullptr, nullptr);
                detail.resize(static_cast<size_t>(utf8Size));
                if (utf8Size > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), detail.data(),
                                        utf8Size, nullptr, nullptr);
                }
                LocalFree(buffer);
                while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n')) {
                    detail.pop_back();
                }
            }
            return what + " (" + std::to_string(code) + ")" +
                   (detail.empty() ? "" : ": " + detail);
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

        bool isBatchFile(const std::wstring& path) {
            if (path.size() < 4) {
                return false;
            }
            const std::wstring extension = path.substr(path.size() - 4);
            return CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".cmd", 4,
                                        TRUE) == CSTR_EQUAL ||
                   CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".bat", 4,
                                        TRUE) == CSTR_EQUAL;
        }

        std::wstring commandInterpreter() {
            wchar_t value[32768];
            const DWORD size = GetEnvironmentVariableW(L"ComSpec", value, 32768);
            return size > 0 && size < 32768 ? std::wstring(value, size) : L"cmd.exe";
        }

        std::wstring environmentName(const std::wstring& entry) {
            const size_t first = entry.find(L'=');
            const size_t equals = first == 0 ? entry.find(L'=', 1) : first;
            return equals == std::wstring::npos ? entry : entry.substr(0, equals);
        }

        bool sameName(const std::wstring& left, const std::wstring& right) {
            return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                        static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
        }

        std::vector<wchar_t> buildEnvironment(const SpawnOptions& options) {
            std::vector<std::pair<std::wstring, std::wstring>> overrides;
            overrides.reserve(options.env.size());
            for (const auto& [name, value] : options.env) {
                overrides.emplace_back(toWide(name), toWide(value));
            }
            std::vector<std::wstring> dropped;
            dropped.reserve(options.dropEnv.size());
            for (const auto& name : options.dropEnv) {
                dropped.push_back(toWide(name));
            }

            std::vector<std::wstring> entries;
            if (LPWCH inherited = GetEnvironmentStringsW()) {
                for (const wchar_t* p = inherited; *p;) {
                    std::wstring entry(p);
                    p += entry.size() + 1;
                    const std::wstring name = environmentName(entry);
                    const bool overridden = std::any_of(
                        overrides.begin(), overrides.end(), [&name](const auto& item) {
                            return sameName(item.first, name);
                        });
                    const bool removed = std::any_of(dropped.begin(), dropped.end(), [&name](const auto& item) {
                        return sameName(item, name);
                    });
                    if (!overridden && !removed) {
                        entries.push_back(std::move(entry));
                    }
                }
                FreeEnvironmentStringsW(inherited);
            }
            for (const auto& [name, value] : overrides) {
                entries.push_back(name + L"=" + value);
            }
            std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
                return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                            static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
            });
            std::vector<wchar_t> block;
            for (const auto& entry : entries) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }

        void closeHandle(HANDLE& handle) {
            if (handle) {
                CloseHandle(handle);
                handle = nullptr;
            }
        }

        template <typename OnLine>
        void pumpLines(HANDLE handle, const std::atomic<bool>& stopping, OnLine onLine) {
            std::string buffer;
            char chunk[8192];
            while (!stopping) {
                DWORD read = 0;
                if (!ReadFile(handle, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
                    break;
                }
                buffer.append(chunk, static_cast<size_t>(read));
                size_t pos = 0;
                while (true) {
                    const auto nl = buffer.find('\n', pos);
                    if (nl == std::string::npos) {
                        break;
                    }
                    std::string line = buffer.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (!line.empty()) {
                        onLine(line);
                    }
                }
                buffer.erase(0, pos);
            }
        }
    } // namespace

    std::pair<std::unique_ptr<Connection>, std::string>
    Connection::spawn(Client& client, const std::vector<std::string>& argv,
                      const SpawnOptions& options) {
        if (argv.empty()) {
            return {nullptr, "No agent command configured"};
        }

        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE childStdin = nullptr, childStdout = nullptr, childStderr = nullptr;
        HANDLE parentStdin = nullptr, parentStdout = nullptr, parentStderr = nullptr;
        const auto cleanup = [&] {
            closeHandle(childStdin);
            closeHandle(childStdout);
            closeHandle(childStderr);
            closeHandle(parentStdin);
            closeHandle(parentStdout);
            closeHandle(parentStderr);
        };
        if (!CreatePipe(&childStdin, &parentStdin, &inheritable, 0) ||
            !CreatePipe(&parentStdout, &childStdout, &inheritable, 0) ||
            !CreatePipe(&parentStderr, &childStderr, &inheritable, 0) ||
            !SetHandleInformation(parentStdin, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parentStdout, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parentStderr, HANDLE_FLAG_INHERIT, 0)) {
            const std::string error = lastError("Failed to create agent pipes");
            cleanup();
            return {nullptr, error};
        }

        std::vector<std::wstring> wideArgs;
        wideArgs.reserve(argv.size());
        for (const auto& arg : argv) {
            const std::wstring wide = toWide(arg);
            if (wide.empty() && !arg.empty()) {
                cleanup();
                return {nullptr, "Agent command contains invalid UTF-8"};
            }
            wideArgs.push_back(wide);
        }
        std::wstring commandLine;
        std::wstring application;
        if (isBatchFile(wideArgs.front())) {
            application = commandInterpreter();
            commandLine = quoteArg(application) + L" /d /s /c \"";
            bool first = true;
            for (const auto& arg : wideArgs) {
                if (!first) {
                    commandLine += L' ';
                }
                commandLine += quoteArg(arg);
                first = false;
            }
            commandLine += L'\"';
        } else {
            for (const auto& arg : wideArgs) {
                if (!commandLine.empty()) {
                    commandLine += L' ';
                }
                commandLine += quoteArg(arg);
            }
        }
        const std::wstring cwd = toWide(options.cwd);
        if (cwd.empty() && !options.cwd.empty()) {
            cleanup();
            return {nullptr, "Agent working directory contains invalid UTF-8"};
        }
        std::vector<wchar_t> environment = buildEnvironment(options);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childStdin;
        startup.hStdOutput = childStdout;
        startup.hStdError = childStderr;
        PROCESS_INFORMATION process{};
        DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT;
        if (options.newProcessGroup) {
            creationFlags |= CREATE_NEW_PROCESS_GROUP;
        }
        if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), commandLine.data(), nullptr,
                            nullptr, TRUE, creationFlags,
                            environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup,
                            &process)) {
            const std::string error = lastError("Failed to launch agent");
            cleanup();
            return {nullptr, error};
        }
        closeHandle(childStdin);
        closeHandle(childStdout);
        closeHandle(childStderr);
        CloseHandle(process.hThread);

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                             sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess)) {
            const std::string error = lastError("Failed to create agent job");
            if (job) {
                CloseHandle(job);
            }
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
            cleanup();
            return {nullptr, error};
        }

        std::unique_ptr<Connection> conn(new Connection());
        conn->client_ = &client;
        conn->pid_ = static_cast<long long>(process.dwProcessId);
        conn->processHandle_ = process.hProcess;
        conn->jobHandle_ = job;
        conn->stdinHandle_ = parentStdin;
        conn->stdoutHandle_ = parentStdout;
        conn->stderrHandle_ = parentStderr;
        Connection* raw = conn.get();
        conn->reader_ = std::thread([raw] { raw->readerLoop(); });
        conn->errReader_ = std::thread([raw] { raw->stderrLoop(); });
        log(LogLevel::Info, "agent spawned (pid " + std::to_string(conn->pid_) + "): " + argv[0]);
        return {std::move(conn), ""};
    }

    void Connection::stop() {
        stopping_ = true;
        if (jobHandle_) {
            TerminateJobObject(jobHandle_, 1);
        } else if (processHandle_) {
            TerminateProcess(processHandle_, 1);
        }
        if (processHandle_) {
            WaitForSingleObject(processHandle_, 1000);
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        if (errReader_.joinable()) {
            errReader_.join();
        }
        {
            std::lock_guard lock(writeMutex_);
            closeHandle(stdinHandle_);
        }
        closeHandle(stdoutHandle_);
        closeHandle(stderrHandle_);
        closeHandle(processHandle_);
        closeHandle(jobHandle_);
        pid_ = -1;
        failPending("connection closed");
    }

    void Connection::writeLine(const std::string& line) {
        const std::string data = line + "\n";
        std::lock_guard lock(writeMutex_);
        if (!stdinHandle_) {
            return;
        }
        size_t written = 0;
        while (written < data.size()) {
            DWORD count = 0;
            const DWORD requested = static_cast<DWORD>(
                std::min<size_t>(data.size() - written, static_cast<size_t>(MAXDWORD)));
            if (!WriteFile(stdinHandle_, data.data() + written, requested, &count, nullptr)) {
                log(LogLevel::Warn, lastError("write to agent failed"));
                return;
            }
            if (count == 0) {
                log(LogLevel::Warn, "write to agent made no progress");
                return;
            }
            written += count;
        }
    }

    void Connection::readerLoop() {
        pumpLines(stdoutHandle_, stopping_, [this](const std::string& line) {
            try {
                handleMessage(json::parse(line));
            } catch (const std::exception& e) {
                log(LogLevel::Warn, std::string("bad message: ") + e.what() + " (" +
                                        line.substr(0, std::min<size_t>(line.size(), 200)) + ")");
            }
        });
        exited_ = true;
        failPending("agent exited");
        if (!stopping_) {
            client_->agentExited();
        }
    }

    void Connection::stderrLoop() {
        pumpLines(stderrHandle_, stopping_,
                  [this](const std::string& line) { client_->agentStderr(line); });
    }
} // namespace acp

#else

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace acp {

    std::pair<std::unique_ptr<Connection>, std::string>
    Connection::spawn(Client& client, const std::vector<std::string>& argv,
                      const SpawnOptions& options) {
        if (argv.empty()) {
            return {nullptr, "No agent command configured"};
        }

        signal(SIGPIPE, SIG_IGN); // dead agent stdin → EPIPE, not process exit

        int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
        if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
            for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
                if (fd >= 0) {
                    close(fd);
                }
            }
            return {nullptr, "Failed to create pipes"};
        }

        std::vector<const char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            cargv.push_back(a.c_str());
        }
        cargv.push_back(nullptr);

        std::vector<std::string> envStrings;
        for (char** e = environ; *e; ++e) {
            std::string var(*e);
            const std::string name = var.substr(0, var.find('='));
            const bool overridden = std::any_of(options.env.begin(), options.env.end(),
                                                [&](const auto& kv) { return kv.first == name; });
            const bool dropped = std::find(options.dropEnv.begin(), options.dropEnv.end(), name) !=
                                 options.dropEnv.end();
            if (!overridden && !dropped) {
                envStrings.push_back(std::move(var));
            }
        }
        for (const auto& [name, value] : options.env) {
            envStrings.push_back(name + "=" + value);
        }
        std::vector<const char*> envp;
        for (const auto& s : envStrings) {
            envp.push_back(s.c_str());
        }
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, inPipe[1]);
        posix_spawn_file_actions_addclose(&actions, outPipe[0]);
        posix_spawn_file_actions_addclose(&actions, errPipe[0]);
        if (!options.cwd.empty()) {
            posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
        }

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        if (options.newProcessGroup) {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
            posix_spawnattr_setpgroup(&attr, 0);
        }

        pid_t pid = -1;
        const int rc =
            posix_spawnp(&pid, cargv[0], &actions, &attr, const_cast<char* const*>(cargv.data()),
                         const_cast<char* const*>(envp.data()));
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);

        close(inPipe[0]);
        close(outPipe[1]);
        close(errPipe[1]);

        if (rc != 0) {
            close(inPipe[1]);
            close(outPipe[0]);
            close(errPipe[0]);
            return {nullptr, std::string("Failed to launch agent: ") + std::strerror(rc)};
        }

        std::unique_ptr<Connection> conn(new Connection());
        conn->client_ = &client;
        conn->pid_ = pid;
        conn->stdinFd_ = inPipe[1];
        conn->stdoutFd_ = outPipe[0];
        conn->stderrFd_ = errPipe[0];
        Connection* raw = conn.get();
        conn->reader_ = std::thread([raw] { raw->readerLoop(); });
        conn->errReader_ = std::thread([raw] { raw->stderrLoop(); });
        log(LogLevel::Info, "agent spawned (pid " + std::to_string(pid) + "): " + argv[0]);
        return {std::move(conn), ""};
    }

    void Connection::stop() {
        stopping_ = true; // lets the reader threads fall out of poll()

        if (pid_ > 0) {
            const auto pid = static_cast<pid_t>(pid_);
            kill(-pid, SIGTERM); // whole group, so the node grandchild goes too
            kill(pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, &status, WNOHANG) != 0) {
                    break;
                }
                usleep(50 * 1000);
            }
            if (kill(pid, 0) == 0) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            log(LogLevel::Info, "agent stopped (pid " + std::to_string(pid_) + ")");
            pid_ = -1;
        }
        // reader writes to stdin too; join before closing the fd
        if (reader_.joinable()) {
            reader_.join();
        }
        if (errReader_.joinable()) {
            errReader_.join();
        }
        {
            std::lock_guard lock(writeMutex_);
            if (stdinFd_ >= 0) {
                close(stdinFd_);
                stdinFd_ = -1;
            }
        }
        if (stdoutFd_ >= 0) {
            close(stdoutFd_);
            stdoutFd_ = -1;
        }
        if (stderrFd_ >= 0) {
            close(stderrFd_);
            stderrFd_ = -1;
        }
        failPending("connection closed");
    }

    void Connection::writeLine(const std::string& line) {
        const std::string data = line + "\n";
        std::lock_guard lock(writeMutex_);
        if (stdinFd_ < 0) {
            return;
        }
        size_t written = 0;
        while (written < data.size()) {
            const ssize_t n = write(stdinFd_, data.data() + written, data.size() - written);
            if (n <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                log(LogLevel::Warn, std::string("write to agent failed: ") + std::strerror(errno));
                return;
            }
            written += static_cast<size_t>(n);
        }
    }

    namespace {
        // poll + read fd line by line until it closes or `stopping` flips
        template <typename OnLine>
        void pumpLines(int fd, const std::atomic<bool>& stopping, OnLine onLine) {
            std::string buffer;
            char chunk[8192];
            while (!stopping) {
                // poll rather than block in read(): anything else that inherited the
                // pipe would keep read() from ever returning, and stop() would hang
                pollfd pfd{fd, POLLIN, 0};
                const int ready = poll(&pfd, 1, 100);
                if (ready == 0) {
                    continue;
                }
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                const ssize_t n = read(fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    break;
                }
                buffer.append(chunk, static_cast<size_t>(n));
                size_t pos = 0;
                while (true) {
                    const auto nl = buffer.find('\n', pos);
                    if (nl == std::string::npos) {
                        break;
                    }
                    std::string line = buffer.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (!line.empty()) {
                        onLine(line);
                    }
                }
                buffer.erase(0, pos);
            }
        }
    } // namespace

    void Connection::readerLoop() {
        pumpLines(stdoutFd_, stopping_, [this](const std::string& line) {
            try {
                handleMessage(json::parse(line));
            } catch (const std::exception& e) {
                log(LogLevel::Warn, std::string("bad message: ") + e.what() + " (" +
                                        line.substr(0, std::min<size_t>(line.size(), 200)) + ")");
            }
        });
        exited_ = true;
        failPending("agent exited");
        if (!stopping_) { // a stop we asked for is not an agent crash
            client_->agentExited();
        }
    }

    void Connection::stderrLoop() {
        pumpLines(stderrFd_, stopping_,
                  [this](const std::string& line) { client_->agentStderr(line); });
    }

} // namespace acp

#endif
