#pragma once

// Falcon Transport — ibverbs-based Host Driver
//
// On MEV, Falcon runs entirely in HW on the ACC (CPE — Custom Protocol
// Engine).  From the host's perspective the data path is standard ibverbs
// through the IRDMA driver loaded with crt_ena=1.  The Falcon HW handles:
//   - Reliable delivery (sliding windows, retransmission, ACK/NACK/EACK)
//   - Congestion control (HW RUE or SW RUE — Swift + PLB)
//   - PSP encryption / decryption per-connection
//   - Packet pacing / Tx gating
//
// The host-side UCCL plugin therefore is a thin wrapper around ibverbs:
//   create QP → exchange QPNs → modify QP RTR/RTS → post send/recv → poll CQ
//
// CRT-specific differences from plain RoCE:
//   - IRDMA loaded with crt_ena=1
//   - 256-byte Falcon header overhead (CRT_RDMA_HEADER) subtracted from MTU
//   - Ethernet MTU must be set ≥ RDMA_MTU + 400 (e.g. 4500 for MTU 4096)
//   - Connection setup intercepted by rtcmd/RCA on the ACC

#include "falcon_config.h"
#include "param.h"
#include "util/debug.h"
#include "util/util.h"
#include "util_buffpool.h"

#include <infiniband/verbs.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace uccl {
namespace falcon {

// Forward declarations
class FalconEngine;
class FalconEndpoint;

// ---------------------------------------------------------------------------
// Type Aliases
// ---------------------------------------------------------------------------

typedef uint64_t FlowID;
typedef uint64_t PeerID;

// ---------------------------------------------------------------------------
// IMMData — 32-bit immediate data encoding for RDMA WRITE WITH IMM
//
// Layout (matching RDMA transport):
//   [10:0]   FID  — 11-bit flow index
//   [14:11]  RID  — 4-bit request ID (0..kMaxReq-1)
//   [22:15]  CSN  — 8-bit chunk sequence number
//   [31]     HINT — 1-bit last-chunk flag
// ---------------------------------------------------------------------------

class IMMData {
 public:
  static constexpr int kFID = 0;
  static constexpr int kRID = 11;
  static constexpr int kCSN = 15;
  static constexpr int kHINT = 31;

  IMMData(uint32_t v = 0) : v_(v) {}

  uint32_t GetFID()  const { return (v_ >> kFID) & 0x7FF; }
  uint32_t GetRID()  const { return (v_ >> kRID) & 0xF; }
  uint32_t GetCSN()  const { return (v_ >> kCSN) & 0xFF; }
  uint32_t GetHINT() const { return (v_ >> kHINT) & 0x1; }

  void SetFID(uint32_t fid)   { v_ |= (fid  & 0x7FF) << kFID; }
  void SetRID(uint32_t rid)   { v_ |= (rid  & 0xF)   << kRID; }
  void SetCSN(uint32_t csn)   { v_ |= (csn  & 0xFF)  << kCSN; }
  void SetHINT(uint32_t hint) { v_ |= (hint & 0x1)   << kHINT; }

  uint32_t raw() const { return v_; }
 private:
  uint32_t v_;
};

// ---------------------------------------------------------------------------
// FifoItem — receiver buffer advertisement pushed to sender via RDMA WRITE
// ---------------------------------------------------------------------------

struct FifoItem {
  uint64_t addr;             // Receiver buffer address
  uint32_t size;             // Buffer size in bytes
  uint32_t rkey;             // Receiver's MR rkey
  uint32_t nmsgs;            // Number of messages in batch (typically 1)
  uint32_t rid;              // Request ID (sender encodes in imm_data)
  uint64_t idx;              // FIFO tail+1 — sync marker for sender polling
  uint32_t engine_offset;    // Target engine index
  char     padding[28];      // Pad to 64 bytes
};
static_assert(sizeof(FifoItem) == 64, "FifoItem must be 64 bytes");

struct RemFifo {
  FifoItem elems[kMaxReq][kMaxRecv];
  uint64_t fifo_tail;
};

// ---------------------------------------------------------------------------
// RecvRequest — pending receive tracked on receiver side for completion
// ---------------------------------------------------------------------------

struct RecvRequest {
  enum State { kUnused = 0, kRecv };
  State state = kUnused;
  FalconRequest* ureq = nullptr;      // Back-pointer to NCCL-layer request
  uint32_t received_bytes[kMaxRecv];  // Bytes received per gather entry
  uint32_t total_expected[kMaxRecv];  // Expected total per entry
  int n = 0;                          // Number of gather entries
  bool last_chunk_arrived = false;
};

// ---------------------------------------------------------------------------
// ConnID — connection identifier returned to the NCCL plugin layer
// ---------------------------------------------------------------------------

struct ConnID {
  void* context;      // Opaque pointer to FalconFlow
  int sock_fd;        // Bootstrap TCP socket
  FlowID flow_id;
  PeerID peer_id;
  int dev;            // Local device index
};

// ---------------------------------------------------------------------------
// Mhandle — memory registration handle (wrapping ibv_mr)
// ---------------------------------------------------------------------------

struct Mhandle {
  struct ibv_mr* mr;
};

// ---------------------------------------------------------------------------
// FalconRequest — async request descriptor for the NCCL plugin
// ---------------------------------------------------------------------------

enum ReqType : uint8_t {
  kReqNone = 0,
  kReqTx,
  kReqRx,
  kReqFlush,
};

struct FalconRequest {
  ReqType type;
  int n;
  std::atomic<bool> done{false};
  void* req_pool;

  // Number of chunks remaining (send-side; set to total, decremented on CQE)
  std::atomic<int> chunks_left{0};

  union {
    struct {
      int data_len;
      uint64_t raddr;   // Remote addr from FifoItem
      uint32_t rkey;    // Remote rkey from FifoItem
      uint32_t rid;     // Request ID from FifoItem
    } send;
    struct {
      int data_len[kMaxRecv];
    } recv;
  };
};

// ---------------------------------------------------------------------------
// FactoryDevice — per-IB-device metadata (populated once at init)
// ---------------------------------------------------------------------------

struct FactoryDevice {
  char ib_name[64];
  std::string local_ip_str;
  int numa_node = -1;
  struct ibv_context* context = nullptr;
  struct ibv_device_attr dev_attr{};
  struct ibv_port_attr port_attr{};
  uint8_t ib_port_num = 1;
  int gid_idx = 0;
  union ibv_gid gid{};
  double link_bw = 0.0;
  struct ibv_pd* pd = nullptr;
  bool is_roce = false;
  bool dma_buf_support = false;
  bool support_cq_ex = false;
};

// ---------------------------------------------------------------------------
// RemoteQPInfo — exchanged over bootstrap TCP during connection setup
// ---------------------------------------------------------------------------

struct RemoteQPInfo {
  uint32_t qpn;
  uint16_t lid;
  union ibv_gid gid;
  uint8_t port_num;
  uint8_t is_roce;
};

// ---------------------------------------------------------------------------
// FalconFlow — per-connection state (one per peer direction)
//
// Wraps PORT_ENTROPY ibverbs RC QPs.  The Falcon HW on the ACC intercepts
// QP creation via the RCA manager and establishes the CRT connection context
// transparently.
// ---------------------------------------------------------------------------

struct FalconFlow {
  FlowID flow_id = 0;
  PeerID peer_id = 0;
  int dev = 0;
  bool is_send = false;
  int engine_idx = 0;

  int num_qps = 0;
  std::vector<struct ibv_qp*> qps;
  RemoteQPInfo remote_info{};

  // ------ FIFO state (receiver buffer advertisement) ------
  // FIFO QP: separate RC QP used by receiver to push FifoItems to sender
  struct ibv_qp* fifo_qp = nullptr;
  struct ibv_mr* fifo_mr = nullptr;    // MR covering local RemFifo
  RemFifo* fifo = nullptr;             // Local FIFO buffer (sender reads)
  struct ibv_cq* fifo_cq = nullptr;
  // Remote FIFO address/rkey (for receiver to RDMA WRITE to sender's FIFO)
  uint64_t remote_fifo_addr = 0;
  uint32_t remote_fifo_rkey = 0;
  // Sender-side FIFO head (next slot to consume)
  uint32_t fifo_head = 0;

  // ------ Recv request tracking (receiver side) ------
  RecvRequest recv_reqs[kMaxReq];
  RecvRequest* alloc_recv_req() {
    for (int i = 0; i < kMaxReq; i++) {
      if (recv_reqs[i].state == RecvRequest::kUnused) return &recv_reqs[i];
    }
    return nullptr;
  }
  int get_recv_req_id(RecvRequest* r) const { return (int)(r - recv_reqs); }
  RecvRequest* get_recv_req_by_id(int id) { return &recv_reqs[id]; }
  void free_recv_req(RecvRequest* r) {
    r->state = RecvRequest::kUnused;
    r->ureq = nullptr;
    r->last_chunk_arrived = false;
    r->n = 0;
  }

  // ------ Chunk sequence numbers ------
  std::atomic<uint32_t> snd_csn{0};    // Sender: next CSN to assign

  // GPU-flush QP (receiver-side only, loopback RC READ for PCIe ordering)
  struct ibv_qp* gpu_flush_qp = nullptr;
  struct ibv_mr* gpu_flush_mr = nullptr;
  int gpu_flush_buf = 0;
  struct ibv_sge gpu_flush_sge{};
  struct ibv_cq* flow_cq = nullptr;

  // Round-robin QP selection counter
  std::atomic<uint32_t> next_qp_idx{0};

  // Select next QP for load-balanced ECMP spreading
  inline struct ibv_qp* select_qp() {
    uint32_t idx = next_qp_idx.fetch_add(1, std::memory_order_relaxed);
    return qps[idx % num_qps];
  }
};

// ---------------------------------------------------------------------------
// FalconEngine — per-engine resources and polling thread
//
// Each engine owns a shared send CQ, recv CQ, and SRQ.  All data-path QPs
// for flows assigned to this engine share these resources.  The engine
// thread polls the recv CQ and completes FalconRequests.
// ---------------------------------------------------------------------------

class FalconEngine {
 public:
  FalconEngine(int engine_idx, FalconEndpoint* endpoint);
  ~FalconEngine();

  void start();
  void stop();

  // Create ibverbs resources (send CQ, recv CQ, SRQ) for this engine
  int init_resources(int dev);

  // Create PORT_ENTROPY RC QPs for a flow, attached to this engine's CQ/SRQ
  int create_flow_qps(FalconFlow* flow);

  // Post an RDMA WRITE WITH IMM
  int post_rdma_write_imm(FalconFlow* flow, struct ibv_qp* qp,
                          void* local_addr, uint32_t lkey, uint32_t length,
                          uint64_t remote_addr, uint32_t rkey,
                          uint32_t imm_data, bool signal);

  // Post zero-byte recv WRs on SRQ (for receiving WRITE+IMM completions)
  int refill_srq(int count);

  // Poll recv CQ for completions
  int poll_recv_cq(FalconRequest** completed, int max_completions);

  // Register/unregister a flow for recv dispatch by the engine loop
  void register_flow(FalconFlow* flow);
  void unregister_flow(FlowID fid);

  struct ibv_cq_ex* send_cq_ex() { return send_cq_ex_; }
  struct ibv_cq_ex* recv_cq_ex() { return recv_cq_ex_; }
  struct ibv_srq* srq() { return srq_; }
  int engine_idx() const { return engine_idx_; }

 private:
  void engine_loop();

  int engine_idx_;
  FalconEndpoint* endpoint_;
  std::thread engine_thread_;
  std::atomic<bool> running_{false};

  struct ibv_cq_ex* send_cq_ex_ = nullptr;
  struct ibv_cq_ex* recv_cq_ex_ = nullptr;
  struct ibv_srq* srq_ = nullptr;

  std::atomic<uint32_t> sends_posted_{0};
  std::atomic<uint32_t> sends_completed_{0};

  // Flow registry: FID -> FalconFlow* for recv CQE dispatch
  std::mutex flow_map_mutex_;
  std::unordered_map<uint32_t, FalconFlow*> flow_map_;  // key = FID & 0x7FF
};

// ---------------------------------------------------------------------------
// FalconEndpoint — top-level transport endpoint (maps to RDMAEndpoint)
// ---------------------------------------------------------------------------

class FalconEndpoint {
 public:
  explicit FalconEndpoint(int num_engines = 0);
  ~FalconEndpoint();

  // -- Device Discovery --
  int get_num_devices() const;
  int get_best_dev_idx(int gpu_idx) const;
  FactoryDevice* get_device(int dev) const;

  bool initialize_engine_by_dev(int dev, bool force = false);

  // -- Connection Management --
  ConnID falcon_connect(int dev, int local_gpuidx, int remote_dev,
                        int remote_gpuidx, const std::string& remote_ip,
                        uint16_t remote_port);

  ConnID falcon_accept(int dev, int listen_fd, int local_gpuidx,
                       std::string& remote_ip, int* remote_dev,
                       int* remote_gpuidx);

  // -- Memory Registration --
  int falcon_regmr(void* flow_context, void* data, size_t size, int type,
                   Mhandle** mhandle);
  int falcon_regmr_dmabuf(void* flow_context, void* data, size_t size,
                          int type, uint64_t offset, int fd,
                          Mhandle** mhandle);
  void falcon_deregmr(Mhandle* mhandle);

  // -- Data Path --
  int falcon_send_async(void* flow_context, Mhandle* mh, void* data, int size,
                        FalconRequest* req);
  int falcon_recv_async(void* flow_context, Mhandle** mhs, void** data,
                        int* sizes, int n, FalconRequest* req);
  int falcon_flush(void* flow_context, Mhandle** mhs, void** data, int* sizes,
                   int n, FalconRequest* req);
  bool falcon_poll_request(FalconRequest* req);

 private:
  int init_devices();

  // QP state transitions
  int modify_qp_to_init(struct ibv_qp* qp, int dev);
  int modify_qp_to_rtr(struct ibv_qp* qp, int dev,
                       const RemoteQPInfo* remote, uint32_t remote_qpn);
  int modify_qp_to_rts(struct ibv_qp* qp);
  int modify_qp_to_rtr_gpuflush(struct ibv_qp* qp, int dev);

  // Exchange QP info over bootstrap TCP socket
  int exchange_qp_info(int sock_fd, int dev,
                       const std::vector<struct ibv_qp*>& local_qps,
                       RemoteQPInfo* remote_info,
                       std::vector<uint32_t>& remote_qpns);

  // FIFO QP setup + exchange
  int create_fifo_resources(FalconFlow* flow);
  int exchange_fifo_info(int sock_fd, FalconFlow* flow);

  // Post receiver FIFO item (receiver side — RDMA WRITE INLINE to sender)
  int post_fifo_item(FalconFlow* flow, void** data, int* sizes, int n,
                     Mhandle** mhs, uint32_t rid);

  // Assign flow to least-loaded engine
  int select_engine_for_flow();

  int num_engines_;
  int dev_ = -1;

  // Device registry (populated by init_devices)
  std::vector<std::unique_ptr<FactoryDevice>> devices_;

  // Engines (per-device)
  std::vector<std::unique_ptr<FalconEngine>> engines_;

  // Flow registry
  std::mutex flow_mutex_;
  std::unordered_map<FlowID, std::unique_ptr<FalconFlow>> flows_;
  FlowID next_flow_id_{1};
  PeerID next_peer_id_{1};

  // Engine load tracking for flow assignment
  std::vector<std::atomic<uint32_t>> engine_load_;

  bool initialized_ = false;
};

}  // namespace falcon

using FalconConnID = falcon::ConnID;

}  // namespace uccl
