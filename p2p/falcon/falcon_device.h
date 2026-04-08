#pragma once

// Falcon device abstraction for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_device.h but targets Falcon NICs.

#include "util/debug.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FalconDevice {
 public:
  explicit FalconDevice(std::string const& dev_name)
      : dev_name_(dev_name) {}
  ~FalconDevice() = default;

  std::string const& name() const { return dev_name_; }

  // TODO: Implement Falcon device open/query.
  // Should return a device context handle analogous to ibv_context.

  static std::vector<std::shared_ptr<FalconDevice>> enumerate() {
    // TODO: Enumerate available Falcon devices on the system.
    return {};
  }

 private:
  std::string dev_name_;
};
