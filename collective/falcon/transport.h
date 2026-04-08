#pragma once

// Falcon transport stub for UCCL collective.
// Based on OCP-NET-Falcon specification:
//   https://github.com/opencomputeproject/OCP-NET-Falcon
//
// This file mirrors the structure of the RDMA/EFA collective transports,
// providing the same Channel / ConnID / Endpoint / Engine abstractions
// over Falcon hardware.

#include "transport_config.h"
#include "util/debug.h"
#include "util/latency.h"
#include "util/jring.h"
#include "util/shared_pool.h"
#include "util/util.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace uccl {

struct ConnID {
  void* context;
  int sock_fd;
  FlowID flow_id;
  PeerID peer_id;
  int dev;
};

struct Mhandle {
  // TODO: Replace with Falcon-specific memory handle.
  void* opaque;
};

struct ucclRequest {
  // TODO: Populate with Falcon request tracking state.
  bool done;
  int size;
};

/**
 * @class Channel
 * @brief Command queue for application threads to submit rx/tx requests
 * to a UcclFlow over Falcon transport.
 */
class Channel {
  constexpr static uint32_t kChannelSize = 1024;

 public:
  struct Msg {
    enum Op : uint8_t { kTx, kRx, kRead, kWrite };
    Op opcode;
    PeerID peer_id;
    struct ucclRequest* ureq;
    PollCtx* poll_ctx;
  };

  struct CtrlMsg {
    enum Op : uint8_t {
      kInstallCtx = 0,
      kInstallFlow,
    };
    Op opcode;
    PeerID peer_id;
    PollCtx* poll_ctx;
  };

  Channel() {
    tx_cmdq_ = create_ring(sizeof(Msg), kChannelSize);
    rx_cmdq_ = create_ring(sizeof(Msg), kChannelSize);
    ctrl_cmdq_ = create_ring(sizeof(CtrlMsg), kChannelSize);
  }

  ~Channel() {
    free(tx_cmdq_);
    free(rx_cmdq_);
    free(ctrl_cmdq_);
  }

  jring_t* tx_cmdq_;
  jring_t* rx_cmdq_;
  jring_t* ctrl_cmdq_;
};

class UcclFlow;
class FalconEngine;
class FalconEndpoint;

/**
 * @class FalconEngine
 * @brief Main Falcon engine. Manages Falcon device resources and
 * processes send/receive work requests.
 *
 * TODO: Implement Falcon device initialization, QP management,
 * and completion polling.
 */
class FalconEngine {
 public:
  FalconEngine(int engine_idx, int gpu_idx)
      : engine_idx_(engine_idx), gpu_idx_(gpu_idx) {}
  ~FalconEngine() = default;

  // TODO: Implement engine main loop.
  void run() {}

 private:
  int engine_idx_;
  int gpu_idx_;
};

/**
 * @class FalconEndpoint
 * @brief Application-facing interface for Falcon transport.
 * Communicates with FalconEngine through Channel.
 * Each connection is identified by a unique flow_id.
 */
class FalconEndpoint {
 public:
  explicit FalconEndpoint(int num_engines = 1) : num_engines_(num_engines) {
    // TODO: Initialize Falcon devices and engines.
  }
  ~FalconEndpoint() = default;

  int get_num_devices() {
    // TODO: Enumerate Falcon devices.
    return 0;
  }

  // Listen on Falcon transport, return listen port and fd.
  std::tuple<uint16_t, int> uccl_listen() {
    // TODO: Implement Falcon listen.
    return {0, -1};
  }

  // Connect to a remote Falcon endpoint.
  ConnID uccl_connect(int local_vdev, int remote_vdev,
                      std::string remote_ip, uint16_t listen_port) {
    // TODO: Implement Falcon connection setup.
    return {};
  }

  // Accept a connection from a remote Falcon endpoint.
  ConnID uccl_accept(int local_vdev, int* remote_vdev,
                     std::string& remote_ip, int listen_fd) {
    // TODO: Implement Falcon accept.
    return {};
  }

  // Asynchronous send.
  PollCtx* uccl_send_async(ConnID conn_id, void const* data, int len,
                           Mhandle* mhandle) {
    // TODO: Implement Falcon async send.
    return nullptr;
  }

  // Asynchronous receive.
  PollCtx* uccl_recv_async(ConnID conn_id, void* data, int* len_p,
                           Mhandle* mhandle) {
    // TODO: Implement Falcon async receive.
    return nullptr;
  }

  // Register memory for Falcon transport.
  Mhandle* reg_mr(void* data, size_t size, int type) {
    // TODO: Implement Falcon memory registration.
    return nullptr;
  }

  // Deregister memory.
  void dereg_mr(Mhandle* mhandle) {
    // TODO: Implement Falcon memory deregistration.
  }

 private:
  int num_engines_;
};

// Convenience alias matching other transports.
using Endpoint = FalconEndpoint;

}  // namespace uccl
