#include "hikari/invite.h"

#include <cctype>

#include "hikari/crypto.h"
#include "hikari/net_protocol.h"

namespace hikari {

namespace {
std::string with_port(const std::string& host, uint16_t port)
{
    if (host.empty() || port == proto::kDefaultPort) return host;
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) || s[b] == '"' || s[b] == '\'' || s[b] == '<')) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '"' || s[e - 1] == '\'' ||
                     s[e - 1] == '>' || s[e - 1] == '.'))
        --e;
    return s.substr(b, e - b);
}

bool valid_host(const std::string& h)
{
    if (h.empty() || h.size() > 253) return false;
    for (char c : h)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']' ||
              c == '_'))
            return false;
    return true;
}
}  // namespace

std::string make_invite(const std::string& host, uint16_t port, const std::string& fallback_ip,
                        const std::string& pairing_code)
{
    std::string s = "hikari://" + with_port(host, port) + "/" + pairing_code;
    if (!fallback_ip.empty() && fallback_ip != host) s += "?ip=" + with_port(fallback_ip, port);
    return s;
}

bool parse_invite(const std::string& text, Invite& out)
{
    std::string s = trim(text);
    const std::string scheme = "hikari://";
    if (s.size() >= scheme.size()) {
        std::string head = s.substr(0, scheme.size());
        for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (head == scheme) s = s.substr(scheme.size());
    }
    const auto slash = s.find('/');
    if (slash == std::string::npos) return false;
    Invite inv;
    inv.host = s.substr(0, slash);
    std::string rest = s.substr(slash + 1);
    const auto q = rest.find('?');
    if (q != std::string::npos) {
        const std::string query = rest.substr(q + 1);
        rest = rest.substr(0, q);
        if (query.rfind("ip=", 0) == 0) inv.fallback = query.substr(3, query.find('&') == std::string::npos
                                                                            ? std::string::npos
                                                                            : query.find('&') - 3);
    }
    inv.code = crypto::normalize_pairing_code(rest);
    if (!valid_host(inv.host) || inv.code.size() != 12) return false;
    if (!inv.fallback.empty() && !valid_host(inv.fallback)) inv.fallback.clear();
    out = inv;
    return true;
}

}  // namespace hikari
