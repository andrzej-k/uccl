#pragma once

// Falcon control channel for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_ctrl_channel.h — out-of-band metadata
// exchange channel used during connection setup.

#include <cstdint>
#include <string>

class FalconCtrlChannel {
 public:
  FalconCtrlChannel() = default;
  virtual ~FalconCtrlChannel() = default;

  // TODO: Implement metadata exchange (QP info, keys, addresses)
  // over a side channel (e.g., TCP or Falcon-native control path).

 private:
};
