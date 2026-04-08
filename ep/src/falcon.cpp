// Falcon transport implementation stub for UCCL EP.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// TODO: Implement all functions declared in falcon.hpp.
// Each function below is a placeholder that mirrors the corresponding
// RDMA function in ep/src/rdma.cpp but targeting Falcon hardware.

#include "falcon.hpp"
#include "util/debug.h"

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------

void falcon_setup(void* gpu_buffer, size_t size,
                  FalconConnectionInfo* local_info, int rank) {
  // TODO: Open Falcon device, create PD, register gpu_buffer,
  // populate local_info with QP number, keys, addresses.
  (void)gpu_buffer;
  (void)size;
  (void)local_info;
  (void)rank;
  UCCL_LOG(WARN) << "falcon_setup: not yet implemented";
}

void falcon_per_thread_init(ProxyCtx& S, void* gpu_buf, size_t bytes,
                            int rank, int thread_idx, int local_rank) {
  // TODO: Per-thread Falcon device/context init, analogous to
  // per_thread_rdma_init() in rdma.cpp.
  (void)S;
  (void)gpu_buf;
  (void)bytes;
  (void)rank;
  (void)thread_idx;
  (void)local_rank;
  UCCL_LOG(WARN) << "falcon_per_thread_init: not yet implemented";
}

void falcon_create_per_thread_qp(ProxyCtx& S, void* gpu_buffer, size_t size,
                                 FalconConnectionInfo* local_info, int rank,
                                 size_t num_rings, bool use_normal_mode,
                                 void* atomic_buffer_ptr) {
  // TODO: Create Falcon QP(s) for this proxy thread.
  (void)S;
  (void)gpu_buffer;
  (void)size;
  (void)local_info;
  (void)rank;
  (void)num_rings;
  (void)use_normal_mode;
  (void)atomic_buffer_ptr;
  UCCL_LOG(WARN) << "falcon_create_per_thread_qp: not yet implemented";
}

// ---------------------------------------------------------------------------
// Connection exchange
// ---------------------------------------------------------------------------

void falcon_recv_connection_info_as_server(
    int my_rank, int* actual_peer, int listen_fd,
    FalconConnectionInfo* remote_array) {
  // TODO: Accept TCP connection and receive remote FalconConnectionInfo.
  (void)my_rank;
  (void)actual_peer;
  (void)listen_fd;
  (void)remote_array;
  UCCL_LOG(WARN)
      << "falcon_recv_connection_info_as_server: not yet implemented";
}

void falcon_send_connection_info_as_client(int my_rank, int peer,
                                           char const* peer_ip,
                                           int peer_listen_port,
                                           FalconConnectionInfo* local) {
  // TODO: Connect via TCP and send local FalconConnectionInfo.
  (void)my_rank;
  (void)peer;
  (void)peer_ip;
  (void)peer_listen_port;
  (void)local;
  UCCL_LOG(WARN)
      << "falcon_send_connection_info_as_client: not yet implemented";
}

// ---------------------------------------------------------------------------
// QP state transitions
// ---------------------------------------------------------------------------

void falcon_modify_qp_to_init(ProxyCtx& S) {
  // TODO: Transition Falcon QP from RESET to INIT.
  (void)S;
  UCCL_LOG(WARN) << "falcon_modify_qp_to_init: not yet implemented";
}

void falcon_modify_qp_to_rtr(ProxyCtx& S, FalconConnectionInfo* remote,
                              bool use_normal_mode) {
  // TODO: Transition Falcon QP from INIT to RTR.
  (void)S;
  (void)remote;
  (void)use_normal_mode;
  UCCL_LOG(WARN) << "falcon_modify_qp_to_rtr: not yet implemented";
}

void falcon_modify_qp_to_rts(ProxyCtx& S, FalconConnectionInfo* local_info) {
  // TODO: Transition Falcon QP from RTR to RTS.
  (void)S;
  (void)local_info;
  UCCL_LOG(WARN) << "falcon_modify_qp_to_rts: not yet implemented";
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

void falcon_post_receive_buffer_for_imm(ProxyCtx& S) {
  // TODO: Post receive work requests for immediate data notifications.
  (void)S;
  UCCL_LOG(WARN)
      << "falcon_post_receive_buffer_for_imm: not yet implemented";
}

void falcon_post_async_batched(
    ProxyCtx& S, void* buf, size_t num_wrs,
    std::vector<uint64_t> const& wrs_to_post,
    std::vector<TransferCmd> const& cmds_to_post,
    std::vector<std::unique_ptr<ProxyCtx>>& ctxs, int my_rank,
    int thread_idx, bool use_normal_mode) {
  // TODO: Post a batch of RDMA write-with-imm (or Falcon equivalent)
  // work requests.
  (void)S;
  (void)buf;
  (void)num_wrs;
  (void)wrs_to_post;
  (void)cmds_to_post;
  (void)ctxs;
  (void)my_rank;
  (void)thread_idx;
  (void)use_normal_mode;
  UCCL_LOG(WARN) << "falcon_post_async_batched: not yet implemented";
}

// ---------------------------------------------------------------------------
// Completion polling
// ---------------------------------------------------------------------------

void falcon_local_poll_completions(
    ProxyCtx& S, std::unordered_set<uint64_t>& acked_wrs, int thread_idx,
    std::vector<ProxyCtx*>& ctx_by_tag) {
  // TODO: Poll local (send-side) Falcon CQ for completions.
  (void)S;
  (void)acked_wrs;
  (void)thread_idx;
  (void)ctx_by_tag;
}

void falcon_remote_poll_completions(
    ProxyCtx& S, int idx, CopyRingBuffer& g_ring,
    std::vector<ProxyCtx*>& ctx_by_tag, void* atomic_buffer_ptr,
    int num_ranks, int num_experts,
    std::set<PendingUpdate>& pending_atomic_updates, int my_rank,
    int num_nodes, bool use_normal_mode) {
  // TODO: Poll remote (recv-side) Falcon CQ for completions and
  // process incoming immediate data.
  (void)S;
  (void)idx;
  (void)g_ring;
  (void)ctx_by_tag;
  (void)atomic_buffer_ptr;
  (void)num_ranks;
  (void)num_experts;
  (void)pending_atomic_updates;
  (void)my_rank;
  (void)num_nodes;
  (void)use_normal_mode;
}

// ---------------------------------------------------------------------------
// Memory registration helpers
// ---------------------------------------------------------------------------

bool falcon_can_register_gpu_memory(int gpu_idx, size_t bytes) {
  // TODO: Probe whether Falcon NIC can register GPU memory of this size.
  (void)gpu_idx;
  (void)bytes;
  return false;
}

bool falcon_can_register_gpu_memory_for_atomics(int gpu_idx) {
  // TODO: Probe atomic buffer registration capability.
  (void)gpu_idx;
  return false;
}

void falcon_release_shared_resources(ProxyCtx& ctx, void* gpu_buf) {
  // TODO: Release shared Falcon resources (context/pd/mr) for this NIC.
  (void)ctx;
  (void)gpu_buf;
}
