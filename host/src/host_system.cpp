#include "host_system.h"

#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cwctype>
#include <vector>

#include "hikari/log.h"
#include "hikari/system.h"

using Microsoft::WRL::ComPtr;

namespace hikari {

namespace {

const CLSID kNetFwPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
const CLSID kNetFwRule = {0x2C5BC43E, 0x3369, 0x4C33, {0xAB, 0x0C, 0xBE, 0x94, 0x69, 0x67, 0x7A, 0xF4}};
const GUID kButtonSubgroup = {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
const GUID kLidCloseAction = {0x5ca83367, 0x6e45, 0x459f, {0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36}};
constexpr wchar_t kRuleName[] = L"Hikari Host (UDP)";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Hikari";

std::wstring exe_path()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

bool same_path(const wchar_t* a, const std::wstring& b)
{
    if (!a) return false;
    return CompareStringOrdinal(a, -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// RAII CoInitialize for helpers that may run on any thread.
struct ComScope {
    bool ok;
    ComScope() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComScope()
    {
        if (ok) CoUninitialize();
    }
};

template <typename Fn>
void for_each_rule(INetFwPolicy2* policy, Fn&& fn)
{
    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules))) return;
    ComPtr<IUnknown> unk;
    if (FAILED(rules->get__NewEnum(&unk))) return;
    ComPtr<IEnumVARIANT> en;
    if (FAILED(unk.As(&en))) return;
    VARIANT v;
    VariantInit(&v);
    ULONG fetched = 0;
    while (en->Next(1, &v, &fetched) == S_OK && fetched) {
        ComPtr<INetFwRule> rule;
        if (v.vt == VT_DISPATCH && v.pdispVal) v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
        if (rule) fn(rule.Get());
        VariantClear(&v);
    }
}

struct RuleInfo {
    bool ours = false;    // targets Hikari.exe
    bool inbound = false;
    bool enabled = false;
    bool allow = false;
    bool udp_or_any = false;
    std::wstring name;
};

RuleInfo describe(INetFwRule* r, const std::wstring& exe)
{
    RuleInfo i;
    BSTR app = nullptr;
    if (SUCCEEDED(r->get_ApplicationName(&app))) {
        i.ours = same_path(app, exe);
        SysFreeString(app);
    }
    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
    r->get_Direction(&dir);
    i.inbound = dir == NET_FW_RULE_DIR_IN;
    VARIANT_BOOL en = VARIANT_FALSE;
    r->get_Enabled(&en);
    i.enabled = en != VARIANT_FALSE;
    NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
    r->get_Action(&act);
    i.allow = act == NET_FW_ACTION_ALLOW;
    LONG proto = 0;
    r->get_Protocol(&proto);
    constexpr LONG kAnyProtocol = 256;  // NET_FW_IP_PROTOCOL_ANY (missing from some SDK headers)
    i.udp_or_any = proto == NET_FW_IP_PROTOCOL_UDP || proto == kAnyProtocol;
    BSTR name = nullptr;
    if (SUCCEEDED(r->get_Name(&name))) {
        i.name = name ? name : L"";
        SysFreeString(name);
    }
    return i;
}

using ReadIndexFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD);
using WriteIndexFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, DWORD);
using GetSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
using SetSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);

struct PowerApi {
    HMODULE dll = LoadLibraryW(L"powrprof.dll");
    template <typename T>
    T fn(const char* name) const
    {
        return dll ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name))) : nullptr;
    }
};

bool lid_present()
{
    SYSTEM_POWER_CAPABILITIES caps{};
    using GetCaps = BOOLEAN(WINAPI*)(PSYSTEM_POWER_CAPABILITIES);
    PowerApi api;
    auto get = api.fn<GetCaps>("GetPwrCapabilities");
    return get && get(&caps) && caps.LidPresent;
}

bool write_lid_do_nothing()
{
    PowerApi api;
    auto get_scheme = api.fn<GetSchemeFn>("PowerGetActiveScheme");
    auto write_ac = api.fn<WriteIndexFn>("PowerWriteACValueIndex");
    auto write_dc = api.fn<WriteIndexFn>("PowerWriteDCValueIndex");
    auto set_scheme = api.fn<SetSchemeFn>("PowerSetActiveScheme");
    if (!get_scheme || !write_ac || !write_dc || !set_scheme) return false;
    GUID* scheme = nullptr;
    if (get_scheme(nullptr, &scheme) != ERROR_SUCCESS) return false;
    const bool ok = write_ac(nullptr, scheme, &kButtonSubgroup, &kLidCloseAction, 0) == ERROR_SUCCESS &&
                    write_dc(nullptr, scheme, &kButtonSubgroup, &kLidCloseAction, 0) == ERROR_SUCCESS &&
                    set_scheme(nullptr, scheme) == ERROR_SUCCESS;  // re-apply so the change takes effect now
    LocalFree(scheme);
    return ok;
}

}  // namespace

FirewallStatus firewall_status(uint16_t port)
{
    (void)port;
    FirewallStatus st;
    ComScope com;
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(kNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return st;
    st.checked = true;
    long profiles = 0;
    policy->get_CurrentProfileTypes(&profiles);
    for (NET_FW_PROFILE_TYPE2 p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
        if (!(profiles & p)) continue;
        VARIANT_BOOL on = VARIANT_FALSE;
        if (SUCCEEDED(policy->get_FirewallEnabled(p, &on)) && on != VARIANT_FALSE) st.enabled = true;
    }
    const std::wstring exe = exe_path();
    for_each_rule(policy.Get(), [&](INetFwRule* r) {
        const RuleInfo i = describe(r, exe);
        if (!i.ours || !i.inbound || !i.enabled || !i.udp_or_any) return;
        if (i.allow) st.allowed = true;
        else st.blocked = true;
    });
    return st;
}

bool configure_firewall(uint16_t port)
{
    ComScope com;
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(kNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return false;
    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules))) return false;
    const std::wstring exe = exe_path();

    // Windows adds inbound *block* rules for the exe when its "Allow access?"
    // prompt is cancelled; block rules win over allow rules, so remove them
    // along with any previous Hikari rule (the exe may have moved).
    std::vector<std::wstring> remove;
    for_each_rule(policy.Get(), [&](INetFwRule* r) {
        const RuleInfo i = describe(r, exe);
        if ((i.ours && i.inbound && !i.allow) || i.name == kRuleName) remove.push_back(i.name);
    });
    for (const auto& name : remove) {
        BSTR b = SysAllocString(name.c_str());
        rules->Remove(b);
        SysFreeString(b);
    }

    ComPtr<INetFwRule> rule;
    if (FAILED(CoCreateInstance(kNetFwRule, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule)))) return false;
    BSTR name = SysAllocString(kRuleName);
    BSTR desc = SysAllocString(L"Lets paired Hikari clients reach this host (game stream and controller input).");
    BSTR app = SysAllocString(exe.c_str());
    BSTR ports = SysAllocString(std::to_wstring(port).c_str());
    BSTR group = SysAllocString(L"Hikari");
    rule->put_Name(name);
    rule->put_Description(desc);
    rule->put_ApplicationName(app);
    rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP);
    rule->put_LocalPorts(ports);
    rule->put_Direction(NET_FW_RULE_DIR_IN);
    rule->put_Action(NET_FW_ACTION_ALLOW);
    rule->put_Profiles(NET_FW_PROFILE2_ALL);  // Tailscale's adapter is often classed as Public
    rule->put_Grouping(group);
    rule->put_Enabled(VARIANT_TRUE);
    const HRESULT hr = rules->Add(rule.Get());
    SysFreeString(name);
    SysFreeString(desc);
    SysFreeString(app);
    SysFreeString(ports);
    SysFreeString(group);
    if (FAILED(hr)) HK_LOGE("Adding firewall rule failed: %s", hresult_string(hr).c_str());
    return SUCCEEDED(hr);
}

int lid_close_action()
{
    if (!lid_present()) return -1;
    PowerApi api;
    auto get_scheme = api.fn<GetSchemeFn>("PowerGetActiveScheme");
    auto read_ac = api.fn<ReadIndexFn>("PowerReadACValueIndex");
    if (!get_scheme || !read_ac) return -1;
    GUID* scheme = nullptr;
    if (get_scheme(nullptr, &scheme) != ERROR_SUCCESS) return -1;
    DWORD value = 1;
    const DWORD r = read_ac(nullptr, scheme, &kButtonSubgroup, &kLidCloseAction, &value);
    LocalFree(scheme);
    return r == ERROR_SUCCESS ? static_cast<int>(value) : -1;
}

bool keep_awake_on_lid_close(bool allow_elevation_fallback)
{
    if (write_lid_do_nothing()) return true;
    return allow_elevation_fallback && run_elevated_setup("lid", 0) && lid_close_action() == 0;
}

bool autostart_enabled()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    // Only counts if it points at this copy of Hikari (it may have been moved).
    return std::wstring(buf).find(exe_path()) != std::wstring::npos;
}

bool set_autostart(bool on)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS r;
    if (on) {
        const std::wstring cmd = L"\"" + exe_path() + L"\" --host --minimized";
        r = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kRunValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool run_elevated_setup(const std::string& tasks, uint16_t port)
{
    const std::wstring exe = exe_path();
    const std::wstring params = L"--elevated-setup=" + widen(tasks) + L" --port=" + std::to_wstring(port);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        HK_LOGW("Elevated setup not started (%lu)", GetLastError());  // ERROR_CANCELLED = UAC declined
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

int elevated_setup_main(const std::string& tasks, uint16_t port)
{
    bool ok = true;
    if (tasks.find("firewall") != std::string::npos) ok = configure_firewall(port ? port : 47800) && ok;
    if (tasks.find("lid") != std::string::npos) ok = write_lid_do_nothing() && ok;
    return ok ? 0 : 1;
}

}  // namespace hikari
