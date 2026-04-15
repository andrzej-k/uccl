#pragma once

// Falcon Transport — Configuration Parameters
//
// On MEV, Falcon is a HW transport.  The host uses standard ibverbs through
// IRDMA with crt_ena=1.  Most protocol parameters (CC, retransmission,
// sliding windows, PSP) are configured on the ACC via rtcm-config.json
// and rue-cfg.json — NOT from the host.
//
// The parameters below control only the host-side plugin geometry (engines,
// QPs, chunk size) and CRT-specific constants.

#include "param.h"
#include <cstdint>

namespace uccl {
namespace falcon {

// ---------------------------------------------------------------------------
// Transport Geometry
// ---------------------------------------------------------------------------

// Number of engines (polling threads) per device.
UCCL_PARAM(FalconNumEngines, "FALCON_NUM_ENGINES", 4);

// Number of RC QPs per engine per peer (PORT_ENTROPY for ECMP spreading).
UCCL_PARAM(FalconPortEntropy, "FALCON_PORT_ENTROPY", 32);

// Chunk size in KB for segmenting large sends into individual RDMA WRs.
UCCL_PARAM(FalconChunkSizeKB, "FALCON_CHUNK_SIZE_KB", 64);

// Use RC mode (Falcon on MEV always uses RC).
// Kept as a param for compatibility but defaults to true.
UCCL_PARAM(FalconRCMode, "FALCON_RCMODE", 1);

// ---------------------------------------------------------------------------
// CRT / Falcon-Specific
// ---------------------------------------------------------------------------

// Falcon header overhead in bytes (CRT_RDMA_HEADER from IRDMA driver).
// Subtracted from Ethernet MTU to compute effective RDMA payload MTU.
// Ethernet MTU must be ≥ RDMA_MTU + this value (+ ~144B for other headers).
static constexpr uint32_t kCrtHeaderOverhead = 256;

// RoCE traffic class for Falcon traffic.
UCCL_PARAM(FalconRoceTrafficClass, "FALCON_ROCE_TRAFFIC_CLASS", 3);

// RoCE service level.
UCCL_PARAM(FalconRoceServiceLevel, "FALCON_ROCE_SERVICE_LEVEL", 0);

// ---------------------------------------------------------------------------
// CQ / SRQ / WR Sizes
// ---------------------------------------------------------------------------

// CQ depth per engine (send + recv separate CQs).
static constexpr uint32_t kCQDepth = 16384;

// SRQ depth (shared receive queue for zero-byte RC WRITE+IMM receives).
static constexpr uint32_t kSRQDepth = 256;

// Max send/recv WRs per QP.
static constexpr uint32_t kMaxSendRecvWR = 256;

// Maximum outstanding requests per flow.  Limited to 16 by the 4-bit RID
// field in the IMMData encoding.
static constexpr uint32_t kMaxReq = 16;

// Max recv gathers in a single pluginIrecv.
static constexpr int kMaxRecv = 8;

// Signal CQ every N sends (1 = signal every send for simplicity).
static constexpr int kSignalInterval = 1;

// Max SGEs per WR.
static constexpr int kMaxSge = 2;

// Max inline data bytes.
static constexpr int kMaxInline = 64;

// CQ poll batch size.
static constexpr int kMaxBatchCQ = 16;

// ---------------------------------------------------------------------------
// Congestion Control / Reliable Delivery
//
// ALL handled by Falcon HW on the ACC.  Nothing to configure from the host.
//   - HW RUE: rtcm-config.json → "hw-rue" section
//   - SW RUE: rue-cfg.json → Swift profile + PLB section
//   - Retransmission: Falcon HW sliding windows + RTO
//   - PSP: rtcmd PSP Manager on ACC
// ---------------------------------------------------------------------------

}  // namespace falcon
}  // namespace uccl
