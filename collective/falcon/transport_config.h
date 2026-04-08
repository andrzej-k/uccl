#pragma once

#include "param.h"
#include <cstdint>
#include <string>
#include <thread>
#include <unistd.h>

// Falcon transport configuration.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon

// Whether to pin the thread to the NUMA node.
UCCL_PARAM(PIN_TO_NUMA, "PIN_TO_NUMA", 1);

// Number of engines per device.
UCCL_PARAM(NUM_ENGINES, "NUM_ENGINES", 4);
// Path/QP per engine.
UCCL_PARAM(PORT_ENTROPY, "PORT_ENTROPY", 32);
// Maximum chunk size for each WQE.
UCCL_PARAM(CHUNK_SIZE_KB, "CHUNK_SIZE_KB", 64);

static constexpr uint32_t MAX_PEER = 2048;
static constexpr uint32_t MAX_FLOW = 2048;

// Maximum outstanding requests.
static constexpr uint32_t kMaxReq = 256;
// Maximum paths per flow.
static constexpr uint32_t kMaxPath = 32;

// Falcon-specific constants.
// TODO: Tune these for OCP Falcon hardware.
static constexpr uint32_t kFalconMaxMTU = 4096;
static constexpr uint32_t kFalconMaxQP = 128;

// Basic type aliases used across the transport layer.
typedef uint64_t FlowID;
typedef uint64_t PeerID;
