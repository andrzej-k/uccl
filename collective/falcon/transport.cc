// Falcon transport implementation stub for UCCL collective.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// TODO: Implement Falcon device discovery, connection management,
// data transfer, and completion handling.

#include "transport.h"
#include "transport_config.h"
#include "util/debug.h"

namespace uccl {

// TODO: Implement FalconEngine::run() — main engine loop that polls
// Falcon CQs and processes work requests from Channel queues.

// TODO: Implement FalconEndpoint methods:
//   - Device enumeration (get_num_devices)
//   - Listen / connect / accept handshake over TCP bootstrap
//   - Memory registration / deregistration with Falcon NIC
//   - Async send / recv posting to Falcon QPs
//   - Completion polling

}  // namespace uccl
