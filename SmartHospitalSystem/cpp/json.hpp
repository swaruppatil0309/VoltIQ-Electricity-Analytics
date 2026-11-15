// json.hpp — tiny JSON string helpers (no external dependencies)
#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>

namespace js {

inline std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

inline std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }

inline std::string num(double v, int prec = 2) {
    if (std::isnan(v) || std::isinf(v)) return "0";
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

inline std::string integer(long long v) { return std::to_string(v); }

} // namespace js
