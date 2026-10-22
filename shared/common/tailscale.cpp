#include "hikari/udp.h"
#include "hikari/tailscale.h"
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace hikari {
namespace {
std::wstring app_path()
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &root))) return {};
    std::wstring path = std::wstring(root) + L"\\Tailscale\\tailscale-ipn.exe";
    CoTaskMemFree(root);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? path : L"";
}
std::wstring cli_path()
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &root))) return {};
    std::wstring path = std::wstring(root) + L"\\Tailscale\\tailscale.exe";
    CoTaskMemFree(root);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? path : L"";
}

// Runs `tailscale.exe status --json` without a console window; returns stdout.
std::string status_json()
{
    const std::wstring exe = cli_path();
    if (exe.empty()) return {};
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" status --json";
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(wr);  // our copy; ReadFile now sees EOF when the child exits
    std::string out;
    if (ok) {
        const ULONGLONG deadline = GetTickCount64() + 3000;
        char buf[4096];
        bool timed_out = false;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // child exited, pipe drained
            if (avail) {
                DWORD n = 0;
                if (!ReadFile(rd, buf, sizeof(buf), &n, nullptr) || n == 0) break;
                out.append(buf, n);
                if (out.size() > 8u * 1024 * 1024) break;
                continue;
            }
            if (GetTickCount64() > deadline) {
                timed_out = true;
                break;
            }
            Sleep(10);
        }
        if (timed_out || WaitForSingleObject(pi.hProcess, 500) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            out.clear();
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

// Minimal JSON reader, enough for `tailscale status --json`.
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const char* key) const
    {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const char* key) const
    {
        const Json* v = get(key);
        return v && v->type == String ? v->s : std::string();
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out)
    {
        ws();
        return value(out, 0);
    }

private:
    void ws()
    {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) ++i_;
    }
    bool lit(const char* w)
    {
        const size_t n = std::strlen(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    bool peek(char c) const { return i_ < t_.size() && t_[i_] == c; }
    bool string(std::string& out)
    {
        if (!peek('"')) return false;
        ++i_;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c == '\\' && i_ < t_.size()) {
                const char e = t_[i_++];
                if (e == 'u') {
                    i_ = std::min(t_.size(), i_ + 4);  // non-ASCII escapes don't matter for the fields we read
                    c = '?';
                } else {
                    c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
                }
            }
            out.push_back(c);
        }
        if (i_ >= t_.size()) return false;
        ++i_;
        return true;
    }
    bool value(Json& v, int depth)
    {
        if (depth > 64 || i_ >= t_.size()) return false;
        if (peek('{')) {
            v.type = Json::Object;
            ++i_;
            ws();
            if (peek('}')) {
                ++i_;
                return true;
            }
            for (;;) {
                std::string key;
                ws();
                if (!string(key)) return false;
                ws();
                if (!peek(':')) return false;
                ++i_;
                ws();
                Json child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                ws();
                if (peek(',')) {
                    ++i_;
                    continue;
                }
                if (!peek('}')) return false;
                ++i_;
                return true;
            }
        }
        if (peek('[')) {
            v.type = Json::Array;
            ++i_;
            ws();
            if (peek(']')) {
                ++i_;
                return true;
            }
            for (;;) {
                ws();
                Json child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (peek(',')) {
                    ++i_;
                    continue;
                }
                if (!peek(']')) return false;
                ++i_;
                return true;
            }
        }
        if (peek('"')) {
            v.type = Json::String;
            return string(v.s);
        }
        if (lit("true")) {
            v.type = Json::Bool;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.type = Json::Bool;
            return true;
        }
        if (lit("null")) return true;
        char* end = nullptr;
        v.n = std::strtod(t_.c_str() + i_, &end);
        if (end == t_.c_str() + i_) return false;
        v.type = Json::Number;
        i_ = static_cast<size_t>(end - t_.c_str());
        return true;
    }

    const std::string& t_;
    size_t i_ = 0;
};

bool parse_status(const std::string& text, Json& root)
{
    return !text.empty() && JsonParser(text).parse(root) && root.type == Json::Object;
}

bool contains_tailscale(const wchar_t* value)
{
    if (!value) return false;
    std::wstring text(value);
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text.find(L"tailscale") != std::wstring::npos;
}
}
bool is_tailscale_adapter(const wchar_t* name, const wchar_t* description)
{
    return contains_tailscale(name) || contains_tailscale(description);
}
bool is_tailscale_ipv4(const std::string& ip)
{
    in_addr address{};
    return inet_pton(AF_INET, ip.c_str(), &address) == 1 &&
        (ntohl(address.s_addr) & 0xffc00000u) == 0x64400000u;
}
TailscaleInfo tailscale_info()
{
    TailscaleInfo result;
    result.installed = !app_path().empty();
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG error = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (error == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        error = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (error != NO_ERROR) return result;
    for (auto* a = adapters; a; a = a->Next) {
        if (!is_tailscale_adapter(a->FriendlyName, a->Description)) continue;
        result.installed = true;
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN]{};
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) && is_tailscale_ipv4(ip)) {
                result.ipv4 = ip;
                return result;
            }
        }
    }
    return result;
}
bool open_tailscale_setup()
{
    const auto app = app_path();
    const wchar_t* target = app.empty() ? L"https://tailscale.com/download/windows" : app.c_str();
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::string tailscale_dns_name() { return tailscale_dns_name_from_json(status_json()); }

TailscalePath tailscale_path_to(const std::string& peer_ip) { return tailscale_path_from_json(status_json(), peer_ip); }

std::string tailscale_dns_name_from_json(const std::string& json)
{
    Json root;
    if (!parse_status(json, root) || root.str("BackendState") != "Running") return {};
    const Json* self = root.get("Self");
    if (!self) return {};
    std::string name = self->str("DNSName");
    while (!name.empty() && name.back() == '.') name.pop_back();  // FQDN trailing dot
    return name;
}

TailscalePath tailscale_path_from_json(const std::string& json, const std::string& peer_ip)
{
    TailscalePath path;
    Json root;
    if (!parse_status(json, root)) return path;
    const Json* peers = root.get("Peer");
    if (!peers || peers->type != Json::Object) return path;
    for (const auto& kv : peers->obj) {
        const Json& peer = kv.second;
        const Json* ips = peer.get("TailscaleIPs");
        if (!ips) continue;
        bool match = false;
        for (const auto& ip : ips->arr) match = match || (ip.type == Json::String && ip.s == peer_ip);
        if (!match) continue;
        path.known = true;
        path.endpoint = peer.str("CurAddr");
        path.direct = !path.endpoint.empty();
        path.relay = peer.str("Relay");
        return path;
    }
    return path;
}
} // namespace hikari
