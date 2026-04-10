#include <autoruntime/node.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace autoruntime {
namespace {

Message BuildMessage(const NodeConfig& node,
                     std::atomic<std::uint64_t>& sequence,
                     std::span<const std::byte> payload,
                     TraceContext trace) {
  const auto now = MonotonicNanoseconds();
  Message message;
  message.envelope.trace_id =
      trace.trace_id == 0U ? NextTraceId() : trace.trace_id;
  message.envelope.span_id = NextSpanId();
  message.envelope.parent_span_id = trace.parent_span_id;
  message.envelope.sequence =
      sequence.fetch_add(1U, std::memory_order_relaxed) + 1U;
  message.envelope.source_timestamp_ns =
      trace.origin_timestamp_ns == 0U ? now : trace.origin_timestamp_ns;
  message.envelope.publish_timestamp_ns = now;
  message.envelope.source_generation = node.generation;
  message.payload.assign(payload.begin(), payload.end());
  return message;
}

}  // namespace

struct Publisher::Impl {
  NodeConfig node;
  std::string topic;
  QosProfile qos;
  std::shared_ptr<Transport> transport;
  mutable std::atomic<std::uint64_t> sequence{0U};
};

Publisher::Publisher(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Status Publisher::Publish(std::span<const std::byte> payload,
                          TraceContext trace) const {
  if (!impl_) {
    return Status(StatusCode::Closed, "publisher is empty");
  }
  return impl_->transport->Publish(
      impl_->topic,
      BuildMessage(impl_->node, impl_->sequence, payload, trace),
      impl_->qos);
}

Publisher::operator bool() const noexcept { return impl_ != nullptr; }

struct Subscriber::Impl : std::enable_shared_from_this<Subscriber::Impl> {
  mutable std::mutex mutex;
  bool closed{false};
  bool scheduled{false};
  std::deque<Message> queue;
  SubscriptionOptions options;
  SubscriptionStats stats;
  MessageCallback callback;
  std::shared_ptr<Executor> executor;
  std::shared_ptr<Transport> transport;
  TaskId task_id{0U};
  SubscriptionId subscription_id{0U};

  ~Impl() { static_cast<void>(Close()); }

  void Accept(Message message) {
    bool notify = false;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      ++stats.received_messages;
      if (queue.size() >= options.queue_capacity) {
        ++stats.dropped_messages;
        if (options.overflow_policy == OverflowPolicy::DropNewest) {
          return;
        }
        queue.pop_front();
      }
      queue.push_back(std::move(message));
      stats.queue_depth = queue.size();
      stats.queue_high_watermark =
          std::max(stats.queue_high_watermark, queue.size());
      if (!scheduled) {
        scheduled = true;
        notify = true;
      }
    }
    if (notify) {
      const auto status = executor->Notify(task_id);
      if (!status) {
        std::lock_guard lock(mutex);
        scheduled = false;
      }
    }
  }

  void ProcessOne() {
    Message message;
    {
      std::lock_guard lock(mutex);
      if (closed || queue.empty()) {
        scheduled = false;
        return;
      }
      message = std::move(queue.front());
      queue.pop_front();
      stats.queue_depth = queue.size();
    }

    try {
      callback(message);
    } catch (...) {
      std::lock_guard lock(mutex);
      ++stats.dropped_messages;
    }
    {
      std::lock_guard lock(mutex);
      ++stats.delivered_messages;
    }

    bool notify = false;
    {
      std::lock_guard lock(mutex);
      if (closed || queue.empty()) {
        scheduled = false;
      } else {
        notify = true;
      }
    }
    if (notify) {
      static_cast<void>(executor->Notify(task_id));
    }
  }

  Status Close() {
    SubscriptionId id = 0U;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return Status::Ok();
      }
      closed = true;
      queue.clear();
      stats.queue_depth = 0U;
      scheduled = false;
      id = subscription_id;
    }
    const auto unsubscribe_status = transport->Unsubscribe(id);
    static_cast<void>(executor->Cancel(task_id));
    if (!unsubscribe_status &&
        unsubscribe_status.code() != StatusCode::NotFound) {
      return unsubscribe_status;
    }
    return Status::Ok();
  }

  [[nodiscard]] SubscriptionStats Snapshot() const {
    std::lock_guard lock(mutex);
    return stats;
  }
};

Subscriber::Subscriber(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

SubscriptionStats Subscriber::Stats() const {
  return impl_ ? impl_->Snapshot() : SubscriptionStats{};
}

Status Subscriber::Close() {
  return impl_ ? impl_->Close() : Status::Ok();
}

Subscriber::operator bool() const noexcept { return impl_ != nullptr; }

struct Service::Impl : std::enable_shared_from_this<Service::Impl> {
  struct PendingCall {
    Message request;
    Deadline deadline{Deadline::Infinite()};
    std::promise<Result<Message>> completion;
  };

  mutable std::mutex mutex;
  bool closed{false};
  bool scheduled{false};
  std::deque<std::shared_ptr<PendingCall>> queue;
  static constexpr std::size_t kQueueCapacity = 64U;
  NodeConfig node;
  ServiceHandler handler;
  std::shared_ptr<Executor> executor;
  std::shared_ptr<Transport> transport;
  TaskId task_id{0U};
  ServiceId service_id{0U};
  std::atomic<std::uint64_t> response_sequence{0U};

  ~Impl() { static_cast<void>(Close()); }

  Result<Message> Invoke(Message request, Deadline deadline) {
    if (deadline.expired()) {
      return Status(StatusCode::Timeout, "service deadline expired");
    }
    auto pending = std::make_shared<PendingCall>();
    pending->request = std::move(request);
    pending->deadline = deadline;
    auto future = pending->completion.get_future();

    bool notify = false;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return Status(StatusCode::Closed, "service is closed");
      }
      if (queue.size() >= kQueueCapacity) {
        return Status(StatusCode::QueueFull, "service queue is full");
      }
      queue.push_back(pending);
      if (!scheduled) {
        scheduled = true;
        notify = true;
      }
    }
    if (notify) {
      const auto status = executor->Notify(task_id);
      if (!status) {
        std::lock_guard lock(mutex);
        const auto iterator =
            std::find(queue.begin(), queue.end(), pending);
        if (iterator != queue.end()) {
          queue.erase(iterator);
        }
        if (queue.empty()) {
          scheduled = false;
        }
        return status;
      }
    }

    if (deadline.infinite()) {
      return future.get();
    }
    if (future.wait_until(deadline.time_point()) !=
        std::future_status::ready) {
      return Status(StatusCode::Timeout, "service call timed out");
    }
    return future.get();
  }

  void ProcessOne() {
    std::shared_ptr<PendingCall> pending;
    {
      std::lock_guard lock(mutex);
      if (closed || queue.empty()) {
        scheduled = false;
        return;
      }
      pending = queue.front();
      queue.pop_front();
    }

    Result<std::vector<std::byte>> payload_result =
        Status(StatusCode::Internal, "service handler did not run");
    if (pending->deadline.expired()) {
      payload_result =
          Status(StatusCode::Timeout, "service request expired in queue");
    } else {
      try {
        payload_result = handler(pending->request);
      } catch (...) {
        payload_result =
            Status(StatusCode::Internal, "service handler threw");
      }
    }

    if (payload_result) {
      auto response = BuildMessage(
          node, response_sequence, payload_result.value(),
          pending->request.ContinueTrace());
      pending->completion.set_value(Result<Message>(std::move(response)));
    } else {
      pending->completion.set_value(
          Result<Message>(payload_result.status()));
    }

    bool notify = false;
    {
      std::lock_guard lock(mutex);
      if (closed || queue.empty()) {
        scheduled = false;
      } else {
        notify = true;
      }
    }
    if (notify) {
      static_cast<void>(executor->Notify(task_id));
    }
  }

  Status Close() {
    std::deque<std::shared_ptr<PendingCall>> pending;
    ServiceId id = 0U;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return Status::Ok();
      }
      closed = true;
      scheduled = false;
      pending.swap(queue);
      id = service_id;
    }
    for (auto& call : pending) {
      call->completion.set_value(
          Result<Message>(Status(StatusCode::Closed, "service closed")));
    }
    const auto remove_status = transport->RemoveService(id);
    static_cast<void>(executor->Cancel(task_id));
    if (!remove_status && remove_status.code() != StatusCode::NotFound) {
      return remove_status;
    }
    return Status::Ok();
  }
};

Service::Service(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Status Service::Close() {
  return impl_ ? impl_->Close() : Status::Ok();
}

Service::operator bool() const noexcept { return impl_ != nullptr; }

struct Client::Impl {
  NodeConfig node;
  std::string service_name;
  std::shared_ptr<Transport> transport;
  mutable std::atomic<std::uint64_t> sequence{0U};
};

Client::Client(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<Message> Client::Call(std::span<const std::byte> payload,
                             Deadline deadline,
                             TraceContext trace) const {
  if (!impl_) {
    return Status(StatusCode::Closed, "client is empty");
  }
  if (deadline.expired()) {
    return Status(StatusCode::Timeout, "client deadline expired");
  }
  return impl_->transport->Request(
      impl_->service_name,
      BuildMessage(impl_->node, impl_->sequence, payload, trace),
      deadline);
}

Client::operator bool() const noexcept { return impl_ != nullptr; }

struct Timer::Impl {
  std::shared_ptr<Executor> executor;
  TaskId task_id{0U};
  std::atomic<bool> cancelled{false};

  ~Impl() { static_cast<void>(Cancel()); }

  Status Cancel() {
    if (cancelled.exchange(true, std::memory_order_acq_rel)) {
      return Status::Ok();
    }
    return executor->Cancel(task_id);
  }
};

Timer::Timer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Status Timer::Cancel() {
  return impl_ ? impl_->Cancel() : Status::Ok();
}

Result<TaskStats> Timer::Stats() const {
  if (!impl_) {
    return Status(StatusCode::Closed, "timer is empty");
  }
  return impl_->executor->Stats(impl_->task_id);
}

Timer::operator bool() const noexcept { return impl_ != nullptr; }

Node::Node(NodeConfig config, std::shared_ptr<Executor> executor,
           std::shared_ptr<Transport> transport)
    : config_(std::move(config)),
      executor_(std::move(executor)),
      transport_(std::move(transport)) {
  if (config_.name.empty() || config_.generation == 0U ||
      !executor_ || !transport_) {
    throw std::invalid_argument(
        "Node requires name, nonzero generation, executor, and transport");
  }
}

const NodeConfig& Node::config() const noexcept { return config_; }

Result<Publisher> Node::CreatePublisher(
    std::string topic, QosProfile qos) const {
  if (topic.empty() || qos.depth == 0U) {
    return Status(StatusCode::InvalidArgument,
                  "publisher requires topic and nonzero QoS depth");
  }
  auto impl = std::make_shared<Publisher::Impl>();
  impl->node = config_;
  impl->topic = std::move(topic);
  impl->qos = qos;
  impl->transport = transport_;
  return Publisher(std::move(impl));
}

Result<Subscriber> Node::CreateSubscriber(
    std::string topic, SubscriptionOptions options,
    MessageCallback callback) const {
  if (topic.empty() || options.queue_capacity == 0U ||
      options.qos.depth == 0U || !callback) {
    return Status(StatusCode::InvalidArgument,
                  "invalid subscriber configuration");
  }

  auto impl = std::make_shared<Subscriber::Impl>();
  impl->options = options;
  impl->callback = std::move(callback);
  impl->executor = executor_;
  impl->transport = transport_;

  TaskConfig task_config;
  task_config.name = config_.name + ":subscription:" + topic;
  task_config.kind = TaskKind::Event;
  task_config.deadline = options.qos.deadline;
  task_config.queue_capacity = 1U;
  task_config.callback_group = options.callback_group;
  std::weak_ptr<Subscriber::Impl> weak = impl;
  auto task_result = executor_->AddTask(
      std::move(task_config),
      [weak](std::stop_token) {
        if (auto state = weak.lock()) {
          state->ProcessOne();
        }
      });
  if (!task_result) {
    return task_result.status();
  }
  impl->task_id = task_result.value();

  auto subscription_result = transport_->Subscribe(
      topic, options.qos,
      [weak](Message message) {
        if (auto state = weak.lock()) {
          state->Accept(std::move(message));
        }
      });
  if (!subscription_result) {
    static_cast<void>(executor_->Cancel(impl->task_id));
    return subscription_result.status();
  }
  impl->subscription_id = subscription_result.value();
  return Subscriber(std::move(impl));
}

Result<Service> Node::CreateService(
    std::string service_name, CallbackGroupId callback_group,
    ServiceHandler handler) const {
  if (service_name.empty() || !handler) {
    return Status(StatusCode::InvalidArgument,
                  "service requires name and handler");
  }

  auto impl = std::make_shared<Service::Impl>();
  impl->node = config_;
  impl->handler = std::move(handler);
  impl->executor = executor_;
  impl->transport = transport_;

  TaskConfig task_config;
  task_config.name = config_.name + ":service:" + service_name;
  task_config.kind = TaskKind::Event;
  task_config.deadline = std::chrono::seconds(1);
  task_config.queue_capacity = 1U;
  task_config.callback_group = callback_group;
  std::weak_ptr<Service::Impl> weak = impl;
  auto task_result = executor_->AddTask(
      std::move(task_config),
      [weak](std::stop_token) {
        if (auto state = weak.lock()) {
          state->ProcessOne();
        }
      });
  if (!task_result) {
    return task_result.status();
  }
  impl->task_id = task_result.value();

  auto service_result = transport_->AdvertiseService(
      service_name,
      [weak](Message request, Deadline deadline) -> Result<Message> {
        auto state = weak.lock();
        if (!state) {
          return Status(StatusCode::Closed, "service owner is gone");
        }
        return state->Invoke(std::move(request), deadline);
      });
  if (!service_result) {
    static_cast<void>(executor_->Cancel(impl->task_id));
    return service_result.status();
  }
  impl->service_id = service_result.value();
  return Service(std::move(impl));
}

Result<Client> Node::CreateClient(std::string service_name) const {
  if (service_name.empty()) {
    return Status(StatusCode::InvalidArgument,
                  "client requires a service name");
  }
  auto impl = std::make_shared<Client::Impl>();
  impl->node = config_;
  impl->service_name = std::move(service_name);
  impl->transport = transport_;
  return Client(std::move(impl));
}

Result<Timer> Node::CreateTimer(
    std::string name, CallbackGroupId callback_group,
    std::chrono::nanoseconds period,
    std::chrono::nanoseconds deadline,
    TaskCallback callback, int priority) const {
  if (name.empty() || period <= std::chrono::nanoseconds::zero() ||
      deadline <= std::chrono::nanoseconds::zero() || !callback) {
    return Status(StatusCode::InvalidArgument,
                  "timer requires name, positive period/deadline, and callback");
  }
  TaskConfig config;
  config.name = config_.name + ":timer:" + std::move(name);
  config.kind = TaskKind::Periodic;
  config.priority = priority;
  config.period = period;
  config.deadline = deadline;
  config.queue_capacity = 1U;
  config.callback_group = callback_group;
  auto task_result = executor_->AddTask(std::move(config), std::move(callback));
  if (!task_result) {
    return task_result.status();
  }
  auto impl = std::make_shared<Timer::Impl>();
  impl->executor = executor_;
  impl->task_id = task_result.value();
  return Timer(std::move(impl));
}

}  // namespace autoruntime
