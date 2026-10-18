// Unit test for the client half of ws.{hpp,cpp}: the message layer over a real TLS socket.
//
// ws.cpp is transport, so it cannot be pointed at a capture the way the JSON parsers can -- the only
// way to make it see the frames a case needs is to be the server. The server below is therefore
// scripted: it completes a real handshake and then writes exact frames, in the order the case is
// about, and its certificate is generated in this process (the client is told to skip verification,
// since the verification path is not what this suite exists for).
//
// The regression this file exists for is fragmented-message reassembly: a message split across a
// first frame and continuation frames used to come back empty, because the accumulated payload was
// cleared before it was swapped out. Nothing in the bilibili stream fragments, so only a scripted
// server can produce that case at all -- which is why the bug survived a live run.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "ws.hpp"

namespace {

std::atomic<int> failures{0};

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void checkEq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want.c_str());
    ++failures;
  }
}

/** A self-signed P-256 certificate made per run. With verification skipped, the only job it has is
 *  to let OpenSSL complete a handshake; generating it here keeps a private key out of the repo. */
SSL_CTX* makeServerCtx() {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
  X509* cert = X509_new();
  if (!ctx || !key || !cert) {
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
    if (ctx) SSL_CTX_free(ctx);
    return nullptr;
  }
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 24 * 60 * 60);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  const bool ok = X509_sign(cert, key, EVP_sha256()) > 0 &&
                  SSL_CTX_use_certificate(ctx, cert) == 1 &&
                  SSL_CTX_use_PrivateKey(ctx, key) == 1;
  X509_free(cert);
  EVP_PKEY_free(key);
  if (!ok) {
    SSL_CTX_free(ctx);
    return nullptr;
  }
  return ctx;
}

/** A read/write timeout on both directions, so a client that stops talking fails the test instead
 *  of hanging it. */
bool setTimeouts(int fd, int ms) {
  struct timeval tv {};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

/** Binds an ephemeral port on 127.0.0.1 and reports it. The accept timeout makes the server thread
 *  return even when the client never gets as far as connecting. */
int listenLoopback(int& port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 1) != 0) {
    ::close(fd);
    return -1;
  }
  struct sockaddr_in bound {};
  socklen_t len = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &len) != 0) {
    ::close(fd);
    return -1;
  }
  port = ntohs(bound.sin_port);
  setTimeouts(fd, 4000);
  return fd;
}

bool sslWriteAll(SSL* ssl, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int rc = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
    if (rc <= 0) return false;
    sent += static_cast<size_t>(rc);
  }
  return true;
}

bool sslReadExact(SSL* ssl, void* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    const int rc = SSL_read(ssl, static_cast<char*>(buf) + got, static_cast<int>(n - got));
    if (rc <= 0) return false;
    got += static_cast<size_t>(rc);
  }
  return true;
}

bool sslReadUntil(SSL* ssl, std::string& buf, std::string_view marker) {
  while (buf.find(marker) == std::string::npos) {
    char tmp[1024];
    const int rc = SSL_read(ssl, tmp, sizeof(tmp));
    if (rc <= 0) return false;
    buf.append(tmp, static_cast<size_t>(rc));
  }
  return true;
}

std::string headerValue(const std::string& head, std::string_view name) {
  size_t eol = head.find("\r\n");
  size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    if (next == std::string::npos) next = head.size();
    const std::string line = head.substr(pos, next - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string field = line.substr(0, colon);
      for (char& c : field) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (field == name) {
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        return value;
      }
    }
    pos = next + 2;
  }
  return {};
}

/** An unmasked server frame, which is the only kind a server may send. */
std::string serverFrame(uint8_t opcode, bool fin, std::string_view payload) {
  std::string f;
  f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
  const size_t len = payload.size();
  if (len < 126) {
    f.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFF) {
    f.push_back(static_cast<char>(126));
    f.push_back(static_cast<char>((len >> 8) & 0xFF));
    f.push_back(static_cast<char>(len & 0xFF));
  } else {
    f.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i)
      f.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
  }
  f.append(payload.data(), payload.size());
  return f;
}

/** Reads one frame from the client and unmasks it. The mask bit is reported so the caller can pin
 *  it: a client that stopped masking would be disconnected by every real server, and nothing else in
 *  the suite would notice. */
bool readClientFrame(SSL* ssl, uint8_t& opcode, bool& masked, std::string& payload) {
  unsigned char h[2];
  if (!sslReadExact(ssl, h, sizeof(h))) return false;
  opcode = static_cast<uint8_t>(h[0] & 0x0F);
  masked = (h[1] & 0x80) != 0;
  uint64_t len = h[1] & 0x7F;
  if (len == 126) {
    unsigned char e[2];
    if (!sslReadExact(ssl, e, sizeof(e))) return false;
    len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
  } else if (len == 127) {
    unsigned char e[8];
    if (!sslReadExact(ssl, e, sizeof(e))) return false;
    len = 0;
    for (unsigned char b : e) len = (len << 8) | b;
  }
  unsigned char mask[4] = {0, 0, 0, 0};
  if (masked && !sslReadExact(ssl, mask, sizeof(mask))) return false;
  payload.assign(static_cast<size_t>(len), '\0');
  if (len && !sslReadExact(ssl, payload.data(), static_cast<size_t>(len))) return false;
  if (masked)
    for (size_t i = 0; i < payload.size(); ++i)
      payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);
  return true;
}

int closeCode(const std::string& payload) {
  if (payload.size() < 2) return -1;
  return (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
}

/** One connection, end to end. The frames are written in the order the client has to see them,
 *  because that order is the case: a text frame that does not finish, a ping between its fragments
 *  (a control frame is allowed to interleave with them), the continuations that finish it, then an
 *  unfragmented binary frame, and a close carrying a code and a reason. */
void serve(SSL_CTX* ctx, int listenFd) {
  const int fd = ::accept(listenFd, nullptr, nullptr);
  if (fd < 0) return;  // the accept timeout fired; the client never connected, and main says so
  setTimeouts(fd, 4000);
  SSL* ssl = SSL_new(ctx);
  SSL_set_fd(ssl, fd);
  bool ok = SSL_accept(ssl) == 1;

  std::string req;
  ok = ok && sslReadUntil(ssl, req, "\r\n\r\n");
  ok = ok && sslWriteAll(ssl,
                         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                             dwm::wsAcceptKey(headerValue(req, "sec-websocket-key")) + "\r\n\r\n");
  ok = ok && sslWriteAll(ssl, serverFrame(0x1, false, "AB"));
  ok = ok && sslWriteAll(ssl, serverFrame(0x9, true, "PING"));
  ok = ok && sslWriteAll(ssl, serverFrame(0x0, false, "CD"));
  ok = ok && sslWriteAll(ssl, serverFrame(0x0, true, "EF"));
  ok = ok && sslWriteAll(ssl, serverFrame(0x2, true, "BIN"));
  ok = ok && sslWriteAll(ssl, serverFrame(0x8, true, std::string("\x03\xe8" "bye")));
  check(ok, "server: the script was written");

  // The ping was answered while the client was still assembling the message, so this reply is
  // waiting here before the close echo that comes after the client reads the close.
  uint8_t opcode = 0;
  bool masked = false;
  std::string payload;
  const bool gotPong = readClientFrame(ssl, opcode, masked, payload);
  check(gotPong, "client: answered the ping");
  if (gotPong) {
    check(opcode == 0xA, "client: the reply to a ping is a pong");
    check(masked, "client: pongs are masked, as every client frame must be");
    checkEq(payload, "PING", "client: the pong carries the ping's payload");
  }

  const bool gotClose = readClientFrame(ssl, opcode, masked, payload);
  check(gotClose, "client: echoed the close");
  if (gotClose) {
    check(opcode == 0x8, "client: the reply to a close is a close");
    check(masked, "client: close echoes are masked too");
    check(closeCode(payload) == 1000, "client: the echoed close carries the peer's status code");
  }

  // Quiet: a waiting close_notify would only cost a timeout, and the client has already been told
  // everything this script had to say.
  SSL_set_quiet_shutdown(ssl, 1);
  SSL_free(ssl);
  ::close(fd);
}

}  // namespace

int main() {
  SSL_CTX* ctx = makeServerCtx();
  check(ctx != nullptr, "server: a TLS context could be made");
  int port = 0;
  const int listenFd = listenLoopback(port);
  check(listenFd >= 0, "server: listening on loopback");
  if (!ctx || listenFd < 0) {
    if (ctx) SSL_CTX_free(ctx);
    if (listenFd >= 0) ::close(listenFd);
    return 1;
  }

  std::thread server([&] { serve(ctx, listenFd); });

  dwm::WsOptions opt;
  opt.host = "127.0.0.1";
  opt.port = port;
  opt.path = "/sub";
  opt.timeoutMs = 4000;
  // The certificate was generated in this process, so there is nothing to trust it against. The
  // verification path is not what this suite is about.
  opt.insecureSkipVerify = true;

  dwm::WsClient ws;
  bool connected = true;
  try {
    ws.connect(opt);
  } catch (const std::exception& e) {
    connected = false;
    std::fprintf(stderr, "FAIL: client connect: %s\n", e.what());
    ++failures;
  }

  if (connected) {
    std::string msg;
    check(ws.next(msg, 5000) == dwm::WsClient::Event::Text,
          "client: a fragmented message surfaces as one text event");
    checkEq(msg, "ABCDEF", "client: the fragments were reassembled in order");
    check(ws.next(msg, 5000) == dwm::WsClient::Event::Binary, "client: binary event");
    checkEq(msg, "BIN", "client: binary payload arrives whole");
    check(ws.next(msg, 5000) == dwm::WsClient::Event::Close, "client: close event");
    checkEq(msg, std::string("\x03\xe8" "bye", 5), "client: close carries the code and the reason");
  }
  ws.close();

  // The listener has an accept timeout, so serve() returns even when the client never connected.
  server.join();
  ::close(listenFd);
  SSL_CTX_free(ctx);

  if (failures.load()) {
    std::fprintf(stderr, "%d failure(s)\n", failures.load());
    return 1;
  }
  std::printf("ws: all checks passed\n");
  return 0;
}
