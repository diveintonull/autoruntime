#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace autoruntime {

struct NetworkEndpoint {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0U};

  [[nodiscard]] bool operator==(
      const NetworkEndpoint&) const noexcept = default;
};

struct MemberRecord {
  std::string node_id;
  std::uint64_t generation{0U};
  std::uint64_t heartbeat_sequence{0U};
  NetworkEndpoint rpc_endpoint;
  Deadline::TimePoint last_seen{};
};

struct DiscoveryStats {
  std::uint64_t announcements_sent{0U};
  std::uint64_t announcements_received{0U};
  std::uint64_t parse_errors{0U};
  std::uint64_t stale_announcements{0U};
  std::uint64_t duplicate_announcements{0U};
  std::uint64_t expired_members{0U};
  std::uint64_t capacity_drops{0U};
  std::uint64_t send_failures{0U};
};

struct DiscoveryConfig {
  std::string node_id;
  std::uint64_t generation{1U};
  NetworkEndpoint bind_endpoint{"0.0.0.0", 0U};
  NetworkEndpoint advertised_rpc_endpoint{"127.0.0.1", 0U};
  std::vector<NetworkEndpoint> peers;
  std::chrono::milliseconds heartbeat_period{100};
  std::chrono::milliseconds lease_timeout{500};
  std::size_t max_members{32U};
  std::size_t max_peers{16U};
};

class DiscoveryService {
 public:
  [[nodiscard]] static Result<std::unique_ptr<DiscoveryService>> Create(
      DiscoveryConfig config);
  ~DiscoveryService();

  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;
  DiscoveryService(DiscoveryService&&) = delete;
  DiscoveryService& operator=(DiscoveryService&&) = delete;

  Status AddPeer(NetworkEndpoint peer);
  Status Start();
  Status Stop();

  [[nodiscard]] NetworkEndpoint LocalEndpoint() const;
  [[nodiscard]] std::vector<MemberRecord> Members() const;
  [[nodiscard]] Result<MemberRecord> Find(std::string_view node_id) const;
  [[nodiscard]] DiscoveryStats Stats() const;

 private:
  struct Impl;
  explicit DiscoveryService(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

using RpcHandler =
    std::function<Result<std::vector<std::byte>>(
        std::span<const std::byte>)>;

struct RpcServerConfig {
  NetworkEndpoint bind_endpoint{"0.0.0.0", 0U};
  std::size_t max_request_bytes{1024U * 1024U};
  std::chrono::milliseconds io_timeout{1000};
  std::size_t listen_backlog{16U};
};

struct RpcServerStats {
  std::uint64_t accepted_connections{0U};
  std::uint64_t requests{0U};
  std::uint64_t responses{0U};
  std::uint64_t malformed_requests{0U};
  std::uint64_t handler_failures{0U};
  std::uint64_t io_failures{0U};
};

class RpcServer {
 public:
  [[nodiscard]] static Result<std::unique_ptr<RpcServer>> Create(
      RpcServerConfig config);
  ~RpcServer();

  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;
  RpcServer(RpcServer&&) = delete;
  RpcServer& operator=(RpcServer&&) = delete;

  Status RegisterHandler(std::string method, RpcHandler handler);
  Status Start();
  Status Stop();

  [[nodiscard]] NetworkEndpoint LocalEndpoint() const;
  [[nodiscard]] RpcServerStats Stats() const;

 private:
  struct Impl;
  explicit RpcServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class RpcClient {
 public:
  [[nodiscard]] static Result<std::vector<std::byte>> Call(
      const NetworkEndpoint& endpoint, std::string_view method,
      std::span<const std::byte> payload, Deadline deadline,
      std::stop_token stop_token = {});
};

}  // namespace autoruntime
