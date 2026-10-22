#include "hikari/udp.h"
#include "hikari/tailscale.h"
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cwctype>
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
} // namespace hikari
