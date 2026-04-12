#include <autoruntime/distributed.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <unistd.h>

namespace autoruntime {
namespace {

constexpr std::array<unsigned char, 4U> kDiscoveryMagic{
    'A', 'R', 'D', '1'};
constexpr unsigned char kDiscoveryVersion = 1U;
constexpr std::size_t kFixedAnnouncementBytes = 26U;
constexpr std::size_t kMaximumIdentityBytes = 63U;
constexpr std::size_t kMaximumDatagramBytes = 512U;

Status NetworkError(std::string_view operation) {
  return Status(StatusCode::TransportError,
                std::string(operation) + ": " + std::strerror(errno));
}

bool ParseIpv4(std::string_view address, in_addr& result) {
  const std::string owned(address);
  return ::inet_pton(AF_INET, owned.c_str(), &result) == 1;
}

Result<sockaddr_in> SocketAddress(const NetworkEndpoint& endpoint,
                                  bool allow_zero_port) {
  if (endpoint.address.empty() ||
      (!allow_zero_port && endpoint.port == 0U)) {
    return Status(StatusCode::InvalidArgument,
                  "IPv4 endpoint requires address and port");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  if (!ParseIpv4(endpoint.address, address.sin_addr)) {
    return Status(StatusCode::InvalidArgument,
                  "only numeric IPv4 endpoints are supported");
  }
  return address;
}

void StoreU16(unsigned char* destination, std::uint16_t value) {
  destination[0] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  destination[1] = static_cast<unsigned char>(value & 0xFFU);
}

void StoreU64(unsigned char* destination, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto shift = static_cast<unsigned int>((7U - index) * 8U);
    destination[index] =
        static_cast<unsigned char>((value >> shift) & 0xFFU);
  }
}

std::uint16_t LoadU16(const unsigned char* source) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(source[0]) << 8U) |
      static_cast<std::uint16_t>(source[1]));
}

std::uint64_t LoadU64(const unsigned char* source) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(source[index]);
  }
  return value;
}

struct Announcement {
  std::string node_id;
  std::uint64_t generation{0U};
  std::uint64_t sequence{0U};
  NetworkEndpoint rpc_endpoint;
};

std::vector<unsigned char> BuildAnnouncement(
    const DiscoveryConfig& config, std::uint64_t sequence) {
  const auto node_length =
      static_cast<unsigned char>(config.node_id.size());
  const auto address_length = static_cast<unsigned char>(
      config.advertised_rpc_endpoint.address.size());
  std::vector<unsigned char> packet(
      kFixedAnnouncementBytes + node_length + address_length, 0U);
  std::copy(kDiscoveryMagic.begin(), kDiscoveryMagic.end(), packet.begin());
  packet[4] = kDiscoveryVersion;
  packet[5] = node_length;
  packet[6] = address_length;
  StoreU64(packet.data() + 8U, config.generation);
  StoreU64(packet.data() + 16U, sequence);
  StoreU16(packet.data() + 24U,
           config.advertised_rpc_endpoint.port);
  std::copy(config.node_id.begin(), config.node_id.end(),
            packet.begin() +
                static_cast<std::ptrdiff_t>(kFixedAnnouncementBytes));
  std::copy(
      config.advertised_rpc_endpoint.address.begin(),
      config.advertised_rpc_endpoint.address.end(),
      packet.begin() + static_cast<std::ptrdiff_t>(
                           kFixedAnnouncementBytes + node_length));
  return packet;
}

std::optional<Announcement> ParseAnnouncement(
    const unsigned char* data, std::size_t size) {
  if (size < kFixedAnnouncementBytes ||
      !std::equal(kDiscoveryMagic.begin(), kDiscoveryMagic.end(), data) ||
      data[4] != kDiscoveryVersion) {
    return std::nullopt;
  }
  const auto node_length = static_cast<std::size_t>(data[5]);
  const auto address_length = static_cast<std::size_t>(data[6]);
  if (node_length == 0U || node_length > kMaximumIdentityBytes ||
      address_length == 0U ||
      address_length > kMaximumIdentityBytes ||
      size != kFixedAnnouncementBytes + node_length + address_length) {
    return std::nullopt;
  }
  Announcement announcement;
  announcement.generation = LoadU64(data + 8U);
  announcement.sequence = LoadU64(data + 16U);
  announcement.rpc_endpoint.port = LoadU16(data + 24U);
  announcement.node_id.assign(
      reinterpret_cast<const char*>(data + kFixedAnnouncementBytes),
      node_length);
  announcement.rpc_endpoint.address.assign(
      reinterpret_cast<const char*>(
          data + kFixedAnnouncementBytes + node_length),
      address_length);
  in_addr parsed{};
  if (announcement.generation == 0U ||
      announcement.sequence == 0U ||
      !ParseIpv4(announcement.rpc_endpoint.address, parsed)) {
    return std::nullopt;
  }
  return announcement;
}

Status SetNonBlocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 ||
      ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
    return NetworkError("fcntl(O_NONBLOCK)");
  }
  return Status::Ok();
}

}  // namespace

struct DiscoveryService::Impl {
  DiscoveryConfig config;
  NetworkEndpoint local_endpoint;
  int socket{-1};
  mutable std::mutex mutex;
  std::vector<NetworkEndpoint> peers;
  std::unordered_map<std::string, MemberRecord> members;
  DiscoveryStats stats;
  std::uint64_t next_sequence{1U};
  std::atomic<bool> running{false};
  std::jthread worker;

  void SendAnnouncement() {
    std::vector<NetworkEndpoint> current_peers;
    std::uint64_t sequence = 0U;
    {
      std::lock_guard lock(mutex);
      current_peers = peers;
      sequence = next_sequence++;
    }
    const auto packet = BuildAnnouncement(config, sequence);
    std::uint64_t sent = 0U;
    std::uint64_t failed = 0U;
    for (const auto& peer : current_peers) {
      const auto address_result = SocketAddress(peer, false);
      if (!address_result) {
        ++failed;
        continue;
      }
      const auto address = address_result.value();
      const auto result = ::sendto(
          socket, packet.data(), packet.size(), 0,
          reinterpret_cast<const sockaddr*>(&address), sizeof(address));
      if (result >= 0 &&
          static_cast<std::size_t>(result) == packet.size()) {
        ++sent;
      } else {
        ++failed;
      }
    }
    std::lock_guard lock(mutex);
    stats.announcements_sent += sent;
    stats.send_failures += failed;
  }

  void Accept(const Announcement& announcement,
              Deadline::TimePoint observed_at) {
    if (announcement.node_id == config.node_id) {
      return;
    }
    std::lock_guard lock(mutex);
    ++stats.announcements_received;
    const auto iterator = members.find(announcement.node_id);
    if (iterator == members.end()) {
      if (members.size() >= config.max_members) {
        ++stats.capacity_drops;
        return;
      }
      members.emplace(
          announcement.node_id,
          MemberRecord{announcement.node_id,
                       announcement.generation,
                       announcement.sequence,
                       announcement.rpc_endpoint,
                       observed_at});
      return;
    }

    auto& member = iterator->second;
    if (announcement.generation < member.generation) {
      ++stats.stale_announcements;
      return;
    }
    if (announcement.generation == member.generation &&
        announcement.sequence <= member.heartbeat_sequence) {
      ++stats.duplicate_announcements;
      return;
    }
    member.generation = announcement.generation;
    member.heartbeat_sequence = announcement.sequence;
    member.rpc_endpoint = announcement.rpc_endpoint;
    member.last_seen = observed_at;
  }

  void ReceiveAnnouncements() {
    for (;;) {
      std::array<unsigned char, kMaximumDatagramBytes> buffer{};
      const auto received = ::recvfrom(
          socket, buffer.data(), buffer.size(), 0, nullptr, nullptr);
      if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        std::lock_guard lock(mutex);
        ++stats.parse_errors;
        return;
      }
      const auto parsed = ParseAnnouncement(
          buffer.data(), static_cast<std::size_t>(received));
      if (!parsed) {
        std::lock_guard lock(mutex);
        ++stats.parse_errors;
        continue;
      }
      Accept(*parsed, Deadline::Clock::now());
    }
  }

  void ExpireMembers(Deadline::TimePoint observed_at) {
    std::lock_guard lock(mutex);
    for (auto iterator = members.begin(); iterator != members.end();) {
      if (observed_at - iterator->second.last_seen >
          config.lease_timeout) {
        iterator = members.erase(iterator);
        ++stats.expired_members;
      } else {
        ++iterator;
      }
    }
  }

  void Run(std::stop_token stop_token) {
    auto next_heartbeat = Deadline::Clock::now();
    while (!stop_token.stop_requested()) {
      const auto now = Deadline::Clock::now();
      if (now >= next_heartbeat) {
        SendAnnouncement();
        next_heartbeat = now + config.heartbeat_period;
      }

      pollfd descriptor{};
      descriptor.fd = socket;
      descriptor.events = POLLIN;
      const int poll_result = ::poll(&descriptor, 1U, 10);
      if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
        ReceiveAnnouncements();
      } else if (poll_result < 0 && errno != EINTR) {
        std::lock_guard lock(mutex);
        ++stats.parse_errors;
      }
      ExpireMembers(Deadline::Clock::now());
    }
  }
};

DiscoveryService::DiscoveryService(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DiscoveryService::~DiscoveryService() {
  static_cast<void>(Stop());
  if (impl_ && impl_->socket >= 0) {
    static_cast<void>(::close(impl_->socket));
    impl_->socket = -1;
  }
}

Result<std::unique_ptr<DiscoveryService>> DiscoveryService::Create(
    DiscoveryConfig config) {
  if (config.node_id.empty() ||
      config.node_id.size() > kMaximumIdentityBytes ||
      config.generation == 0U ||
      config.advertised_rpc_endpoint.address.empty() ||
      config.advertised_rpc_endpoint.address.size() >
          kMaximumIdentityBytes ||
      config.heartbeat_period <= std::chrono::milliseconds::zero() ||
      config.lease_timeout <= config.heartbeat_period ||
      config.max_members == 0U || config.max_peers == 0U ||
      config.peers.size() > config.max_peers) {
    return Status(StatusCode::InvalidArgument,
                  "invalid discovery configuration");
  }
  in_addr advertised{};
  if (!ParseIpv4(config.advertised_rpc_endpoint.address, advertised)) {
    return Status(StatusCode::InvalidArgument,
                  "advertised RPC endpoint must be numeric IPv4");
  }
  const auto bind_result = SocketAddress(config.bind_endpoint, true);
  if (!bind_result) {
    return bind_result.status();
  }
  for (const auto& peer : config.peers) {
    const auto peer_result = SocketAddress(peer, false);
    if (!peer_result) {
      return peer_result.status();
    }
  }

  const int descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (descriptor < 0) {
    return NetworkError("socket(discovery)");
  }
  const int reuse = 1;
  static_cast<void>(::setsockopt(
      descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
  const auto bind_address = bind_result.value();
  if (::bind(descriptor,
             reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) < 0) {
    const auto status = NetworkError("bind(discovery)");
    static_cast<void>(::close(descriptor));
    return status;
  }
  const auto nonblocking = SetNonBlocking(descriptor);
  if (!nonblocking) {
    static_cast<void>(::close(descriptor));
    return nonblocking;
  }

  sockaddr_in local_address{};
  socklen_t local_size = sizeof(local_address);
  if (::getsockname(
          descriptor, reinterpret_cast<sockaddr*>(&local_address),
          &local_size) < 0) {
    const auto status = NetworkError("getsockname(discovery)");
    static_cast<void>(::close(descriptor));
    return status;
  }

  auto impl = std::make_unique<Impl>();
  impl->local_endpoint = {
      config.bind_endpoint.address, ntohs(local_address.sin_port)};
  impl->peers = config.peers;
  impl->config = std::move(config);
  impl->socket = descriptor;
  return std::unique_ptr<DiscoveryService>(
      new DiscoveryService(std::move(impl)));
}

Status DiscoveryService::AddPeer(NetworkEndpoint peer) {
  const auto address = SocketAddress(peer, false);
  if (!address) {
    return address.status();
  }
  std::lock_guard lock(impl_->mutex);
  if (std::find(impl_->peers.begin(), impl_->peers.end(), peer) !=
      impl_->peers.end()) {
    return Status(StatusCode::AlreadyExists,
                  "discovery peer already exists");
  }
  if (impl_->peers.size() >= impl_->config.max_peers) {
    return Status(StatusCode::QueueFull,
                  "discovery peer capacity reached");
  }
  impl_->peers.push_back(std::move(peer));
  return Status::Ok();
}

Status DiscoveryService::Start() {
  if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
    return Status(StatusCode::AlreadyExists,
                  "discovery service is already running");
  }
  impl_->worker =
      std::jthread([state = impl_.get()](std::stop_token stop_token) {
        state->Run(stop_token);
      });
  return Status::Ok();
}

Status DiscoveryService::Stop() {
  if (!impl_ ||
      !impl_->running.exchange(false, std::memory_order_acq_rel)) {
    return Status::Ok();
  }
  impl_->worker.request_stop();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  return Status::Ok();
}

NetworkEndpoint DiscoveryService::LocalEndpoint() const {
  return impl_->local_endpoint;
}

std::vector<MemberRecord> DiscoveryService::Members() const {
  std::vector<MemberRecord> result;
  {
    std::lock_guard lock(impl_->mutex);
    result.reserve(impl_->members.size());
    for (const auto& [node_id, member] : impl_->members) {
      static_cast<void>(node_id);
      result.push_back(member);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const MemberRecord& left, const MemberRecord& right) {
              return left.node_id < right.node_id;
            });
  return result;
}

Result<MemberRecord> DiscoveryService::Find(
    std::string_view node_id) const {
  std::lock_guard lock(impl_->mutex);
  const auto iterator = impl_->members.find(std::string(node_id));
  if (iterator == impl_->members.end()) {
    return Status(StatusCode::NotFound,
                  "discovery member was not found");
  }
  return iterator->second;
}

DiscoveryStats DiscoveryService::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

}  // namespace autoruntime
