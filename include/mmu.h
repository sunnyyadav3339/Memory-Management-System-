#pragma once
#include "vm_types.h"
#include "tlb.h"
#include "replacement.h"
#include <vector>
#include <memory>
#include <iostream>
#include <iomanip>
#include <cassert>

// ============================================================
//  MMU — Memory Management Unit
//
//  This is the heart of the simulator.
//  It coordinates:
//    1. Address translation (virtual → physical)
//    2. TLB lookup and insertion
//    3. Page table management
//    4. Page fault handling (calling the replacement policy)
//    5. Statistics collection
//
//  How a real access flows:
//
//  CPU issues virtual address
//       │
//       ▼
//  Split into VPN + Offset
//       │
//       ▼
//  TLB lookup(VPN)  ──→  HIT: PFN found, done. EAT = ~5ns
//       │ MISS
//       ▼
//  Page Table[VPN].valid?  ──→  YES: PFN found. EAT = ~100ns
//       │ NO (page fault!)
//       ▼
//  Evict a frame (replacement policy)
//  Load page from disk into freed frame
//  Update Page Table
//  Update TLB
//  Resume process  EAT = ~10ms
// ============================================================

class MMU {
public:
    explicit MMU(const SystemConfig& cfg)
        : cfg_(cfg),
          tlb_(cfg.tlb_entries),
          page_table_(1 << (32 - cfg.offset_bits)),  // 2^vpn_bits entries
          physical_frames_(cfg.num_frames, -1),        // -1 = empty
          timer_(0)
    {
        cfg_.validate();
        page_table_.resize(1 << (32 - cfg_.offset_bits));
        build_policy();
    }

    // --------------------------------------------------------
    //  Core interface: simulate one memory access
    // --------------------------------------------------------
    MemoryAccess access(vaddr_t vaddr, bool is_write = false) {
        timer_++;

        // --- Step 1: Split virtual address ---
        vpn_t  vpn    = vaddr >> cfg_.vpn_shift;
        vaddr_t offset = vaddr &  cfg_.page_mask;

        MemoryAccess result;
        result.vaddr    = vaddr;
        result.is_write = is_write;
        result.vpn      = vpn;
        result.evicted_frame = -1;

        stats_.total_accesses++;

        // --- Step 2: TLB lookup ---
        MaybePFN tlb_result = cfg_.tlb_enabled ? tlb_.lookup(vpn) : MaybePFN();

        if (tlb_result.has_value) {
            // TLB HIT — fastest path
            pfn_t pfn = tlb_result.value;
            result.result = AccessResult::TLB_HIT;
            result.pfn    = pfn;
            stats_.tlb_hits++;

            // Update ref bit for Clock algorithm
            policy_->record_access(vpn, pfn);

            // Mark dirty if write
            if (is_write) page_table_[vpn].dirty = true;

            return result;
        }

        stats_.tlb_misses++;

        // --- Step 3: Page table lookup ---
        PageTableEntry& pte = page_table_[vpn];

        if (pte.valid) {
            // PAGE HIT (TLB miss, but page is in RAM)
            result.result = AccessResult::PAGE_HIT;
            result.pfn    = pte.pfn;
            stats_.page_hits++;

            // Update TLB with this translation
            tlb_.insert(vpn, pte.pfn);

            // Update ref bit
            pte.ref_bit = true;
            if (is_write) pte.dirty = true;

            policy_->record_access(vpn, pte.pfn);
            return result;
        }

        // --- Step 4: PAGE FAULT ---
        result.result = AccessResult::PAGE_FAULT;
        stats_.page_faults++;

        int free_frame = find_free_frame();
        int frame_to_use;

        if (free_frame >= 0) {
            // Free frame available — no eviction needed
            frame_to_use = free_frame;
            result.evicted_frame = -1;
        } else {
            // Must evict a page
            frame_to_use = policy_->get_victim();
            result.evicted_frame = frame_to_use;

            // Find which VPN was in the evicted frame and invalidate it
            vpn_t evicted_vpn = physical_frames_[frame_to_use];
            result.evicted_vpn = evicted_vpn;

            // Invalidate old PTE
            page_table_[evicted_vpn].valid   = false;
            page_table_[evicted_vpn].ref_bit = false;
            // (dirty bit handling: in real OS would write to disk if dirty)

            // Invalidate TLB entry for evicted page
            tlb_.invalidate(evicted_vpn);
            policy_->on_evict(frame_to_use);
        }

        // Load new page into frame
        physical_frames_[frame_to_use] = vpn;
        pte.valid     = true;
        pte.pfn       = frame_to_use;
        pte.ref_bit   = true;
        pte.dirty     = is_write;
        pte.load_time = timer_;

        // Update TLB
        tlb_.insert(vpn, frame_to_use);

        // Notify policy of the load
        policy_->on_load(vpn, frame_to_use);

        result.pfn = frame_to_use;
        return result;
    }

    // --------------------------------------------------------
    //  Run a batch of accesses (for algorithm benchmarking)
    // --------------------------------------------------------
    Stats run_trace(const std::vector<vaddr_t>& trace) {
        for (vaddr_t addr : trace) {
            access(addr);
        }
        return stats_;
    }

    const Stats&        get_stats()  const { return stats_; }
    const SystemConfig& get_config() const { return cfg_;   }
    const TLB&          get_tlb()    const { return tlb_;   }

    // --------------------------------------------------------
    //  Dump the state of physical frames
    // --------------------------------------------------------
    void dump_frames() const {
        std::cout << "\n  Physical frames:\n";
        std::cout << "  " << std::string(48, '-') << "\n";
        for (uint32_t i = 0; i < cfg_.num_frames; ++i) {
            std::cout << "  Frame [" << std::setw(2) << i << "]: ";
            if (physical_frames_[i] < 0) {
                std::cout << "(empty)\n";
            } else {
                vpn_t v = physical_frames_[i];
                std::cout << "VPN " << std::setw(4) << v;
                if (page_table_[v].dirty) std::cout << "  [DIRTY]";
                std::cout << "\n";
            }
        }
        std::cout << "  " << std::string(48, '-') << "\n";
    }

private:
    SystemConfig            cfg_;
    TLB                     tlb_;
    std::vector<PageTableEntry> page_table_;
    std::vector<int>        physical_frames_;  // frame → VPN (-1 if empty)
    std::unique_ptr<ReplacementPolicy> policy_;
    Stats                   stats_;
    uint64_t                timer_;

    int find_free_frame() const {
        for (int i = 0; i < (int)physical_frames_.size(); ++i)
            if (physical_frames_[i] < 0) return i;
        return -1;
    }

    void build_policy() {
        if      (cfg_.algorithm == "fifo")  policy_ = std::make_unique<FIFOPolicy>(cfg_.num_frames);
        else if (cfg_.algorithm == "lru")   policy_ = std::make_unique<LRUPolicy>(cfg_.num_frames);
        else if (cfg_.algorithm == "clock") policy_ = std::make_unique<ClockPolicy>(cfg_.num_frames);
        else throw std::invalid_argument("Unknown algorithm: " + cfg_.algorithm);
    }
};