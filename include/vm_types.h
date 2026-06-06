#pragma once
#include <cstdint>
#include <string>

// ============================================================
//  VIRTUAL MEMORY SIMULATOR — Type Definitions
//  Everything your MMU needs to know about addresses & pages
// ============================================================

// ---------- Address width -----------------------------------
using vaddr_t  = uint32_t;   // 32-bit virtual address
using paddr_t  = uint32_t;   // 32-bit physical address
using vpn_t    = uint32_t;   // Virtual Page Number
using pfn_t    = uint32_t;   // Physical Frame Number

// ---------- Configurable system parameters ------------------
struct SystemConfig {
    uint32_t    page_size_bytes  = 4096;   // 4 KB default
    uint32_t    num_frames       = 16;     // physical frames
    uint32_t    tlb_entries      = 8;      // TLB capacity
    bool        tlb_enabled      = true;
    std::string algorithm        = "lru";  // fifo/lru/clock/opt

    // Derived (computed in constructor / validate())
    uint32_t offset_bits  = 12;  // log2(page_size_bytes)
    uint32_t page_mask    = 0xFFF;
    uint32_t vpn_shift    = 12;

    void validate() {
        // page_size must be power-of-two
        offset_bits = 0;
        uint32_t ps = page_size_bytes;
        while (ps >>= 1) ++offset_bits;
        page_mask  = page_size_bytes - 1;
        vpn_shift  = offset_bits;
    }
};

// ---------- A single page table entry -----------------------
// Mirrors what real hardware uses (simplified PTE)
struct PageTableEntry {
    bool     valid    = false;  // Is this page in RAM?
    bool     dirty    = false;  // Has it been written to?
    bool     ref_bit  = false;  // Used by Clock algorithm
    pfn_t    pfn      = 0;      // Which physical frame?
    uint64_t load_time = 0;     // When was it loaded? (for FIFO/LRU)
};

// ---------- A TLB entry ------------------------------------
struct TLBEntry {
    bool  valid = false;
    vpn_t vpn   = 0;
    pfn_t pfn   = 0;
};

// ---------- Result of a single memory access ---------------
enum class AccessResult { TLB_HIT, PAGE_HIT, PAGE_FAULT };

struct MemoryAccess {
    vaddr_t      vaddr;
    bool         is_write;
    AccessResult result;
    pfn_t        pfn;
    vpn_t        vpn;
    int          evicted_frame = -1;   // -1 if no eviction
    vpn_t        evicted_vpn   = 0;
};

// ---------- Cumulative statistics --------------------------
struct Stats {
    uint64_t total_accesses  = 0;
    uint64_t tlb_hits        = 0;
    uint64_t tlb_misses      = 0;
    uint64_t page_hits       = 0;
    uint64_t page_faults     = 0;

    // Latencies (nanoseconds — typical values)
    static constexpr double TLB_HIT_NS   = 5.0;
    static constexpr double RAM_HIT_NS   = 100.0;
    static constexpr double DISK_HIT_NS  = 10'000'000.0;  // 10 ms

    double tlb_hit_rate()    const { return total_accesses ? (double)tlb_hits / total_accesses : 0.0; }
    double page_fault_rate() const { return total_accesses ? (double)page_faults / total_accesses : 0.0; }

    // EAT = Effective Access Time
    // Formula: (tlb_hit_rate × RAM) + (tlb_miss_rate × (RAM + page_fault_penalty))
    double effective_access_time_ns() const {
        double thr = tlb_hit_rate();
        double pfr = page_fault_rate();
        double eat =
            thr        * RAM_HIT_NS +
            (1 - thr)  * RAM_HIT_NS +    // page table walk costs RAM access
            pfr        * DISK_HIT_NS;
        return eat;
    }

    void print() const;
};