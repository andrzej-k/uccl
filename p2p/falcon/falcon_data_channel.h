#pragma once

// Falcon data channel for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_data_channel.h — represents a single
// Falcon QP used for data transfer within a connection.

#include <cstdint>

class FalconDataChannel {
 public:
  FalconDataChannel(uint64_t context_id = 0)
      : context_id_(context_id) {}
  virtual ~FalconDataChannel() = default;

  uint64_t contextID() const { return context_id_; }

  // TODO: Implement send/recv/write/read operations over Falcon QP.

 private:
  uint64_t context_id_;
};
