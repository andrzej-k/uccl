// Falcon Transport — NCCL Plugin Interface
//
// Implements ncclNet_v8_t so that Falcon can be loaded as an NCCL network
// plugin (libnccl-net-falcon.so).  The plugin is a thin wrapper that
// delegates everything to FalconEndpoint, which uses standard ibverbs
// through the IRDMA driver with crt_ena=1.

#include "nccl_net.h"
#include "transport.h"
#include "util/debug.h"
#include "util/util.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>

using namespace uccl;
using namespace uccl::falcon;

char const* PLUGIN_NAME = "Falcon_Plugin";

// ---------------------------------------------------------------------------
// Request Pool
// ---------------------------------------------------------------------------

class FalconRequestBuffPool : public BuffPool {
  static constexpr size_t num_elements = kMaxReq << 2;
  static constexpr size_t element_size = sizeof(FalconRequest);

 public:
  FalconRequestBuffPool() : BuffPool(num_elements, element_size, nullptr) {}
  ~FalconRequestBuffPool() = default;
};

// ---------------------------------------------------------------------------
// Global Endpoint
// ---------------------------------------------------------------------------

std::shared_ptr<FalconEndpoint> falcon_ep;

// ---------------------------------------------------------------------------
// Connection State Machine (matches RDMA plugin pattern for async connect)
// ---------------------------------------------------------------------------

enum ConnState { kConnInit = 0, kConnConnecting, kConnConnected };

struct FalconBaseComm {
  int dev;
  ConnID conn_id;
  std::shared_ptr<FalconRequestBuffPool> req_pool;
};

struct AsyncAcceptState {
  struct FalconBaseComm base;
  std::string remote_ip_str;
  int remote_dev;
};

struct AsyncConnectState {
  struct FalconBaseComm base;
};

// Handle passed to remote side via NCCL bootstrap
struct FalconHandle {
  uint32_t ip_addr_u32;
  uint16_t listen_port;
  int remote_dev;
  int remote_gpuidx;
  enum ConnState state = kConnInit;
  AsyncConnectState connect_buffer;
};
static_assert(sizeof(struct FalconHandle) < NCCL_NET_HANDLE_MAXSIZE,
              "FalconHandle size too large");

struct FalconListenComm {
  int dev;
  int listen_fd;
  int remote_dev;
  int gpuidx;
  enum ConnState state = kConnInit;
  AsyncAcceptState accept_buffer;
};

struct FalconRecvComm {
  struct FalconBaseComm base;
  std::string remote_ip_str;
  int remote_dev;
};

struct FalconSendComm {
  struct FalconBaseComm base;
};

// ---------------------------------------------------------------------------
// NCCL Plugin Functions
// ---------------------------------------------------------------------------

ncclResult_t pluginInit(ncclDebugLogger_t logFunction) {
  std::cout << "Hello UCCL Falcon from PID: " << getpid() << std::endl;
  falcon_ep = std::make_shared<FalconEndpoint>(ucclParamFalconNumEngines());
  return ncclSuccess;
}

ncclResult_t pluginDevices(int* ndev) {
  // Trigger lazy device discovery (initialize_engine_by_dev calls init_devices
  // if devices_ is empty).
  if (falcon_ep->get_num_devices() == 0) {
    falcon_ep->initialize_engine_by_dev(0, false);
  }
  *ndev = falcon_ep->get_num_devices();
  return ncclSuccess;
}

static ncclResult_t pluginPciPath(char const* ib_name, char** path) {
  char devicePath[256];
  snprintf(devicePath, sizeof(devicePath),
           "/sys/class/infiniband/%s/device", ib_name);
  char* p = realpath(devicePath, NULL);
  if (p == NULL) {
    UCCL_LOG(ERROR) << "Could not find device path for " << ib_name;
    return ncclInternalError;
  }
  *path = p;
  return ncclSuccess;
}

ncclResult_t pluginGetProperties(int dev, ncclNetProperties_v8_t* props) {
  auto* fdev = falcon_ep->get_device(dev);
  if (!fdev) return ncclInternalError;

  props->name = fdev->ib_name;
  props->speed = fdev->link_bw * 8 / 1e6;  // Mbps
  pluginPciPath(fdev->ib_name, &props->pciPath);
  props->guid = fdev->dev_attr.sys_image_guid;
  props->ptrSupport = NCCL_PTR_HOST;

  // GPU Direct support — check if GDR module is loaded
  // For MEV/Falcon, GPU Direct should work through standard ibverbs
  props->ptrSupport |= NCCL_PTR_CUDA;

  if (fdev->dma_buf_support) {
    props->ptrSupport |= NCCL_PTR_DMABUF;
  }

  props->regIsGlobal = 0;
  props->port = fdev->ib_port_num;
  props->latency = 0;
  props->maxComms = 1024 * 1024;
  props->maxRecvs = 1;
  props->netDeviceType = NCCL_NET_DEVICE_HOST;
  props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
  return ncclSuccess;
}

ncclResult_t pluginListen(int dev, void* opaqueHandle, void** listenComm) {
  struct FalconHandle* handle = (struct FalconHandle*)opaqueHandle;
  memset(handle, 0, sizeof(struct FalconHandle));

  int local_gpuidx;
  GPU_RT_CHECK(gpuGetDevice(&local_gpuidx));

  auto best_dev = falcon_ep->get_best_dev_idx(local_gpuidx);
  if (dev != best_dev) dev = best_dev;

  if (falcon_ep->initialize_engine_by_dev(dev, false)) {
    UCCL_LOG_PLUGIN << "Falcon pluginListen init dev=" << dev
                    << " pid=" << getpid();
  }

  // Create TCP listening socket for bootstrap
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return ncclInternalError;

  int flag = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(int));

  struct sockaddr_in serv_addr{};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = 0;

  if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    close(listen_fd);
    return ncclInternalError;
  }

  socklen_t addrlen = sizeof(serv_addr);
  getsockname(listen_fd, (struct sockaddr*)&serv_addr, &addrlen);

  if (listen(listen_fd, 1) < 0) {
    close(listen_fd);
    return ncclInternalError;
  }

  // Fill handle with local device IP for remote side to connect
  auto* fdev = falcon_ep->get_device(dev);
  if (fdev && !fdev->local_ip_str.empty()) {
    handle->ip_addr_u32 = str_to_ip(fdev->local_ip_str);
  } else {
    handle->ip_addr_u32 = ntohl(INADDR_ANY);
  }
  handle->listen_port = ntohs(serv_addr.sin_port);
  handle->remote_dev = dev;
  handle->remote_gpuidx = local_gpuidx;

  struct FalconListenComm* lcomm =
      (struct FalconListenComm*)calloc(1, sizeof(struct FalconListenComm));
  lcomm->dev = dev;
  lcomm->listen_fd = listen_fd;
  lcomm->gpuidx = local_gpuidx;

  *listenComm = lcomm;

  UCCL_LOG_PLUGIN << "Falcon Listen on dev=" << dev << " pid=" << getpid();
  return ncclSuccess;
}

ncclResult_t pluginConnect(int dev, void* opaque_handle, void** sendComm,
                           ncclNetDeviceHandle_v8_t** /*sendDevComm*/) {
  struct FalconHandle* handle = (struct FalconHandle*)opaque_handle;
  int local_gpuidx;
  GPU_RT_CHECK(gpuGetDevice(&local_gpuidx));

  auto best_dev = falcon_ep->get_best_dev_idx(local_gpuidx);
  if (dev != best_dev) dev = best_dev;

  if (falcon_ep->initialize_engine_by_dev(dev, false)) {
    UCCL_LOG_PLUGIN << "Falcon pluginConnect init dev=" << dev
                    << " pid=" << getpid();
  }

  std::string remote_ip_str = ip_to_str(handle->ip_addr_u32);

  if (handle->state == kConnInit) {
    handle->state = kConnConnecting;
    std::thread t = std::thread([dev, local_gpuidx, handle, remote_ip_str] {
      handle->connect_buffer.base.conn_id = falcon_ep->falcon_connect(
          dev, local_gpuidx, handle->remote_dev, handle->remote_gpuidx,
          remote_ip_str, handle->listen_port);
      handle->connect_buffer.base.dev = dev;
      std::atomic_thread_fence(std::memory_order_release);
      handle->state = kConnConnected;
    });
    t.detach();
    *sendComm = nullptr;
  } else if (handle->state == kConnConnecting) {
    *sendComm = nullptr;
  } else {
    struct FalconSendComm* scomm =
        (struct FalconSendComm*)calloc(1, sizeof(struct FalconSendComm));
    scomm->base = handle->connect_buffer.base;
    scomm->base.req_pool = std::make_shared<FalconRequestBuffPool>();
    *sendComm = scomm;
    UCCL_LOG_PLUGIN << "Falcon Connected to " << remote_ip_str
                    << " flow_id=" << scomm->base.conn_id.flow_id
                    << " pid=" << getpid();
  }
  return ncclSuccess;
}

ncclResult_t pluginAccept(void* listenComm, void** recvComm,
                          ncclNetDeviceHandle_v8_t** /*recvDevComm*/) {
  struct FalconListenComm* lcomm = (struct FalconListenComm*)listenComm;

  if (lcomm->state == kConnInit) {
    lcomm->state = kConnConnecting;
    std::thread t = std::thread([lcomm] {
      std::string remote_ip_str;
      int remote_dev;
      int remote_gpuidx;
      lcomm->accept_buffer.base.conn_id =
          falcon_ep->falcon_accept(lcomm->dev, lcomm->listen_fd, lcomm->gpuidx,
                                   remote_ip_str, &remote_dev, &remote_gpuidx);
      lcomm->accept_buffer.base.dev = lcomm->dev;
      lcomm->accept_buffer.remote_ip_str = remote_ip_str;
      lcomm->accept_buffer.remote_dev = remote_dev;
      std::atomic_thread_fence(std::memory_order_release);
      lcomm->state = kConnConnected;
    });
    t.detach();
    *recvComm = nullptr;
  } else if (lcomm->state == kConnConnecting) {
    *recvComm = nullptr;
  } else {
    struct FalconRecvComm* rcomm =
        (struct FalconRecvComm*)calloc(1, sizeof(struct FalconRecvComm));
    rcomm->base = lcomm->accept_buffer.base;
    rcomm->base.req_pool = std::make_shared<FalconRequestBuffPool>();
    rcomm->remote_ip_str = lcomm->accept_buffer.remote_ip_str;
    rcomm->remote_dev = lcomm->accept_buffer.remote_dev;
    *recvComm = rcomm;
    UCCL_LOG_PLUGIN << "Falcon Accepted from " << rcomm->remote_ip_str
                    << " flow_id=" << rcomm->base.conn_id.flow_id
                    << " pid=" << getpid();
  }
  return ncclSuccess;
}

ncclResult_t pluginRegMr(void* collComm, void* data, size_t size, int type,
                         void** mhandle) {
  struct FalconBaseComm* base = (struct FalconBaseComm*)collComm;
  int ret = falcon_ep->falcon_regmr(base->conn_id.context, data, size, type,
                                    (struct Mhandle**)mhandle);
  return ret == 0 ? ncclSuccess : ncclInternalError;
}

ncclResult_t pluginRegMrDmaBuf(void* collComm, void* data, size_t size,
                               int type, uint64_t offset, int fd,
                               void** mhandle) {
  struct FalconBaseComm* base = (struct FalconBaseComm*)collComm;
  int ret = falcon_ep->falcon_regmr_dmabuf(base->conn_id.context, data, size,
                                           type, offset, fd,
                                           (struct Mhandle**)mhandle);
  return ret == 0 ? ncclSuccess : ncclInternalError;
}

ncclResult_t pluginDeregMr(void* collComm, void* mhandle) {
  falcon_ep->falcon_deregmr((struct Mhandle*)mhandle);
  return ncclSuccess;
}

ncclResult_t pluginIsend(void* sendComm, void* data, int size, int tag,
                         void* mhandle, void** request) {
  struct FalconSendComm* scomm = (struct FalconSendComm*)sendComm;
  auto conn_id = scomm->base.conn_id;
  struct Mhandle* mh = (struct Mhandle*)mhandle;

  uint64_t addr;
  if (scomm->base.req_pool->alloc_buff(&addr)) {
    *request = nullptr;
    return ncclSuccess;
  }

  struct FalconRequest* req = reinterpret_cast<struct FalconRequest*>(addr);
  if (falcon_ep->falcon_send_async(conn_id.context, mh, data, size, req)) {
    scomm->base.req_pool->free_buff(reinterpret_cast<uint64_t>(req));
    *request = nullptr;
    return ncclSuccess;
  }
  req->req_pool = (void*)scomm->base.req_pool.get();
  *request = req;

  UCCL_LOG_PLUGIN << "Falcon Isend " << size << "B pid=" << getpid();
  return ncclSuccess;
}

ncclResult_t pluginIrecv(void* recvComm, int n, void** data, int* sizes,
                         int* tags, void** mhandles, void** request) {
  struct FalconRecvComm* rcomm = (struct FalconRecvComm*)recvComm;
  auto conn_id = rcomm->base.conn_id;
  struct Mhandle** mhs = (struct Mhandle**)mhandles;

  uint64_t addr;
  if (rcomm->base.req_pool->alloc_buff(&addr)) {
    *request = nullptr;
    return ncclSuccess;
  }

  struct FalconRequest* req = reinterpret_cast<struct FalconRequest*>(addr);
  if (falcon_ep->falcon_recv_async(conn_id.context, mhs, data, sizes, n,
                                   req)) {
    rcomm->base.req_pool->free_buff(reinterpret_cast<uint64_t>(req));
    *request = nullptr;
    return ncclSuccess;
  }
  req->req_pool = (void*)rcomm->base.req_pool.get();
  *request = req;

  UCCL_LOG_PLUGIN << "Falcon Irecv " << sizes[0] << "B pid=" << getpid();
  return ncclSuccess;
}

ncclResult_t pluginIflush(void* recvComm, int n, void** data, int* sizes,
                          void** mhandles, void** request) {
  struct FalconRecvComm* rcomm = (struct FalconRecvComm*)recvComm;
  auto conn_id = rcomm->base.conn_id;
  struct Mhandle** mhs = (struct Mhandle**)mhandles;

  uint64_t addr;
  if (rcomm->base.req_pool->alloc_buff(&addr)) {
    *request = nullptr;
    return ncclSuccess;
  }

  struct FalconRequest* req = reinterpret_cast<struct FalconRequest*>(addr);
  if (falcon_ep->falcon_flush(conn_id.context, mhs, data, sizes, n, req)) {
    rcomm->base.req_pool->free_buff(reinterpret_cast<uint64_t>(req));
    *request = nullptr;
    return ncclSuccess;
  }
  req->req_pool = (void*)rcomm->base.req_pool.get();
  *request = req;
  return ncclSuccess;
}

ncclResult_t pluginTest(void* request, int* done, int* size) {
  struct FalconRequest* req =
      reinterpret_cast<struct FalconRequest*>(request);

  if (falcon_ep->falcon_poll_request(req)) {
    *done = 1;
    if (req->type == kReqTx) {
      size[0] = req->send.data_len;
      UCCL_LOG_PLUGIN << "Falcon Test Tx done " << size[0] << "B";
    } else if (req->type == kReqRx) {
      for (int i = 0; i < req->n; i++) size[i] = req->recv.data_len[i];
      UCCL_LOG_PLUGIN << "Falcon Test Rx done " << size[0] << "B";
    }
    auto* pool = reinterpret_cast<FalconRequestBuffPool*>(req->req_pool);
    pool->free_buff(reinterpret_cast<uint64_t>(req));
  } else {
    *done = 0;
  }
  return ncclSuccess;
}

ncclResult_t pluginCloseSend(void* sendComm) {
  free(sendComm);
  return ncclSuccess;
}

ncclResult_t pluginCloseRecv(void* recvComm) {
  free(recvComm);
  return ncclSuccess;
}

ncclResult_t pluginCloseListen(void* listenComm) {
  struct FalconListenComm* lcomm = (struct FalconListenComm*)listenComm;
  close(lcomm->listen_fd);
  free(lcomm);
  return ncclSuccess;
}

// ---------------------------------------------------------------------------
// Plugin Export
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
