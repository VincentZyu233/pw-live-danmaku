// The outbound feed: the wire format and the loopback server. See broadcast.hpp for scope, and for
// why this is not part of ws.{hpp,cpp}.
#include "broadcast.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "ws.hpp"  // wsAcceptKey(), the one computation the two ends of the handshake share

namespace dwm {
namespace {

// Opcodes, RFC 6455 section 5.2.
constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

/** An un-upgraded client is given this long to send its request. */
constexpr int kHandshakeMs = 5000;
/** Cap on the bytes read from one client. A client's frames are read only so the socket does not
 *  fill up while it talks to itself: nothing here is data this program wants, so the cap can be
 *  small and the handler can afford to be strict. */
constexpr size_t kMaxClientBytes = 64u * 1024u;

/** The panel's default tokens, copied rather than read from PanelTokens for two reasons: the feed
 *  has no token overrides of its own to respect, and reaching into panel.hpp would drag cairo and
 *  pango into a module that otherwise only needs a socket. They are here so a message's name, its
 *  gift verb and a gift's unloaded name keep the colours the panel gives them. */
constexpr uint32_t kNameNormal = 0x9AA0A6;
constexpr uint32_t kNameMember = 0x0F9D58;
constexpr uint32_t kNameModerator = 0x5E84F1;
constexpr uint32_t kNameOwner = 0xFFD600;
constexpr uint32_t kVerbGift = 0xF5C542;
constexpr uint32_t kNameGift = 0xFB7299;

/** The row alpha the panel dims the ambient kinds with, as the byte <alpha> wants: 0.5 -> #80.
 *  Entry and liked rows arrive an order of magnitude faster than chat, so on the panel they are
 *  half-strength; a text feed that showed them at full strength would re-create exactly the
 *  crowding the dimming exists to prevent. */
constexpr const char* kRtfDim = "<alpha=#80>";

int64_t steadyNowMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/** A field as it goes on the wire: control characters become spaces.
 *
 *  Tab is the one that matters -- a tab in the name or the body would split one message into four
 *  fields and a client that splits on tabs would read the rest of the body as an avatar URL -- but
 *  CR, LF and the rest are treated the same way, because a text component is single-line and a
 *  control character that survives to it is either invisible or a stray box. DEL is in for the same
 *  reason. Multi-byte UTF-8 never matches: every byte of a continuation is >= 0x80. */
std::string wireField(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) c = ' ';
  }
  return out;
}

/** Text as it goes into an RTF field, with `<` disarmed.
 *
 *  Resonite's only escape is <noparse>: the closed form cannot contain a literal `</noparse>`, so
 *  it cannot wrap arbitrary chat text, and the counted form `<noparse=N>` shields exactly the N
 *  characters that follow it. One character at a time is deliberate rather than the whole run as a
 *  single N: `<` is one unit whether the count is measured in bytes, UTF-16 units or code points,
 *  and the run around it is usually CJK, where those three disagree with each other and with the
 *  byte length this program has in hand. Getting that count wrong would release the `</color>` that
 *  follows the run into the text as markup. */
void appendLiteral(std::string& out, std::string_view s) {
  for (const char c : s) {
    if (c == '<') out += "<noparse=1>";
    out += c;
  }
}

void appendColorOpen(std::string& out, uint32_t rgb) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "<color=#%06X>", rgb & 0xFFFFFFu);
  out += buf;
}

void appendStyled(std::string& out, uint32_t rgb, std::string_view text) {
  appendColorOpen(out, rgb);
  appendLiteral(out, text);
  out += "</color>";
}

/** The name colour, in the panel's order of preference: the colour the sender chose, else the
 *  colour of their role. */
uint32_t nameColor(const Message& m) {
  if (m.userColor != 0) return m.userColor;
  switch (m.type) {
    case UserType::Owner: return kNameOwner;
    case UserType::Moderator: return kNameModerator;
    case UserType::Member: return kNameMember;
    default: return kNameNormal;
  }
}

}  // namespace

std::string broadcastLine(const Message& m, bool rtf) {
  std::string out = wireField(m.avatarUrl);
  out += '\t';

  if (!rtf) {
    out += wireField(m.user);
    out += '\t';
    // plainText() keeps an emote's token as the sender typed it, which is the best a text field
    // can do with a picture: the token is what the panel falls back to as well when the image has
    // not arrived.
    out += wireField(m.plainText());
    return out;
  }

  const bool dim = (m.kind == MsgKind::Entry || m.kind == MsgKind::Like);
  if (dim) out += kRtfDim;
  appendStyled(out, nameColor(m), wireField(m.user));
  if (dim) out += "</alpha>";
  out += '\t';

  if (dim) out += kRtfDim;
  for (const Fragment& f : m.parts) {
    const std::string text = wireField(f.text);
    if (text.empty()) continue;
    if (f.verb) {
      // The gift's "投喂" and the like's "为主播点赞了" are the verbs of their rows; on the panel
      // they are the one run drawn in the accent colour rather than body white.
      appendStyled(out, kVerbGift, text);
    } else if (f.kind == Fragment::Kind::Emote && m.kind == MsgKind::Gift) {
      // A gift picture cannot be a <sprite>: sprites resolve by name against a DynamicSpriteFont
      // the client would have to have registered every gift in. Falling back to the token is what
      // the panel does while the picture is in flight, and the pink is the colour it does it in.
      appendStyled(out, kNameGift, text);
    } else {
      appendLiteral(out, text);
    }
  }
  if (dim) out += "</alpha>";
  return out;
}

WsServer::~WsServer() {
  stop();
  for (Client& c : clients_)
    if (c.fd >= 0) ::close(c.fd);
  if (listenFd_ >= 0) ::close(listenFd_);
}

void WsServer::listen(const std::string& address, int port) {
  if (listenFd_ >= 0) throw std::runtime_error("WsServer is already listening");

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  struct addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  const int rc = ::getaddrinfo(address.empty() ? nullptr : address.c_str(), portStr.c_str(), &hints,
                               &res);
  if (rc != 0) throw std::runtime_error("resolve " + address + ": " + ::gai_strerror(rc));

  int lastErrno = 0;
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      lastErrno = errno;
      continue;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd, 8) != 0) {
      lastErrno = errno;
      ::close(fd);
      continue;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // The port the kernel actually chose, which is the one a caller that asked for 0 has to tell
    // its clients about.
    struct sockaddr_storage bound {};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &boundLen) == 0) {
      if (bound.ss_family == AF_INET)
        port_ = ntohs(reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
      else if (bound.ss_family == AF_INET6)
        port_ = ntohs(reinterpret_cast<struct sockaddr_in6*>(&bound)->sin6_port);
    }
    listenFd_ = fd;
    ::freeaddrinfo(res);
    return;
  }
  ::freeaddrinfo(res);
  throw std::runtime_error("listen on " + address + ":" + portStr + ": " +
                           std::strerror(lastErrno ? lastErrno : EADDRINUSE));
}

void WsServer::broadcast(std::string_view payload) {
  std::lock_guard<std::mutex> lk(mu_);
  bool any = false;
  for (Client& c : clients_) {
    if (c.fd < 0 || !c.upgraded || c.closing) continue;
    any = true;
    if (c.out.size() >= kWsBacklog) {
      // Drop the new message, not the oldest. The oldest may be half-written, and half a frame
      // can never be repaired: the rest of that message is gone while every byte after it is
      // read as a frame header, so one dropped message desynchronises the client -- which then
      // reports slices of old messages, or the same text again, until the connection dies.
      // Dropping the newest keeps the byte stream a valid prefix and loses whole messages only.
      ++dropped_;
      continue;
    }
    c.out.push_back(encodeFrame(kOpText, payload));
  }
  if (any) ++messages_;
}

void WsServer::run() {
  if (listenFd_ < 0) throw std::runtime_error("WsServer::run() was called before listen()");
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) throw std::runtime_error("WsServer::run() was called twice");
    running_ = true;
  }

  while (!stopping_.load()) {
    // The socket set is snapshotted under the lock and polled without it: a client appearing or
    // being dropped while poll is asleep is handled on the next pass, and the 100 ms ceiling is
    // what bounds the wait to notice it (and to notice stop()).
    std::vector<struct pollfd> fds;
    {
      std::lock_guard<std::mutex> lk(mu_);
      fds.reserve(clients_.size() + 1);
      fds.push_back({listenFd_, POLLIN, 0});
      for (const Client& c : clients_) {
        if (c.fd < 0) continue;
        short events = POLLIN;
        if (!c.out.empty()) events |= POLLOUT;
        fds.push_back({c.fd, events, 0});
      }
    }

    if (::poll(fds.data(), fds.size(), 100) < 0) {
      if (errno == EINTR) continue;
      if (verbose_) std::fprintf(stderr, "[ws] poll: %s\n", std::strerror(errno));
      break;
    }

    const int64_t now = steadyNowMs();
    std::lock_guard<std::mutex> lk(mu_);
    if (fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) acceptAll(now);

    // Looked up by fd rather than by the index it had when the snapshot was taken: acceptAll() may
    // have appended clients, and a vector that reallocates moves everything the old indices named.
    for (size_t i = 1; i < fds.size(); ++i) {
      if (!fds[i].revents) continue;
      for (Client& c : clients_) {
        if (c.fd != fds[i].fd) continue;
        serviceClient(c, fds[i].revents);
        break;
      }
    }

    for (Client& c : clients_)
      if (c.fd >= 0 && !c.upgraded && now > c.deadlineMs) dropClient(c, "handshake timed out");
    std::erase_if(clients_, [](const Client& c) { return c.fd < 0; });
  }

  // A close frame, then whatever the sockets will take without blocking. A client that is gone
  // already, or whose buffer is full, does not hold up the exit: the process is going away and the
  // feed with it.
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (Client& c : clients_)
      if (c.fd >= 0 && c.upgraded && !c.closing) closeClient(c, 1001);  // 1001 = going away
    for (Client& c : clients_) flushClient(c);
    for (Client& c : clients_) {
      if (c.fd < 0) continue;
      ::close(c.fd);
      c.fd = -1;
    }
    clients_.clear();
  }
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
  running_ = false;
}

void WsServer::stop() { stopping_.store(true); }

size_t WsServer::clients() const {
  std::lock_guard<std::mutex> lk(mu_);
  size_t n = 0;
  for (const Client& c : clients_)
    if (c.fd >= 0 && c.upgraded) ++n;
  return n;
}

uint64_t WsServer::messages() const {
  std::lock_guard<std::mutex> lk(mu_);
  return messages_;
}

uint64_t WsServer::dropped() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dropped_;
}

void WsServer::acceptAll(int64_t now) {
  for (;;) {
    struct sockaddr_storage peer {};
    socklen_t peerLen = sizeof(peer);
    const int fd = ::accept4(listenFd_, reinterpret_cast<struct sockaddr*>(&peer), &peerLen,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      // EAGAIN is the ordinary "that was the last one"; anything else is worth saying out loud
      // because it can also mean the listening socket itself has gone bad.
      if (errno != EAGAIN && errno != EWOULDBLOCK && verbose_)
        std::fprintf(stderr, "[ws] accept: %s\n", std::strerror(errno));
      return;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (verbose_) {
      char host[NI_MAXHOST] = "?", serv[NI_MAXSERV] = "?";
      if (::getnameinfo(reinterpret_cast<struct sockaddr*>(&peer), peerLen, host, sizeof(host), serv,
                        sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
        std::fprintf(stderr, "[ws] client connected from %s:%s\n", host, serv);
      else
        std::fprintf(stderr, "[ws] client connected\n");
    }
    Client c;
    c.fd = fd;
    c.deadlineMs = now + kHandshakeMs;
    clients_.push_back(std::move(c));
  }
}

void WsServer::serviceClient(Client& c, short revents) {
  if (c.fd < 0) return;
  if (revents & (POLLERR | POLLNVAL)) {
    dropClient(c, "socket error");
    return;
  }

  if (revents & POLLIN) {
    char buf[4096];
    for (;;) {
      const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
      if (n > 0) {
        c.in.append(buf, static_cast<size_t>(n));
        if (c.in.size() > kMaxClientBytes) {
          dropClient(c, "client sent more than the inbound cap allows");
          return;
        }
        continue;
      }
      if (n == 0) {
        dropClient(c, "peer closed");
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      dropClient(c, "recv failed");
      return;
    }
    if (!c.upgraded) handshake(c);
    if (c.fd >= 0 && c.upgraded && !c.in.empty()) readFrames(c);
  }

  if (c.fd >= 0 && !c.out.empty()) flushClient(c);
  if (c.fd >= 0 && (revents & POLLHUP)) dropClient(c, "peer hung up");
}

void WsServer::handshake(Client& c) {
  const size_t end = c.in.find("\r\n\r\n");
  if (end == std::string::npos) return;  // more to come, or the deadline will end it
  const std::string head = c.in.substr(0, end);
  c.in.erase(0, end + 4);

  const size_t eol = head.find("\r\n");
  const std::string requestLine = head.substr(0, eol);
  std::string key;
  bool version13 = false;
  size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    if (next == std::string::npos) next = head.size();
    const std::string line = head.substr(pos, next - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      for (char& ch : name) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      if (name == "sec-websocket-key") key = value;
      else if (name == "sec-websocket-version") version13 = (value == "13");
    }
    pos = next + 2;
  }

  // Every refusal answers before it closes: a client that is told "400" can say why it failed,
  // while a socket that just disappears reads as a network fault.
  if (requestLine.rfind("GET ", 0) != 0 || key.empty()) {
    c.out.push_back("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    c.closing = true;
    return;
  }
  if (!version13) {
    c.out.push_back(
        "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    c.closing = true;
    return;
  }

  c.out.push_back("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: " +
                  wsAcceptKey(key) + "\r\n\r\n");
  c.upgraded = true;
  if (verbose_) std::fprintf(stderr, "[ws] handshake done, %zu client(s)\n", clients_.size());
}

void WsServer::readFrames(Client& c) {
  for (;;) {
    size_t consumed = 0;
    uint8_t opcode = 0;
    bool fin = false;
    std::string payload, err;
    if (!decodeFrame(c.in, consumed, opcode, fin, payload, err)) {
      if (!err.empty()) {
        if (verbose_) std::fprintf(stderr, "[ws] protocol error, closing: %s\n", err.c_str());
        closeClient(c, 1002);  // 1002 = protocol error
      }
      return;
    }
    c.in.erase(0, consumed);

    switch (opcode) {
      case kOpPing:
        c.out.push_back(encodeFrame(kOpPong, payload));
        break;
      case kOpClose:
        // The peer's code is echoed back, which is what RFC 6455 section 5.5.1 asks for.
        if (payload.size() >= 2)
          c.out.push_back(encodeFrame(kOpClose, std::string_view(payload).substr(0, 2)));
        else
          closeClient(c, 1000);
        c.closing = true;
        return;
      case kOpPong:
      case kOpText:
      case kOpBinary:
      case kOpContinuation:
        // The feed is one-way. A client's data frames are read so the socket stays drained and are
        // otherwise ignored -- there is no command for a consumer to send.
        break;
      default:
        if (verbose_) std::fprintf(stderr, "[ws] unknown opcode 0x%X\n", opcode);
        closeClient(c, 1002);
        return;
    }
  }
}

bool WsServer::flushClient(Client& c) {
  while (c.fd >= 0 && !c.out.empty()) {
    std::string& front = c.out.front();
    const ssize_t n =
        ::send(c.fd, front.data() + c.outSent, front.size() - c.outSent, MSG_NOSIGNAL);
    if (n > 0) {
      c.outSent += static_cast<size_t>(n);
      if (c.outSent == front.size()) {
        c.out.pop_front();
        c.outSent = 0;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // wait for POLLOUT
    dropClient(c, "send failed");
    return false;
  }
  if (c.fd >= 0 && c.closing && c.out.empty()) dropClient(c, "closed");
  return c.fd >= 0;
}

void WsServer::closeClient(Client& c, int code) {
  const unsigned char payload[2] = {static_cast<unsigned char>((code >> 8) & 0xFF),
                                    static_cast<unsigned char>(code & 0xFF)};
  c.out.push_back(
      encodeFrame(kOpClose, std::string_view(reinterpret_cast<const char*>(payload), sizeof(payload))));
  c.closing = true;
}

void WsServer::dropClient(Client& c, const char* why) {
  if (c.fd < 0) return;
  if (verbose_) std::fprintf(stderr, "[ws] client dropped: %s\n", why);
  ::close(c.fd);
  c.fd = -1;
  c.in.clear();
  c.out.clear();
  c.outSent = 0;
}

std::string WsServer::encodeFrame(uint8_t opcode, std::string_view payload) {
  std::string frame;
  frame.reserve(payload.size() + 10);
  frame.push_back(static_cast<char>(0x80 | opcode));  // FIN, no extensions
  const size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFF) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
  }
  // Unmasked: RFC 6455 section 5.1 -- a server must not mask, and a client must fail the
  // connection if it does.
  frame.append(payload.data(), payload.size());
  return frame;
}

bool WsServer::decodeFrame(const std::string& in, size_t& consumed, uint8_t& opcode, bool& fin,
                           std::string& payload, std::string& err) {
  err.clear();
  if (in.size() < 2) return false;

  const uint8_t b0 = static_cast<uint8_t>(in[0]);
  const uint8_t b1 = static_cast<uint8_t>(in[1]);
  opcode = static_cast<uint8_t>(b0 & 0x0F);
  fin = (b0 & 0x80) != 0;
  if (b0 & 0x70) {
    err = "RSV bits set with no extension negotiated";
    return false;
  }
  if ((b1 & 0x80) == 0) {
    err = "client frame is not masked";
    return false;
  }

  uint64_t len = b1 & 0x7F;
  size_t pos = 2;
  if (len == 126) {
    if (in.size() < pos + 2) return false;
    len = (static_cast<uint64_t>(static_cast<uint8_t>(in[pos])) << 8) |
          static_cast<uint8_t>(in[pos + 1]);
    pos += 2;
  } else if (len == 127) {
    if (in.size() < pos + 8) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(in[pos + i]);
    pos += 8;
  }
  if (len > kMaxClientBytes) {
    err = "client frame exceeds the inbound cap";
    return false;
  }
  // A control frame may not be fragmented and may not carry more than 125 bytes (section 5.5).
  if ((opcode & 0x08) && (!fin || len > 125)) {
    err = "invalid control frame";
    return false;
  }
  if (in.size() < pos + 4 + len) return false;  // header, mask key and payload all have to be here

  const unsigned char* mask = reinterpret_cast<const unsigned char*>(in.data()) + pos;
  pos += 4;
  payload.assign(in, pos, static_cast<size_t>(len));
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
  consumed = pos + static_cast<size_t>(len);
  return true;
}

}  // namespace dwm
