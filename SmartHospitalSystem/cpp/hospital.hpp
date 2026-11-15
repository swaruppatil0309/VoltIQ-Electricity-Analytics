// hospital.hpp — the Smart Hospital Management System engine
//
// Integrates all data structures:
//   PatientHashTable  -> O(1)  patient lookup         (hash_table.hpp)
//   ArrayMaxHeap      -> O(log n) emergency triage    (priority_queue.hpp)
//   SortedIndex / BST -> O(log n) search              (search.hpp)
//   SparseMatrix      -> O(1)-amortised bed allocation (sparse_matrix.hpp)
#pragma once
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include "models.hpp"
#include "hash_table.hpp"
#include "priority_queue.hpp"
#include "search.hpp"
#include "sparse_matrix.hpp"

inline std::string lowerStr(const std::string& s) { return toLower(s); }

struct EngineStats {
    long long elapsedTicks = 0;
    int totalPatients = 0;
    int waiting = 0;
    int admitted = 0;
    int treated = 0;
    int bedsTotal = 0;
    int bedsOccupied = 0;
    int doctors = 0;
    int doctorsBusy = 0;
    int criticalWaiting = 0;
    double hashLoadFactor = 0.0;
    std::size_t hashBuckets = 0;
    int longestChain = 0;
    int heapHeight = 0;
    int bstHeight = 0;
    double bedDensity = 0.0;
    double avgWaitMinutes = 0.0;
    int nightStaff = 0;
    std::string shift = "Day";
};

class Hospital {
public:
    Hospital() { buildLayout(); }

    // =====================================================================
    // setup
    // =====================================================================
    void buildLayout() {
        wards_.clear();
        wardBeds_.clear();
        addWard("ICU", 3, 8);
        addWard("Emergency", 4, 10);
        addWard("General Ward A", 4, 14);
        addWard("General Ward B", 4, 14);
        addWard("Pediatrics", 3, 8);
        addWard("Maternity", 3, 8);

        doctors_.clear();
        addDoctor("Aarav Sharma",   "Cardiology",        "MD, DM Cardio",          2);
        addDoctor("Priya Menon",    "Emergency Medicine","MBBS, MEM",              1);
        addDoctor("Rohit Deshmukh", "General Medicine",  "MBBS, MD",               3);
        addDoctor("Sneha Iyer",     "Pediatrics",        "MBBS, DCH",              4);
        addDoctor("Vikram Rao",     "Orthopaedics",      "MS Ortho",               3);
        addDoctor("Ananya Kulkarni","Maternity",         "MBBS, MS OBG",           3);
        addDoctor("Imran Sheikh",   "Anaesthesiology",   "MBBS, DA",               2);
        addDoctor("Kavya Nair",     "Pulmonology",       "MD Pulmo",               2);
        addDoctor("Aditya Joshi",   "Neurology",         "DM Neuro",               2);
        addDoctor("Meera Patil",    "General Medicine",  "MBBS",                   4);
        addDoctor("Sanjay Gupta",   "Trauma Surgery",    "MS Gen Surg",            1);
        addDoctor("Farah Khan",     "Internal Medicine", "MD",                     3);
    }

    // =====================================================================
    // registration — hash O(1), sorted index O(log n), heap O(log n)
    // =====================================================================
    int registerPatient(const std::string& name, int age, const std::string& gender,
                        const std::string& phone, Severity severity,
                        const std::string& condition, const std::string& department,
                        const std::string& arrivalReal) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient p;
        p.id = nextId_++;
        p.name = name.empty() ? "Unknown" : name;
        p.age = std::max(0, std::min(age, 130));
        p.gender = gender.empty() ? "Other" : gender;
        p.phone = phone;
        p.severity = severity;
        p.condition = condition.empty() ? "General observation" : condition;
        p.department = department.empty() ? "General Medicine" : department;
        p.arrivalTick = tick_;
        p.arrivalReal = arrivalReal.empty() ? clockString() : arrivalReal;
        p.status = Status::WAITING;

        patients_.put(p.id, p);
        index_.insert(p.id, p.name);
        bst_.insert(p.name, p.id);
        triage_.push(QueueEntry{p.id, severityWeight(severity), p.arrivalTick, seq_++, severity});

        logEvent("REGISTER", "Patient #" + std::to_string(p.id) + " " + p.name +
                 " triaged as " + severityName(severity));
        return p.id;
    }

    // =====================================================================
    // lookup
    // =====================================================================
    struct SearchReport {
        std::vector<Patient> results;
        std::string algorithm;
        int probes = 0;
        double microseconds = 0.0;
        bool viaHash = false;
    };

    SearchReport search(const std::string& rawQuery) {
        std::lock_guard<std::mutex> lock(mutex_);
        SearchReport rep;
        std::string query = trim(rawQuery);
        if (query.empty()) { rep.algorithm = "empty query"; return rep; }

        auto t0 = std::chrono::steady_clock::now();

        if (isAllDigits(query)) {
            int id = std::stoi(query);
            rep.viaHash = true;
            rep.algorithm = "Hash table O(1) → binary search O(log n) verification";
            const Patient* hit = patients_.get(id);
            if (hit) {
                int probes = 0;
                const SortedIndex::Pair* p = index_.binarySearchId(id, probes);
                rep.probes = probes;
                if (p) rep.results.push_back(*hit);
            } else {
                rep.probes = 1;
            }
        } else {
            rep.algorithm = "BST exact match O(log n) → binary-search prefix scan O(log n)";
            int cmp = 0;
            int bstId = bst_.find(query, cmp);
            rep.probes += cmp;
            if (bstId > 0) {
                const Patient* p = patients_.get(bstId);
                if (p) rep.results.push_back(*p);
            }
            int probes = 0;
            for (int id : index_.searchNamePrefix(query, probes)) {
                const Patient* p = patients_.get(id);
                if (!p) continue;
                bool dup = false;
                for (const auto& r : rep.results) if (r.id == p->id) { dup = true; break; }
                if (!dup) rep.results.push_back(*p);
            }
            rep.probes += probes;
        }

        rep.microseconds =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        return rep;
    }

    // =====================================================================
    // triage priority queue
    // =====================================================================
    bool callNext() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<QueueEntry> pending;
        bool called = false;
        while (!triage_.empty()) {
            QueueEntry e = triage_.pop();
            Patient* p = patients_.get(e.patientId);
            if (!p || p->status != Status::WAITING) continue;   // lazy deletion

            int doctor = pickDoctor(*p);
            if (doctor < 0) { pending.push_back(e); continue; } // no free doctor

            p->status = Status::ADMITTED;
            p->waitTicks = tick_ - p->arrivalTick;
            p->doctorId = doctor;
            p->treatedTick = tick_;
            doctors_[static_cast<std::size_t>(doctor)].busy = true;
            doctors_[static_cast<std::size_t>(doctor)].currentPatient = p->id;
            doctors_[static_cast<std::size_t>(doctor)].consultStartTick = tick_;
            doctors_[static_cast<std::size_t>(doctor)].treatedCount++;
            logEvent("CONSULT", "Dr. " + doctors_[static_cast<std::size_t>(doctor)].name +
                     " called patient #" + std::to_string(p->id) + " (" +
                     severityName(p->severity) + " · " + std::to_string(p->waitTicks) + " min wait)");
            called = true;
            break;
        }
        for (const auto& e : pending) triage_.push(e);
        return called;
    }

    bool finishConsultation(int patientId) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient* p = patients_.get(patientId);
        if (!p) return false;
        if (p->doctorId >= 0 && p->doctorId < static_cast<int>(doctors_.size())) {
            auto& d = doctors_[static_cast<std::size_t>(p->doctorId)];
            if (d.currentPatient == p->id) { d.busy = false; d.currentPatient = -1; }
        }
        p->status = Status::TREATED;
        p->doctorId = -1;
        logEvent("TREATED", "Patient #" + std::to_string(p->id) + " " + p->name +
                 " completed consultation");
        return true;
    }

    bool escalate(int patientId, Severity newSeverity) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient* p = patients_.get(patientId);
        if (!p || p->status != Status::WAITING) return false;
        if (severityWeight(newSeverity) <= severityWeight(p->severity)) return false;
        triage_.removeById(patientId);
        p->severity = newSeverity;
        triage_.push(QueueEntry{p->id, severityWeight(newSeverity), p->arrivalTick, seq_++, newSeverity});
        logEvent("ESCALATE", "Patient #" + std::to_string(p->id) + " " + p->name +
                 " re-triaged to " + severityName(newSeverity));
        return true;
    }

    bool deletePatient(int patientId) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient* p = patients_.get(patientId);
        if (!p) return false;
        if (p->bedRow >= 0 && p->wardIndex >= 0 &&
            p->wardIndex < static_cast<int>(wardBeds_.size()))
            wardBeds_[static_cast<std::size_t>(p->wardIndex)].clearCell(p->bedRow, p->bedCol);
        if (p->doctorId >= 0 && p->doctorId < static_cast<int>(doctors_.size())) {
            auto& d = doctors_[static_cast<std::size_t>(p->doctorId)];
            if (d.currentPatient == patientId) { d.busy = false; d.currentPatient = -1; }
        }
        triage_.removeById(patientId);
        index_.erase(patientId);
        patients_.remove(patientId);
        logEvent("DELETE", "Patient #" + std::to_string(patientId) + " removed from registry");
        return true;
    }

    std::vector<QueueEntry> priorityQueue() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return triage_.snapshot();
    }

    bool heapIsValid() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return triage_.validateHeap();
    }

    // =====================================================================
    // beds (sparse matrices)
    // =====================================================================
    bool admit(int patientId, int wardIndex, int row, int col) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient* p = patients_.get(patientId);
        if (!p) return false;
        if (wardIndex < 0 || wardIndex >= static_cast<int>(wards_.size())) wardIndex = autoWard(*p);
        SparseMatrix& m = wardBeds_[static_cast<std::size_t>(wardIndex)];

        if (row < 0 || col < 0) {
            auto alloc = m.allocate(allocStrategy_);
            if (!alloc.found) {
                logEvent("FULL", std::string(wards_[static_cast<std::size_t>(wardIndex)].name) +
                         " is full — patient #" + std::to_string(patientId) + " not admitted");
                return false;
            }
            row = alloc.row;
            col = alloc.col;
        }
        if (m.occupied(row, col)) return false;

        if (p->bedRow >= 0 && p->wardIndex >= 0 &&
            p->wardIndex < static_cast<int>(wardBeds_.size()))
            wardBeds_[static_cast<std::size_t>(p->wardIndex)].clearCell(p->bedRow, p->bedCol);

        m.set(row, col, patientId);
        p->wardIndex = wardIndex;
        p->bedRow = row;
        p->bedCol = col;
        if (p->status == Status::WAITING) {
            triage_.removeById(patientId);
            p->waitTicks = tick_ - p->arrivalTick;
        }
        p->status = Status::ADMITTED;
        wards_[static_cast<std::size_t>(wardIndex)].occupied = m.nonZero();
        logEvent("ADMIT", "Patient #" + std::to_string(p->id) + " " + p->name + " → " +
                 wards_[static_cast<std::size_t>(wardIndex)].name + " bed [" +
                 std::to_string(row) + "," + std::to_string(col) + "]");
        return true;
    }

    bool dischargeBed(int patientId) {
        std::lock_guard<std::mutex> lock(mutex_);
        Patient* p = patients_.get(patientId);
        if (!p || p->bedRow < 0) return false;
        if (p->wardIndex >= 0 && p->wardIndex < static_cast<int>(wardBeds_.size())) {
            auto& m = wardBeds_[static_cast<std::size_t>(p->wardIndex)];
            m.clearCell(p->bedRow, p->bedCol);
            wards_[static_cast<std::size_t>(p->wardIndex)].occupied = m.nonZero();
        }
        p->bedRow = -1;
        p->bedCol = -1;
        p->wardIndex = -1;
        logEvent("BED-FREE", "Bed released for patient #" + std::to_string(p->id));
        return true;
    }

    bool setAllocationStrategy(const std::string& strategy) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (strategy != "first" && strategy != "column" && strategy != "spread") return false;
        allocStrategy_ = strategy;
        return true;
    }

    std::string allocationStrategy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocStrategy_;
    }

    // =====================================================================
    // snapshot
    // =====================================================================
    struct Snapshot {
        EngineStats stats;
        std::vector<Patient> patients;
        std::vector<Doctor> doctors;
        std::vector<Ward> wards;
        std::vector<std::vector<Triplet>> wardTriplets;
        std::vector<QueueEntry> queue;           // raw heap layout, index 0 = next
        std::vector<std::string> events;
        std::string allocationStrategy;
        bool heapValid = true;
    };

    Snapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot snap;
        snap.patients = patients_.values();
        std::sort(snap.patients.begin(), snap.patients.end(),
                  [](const Patient& a, const Patient& b) { return a.id < b.id; });

        // refresh ward occupancy from the sparse matrices
        for (std::size_t i = 0; i < wards_.size(); ++i)
            wards_[i].occupied = wardBeds_[i].nonZero();

        snap.doctors = doctors_;
        snap.wards = wards_;
        for (auto& m : wardBeds_) snap.wardTriplets.push_back(m.triplets());
        snap.queue = triage_.snapshot();
        snap.events.assign(recentEvents_.begin(), recentEvents_.end());
        snap.allocationStrategy = allocStrategy_;
        snap.heapValid = triage_.validateHeap();

        EngineStats s;
        s.elapsedTicks = tick_;
        s.totalPatients = static_cast<int>(patients_.size());
        s.doctors = static_cast<int>(doctors_.size());
        for (const auto& p : snap.patients) {
            if (p.status == Status::WAITING) ++s.waiting;
            else if (p.status == Status::ADMITTED) ++s.admitted;
            else ++s.treated;
            if (p.status == Status::WAITING && p.severity == Severity::CRITICAL) ++s.criticalWaiting;
        }
        for (const auto& d : doctors_) if (d.busy) ++s.doctorsBusy;
        for (const auto& w : wards_) { s.bedsTotal += w.capacity; s.bedsOccupied += w.occupied; }
        s.hashLoadFactor = patients_.loadFactor();
        s.hashBuckets = patients_.bucketCount();
        s.longestChain = static_cast<int>(patients_.longestChain());
        s.bstHeight = bst_.height();
        s.heapHeight = static_cast<int>(heapHeightVal(snap.queue.size()));
        s.bedDensity = s.bedsTotal == 0 ? 0.0 :
            100.0 * static_cast<double>(s.bedsOccupied) / static_cast<double>(s.bedsTotal);
        long long totalWait = 0;
        int waited = 0;
        for (const auto& p : snap.patients)
            if (p.waitTicks > 0) { totalWait += p.waitTicks; ++waited; }
        s.avgWaitMinutes = waited ? static_cast<double>(totalWait) / waited : 0.0;
        s.shift = currentShift();
        snap.stats = s;
        return snap;
    }

    // =====================================================================
    // simulation — one tick = one virtual minute
    // =====================================================================
    void tick(int minutes = 1, bool autoArrivals = false, bool autoCall = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < minutes; ++i) {
            ++tick_;

            if (autoArrivals && (tick_ % 3 == 0) && pendingArrivals_ > 0) {
                spawnArrival();
                --pendingArrivals_;
            }

            for (auto& d : doctors_) {
                if (!d.busy || d.currentPatient < 0) continue;
                Patient* p = patients_.get(d.currentPatient);
                if (!p) { d.busy = false; d.currentPatient = -1; continue; }
                int stay = 14 - 2 * severityWeight(p->severity);   // 6..10 minutes
                if (tick_ - d.consultStartTick >= stay) {
                    d.busy = false;
                    p->status = Status::TREATED;
                    d.currentPatient = -1;
                    logEvent("TREATED", "Patient #" + std::to_string(p->id) + " " + p->name +
                             " released after consultation");
                }
            }
            if (autoCall) autoCallNext();
        }
    }

    void startMassCasualty(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingArrivals_ += std::max(0, count);
        logEvent("ALERT", "Mass-casualty intake started: " + std::to_string(count) +
                 " incoming patients queued");
    }

    // =====================================================================
    // seed / demo data
    // =====================================================================
    void seedDemo() {
        struct Seed { const char* name; int age; const char* gender; const char* phone;
                      Severity sev; const char* condition; const char* dept; };
        const Seed seeds[] = {
            {"Ramesh Patil",      67, "Male",   "98220 11001", Severity::CRITICAL,  "Chest pain, ST elevation", "Cardiology"},
            {"Sunita Deshpande",  54, "Female", "98220 11002", Severity::EMERGENCY, "Severe breathlessness",    "Pulmonology"},
            {"Arjun Mehta",       29, "Male",   "98220 11003", Severity::URGENT,    "Fractured forearm",        "Orthopaedics"},
            {"Kavita Joshi",      38, "Female", "98220 11004", Severity::STANDARD,  "Routine check-up",         "General Medicine"},
            {"Mohammed Ansari",   72, "Male",   "98220 11005", Severity::CRITICAL,  "Stroke symptoms",          "Neurology"},
            {"Priyanka Shinde",   31, "Female", "98220 11006", Severity::EMERGENCY, "High fever, dehydration",  "General Medicine"},
            {"Baby Aarohi Kale",  2,  "Female", "98220 11007", Severity::URGENT,    "Persistent vomiting",      "Pediatrics"},
            {"Vishal Chavan",     45, "Male",   "98220 11008", Severity::STANDARD,  "BP follow-up",             "General Medicine"},
            {"Anjali Kulkarni",   27, "Female", "98220 11009", Severity::URGENT,    "Maternity admission",      "Maternity"},
            {"Dinesh Pawar",      58, "Male",   "98220 11010", Severity::EMERGENCY, "Road accident, head trauma","Trauma Surgery"},
            {"Snehal Bhosale",    34, "Female", "98220 11011", Severity::STANDARD,  "Migraine consultation",    "Neurology"},
            {"Rafiq Shaikh",      61, "Male",   "98220 11012", Severity::URGENT,    "Diabetic foot ulcer",      "General Medicine"},
            {"Rohit Sharma",      49, "Male",   "98220 11013", Severity::CRITICAL,  "Crush injury, internal bleeding", "Trauma Surgery"},
            {"Meenakshi Rao",     66, "Female", "98220 11014", Severity::CRITICAL,  "Acute MI, unstable rhythm","Cardiology"},
            {"Tanvi Shah",        23, "Female", "98220 11015", Severity::EMERGENCY, "Severe asthma attack",     "Pulmonology"},
            {"Aniket More",       41, "Male",   "98220 11016", Severity::URGENT,    "Deep laceration",          "General Medicine"},
        };
        for (const auto& s : seeds)
            registerPatient(s.name, s.age, s.gender, s.phone, s.sev, s.condition, s.dept, "");

        // pre-admitted patients so every ward shows live sparse-matrix triplets
        admit(1013, 0, -1, -1);   // ICU      · critical · trauma
        admit(1014, 0, -1, -1);   // ICU      · critical · cardiac
        admit(1010, 1, -1, -1);   // Emergency
        admit(1003, 2, -1, -1);   // General A
        admit(1006, 2, -1, -1);   // General A
        admit(1012, 3, -1, -1);   // General B
        admit(1007, 4, -1, -1);   // Pediatrics
        admit(1009, 5, -1, -1);   // Maternity

        tick(3, false, false);

        // four consultations already running
        callNext();
        callNext();
        callNext();
        callNext();

        // a fresh wave of walk-ins keeps the triage queue under pressure
        registerPatient("Nisha Rane",   35, "Female", "98220 11017", Severity::CRITICAL,
                        "Anaphylaxis after drug reaction", "Emergency Medicine", "");
        registerPatient("Suresh Kamble", 70, "Male",  "98220 11018", Severity::CRITICAL,
                        "Acute breathlessness, low SpO2", "Pulmonology", "");
        registerPatient("Isha Sethi",   19, "Female", "98220 11019", Severity::EMERGENCY,
                        "High fever with seizures", "Neurology", "");
        registerPatient("Varun Nair",   52, "Male",   "98220 11020", Severity::URGENT,
                        "Chest discomfort on exertion", "Cardiology", "");

        tick(1, false, false);

        seedExistingInpatients();
    }

    // long-stay inpatients already on the wards — makes the sparse bed map realistic
    void seedExistingInpatients() {
        struct Inpatient { const char* name; int age; const char* gender; Severity sev;
                           const char* condition; const char* dept; };
        const Inpatient roster[] = {
            {"Sadanand Kulkarni", 74, "Male",   Severity::CRITICAL,  "Post-bypass recovery",     "Cardiology"},
            {"Lata Deshpande",    69, "Female", Severity::CRITICAL,  "Ventilator support",       "Pulmonology"},
            {"Omkar Bhide",       55, "Male",   Severity::EMERGENCY, "Fractured femur, traction", "Orthopaedics"},
            {"Rekha Sathe",       62, "Female", Severity::EMERGENCY, "Pneumonia, IV antibiotics","Pulmonology"},
            {"Chinmay Gokhale",   33, "Male",   Severity::URGENT,    "Appendectomy follow-up",   "General Medicine"},
            {"Asha Wagh",         48, "Female", Severity::URGENT,    "Dengue monitoring",        "General Medicine"},
            {"Nitin Bhosale",     51, "Male",   Severity::URGENT,    "Renal colic",              "General Medicine"},
            {"Pallavi Jagtap",    36, "Female", Severity::STANDARD,  "Observation after fall",   "Orthopaedics"},
            {"Girish Naik",       44, "Male",   Severity::STANDARD,  "Hypertension control",     "General Medicine"},
            {"Shital Pawar",      29, "Female", Severity::STANDARD,  "Migraine protocol",        "Neurology"},
            {"Baby Vedant Sane",  1,  "Male",   Severity::URGENT,    "Bronchiolitis",            "Pediatrics"},
            {"Baby Ira Bhagat",   3,  "Female", Severity::STANDARD,  "Gastroenteritis",          "Pediatrics"},
            {"Aarohi Sule",       42, "Female", Severity::EMERGENCY, "Post C-section recovery",  "Maternity"},
            {"Madhuri Deo",       31, "Female", Severity::URGENT,    "Pre-eclampsia monitoring", "Maternity"},
            {"Kiran Kothari",     58, "Male",   Severity::EMERGENCY, "Diabetic ketoacidosis",    "General Medicine"},
            {"Vaishali Rane",     47, "Female", Severity::URGENT,    "Thyroid storm",            "General Medicine"},
            {"Sandeep Chavan",    40, "Male",   Severity::STANDARD,  "Fractured wrist, cast",    "Orthopaedics"},
            {"Nandini Bhat",      65, "Female", Severity::CRITICAL,  "Arrhythmia, telemetry",    "Cardiology"},
            {"Yogesh Datar",      38, "Male",   Severity::URGENT,    "Acute pancreatitis",       "General Medicine"},
            {"Smita Kher",        53, "Female", Severity::STANDARD,  "Post-op observation",      "General Medicine"},
            {"Rahul Bhandari",    26, "Male",   Severity::URGENT,    "Road accident, clavicle",  "Trauma Surgery"},
            {"Aparna Limaye",     34, "Female", Severity::EMERGENCY, "Severe anaemia, transfusion","General Medicine"},
            {"Prashant Shelar",   49, "Male",   Severity::STANDARD,  "Alcohol withdrawal",       "General Medicine"},
            {"Vaibhavi Kale",     30, "Female", Severity::URGENT,    "Ectopic pregnancy watch",  "Maternity"},
            {"Suresh Marathe",    71, "Male",   Severity::EMERGENCY, "COPD exacerbation",        "Pulmonology"},
            {"Anagha Phadke",     39, "Female", Severity::STANDARD,  "Fever of unknown origin",  "General Medicine"},
            {"Devendra Patil",    56, "Male",   Severity::URGENT,    "Cellulitis, IV therapy",   "General Medicine"},
            {"Rutuja Shinde",     22, "Female", Severity::STANDARD,  "Observation after seizure","Neurology"},
        };
        for (const auto& p : roster) {
            int id = registerPatient(p.name, p.age, p.gender, "98230 4" + std::to_string(1000 + idCounter_++),
                                     p.sev, p.condition, p.dept, "");
            triage_.removeById(id);            // already an inpatient, not queued
            Patient* rec = patients_.get(id);
            if (rec) { rec->status = Status::ADMITTED; rec->waitTicks = 0; }
            admit(id, autoWard(*patients_.get(id)), -1, -1);
        }
    }

private:
    mutable std::mutex mutex_;
    PatientHashTable patients_{128};
    SortedIndex index_;
    NameBst bst_;
    ArrayMaxHeap triage_;
    std::vector<Ward> wards_;
    std::vector<SparseMatrix> wardBeds_;
    std::vector<Doctor> doctors_;
    std::deque<std::string> recentEvents_;
    int nextId_ = 1001;
    long long tick_ = 0;
    long long seq_ = 0;
    int pendingArrivals_ = 0;
    int idCounter_ = 0;
    std::string allocStrategy_ = "first";

    // ------------------------------------------------------------------
    void addWard(const std::string& name, int rows, int cols) {
        Ward w;
        w.index = static_cast<int>(wards_.size());
        w.name = name;
        w.rows = rows;
        w.cols = cols;
        w.capacity = rows * cols;
        wards_.push_back(w);
        wardBeds_.emplace_back(rows, cols);
    }

    void addDoctor(const std::string& name, const std::string& specialty,
                   const std::string& qual, int maxSeverityNumber) {
        Doctor d;
        d.id = static_cast<int>(doctors_.size());
        d.name = name;
        d.specialty = specialty;
        d.qualification = qual;
        d.shift = "Day";
        d.maxSeverity = static_cast<Severity>(maxSeverityNumber);
        doctors_.push_back(d);
    }

    int pickDoctor(const Patient& p) {
        std::string dep = lowerStr(p.department);
        for (std::size_t i = 0; i < doctors_.size(); ++i)
            if (!doctors_[i].busy && lowerStr(doctors_[i].specialty).find(dep) != std::string::npos)
                return static_cast<int>(i);
        for (std::size_t i = 0; i < doctors_.size(); ++i)
            if (!doctors_[i].busy) return static_cast<int>(i);
        return -1;
    }

    void autoCallNext() {
        if (triage_.empty()) return;
        // pop until we find a doctor for the highest-priority waiting patient
        std::vector<QueueEntry> skipped;
        while (!triage_.empty()) {
            QueueEntry e = triage_.pop();
            Patient* p = patients_.get(e.patientId);
            if (!p || p->status != Status::WAITING) continue;
            int doctor = pickDoctor(*p);
            if (doctor < 0) { skipped.push_back(e); continue; }
            p->status = Status::ADMITTED;
            p->waitTicks = tick_ - p->arrivalTick;
            p->doctorId = doctor;
            doctors_[static_cast<std::size_t>(doctor)].busy = true;
            doctors_[static_cast<std::size_t>(doctor)].currentPatient = p->id;
            doctors_[static_cast<std::size_t>(doctor)].consultStartTick = tick_;
            doctors_[static_cast<std::size_t>(doctor)].treatedCount++;
            logEvent("CONSULT", "Dr. " + doctors_[static_cast<std::size_t>(doctor)].name +
                     " called patient #" + std::to_string(p->id) + " (" +
                     severityName(p->severity) + ")");
            break;
        }
        for (const auto& e : skipped) triage_.push(e);
    }

    int autoWard(const Patient& p) {
        switch (p.severity) {
            case Severity::CRITICAL:  return 0;   // ICU
            case Severity::EMERGENCY: return 1;   // Emergency
            default: break;
        }
        if (p.age > 0 && p.age < 14) return 4;
        if (lowerStr(p.department) == "maternity") return 5;
        int best = 2;
        double bestFree = -1;
        for (int i = 2; i <= 3; ++i) {
            double free = static_cast<double>(wards_[static_cast<std::size_t>(i)].capacity -
                                              wardBeds_[static_cast<std::size_t>(i)].nonZero());
            if (free > bestFree) { bestFree = free; best = i; }
        }
        return best;
    }

    void spawnArrival() {
        static const std::vector<std::string> names = {
            "Nikhil Kadam", "Pooja Salunkhe", "Sameer Wagh", "Ritika More", "Ganesh Shinde",
            "Aarti Gaikwad", "Yash Chougule", "Neha Bhosale", "Sagar Jadhav", "Divya Mane",
            "Tushar Kulkarni", "Shweta Pawar", "Akash Deshmukh", "Manasi Patil", "Harsh Vardhan",
            "Rupali Jadhav"
        };
        static const std::vector<std::string> conditions = {
            "Acute abdominal pain", "High-grade fever", "Minor laceration", "Chest tightness",
            "Severe allergic reaction", "Suspected fracture", "Persistent cough", "Dehydration",
            "Hypertensive crisis", "Post-operative review"
        };
        static const std::vector<std::string> departments = {
            "General Medicine", "Emergency Medicine", "Orthopaedics", "Cardiology",
            "Pulmonology", "Neurology", "Pediatrics", "Maternity"
        };
        static std::size_t i = 0;
        Severity sev = static_cast<Severity>(1 + static_cast<int>(i % 4));

        Patient p;
        p.id = nextId_++;
        p.name = names[i % names.size()];
        p.age = 18 + static_cast<int>((i * 13) % 70);
        p.gender = (i % 2 == 0) ? "Male" : "Female";
        p.phone = "98220 2" + std::to_string(1000 + static_cast<int>(i));
        p.severity = sev;
        p.condition = conditions[(i * 7 + 3) % conditions.size()];
        p.department = departments[(i * 5 + 1) % departments.size()];
        p.arrivalTick = tick_;
        p.arrivalReal = clockString();
        p.status = Status::WAITING;
        ++i;

        patients_.put(p.id, p);
        index_.insert(p.id, p.name);
        bst_.insert(p.name, p.id);
        triage_.push(QueueEntry{p.id, severityWeight(sev), p.arrivalTick, seq_++, sev});
        logEvent("ARRIVAL", "Walk-in patient #" + std::to_string(p.id) + " " + p.name +
                 " (" + severityName(sev) + ")");
    }

    std::string currentShift() const {
        long long total = 8 * 60 + tick_;
        int h = static_cast<int>((total / 60) % 24);
        return (h >= 8 && h < 20) ? "Day" : "Night";
    }

    void logEvent(const std::string& type, const std::string& message) {
        recentEvents_.push_back("[" + clockString() + "] " + type + " · " + message);
        while (recentEvents_.size() > 80) recentEvents_.pop_front();
    }

    std::string clockString() const {
        long long total = 8 * 60 + tick_;
        int h = static_cast<int>((total / 60) % 24);
        int m = static_cast<int>(total % 60);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        return std::string(buf);
    }

    static int heapHeightVal(std::size_t n) {
        int h = 0;
        while (n > 0) { ++h; n >>= 1; }
        return h;
    }

    static std::string trim(const std::string& s) {
        std::size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        std::size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static bool isAllDigits(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }
};
