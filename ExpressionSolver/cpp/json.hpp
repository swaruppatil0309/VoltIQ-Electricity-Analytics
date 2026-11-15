// json.hpp — minimal JSON writing helpers (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace js {

inline std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
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

// fixed precision, "null" when the value is not finite
inline std::string num(double v, int prec = 3) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

// round-trippable-ish value: 11, 2.5, 0.333333333333
inline std::string value(double v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    if (v == 0.0) return "0";
    char buf[64];
    if (std::fabs(v) < 1e15 && v == std::floor(v))
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    else
        std::snprintf(buf, sizeof(buf), "%.12g", v);
    return buf;
}

}  // namespace js
