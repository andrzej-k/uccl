#pragma once

// Falcon context for UCCL P2P.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors p2p/rdma/rdma_context.h — wraps a Falcon device context,
// protection domain, and memory registration cache.

#include "falcon_device.h"
#include "util/debug.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

class FalconContext {
 public:
  explicit FalconContext(std::shared_ptr<FalconDevice> dev,
                        uint64_t context_id = 0)
      : dev_(std::move(dev)), context_id_(context_id) {
    // TODO: Open Falcon device context and allocate protection domain.
  }

  ~FalconContext() {
    // TODO: Destroy PD and close context.
  }

  uint64_t getContextID() const { return context_id_; }

  // Register a memory region with the Falcon NIC.
  // Returns an opaque handle (nullptr on failure).
  void* regMem(void* addr, size_t size) {
    // TODO: Implement Falcon memory registration.
    (void)addr;
    (void)size;
    return nullptr;
  }

  // Deregister a previously registered memory region.
  void deregMem(void* mr_handle) {
    // TODO: Implement Falcon memory deregistration.
    (void)mr_handle;
  }

 private:
  std::shared_ptr<FalconDevice> dev_;
  uint64_t context_id_;
};
