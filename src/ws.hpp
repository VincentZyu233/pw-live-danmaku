#pragma once
// Minimal RFC 6455 WebSocket client over TLS.
//
// Only what a live chat stream needs: a client handshake, masked text/binary sends, fragmented
// message reassembly, automatic pong replies, and a read with a deadline so the caller can drive
// its own heartbeat. No extensions (no permessage-deflate, no compression), no client-side ping
// scheduler, no HTTP/2.
//
// Why hand-rolled rather than libcurl's WebSocket transport:
//   * libcurl's WebSocket interface is documented as experimental, and this project depends only
//     on stable, distribution-shipped APIs.
//   * The submodule's HttpClient is a blocking GET and does not expose CONNECT_ONLY, so using
//     curl's WebSocket would mean extending the submodule for one site.
//   * The framing that remains is ~250 lines, and OpenSSL is already needed for the TLS layer.
//     SHA-1 and base64 for the handshake come from OpenSSL, so nothing new is introduced.
//
// Thread contract: not thread-safe. One client is owned by one thread.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dwm {

/** The one value both ends of an RFC 6455 handshake compute the same way: base64 of the SHA-1 of
the client's Sec-WebSocket-Key followed by the protocol GUID. The client checks the server's
answer with it and the local server writes its own; exporting it is what keeps the two sides from
each growing a private copy of the same twenty lines. */
std::string wsAcceptKey(const std::string& secWebSocketKey);

struct WsOptions {
  /** Host name, used for the TLS SNI extension and the certificate check as well as DNS. */
  std::string host;
  int port = 443;
  /** Request target, e.g. "/sub". */
  std::string path = "/";
  /** Extra request headers, e.g. Cookie / Origin / User-Agent. */
  std::vector<std::pair<std::string, std::string>> headers;
  /** Connect and handshake budget. */
  int timeoutMs = 10000;
  /** TLS certificate verification is on by default and should stay on. */
  bool insecureSkipVerify = false;
};

class WsClient {
 public:
  WsClient() = default;
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  /** Connects, performs the TLS handshake and the HTTP upgrade. Throws std::runtime_error with a
   *  description on any failure, including a non-101 response (whose status line is in the
   *  message, since "the server said no" and "the network is down" need different fixes). */
  void connect(const WsOptions& opt);

  /** Sends one unfragmented masked binary message. False on transport failure, with lastError()
   *  describing it. */
  bool sendBinary(std::string_view payload);
  /** Sends one unfragmented masked text message. */
  bool sendText(std::string_view payload);

  enum class Event {
    /** A complete text message arrived; payload is in out. */
    Text,
    /** A complete binary message arrived; payload is in out. */
    Binary,
    /** The peer sent a close frame. out holds the two-byte status code followed by the reason
     *  text when the peer carried one, and is empty only when the frame had no payload at all. */
    Close,
    /** Nothing arrived within the timeout. Not an error: the caller uses this to run timers. */
    Timeout,
    /** Transport failure or a protocol violation. lastError() describes it. */
    Error,
  };

  /** Reads the next event, waiting at most timeoutMs. Frames belonging to one message are
   *  reassembled into a single out payload. A ping is answered with a pong and does not surface;
   *  a pong does not surface either. */
  Event next(std::string& out, int timeoutMs);

  /** Sends a close frame and tears down the socket. Idempotent. */
  void close(int code = 1000);

  bool connected() const { return ssl_ != nullptr; }
  const std::string& lastError() const { return error_; }

 private:
  // Low-level transport.
  void tcpConnect(const std::string& host, int port, int timeoutMs);
  void tlsHandshake(int timeoutMs);
  /** Reads up to n bytes into buf. Returns >0 bytes read, 0 on clean EOF, -1 on timeout or error. */
  long readSome(char* buf, size_t n, int timeoutMs);
  bool writeAll(const char* buf, size_t n, int timeoutMs);
  /** Consumes up to n already-buffered bytes. Returns false only when the buffer is exhausted. */
  bool fill(size_t n, int timeoutMs);

  void httpUpgrade(const WsOptions& opt);
  bool sendFrame(uint8_t opcode, const char* data, size_t len);
  /** Reads exactly one frame header + payload. Opcode and payload go into out_header/out_payload. */
  bool readFrame(uint8_t& opcode, bool& fin, std::string& payload, int timeoutMs);
  void fail(std::string message);

  int fd_ = -1;
  void* ctx_ = nullptr;  // SSL_CTX*
  void* ssl_ = nullptr;  // SSL*
  WsOptions options_;
  std::string rbuf_;
  size_t rpos_ = 0;
  std::string error_;

  // Message reassembly state, carried across frames.
  std::string fragment_;
  uint8_t fragmentOpcode_ = 0;
};

}  // namespace dwm