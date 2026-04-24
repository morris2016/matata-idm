// matata-host.exe — Chrome / Edge Native Messaging host.
//
// Protocol: each message is a 4-byte little-endian length, followed by that
// many bytes of UTF-8 JSON. The browser launches us with our stdin/stdout
// already plumbed; we read one or more messages, dispatch, and reply.
//
// Incoming message shape (flat object, all fields optional except action):
//   {
//     "action":   "download" | "ping",
//     "url":      "https://...",
//     "referer":  "https://...",
//     "cookie":   "k=v; k2=v2",
//     "userAgent":"Mozilla/...",
//     "filename": "foo.zip",
//     "outDir":   "C:\\Users\\...\\Downloads"
//   }
//
// On "download" we spawn matata.exe with the URL + headers and return
// immediately. matata.exe runs detached — its own window (if any) is hidden.
//
// This host is intentionally small: 0 external deps, minimal JSON parser
// scoped to flat string-valued objects.

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- stdio helpers -----------------------------------------------------

bool readExact(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t   got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE),
                      p + got, (DWORD)(n - got), &r, nullptr)) return false;
        if (r == 0) return false;
        got += r;
    }
    return true;
}

bool writeExact(const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < n) {
        DWORD w = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),
                       p + sent, (DWORD)(n - sent), &w, nullptr)) return false;
        if (w == 0) return false;
        sent += w;
    }
    return true;
}

bool readMessage(std::string& out) {
    uint32_t len = 0;
    if (!readExact(&len, sizeof(len))) return false;
    if (len == 0 || len > (1u << 20)) return false; // 1 MiB sanity cap
    out.resize(len);
    return readExact(&out[0], len);
}

// All writes to the host's stdout must be serialized: reader threads,
// response sender, and exit-notifier all share this channel.
std::mutex g_writeMu;

bool writeMessage(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_writeMu);
    uint32_t len = (uint32_t)json.size();
    if (!writeExact(&len, sizeof(len))) return false;
    return writeExact(json.data(), json.size());
}

// ---- tiny JSON reader: flat {"string":"string", ...} ------------------

// Skip over any JSON value starting at pos; advance pos to one past the value.
// Handles string / number / bool / null / nested objects / arrays. Values
// that aren't strings are returned as raw text (unused by our protocol).
void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                            s[i] == '\n' || s[i] == '\r')) ++i;
}

bool parseString(const std::string& s, size_t& i, std::string& out) {
    skipWs(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    if (i + 5 >= s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i + 2 + k];
                        cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= 10 + (h - 'a');
                        else if (h >= 'A' && h <= 'F') cp |= 10 + (h - 'A');
                        else return false;
                    }
                    // Encode as UTF-8 (no surrogate pair handling — fine for
                    // our inputs, which are ASCII-dominant).
                    if (cp < 0x80) out.push_back((char)cp);
                    else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    i += 6;
                    continue;
                }
                default: return false;
            }
            i += 2;
        } else {
            out.push_back(s[i++]);
        }
    }
    if (i >= s.size()) return false;
    ++i;  // skip closing "
    return true;
}

// Skip one JSON value (string/number/bool/null/object/array) without
// extracting it. Used when a field isn't one we care about.
bool skipValue(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') { std::string dummy; return parseString(s, i, dummy); }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            char x = s[i];
            if (x == '"') { std::string d; if (!parseString(s, i, d)) return false; continue; }
            if (x == open)  { ++depth; ++i; continue; }
            if (x == close) { --depth; ++i; if (depth == 0) return true; continue; }
            ++i;
        }
        return false;
    }
    // number / true / false / null: scan until comma / } / ] / whitespace
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') ++i;
    return true;
}

std::map<std::string, std::string> parseFlat(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') return out;
    ++i;
    while (true) {
        skipWs(s, i);
        if (i >= s.size()) break;
        if (s[i] == '}') { ++i; break; }

        std::string key;
        if (!parseString(s, i, key)) break;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') break;
        ++i;
        skipWs(s, i);
        if (i >= s.size()) break;

        if (s[i] == '"') {
            std::string val;
            if (!parseString(s, i, val)) break;
            out[key] = std::move(val);
        } else {
            if (!skipValue(s, i)) break;
        }

        skipWs(s, i);
        if (i >= s.size()) break;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; break; }
        break;
    }
    return out;
}

// ---- JSON writer -------------------------------------------------------

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    _snprintf_s(buf, _TRUNCATE, "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string buildJson(const std::vector<std::pair<std::string,std::string>>& kvs) {
    std::string out = "{";
    bool first = true;
    for (auto& kv : kvs) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += escape(kv.first);
        out += "\":\"";
        out += escape(kv.second);
        out += "\"";
    }
    out += "}";
    return out;
}

// ---- launch matata.exe -------------------------------------------------

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring quoteArg(const std::wstring& s) {
    // Escape for CreateProcess command-line quoting rules.
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        int slashes = 0;
        while (i < s.size() && s[i] == L'\\') { ++slashes; ++i; }
        if (i == s.size()) {
            out.append(slashes * 2, L'\\');
            break;
        } else if (s[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(s[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring defaultDownloadsDir() {
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path) != S_OK
        || !path) return L"";
    std::wstring s(path);
    CoTaskMemFree(path);
    return s;
}

// Accumulate bytes from a child's stdout pipe, emit one host message per
// full line. Lines that parse as JSON object are spliced with `{"jobId":X,`;
// anything else is wrapped in `{"jobId":X,"type":"log","line":"..."}`.
void forwardChildLines(HANDLE hRead, std::string jobId) {
    std::string buf;
    for (;;) {
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(hRead, chunk, sizeof(chunk), &got, nullptr)) break;
        if (got == 0) break;
        buf.append(chunk, got);
        for (;;) {
            auto nl = buf.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string out;
            if (!line.empty() && line[0] == '{') {
                // matata with --job-id already splices jobId first; avoid
                // adding it twice.
                if (line.compare(0, 9, "{\"jobId\":") == 0) {
                    out = line;
                } else {
                    out = std::string("{\"jobId\":\"") + jobId + "\","
                          + line.substr(1);
                }
            } else {
                out = std::string("{\"jobId\":\"") + jobId + "\","
                      + "\"type\":\"log\",\"line\":\""
                      + escape(line) + "\"}";
            }
            writeMessage(out);
        }
    }
}

std::atomic<uint64_t> g_jobCounter{1};
std::atomic<int>      g_activeJobs{0};

std::string generateJobId() {
    wchar_t pid[16];
    _snwprintf_s(pid, _TRUNCATE, L"%u", GetCurrentProcessId());
    uint64_t n = g_jobCounter.fetch_add(1);
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "j%u-%llu", GetCurrentProcessId(), (unsigned long long)n);
    (void)pid;
    return buf;
}

bool launchMatata(const std::map<std::string,std::string>& msg,
                  std::string& jobIdOut,
                  std::string& errorOut) {
    auto get = [&](const char* key) -> std::string {
        auto it = msg.find(key);
        return (it == msg.end()) ? std::string() : it->second;
    };

    std::string url = get("url");
    if (url.empty()) { errorOut = "missing url"; return false; }

    std::wstring matataExe = exeDir() + L"\\matata.exe";
    DWORD attrs = GetFileAttributesW(matataExe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        errorOut = "matata.exe not found next to matata-host.exe";
        return false;
    }

    std::wstring outDir = utf8ToWide(get("outDir"));
    if (outDir.empty()) outDir = defaultDownloadsDir();

    std::string jobId = get("jobId");
    if (jobId.empty()) jobId = generateJobId();

    std::wstring cmd = quoteArg(matataExe);
    cmd += L" " + quoteArg(utf8ToWide(url));
    cmd += L" --json-progress --job-id " + quoteArg(utf8ToWide(jobId));

    auto addHeader = [&](const char* key, const char* prefix) {
        std::string v = get(key);
        if (v.empty()) return;
        std::wstring hv = utf8ToWide(std::string(prefix) + v);
        cmd += L" -H ";
        cmd += quoteArg(hv);
    };
    addHeader("referer",   "Referer: ");
    addHeader("cookie",    "Cookie: ");
    addHeader("userAgent", "User-Agent: ");

    std::string fn = get("filename");
    if (!fn.empty()) { cmd += L" -o "; cmd += quoteArg(utf8ToWide(fn)); }
    if (!outDir.empty()) { cmd += L" -d "; cmd += quoteArg(outDir); }

    // Create a pipe for the child's stdout+stderr.
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        errorOut = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        errorOut = "CreateProcess failed";
        return false;
    }

    // Reader + waiter thread: forwards lines, then emits a final "exit"
    // message once matata exits. g_activeJobs lets main() hold the process
    // alive until every spawned child has flushed.
    HANDLE hProc = pi.hProcess;
    g_activeJobs.fetch_add(1);
    std::thread([hRead, hProc, jobId]() {
        forwardChildLines(hRead, jobId);
        CloseHandle(hRead);
        WaitForSingleObject(hProc, INFINITE);
        DWORD rc = 1;
        GetExitCodeProcess(hProc, &rc);
        CloseHandle(hProc);
        std::string out = std::string("{\"jobId\":\"") + jobId
                        + "\",\"type\":\"exit\",\"code\":" + std::to_string((int)rc) + "}";
        writeMessage(out);
        g_activeJobs.fetch_sub(1);
    }).detach();
    CloseHandle(pi.hThread);

    jobIdOut = jobId;
    return true;
}

}

int main() {
    // Chrome expects raw binary on stdio. Without this, Windows will rewrite
    // CRLF on write and eat 0x1A on read, corrupting messages.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    for (;;) {
        std::string raw;
        if (!readMessage(raw)) break;

        auto msg = parseFlat(raw);
        std::string action = msg.count("action") ? msg["action"] : std::string();

        if (action == "ping") {
            writeMessage(buildJson({ {"ok","true"}, {"version","0.4"} }));
        } else if (action == "download") {
            std::string err, jobId;
            if (launchMatata(msg, jobId, err)) {
                writeMessage(buildJson({ {"ok","true"}, {"jobId", jobId} }));
            } else {
                writeMessage(buildJson({ {"ok","false"}, {"error",err} }));
            }
        } else {
            writeMessage(buildJson({ {"ok","false"},
                                     {"error", "unknown action: " + action} }));
        }
    }
    // Stdin closed. Wait for any in-flight downloads to finish flushing so
    // the extension receives their final progress / done / exit events.
    while (g_activeJobs.load() > 0) Sleep(50);
    return 0;
}
