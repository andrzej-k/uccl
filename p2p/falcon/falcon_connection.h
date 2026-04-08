#pragma once

// Falcon connection for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_connection.h — tracks per-peer data channels
// and provides round-robin channel selection.

#include "falcon_data_channel.h"
#include "util/debug.h"
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

class FalconConnection {
 public:
  FalconConnection() : last_channel_id_(0) {}
  virtual ~FalconConnection() = default;

  void addChannel(uint32_t channel_id,
                  std::shared_ptr<FalconDataChannel> channel) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    channels_[channel_id] = std::move(channel);
  }

  std::shared_ptr<FalconDataChannel> getChannel(uint32_t channel_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return nullptr;
    return it->second;
  }

  size_t channelCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return channels_.size();
  }

  // Round-robin channel selection.
  std::pair<uint32_t, uint64_t> selectNextChannelRoundRobin() {
    size_t n = channelCount();
    if (n == 0) return {0, 0};
    uint32_t cur = last_channel_id_.load(std::memory_order_relaxed);
    uint32_t next = (cur % n) + 1;
    last_channel_id_.store(next, std::memory_order_relaxed);
    auto ch = getChannel(next);
    if (!ch) return {0, 0};
    return {next, ch->contextID()};
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<FalconDataChannel>> channels_;
  std::atomic<uint32_t> last_channel_id_;
};
