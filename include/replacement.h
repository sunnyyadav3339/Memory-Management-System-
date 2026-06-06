#pragma once
#include "vm_types.h"
#include <vector>
#include <queue>
#include <list>
#include <unordered_map>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <map>

// ============================================================
//  PAGE REPLACEMENT ALGORITHMS
//  FIFO, LRU, Clock, OPT — all compatible with GCC 6.x
// ============================================================

class ReplacementPolicy {
public:
    explicit ReplacementPolicy(uint32_t num_frames) : num_frames_(num_frames) {}
    virtual ~ReplacementPolicy() {}

    virtual void record_access(vpn_t vpn, int frame_idx) = 0;
    virtual int  get_victim() = 0;
    virtual void on_load(vpn_t vpn, int frame_idx) = 0;
    virtual void on_evict(int frame_idx) {}
    virtual std::string name() const = 0;

protected:
    uint32_t num_frames_;
};

// ============================================================
//  FIFO
// ============================================================
class FIFOPolicy : public ReplacementPolicy {
public:
    explicit FIFOPolicy(uint32_t nf) : ReplacementPolicy(nf) {}

    void record_access(vpn_t, int) override {}

    void on_load(vpn_t, int frame_idx) override {
        fifo_queue_.push(frame_idx);
    }

    int get_victim() override {
        if (fifo_queue_.empty())
            throw std::runtime_error("FIFO: no frames loaded");
        int victim = fifo_queue_.front();
        fifo_queue_.pop();
        return victim;
    }

    std::string name() const override { return "FIFO"; }

private:
    std::queue<int> fifo_queue_;
};

// ============================================================
//  LRU — O(1) with hashmap + doubly linked list
// ============================================================
class LRUPolicy : public ReplacementPolicy {
public:
    explicit LRUPolicy(uint32_t nf) : ReplacementPolicy(nf) {}

    void record_access(vpn_t, int frame_idx) override {
        auto it = frame_to_iter_.find(frame_idx);
        if (it != frame_to_iter_.end())
            lru_list_.erase(it->second);
        lru_list_.push_front(frame_idx);
        frame_to_iter_[frame_idx] = lru_list_.begin();
    }

    void on_load(vpn_t, int frame_idx) override {
        record_access(0, frame_idx);
    }

    int get_victim() override {
        if (lru_list_.empty())
            throw std::runtime_error("LRU: no frames loaded");
        int victim = lru_list_.back();
        frame_to_iter_.erase(victim);
        lru_list_.pop_back();
        return victim;
    }

    void on_evict(int frame_idx) override {
        auto it = frame_to_iter_.find(frame_idx);
        if (it != frame_to_iter_.end()) {
            lru_list_.erase(it->second);
            frame_to_iter_.erase(it);
        }
    }

    std::string name() const override { return "LRU"; }

private:
    std::list<int>                                    lru_list_;
    std::unordered_map<int, std::list<int>::iterator> frame_to_iter_;
};

// ============================================================
//  Clock (Second Chance)
// ============================================================
class ClockPolicy : public ReplacementPolicy {
public:
    explicit ClockPolicy(uint32_t nf)
        : ReplacementPolicy(nf), frames_(nf, false),
          ref_bits_(nf, false), hand_(0) {}

    void record_access(vpn_t, int frame_idx) override {
        ref_bits_[frame_idx] = true;
    }

    void on_load(vpn_t, int frame_idx) override {
        frames_[frame_idx]   = true;
        ref_bits_[frame_idx] = true;
    }

    int get_victim() override {
        while (true) {
            if (frames_[hand_]) {
                if (!ref_bits_[hand_]) {
                    int victim = hand_;
                    frames_[victim]   = false;
                    ref_bits_[victim] = false;
                    hand_ = (hand_ + 1) % num_frames_;
                    return victim;
                } else {
                    ref_bits_[hand_] = false;
                }
            }
            hand_ = (hand_ + 1) % num_frames_;
        }
    }

    void on_evict(int frame_idx) override {
        frames_[frame_idx]   = false;
        ref_bits_[frame_idx] = false;
    }

    std::string name() const override { return "Clock"; }

private:
    std::vector<bool> frames_;
    std::vector<bool> ref_bits_;
    uint32_t          hand_;
};

// ============================================================
//  OPT — Optimal (requires future knowledge — benchmark only)
// ============================================================
class OPTPolicy : public ReplacementPolicy {
public:
    OPTPolicy(uint32_t nf, const std::vector<vpn_t>& ref_string)
        : ReplacementPolicy(nf), ref_string_(ref_string),
          current_pos_(0), loaded_frames_vpn_(nf, -1) {}

    void record_access(vpn_t, int) override {
        current_pos_++;
    }

    void on_load(vpn_t vpn, int frame_idx) override {
        if (frame_idx < (int)loaded_frames_vpn_.size())
            loaded_frames_vpn_[frame_idx] = (int)vpn;
    }

    int get_victim() override {
        int farthest = -1;
        int victim   = 0;

        for (int fi = 0; fi < (int)loaded_frames_vpn_.size(); ++fi) {
            if (loaded_frames_vpn_[fi] < 0) continue;
            vpn_t v = (vpn_t)loaded_frames_vpn_[fi];
            int next = std::numeric_limits<int>::max();
            for (int j = current_pos_; j < (int)ref_string_.size(); ++j) {
                if (ref_string_[j] == v) { next = j; break; }
            }
            if (next > farthest) { farthest = next; victim = fi; }
        }
        loaded_frames_vpn_[victim] = -1;
        return victim;
    }

    void on_evict(int frame_idx) override {
        if (frame_idx < (int)loaded_frames_vpn_.size())
            loaded_frames_vpn_[frame_idx] = -1;
    }

    std::string name() const override { return "OPT"; }

private:
    std::vector<vpn_t> ref_string_;
    int                current_pos_;
    std::vector<int>   loaded_frames_vpn_;
};