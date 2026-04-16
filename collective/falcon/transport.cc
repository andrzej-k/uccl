// Falcon Transport — ibverbs Implementation
//
// Thin ibverbs wrapper for Falcon on MEV.  All protocol machinery
// (CC, retransmission, ACK, PSP) is handled by Falcon HW on the ACC.
// The host just does: create QP → post RDMA WRITE WITH IMM → poll CQ.

#include "transport.h"
#include "util/debug.h"
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

namespace uccl {
namespace falcon {

// ===== Helpers: TCP bootstrap message exchange =============================

static ssize_t send_message(int fd, const void* buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char*)buf + sent, len - sent);
    if (n <= 0) return -1;
    sent += n;
  }
  return sent;
}

static ssize_t receive_message(int fd, void* buf, size_t len) {
  size_t recvd = 0;
  while (recvd < len) {
    ssize_t n = read(fd, (char*)buf + recvd, len - recvd);
    if (n <= 0) return -1;
    recvd += n;
  }
  return recvd;
}

// ===== ibverbs speed helpers (from RDMA transport) =========================

static int ibvWidths[] = {1, 4, 8, 12, 2};
static int ibvSpeeds[] = {2500, 5000, 10000, 10000, 14000, 25000, 50000, 100000};

static int firstBitSet(int val, int max) {
  int i = 0;
  while (i < max && ((val & (1 << i)) == 0)) i++;
  return i;
}

static double compute_link_bw(struct ibv_port_attr* port_attr) {
  int width = ibvWidths[firstBitSet(port_attr->active_width,
                                    sizeof(ibvWidths) / sizeof(int) - 1)];
  int speed = ibvSpeeds[firstBitSet(port_attr->active_speed,
                                    sizeof(ibvSpeeds) / sizeof(int) - 1)];
  return (double)width * speed * 1e6 / 8.0;  // bytes/sec
}

// ===========================================================================
// FalconEngine
// ===========================================================================

FalconEngine::FalconEngine(int engine_idx, FalconEndpoint* endpoint)
    : engine_idx_(engine_idx), endpoint_(endpoint) {}

FalconEngine::~FalconEngine() {
  stop();
  if (srq_) ibv_destroy_srq(srq_);
  if (send_cq_ex_) ibv_destroy_cq(ibv_cq_ex_to_cq(send_cq_ex_));
  if (recv_cq_ex_) ibv_destroy_cq(ibv_cq_ex_to_cq(recv_cq_ex_));
}

int FalconEngine::init_resources(int dev) {
  auto* fdev = endpoint_->get_device(dev);
  if (!fdev) return -1;

  // Create send CQ
  struct ibv_cq_init_attr_ex cq_attr{};
  cq_attr.cqe = kCQDepth;
  cq_attr.wc_flags = IBV_WC_EX_WITH_IMM;
  send_cq_ex_ = ibv_create_cq_ex(fdev->context, &cq_attr);
  if (!send_cq_ex_) {
    // Fallback: older ibverbs — create normal CQ, cast
    struct ibv_cq* cq = ibv_create_cq(fdev->context, kCQDepth, nullptr,
                                      nullptr, 0);
    if (!cq) {
      UCCL_LOG(ERROR) << "Falcon: failed to create send CQ for engine "
                      << engine_idx_;
      return -1;
    }
    send_cq_ex_ = (struct ibv_cq_ex*)cq;
  }

  // Create recv CQ
  cq_attr = {};
  cq_attr.cqe = kCQDepth;
  cq_attr.wc_flags = IBV_WC_EX_WITH_IMM | IBV_WC_EX_WITH_QP_NUM;
  recv_cq_ex_ = ibv_create_cq_ex(fdev->context, &cq_attr);
  if (!recv_cq_ex_) {
    struct ibv_cq* cq = ibv_create_cq(fdev->context, kCQDepth, nullptr,
                                      nullptr, 0);
    if (!cq) {
      UCCL_LOG(ERROR) << "Falcon: failed to create recv CQ for engine "
                      << engine_idx_;
      return -1;
    }
    recv_cq_ex_ = (struct ibv_cq_ex*)cq;
  }

  // Create SRQ
  struct ibv_srq_init_attr srq_attr{};
  srq_attr.attr.max_wr = kSRQDepth;
  srq_attr.attr.max_sge = 1;
  srq_ = ibv_create_srq(fdev->pd, &srq_attr);
  if (!srq_) {
    UCCL_LOG(ERROR) << "Falcon: failed to create SRQ for engine "
                    << engine_idx_;
    return -1;
  }

  // Pre-fill SRQ with zero-byte recv WRs (for RC RDMA WRITE WITH IMM)
  refill_srq(kSRQDepth / 2);

  return 0;
}

int FalconEngine::create_flow_qps(FalconFlow* flow) {
  auto* fdev = endpoint_->get_device(flow->dev);
  if (!fdev) return -1;

  // Create multiple RC QPs per flow for ECMP path diversity.  Each QP maps
  // to a separate Falcon ordered connection with a unique flow label (derived
  // from QPN by the irdma driver), producing a distinct 5-tuple hash and
  // therefore a different network path through the fabric.  This is necessary
  // because MEV's HW RUE does not currently implement Falcon PLB
  // (Protective Load Balancing / randomize_path), so the NIC cannot
  // dynamically reroute a single connection onto a less-congested path.
  // Multi-QP spreading provides immediate static path diversity from the
  // first packet, complementing any future PLB enablement.
  int num_qps = ucclParamFalconPortEntropy();
  flow->num_qps = num_qps;
  flow->qps.resize(num_qps);

  for (int i = 0; i < num_qps; i++) {
    struct ibv_qp_init_attr qp_init_attr{};
    qp_init_attr.send_cq = ibv_cq_ex_to_cq(send_cq_ex_);
    qp_init_attr.recv_cq = ibv_cq_ex_to_cq(recv_cq_ex_);
    qp_init_attr.srq = srq_;
    qp_init_attr.qp_type = IBV_QPT_RC;  // Always RC for Falcon
    qp_init_attr.cap.max_send_wr = kMaxSendRecvWR;
    qp_init_attr.cap.max_recv_wr = 0;  // Using SRQ
    qp_init_attr.cap.max_send_sge = kMaxSge;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = kMaxInline;
    qp_init_attr.sq_sig_all = 0;

    flow->qps[i] = ibv_create_qp(fdev->pd, &qp_init_attr);
    if (!flow->qps[i]) {
      UCCL_LOG(ERROR) << "Falcon: failed to create QP " << i
                      << " for flow " << flow->flow_id;
      return -1;
    }

    // Transition to INIT
    if (endpoint_->modify_qp_to_init(flow->qps[i], flow->dev) != 0) {
      UCCL_LOG(ERROR) << "Falcon: failed to modify QP to INIT";
      return -1;
    }
  }

  return 0;
}

int FalconEngine::post_rdma_write_imm(FalconFlow* flow, struct ibv_qp* qp,
                                      void* local_addr, uint32_t lkey,
                                      uint32_t length,
                                      uint64_t remote_addr, uint32_t rkey,
                                      uint32_t imm_data, bool signal) {
  struct ibv_sge sge{};
  sge.addr = (uint64_t)local_addr;
  sge.length = length;
  sge.lkey = lkey;

  struct ibv_send_wr wr{};
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  wr.send_flags = signal ? IBV_SEND_SIGNALED : 0;
  wr.imm_data = htonl(imm_data);
  wr.wr.rdma.remote_addr = remote_addr;
  wr.wr.rdma.rkey = rkey;

  struct ibv_send_wr* bad_wr = nullptr;
  int ret = ibv_post_send(qp, &wr, &bad_wr);
  if (ret) {
    UCCL_LOG(ERROR) << "Falcon: ibv_post_send failed: " << ret;
  }
  return ret;
}

int FalconEngine::refill_srq(int count) {
  if (!srq_) return -1;

  // For RC RDMA WRITE WITH IMM, the recv WR can be zero-byte
  // (data goes directly to remote_addr; only the IMM triggers the CQE)
  struct ibv_sge sge{};
  sge.addr = 0;
  sge.length = 0;
  sge.lkey = 0;

  std::vector<struct ibv_recv_wr> wrs(count);
  for (int i = 0; i < count; i++) {
    wrs[i].wr_id = 0;
    wrs[i].sg_list = &sge;
    wrs[i].num_sge = 0;  // Zero-byte receive
    wrs[i].next = (i + 1 < count) ? &wrs[i + 1] : nullptr;
  }

  struct ibv_recv_wr* bad_wr = nullptr;
  int ret = ibv_post_srq_recv(srq_, &wrs[0], &bad_wr);
  if (ret) {
    UCCL_LOG(ERROR) << "Falcon: ibv_post_srq_recv failed: " << ret;
  }
  return ret;
}

void FalconEngine::start() {
  if (running_.exchange(true)) return;
  engine_thread_ = std::thread(&FalconEngine::engine_loop, this);
}

void FalconEngine::stop() {
  if (!running_.exchange(false)) return;
  if (engine_thread_.joinable()) {
    engine_thread_.join();
  }
}

void FalconEngine::register_flow(FalconFlow* flow) {
  std::lock_guard<std::mutex> lock(flow_map_mutex_);
  flow_map_[flow->flow_id & 0x7FF] = flow;
}

void FalconEngine::unregister_flow(FlowID fid) {
  std::lock_guard<std::mutex> lock(flow_map_mutex_);
  flow_map_.erase(fid & 0x7FF);
}

void FalconEngine::engine_loop() {
  // The engine thread polls both recv CQ and send CQ.
  //
  // Recv CQ: Each IBV_WC_RECV_RDMA_WITH_IMM carries imm_data encoding
  //   FID (flow), RID (request), CSN (chunk seq), HINT (last chunk).
  //   We decode it, look up the flow + RecvRequest, accumulate bytes,
  //   and mark req->done when HINT=1.
  //
  // Send CQ: wr_id carries the FalconRequest*.  We decrement chunks_left
  //   and mark req->done when it hits 0.

  struct ibv_wc wcs[kMaxBatchCQ];
  struct ibv_cq* recv_cq = ibv_cq_ex_to_cq(recv_cq_ex_);
  struct ibv_cq* send_cq = ibv_cq_ex_to_cq(send_cq_ex_);
  int srq_refill_pending = 0;

  while (running_.load(std::memory_order_relaxed)) {
    // ---- Poll recv CQ ----
    int n = ibv_poll_cq(recv_cq, kMaxBatchCQ, wcs);
    if (n < 0) {
      UCCL_LOG(ERROR) << "Falcon: recv ibv_poll_cq error";
      continue;
    }

    for (int i = 0; i < n; i++) {
      if (wcs[i].status != IBV_WC_SUCCESS) {
        UCCL_LOG(ERROR) << "Falcon: recv CQE error status=" << wcs[i].status
                        << " qpn=" << wcs[i].qp_num;
        srq_refill_pending++;
        continue;
      }

      if (wcs[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        IMMData imm(ntohl(wcs[i].imm_data));
        uint32_t fid  = imm.GetFID();
        uint32_t rid  = imm.GetRID();
        uint32_t hint = imm.GetHINT();

        FalconFlow* flow = nullptr;
        {
          std::lock_guard<std::mutex> lock(flow_map_mutex_);
          auto it = flow_map_.find(fid);
          if (it != flow_map_.end()) flow = it->second;
        }

        if (flow) {
          auto* rr = flow->get_recv_req_by_id(rid);
          if (rr->state == RecvRequest::kRecv && rr->ureq) {
            // Accumulate received bytes (wcs[i].byte_len is the data written)
            rr->received_bytes[0] += wcs[i].byte_len;

            if (hint) {
              rr->last_chunk_arrived = true;
              // Fill actual received length in NCCL request
              auto* ureq = rr->ureq;
              for (int j = 0; j < rr->n; j++) {
                ureq->recv.data_len[j] = rr->received_bytes[j];
              }
              ureq->done.store(true, std::memory_order_release);
              flow->free_recv_req(rr);
            }
          }
        }

        srq_refill_pending++;
      }
    }

    // Batch-refill SRQ
    if (srq_refill_pending > 0) {
      refill_srq(srq_refill_pending);
      srq_refill_pending = 0;
    }

    // ---- Poll send CQ ----
    n = ibv_poll_cq(send_cq, kMaxBatchCQ, wcs);
    for (int i = 0; i < n; i++) {
      if (wcs[i].status != IBV_WC_SUCCESS) {
        UCCL_LOG(ERROR) << "Falcon: send CQE error status=" << wcs[i].status;
      }
      if (wcs[i].wr_id != 0) {
        auto* req = reinterpret_cast<FalconRequest*>(wcs[i].wr_id);
        int left = req->chunks_left.fetch_sub(1, std::memory_order_acq_rel);
        if (left == 1) {
          // Last chunk completed
          req->done.store(true, std::memory_order_release);
        }
      }
    }

    if (n == 0) {
      std::this_thread::yield();
    }
  }
}

// ===========================================================================
// FalconEndpoint — Device Discovery
// ===========================================================================

FalconEndpoint::FalconEndpoint(int num_engines)
    : num_engines_(num_engines > 0 ? num_engines
                                   : ucclParamFalconNumEngines()) {}

FalconEndpoint::~FalconEndpoint() {
  for (auto& engine : engines_) {
    engine->stop();
  }
  for (auto& [fid, flow] : flows_) {
    for (auto* qp : flow->qps) {
      if (qp) ibv_destroy_qp(qp);
    }
    if (flow->fifo_qp) ibv_destroy_qp(flow->fifo_qp);
    if (flow->fifo_mr) {
      munmap(flow->fifo_mr->addr, flow->fifo_mr->length);
      ibv_dereg_mr(flow->fifo_mr);
    }
    if (flow->fifo_cq) ibv_destroy_cq(flow->fifo_cq);
    if (flow->gpu_flush_qp) ibv_destroy_qp(flow->gpu_flush_qp);
    if (flow->gpu_flush_mr) {
      munmap(flow->gpu_flush_mr->addr, flow->gpu_flush_mr->length);
      ibv_dereg_mr(flow->gpu_flush_mr);
    }
    if (flow->flow_cq) ibv_destroy_cq(flow->flow_cq);
  }
  // PDs and contexts cleaned up with device close
  for (auto& dev : devices_) {
    if (dev->pd) ibv_dealloc_pd(dev->pd);
    if (dev->context) ibv_close_device(dev->context);
  }
}

int FalconEndpoint::init_devices() {
  int num_ib_devs = 0;
  struct ibv_device** ib_devs = ibv_get_device_list(&num_ib_devs);
  if (!ib_devs || num_ib_devs == 0) {
    UCCL_LOG(ERROR) << "Falcon: no IB devices found";
    return -1;
  }

  for (int d = 0; d < num_ib_devs; d++) {
    struct ibv_context* ctx = ibv_open_device(ib_devs[d]);
    if (!ctx) continue;

    auto fdev = std::make_unique<FactoryDevice>();
    strncpy(fdev->ib_name, ibv_get_device_name(ib_devs[d]),
            sizeof(fdev->ib_name) - 1);

    if (ibv_query_device(ctx, &fdev->dev_attr) != 0) {
      ibv_close_device(ctx);
      continue;
    }

    // Probe all ports for an active one
    bool found_active = false;
    for (uint8_t p = 1; p <= fdev->dev_attr.phys_port_cnt; p++) {
      if (ibv_query_port(ctx, p, &fdev->port_attr) != 0) continue;
      if (fdev->port_attr.state != IBV_PORT_ACTIVE) continue;

      fdev->ib_port_num = p;
      fdev->is_roce =
          (fdev->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);

      // Get GID
      if (fdev->is_roce) {
        // Try GID index 3 first (RoCE v2), then 0
        for (int gid_try : {3, 1, 0}) {
          union ibv_gid gid{};
          if (ibv_query_gid(ctx, p, gid_try, &gid) == 0 &&
              gid.global.interface_id != 0) {
            fdev->gid = gid;
            fdev->gid_idx = gid_try;
            break;
          }
        }
      } else {
        ibv_query_gid(ctx, p, 0, &fdev->gid);
        fdev->gid_idx = 0;
      }

      fdev->link_bw = compute_link_bw(&fdev->port_attr);
      found_active = true;
      break;
    }

    if (!found_active) {
      ibv_close_device(ctx);
      continue;
    }

    fdev->context = ctx;

    // Allocate PD
    fdev->pd = ibv_alloc_pd(ctx);
    if (!fdev->pd) {
      UCCL_LOG(ERROR) << "Falcon: ibv_alloc_pd failed for " << fdev->ib_name;
      ibv_close_device(ctx);
      continue;
    }

    // Probe DMA-BUF support
    fdev->dma_buf_support = false;
    struct ibv_mr* test_mr = ibv_reg_dmabuf_mr(fdev->pd, 0, 0, 0, -1, 0);
    if (!test_mr && errno == EOPNOTSUPP) {
      fdev->dma_buf_support = false;
    } else if (!test_mr && errno != EOPNOTSUPP) {
      // ibv_reg_dmabuf_mr exists but failed for other reason → supported
      fdev->dma_buf_support = true;
    } else {
      if (test_mr) ibv_dereg_mr(test_mr);
      fdev->dma_buf_support = true;
    }

    // Probe CQ-EX support
    struct ibv_cq_init_attr_ex cq_attr{};
    cq_attr.cqe = 2;
    struct ibv_cq_ex* test_cq = ibv_create_cq_ex(ctx, &cq_attr);
    fdev->support_cq_ex = (test_cq != nullptr);
    if (test_cq) ibv_destroy_cq(ibv_cq_ex_to_cq(test_cq));

    UCCL_LOG(INFO) << "Falcon: found device " << fdev->ib_name
                   << " port " << (int)fdev->ib_port_num
                   << " roce=" << fdev->is_roce
                   << " link_bw=" << fdev->link_bw / 1e9 << "Gbps"
                   << " dma_buf=" << fdev->dma_buf_support;

    devices_.push_back(std::move(fdev));
  }

  ibv_free_device_list(ib_devs);
  return devices_.size();
}

int FalconEndpoint::get_num_devices() const {
  return devices_.size();
}

int FalconEndpoint::get_best_dev_idx(int gpu_idx) const {
  // TODO: NUMA-aware GPU-NIC affinity mapping
  if (devices_.empty()) return 0;
  return gpu_idx % devices_.size();
}

FactoryDevice* FalconEndpoint::get_device(int dev) const {
  if (dev < 0 || dev >= (int)devices_.size()) return nullptr;
  return devices_[dev].get();
}

bool FalconEndpoint::initialize_engine_by_dev(int dev, bool force) {
  if (initialized_ && !force) return false;

  // Discover devices on first call
  if (devices_.empty()) {
    if (init_devices() <= 0) {
      UCCL_LOG(ERROR) << "Falcon: no usable IB devices";
      return false;
    }
  }

  if (dev < 0 || dev >= (int)devices_.size()) {
    UCCL_LOG(ERROR) << "Falcon: invalid dev index " << dev;
    return false;
  }

  dev_ = dev;

  // Create engines with ibverbs resources
  engines_.clear();
  engine_load_.resize(num_engines_);
  for (int i = 0; i < num_engines_; i++) {
    engine_load_[i].store(0);
    auto engine = std::make_unique<FalconEngine>(i, this);
    if (engine->init_resources(dev) != 0) {
      UCCL_LOG(ERROR) << "Falcon: engine " << i << " init failed";
      return false;
    }
    engine->start();
    engines_.push_back(std::move(engine));
  }

  initialized_ = true;
  UCCL_LOG(INFO) << "Falcon: initialized dev " << dev
                 << " (" << devices_[dev]->ib_name << ")"
                 << " with " << num_engines_ << " engines";
  return true;
}

// ===========================================================================
// QP State Transitions
// ===========================================================================

int FalconEndpoint::modify_qp_to_init(struct ibv_qp* qp, int dev) {
  auto* fdev = get_device(dev);
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = fdev->ib_port_num;
  attr.qp_access_flags =
      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;

  return ibv_modify_qp(qp, &attr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                           IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

int FalconEndpoint::modify_qp_to_rtr(struct ibv_qp* qp, int dev,
                                     const RemoteQPInfo* remote,
                                     uint32_t remote_qpn) {
  auto* fdev = get_device(dev);
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_4096;  // CRT MTU; actual payload after 256B header
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;

  // Address vector
  if (fdev->is_roce) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = remote->gid;
    attr.ah_attr.grh.sgid_index = fdev->gid_idx;
    attr.ah_attr.grh.hop_limit = 0xFF;
    attr.ah_attr.grh.traffic_class = ucclParamFalconRoceTrafficClass();
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.sl = ucclParamFalconRoceServiceLevel();
  } else {
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote->lid;
    attr.ah_attr.sl = 0;
  }
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = fdev->ib_port_num;

  // RC specific
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
              IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
              IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

  return ibv_modify_qp(qp, &attr, flags);
}

int FalconEndpoint::modify_qp_to_rts(struct ibv_qp* qp) {
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = 0;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.max_rd_atomic = 1;

  return ibv_modify_qp(qp, &attr,
                       IBV_QP_STATE | IBV_QP_SQ_PSN |
                           IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                           IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

int FalconEndpoint::modify_qp_to_rtr_gpuflush(struct ibv_qp* qp, int dev) {
  auto* fdev = get_device(dev);
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_4096;
  attr.dest_qp_num = qp->qp_num;  // Loopback: points at itself
  attr.rq_psn = 0;

  if (fdev->is_roce) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = fdev->gid;  // Own GID
    attr.ah_attr.grh.sgid_index = fdev->gid_idx;
    attr.ah_attr.grh.hop_limit = 0xFF;
  }
  attr.ah_attr.port_num = fdev->ib_port_num;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;

  return ibv_modify_qp(qp, &attr,
                       IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                           IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

// ===========================================================================
// Connection Setup
// ===========================================================================

int FalconEndpoint::exchange_qp_info(int sock_fd, int dev,
                                     const std::vector<struct ibv_qp*>& local_qps,
                                     RemoteQPInfo* remote_info,
                                     std::vector<uint32_t>& remote_qpns) {
  auto* fdev = get_device(dev);

  // Send local info: {lid, gid, port_num, is_roce, num_qps, qpn[0..N]}
  struct {
    uint16_t lid;
    union ibv_gid gid;
    uint8_t port_num;
    uint8_t is_roce;
    uint32_t num_qps;
  } local_hdr{};

  local_hdr.lid = fdev->port_attr.lid;
  local_hdr.gid = fdev->gid;
  local_hdr.port_num = fdev->ib_port_num;
  local_hdr.is_roce = fdev->is_roce ? 1 : 0;
  local_hdr.num_qps = local_qps.size();

  if (send_message(sock_fd, &local_hdr, sizeof(local_hdr)) < 0) return -1;

  // Send all QPNs
  std::vector<uint32_t> local_qpns(local_qps.size());
  for (size_t i = 0; i < local_qps.size(); i++) {
    local_qpns[i] = local_qps[i]->qp_num;
  }
  if (send_message(sock_fd, local_qpns.data(),
                   local_qpns.size() * sizeof(uint32_t)) < 0)
    return -1;

  // Receive remote info
  decltype(local_hdr) remote_hdr{};
  if (receive_message(sock_fd, &remote_hdr, sizeof(remote_hdr)) < 0) return -1;

  remote_info->lid = remote_hdr.lid;
  remote_info->gid = remote_hdr.gid;
  remote_info->port_num = remote_hdr.port_num;
  remote_info->is_roce = remote_hdr.is_roce;

  // Receive remote QPNs
  remote_qpns.resize(remote_hdr.num_qps);
  if (receive_message(sock_fd, remote_qpns.data(),
                      remote_hdr.num_qps * sizeof(uint32_t)) < 0)
    return -1;

  return 0;
}

int FalconEndpoint::select_engine_for_flow() {
  // Find least-loaded engine
  int best = 0;
  uint32_t min_load = engine_load_[0].load();
  for (int i = 1; i < num_engines_; i++) {
    uint32_t load = engine_load_[i].load();
    if (load < min_load) {
      min_load = load;
      best = i;
    }
  }
  engine_load_[best].fetch_add(1);
  return best;
}

// ===========================================================================
// FIFO Resources
// ===========================================================================

int FalconEndpoint::create_fifo_resources(FalconFlow* flow) {
  auto* fdev = get_device(flow->dev);
  if (!fdev) return -1;

  // Allocate FIFO CQ (shared with GPU flush CQ if recv side)
  if (!flow->fifo_cq) {
    flow->fifo_cq = ibv_create_cq(fdev->context, 4096, nullptr, nullptr, 0);
    if (!flow->fifo_cq) {
      UCCL_LOG(ERROR) << "Falcon: failed to create FIFO CQ";
      return -1;
    }
  }

  // Allocate local RemFifo buffer + register MR
  size_t fifo_size = sizeof(RemFifo);
  void* fifo_buf = mmap(nullptr, fifo_size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (fifo_buf == MAP_FAILED) return -1;
  memset(fifo_buf, 0, fifo_size);

  flow->fifo_mr = ibv_reg_mr(fdev->pd, fifo_buf, fifo_size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!flow->fifo_mr) {
    UCCL_LOG(ERROR) << "Falcon: failed to register FIFO MR";
    munmap(fifo_buf, fifo_size);
    return -1;
  }
  flow->fifo = reinterpret_cast<RemFifo*>(fifo_buf);

  // Create FIFO QP (separate RC QP for control messages)
  struct ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq = flow->fifo_cq;
  qp_attr.recv_cq = flow->fifo_cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = kMaxSendRecvWR;
  qp_attr.cap.max_recv_wr = kMaxSendRecvWR;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;
  qp_attr.cap.max_inline_data = sizeof(FifoItem) * kMaxRecv;

  flow->fifo_qp = ibv_create_qp(fdev->pd, &qp_attr);
  if (!flow->fifo_qp) {
    UCCL_LOG(ERROR) << "Falcon: failed to create FIFO QP";
    return -1;
  }

  // Transition FIFO QP to INIT
  if (modify_qp_to_init(flow->fifo_qp, flow->dev) != 0) {
    UCCL_LOG(ERROR) << "Falcon: FIFO QP INIT failed";
    return -1;
  }

  return 0;
}

int FalconEndpoint::exchange_fifo_info(int sock_fd, FalconFlow* flow) {
  // Exchange FIFO QPN
  uint32_t local_fifo_qpn = flow->fifo_qp->qp_num;
  if (send_message(sock_fd, &local_fifo_qpn, sizeof(uint32_t)) < 0) return -1;

  uint32_t remote_fifo_qpn = 0;
  if (receive_message(sock_fd, &remote_fifo_qpn, sizeof(uint32_t)) < 0)
    return -1;

  // Exchange FIFO MR addr + rkey (12 bytes)
  struct {
    uint64_t addr;
    uint32_t rkey;
  } local_fifo_meta{
      reinterpret_cast<uint64_t>(flow->fifo_mr->addr),
      flow->fifo_mr->rkey};

  if (send_message(sock_fd, &local_fifo_meta, sizeof(local_fifo_meta)) < 0)
    return -1;

  decltype(local_fifo_meta) remote_fifo_meta{};
  if (receive_message(sock_fd, &remote_fifo_meta, sizeof(remote_fifo_meta)) < 0)
    return -1;

  flow->remote_fifo_addr = remote_fifo_meta.addr;
  flow->remote_fifo_rkey = remote_fifo_meta.rkey;

  // Transition FIFO QP to RTR using remote FIFO QPN
  RemoteQPInfo fifo_remote{};
  fifo_remote.lid = flow->remote_info.lid;
  fifo_remote.gid = flow->remote_info.gid;
  fifo_remote.port_num = flow->remote_info.port_num;
  fifo_remote.is_roce = flow->remote_info.is_roce;

  if (modify_qp_to_rtr(flow->fifo_qp, flow->dev, &fifo_remote,
                        remote_fifo_qpn) != 0) {
    UCCL_LOG(ERROR) << "Falcon: FIFO QP RTR failed";
    return -1;
  }
  if (modify_qp_to_rts(flow->fifo_qp) != 0) {
    UCCL_LOG(ERROR) << "Falcon: FIFO QP RTS failed";
    return -1;
  }

  UCCL_LOG(INFO) << "Falcon: FIFO exchange complete, remote_addr=0x"
                 << std::hex << flow->remote_fifo_addr
                 << " rkey=0x" << flow->remote_fifo_rkey << std::dec;
  return 0;
}

int FalconEndpoint::post_fifo_item(FalconFlow* flow, void** data, int* sizes,
                                   int n, Mhandle** mhs, uint32_t rid) {
  RemFifo* fifo = flow->fifo;
  int slot = fifo->fifo_tail % kMaxReq;
  FifoItem* elems = fifo->elems[slot];

  for (int i = 0; i < n; i++) {
    elems[i].addr = reinterpret_cast<uint64_t>(data[i]);
    elems[i].rkey = mhs[i]->mr->rkey;
    elems[i].size = sizes[i];
    elems[i].nmsgs = n;
    elems[i].rid = rid;
    elems[i].engine_offset = flow->engine_idx;
    elems[i].idx = fifo->fifo_tail + 1;  // Sync marker
  }

  // RDMA WRITE INLINE to remote sender's FIFO
  struct ibv_sge sge{};
  sge.addr = reinterpret_cast<uint64_t>(elems);
  sge.length = n * sizeof(FifoItem);
  sge.lkey = flow->fifo_mr->lkey;

  struct ibv_send_wr wr{};
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_INLINE;
  wr.wr.rdma.remote_addr =
      flow->remote_fifo_addr + slot * kMaxRecv * sizeof(FifoItem);
  wr.wr.rdma.rkey = flow->remote_fifo_rkey;

  // Signal occasionally to drain FIFO CQ
  if (slot == 0) {
    wr.send_flags |= IBV_SEND_SIGNALED;
  }

  struct ibv_send_wr* bad_wr = nullptr;
  int ret = ibv_post_send(flow->fifo_qp, &wr, &bad_wr);
  if (ret) {
    UCCL_LOG(ERROR) << "Falcon: FIFO ibv_post_send failed: " << ret;
    return -1;
  }

  fifo->fifo_tail++;

  // Drain FIFO CQ if signaled
  if (slot == 0) {
    struct ibv_wc wc{};
    while (ibv_poll_cq(flow->fifo_cq, 1, &wc) > 0) {
      // Just drain
    }
  }

  return 0;
}

ConnID FalconEndpoint::falcon_connect(int dev, int local_gpuidx,
                                      int remote_dev, int remote_gpuidx,
                                      const std::string& remote_ip,
                                      uint16_t remote_port) {
  ConnID conn_id{};
  conn_id.dev = dev;

  // TCP bootstrap connection
  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    UCCL_LOG(ERROR) << "Falcon connect: socket() failed";
    return conn_id;
  }
  int flag = 1;
  setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

  struct sockaddr_in serv_addr{};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(remote_port);
  inet_pton(AF_INET, remote_ip.c_str(), &serv_addr.sin_addr);

  if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    UCCL_LOG(ERROR) << "Falcon connect: connect() to " << remote_ip << ":"
                    << remote_port << " failed";
    close(sock_fd);
    return conn_id;
  }

  // Exchange dev/gpuidx
  struct { int dev; int gpuidx; } local_meta{dev, local_gpuidx};
  send_message(sock_fd, &local_meta, sizeof(local_meta));
  struct { int dev; int gpuidx; } remote_meta{};
  receive_message(sock_fd, &remote_meta, sizeof(remote_meta));

  // Create flow
  auto flow = std::make_unique<FalconFlow>();
  FlowID fid;
  {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    fid = next_flow_id_++;
  }
  flow->flow_id = fid;
  flow->peer_id = next_peer_id_++;
  flow->dev = dev;
  flow->is_send = true;
  flow->engine_idx = select_engine_for_flow();

  // Create QPs on the assigned engine
  auto* engine = engines_[flow->engine_idx].get();
  if (engine->create_flow_qps(flow.get()) != 0) {
    close(sock_fd);
    return conn_id;
  }

  // Exchange QP info with remote peer
  std::vector<uint32_t> remote_qpns;
  if (exchange_qp_info(sock_fd, dev, flow->qps,
                       &flow->remote_info, remote_qpns) != 0) {
    UCCL_LOG(ERROR) << "Falcon connect: QP info exchange failed";
    close(sock_fd);
    return conn_id;
  }

  // Transition all QPs to RTR then RTS
  for (size_t i = 0; i < flow->qps.size(); i++) {
    uint32_t rqpn = (i < remote_qpns.size()) ? remote_qpns[i] : remote_qpns[0];
    if (modify_qp_to_rtr(flow->qps[i], dev, &flow->remote_info, rqpn) != 0) {
      UCCL_LOG(ERROR) << "Falcon connect: QP " << i << " RTR failed";
      close(sock_fd);
      return conn_id;
    }
    if (modify_qp_to_rts(flow->qps[i]) != 0) {
      UCCL_LOG(ERROR) << "Falcon connect: QP " << i << " RTS failed";
      close(sock_fd);
      return conn_id;
    }
  }

  // Create FIFO resources and exchange with peer
  if (create_fifo_resources(flow.get()) != 0) {
    UCCL_LOG(ERROR) << "Falcon connect: FIFO resource creation failed";
    close(sock_fd);
    return conn_id;
  }
  if (exchange_fifo_info(sock_fd, flow.get()) != 0) {
    UCCL_LOG(ERROR) << "Falcon connect: FIFO exchange failed";
    close(sock_fd);
    return conn_id;
  }

  // Register flow with engine for recv CQE dispatch
  engine->register_flow(flow.get());

  conn_id.context = flow.get();
  conn_id.sock_fd = sock_fd;
  conn_id.flow_id = fid;
  conn_id.peer_id = flow->peer_id;

  {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    flows_[fid] = std::move(flow);
  }

  UCCL_LOG(INFO) << "Falcon: connected to " << remote_ip << ":" << remote_port
                 << " flow_id=" << fid
                 << " num_qps=" << ucclParamFalconPortEntropy();
  return conn_id;
}

ConnID FalconEndpoint::falcon_accept(int dev, int listen_fd, int local_gpuidx,
                                     std::string& remote_ip, int* remote_dev,
                                     int* remote_gpuidx) {
  ConnID conn_id{};
  conn_id.dev = dev;

  struct sockaddr_in cli_addr{};
  socklen_t cli_len = sizeof(cli_addr);
  int sock_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
  if (sock_fd < 0) {
    UCCL_LOG(ERROR) << "Falcon accept: accept() failed";
    return conn_id;
  }
  int flag = 1;
  setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

  char ip_buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &cli_addr.sin_addr, ip_buf, INET_ADDRSTRLEN);
  remote_ip = ip_buf;

  // Exchange dev/gpuidx
  struct { int dev; int gpuidx; } remote_meta{};
  receive_message(sock_fd, &remote_meta, sizeof(remote_meta));
  if (remote_dev) *remote_dev = remote_meta.dev;
  if (remote_gpuidx) *remote_gpuidx = remote_meta.gpuidx;

  struct { int dev; int gpuidx; } local_meta{dev, local_gpuidx};
  send_message(sock_fd, &local_meta, sizeof(local_meta));

  // Create flow
  auto flow = std::make_unique<FalconFlow>();
  FlowID fid;
  {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    fid = next_flow_id_++;
  }
  flow->flow_id = fid;
  flow->peer_id = next_peer_id_++;
  flow->dev = dev;
  flow->is_send = false;
  flow->engine_idx = select_engine_for_flow();

  auto* engine = engines_[flow->engine_idx].get();
  if (engine->create_flow_qps(flow.get()) != 0) {
    close(sock_fd);
    return conn_id;
  }

  // Exchange QP info
  std::vector<uint32_t> remote_qpns;
  if (exchange_qp_info(sock_fd, dev, flow->qps,
                       &flow->remote_info, remote_qpns) != 0) {
    UCCL_LOG(ERROR) << "Falcon accept: QP info exchange failed";
    close(sock_fd);
    return conn_id;
  }

  // RTR/RTS
  for (size_t i = 0; i < flow->qps.size(); i++) {
    uint32_t rqpn = (i < remote_qpns.size()) ? remote_qpns[i] : remote_qpns[0];
    if (modify_qp_to_rtr(flow->qps[i], dev, &flow->remote_info, rqpn) != 0) {
      UCCL_LOG(ERROR) << "Falcon accept: QP " << i << " RTR failed";
      close(sock_fd);
      return conn_id;
    }
    if (modify_qp_to_rts(flow->qps[i]) != 0) {
      UCCL_LOG(ERROR) << "Falcon accept: QP " << i << " RTS failed";
      close(sock_fd);
      return conn_id;
    }
  }

  // Create FIFO resources and exchange with peer
  if (create_fifo_resources(flow.get()) != 0) {
    UCCL_LOG(ERROR) << "Falcon accept: FIFO resource creation failed";
    close(sock_fd);
    return conn_id;
  }
  if (exchange_fifo_info(sock_fd, flow.get()) != 0) {
    UCCL_LOG(ERROR) << "Falcon accept: FIFO exchange failed";
    close(sock_fd);
    return conn_id;
  }

  // Register flow with engine for recv CQE dispatch
  engine->register_flow(flow.get());

  // Create GPU-flush QP (receiver only, loopback RC READ)
  auto* fdev = get_device(dev);
  flow->flow_cq = ibv_create_cq(fdev->context, 64, nullptr, nullptr, 0);
  if (!flow->flow_cq) {
    UCCL_LOG(ERROR) << "Falcon accept: failed to create flow CQ";
    close(sock_fd);
    return conn_id;
  }

  {
    struct ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq = flow->flow_cq;
    qp_attr.recv_cq = flow->flow_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = kMaxSendRecvWR;
    qp_attr.cap.max_recv_wr = kMaxSendRecvWR;
    qp_attr.cap.max_send_sge = kMaxSge;
    qp_attr.cap.max_recv_sge = kMaxSge;
    flow->gpu_flush_qp = ibv_create_qp(fdev->pd, &qp_attr);
  }

  if (flow->gpu_flush_qp) {
    // Allocate flush buffer + MR
    void* buf = mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    flow->gpu_flush_mr = ibv_reg_mr(fdev->pd, buf, sizeof(int),
                                    IBV_ACCESS_LOCAL_WRITE |
                                        IBV_ACCESS_REMOTE_WRITE);
    flow->gpu_flush_sge.addr = (uint64_t)buf;
    flow->gpu_flush_sge.length = 1;
    flow->gpu_flush_sge.lkey = flow->gpu_flush_mr->lkey;

    modify_qp_to_init(flow->gpu_flush_qp, dev);
    modify_qp_to_rtr_gpuflush(flow->gpu_flush_qp, dev);
    modify_qp_to_rts(flow->gpu_flush_qp);
  }

  conn_id.context = flow.get();
  conn_id.sock_fd = sock_fd;
  conn_id.flow_id = fid;
  conn_id.peer_id = flow->peer_id;

  {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    flows_[fid] = std::move(flow);
  }

  UCCL_LOG(INFO) << "Falcon: accepted from " << remote_ip
                 << " flow_id=" << fid;
  return conn_id;
}

// ===========================================================================
// Memory Registration
// ===========================================================================

int FalconEndpoint::falcon_regmr(void* flow_context, void* data, size_t size,
                                 int type, Mhandle** mhandle) {
  auto* flow = static_cast<FalconFlow*>(flow_context);
  auto* fdev = get_device(flow ? flow->dev : dev_);
  if (!fdev) return -1;

  int flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
              IBV_ACCESS_REMOTE_READ;

  struct ibv_mr* mr = ibv_reg_mr(fdev->pd, data, size, flags);
  if (!mr) {
    UCCL_LOG(ERROR) << "Falcon: ibv_reg_mr failed, size=" << size;
    return -1;
  }

  *mhandle = new Mhandle{mr};
  return 0;
}

int FalconEndpoint::falcon_regmr_dmabuf(void* flow_context, void* data,
                                        size_t size, int type, uint64_t offset,
                                        int fd, Mhandle** mhandle) {
  auto* flow = static_cast<FalconFlow*>(flow_context);
  auto* fdev = get_device(flow ? flow->dev : dev_);
  if (!fdev) return -1;

  int flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
              IBV_ACCESS_REMOTE_READ;

  struct ibv_mr* mr = ibv_reg_dmabuf_mr(fdev->pd, offset, size,
                                        (uint64_t)data, fd, flags);
  if (!mr) {
    UCCL_LOG(ERROR) << "Falcon: ibv_reg_dmabuf_mr failed, size=" << size;
    return -1;
  }

  *mhandle = new Mhandle{mr};
  return 0;
}

void FalconEndpoint::falcon_deregmr(Mhandle* mhandle) {
  if (mhandle) {
    if (mhandle->mr) ibv_dereg_mr(mhandle->mr);
    delete mhandle;
  }
}

// ===========================================================================
// Data Path
// ===========================================================================

int FalconEndpoint::falcon_send_async(void* flow_context, Mhandle* mh,
                                      void* data, int size,
                                      FalconRequest* req) {
  auto* flow = static_cast<FalconFlow*>(flow_context);
  if (!flow || !mh) return -1;

  req->type = kReqTx;
  req->send.data_len = size;
  req->done.store(false, std::memory_order_relaxed);

  // Check if receiver has posted a FIFO entry (sender polls local FIFO copy)
  int slot = flow->fifo_head % kMaxReq;
  volatile FifoItem* fi = &flow->fifo->elems[slot][0];
  uint64_t expected_idx = flow->fifo_head + 1;
  if (fi->idx != expected_idx) {
    // Receiver hasn't posted yet — caller should retry
    return -1;
  }
  __sync_synchronize();

  // Extract remote buffer info from FifoItem
  uint64_t raddr = fi->addr;
  uint32_t rkey  = fi->rkey;
  uint32_t rid   = fi->rid;
  flow->fifo_head++;

  req->send.raddr = raddr;
  req->send.rkey  = rkey;
  req->send.rid   = rid;

  // Chunk segmentation
  uint32_t chunk_size_bytes = ucclParamFalconChunkSizeKB() << 10;
  int num_chunks = (size + chunk_size_bytes - 1) / chunk_size_bytes;
  if (num_chunks == 0) num_chunks = 1;  // Zero-byte send still needs 1 WR

  req->chunks_left.store(num_chunks, std::memory_order_relaxed);

  auto* engine = engines_[flow->engine_idx].get();
  uint32_t sent_offset = 0;
  uint32_t fid = flow->flow_id & 0x7FF;

  for (int c = 0; c < num_chunks; c++) {
    uint32_t this_chunk = std::min(chunk_size_bytes,
                                   (uint32_t)size - sent_offset);
    bool is_last = (sent_offset + this_chunk >= (uint32_t)size);

    // Build imm_data
    IMMData imm(0);
    imm.SetFID(fid);
    imm.SetRID(rid);
    imm.SetCSN(flow->snd_csn.fetch_add(1, std::memory_order_relaxed) & 0xFF);
    if (is_last) imm.SetHINT(1);

    struct ibv_qp* qp = flow->select_qp();

    struct ibv_sge sge{};
    sge.addr = (uint64_t)data + sent_offset;
    sge.length = this_chunk;
    sge.lkey = mh->mr->lkey;

    struct ibv_send_wr wr{};
    wr.wr_id = reinterpret_cast<uint64_t>(req);  // For send completion tracking
    wr.sg_list = &sge;
    wr.num_sge = (this_chunk > 0) ? 1 : 0;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(imm.raw());
    wr.wr.rdma.remote_addr = raddr + sent_offset;
    wr.wr.rdma.rkey = rkey;

    struct ibv_send_wr* bad_wr = nullptr;
    int ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret) {
      UCCL_LOG(ERROR) << "Falcon: ibv_post_send chunk " << c << " failed: "
                      << ret;
      // Mark remaining chunks done to avoid stuck request
      req->chunks_left.store(0, std::memory_order_relaxed);
      req->done.store(true, std::memory_order_release);
      return -1;
    }

    sent_offset += this_chunk;
  }

  return 0;
}

int FalconEndpoint::falcon_recv_async(void* flow_context, Mhandle** mhs,
                                      void** data, int* sizes, int n,
                                      FalconRequest* req) {
  auto* flow = static_cast<FalconFlow*>(flow_context);
  if (!flow) return -1;

  req->type = kReqRx;
  req->n = n;
  req->done.store(false, std::memory_order_relaxed);

  // Allocate a recv request slot for completion matching
  RecvRequest* rr = flow->alloc_recv_req();
  if (!rr) {
    // No free request slots — caller should retry
    return -1;
  }

  rr->state = RecvRequest::kRecv;
  rr->ureq = req;
  rr->n = n;
  rr->last_chunk_arrived = false;
  for (int i = 0; i < n; i++) {
    rr->received_bytes[i] = 0;
    rr->total_expected[i] = sizes[i];
    req->recv.data_len[i] = sizes[i];
  }

  int rid = flow->get_recv_req_id(rr);

  // Post FifoItem to sender's FIFO (RDMA WRITE INLINE)
  if (post_fifo_item(flow, data, sizes, n, mhs, rid) != 0) {
    flow->free_recv_req(rr);
    return -1;
  }

  return 0;
}

int FalconEndpoint::falcon_flush(void* flow_context, Mhandle** mhs,
                                 void** data, int* sizes, int n,
                                 FalconRequest* req) {
  auto* flow = static_cast<FalconFlow*>(flow_context);
  req->type = kReqFlush;
  req->done.store(false);

  if (!flow || flow->is_send || !flow->gpu_flush_qp) {
    // No flush needed for sender or if GPU flush QP doesn't exist
    req->done.store(true, std::memory_order_release);
    return 0;
  }

  // Find last non-zero size for the flush target
  int last = -1;
  for (int i = 0; i < n; i++) {
    if (sizes[i]) last = i;
  }
  if (last < 0) {
    req->done.store(true, std::memory_order_release);
    return 0;
  }

  // Post loopback RDMA READ to force PCIe write ordering
  struct ibv_sge sge{};
  sge.addr = flow->gpu_flush_sge.addr;
  sge.length = 1;
  sge.lkey = flow->gpu_flush_mr->lkey;

  struct ibv_send_wr wr{};
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = (uint64_t)data[last];
  wr.wr.rdma.rkey = mhs[last]->mr->rkey;

  struct ibv_send_wr* bad_wr = nullptr;
  int ret = ibv_post_send(flow->gpu_flush_qp, &wr, &bad_wr);
  if (ret) {
    UCCL_LOG(ERROR) << "Falcon: GPU flush ibv_post_send failed: " << ret;
    req->done.store(true, std::memory_order_release);
    return -1;
  }

  // Poll for flush completion
  struct ibv_wc wc{};
  while (true) {
    int nc = ibv_poll_cq(flow->flow_cq, 1, &wc);
    if (nc > 0) {
      if (wc.status != IBV_WC_SUCCESS) {
        UCCL_LOG(ERROR) << "Falcon: GPU flush CQE error: " << wc.status;
      }
      break;
    }
    if (nc < 0) {
      UCCL_LOG(ERROR) << "Falcon: GPU flush poll error";
      break;
    }
  }

  req->done.store(true, std::memory_order_release);
  return 0;
}

bool FalconEndpoint::falcon_poll_request(FalconRequest* req) {
  return req->done.load(std::memory_order_acquire);
}

}  // namespace falcon
}  // namespace uccl
