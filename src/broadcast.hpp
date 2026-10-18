#pragma once
// The outbound feed: one message as it looks on the wire, and the loopback WebSocket server that
// carries it.
//
// Why a second WebSocket engine when ws.{hpp,cpp} already has one:
//   * ws.cpp is a client. It dials out over TLS and owns exactly one socket for its whole life.
//     This is the other side of the same protocol: it accepts sockets it did not ask for, serves
//     several at once, and is deliberately plaintext on loopback so a Resonite or browser client
//     can read it without a certificate.
//   * The two share one thing -- the handshake's Accept hash, exported from ws.hpp as
//     wsAcceptKey() -- and share nothing else. The framing differs in the direction that matters
//     (a server must not mask, a client must), the lifecycle differs, and the error model differs.
//     Folding both into one class would cost more than the ~120 lines this file re-states.
//
// The format and the transport live in one module on purpose: the three tab-separated fields are
// the server's contract with its clients, and a change to one without the other is a feed that
// still connects and quietly says the wrong thing.
//
// Thread contract: broadcast() is called by the site thread as messages arrive; run() owns its own
// thread. The queue between them is the only shared state, and it is behind one mutex.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "message.hpp"

namespace dwm {

/** The port --ws uses when no explicit one is given. Picked to be memorable rather than
 *  privileged: anything below 1024 would need help from the system to bind. */
constexpr int kDefaultWsPort = 8765;

/** The bytes broadcast for one message: three tab-separated fields -- avatar URL, name, body --
 *  and nothing else, so the framing is visible in a terminal and a client's split is trivial.
 *
 *  One message per WebSocket text frame, so a message boundary never has to be recovered from the
 *  payload. A message is not newline-terminated for the same reason: the frame already says where
 *  it ends, and a trailing newline only moves into whatever text component consumes it.
 *
 *  Control characters in every field become spaces, tabs above all. A tab inside the name or the
 *  body would split one message into four fields, and a client that splits on tabs would then read
 *  the rest of the body as an avatar URL. A chat message really can contain one: JSON carries a
 *  literal tab without complaint, so nothing upstream has ruled it out.
 *
 *  With rtf, the name and body fields are wrapped in Resonite rich-text tags (the wiki's
 *  Text_formatting tag list): the name keeps the colour the panel gives it, a gift's verb and name
 *  keep theirs, the ambient kinds are dimmed with <alpha>, and any '<' the viewer typed is defused
 *  through <noparse=1> so chat text cannot inject formatting -- or malformed tags -- into the
 *  display. The avatar field stays raw: it is data a client parses, not text a person reads, and a
 *  colour tag around a URL would corrupt it. */
std::string broadcastLine(const Message& m, bool rtf);

/** How many messages one client may fall behind before new ones are dropped for it. A reader that
 *  has stopped reading must not be able to make the overlay's memory grow without limit; the same
 *  reasoning as Shared::kPendingCap, one hop further out.
 *
 *  What is dropped is the newest, not the oldest, and that is not an arbitrary choice: the oldest
 *  queued frame may already be half on the socket, and half a frame cannot be repaired. Dropping it
 *  would leave the client parsing the middle of a message as a frame header, and every frame after
 *  it would be garbage. Dropping the newest keeps the byte stream a valid prefix and only ever
 *  loses whole messages. */
constexpr size_t kWsBacklog = 256;

/** A minimal RFC 6455 server, plaintext and meant for loopback.
 *
 *  No TLS, no authentication, no subprotocols, no compression, and no replay: a client that
 *  connects mid-stream sees the next message and nothing older. It is a local pipe for a local
 *  consumer, so every one of those would be a liability rather than a feature -- but it also means
 *  it must not be exposed on anything but a loopback address, and listen() is documented to say so
 *  rather than to enforce it, because refusing a non-loopback bind would be guessing at intent. */
class WsServer {
 public:
  WsServer() = default;
  ~WsServer();
  WsServer(const WsServer&) = delete;
  WsServer& operator=(const WsServer&) = delete;

  /** Binds and listens on `address` (a numeric host or name). Port 0 takes an ephemeral one, which
   *  port() then reports. Throws std::runtime_error with the errno text on failure; the caller is
   *  expected to fail startup rather than run without the feed it was asked for. */
  void listen(const std::string& address, int port);
  /** The port actually bound. Meaningful only after listen(). */
  int port() const { return port_; }

  /** Logs connects, drops and handshake failures to stderr. Off by default. */
  void setVerbose(bool verbose) { verbose_ = verbose; }

  /** Queues one text message for every connected client. Safe to call from any thread; the site
   *  thread is the only caller in practice. Does not block on a slow reader. */
  void broadcast(std::string_view payload);

  /** Serves until stop() is called. Blocks, so it gets a thread of its own; run() twice is a
   *  programming error and throws. */
  void run();
  /** Asks run() to return. It does so within one poll interval; a close frame is sent to each
   *  client on the way out. Safe to call from any thread, and safe to call twice. */
  void stop();

  /** Connected clients that finished the handshake. */
  size_t clients() const;
  /** Messages handed to at least one client. */
  uint64_t messages() const;
  /** Messages a client never got because its backlog was full. */
  uint64_t dropped() const;

 private:
  struct Client {
    int fd = -1;
    /** False until the upgrade request has been read and the 101 answered: until then the client
     *  is a half-open socket that may still turn out to be someone's curl --v. */
    bool upgraded = false;
    /** When an un-upgraded client is given up on. The entire handshake is a few hundred bytes on
     *  loopback, so five seconds is generous by three orders of magnitude and still bounds a
     *  connection that says nothing. */
    int64_t deadlineMs = 0;
    std::string in;               // bytes read and not yet consumed
    std::deque<std::string> out;  // encoded bytes, in order
    size_t outSent = 0;           // how much of out.front() has already reached the kernel
    bool closing = false;         // a close frame was queued; drop once it is out
  };

  // All of these run with mu_ held. Clients are handled one at a time, non-blocking, so the lock
  // is held for microseconds and a broadcast never waits on a syscall.
  void acceptAll(int64_t now);
  void serviceClient(Client& c, short revents);
  void readFrames(Client& c);
  void handshake(Client& c);
  /** Writes as much of a client's queue as the socket will take without blocking. */
  bool flushClient(Client& c);
  /** Queues a close frame and marks the client to be dropped once it is written out. */
  void closeClient(Client& c, int code);
  void dropClient(Client& c, const char* why);

  static std::string encodeFrame(uint8_t opcode, std::string_view payload);
  /** Pulls one frame off the front of `in` without consuming it. False means "not yet complete"
   *  when `err` is empty and "protocol violation" when it is not. `consumed` is valid only on
   *  success. */
  static bool decodeFrame(const std::string& in, size_t& consumed, uint8_t& opcode, bool& fin,
                          std::string& payload, std::string& err);

  int listenFd_ = -1;
  int port_ = 0;
  bool verbose_ = false;
  bool running_ = false;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mu_;
  std::vector<Client> clients_;
  uint64_t messages_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace dwm
