#pragma once
#include "vm_types.h"
#include <vector>

// Simple optional replacement — works on GCC 6.x (no C++17 needed)
struct MaybePFN {
    bool  has_value;
    pfn_t value;
    MaybePFN()          : has_value(false), value(0) {}
    MaybePFN(pfn_t pfn) : has_value(true),  value(pfn) {}
};

// ============================================================
//  TLB — Translation Lookaside Buffer
// ============================================================

class TLB {
public:
    explicit TLB(uint32_t capacity) : capacity_(capacity) {
        entries_.resize(capacity);
    }

    // Look up a VPN. Returns MaybePFN with has_value=true on hit.
    MaybePFN lookup(vpn_t vpn) {
        for (auto& e : entries_) {
            if (e.valid && e.vpn == vpn) {
                TLBEntry copy = e;
                e.valid = false;
                insert_front(copy);
                return MaybePFN(copy.pfn);
            }
        }
        return MaybePFN();  // miss
    }

    void insert(vpn_t vpn, pfn_t pfn) {
        for (auto& e : entries_) {
            if (e.valid && e.vpn == vpn) {
                e.pfn = pfn;
                return;
            }
        }
        int slot = find_free_slot();
        TLBEntry entry;
        entry.valid = true; entry.vpn = vpn; entry.pfn = pfn;
        if (slot >= 0) entries_[slot] = entry;
        else           entries_.back() = entry;
        insert_front(entry);
    }

    void invalidate(vpn_t vpn) {
        for (auto& e : entries_)
            if (e.valid && e.vpn == vpn) e.valid = false;
    }

    void flush() {
        for (auto& e : entries_) e.valid = false;
    }

    void print() const;

private:
    uint32_t              capacity_;
    std::vector<TLBEntry> entries_;

    int find_free_slot() const {
        for (int i = 0; i < (int)entries_.size(); ++i)
            if (!entries_[i].valid) return i;
        return -1;
    }

    void insert_front(const TLBEntry& e) {
        for (int i = (int)entries_.size() - 1; i > 0; --i)
            entries_[i] = entries_[i - 1];
        entries_[0] = e;
    }
};