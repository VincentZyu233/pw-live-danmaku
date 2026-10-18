// Unit tests for the outbound feed: the three-field line format with and without Resonite tags,
// and the loopback server that carries it -- handshake, frame encoding, ping/pong, and the two
// refusals a client can earn.
//
// The server side is tested over a real socket rather than against its own helpers. Frame encoding
// is exactly the kind of code where a test that reuses the encoder's idea of a frame agrees with
// the encoder and with nothing else; this the client side below re-derives from RFC 6455 instead.
// The handshake is checked against the example nonce and accept value in section 1.3 of the RFC,
// which is a fact about the RFC and not about this program.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "broadcast.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const char* what) {
  if (!(got == want)) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

using dwm::Fragment;
using dwm::Message;
using dwm::MsgKind;
using dwm::UserType;

size_t countOf(const std::string& s, char c) {
  size_t n = 0;
  for (const char x : s)
    if (x == c) ++n;
  return n;
}

/** Splits a line into its three fields. The format is the whole contract, so the tests read it the
 *  way a client does -- by the tabs -- rather than by rebuilding it from the same pieces. */
bool fields3(const std::string& line, std::string& a, std::string& b, std::string& c) {
  const size_t t1 = line.find('\t');
  if (t1 == std::string::npos) return false;
  const size_t t2 = line.find('\t', t1 + 1);
  if (t2 == std::string::npos) return false;
  a = line.substr(0, t1);
  b = line.substr(t1 + 1, t2 - t1 - 1);
  c = line.substr(t2 + 1);
  return true;
}

Message chat() {
  Message m;
  m.user = "哈基米";
  m.type = UserType::Member;
  m.avatarUrl = "https://example.invalid/face.png";
  m.parts.push_back(Fragment{Fragment::Kind::Text, "你", "", 0});
  m.parts.push_back(Fragment{Fragment::Kind::Emote, "[热]", "https://example.invalid/e.png", 28});
  m.parts.push_back(Fragment{Fragment::Kind::Text, "好", "", 0});
  return m;
}

void testPlainLine() {
  const Message m = chat();
  std::string avatar, name, body;
  const std::string line = dwm::broadcastLine(m, false);
  check(fields3(line, avatar, name, body), "plain: three tab-separated fields");
  checkEq(avatar, std::string("https://example.invalid/face.png"), "plain: avatar");
  checkEq(name, std::string("哈基米"), "plain: name");
  // An emote travels as the token the sender typed, the same text the panel falls back to.
  checkEq(body, std::string("你[热]好"), "plain: emote token kept");

  // A tab or a newline in the body must not survive into a field, or a client that splits on tabs
  // reads the rest of the message as another field.
  Message t = chat();
  t.parts.clear();
  t.parts.push_back(Fragment{Fragment::Kind::Text, "a\tb\nc\x7F", "", 0});
  const std::string tampered = dwm::broadcastLine(t, false);
  checkEq(countOf(tampered, '\t'), size_t(2), "plain: control characters cannot add fields");
  checkEq(tampered, std::string("https://example.invalid/face.png\t哈基米\ta b c "),
          "plain: control characters become spaces");
}

void testRtfLine() {
  std::string avatar, name, body;

  const Message m = chat();
  check(fields3(dwm::broadcastLine(m, true), avatar, name, body), "rtf: three tab-separated fields");
  checkEq(avatar, std::string("https://example.invalid/face.png"),
          "rtf: the avatar stays a raw URL");
  checkEq(name, std::string("<color=#0F9D58>哈基米</color>"), "rtf: member name in role colour");
  checkEq(body, std::string("你[热]好"), "rtf: plain chat carries no tags of its own");

  // The colour the sender chose wins over the role colour, the same order the panel uses.
  Message coloured = chat();
  coloured.userColor = 0x123456;
  check(fields3(dwm::broadcastLine(coloured, true), avatar, name, body), "rtf: coloured fields");
  checkEq(name, std::string("<color=#123456>哈基米</color>"), "rtf: the sender's colour wins");

  // Chat text is data. A tag the viewer typed is shown as glyphs on the panel and must be shown as
  // glyphs here too -- which takes the wiki's counted noparse, because the closed form cannot wrap
  // text that might contain its own closing tag.
  Message evil;
  evil.user = "x";
  evil.parts.push_back(Fragment{Fragment::Kind::Text, "<b>bold</b>", "", 0});
  check(fields3(dwm::broadcastLine(evil, true), avatar, name, body), "rtf: injected-tag fields");
  checkEq(body, std::string("<noparse=1><b>bold<noparse=1></b>"), "rtf: a typed tag is disarmed");

  // The gift verb and the gift's name keep the panel's accents; everything after them does not.
  Message gift;
  gift.kind = MsgKind::Gift;
  gift.user = "氧***";
  gift.parts.push_back(Fragment{Fragment::Kind::Text, "投喂 ", "", 0, true});
  gift.parts.push_back(Fragment{Fragment::Kind::Emote, "人气票", "", 0});
  gift.parts.push_back(Fragment{Fragment::Kind::Text, " ×6", "", 0});
  check(fields3(dwm::broadcastLine(gift, true), avatar, name, body), "rtf: gift fields");
  checkEq(name, std::string("<color=#9AA0A6>氧***</color>"), "rtf: gift name keeps role colour");
  checkEq(body,
          std::string("<color=#F5C542>投喂 </color><color=#FB7299>人气票</color> ×6"),
          "rtf: gift verb and gift name accents");

  // The ambient kinds arrive an order of magnitude faster than chat; the panel dims them, and so
  // does the feed, on both fields so neither is left at full strength.
  Message entry;
  entry.kind = MsgKind::Entry;
  entry.user = "a";
  entry.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
  check(fields3(dwm::broadcastLine(entry, true), avatar, name, body), "rtf: entry fields");
  checkEq(name, std::string("<alpha=#80><color=#9AA0A6>a</color></alpha>"),
          "rtf: entry name dimmed");
  checkEq(body, std::string("<alpha=#80>进入了直播间</alpha>"), "rtf: entry body dimmed");
}

// --- the server ---------------------------------------------------------------------------------

int connectTo(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool sendAll(int fd, const std::string& s) {
  size_t sent = 0;
  while (sent < s.size()) {
    const ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

enum class Read { Data, Timeout, Eof };

/** Appends whatever is readable within the timeout. Timeout and EOF are told apart: a poll that
 *  times out is a reason to keep waiting, while EOF is a reason to stop, and folding them together
 *  makes a read with a long budget give up after the first short slice. */
Read readSome(int fd, std::string& buf, int timeoutMs) {
  struct pollfd pfd {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (::poll(&pfd, 1, timeoutMs) <= 0) return Read::Timeout;
  char tmp[4096];
  const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
  if (n <= 0) return Read::Eof;
  buf.append(tmp, static_cast<size_t>(n));
  return Read::Data;
}

/** Pops one complete server-to-client frame from buf. A server frame is never masked, so the
 *  layout is header, extended length, payload. False means "not enough bytes yet". */
bool takeFrame(std::string& buf, uint8_t& opcode, std::string& payload) {
  if (buf.size() < 2) return false;
  opcode = static_cast<uint8_t>(buf[0]) & 0x0F;
  if (static_cast<uint8_t>(buf[1]) & 0x80) {
    check(false, "a server frame must not be masked");
    return false;
  }
  uint64_t len = static_cast<uint8_t>(buf[1]) & 0x7F;
  size_t pos = 2;
  if (len == 126) {
    if (buf.size() < pos + 2) return false;
    len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[pos])) << 8) |
          static_cast<uint8_t>(buf[pos + 1]);
    pos += 2;
  } else if (len == 127) {
    if (buf.size() < pos + 8) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(buf[pos + i]);
    pos += 8;
  }
  if (buf.size() < pos + len) return false;
  payload = buf.substr(pos, static_cast<size_t>(len));
  buf.erase(0, pos + static_cast<size_t>(len));
  return true;
}

bool waitForFrame(int fd, std::string& buf, uint8_t want, std::string& payload, int timeoutMs) {
  for (;;) {
    uint8_t opcode = 0;
    if (takeFrame(buf, opcode, payload)) return opcode == want;
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

bool waitForStatus(int fd, std::string& buf, const char* status, int timeoutMs) {
  for (;;) {
    if (buf.find(status) != std::string::npos) return true;
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

/** Reads the handshake response and consumes it whole, so what is left in buf starts at a frame
 *  boundary. Stopping at the status line would leave the tail of the headers there, and its ASCII
 *  reads as a frame header -- which desynchronises every read after it. The response and the first
 *  broadcast frame can arrive in one segment, so this must not over-read either: it stops at the
 *  first \r\n\r\n and leaves anything after it alone. */
bool readHandshake(int fd, std::string& buf, int timeoutMs, std::string* headOut = nullptr) {
  for (;;) {
    const size_t end = buf.find("\r\n\r\n");
    if (end != std::string::npos) {
      const std::string head = buf.substr(0, end);
      if (headOut) *headOut = head;
      buf.erase(0, end + 4);
      return head.find("101 Switching Protocols") != std::string::npos;
    }
    if (timeoutMs <= 0) return false;
    const int slice = timeoutMs < 100 ? timeoutMs : 100;
    if (readSome(fd, buf, slice) == Read::Eof) return false;
    timeoutMs -= slice;
  }
}

/** A masked client frame with a fixed key: short payloads only, which is all a client of this feed
 *  has any reason to send. */
std::string clientFrame(uint8_t opcode, const std::string& payload) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | opcode));
  f.push_back(static_cast<char>(0x80 | payload.size()));
  const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
  f.append(reinterpret_cast<const char*>(mask), sizeof(mask));
  for (size_t i = 0; i < payload.size(); ++i)
    f.push_back(static_cast<char>(payload[i] ^ mask[i & 3]));
  return f;
}

constexpr const char* kUpgrade =
    "GET /sub HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"  // RFC 6455 section 1.3
    "Sec-WebSocket-Version: 13\r\n\r\n";

bool waitForClients(const dwm::WsServer& server, size_t want, int timeoutMs) {
  for (int waited = 0; waited < timeoutMs; waited += 20) {
    if (server.clients() == want) return true;
    struct timespec ts {0, 20 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  return false;
}

void testServer() {
  dwm::WsServer server;
  server.listen("127.0.0.1", 0);  // an ephemeral port, so the test cannot collide with a real run
  check(server.port() > 0, "server: an ephemeral port was reported");
  std::thread thread([&server] { server.run(); });

  // A normal client: upgrade, receive, ping, close.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "server: connected");
    check(sendAll(fd, kUpgrade), "server: upgrade request sent");
    std::string buf, head;
    check(readHandshake(fd, buf, 2000, &head), "server: answered 101");
    // The RFC's own nonce and the accept value it must produce.
    check(head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
          "server: RFC 6455 example accept value");
    check(buf.empty(), "server: the handshake left no bytes in the frame buffer");
    check(waitForClients(server, 1, 2000), "server: client counted after the handshake");

    server.broadcast("face.png\tname\tbody");
    std::string payload;
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: text frame arrived");
    checkEq(payload, std::string("face.png\tname\tbody"), "server: payload survived the framing");

    // Several messages in a row, one of them past the 125-byte boundary where the length moves
    // into the extended field: each must arrive exactly once, in order. A queue that is re-flushed
    // from the front instead of advanced would repeat the earlier frames here, and a duplicated
    // danmaku is the kind of thing a screenshot never shows.
    const std::string longBody(300, 'x');
    for (int i = 0; i < 4; ++i) server.broadcast("message " + std::to_string(i));
    server.broadcast(longBody);
    for (int i = 0; i < 4; ++i) {
      check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: repeated frame arrived");
      checkEq(payload, "message " + std::to_string(i), "server: repeated frame in order");
    }
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: extended-length frame arrived");
    checkEq(payload, longBody, "server: extended-length payload intact");
    check(!waitForFrame(fd, buf, 0x1, payload, 300), "server: nothing was sent twice");

    // A second client gets its own copy, and neither gets a second copy of it. The broadcast loop
    // is per client, so this is where a copy-into-the-wrong-queue would show up.
    const int fd2 = connectTo(server.port());
    check(fd2 >= 0, "server: second client connected");
    check(sendAll(fd2, kUpgrade), "server: second upgrade sent");
    std::string buf2, head2;
    check(readHandshake(fd2, buf2, 2000, &head2), "server: second client answered 101");
    check(waitForClients(server, 2, 2000), "server: two clients counted");
    server.broadcast("to both");
    check(waitForFrame(fd, buf, 0x1, payload, 2000), "server: first client got it");
    checkEq(payload, std::string("to both"), "server: first client's copy");
    check(waitForFrame(fd2, buf2, 0x1, payload, 2000), "server: second client got it");
    checkEq(payload, std::string("to both"), "server: second client's copy");
    check(!waitForFrame(fd, buf, 0x1, payload, 300), "server: first client got no extra copy");
    check(!waitForFrame(fd2, buf2, 0x1, payload, 300), "server: second client got no extra copy");
    ::close(fd2);
    check(waitForClients(server, 1, 2000), "server: back to one client");

    check(sendAll(fd, clientFrame(0x9, "keepalive")), "server: ping sent");
    check(waitForFrame(fd, buf, 0xA, payload, 2000), "server: ping answered with a pong");
    checkEq(payload, std::string("keepalive"), "server: pong carries the ping's payload");

    check(sendAll(fd, clientFrame(0x8, std::string("\x03\xe8", 2))), "server: close sent");
    check(waitForFrame(fd, buf, 0x8, payload, 2000), "server: close echoed");
    ::close(fd);
    check(waitForClients(server, 0, 2000), "server: client forgotten after the close");
  }

  // A client that violates the framing: an unmasked frame is a protocol error, and the answer is
  // 1002 rather than silence, so the client can tell which side broke the connection.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "refusal: connected");
    check(sendAll(fd, kUpgrade), "refusal: upgrade request sent");
    std::string buf;
    check(readHandshake(fd, buf, 2000), "refusal: answered 101");
    check(waitForClients(server, 1, 2000), "refusal: client counted");

    std::string unmasked;
    unmasked.push_back(static_cast<char>(0x81));  // FIN + text
    unmasked.push_back(static_cast<char>(0x01));  // unmasked, one byte
    unmasked.push_back('x');
    check(sendAll(fd, unmasked), "refusal: unmasked frame sent");
    std::string payload;
    check(waitForFrame(fd, buf, 0x8, payload, 2000), "refusal: closed with a close frame");
    checkEq(payload, std::string("\x03\xea", 2), "refusal: 1002 protocol error");
    ::close(fd);
    check(waitForClients(server, 0, 2000), "refusal: client forgotten");
  }

  // Something that is not a WebSocket at all: the answer is an HTTP error before the close, not a
  // socket that vanishes.
  {
    const int fd = connectTo(server.port());
    check(fd >= 0, "non-websocket: connected");
    check(sendAll(fd, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), "non-websocket: request sent");
    std::string buf;
    check(waitForStatus(fd, buf, "400 Bad Request", 2000), "non-websocket: answered 400");
    ::close(fd);
  }

  server.stop();
  thread.join();
}

/** A client that stops reading costs the feed a bounded amount of memory, and what it does get
 *  has to stay a stream of whole frames.
 *
 *  The backlog cap is the only place the server drops anything, and the natural move -- throw away
 *  the oldest queued frame, ring-buffer style -- is wrong: the oldest may already be half on the
 *  socket, and no later bytes can repair a half frame. This test stalls a real connection until the
 *  cap engages, then reads what was written and requires every frame to parse and every payload to
 *  be a message that was actually sent, in order. Dropping the front instead fails here, either as
 *  a payload nobody sent or as trailing bytes that are not a frame. */
void testStalledClient() {
  dwm::WsServer server;
  server.listen("127.0.0.1", 0);
  std::thread thread([&server] { server.run(); });

  const int fd = connectTo(server.port());
  check(fd >= 0, "stall: connected");
  const int small = 4096;  // a tiny window, so backpressure arrives quickly
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  check(sendAll(fd, kUpgrade), "stall: upgrade request sent");
  std::string buf, head;
  check(readHandshake(fd, buf, 2000, &head), "stall: answered 101");
  check(waitForClients(server, 1, 2000), "stall: client counted");

  // Big messages, more of them than any socket buffer will hold: tens of megabytes if they were
  // all queued for one stalled reader.
  const size_t kMessages = 2000;
  const std::string tail(8000, 'y');
  std::vector<std::string> sent;
  sent.reserve(kMessages);
  for (size_t i = 0; i < kMessages; ++i) {
    sent.push_back("msg-" + std::to_string(i) + "-" + tail);
    server.broadcast(sent.back());
  }
  check(server.dropped() > 0, "stall: the backlog cap engaged");

  // Read to the end of what the server wrote. Silence is the end: it has nothing left queued.
  std::string all = buf;
  while (readSome(fd, all, 300) == Read::Data) {
  }

  size_t frames = 0;
  bool ordered = true;
  for (;;) {
    uint8_t opcode = 0;
    std::string payload;
    if (!takeFrame(all, opcode, payload)) break;
    if (opcode != 0x1) continue;
    if (frames >= sent.size() || payload != sent[frames]) ordered = false;
    ++frames;
  }
  check(frames > 0, "stall: something was delivered");
  check(ordered, "stall: every frame is a message that was sent, in order");
  check(all.empty(), "stall: the stream ends on a frame boundary");
  ::close(fd);
  server.stop();
  thread.join();
}

}  // namespace

int main() {
  testPlainLine();
  testRtfLine();
  testServer();
  testStalledClient();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("broadcast: ok\n");
  return 0;
}
