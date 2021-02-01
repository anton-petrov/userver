#include "async_stream.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <engine/async.hpp>
#include <engine/deadline.hpp>
#include <engine/io/addr.hpp>
#include <engine/io/exception.hpp>
#include <engine/io/socket.hpp>
#include <engine/task/local_variable.hpp>
#include <engine/task/task_with_result.hpp>
#include <logging/log.hpp>
#include <utils/assert.hpp>

#include <engine/io/poller.hpp>
#include <storages/mongo/tcp_connect_precheck.hpp>
#include <storages/mongo/wrappers.hpp>

namespace storages::mongo::impl {
namespace {

constexpr size_t kBufferSize = 16 * 1024;

constexpr int kCompatibleMajorVersion = 1;
constexpr int kMaxCompatibleMinorVersion = 17;

static_assert((MONGOC_MAJOR_VERSION) == kCompatibleMajorVersion &&
                  (MONGOC_MINOR_VERSION) <= kMaxCompatibleMinorVersion,
              "Check mongoc_stream_t structure compatibility with "
              "version " MONGOC_VERSION_S);

void SetWatcher(engine::io::Poller::WatcherPtr& old_watcher,
                engine::io::Poller::WatcherPtr new_watcher) {
  if (old_watcher == new_watcher) return;
  if (old_watcher) old_watcher->Stop();
  old_watcher = std::move(new_watcher);
}

class AsyncStream : public mongoc_stream_t {
 public:
  static constexpr int kStreamType = 0x53755459;

  static StreamPtr Create(engine::io::Socket);

 private:
  AsyncStream(engine::io::Socket) noexcept;

  // mongoc_stream_buffered does not provide write buffering
  // NOTE: returns number of bytes sent, not buffered!
  size_t BufferedSend(void*, size_t, engine::Deadline);
  size_t FlushSendBuffer(engine::Deadline);

  // mongoc_stream_t interface
  static void Destroy(mongoc_stream_t*) noexcept;
  static int Close(mongoc_stream_t*) noexcept;
  static int Flush(mongoc_stream_t*) noexcept;
  static ssize_t Writev(mongoc_stream_t*, mongoc_iovec_t*, size_t,
                        int32_t) noexcept;
  static ssize_t Readv(mongoc_stream_t*, mongoc_iovec_t*, size_t, size_t,
                       int32_t) noexcept;
  static int Setsockopt(mongoc_stream_t*, int, int, void*,
                        mongoc_socklen_t) noexcept;
  static bool CheckClosed(mongoc_stream_t*) noexcept;
  // NOLINTNEXTLINE(bugprone-exception-escape)
  static ssize_t Poll(mongoc_stream_poll_t*, size_t, int32_t) noexcept;
  static void Failed(mongoc_stream_t*) noexcept;
  static bool TimedOut(mongoc_stream_t*) noexcept;
  static bool ShouldRetry(mongoc_stream_t*) noexcept;

  const uint64_t epoch_;
  engine::io::Socket socket_;
  engine::io::Poller::WatcherPtr read_watcher_;
  engine::io::Poller::WatcherPtr write_watcher_;
  bool is_timed_out_;

  size_t send_buffer_bytes_used_;
  // buffer size is adjusted for better heap utilization
  static constexpr size_t kMaxBufferOffset = 512;
  std::array<char, kBufferSize - kMaxBufferOffset> send_buffer_;
};
static_assert(sizeof(AsyncStream) <= kBufferSize &&
                  sizeof(AsyncStream) >= (kBufferSize / 2 + kBufferSize / 4),
              "AsyncStream has suboptimal size");

engine::Deadline DeadlineFromTimeoutMs(int32_t timeout_ms) {
  if (timeout_ms < 0) return {};
  if (!timeout_ms) {
    return engine::Deadline::FromTimePoint(engine::Deadline::kPassed);
  }
  return engine::Deadline::FromDuration(std::chrono::milliseconds(timeout_ms));
}

engine::io::Socket ConnectUnix(const mongoc_host_list_t& host,
                               int32_t timeout_ms, bson_error_t* error) {
  engine::io::AddrStorage addr;
  auto* sa = addr.As<sockaddr_un>();
  sa->sun_family = AF_UNIX;

  const auto host_len = std::strlen(host.host);
  if (host_len >= sizeof(sa->sun_path)) {
    bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                   "Cannot connect to UNIX socket '%s': path too long",
                   host.host);
    return {};
  }
  std::memcpy(sa->sun_path, host.host, host_len);

  try {
    return engine::io::Connect(engine::io::Addr(addr, SOCK_STREAM, 0),
                               DeadlineFromTimeoutMs(timeout_ms));
  } catch (const engine::io::IoCancelled& ex) {
    // do not log
    bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                   "%s", ex.what());
    return {};
  } catch (const std::exception& ex) {
    LOG_INFO() << "Cannot connect to UNIX socket '" << host.host << "': " << ex;
  }
  bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                 "Cannot connect to UNIX socket '%s'", host.host);
  return {};
}

struct AddrinfoDeleter {
  void operator()(struct addrinfo* p) const noexcept { freeaddrinfo(p); }
};
using AddrinfoPtr = std::unique_ptr<struct addrinfo, AddrinfoDeleter>;

engine::io::Socket ConnectTcpByName(const mongoc_host_list_t& host,
                                    int32_t timeout_ms, bson_error_t* error) {
  if (!IsTcpConnectAllowed(host.host_and_port)) {
    bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                   "Too many connection errors in recent period for %s",
                   host.host_and_port);
    return {};
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = host.family;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* ai_result_raw;
  LOG_DEBUG() << "Trying to resolve " << host.host_and_port;
  const auto port_string = std::to_string(host.port);
  if (getaddrinfo(host.host, port_string.c_str(), &hints, &ai_result_raw)) {
    ReportTcpConnectError(host.host_and_port);
    bson_set_error(error, MONGOC_ERROR_STREAM,
                   MONGOC_ERROR_STREAM_NAME_RESOLUTION, "Cannot resolve %s",
                   host.host_and_port);
    return {};
  }
  AddrinfoPtr ai_result(ai_result_raw);
  try {
    for (auto res = ai_result.get(); res; res = res->ai_next) {
      engine::io::Addr current_addr(res->ai_addr, res->ai_socktype,
                                    res->ai_protocol);
      try {
        auto socket = engine::io::Connect(current_addr,
                                          DeadlineFromTimeoutMs(timeout_ms));
        socket.SetOption(IPPROTO_TCP, TCP_NODELAY, 1);
        ReportTcpConnectSuccess(host.host_and_port);
        return socket;
      } catch (const engine::io::IoCancelled& ex) {
        // do not log or account
        bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                       "%s", ex.what());
        return {};
      } catch (const engine::io::IoException& ex) {
        LOG_DEBUG() << "Cannot connect to " << host.host << " at "
                    << current_addr << ": " << ex;
      }
    }
  } catch (const std::exception& ex) {
    LOG_LIMITED_ERROR() << "Cannot connect to " << host.host << ": " << ex;
  }
  ReportTcpConnectError(host.host_and_port);
  bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT,
                 "Cannot connect to %s", host.host_and_port);
  return {};
}

engine::io::Socket Connect(const mongoc_host_list_t* host, int32_t timeout_ms,
                           bson_error_t* error) {
  UASSERT(host);
  switch (host->family) {
    case AF_UNSPEC:  // mongoc thinks this is okay
    case AF_INET:
    case AF_INET6:
      return ConnectTcpByName(*host, timeout_ms, error);

    case AF_UNIX:
      return ConnectUnix(*host, timeout_ms, error);

    default:
      bson_set_error(error, MONGOC_ERROR_STREAM,
                     MONGOC_ERROR_STREAM_INVALID_TYPE,
                     "Invalid address family 0x%02x", host->family);
  }
  return {};
}

uint64_t GetNextStreamEpoch() {
  static std::atomic<uint64_t> current_epoch{0};
  return current_epoch++;
}

// We need to reset the poller because of fd reuse and to wipe stale events.
// This operation syncs on ev loops so we want it to be done
// as rarely as possible.
//
// mongoc uses poll only on freshly made streams, we use that knowledge
// to only reset poller when a new poll cycle begins.
class PollerDispenser {
 public:
  engine::io::Poller& Get(uint64_t current_epoch) {
    if (seen_epoch_ < current_epoch) {
      poller_.Reset();
      seen_epoch_ = current_epoch;
    }
    return poller_;
  }

 private:
  uint64_t seen_epoch_{0};
  engine::io::Poller poller_;
};

}  // namespace

void CheckAsyncStreamCompatible() {
  if (mongoc_get_major_version() != kCompatibleMajorVersion ||
      mongoc_get_minor_version() > kMaxCompatibleMinorVersion) {
    throw std::runtime_error(
        "This implementation of AsyncStream was not checked with "
        "libmongoc " MONGOC_VERSION_S
        " and may be binary incompatible with it, please downgrade to "
        "version " +
        std::to_string(kCompatibleMajorVersion) + '.' +
        std::to_string(kMaxCompatibleMinorVersion));
  }
}

mongoc_stream_t* MakeAsyncStream(const mongoc_uri_t* uri,
                                 const mongoc_host_list_t* host,
                                 void* user_data,
                                 bson_error_t* error) noexcept {
  const auto connect_timeout_ms =
      mongoc_uri_get_option_as_int32(uri, MONGOC_URI_CONNECTTIMEOUTMS, 5000);
  auto socket = Connect(host, connect_timeout_ms, error);
  if (!socket) return nullptr;

  auto stream = AsyncStream::Create(std::move(socket));

  // from mongoc_client_default_stream_initiator
  // enable TLS if needed
  const char* mechanism = mongoc_uri_get_auth_mechanism(uri);
  if (mongoc_uri_get_tls(uri) ||
      (mechanism && !std::strcmp(mechanism, "MONGODB-X509"))) {
    auto* ssl_opt = static_cast<mongoc_ssl_opt_t*>(user_data);

    {
      StreamPtr wrapped_stream(mongoc_stream_tls_new_with_hostname(
          stream.get(), host->host, ssl_opt, true));
      if (!wrapped_stream) {
        bson_set_error(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_SOCKET,
                       "Cannot initialize TLS stream");
        return nullptr;
      }
      [[maybe_unused]] auto ptr = stream.release();
      stream = std::move(wrapped_stream);
    }

    if (!mongoc_stream_tls_handshake_block(stream.get(), host->host,
                                           connect_timeout_ms, error)) {
      return nullptr;
    }
  }

  // enable read buffering
  UASSERT(stream);
  return mongoc_stream_buffered_new(stream.release(), kBufferSize);
}

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
AsyncStream::AsyncStream(engine::io::Socket socket) noexcept
    : epoch_(GetNextStreamEpoch()),
      socket_(std::move(socket)),
      is_timed_out_(false),
      send_buffer_bytes_used_(0) {
  type = kStreamType;
  destroy = &Destroy;
  close = &Close;
  flush = &Flush;
  writev = &Writev;
  readv = &Readv;
  setsockopt = &Setsockopt;
  get_base_stream = nullptr;
  check_closed = &CheckClosed;
  poll = &Poll;
  failed = &Failed;
  timed_out = &TimedOut;
  should_retry = &ShouldRetry;
}

size_t AsyncStream::BufferedSend(void* data, size_t size,
                                 engine::Deadline deadline) {
  size_t bytes_sent = 0;

  size_t bytes_left = size;
  const char* pos = static_cast<const char*>(data);
  while (bytes_left) {
    const size_t batch_size = std::min<size_t>(
        bytes_left, send_buffer_.size() - send_buffer_bytes_used_);

    std::memcpy(send_buffer_.data() + send_buffer_bytes_used_, pos, batch_size);
    send_buffer_bytes_used_ += batch_size;
    bytes_left -= batch_size;
    pos += batch_size;

    UASSERT(send_buffer_bytes_used_ <= send_buffer_.size());
    if (send_buffer_bytes_used_ == send_buffer_.size()) {
      bytes_sent += FlushSendBuffer(deadline);
    }
  }
  return bytes_sent;
}

size_t AsyncStream::FlushSendBuffer(engine::Deadline deadline) {
  const size_t bytes_to_send = std::exchange(send_buffer_bytes_used_, 0);
  if (!bytes_to_send) return 0;
  return socket_.SendAll(send_buffer_.data(), bytes_to_send, deadline);
}

StreamPtr AsyncStream::Create(engine::io::Socket socket) {
  return StreamPtr(new AsyncStream(std::move(socket)));
}

void AsyncStream::Destroy(mongoc_stream_t* stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Destroying async stream " << self;

  Close(stream);

  delete self;
}

int AsyncStream::Close(mongoc_stream_t* stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Closing async stream " << self;
  self->is_timed_out_ = false;

  SetWatcher(self->read_watcher_, {});
  SetWatcher(self->write_watcher_, {});
  try {
    self->socket_.Close();
  } catch (const std::exception&) {
    return -1;
  }
  return 0;
}

int AsyncStream::Flush(mongoc_stream_t* stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Flushing async stream " << self;
  self->is_timed_out_ = false;

  return 0;
}

ssize_t AsyncStream::Writev(mongoc_stream_t* stream, mongoc_iovec_t* iov,
                            size_t iovcnt, int32_t timeout_ms) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Writing to async stream " << self;
  self->is_timed_out_ = false;
  int error = 0;

  const auto deadline = DeadlineFromTimeoutMs(timeout_ms);

  ssize_t bytes_sent = 0;
  try {
    for (size_t i = 0; i < iovcnt; ++i) {
      bytes_sent +=
          self->BufferedSend(iov[i].iov_base, iov[i].iov_len, deadline);
    }
    bytes_sent += self->FlushSendBuffer(deadline);
  } catch (const engine::io::IoCancelled&) {
    if (!bytes_sent) {
      error = EINVAL;
      bytes_sent = -1;
    }
  } catch (const engine::io::IoTimeout& timeout_ex) {
    self->is_timed_out_ = true;
    error = ETIMEDOUT;
    bytes_sent += timeout_ex.BytesTransferred();
  } catch (const engine::io::IoSystemError& io_sys) {
    error = io_sys.Code().value();
    bytes_sent = -1;
  } catch (const engine::io::IoException& io_ex) {
    error = EINVAL;
    bytes_sent = -1;
  }
  // libmongoc expects restored errno
  errno = error;
  return bytes_sent;
}

ssize_t AsyncStream::Readv(mongoc_stream_t* stream, mongoc_iovec_t* iov,
                           size_t iovcnt, size_t min_bytes,
                           int32_t timeout_ms) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Reading from async stream " << self;
  self->is_timed_out_ = false;
  int error = 0;

  const auto deadline = DeadlineFromTimeoutMs(timeout_ms);

  size_t recvd_total = 0;
  try {
    size_t curr_iov = 0;
    while (curr_iov < iovcnt) {
      auto recvd_now = self->socket_.RecvSome(iov[curr_iov].iov_base,
                                              iov[curr_iov].iov_len, deadline);
      recvd_total += recvd_now;
      if (!recvd_now || recvd_total >= min_bytes) break;

      iov[curr_iov].iov_base =
          reinterpret_cast<char*>(iov[curr_iov].iov_base) + recvd_now;
      iov[curr_iov].iov_len -= recvd_now;

      if (!iov[curr_iov].iov_len) ++curr_iov;
    }
  } catch (const engine::io::IoCancelled&) {
    if (!recvd_total) error = EINVAL;
  } catch (const engine::io::IoTimeout& timeout_ex) {
    self->is_timed_out_ = true;
    error = ETIMEDOUT;
    recvd_total += timeout_ex.BytesTransferred();
  } catch (const engine::io::IoSystemError& io_sys) {
    error = io_sys.Code().value();
  } catch (const engine::io::IoException& io_ex) {
    error = EINVAL;
  }
  // return value logic from _mongoc_stream_socket_readv
  if (recvd_total < min_bytes) {
    // libmongoc expects restored errno
    errno = error;
    return -1;
  }
  return recvd_total;
}

int AsyncStream::Setsockopt(mongoc_stream_t* stream, int level, int optname,
                            void* optval, mongoc_socklen_t optlen) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Setting socket option for async stream " << self;
  self->is_timed_out_ = false;

  return ::setsockopt(self->socket_.Fd(), level, optname, optval, optlen);
}

bool AsyncStream::CheckClosed(mongoc_stream_t* base_stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* stream = static_cast<AsyncStream*>(base_stream);
  LOG_TRACE() << "Checking whether async stream is closed";
  stream->is_timed_out_ = false;

  // XXX: mongoc peeks to get ECONNRESET
  return !stream->socket_.IsValid();
}

// NOLINTNEXTLINE(bugprone-exception-escape)
ssize_t AsyncStream::Poll(mongoc_stream_poll_t* streams, size_t nstreams,
                          int32_t timeout_ms) noexcept {
  static engine::TaskLocalVariable<PollerDispenser> poller_dispenser;

  LOG_TRACE() << "Polling " << nstreams << " async streams";

  if (!nstreams) return 0;

  if (engine::current_task::ShouldCancel()) {
    // mark all streams as errored out, mongoc tend to ignore poll errors
    for (size_t i = 0; i < nstreams; ++i) streams[i].revents = POLLERR;
    return nstreams;
  }

  const auto deadline = DeadlineFromTimeoutMs(timeout_ms);

  uint64_t current_epoch = 0;
  std::vector<int> stream_fds(nstreams);
  for (size_t i = 0; i < nstreams; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* stream = static_cast<const AsyncStream*>(streams[i].stream);
    current_epoch = std::max(current_epoch, stream->epoch_);
    stream_fds[i] = stream->socket_.Fd();
  }
  auto& poller = poller_dispenser->Get(current_epoch);

  for (size_t i = 0; i < nstreams; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* stream = static_cast<AsyncStream*>(streams[i].stream);
    if (streams[i].events & POLLOUT) {
      SetWatcher(stream->write_watcher_, poller.AddWrite(stream_fds[i]));
    } else if (streams[i].events) {
      SetWatcher(stream->read_watcher_, poller.AddRead(stream_fds[i]));
    }
    streams[i].revents = 0;
  }

  ssize_t ready = 0;
  try {
    engine::io::Poller::Event poller_event;
    for (bool has_more = poller.NextEvent(poller_event, deadline); has_more;
         has_more = poller.NextEventNoblock(poller_event)) {
      for (size_t i = 0; i < nstreams; ++i) {
        if (stream_fds[i] == poller_event.fd) {
          ready += !streams[i].revents;
          switch (poller_event.type) {
            case engine::io::Poller::Event::kError:
              streams[i].revents |= POLLERR;
              break;
            case engine::io::Poller::Event::kRead:
              streams[i].revents |= streams[i].events & POLLIN;
              break;
            case engine::io::Poller::Event::kWrite:
              streams[i].revents |= streams[i].events & POLLOUT;
              break;
          }
          break;
        }
      }
    }
  } catch (const std::exception&) {
    errno = EINVAL;
    return -1;
  }

  return ready;
}

void AsyncStream::Failed(mongoc_stream_t* stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Failing async stream " << self;

  Destroy(self);
}

bool AsyncStream::TimedOut(mongoc_stream_t* stream) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* self = static_cast<AsyncStream*>(stream);
  LOG_TRACE() << "Checking timeout state for async stream " << self;

  return self->is_timed_out_;
}

bool AsyncStream::ShouldRetry(mongoc_stream_t*) noexcept {
  // we handle socket retries ourselves
  return false;
}

}  // namespace storages::mongo::impl
