#pragma once

#include <autoruntime/dds_transport.hpp>

#include <dds/dds.h>

#include <functional>
#include <memory>
#include <string_view>

namespace autoruntime::detail {

struct DdsRpcStatsHooks {
  std::function<void()> count_request;
  std::function<void()> count_failure;
  std::function<void()> count_dropped;
};

class DdsRpcEngine final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<DdsRpcEngine>> Create(
      dds_entity_t participant, const DdsTransportConfig& config,
      DdsRpcStatsHooks hooks);

  ~DdsRpcEngine();

  DdsRpcEngine(const DdsRpcEngine&) = delete;
  DdsRpcEngine& operator=(const DdsRpcEngine&) = delete;
  DdsRpcEngine(DdsRpcEngine&&) = delete;
  DdsRpcEngine& operator=(DdsRpcEngine&&) = delete;

  [[nodiscard]] Result<ServiceId> AdvertiseService(
      std::string_view service_name,
      TransportServiceCallback callback);
  Status RemoveService(ServiceId service_id);
  [[nodiscard]] Result<Message> Request(
      std::string_view service_name, Message request,
      Deadline deadline);
  Status Close();

 private:
  struct Impl;
  explicit DdsRpcEngine(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime::detail
