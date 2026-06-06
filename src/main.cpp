#include "mmu.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

// ============================================================
//  HELPER: Pretty-print statistics
// ============================================================
void print_stats(const std::string& algo, const Stats& s, const SystemConfig& cfg) {
    std::cout << "\n  ┌─────────────────────────────────────────────┐\n";
    std::cout << "  │  Algorithm: " << std::left << std::setw(32) << algo << "│\n";
    std::cout << "  ├─────────────────────────────────────────────┤\n";

    auto row = [](const std::string& label, const std::string& value) {
        std::cout << "  │  " << std::left << std::setw(28) << label
                  << std::right << std::setw(13) << value << "  │\n";
    };

    row("Total accesses:",   std::to_string(s.total_accesses));
    row("TLB hits:",         std::to_string(s.tlb_hits));
    row("TLB misses:",       std::to_string(s.tlb_misses));
    row("Page hits:",        std::to_string(s.page_hits));
    row("Page faults:",      std::to_string(s.page_faults));

    // Formatted percentages
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f%%", s.tlb_hit_rate() * 100.0);
    row("TLB hit rate:",     buf);
    snprintf(buf, sizeof(buf), "%.2f%%", s.page_fault_rate() * 100.0);
    row("Page fault rate:",  buf);
    snprintf(buf, sizeof(buf), "%.1f ns", s.effective_access_time_ns());
    row("Eff. access time:", buf);

    std::cout << "  └─────────────────────────────────────────────┘\n";
}

// ============================================================
//  HELPER: Generate a simple random reference string
//  In a real system you'd use Valgrind/PIN traces
// ============================================================
std::vector<vaddr_t> generate_reference_string(int count, int page_range, uint32_t page_size) {
    std::vector<vaddr_t> refs;
    refs.reserve(count);

    // Simulate locality of reference:
    //   80% of accesses hit 20% of pages (hot pages)
    std::vector<int> hot_pages = {2, 3, 5, 7};

    srand(42);
    for (int i = 0; i < count; ++i) {
        int page;
        if (rand() % 100 < 80) {
            // Hot access
            page = hot_pages[rand() % hot_pages.size()];
        } else {
            // Cold access
            page = rand() % page_range;
        }
        vaddr_t offset = rand() % page_size;
        refs.push_back(page * page_size + offset);
    }
    return refs;
}

// ============================================================
//  BELADY'S ANOMALY DEMO
//  With FIFO: more frames can cause MORE page faults!
//  Reference string: 3 0 1 2 0 3 0 4 2 3 0 3 2 1 2 0 1 7 0 1
// ============================================================
void demo_beladys_anomaly() {
    std::vector<vpn_t> classic_ref = {3,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};
    uint32_t page_size = 256;

    // Convert VPN sequence to virtual addresses
    auto to_addrs = [&](const std::vector<vpn_t>& vpns) {
        std::vector<vaddr_t> v;
        for (auto p : vpns) v.push_back(p * page_size);
        return v;
    };

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "  BÉLÁDY'S ANOMALY DEMO (FIFO)\n";
    std::cout << std::string(50, '=') << "\n";
    std::cout << "  Reference string: 3 0 1 2 0 3 0 4 2 3 0 3 2 1 2 0 1 7 0 1\n\n";

    for (int frames : {3, 4, 5}) {
        SystemConfig cfg;
        cfg.page_size_bytes = page_size;
        cfg.num_frames      = frames;
        cfg.tlb_enabled     = false;   // disable TLB so we see pure algorithm behavior
        cfg.algorithm       = "fifo";
        cfg.validate();

        MMU mmu(cfg);
        auto addrs = to_addrs(classic_ref);
        mmu.run_trace(addrs);
        const Stats& s = mmu.get_stats();

        std::cout << "  Frames=" << frames << "  →  Page faults: " << s.page_faults
                  << "  (fault rate: " << std::fixed << std::setprecision(0)
                  << s.page_fault_rate()*100 << "%)\n";
    }

    std::cout << "\n  Note: 3 frames = fewer faults than 4 frames — Bélády's Anomaly!\n";
    std::cout << "  (This cannot happen with LRU or OPT)\n";
}

// ============================================================
//  COMPARISON: Run all algorithms on the same trace
// ============================================================
void compare_all_algorithms(const std::vector<vaddr_t>& trace, const SystemConfig& base_cfg) {
    std::vector<std::string> algos = {"fifo", "lru", "clock"};

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "  ALGORITHM COMPARISON\n";
    std::cout << "  Frames=" << base_cfg.num_frames
              << "  TLB=" << base_cfg.tlb_entries
              << "  Accesses=" << trace.size() << "\n";
    std::cout << std::string(50, '=') << "\n";

    for (const auto& algo : algos) {
        SystemConfig cfg = base_cfg;
        cfg.algorithm = algo;
        cfg.validate();

        MMU mmu(cfg);
        mmu.run_trace(trace);
        print_stats(algo, mmu.get_stats(), cfg);
    }
}

// ============================================================
//  STEP-BY-STEP TRACE: Show each access in detail
// ============================================================
void step_trace(const std::vector<vaddr_t>& trace, SystemConfig cfg, int max_steps = 20) {
    cfg.validate();
    MMU mmu(cfg);

    std::cout << "\n" << std::string(68, '=') << "\n";
    std::cout << "  STEP-BY-STEP TRACE  [" << cfg.algorithm << "] "
              << cfg.num_frames << " frames\n";
    std::cout << std::string(68, '=') << "\n";
    std::cout << std::left
              << std::setw(6)  << "Step"
              << std::setw(12) << "Vaddr"
              << std::setw(8)  << "VPN"
              << std::setw(8)  << "PFN"
              << std::setw(14) << "Result"
              << "Evicted\n";
    std::cout << std::string(68, '-') << "\n";

    int steps = std::min((int)trace.size(), max_steps);
    for (int i = 0; i < steps; ++i) {
        auto r = mmu.access(trace[i]);

        std::string res_str;
        switch (r.result) {
            case AccessResult::TLB_HIT:   res_str = "TLB HIT";   break;
            case AccessResult::PAGE_HIT:  res_str = "PAGE HIT";  break;
            case AccessResult::PAGE_FAULT:res_str = "FAULT";     break;
        }

        std::cout << std::left
                  << std::setw(6)  << (i+1)
                  << std::setw(12) << std::hex << r.vaddr
                  << std::dec
                  << std::setw(8)  << r.vpn
                  << std::setw(8)  << r.pfn
                  << std::setw(14) << res_str;

        if (r.evicted_frame >= 0) {
            std::cout << "Frame " << r.evicted_frame << " (VPN " << r.evicted_vpn << ")";
        } else {
            std::cout << "—";
        }
        std::cout << "\n";
    }
    if ((int)trace.size() > max_steps)
        std::cout << "  ... (" << trace.size() - max_steps << " more accesses)\n";

    mmu.dump_frames();
    print_stats(cfg.algorithm, mmu.get_stats(), cfg);
}

// ============================================================
//  ADDRESS TRANSLATION DEMO
// ============================================================
void demo_address_translation() {
    SystemConfig cfg;
    cfg.page_size_bytes = 4096;  // 4 KB pages → 12-bit offset
    cfg.validate();

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "  ADDRESS TRANSLATION DEMO\n";
    std::cout << std::string(50, '=') << "\n";

    std::vector<vaddr_t> test_addrs = {
        0x00001004,  // VPN=1, offset=4
        0x00002A00,  // VPN=2, offset=0xA00
        0x0000D0F0,  // VPN=13, offset=0xF0
        0x0001F800,  // VPN=31, offset=0x800
    };

    for (vaddr_t va : test_addrs) {
        vpn_t  vpn    = va >> cfg.offset_bits;
        vaddr_t offset = va & cfg.page_mask;

        std::cout << "  VA=0x" << std::hex << std::setw(8) << std::setfill('0') << va
                  << "  →  VPN=" << std::dec << std::setfill(' ') << std::setw(4) << vpn
                  << "  +  Offset=0x" << std::hex << offset
                  << "\n" << std::dec;
    }

    std::cout << "\n  (With 4 KB pages: low 12 bits = offset, upper 20 bits = VPN)\n";
}

// ============================================================
//  MAIN
// ============================================================
int main(int argc, char* argv[]) {
    system("chcp 65001 > nul");
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║    VIRTUAL MEMORY & PAGING SIMULATOR             ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";

    // Base configuration
    SystemConfig cfg;
    cfg.page_size_bytes = 4096;
    cfg.num_frames      = 8;
    cfg.tlb_entries     = 8;
    cfg.tlb_enabled     = true;
    cfg.algorithm       = "lru";
    cfg.validate();

    // --- Demo 1: Address translation -------------------------
    demo_address_translation();

    // --- Demo 2: Step-by-step trace -------------------------
    // Classic FIFO reference string (page-based, one access per page)
    uint32_t ps = cfg.page_size_bytes;
    std::vector<vpn_t> vpn_seq = {7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2, 1, 2, 0, 1};
    std::vector<vaddr_t> simple_trace;
    for (auto v : vpn_seq) simple_trace.push_back(v * ps);

    step_trace(simple_trace, cfg, 17);

    // --- Demo 3: Algorithm comparison -----------------------
    // Generate a larger trace with locality
    auto big_trace = generate_reference_string(1000, 20, ps);
    compare_all_algorithms(big_trace, cfg);

    // --- Demo 4: Bélády's anomaly ---------------------------
    demo_beladys_anomaly();

    std::cout << "\n  Done.\n\n";
    return 0;
}