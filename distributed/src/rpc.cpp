#include <autoruntime/distributed.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace autoruntime {
namespace {

constexpr std::array<unsigned char, 4U> kRequestMagic{
    'A', 'R', 'R', 'Q'};
constexpr std::array<unsigned char, 4U> kResponseMagic{
    'A', 'R', 'R', 'S'};
constexpr std::uint16_t kRpcVersion = 1U;
constexpr std::size_t kRequestHeaderBytes = 28U;
constexpr std::size_t kResponseHeaderBytes = 24U;
constexpr std::size_t kMaximumMethodBytes = 128U;
constexpr std::size_t kMaximumDetailBytes = 1024U;
constexpr std::size_t kAbsolutePayloadLimit = 16U * 1024U * 1024U;

Status NetworkError(std::string_view operation, int error = errno) {
  return Status(StatusCode::TransportError,
                std::string(operation) + ": " + std::strerror(error));
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
  const std::string owned(endpoint.address);
  if (::inet_pton(AF_INET, owned.c_str(), &address.sin_addr) != 1) {
    return Status(StatusCode::InvalidArgument,
                  "only numeric IPv4 endpoints are supported");
  }
  return address;
}

void StoreU16(unsigned char* destination, std::uint16_t value) {
  destination[0] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  destination[1] = static_cast<unsigned char>(value & 0xFFU);
}

void StoreU32(unsigned char* destination, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    const auto shift = static_cast<unsigned int>((3U - index) * 8U);
    destination[index] =
        static_cast<unsigned char>((value >> shift) & 0xFFU);
  }
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

std::uint32_t LoadU32(const unsigned char* source) {
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value = (value << 8U) | static_cast<std::uint32_t>(source[index]);
  }
  return value;
}

std::uint64_t LoadU64(const unsigned char* source) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(source[index]);
  }
  return value;
}

class Descriptor {
 public:
  explicit Descriptor(int value = -1) : value_(value) {}
  ~Descriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept {
    const int result = value_;
    value_ = -1;
    return result;
  }

 private:
  int value_{-1};
};

Status SetNonBlocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 ||
      ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
    return NetworkError("fcntl(O_NONBLOCK)");
  }
  return Status::Ok();
}

Status WaitFor(int descriptor, short events, Deadline deadline,
               std::stop_token stop_token) {
  for (;;) {
    if (stop_token.stop_requested()) {
      return Status(StatusCode::Cancelled, "RPC operation cancelled");
    }
    if (deadline.expired()) {
      return Status(StatusCode::Timeout, "RPC deadline expired");
    }
    int timeout_milliseconds = 20;
    if (!deadline.infinite()) {
      const auto remaining = deadline.time_point() - Deadline::Clock::now();
      auto milliseconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
      if (milliseconds < remaining) {
        milliseconds += std::chrono::milliseconds(1);
      }
      timeout_milliseconds = static_cast<int>(
          std::clamp<std::int64_t>(milliseconds.count(), 1, 20));
    }

    pollfd item{};
    item.fd = descriptor;
    item.events = events;
    const int result = ::poll(&item, 1U, timeout_milliseconds);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return NetworkError("poll(RPC)");
    }
    if (result == 0) {
      continue;
    }
    if ((item.revents & POLLNVAL) != 0 ||
        ((item.revents & POLLERR) != 0 && (item.revents & events) == 0)) {
      return Status(StatusCode::TransportError,
                    "RPC socket reported an error");
    }
    if ((item.revents & events) != 0 ||
        ((events & POLLIN) != 0 && (item.revents & POLLHUP) != 0)) {
      return Status::Ok();
    }
    if ((item.revents & POLLHUP) != 0) {
      return Status(StatusCode::TransportError,
                    "RPC peer closed the connection");
    }
  }
}

Status WriteExact(int descriptor, const unsigned char* data,
                  std::size_t size, Deadline deadline,
                  std::stop_token stop_token) {
  std::size_t offset = 0U;
  while (offset < size) {
    const auto ready = WaitFor(
        descriptor, POLLOUT, deadline, stop_token);
    if (!ready) {
      return ready;
    }
    const auto written = ::send(
        descriptor, data + offset, size - offset, MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      continue;
    }
    if (written == 0) {
      return Status(StatusCode::TransportError,
                    "RPC socket wrote zero bytes");
    }
    return NetworkError("send(RPC)");
  }
  return Status::Ok();
}

Status ReadExact(int descriptor, unsigned char* data,
                 std::size_t size, Deadline deadline,
                 std::stop_token stop_token) {
  std::size_t offset = 0U;
  while (offset < size) {
    const auto ready = WaitFor(
        descriptor, POLLIN, deadline, stop_token);
    if (!ready) {
      return ready;
    }
    const auto received =
        ::recv(descriptor, data + offset, size - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      continue;
    }
    if (received == 0) {
      return Status(StatusCode::TransportError,
                    "RPC peer closed before frame completed");
    }
    return NetworkError("recv(RPC)");
  }
  return Status::Ok();
}

std::uint32_t RemainingMilliseconds(Deadline deadline) {
  if (deadline.infinite()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  const auto remaining = deadline.time_point() - Deadline::Clock::now();
  if (remaining <= Deadline::Clock::duration::zero()) {
    return 0U;
  }
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (milliseconds < remaining) {
    milliseconds += std::chrono::milliseconds(1);
  }
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
      milliseconds.count(), 1,
      static_cast<std::int64_t>(
          std::numeric_limits<std::uint32_t>::max())));
}

bool ValidStatusCode(std::uint16_t raw) {
  return raw <= static_cast<std::uint16_t>(StatusCode::Internal);
}

}  // namespace

struct RpcServer::Impl {
  RpcServerConfig config;
  NetworkEndpoint local_endpoint;
  int listener{-1};
  mutable std::mutex mutex;
  std::unordered_map<std::string, RpcHandler> handlers;
  RpcServerStats stats;
  std::atomic<bool> running{false};
  std::jthread worker;

  void CountIoFailure() {
    std::lock_guard lock(mutex);
    ++stats.io_failures;
  }

  void CountMalformed() {
    std::lock_guard lock(mutex);
    ++stats.malformed_requests;
  }

  void HandleConnection(int descriptor, std::stop_token stop_token) {
    const auto io_deadline = Deadline::After(config.io_timeout);
    std::array<unsigned char, kRequestHeaderBytes> header{};
    auto status = ReadExact(
        descriptor, header.data(), header.size(), io_deadline, stop_token);
    if (!status) {
      CountIoFailure();
      return;
    }
    if (!std::equal(
            kRequestMagic.begin(), kRequestMagic.end(), header.begin()) ||
        LoadU16(header.data() + 4U) != kRpcVersion) {
      CountMalformed();
      return;
    }

    const auto request_id = LoadU64(header.data() + 8U);
    const auto timeout_milliseconds = LoadU32(header.data() + 16U);
    const auto method_size =
        static_cast<std::size_t>(LoadU32(header.data() + 20U));
    const auto payload_size =
        static_cast<std::size_t>(LoadU32(header.data() + 24U));
    if (request_id == 0U || timeout_milliseconds == 0U ||
        method_size == 0U || method_size > kMaximumMethodBytes ||
        payload_size > config.max_request_bytes) {
      CountMalformed();
      return;
    }

    std::string method(method_size, '\0');
    std::vector<std::byte> request(payload_size);
    status = ReadExact(
        descriptor, reinterpret_cast<unsigned char*>(method.data()),
        method.size(), io_deadline, stop_token);
    if (status && !request.empty()) {
      status = ReadExact(
          descriptor,
          reinterpret_cast<unsigned char*>(request.data()),
          request.size(), io_deadline, stop_token);
    }
    if (!status) {
      CountIoFailure();
      return;
    }

    RpcHandler handler;
    {
      std::lock_guard lock(mutex);
      ++stats.requests;
      const auto iterator = handlers.find(method);
      if (iterator != handlers.end()) {
        handler = iterator->second;
      }
    }

    Result<std::vector<std::byte>> response =
        Status(StatusCode::NotFound, "RPC method is not registered");
    if (handler) {
      const auto request_deadline = Deadline::After(
          std::chrono::milliseconds(timeout_milliseconds));
      if (request_deadline.expired()) {
        response = Status(StatusCode::Timeout,
                          "RPC request expired before dispatch");
      } else {
        try {
          response = handler(request);
        } catch (const std::exception& error) {
          response = Status(
              StatusCode::Internal,
              std::string("RPC handler threw: ") + error.what());
        } catch (...) {
          response = Status(StatusCode::Internal, "RPC handler threw");
        }
      }
    }

    StatusCode response_code = StatusCode::Ok;
    std::string detail;
    std::vector<std::byte> payload;
    if (response) {
      payload = std::move(response).take_value();
      if (payload.size() > config.max_request_bytes) {
        response_code = StatusCode::Internal;
        detail = "RPC response exceeds configured size";
        payload.clear();
      }
    } else {
      response_code = response.status().code();
      detail = response.status().detail();
      if (detail.size() > kMaximumDetailBytes) {
        detail.resize(kMaximumDetailBytes);
      }
      std::lock_guard lock(mutex);
      ++stats.handler_failures;
    }

    std::array<unsigned char, kResponseHeaderBytes> response_header{};
    std::copy(kResponseMagic.begin(), kResponseMagic.end(),
              response_header.begin());
    StoreU16(response_header.data() + 4U, kRpcVersion);
    StoreU16(response_header.data() + 6U,
             static_cast<std::uint16_t>(response_code));
    StoreU64(response_header.data() + 8U, request_id);
    StoreU32(response_header.data() + 16U,
             static_cast<std::uint32_t>(detail.size()));
    StoreU32(response_header.data() + 20U,
             static_cast<std::uint32_t>(payload.size()));

    const auto write_deadline = Deadline::After(config.io_timeout);
    status = WriteExact(
        descriptor, response_header.data(), response_header.size(),
        write_deadline, stop_token);
    if (status && !detail.empty()) {
      status = WriteExact(
          descriptor,
          reinterpret_cast<const unsigned char*>(detail.data()),
          detail.size(), write_deadline, stop_token);
    }
    if (status && !payload.empty()) {
      status = WriteExact(
          descriptor,
          reinterpret_cast<const unsigned char*>(payload.data()),
          payload.size(), write_deadline, stop_token);
    }
    std::lock_guard lock(mutex);
    if (status) {
      ++stats.responses;
    } else {
      ++stats.io_failures;
    }
  }

  void Run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      pollfd descriptor{};
      descriptor.fd = listener;
      descriptor.events = POLLIN;
      const int poll_result = ::poll(&descriptor, 1U, 20);
      if (poll_result < 0) {
        if (errno != EINTR) {
          CountIoFailure();
        }
        continue;
      }
      if (poll_result == 0 ||
          (descriptor.revents & POLLIN) == 0) {
        continue;
      }

      sockaddr_in peer{};
      socklen_t peer_size = sizeof(peer);
      Descriptor connection(::accept(
          listener, reinterpret_cast<sockaddr*>(&peer), &peer_size));
      if (connection.get() < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          CountIoFailure();
        }
        continue;
      }
      const auto nonblocking = SetNonBlocking(connection.get());
      if (!nonblocking) {
        CountIoFailure();
        continue;
      }
      {
        std::lock_guard lock(mutex);
        ++stats.accepted_connections;
      }
      HandleConnection(connection.get(), stop_token);
    }
  }
};

RpcServer::RpcServer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RpcServer::~RpcServer() {
  static_cast<void>(Stop());
  if (impl_ && impl_->listener >= 0) {
    static_cast<void>(::close(impl_->listener));
    impl_->listener = -1;
  }
}

Result<std::unique_ptr<RpcServer>> RpcServer::Create(
    RpcServerConfig config) {
  if (config.max_request_bytes == 0U ||
      config.max_request_bytes > kAbsolutePayloadLimit ||
      config.io_timeout <= std::chrono::milliseconds::zero() ||
      config.listen_backlog == 0U ||
      config.listen_backlog > 1024U) {
    return Status(StatusCode::InvalidArgument,
                  "invalid RPC server configuration");
  }
  const auto address_result =
      SocketAddress(config.bind_endpoint, true);
  if (!address_result) {
    return address_result.status();
  }

  Descriptor descriptor(::socket(AF_INET, SOCK_STREAM, 0));
  if (descriptor.get() < 0) {
    return NetworkError("socket(RPC)");
  }
  const int reuse = 1;
  static_cast<void>(::setsockopt(
      descriptor.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
  const auto address = address_result.value();
  if (::bind(descriptor.get(),
             reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) < 0) {
    return NetworkError("bind(RPC)");
  }
  if (::listen(descriptor.get(),
               static_cast<int>(config.listen_backlog)) < 0) {
    return NetworkError("listen(RPC)");
  }
  const auto nonblocking = SetNonBlocking(descriptor.get());
  if (!nonblocking) {
    return nonblocking;
  }

  sockaddr_in local_address{};
  socklen_t local_size = sizeof(local_address);
  if (::getsockname(
          descriptor.get(), reinterpret_cast<sockaddr*>(&local_address),
          &local_size) < 0) {
    return NetworkError("getsockname(RPC)");
  }

  auto impl = std::make_unique<Impl>();
  impl->local_endpoint = {
      config.bind_endpoint.address, ntohs(local_address.sin_port)};
  impl->config = std::move(config);
  impl->listener = descriptor.release();
  return std::unique_ptr<RpcServer>(new RpcServer(std::move(impl)));
}

Status RpcServer::RegisterHandler(std::string method,
                                  RpcHandler handler) {
  if (method.empty() || method.size() > kMaximumMethodBytes || !handler) {
    return Status(StatusCode::InvalidArgument,
                  "RPC handler requires a bounded method and callback");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->handlers.contains(method)) {
    return Status(StatusCode::AlreadyExists,
                  "RPC method is already registered");
  }
  impl_->handlers.emplace(std::move(method), std::move(handler));
  return Status::Ok();
}

Status RpcServer::Start() {
  if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
    return Status(StatusCode::AlreadyExists,
                  "RPC server is already running");
  }
  impl_->worker =
      std::jthread([state = impl_.get()](std::stop_token stop_token) {
        state->Run(stop_token);
      });
  return Status::Ok();
}

Status RpcServer::Stop() {
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

NetworkEndpoint RpcServer::LocalEndpoint() const {
  return impl_->local_endpoint;
}

RpcServerStats RpcServer::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

Result<std::vector<std::byte>> RpcClient::Call(
    const NetworkEndpoint& endpoint, std::string_view method,
    std::span<const std::byte> payload, Deadline deadline,
    std::stop_token stop_token) {
  if (method.empty() || method.size() > kMaximumMethodBytes ||
      payload.size() > kAbsolutePayloadLimit) {
    return Status(StatusCode::InvalidArgument,
                  "invalid RPC request bounds");
  }
  if (stop_token.stop_requested()) {
    return Status(StatusCode::Cancelled, "RPC operation cancelled");
  }
  if (deadline.expired()) {
    return Status(StatusCode::Timeout, "RPC deadline expired");
  }
  const auto address_result = SocketAddress(endpoint, false);
  if (!address_result) {
    return address_result.status();
  }

  Descriptor descriptor(::socket(AF_INET, SOCK_STREAM, 0));
  if (descriptor.get() < 0) {
    return NetworkError("socket(RPC client)");
  }
  const auto nonblocking = SetNonBlocking(descriptor.get());
  if (!nonblocking) {
    return nonblocking;
  }

  const auto address = address_result.value();
  if (::connect(descriptor.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
    if (errno != EINPROGRESS) {
      return NetworkError("connect(RPC)");
    }
    const auto connected =
        WaitFor(descriptor.get(), POLLOUT, deadline, stop_token);
    if (!connected) {
      return connected;
    }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR,
                     &socket_error, &error_size) < 0) {
      return NetworkError("getsockopt(SO_ERROR)");
    }
    if (socket_error != 0) {
      return NetworkError("connect(RPC)", socket_error);
    }
  }

  static std::atomic<std::uint64_t> next_request_id{1U};
  const auto request_id =
      next_request_id.fetch_add(1U, std::memory_order_relaxed);
  const auto timeout_milliseconds = RemainingMilliseconds(deadline);
  if (timeout_milliseconds == 0U) {
    return Status(StatusCode::Timeout, "RPC deadline expired");
  }
  std::array<unsigned char, kRequestHeaderBytes> header{};
  std::copy(kRequestMagic.begin(), kRequestMagic.end(), header.begin());
  StoreU16(header.data() + 4U, kRpcVersion);
  StoreU64(header.data() + 8U, request_id);
  StoreU32(header.data() + 16U, timeout_milliseconds);
  StoreU32(header.data() + 20U,
           static_cast<std::uint32_t>(method.size()));
  StoreU32(header.data() + 24U,
           static_cast<std::uint32_t>(payload.size()));

  auto status = WriteExact(
      descriptor.get(), header.data(), header.size(), deadline, stop_token);
  if (status) {
    status = WriteExact(
        descriptor.get(),
        reinterpret_cast<const unsigned char*>(method.data()),
        method.size(), deadline, stop_token);
  }
  if (status && !payload.empty()) {
    status = WriteExact(
        descriptor.get(),
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size(), deadline, stop_token);
  }
  if (!status) {
    return status;
  }

  std::array<unsigned char, kResponseHeaderBytes> response_header{};
  status = ReadExact(
      descriptor.get(), response_header.data(), response_header.size(),
      deadline, stop_token);
  if (!status) {
    return status;
  }
  if (!std::equal(
          kResponseMagic.begin(), kResponseMagic.end(),
          response_header.begin()) ||
      LoadU16(response_header.data() + 4U) != kRpcVersion) {
    return Status(StatusCode::TransportError,
                  "RPC response header is invalid");
  }

  const auto raw_code = LoadU16(response_header.data() + 6U);
  const auto response_request_id = LoadU64(response_header.data() + 8U);
  const auto detail_size =
      static_cast<std::size_t>(LoadU32(response_header.data() + 16U));
  const auto response_size =
      static_cast<std::size_t>(LoadU32(response_header.data() + 20U));
  if (!ValidStatusCode(raw_code) ||
      response_request_id != request_id ||
      detail_size > kMaximumDetailBytes ||
      response_size > kAbsolutePayloadLimit) {
    return Status(StatusCode::TransportError,
                  "RPC response bounds or correlation are invalid");
  }

  std::string detail(detail_size, '\0');
  std::vector<std::byte> response(response_size);
  if (!detail.empty()) {
    status = ReadExact(
        descriptor.get(),
        reinterpret_cast<unsigned char*>(detail.data()),
        detail.size(), deadline, stop_token);
  }
  if (status && !response.empty()) {
    status = ReadExact(
        descriptor.get(),
        reinterpret_cast<unsigned char*>(response.data()),
        response.size(), deadline, stop_token);
  }
  if (!status) {
    return status;
  }

  const auto code = static_cast<StatusCode>(raw_code);
  if (code != StatusCode::Ok) {
    return Status(code, std::move(detail));
  }
  return response;
}

}  // namespace autoruntime
