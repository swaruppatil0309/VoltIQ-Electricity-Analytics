// models.hpp — core entities used across the hospital system
#pragma once
#include <string>
#include <cstdint>

// ---------------------------------------------------------------- severity
enum class Severity : int {
    CRITICAL = 1,   // resuscitation / immediate
    EMERGENCY = 2,  // very urgent
    URGENT = 3,     // urgent
    STANDARD = 4    // routine / OPD
};

// Higher number = treated first
inline int severityWeight(Severity s) { return 5 - static_cast<int>(s); }

inline const char* severityName(Severity s) {
    switch (s) {
        case Severity::CRITICAL:  return "Critical";
        case Severity::EMERGENCY: return "Emergency";
        case Severity::URGENT:    return "Urgent";
        default:                  return "Standard";
    }
}

inline const char* severityCode(Severity s) {
    switch (s) {
        case Severity::CRITICAL:  return "critical";
        case Severity::EMERGENCY: return "emergency";
        case Severity::URGENT:    return "urgent";
        default:                  return "standard";
    }
}

inline bool parseSeverity(const std::string& in, Severity& out) {
    if (in == "critical"  || in == "Critical"  || in == "1") { out = Severity::CRITICAL;  return true; }
    if (in == "emergency" || in == "Emergency" || in == "2") { out = Severity::EMERGENCY; return true; }
    if (in == "urgent"    || in == "Urgent"    || in == "3") { out = Severity::URGENT;    return true; }
    if (in == "standard"  || in == "Standard"  || in == "4") { out = Severity::STANDARD;  return true; }
    return false;
}

// ---------------------------------------------------------------- patient
enum class Status { WAITING, ADMITTED, TREATED };

inline const char* statusName(Status s) {
    switch (s) {
        case Status::ADMITTED: return "Admitted";
        case Status::TREATED:  return "Treated";
        default:               return "Waiting";
    }
}

inline const char* statusCode(Status s) {
    switch (s) {
        case Status::ADMITTED: return "admitted";
        case Status::TREATED:  return "treated";
        default:               return "waiting";
    }
}

inline bool parseStatus(const std::string& in, Status& out) {
    if (in == "waiting" || in == "WAITING")  { out = Status::WAITING;  return true; }
    if (in == "admitted" || in == "ADMITTED") { out = Status::ADMITTED; return true; }
    if (in == "treated" || in == "TREATED")   { out = Status::TREATED;  return true; }
    return false;
}

struct Patient {
    int         id = 0;
    std::string name;
    int         age = 0;
    std::string gender;
    std::string phone;
    Severity    severity = Severity::STANDARD;
    std::string condition;
    std::string department;
    long long   arrivalTick = 0;   // virtual minutes since system start
    std::string arrivalReal;       // wall clock (HH:MM) — display only
    Status      status = Status::WAITING;
    int         wardIndex = -1;    // which sparse matrix holds the bed
    int         bedRow = -1;       // sparse-matrix coordinates when admitted
    int         bedCol = -1;
    int         doctorId = -1;
    long long   waitTicks = 0;     // minutes waited before consultation
    long long   treatedTick = 0;
};

// ---------------------------------------------------------------- doctor
struct Doctor {
    int         id = 0;
    std::string name;
    std::string specialty;
    std::string qualification;
    std::string shift;      // "Day" or "Night"
    Severity    maxSeverity = Severity::CRITICAL; // lowest severity number handled
    bool        busy = false;
    int         currentPatient = -1;
    long long   consultStartTick = 0;
    int         treatedCount = 0;
};

// ---------------------------------------------------------------- ward / bed
struct Ward {
    int         index = 0;
    std::string name;
    int         rows = 0;
    int         cols = 0;
    int         capacity = 0;
    int         occupied = 0;
};

inline double occupancyPct(const Ward& w) {
    if (w.capacity == 0) return 0.0;
    return 100.0 * static_cast<double>(w.occupied) / static_cast<double>(w.capacity);
}
