#pragma once

// Falcon NIC endpoint for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_endpoint.h — the primary entry point that
// the P2P engine uses to establish connections, register memory,
// and transfer data over Falcon NICs.

#include "falcon_connection.h"
#include "falcon_context.h"
#include "falcon_device.h"
#include "util/debug.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations for OOB server/client (reuse epoll infra).
class EpollServer;
class EpollClient;

class FalconEndpoint {
 public:
  explicit FalconEndpoint(
      int gpu_index = -1, uint64_t rank_id = UINT64_MAX,
      uint64_t port = 0, bool auto_start_polling = true,
      std::vector<size_t> const& device_ids = std::vector<size_t>())
      : gpu_index_(gpu_index),
        rank_id_(rank_id),
        auto_start_polling_(auto_start_polling) {
    // TODO: Initialize Falcon contexts for the given GPU.
    // TODO: Start OOB server/client for metadata exchange.
  }

  ~FalconEndpoint() {
    // TODO: Stop OOB server/client, destroy contexts.
  }

  int gpuIndex() const { return gpu_index_; }
  size_t contextCount() const { return contexts_.size(); }

  // Build connection to a remote peer.
  int build_connect(uint64_t rank_id, bool sync = true,
                    int timeout_ms = 10000) {
    // TODO: Implement Falcon peer connection setup.
    (void)rank_id;
    (void)sync;
    (void)timeout_ms;
    return -1;
  }

  // Register a memory region for Falcon transfers.
  bool regMem(void* addr, size_t size) {
    // TODO: Register memory across all Falcon contexts.
    (void)addr;
    (void)size;
    return false;
  }

  // Deregister a memory region.
  bool deregMem(void* addr) {
    // TODO: Deregister memory across all Falcon contexts.
    (void)addr;
    return false;
  }

  // Post async send on an established connection.
  bool send_async(uint64_t conn_id, void const* data, size_t size,
                  uint64_t* transfer_id) {
    // TODO: Implement Falcon send.
    (void)conn_id;
    (void)data;
    (void)size;
    (void)transfer_id;
    return false;
  }

  // Post async recv on an established connection.
  bool recv_async(uint64_t conn_id, void* data, size_t size,
                  uint64_t* transfer_id) {
    // TODO: Implement Falcon receive.
    (void)conn_id;
    (void)data;
    (void)size;
    (void)transfer_id;
    return false;
  }

  // Poll for transfer completion.
  bool poll_async(uint64_t transfer_id, bool* is_done) {
    // TODO: Implement Falcon completion polling.
    (void)transfer_id;
    *is_done = false;
    return false;
  }

 private:
  int gpu_index_;
  uint64_t rank_id_;
  bool auto_start_polling_;
  std::vector<std::shared_ptr<FalconContext>> contexts_;
  std::unordered_map<uint64_t, std::shared_ptr<FalconConnection>> connections_;
};
