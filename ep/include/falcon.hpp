#ifndef FALCON_HPP
#define FALCON_HPP

// Falcon transport stub for UCCL EP (Expert Parallelism).
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Mirrors ep/include/rdma.hpp — provides the same free-function
// interface used by the EP proxy threads, but targeting Falcon NICs
// instead of InfiniBand/RoCE verbs.

#include "common.hpp"
#include "proxy_ctx.hpp"
#include "ring_buffer.cuh"
#include "unistd.h"
#include <atomic>
#include <cassert>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

// Forward declaration — defined in rdma.hpp.
struct PendingUpdate;

// ---------------------------------------------------------------------------
// Falcon connection info — exchanged between peers during setup.
// Mirrors RDMAConnectionInfo for Falcon hardware.
// ---------------------------------------------------------------------------
struct FalconConnectionInfo {
  uint32_t qp_num;
  uint32_t psn;
  uint32_t rkey;
  uintptr_t addr;
  uint64_t len;

  // Falcon addressing (TODO: adjust for Falcon-specific fields).
  uint8_t gid[16];

  // Atomic buffer info (separate from main GPU buffer).
  uint32_t atomic_buffer_rkey = 0;
  uintptr_t atomic_buffer_addr = 0;
  uint64_t atomic_buffer_len = 0;
};

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------

// Setup Falcon resources (open device, create QP, register GPU memory, etc.)
void falcon_setup(void* gpu_buffer, size_t size,
                  FalconConnectionInfo* local_info, int rank);

// Per-proxy-thread Falcon initialization.
void falcon_per_thread_init(ProxyCtx& S, void* gpu_buf, size_t bytes,
                            int rank, int thread_idx, int local_rank);

// Create per-thread Falcon QP.
void falcon_create_per_thread_qp(ProxyCtx& S, void* gpu_buffer, size_t size,
                                 FalconConnectionInfo* local_info, int rank,
                                 size_t num_rings, bool use_normal_mode,
                                 void* atomic_buffer_ptr = nullptr);

// ---------------------------------------------------------------------------
// Connection exchange (TCP bootstrap)
// ---------------------------------------------------------------------------

void falcon_recv_connection_info_as_server(int my_rank, int* actual_peer,
                                           int listen_fd,
                                           FalconConnectionInfo* remote_array);

void falcon_send_connection_info_as_client(int my_rank, int peer,
                                           char const* peer_ip,
                                           int peer_listen_port,
                                           FalconConnectionInfo* local);

// ---------------------------------------------------------------------------
// QP state transitions
// ---------------------------------------------------------------------------

void falcon_modify_qp_to_init(ProxyCtx& S);
void falcon_modify_qp_to_rtr(ProxyCtx& S, FalconConnectionInfo* remote,
                              bool use_normal_mode);
void falcon_modify_qp_to_rts(ProxyCtx& S, FalconConnectionInfo* local_info);

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

void falcon_post_receive_buffer_for_imm(ProxyCtx& S);

void falcon_post_async_batched(ProxyCtx& S, void* buf, size_t num_wrs,
                               std::vector<uint64_t> const& wrs_to_post,
                               std::vector<TransferCmd> const& cmds_to_post,
                               std::vector<std::unique_ptr<ProxyCtx>>& ctxs,
                               int my_rank, int thread_idx,
                               bool use_normal_mode);

// ---------------------------------------------------------------------------
// Completion polling
// ---------------------------------------------------------------------------

void falcon_local_poll_completions(
    ProxyCtx& S, std::unordered_set<uint64_t>& acked_wrs, int thread_idx,
    std::vector<ProxyCtx*>& ctx_by_tag);

void falcon_remote_poll_completions(
    ProxyCtx& S, int idx, CopyRingBuffer& g_ring,
    std::vector<ProxyCtx*>& ctx_by_tag, void* atomic_buffer_ptr,
    int num_ranks, int num_experts,
    std::set<PendingUpdate>& pending_atomic_updates, int my_rank,
    int num_nodes, bool use_normal_mode = false);

// ---------------------------------------------------------------------------
// Memory registration helpers
// ---------------------------------------------------------------------------

// Returns true if GPU memory of the given size can be registered with
// the Falcon NIC on this node.
bool falcon_can_register_gpu_memory(int gpu_idx, size_t bytes);

// Returns true if atomic signaling buffers can be registered.
bool falcon_can_register_gpu_memory_for_atomics(int gpu_idx);

// Release shared Falcon resources for a given NIC + gpu_buf.
void falcon_release_shared_resources(ProxyCtx& ctx, void* gpu_buf);

#endif  // FALCON_HPP
