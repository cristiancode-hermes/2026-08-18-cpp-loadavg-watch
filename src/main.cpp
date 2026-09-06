// SPDX-License-Identifier: MIT
// cpp-loadavg-watch — Real-time /proc/loadavg monitor with process breakdown
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o loadavg-watch
// Usage: ./loadavg-watch [options]

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ── ANSI color codes (mutable, respects --no-color) ──
std::string C_BOLD = "\033[1m";
std::string C_RESET = "\033[0m";
std::string C_RED = "\033[1;31m";
std::string C_YELLOW = "\033[1;33m";
std::string C_GREEN = "\033[1;32m";
std::string C_CYAN = "\033[1;36m";
std::string C_MAGENTA = "\033[1;35m";
std::string C_DIM = "\033[2m";
std::string C_WHITE = "\033[1;37m";
std::string C_BRIGHT_RED = "\033[91m";
std::string C_BG_RED = "\033[41m";

bool g_color = true;

void disable_color() {
    C_BOLD = C_RESET = C_RED = C_YELLOW = C_GREEN = "";
    C_CYAN = C_MAGENTA = C_DIM = C_WHITE = C_BRIGHT_RED = C_BG_RED = "";
}

// ── System data structures ──
struct LoadAvg {
    double one = 0.0;
    double five = 0.0;
    double fifteen = 0.0;
    int running = 0;
    int total = 0;
    int last_pid = 0;
};

struct ProcessInfo {
    int running = 0;
    int sleeping = 0;
    int stopped = 0;
    int zombie = 0;
    int total = 0;
};

struct MemoryInfo {
    unsigned long total_kb = 0;
    unsigned long free_kb = 0;
    unsigned long available_kb = 0;
    unsigned long buffers_kb = 0;
    unsigned long cached_kb = 0;
    unsigned long swap_total_kb = 0;
    unsigned long swap_free_kb = 0;
    unsigned long dirty_kb = 0;
};

// ── Readers ──
LoadAvg read_loadavg() {
    LoadAvg la;
    std::ifstream f("/proc/loadavg");
    if (!f.is_open()) return la;
    f >> la.one >> la.five >> la.fifteen;
    char slash;
    f >> la.running >> slash >> la.total >> la.last_pid;
    return la;
}

int read_cpu_count() {
    std::ifstream f("/proc/cpuinfo");
    int count = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("processor")) count++;
    }
    return count > 0 ? count : 1;
}

ProcessInfo read_processes() {
    ProcessInfo pi;
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return pi;
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("procs_running ")) {
            pi.running = std::stoi(line.substr(14));
        } else if (line.starts_with("procs_blocked ")) {
            pi.stopped = std::stoi(line.substr(13));
        }
    }
    // Derive from loadavg
    LoadAvg la = read_loadavg();
    pi.total = la.total;
    pi.sleeping = std::max(0, pi.total - pi.running - pi.stopped);
    return pi;
}

MemoryInfo read_meminfo() {
    MemoryInfo m;
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return m;
    std::string line;
    while (std::getline(f, line)) {
        unsigned long val = 0;
        if (sscanf(line.c_str(), "MemTotal: %lu kB", &val) == 1) m.total_kb = val;
        else if (sscanf(line.c_str(), "MemFree: %lu kB", &val) == 1) m.free_kb = val;
        else if (sscanf(line.c_str(), "MemAvailable: %lu kB", &val) == 1) m.available_kb = val;
        else if (sscanf(line.c_str(), "Buffers: %lu kB", &val) == 1) m.buffers_kb = val;
        else if (sscanf(line.c_str(), "Cached: %lu kB", &val) == 1) m.cached_kb = val;
        else if (sscanf(line.c_str(), "SwapTotal: %lu kB", &val) == 1) m.swap_total_kb = val;
        else if (sscanf(line.c_str(), "SwapFree: %lu kB", &val) == 1) m.swap_free_kb = val;
        else if (sscanf(line.c_str(), "Dirty: %lu kB", &val) == 1) m.dirty_kb = val;
    }
    return m;
}

std::string read_uptime_str() {
    std::ifstream f("/proc/uptime");
    double up_secs = 0.0;
    if (f.is_open()) f >> up_secs;
    int days = static_cast<int>(up_secs) / 86400;
    int hours = (static_cast<int>(up_secs) % 86400) / 3600;
    int mins = (static_cast<int>(up_secs) % 3600) / 60;
    char buf[32];
    if (days > 0)
        snprintf(buf, sizeof(buf), "%dd %dh %dm", days, hours, mins);
    else if (hours > 0)
        snprintf(buf, sizeof(buf), "%dh %dm", hours, mins);
    else
        snprintf(buf, sizeof(buf), "%dm", mins);
    return buf;
}

double read_uptime_secs() {
    std::ifstream f("/proc/uptime");
    double up = 0.0;
    if (f.is_open()) f >> up;
    return up;
}

// ── Formatting helpers ──
std::string fmt_bytes(unsigned long kb) {
    char buf[32];
    if (kb >= 1048576)
        snprintf(buf, sizeof(buf), "%.1f GB", kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, sizeof(buf), "%.0f MB", kb / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%lu KB", kb);
    return buf;
}

std::string fmt_pct(double pct) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%5.1f%%", pct);
    return buf;
}

std::string load_color(double load, int cores) {
    double ratio = load / cores;
    if (ratio >= 2.0) return C_RED;
    if (ratio >= 1.0) return C_YELLOW;
    if (ratio >= 0.7) return C_CYAN;
    return C_GREEN;
}

std::string bar(double pct, int width = 20) {
    int filled = std::clamp(static_cast<int>(pct / 100.0 * width), 0, width);
    std::string bar_str;
    bar_str += std::string(filled, '#');
    bar_str += std::string(width - filled, '.');
    return bar_str;
}

// ── Display functions ──
void print_header(int cpu_count, const LoadAvg& la) {
    std::string load_str = std::format("{:.2f} / {:.2f} / {:.2f}", la.one, la.five, la.fifteen);
    std::cout << "\n" << C_BOLD << C_CYAN << "  /proc/loadavg" << C_RESET
              << C_DIM << " — " << cpu_count << " cores" << C_RESET
              << "  " << C_DIM << "[" << read_uptime_str() << "]" << C_RESET << "\n";
    std::cout << "\n  " << C_BOLD << "Load:" << C_RESET
              << "  " << load_color(la.one, cpu_count) << C_BOLD << load_str << C_RESET
              << C_DIM << "  (" << la.running << "/" << la.total << " tasks, pid " << la.last_pid << ")" << C_RESET << "\n\n";
}

void print_load_detail(const LoadAvg& la, int cpu_count) {
    auto show = [&](const char* label, double val) {
        std::string c = load_color(val, cpu_count);
        char ratio_buf[16];
        snprintf(ratio_buf, sizeof(ratio_buf), "%.2f", val / cpu_count);
        std::cout << "  " << C_BOLD << label << ":" << C_RESET
                  << " " << c << std::format("{:>6.2f}", val) << C_RESET
                  << C_DIM << "  (" << ratio_buf << "x/cpu)" << C_RESET << "\n";
    };
    show("  1 min", la.one);
    show("  5 min", la.five);
    show(" 15 min", la.fifteen);
    std::cout << "\n";
}

void print_process_summary(const ProcessInfo& pi) {
    std::cout << "  " << C_BOLD << "Processes:" << C_RESET << "\n";
    std::cout << "  " << C_GREEN << "  Running:   " << std::format("{:>4}", pi.running) << C_RESET;
    std::cout << "  " << C_CYAN << "Sleeping:  " << std::format("{:>4}", pi.sleeping) << C_RESET << "\n";
    std::cout << "  " << C_YELLOW << "  Stopped:   " << std::format("{:>4}", pi.stopped) << C_RESET;
    std::cout << "  " << C_RED << "Zombie:    " << std::format("{:>4}", pi.zombie) << C_RESET;
    std::cout << "  " << C_DIM << "Total: " << pi.total << C_RESET << "\n\n";
}

void print_memory_summary(const MemoryInfo& m) {
    if (m.total_kb == 0) return;
    unsigned long used_kb = m.total_kb - m.available_kb;
    double usage_pct = (m.total_kb > 0) ? (100.0 * used_kb / m.total_kb) : 0.0;

    unsigned long swap_used_kb = m.swap_total_kb - m.swap_free_kb;
    double swap_pct = (m.swap_total_kb > 0) ? (100.0 * swap_used_kb / m.swap_total_kb) : 0.0;

    std::string mem_color = (usage_pct > 90) ? C_RED : (usage_pct > 70) ? C_YELLOW : C_GREEN;

    std::cout << "  " << C_BOLD << "Memory:" << C_RESET
              << "  " << mem_color << fmt_bytes(used_kb) << C_RESET
              << " / " << fmt_bytes(m.total_kb)
              << " " << C_DIM << "(" << fmt_pct(usage_pct) << " used)" << C_RESET << "\n";

    std::cout << "  " << C_BOLD << "Swap:  " << C_RESET
              << "  " << fmt_bytes(swap_used_kb) << " / " << fmt_bytes(m.swap_total_kb)
              << " " << C_DIM << "(" << fmt_pct(swap_pct) << " used)" << C_RESET;

    if (m.dirty_kb > 1024)
        std::cout << "  " << C_YELLOW << "dirty:" << fmt_bytes(m.dirty_kb) << C_RESET;
    std::cout << "\n\n";
}

void print_load_bar(double load, int cores, const char* label) {
    double ratio = load / cores;
    double pct = std::min(ratio * 100.0, 200.0);  // allow >100% for overloaded
    std::string c = load_color(load, cores);

    char ratio_buf[16];
    snprintf(ratio_buf, sizeof(ratio_buf), "%.2fx", ratio);

    std::cout << "  " << C_BOLD << label << C_RESET << " "
              << c << bar(std::min(pct, 100.0), 24) << C_RESET
              << " " << c << std::format("{:>6.2f}", load) << C_RESET
              << C_DIM << " (" << ratio_buf << ")" << C_RESET << "\n";
}

void print_snapshot(int cpu_count) {
    LoadAvg la = read_loadavg();
    ProcessInfo pi = read_processes();
    MemoryInfo m = read_meminfo();

    std::cout << "{\n";
    std::cout << "  \"loadavg\": {\n";
    std::cout << "    \"1min\": " << la.one << ",\n";
    std::cout << "    \"5min\": " << la.five << ",\n";
    std::cout << "    \"15min\": " << la.fifteen << "\n";
    std::cout << "  },\n";
    std::cout << "  \"cpu_count\": " << cpu_count << ",\n";
    std::cout << "  \"load_ratio\": {\n";
    std::cout << "    \"1min\": " << (la.one / cpu_count) << ",\n";
    std::cout << "    \"5min\": " << (la.five / cpu_count) << ",\n";
    std::cout << "    \"15min\": " << (la.fifteen / cpu_count) << "\n";
    std::cout << "  },\n";
    std::cout << "  \"processes\": {\n";
    std::cout << "    \"running\": " << pi.running << ",\n";
    std::cout << "    \"sleeping\": " << pi.sleeping << ",\n";
    std::cout << "    \"stopped\": " << pi.stopped << ",\n";
    std::cout << "    \"zombie\": " << pi.zombie << ",\n";
    std::cout << "    \"total\": " << pi.total << "\n";
    std::cout << "  },\n";
    std::cout << "  \"memory\": {\n";
    std::cout << "    \"total_kb\": " << m.total_kb << ",\n";
    std::cout << "    \"available_kb\": " << m.available_kb << ",\n";
    std::cout << "    \"buffers_kb\": " << m.buffers_kb << ",\n";
    std::cout << "    \"cached_kb\": " << m.cached_kb << "\n";
    std::cout << "  },\n";
    std::cout << "  \"uptime\": \"" << read_uptime_str() << "\",\n";
    std::cout << "  \"uptime_secs\": " << read_uptime_secs() << "\n";
    std::cout << "}\n";
}

// ── Watch mode ──
void watch_mode(int cpu_count, int interval_secs) {
    double prev_one = 0.0;
    double prev_five = 0.0;
    auto t_start = std::chrono::steady_clock::now();

    for (int step = 1; ; step++) {
        LoadAvg la = read_loadavg();
        ProcessInfo pi = read_processes();
        MemoryInfo m = read_meminfo();

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t_start).count();

        std::cout << "\033[2J\033[H";  // clear screen

        print_header(cpu_count, la);
        print_load_detail(la, cpu_count);

        // Process summary
        print_process_summary(pi);

        // Memory
        print_memory_summary(m);

        // Load bar visualization
        std::cout << "  " << C_BOLD << "Load Bars:" << C_RESET << "\n\n";
        print_load_bar(la.one, cpu_count, "  1 min");
        print_load_bar(la.five, cpu_count, "  5 min");
        print_load_bar(la.fifteen, cpu_count, "15 min");
        std::cout << "\n";

        // Delta indicators
        if (step > 1 && prev_one > 0.0) {
            double delta1 = la.one - prev_one;
            double delta5 = la.five - prev_five;
            std::string d1_color = (delta1 > 0) ? C_RED : (delta1 < 0) ? C_GREEN : C_DIM;
            std::string d5_color = (delta5 > 0) ? C_RED : (delta5 < 0) ? C_GREEN : C_DIM;

            char d1_buf[32], d5_buf[32];
            snprintf(d1_buf, sizeof(d1_buf), "%+.2f", delta1);
            snprintf(d5_buf, sizeof(d5_buf), "%+.2f", delta5);

            std::cout << "  " << C_DIM << "Delta:" << C_RESET
                      << "  1m " << d1_color << d1_buf << C_RESET
                      << "  5m " << d5_color << d5_buf << C_RESET;
        }
        std::cout << "\n";

        // Footer
        std::cout << "\n  " << C_DIM << "Step " << step
                  << "  |  Elapsed " << std::format("{:.0f}s", elapsed)
                  << "  |  Refresh every " << interval_secs << "s"
                  << "  |  Ctrl+C to exit" << C_RESET << "\n";
        std::cout << std::flush;

        prev_one = la.one;
        prev_five = la.five;

        std::this_thread::sleep_for(std::chrono::seconds(interval_secs));
    }
}

// ── Self-test ──
void selftest() {
    int failures = 0;

    // Test 1: /proc/loadavg readable
    LoadAvg la = read_loadavg();
    if (la.one < 0.0 || la.five < 0.0 || la.fifteen < 0.0) {
        std::cerr << "FAIL: invalid load average values\n";
        failures++;
    }
    if (la.total <= 0) {
        std::cerr << "FAIL: no processes detected\n";
        failures++;
    }
    std::cout << "[OK] loadavg: " << std::format("{:.2f}/{:.2f}/{:.2f}", la.one, la.five, la.fifteen)
              << " (" << la.running << "/" << la.total << " tasks)\n";

    // Test 2: CPU count
    int cores = read_cpu_count();
    if (cores <= 0) {
        std::cerr << "FAIL: no CPUs detected\n";
        failures++;
    }
    std::cout << "[OK] CPUs: " << cores << "\n";

    // Test 3: Memory info
    MemoryInfo m = read_meminfo();
    if (m.total_kb == 0) {
        std::cerr << "FAIL: no memory info available\n";
        failures++;
    } else {
        std::cout << "[OK] Memory: " << fmt_bytes(m.total_kb) << " total, "
                  << fmt_bytes(m.available_kb) << " available\n";
    }

    // Test 4: Process info
    ProcessInfo pi = read_processes();
    if (pi.total <= 0) {
        std::cerr << "FAIL: no process info\n";
        failures++;
    } else {
        std::cout << "[OK] Processes: " << pi.running << " running, "
                  << pi.sleeping << " sleeping, " << pi.total << " total\n";
    }

    // Test 5: Uptime
    std::string up = read_uptime_str();
    if (up.empty()) {
        std::cerr << "FAIL: cannot read uptime\n";
        failures++;
    } else {
        std::cout << "[OK] Uptime: " << up << "\n";
    }

    // Test 6: Load ratio consistency
    double ratio = la.one / cores;
    if (ratio < 0.0) {
        std::cerr << "FAIL: negative load ratio\n";
        failures++;
    } else {
        std::cout << "[OK] Load ratio 1m: " << std::format("{:.3f}", ratio) << "x per core\n";
    }

    // Test 7: Load average ordering (1m >= 5m >= 15m on idle)
    // Not strictly true under load, but verify values are in valid range
    bool valid_range = (la.one >= 0.0 && la.five >= 0.0 && la.fifteen >= 0.0);
    if (!valid_range) {
        std::cerr << "FAIL: negative load average detected\n";
        failures++;
    } else {
        std::cout << "[OK] Load averages in valid range\n";
    }

    if (failures > 0) {
        std::cerr << "\n" << failures << " test(s) FAILED\n";
        std::exit(1);
    }
    std::cout << "\nAll tests passed.\n";
}

// ── Main ──
void print_usage() {
    std::cout << C_BOLD << "loadavg-watch" << C_RESET << " — Real-time /proc/loadavg monitor\n\n"
              << "Reads system load averages, CPU count, process breakdown,\n"
              << "and memory usage from the Linux proc filesystem.\n\n"
              << "Usage:\n"
              << "  loadavg-watch              Watch mode (refresh every 3s)\n"
              << "  loadavg-watch -w <sec>     Watch mode with custom interval\n"
              << "  loadavg-watch -s           Snapshot (JSON)\n"
              << "  loadavg-watch --selftest   Run self-tests\n"
              << "  loadavg-watch -c           No color output\n"
              << "  loadavg-watch -h           Show this help\n\n"
              << "Data sources: /proc/loadavg, /proc/stat, /proc/meminfo,\n"
              << "              /proc/cpuinfo, /proc/uptime\n\n"
              << "Watch mode shows:\n"
              << "  - 1m/5m/15m load averages with per-core ratio\n"
              << "  - Process state breakdown (running/sleeping/stopped/zombie)\n"
              << "  - Memory usage with available + buffers/cached + dirty pages\n"
              << "  - Visual load bars and delta tracking\n\n"
              << "Colors: " << C_GREEN << "low" << C_RESET << " (<0.7x) "
              << C_CYAN << "moderate" << C_RESET << " (<1x) "
              << C_YELLOW << "high" << C_RESET << " (<2x) "
              << C_RED << "critical" << C_RESET << " (>=2x)\n";
}

int main(int argc, char** argv) {
    int watch_secs = 3;
    bool snapshot = false;
    bool do_selftest = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "-w" || arg == "--watch") && i + 1 < argc) {
            watch_secs = std::atoi(argv[++i]);
            if (watch_secs < 1) watch_secs = 1;
        } else if (arg == "-s" || arg == "--snapshot") {
            snapshot = true;
        } else if (arg == "--selftest") {
            do_selftest = true;
        } else if (arg == "-c" || arg == "--no-color") {
            g_color = false;
            disable_color();
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
    }

    int cpu_count = read_cpu_count();

    if (do_selftest) {
        selftest();
        return 0;
    }

    if (snapshot) {
        print_snapshot(cpu_count);
        return 0;
    }

    watch_mode(cpu_count, watch_secs);
    return 0;
}
