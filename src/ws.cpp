// Minimal RFC 6455 WebSocket client over TLS. See ws.hpp for scope and rationale.
//
// Frame layout recap (RFC 6455 section 5.2):
//   byte 0: FIN(1) RSV(3) opcode(4)
//   byte 1: MASK(1) len7(7)
//   len7 == 126 -> 2-byte big-endian length, == 127 -> 8-byte big-endian length
//   if MASK: 4-byte masking key, then the payload XORed with it
// A client MUST mask every frame it sends (section 5.3); this one always does.
#include "ws.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>  // X509_V_OK, X509_verify_cert_error_string

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace dwm {
namespace {

// Opcodes.
constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

// The fixed value RFC 6455 section 1.3 requires in Sec-WebSocket-Accept.
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** Upper bound on a single incoming message. A danmaku batch is a few kilobytes; this leaves
 *  three orders of magnitude of headroom while still refusing to let one frame exhaust memory. */
constexpr size_t kMaxMessageBytes = 64u * 1024u * 1024u;

std::string opensslError() {
  char buf[256];
  const unsigned long e = ERR_get_error();
  if (!e) return "no OpenSSL error recorded";
  ERR_error_string_n(e, buf, sizeof(buf));
  return buf;
}

std::string base64(const unsigned char* data, size_t len) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back(kAlphabet[v & 0x3F]);
  }
  if (i < len) {
    uint32_t v = uint32_t(data[i]) << 16;
    const bool two = (i + 1 < len);
    if (two) v |= uint32_t(data[i + 1]) << 8;
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(two ? kAlphabet[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

/** Monotonic milliseconds. Used for deadlines, so a wall-clock adjustment cannot extend or cut
 *  short a wait. */
int64_t steadyNowMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/** Waits for readiness on fd_. Returns 1 ready, 0 timeout, -1 error. */
int waitFd(int fd, short events, int timeoutMs) {
  struct pollfd pfd {};
  pfd.fd = fd;
  pfd.events = events;
  for (;;) {
    const int rc = ::poll(&pfd, 1, timeoutMs);
    if (rc > 0) return 1;
    if (rc == 0) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

}  // namespace

WsClient::~WsClient() { close(); }

void WsClient::fail(std::string message) {
  if (error_.empty()) error_ = std::move(message);
}

void WsClient::tcpConnect(const std::string& host, int port, int timeoutMs) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
  if (rc != 0) {
    throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(rc));
  }

  int64_t deadline = timeoutMs;
  int lastErrno = 0;
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) {
      lastErrno = errno;
      continue;
    }
    // Non-blocking from here on: connect() and the TLS handshake are both given the same deadline
    // instead of each getting their own.
    const int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const int cr = ::connect(sock, ai->ai_addr, ai->ai_addrlen);
    if (cr == 0) {
      fd_ = sock;
      ::freeaddrinfo(res);
      return;
    }
    if (errno != EINPROGRESS) {
      lastErrno = errno;
      ::close(sock);
      continue;
    }
    const int ready = waitFd(sock, POLLOUT, static_cast<int>(deadline));
    if (ready == 1) {
      int err = 0;
      socklen_t len = sizeof(err);
      if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
        fd_ = sock;
        ::freeaddrinfo(res);
        return;
      }
      lastErrno = err ? err : ECONNREFUSED;
    } else {
      lastErrno = (ready == 0) ? ETIMEDOUT : errno;
    }
    ::close(sock);
  }
  ::freeaddrinfo(res);
  throw std::runtime_error("connect " + host + ": " + std::strerror(lastErrno ? lastErrno : ECONNREFUSED));
}

void WsClient::tlsHandshake(int timeoutMs) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) throw std::runtime_error("SSL_CTX_new: " + opensslError());
  ctx_ = ctx;

  // Default trust store; on a normal distro install this is /etc/ssl/certs. Skipping verification
  // is available behind an explicit option and is not the default.
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  if (options_.insecureSkipVerify) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  }

  SSL* ssl = SSL_new(ctx);
  if (!ssl) throw std::runtime_error("SSL_new: " + opensslError());
  ssl_ = ssl;

  // SNI, plus hostname verification. Without both, a default certificate would validate.
  SSL_set_tlsext_host_name(ssl, options_.host.c_str());
  SSL_set1_host(ssl, options_.host.c_str());
  SSL_set_fd(ssl, fd_);

  for (;;) {
    const int rc = SSL_connect(ssl);
    if (rc == 1) break;
    const int err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
      if (waitFd(fd_, POLLIN, timeoutMs) != 1) throw std::runtime_error("TLS handshake timed out");
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      if (waitFd(fd_, POLLOUT, timeoutMs) != 1) throw std::runtime_error("TLS handshake timed out");
      continue;
    }
    std::string msg = "TLS handshake failed";
    if (long vr = SSL_get_verify_result(ssl); vr != X509_V_OK) {
      msg += ": certificate not trusted (" + std::string(X509_verify_cert_error_string(vr)) + ")";
    } else {
      msg += ": " + opensslError();
    }
    throw std::runtime_error(msg);
  }
}

long WsClient::readSome(char* buf, size_t n, int timeoutMs) {
  if (!ssl_) return -1;
  // OpenSSL is called FIRST and the socket is polled only when OpenSSL says it wants more bytes.
  // The reverse order (poll, then one SSL_read) loses data whenever a TLS record arrives split
  // across TCP segments: the read consumes what is buffered, returns WANT_READ, and the remainder
  // is only picked up by a later call that the caller may well read as a timeout. Observed on the
  // bilibili endpoint, where the 101 response arrives in more than one record.
  const int64_t deadline = steadyNowMs() + timeoutMs;
  for (;;) {
    const int nread = SSL_read(static_cast<SSL*>(ssl_), buf, static_cast<int>(n));
    if (nread > 0) return nread;

    const int err = SSL_get_error(static_cast<SSL*>(ssl_), nread);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      const int left = static_cast<int>(deadline - steadyNowMs());
      if (left <= 0) return -1;  // deadline reached, no error recorded
      const short want = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
      const int ready = waitFd(fd_, want, left);
      if (ready == 0) return -1;
      if (ready < 0) {
        fail(std::string("poll: ") + std::strerror(errno));
        return -1;
      }
      continue;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0;
    fail(std::string("TLS read: ") +
         (err == SSL_ERROR_SYSCALL ? std::strerror(errno) : opensslError()));
    return -1;
  }
}

bool WsClient::writeAll(const char* buf, size_t n, int timeoutMs) {
  // Same ordering as readSome: write first, wait only on demand. Writing first matters because
  // SSL_write may need several passes to push a large frame through a full send buffer.
  const int64_t deadline = steadyNowMs() + timeoutMs;
  size_t written = 0;
  while (written < n) {
    const int rc =
        SSL_write(static_cast<SSL*>(ssl_), buf + written, static_cast<int>(n - written));
    if (rc > 0) {
      written += static_cast<size_t>(rc);
      continue;
    }
    const int err = SSL_get_error(static_cast<SSL*>(ssl_), rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      const int left = static_cast<int>(deadline - steadyNowMs());
      if (left <= 0) {
        if (error_.empty()) fail("TLS write timed out");
        return false;
      }
      const short want = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
      const int ready = waitFd(fd_, want, left);
      if (ready == 0) {
        if (error_.empty()) fail("TLS write timed out");
        return false;
      }
      if (ready < 0) {
        fail(std::string("poll: ") + std::strerror(errno));
        return false;
      }
      continue;
    }
    fail(std::string("TLS write: ") + opensslError());
    return false;
  }
  return true;
}

bool WsClient::fill(size_t n, int timeoutMs) {
  while (rbuf_.size() - rpos_ < n) {
    // Keep the buffer from growing without bound as it is consumed.
    if (rpos_ > 64u * 1024u) {
      rbuf_.erase(0, rpos_);
      rpos_ = 0;
    }
    if (rbuf_.size() >= kMaxMessageBytes) {
      fail("incoming message exceeds the size cap");
      return false;
    }
    char buf[16384];
    const long got = readSome(buf, sizeof(buf), timeoutMs);
    if (got > 0) {
      rbuf_.append(buf, static_cast<size_t>(got));
      continue;
    }
    if (got == 0) {
      fail("peer closed the TLS connection");
      return false;
    }
    if (!error_.empty()) return false;
    return false;  // read timeout before n bytes arrived
  }
  return true;
}

void WsClient::connect(const WsOptions& opt) {
  options_ = opt;
  error_.clear();

  tcpConnect(opt.host, opt.port, opt.timeoutMs);
  try {
    tlsHandshake(opt.timeoutMs);
  } catch (...) {
    close();
    throw;
  }
  try {
    httpUpgrade(opt);
  } catch (...) {
    close();
    throw;
  }
}

void WsClient::httpUpgrade(const WsOptions& opt) {
  unsigned char nonce[16];
  if (RAND_bytes(nonce, sizeof(nonce)) != 1) throw std::runtime_error("RAND_bytes failed");
  const std::string key = base64(nonce, sizeof(nonce));

  std::string req = "GET " + opt.path + " HTTP/1.1\r\n";
  req += "Host: " + opt.host + ":" + std::to_string(opt.port) + "\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: " + key + "\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  for (const auto& h : opt.headers) req += h.first + ": " + h.second + "\r\n";
  req += "\r\n";
  if (!writeAll(req.data(), req.size(), opt.timeoutMs)) {
    throw std::runtime_error("sending the upgrade request: " + error_);
  }

  // Read until the end of the header block. Bounded so a peer that never sends CRLFCRLF cannot
  // make this loop forever.
  size_t headerEnd = std::string::npos;
  while ((headerEnd = rbuf_.find("\r\n\r\n")) == std::string::npos) {
    if (rbuf_.size() > 64u * 1024u) throw std::runtime_error("upgrade response header too large");
    char buf[4096];
    const long got = readSome(buf, sizeof(buf), opt.timeoutMs);
    if (got <= 0) {
      throw std::runtime_error(got == 0 ? "peer closed during the upgrade"
                                        : (error_.empty() ? "upgrade response timed out" : error_));
    }
    rbuf_.append(buf, static_cast<size_t>(got));
  }

  const std::string head = rbuf_.substr(0, headerEnd);
  rbuf_.erase(0, headerEnd + 4);
  rpos_ = 0;

  const size_t eol = head.find("\r\n");
  const std::string statusLine = head.substr(0, eol == std::string::npos ? head.size() : eol);
  if (statusLine.find(" 101") == std::string::npos) {
    throw std::runtime_error("upgrade refused: " + statusLine);
  }

  // Sec-WebSocket-Accept must be base64(SHA1(key + GUID)); verifying it is what makes this a
  // WebSocket handshake rather than a plain HTTP 200 that happens to say 101.
  const std::string expect = wsAcceptKey(key);

  bool acceptOk = false;
  size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    if (next == std::string::npos) next = head.size();
    const std::string line = head.substr(pos, next - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      for (char& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      if (name == "sec-websocket-accept") acceptOk = (value == expect);
    }
    pos = next + 2;
  }
  if (!acceptOk) throw std::runtime_error("Sec-WebSocket-Accept mismatch (not a WebSocket server)");
}

std::string wsAcceptKey(const std::string& key) {
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>((key + kWsGuid).data()), key.size() + strlen(kWsGuid),
       digest);
  return base64(digest, sizeof(digest));
}

bool WsClient::sendFrame(uint8_t opcode, const char* data, size_t len) {
  if (!ssl_) {
    fail("not connected");
    return false;
  }
  if (len > kMaxMessageBytes) {
    fail("outgoing message exceeds the size cap");
    return false;
  }

  std::string frame;
  frame.reserve(len + 14);
  frame.push_back(static_cast<char>(0x80 | opcode));  // FIN, no extensions
  unsigned char mask[4];
  if (RAND_bytes(mask, sizeof(mask)) != 1) {
    fail("RAND_bytes failed");
    return false;
  }
  if (len < 126) {
    frame.push_back(static_cast<char>(0x80 | len));
  } else if (len <= 0xFFFF) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
  }
  frame.append(reinterpret_cast<const char*>(mask), 4);

  // Mask in place over a copy of the tail of the frame, so the caller's buffer is untouched.
  const size_t bodyStart = frame.size();
  frame.append(data, len);
  for (size_t i = 0; i < len; ++i) {
    frame[bodyStart + i] = static_cast<char>(frame[bodyStart + i] ^ mask[i & 3]);
  }
  return writeAll(frame.data(), frame.size(), 10000);
}

bool WsClient::sendBinary(std::string_view payload) {
  return sendFrame(kOpBinary, payload.data(), payload.size());
}

bool WsClient::sendText(std::string_view payload) {
  return sendFrame(kOpText, payload.data(), payload.size());
}

bool WsClient::readFrame(uint8_t& opcode, bool& fin, std::string& payload, int timeoutMs) {
  if (!fill(2, timeoutMs)) return false;
  const uint8_t b0 = static_cast<uint8_t>(rbuf_[rpos_]);
  const uint8_t b1 = static_cast<uint8_t>(rbuf_[rpos_ + 1]);
  rpos_ += 2;
  opcode = static_cast<uint8_t>(b0 & 0x0F);
  fin = (b0 & 0x80) != 0;
  if (b0 & 0x70) {
    fail("RSV bits set but no extension was negotiated");
    return false;
  }

  uint64_t len = b1 & 0x7F;
  if (len == 126) {
    if (!fill(2, timeoutMs)) return false;
    len = (uint64_t(static_cast<uint8_t>(rbuf_[rpos_])) << 8) |
          static_cast<uint8_t>(rbuf_[rpos_ + 1]);
    rpos_ += 2;
  } else if (len == 127) {
    if (!fill(8, timeoutMs)) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(rbuf_[rpos_ + i]);
    rpos_ += 8;
  }
  if (len > kMaxMessageBytes) {
    fail("incoming frame exceeds the size cap");
    return false;
  }
  // A control frame may not be fragmented and may not carry more than 125 bytes (section 5.5).
  // The close path echoes the peer's payload, so an oversized close let through here would go back
  // out as an oversized frame -- a violation on the way in and another on the way out. The server
  // half of this protocol refuses these, and this is the same rule on this side.
  if ((opcode & 0x08) && (!fin || len > 125)) {
    fail("invalid control frame");
    return false;
  }

  // A server never masks, per RFC 6455 section 5.1. Refusing rather than ignoring keeps a
  // protocol error from being silently misread as data.
  if (b1 & 0x80) {
    fail("server sent a masked frame (protocol violation)");
    return false;
  }
  if (len && !fill(static_cast<size_t>(len), timeoutMs)) return false;
  payload.assign(rbuf_, rpos_, static_cast<size_t>(len));
  rpos_ += static_cast<size_t>(len);
  return true;
}

WsClient::Event WsClient::next(std::string& out, int timeoutMs) {
  if (!ssl_) {
    fail("not connected");
    return Event::Error;
  }
  out.clear();

  for (;;) {
    uint8_t opcode = 0;
    bool fin = false;
    std::string payload;
    if (!readFrame(opcode, fin, payload, timeoutMs)) {
      return error_.empty() ? Event::Timeout : Event::Error;
    }

    switch (opcode) {
      case kOpPing:
        // Answered here rather than surfaced: a keepalive is the transport's business, and the
        // chat protocol has no use for it.
        if (!sendFrame(kOpPong, payload.data(), payload.size())) return Event::Error;
        continue;
      case kOpPong:
        continue;
      case kOpClose:
        out = payload.substr(0, 2);  // status code, big-endian
        if (payload.size() > 2) out += payload.substr(2);  // and the reason text
        sendFrame(kOpClose, payload.data(), payload.size());
        return Event::Close;
      case kOpContinuation:
        if (fragmentOpcode_ == 0) {
          fail("continuation frame without a start frame");
          return Event::Error;
        }
        fragment_.append(payload);
        if (fragment_.size() > kMaxMessageBytes) {
          fail("fragmented message exceeds the size cap");
          return Event::Error;
        }
        if (!fin) continue;
        opcode = fragmentOpcode_;
        fragmentOpcode_ = 0;
        // The swap has to come before fragment_ is cleared: clearing first throws the assembled
        // message away and leaves an empty payload here, so every fragmented message used to
        // arrive as an empty one. The swap leaves the frame's own chunk in fragment_, which is
        // what the clear then removes.
        payload.swap(fragment_);
        fragment_.clear();
        break;
      case kOpText:
      case kOpBinary:
        if (fragmentOpcode_ != 0) {
          fail("new data frame while a fragmented message was open");
          return Event::Error;
        }
        if (!fin) {
          fragmentOpcode_ = opcode;
          fragment_.swap(payload);
          continue;
        }
        break;
      default:
        fail("unknown opcode");
        return Event::Error;
    }

    out.swap(payload);
    return opcode == kOpText ? Event::Text : Event::Binary;
  }
}

void WsClient::close(int code) {
  if (ssl_ && fd_ >= 0) {
    unsigned char payload[2] = {static_cast<unsigned char>(code >> 8),
                                static_cast<unsigned char>(code & 0xFF)};
    sendFrame(kOpClose, reinterpret_cast<const char*>(payload), sizeof(payload));
  }
  if (ssl_) SSL_free(static_cast<SSL*>(ssl_));
  if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
  if (fd_ >= 0) ::close(fd_);
  ssl_ = nullptr;
  ctx_ = nullptr;
  fd_ = -1;
  rbuf_.clear();
  rpos_ = 0;
  fragment_.clear();
  fragmentOpcode_ = 0;
}

}  // namespace dwm