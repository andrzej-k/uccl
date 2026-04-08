// NCCL net plugin (v8) for Falcon transport.
// See: https://github.com/opencomputeproject/OCP-NET-Falcon
//
// Implements the ncclNet_v8_t interface so NCCL can use Falcon as a
// network backend for collective operations.

#include "nccl_net.h"
#include "transport.h"
#include "transport_config.h"
#include "util/debug.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace uccl;

char const* PLUGIN_NAME = "Falcon_Plugin";

static std::shared_ptr<FalconEndpoint> ep;

// ---------------------------------------------------------------------------
// NCCL plugin callbacks
// ---------------------------------------------------------------------------

ncclResult_t pluginInit(ncclDebugLogger_t logFunction) {
  std::cout << "Falcon plugin init from PID: " << getpid() << std::endl;
  ep = std::make_shared<FalconEndpoint>(ucclParamNUM_ENGINES());
  // TODO: Initialize Falcon hardware resources.
  return ncclSuccess;
}

ncclResult_t pluginDevices(int* ndev) {
  *ndev = ep->get_num_devices();
  return ncclSuccess;
}

ncclResult_t pluginGetProperties(int dev, ncclNetProperties_v8_t* props) {
  // TODO: Fill in Falcon device properties (name, PCI path, GUID, speed, etc.)
  std::memset(props, 0, sizeof(*props));
  props->name = PLUGIN_NAME;
  props->maxComms = MAX_PEER;
  return ncclSuccess;
}

ncclResult_t pluginListen(int dev, void* handle, void** listenComm) {
  // TODO: Implement Falcon listen (TCP bootstrap + Falcon setup).
  return ncclInternalError;
}

ncclResult_t pluginConnect(int dev, void* handle, void** sendComm) {
  // TODO: Implement Falcon connect.
  return ncclInternalError;
}

ncclResult_t pluginAccept(void* listenComm, void** recvComm) {
  // TODO: Implement Falcon accept.
  return ncclInternalError;
}

ncclResult_t pluginRegMr(void* collComm, void* data, size_t size,
                         int type, void** mhandle) {
  // TODO: Register memory with Falcon NIC.
  return ncclInternalError;
}

ncclResult_t pluginRegMrDmaBuf(void* collComm, void* data, size_t size,
                               int type, uint64_t offset, int fd,
                               void** mhandle) {
  // TODO: Register DMA-BUF backed memory with Falcon NIC.
  return ncclInternalError;
}

ncclResult_t pluginDeregMr(void* collComm, void* mhandle) {
  // TODO: Deregister memory.
  return ncclInternalError;
}

ncclResult_t pluginIsend(void* sendComm, void* data, int size,
                         int tag, void* mhandle, void** request) {
  // TODO: Post an asynchronous send via Falcon.
  return ncclInternalError;
}

ncclResult_t pluginIrecv(void* recvComm, int n, void** data, int* sizes,
                         int* tags, void** mhandles, void** request) {
  // TODO: Post an asynchronous receive via Falcon.
  return ncclInternalError;
}

ncclResult_t pluginIflush(void* recvComm, int n, void** data, int* sizes,
                          void** mhandles, void** request) {
  // TODO: Flush outstanding receives (for GDR consistency).
  return ncclInternalError;
}

ncclResult_t pluginTest(void* request, int* done, int* size) {
  // TODO: Poll for completion.
  *done = 0;
  return ncclSuccess;
}

ncclResult_t pluginCloseSend(void* sendComm) {
  // TODO: Tear down send connection.
  return ncclSuccess;
}

ncclResult_t pluginCloseRecv(void* recvComm) {
  // TODO: Tear down receive connection.
  return ncclSuccess;
}

ncclResult_t pluginCloseListen(void* listenComm) {
  // TODO: Tear down listen state.
  return ncclSuccess;
}

// ---------------------------------------------------------------------------
// NCCL v8 plugin symbol
// ---------------------------------------------------------------------------

ncclNet_v8_t volatile ncclNetPlugin_v8 = {
    .name = PLUGIN_NAME,
    .init = pluginInit,
    .devices = pluginDevices,
    .getProperties = pluginGetProperties,
    .listen = pluginListen,
    .connect = pluginConnect,
    .accept = pluginAccept,
    .regMr = pluginRegMr,
    .regMrDmaBuf = pluginRegMrDmaBuf,
    .deregMr = pluginDeregMr,
    .isend = pluginIsend,
    .irecv = pluginIrecv,
    .iflush = pluginIflush,
    .test = pluginTest,
    .closeSend = pluginCloseSend,
    .closeRecv = pluginCloseRecv,
    .closeListen = pluginCloseListen,
    .getDeviceMr = nullptr,
    .irecvConsumed = nullptr,
};
