#pragma once

// Falcon Transport - Congestion Control
//
// On MEV, congestion control is fully offloaded to the Falcon hardware block
// (CPE - Custom Protocol Engine) running on the ACC.  Two implementations
// exist:
//
//   1. HW RUE  – Fixed integer-based Swift AIMD, ~10x faster, less tunable.
//                Configured via rtcm-config.json → "hw-rue" section:
//                  aif, mdf, naif, nmdf, target_rx_buffer, enable_eack_drop
//
//   2. SW RUE  – The `falcon_rue` binary on ACC cores.  Runs a full Swift CC
//                algorithm responding to Falcon HW congestion signals (ACK
//                timestamps T1-T4, rx_buffer_level, forward_hops, NACK codes).
//                Adjusts per-connection fcwnd (fabric), ncwnd (NIC), and
//                packet pacing rate.  Falcon HW enforces these by gating
//                request/data Tx.  Configured via rue-cfg.json with per-profile
//                Swift params + PLB section.
//
// The host-side UCCL plugin does NOT perform any CC computations.  The Falcon
// HW + RUE closed loop is entirely within the IPU.  The host driver (IRDMA
// with crt_ena=1) only adjusts MTU to account for the 256-byte Falcon header
// (CRT_RDMA_HEADER), and the MEV CP steers packets to CPE ingress queues via
// NSL_PROT_ENG_CPE.
//
// The structures below are retained as reference documentation of the RUE
// event/result mailbox format (Section 10.6.2) — useful if UCCL ever needs
// to read telemetry counters from the ACC or if a future software-only
// fallback path is implemented.

#include "falcon_config.h"
#include <cstdint>

namespace uccl {
namespace falcon {

// ---------------------------------------------------------------------------
// RUE Event & Result (Section 10.6.2) — Reference structures
//
// These mirror the HW mailbox layout used between the Falcon Tx/Rx blocks
// and the RUE (HW or SW).  The host never generates or consumes these
// directly — the ACC-side rtcmd / falcon_rue processes own this interface.
// ---------------------------------------------------------------------------

enum class RueEventType : uint8_t {
  kAck = 0,
  kNack = 1,
  kRetransmit = 2,
};

struct RueEvent {
  uint32_t connection_id;
  RueEventType event_type;

  // Timestamps (131.072ns units, captured by Falcon HW)
  uint32_t t1, t2, t3, t4;

  // Retransmit info
  uint8_t retransmit_count;
  bool retransmit_is_early;  // vs RTO

  // NACK info
  FalconNackCode nack_code;

  // CC signals (populated by Falcon HW from packet headers)
  uint8_t forward_hops;
  uint8_t rx_buffer_level;   // 0-31 quantized NIC Rx buffer occupancy
  uint32_t cc_metadata;

  // Current per-connection state (read from CRT context by HW)
  double fcwnd;
  double ncwnd;
  uint64_t fabric_window_time_marker;
  uint64_t nic_window_time_marker;
  double smoothed_delay;
  double smoothed_rtt;

  uint32_t num_packets_acked;
  bool eack_drop;
  bool eack;
};

struct RueResult {
  uint32_t connection_id;
  double fcwnd;               // New fabric congestion window (packets)
  double ncwnd;               // New NIC congestion window (packets)
  uint64_t inter_packet_gap;  // Pacing rate when fcwnd < 1
  uint64_t retransmit_timeout;
  uint64_t fabric_window_time_marker;
  uint64_t nic_window_time_marker;
  double smoothed_delay;
  double smoothed_rtt;
  bool randomize_path;        // PLB reroute signal
};

// ---------------------------------------------------------------------------
// HW RUE configuration — mirrors rtcm-config.json "hw-rue" section
// ---------------------------------------------------------------------------

struct HwRueConfig {
  double aif = 1.0;           // Fabric additive increment factor
  double mdf = 0.8;           // Fabric multiplicative decrement factor
  double naif = 1.0;          // NIC additive increment factor
  double nmdf = 0.8;          // NIC multiplicative decrement factor
  uint32_t target_rx_buffer = 31;  // Target NIC Rx buffer occupancy (0-31)
  bool enable_eack_drop = false;   // Aggressive window adjust on E-ACK drops
  bool randomize_path = false;     // PLB path randomization (unused currently)
};

// ---------------------------------------------------------------------------
// SW RUE Swift profile — mirrors rue-cfg.json per-profile params
// ---------------------------------------------------------------------------

struct SwRueSwiftProfile {
  double nic_aif = 1.0;
  double nic_mdf = 0.8;
  double nic_max_cwnd = 256.0;
  double nic_min_cwnd = 1.0;
  double fabric_aif = 1.0;
  double fabric_mdf = 0.8;
  double fabric_max_cwnd = 256.0;
  double target_rx_buffer_level = 15.0;
  double target_delay_us = 50.0;          // microseconds
  double topo_scaling_per_hop_us = 1.0;   // ms in MEV docs, but per-hop
  double max_flow_scaling_range_us = 20.0;
  double max_flow_scaling_window = 100.0;
  double min_flow_scaling_window = 1.0;
};

struct SwRuePlbConfig {
  bool is_enabled = false;
  double target_delay_multiplier = 0.1;
  double congestion_threshold = 0.5;
  uint32_t attempt_threshold = 2;
};

}  // namespace falcon
}  // namespace uccl
