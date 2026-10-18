// Bilibili live chat: HTTP bootstrap and websocket packet protocol. See bili.hpp for what is
// established here and what it is not.
//
// The HTTP side runs on the submodule's HttpClient (a blocking libcurl GET with automatic content
// decoding), so brotli/gzip encoded API replies arrive already decoded and no extra dependency is
// needed for the request path. The only compression this file handles itself is the brotli inside
// the websocket command packets, which is libbrotlidec.
#include "bili.hpp"

#include <brotli/decode.h>
#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <vector>

#include "http.hpp"  // submodule: pwvideo::HttpClient
#include "pb.hpp"    // the schema-less protobuf reader behind data.pb

namespace dwm {
namespace {

// A desktop Firefox user agent. The API checks it, and so do the servers behind the websocket
// endpoint; a self-identified client gets a different answer than a browser does.
constexpr const char* kDefaultUA =
    "Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0";

constexpr const char* kApiBase = "https://api.live.bilibili.com";
constexpr const char* kMainApiBase = "https://api.bilibili.com";

// The fixed 64-entry permutation that turns img_key+sub_key into the WBI mixin key. It is shipped
// with the web client and has not changed; the keys it permutes rotate daily.
constexpr int kMixinKeyEncTab[64] = {46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
                                     27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                                     37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
                                     22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};

/** Appends a 16-byte header in bilibili's byte order. totalSize covers header plus body. */
void putHeader(std::string& out, PacketHeader h, const std::string& body) {
  h.headerSize = 16;
  h.totalSize = 16u + static_cast<uint32_t>(body.size());
  auto be16 = [&out](uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
  };
  auto be32 = [&out](uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
  };
  be32(h.totalSize);
  be16(h.headerSize);
  be16(h.proto);
  be32(h.type);
  be32(h.sequence);
  out.append(body);
}

uint16_t readBe16(const char* p) {
  return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}

uint32_t readBe32(const char* p) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

/** Streaming brotli decode with a growing output buffer. The one-shot API needs an output size
 *  the format does not carry, so the stream interface is the only correct choice here. */
bool brotliDecompress(std::string_view in, std::string& out, size_t maxOut) {
  BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  if (!state) return false;

  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(in.data());
  size_t availIn = in.size();
  bool ok = false;
  std::vector<char> chunk(64 * 1024);

  for (;;) {
    uint8_t* nextOut = reinterpret_cast<uint8_t*>(chunk.data());
    size_t availOut = chunk.size();
    const BrotliDecoderResult r =
        BrotliDecoderDecompressStream(state, &availIn, &nextIn, &availOut, &nextOut, nullptr);
    out.append(chunk.data(), chunk.size() - availOut);
    if (out.size() > maxOut) break;

    if (r == BROTLI_DECODER_RESULT_SUCCESS) {
      ok = true;
      break;
    }
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;
    // NEEDS_MORE_INPUT with nothing left means the payload was truncated, which is a protocol
    // error rather than something to retry.
    break;
  }
  BrotliDecoderDestroyInstance(state);
  return ok;
}

bool zlibDecompress(std::string_view in, std::string& out, size_t maxOut) {
  z_stream zs {};
  // 16 + MAX_WBITS: gzip-wrapped, which is what the server sends for protover 2. Using the
  // default 15 would silently fail on a gzip header.
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  std::vector<char> chunk(64 * 1024);
  int rc = Z_OK;
  for (;;) {
    zs.next_out = reinterpret_cast<Bytef*>(chunk.data());
    zs.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    out.append(chunk.data(), chunk.size() - zs.avail_out);
    if (out.size() > maxOut) break;
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) break;
    if (zs.avail_in == 0 && zs.avail_out != 0) break;  // truncated
  }
  inflateEnd(&zs);
  return rc == Z_STREAM_END;
}

/** First run of digits of at least one character in s. Used to pull a room number out of a URL. */
bool firstDigits(const std::string& s, int64_t& value) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') continue;
    int64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      if (v > 2000000000LL) return false;  // not a room id; refuse rather than overflow
      ++i;
    }
    value = v;
    return true;
  }
  return false;
}

}  // namespace

Bili::Bili(BiliConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.userAgent.empty()) cfg_.userAgent = kDefaultUA;
}

std::string Bili::md5Hex(std::string_view data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_md5(), nullptr) != 1) return {};
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out.push_back(hex[digest[i] >> 4]);
    out.push_back(hex[digest[i] & 0xF]);
  }
  return out;
}

std::string Bili::percentEncode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string Bili::mixinKey(const std::string& imgKey, const std::string& subKey) {
  const std::string raw = imgKey + subKey;
  if (raw.size() < 64) return {};
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 64; ++i) {
    const size_t at = static_cast<size_t>(kMixinKeyEncTab[i]);
    if (at >= raw.size()) return {};
    out.push_back(raw[at]);
  }
  return out.substr(0, 32);
}

const std::string& Bili::buvid() const {
  if (!buvid_.empty()) return buvid_;
  // Returns data.b_3. No login and no cookie required; it is the device id a browser would
  // generate on first contact. Cached for the run: it does not change and re-fetching would only
  // add a request the API does not need.
  std::string err;
  pwvideo::HttpResponse res;
  pwvideo::HttpRequest req;
  req.url = std::string(kMainApiBase) + "/x/frontend/finger/spi";
  pwvideo::HttpClient http(cfg_.userAgent);
  if (!http.get(req, res, &err)) throw std::runtime_error("fetching buvid3: " + err);
  // The parsed root is a named local, not a temporary: path() returns a reference into it, so
  // binding the result of path() straight onto a temporary would dangle.
  const Json root = Json::parse(res.body, &err);
  const std::string value = Json::path(root, {"data", "b_3"}).str();
  if (value.empty()) throw std::runtime_error("fetching buvid3: unexpected reply " + res.body);
  buvid_ = value;
  return buvid_;
}

const std::string& Bili::wbiMixinKey() const {
  if (!mixinKey_.empty()) return mixinKey_;

  std::string err;
  pwvideo::HttpResponse res;
  pwvideo::HttpRequest req;
  req.url = std::string(kMainApiBase) + "/x/web-interface/nav";
  pwvideo::HttpClient http(cfg_.userAgent);
  if (!http.get(req, res, &err)) throw std::runtime_error("fetching wbi keys: " + err);

  // An anonymous nav reply carries code -101 ("not logged in") but still includes wbi_img, which
  // is all that is needed. Refusing on a non-zero code here would break the anonymous path.
  const Json root = Json::parse(res.body, &err);
  const Json& img = Json::path(root, {"data", "wbi_img", "img_url"});
  const Json& sub = Json::path(root, {"data", "wbi_img", "sub_url"});
  if (img.str().empty() || sub.str().empty())
    throw std::runtime_error("fetching wbi keys: unexpected reply " + res.body);

  // The URL is a disguise for a token, not an image: its last path segment without the extension
  // is the key. Fetching the URL would be pointless.
  auto keyOf = [](const std::string& url) {
    size_t slash = url.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    std::string k = url.substr(slash + 1);
    size_t dot = k.find_last_of('.');
    if (dot != std::string::npos) k = k.substr(0, dot);
    return k;
  };
  mixinKey_ = mixinKey(keyOf(img.str()), keyOf(sub.str()));
  if (mixinKey_.empty()) throw std::runtime_error("wbi keys: could not derive the mixin key");
  return mixinKey_;
}

const std::string& Bili::cookieHeader() const {
  if (cookieBuilt_) return cookie_;
  std::string c;
  // Only supply our own device id when the user's cookie does not already carry one. Two buvid3
  // values in a single Cookie header is at best ambiguous, and fetching one is a network request we
  // can skip when it is already there.
  if (cfg_.cookie.find("buvid3=") == std::string::npos) {
    c = "buvid3=" + buvid() + "; b_nut=" + std::to_string(::time(nullptr)) + "; ";
  }
  c += cfg_.cookie;
  cookie_ = std::move(c);
  cookieBuilt_ = true;
  return cookie_;
}

int64_t Bili::accountMid() const {
  if (!midParsed_) {
    // DedeUserID is the account mid. Anything non-numeric or absent leaves it 0, which sends the
    // connection as a guest -- the same behaviour as having no cookie at all, so a malformed
    // cookie degrades instead of dropping the connection.
    static const char* kKey = "DedeUserID=";
    const size_t at = cfg_.cookie.find(kKey);
    if (at != std::string::npos) {
      int64_t v = 0;
      for (size_t i = at + std::strlen(kKey); i < cfg_.cookie.size(); ++i) {
        const char ch = cfg_.cookie[i];
        if (ch < '0' || ch > '9') break;
        v = v * 10 + (ch - '0');
      }
      mid_ = v;
    }
    midParsed_ = true;
  }
  return mid_;
}

std::string Bili::get(const std::string& url, size_t maxBytes) const {
  pwvideo::HttpRequest req;
  req.url = url;
  req.maxBytes = maxBytes;
  // Referer and Origin are part of what the API inspects; a request without them is answered with
  // -352 even when the signature is correct.
  req.headers.emplace_back("Referer", "https://live.bilibili.com/");
  req.headers.emplace_back("Origin", "https://live.bilibili.com");
  req.headers.emplace_back("Accept", "application/json, text/plain, */*");
  req.headers.emplace_back("Accept-Language", "zh-CN,zh;q=0.9");
  req.headers.emplace_back("Cookie", cookieHeader());

  pwvideo::HttpResponse res;
  std::string err;
  pwvideo::HttpClient http(cfg_.userAgent);
  if (!http.get(req, res, &err)) throw std::runtime_error("GET " + url + ": " + err);
  return res.body;
}

int64_t Bili::resolveRoom(const std::string& input) const {
  int64_t candidate = 0;
  if (!firstDigits(input, candidate)) {
    // No digits in the path: a b23.tv short link, most likely. Following the redirect and scanning
    // the landing page for a room number avoids adding a second resolution API for this case.
    const std::string body = get(input, 8u * 1024u * 1024u);
    static const char* kPatterns[] = {"live.bilibili.com/", "\"roomid\":", "\"room_id\":"};
    for (const char* pat : kPatterns) {
      size_t at = body.find(pat);
      while (at != std::string::npos) {
        at += std::strlen(pat);
        if (firstDigits(body.substr(at, 24), candidate)) return candidate;
        at = body.find(pat, at);
      }
    }
    throw std::runtime_error("no room number found in \"" + input + "\"");
  }

  // A short room number is not the same as the real one, and getDanmuInfo wants the real one.
  // room_init needs no signing and no login.
  const std::string body =
      get(std::string(kApiBase) + "/room/v1/Room/room_init?id=" + std::to_string(candidate));
  std::string err;
  const Json root = Json::parse(body, &err);
  if (root["code"].num() != 0)
    throw std::runtime_error("room_init failed for " + std::to_string(candidate) + ": " + body);
  const int64_t real = Json::path(root, {"data", "room_id"}).num();
  if (real <= 0) throw std::runtime_error("room_init returned no room_id: " + body);
  return real;
}

DanmuEndpoint Bili::danmuEndpoint(int64_t realRoomId) {
  // WBI signature: sort the parameters, percent-encode, append wts, then md5(query + mixin_key).
  // The five characters !'()* are stripped from the values first, matching the web client.
  std::vector<std::pair<std::string, std::string>> params = {
      {"id", std::to_string(realRoomId)},
      {"type", "0"},
      {"context_type", "0"},
      {"wts", std::to_string(static_cast<int64_t>(::time(nullptr)))},
  };
  std::sort(params.begin(), params.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::string query;
  for (size_t i = 0; i < params.size(); ++i) {
    if (i) query += '&';
    std::string value;
    for (char c : params[i].second) {
      if (std::strchr("!'()*", c) == nullptr) value.push_back(c);
    }
    query += percentEncode(params[i].first) + "=" + percentEncode(value);
  }
  const std::string wRid = md5Hex(query + wbiMixinKey());

  const std::string url = std::string(kApiBase) + "/xlive/web-room/v1/index/getDanmuInfo?" + query +
                          "&w_rid=" + wRid;
  const std::string body = get(url);
  std::string err;
  const Json root = Json::parse(body, &err);
  const int64_t code = root["code"].num();
  if (code != 0) {
    // -352 is risk control: the usual cause is a missing or stale buvid3, not a bad signature.
    throw std::runtime_error("getDanmuInfo failed (code " + std::to_string(code) + "): " + body);
  }

  DanmuEndpoint ep;
  ep.token = Json::path(root, {"data", "token"}).str();
  const Json& host = Json::path(root, {"data", "host_list"}).at(0);
  ep.host = Json::path(host, {"host"}).str();
  ep.wssPort = static_cast<int>(Json::path(host, {"wss_port"}).num());
  if (ep.host.empty() || ep.token.empty()) throw std::runtime_error("getDanmuInfo: " + body);
  return ep;
}

std::string Bili::authPacket(int64_t realRoomId, const std::string& token, int64_t uid) {
  // Body is JSON. The protobuf ClientVerifyReq that most open-source clients send is answered by
  // an immediate disconnect; see docs/internals.md for the variants that were tried.
  //
  // uid is the account mid and must match the credentials that fetched the token, or the server
  // drops the connection. 0 is the guest path: it works, but the server then masks every nickname.
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(realRoomId));
  std::string body = "{\"uid\":";
  body += std::to_string(uid);
  body += ",\"roomid\":";
  body += buf;
  body += ",\"protover\":3,\"platform\":\"web\",\"type\":2,\"key\":\"";
  body += token;
  body += "\"}";

  std::string out;
  PacketHeader h;
  h.proto = kProtoCommandBrotli;
  h.type = kTypeAuth;
  h.sequence = 1;
  putHeader(out, h, body);
  return out;
}

std::string Bili::heartbeatPacket() {
  std::string out;
  PacketHeader h;
  h.proto = kProtoCommandBrotli;
  h.type = kTypeHeartbeat;
  h.sequence = 1;
  putHeader(out, h, "[object]");
  return out;
}

bool Bili::nextPacket(std::string_view& in, PacketHeader& header, std::string_view& body) {
  if (in.size() < 16) return false;
  const char* p = in.data();
  header.totalSize = readBe32(p);
  header.headerSize = readBe16(p + 4);
  header.proto = readBe16(p + 6);
  header.type = readBe32(p + 8);
  header.sequence = readBe32(p + 12);

  // A totalSize below the header, or larger than what is left, means the stream is out of sync.
  // Truncating the buffer instead would make every later packet read at the wrong offset.
  if (header.headerSize < 16 || header.totalSize < header.headerSize ||
      header.totalSize > in.size()) {
    in.remove_suffix(std::min<size_t>(in.size(), 16));  // drop it so the caller stops
    return false;
  }
  body = in.substr(header.headerSize, header.totalSize - header.headerSize);
  in.remove_prefix(header.totalSize);
  return true;
}

bool Bili::decompress(uint16_t proto, std::string_view body, std::string& out, size_t maxOut) {
  switch (proto) {
    case kProtoCommand:  // already JSON
      if (body.size() > maxOut) return false;
      out.assign(body);
      return true;
    case kProtoCommandZlib: return zlibDecompress(body, out, maxOut);
    case kProtoCommandBrotli: return brotliDecompress(body, out, maxOut);
    default: return false;
  }
}

bool Bili::forEachJson(std::string_view message, const JsonVisitor& fn) {
  PacketHeader header;
  std::string_view body;
  if (!nextPacket(message, header, body)) return false;
  if (header.type != kTypeCommand && header.type != kTypeAuthResp) return false;

  // The body is sniffed before proto is trusted: see the header comment for why. A body starting
  // with '{' or '[' is a document whatever the header claims.
  const bool isDocument = !body.empty() && (body.front() == '{' || body.front() == '[');
  if (isDocument) return fn(body);

  std::string expanded;
  if (!decompress(header.proto, body, expanded)) return false;

  // A decompressed body is itself a sequence of framed packets, one per JSON document.
  std::string_view batch(expanded);
  for (;;) {
    PacketHeader inner;
    std::string_view innerBody;
    if (!nextPacket(batch, inner, innerBody)) break;
    if (innerBody.empty()) continue;
    if (!fn(innerBody)) return true;  // early stop is not a parse failure
  }
  return true;
}

namespace {

/** Returns j when it is already an object, otherwise parses it as a JSON string and returns that.
 *
 *  bilibili is inconsistent about the same field arriving either way: info[0][13] (the emote
 *  options) came as a nested object on the wire, while info[0][14] (the voice config) in the very
 *  same message arrived as the string "{}". Both spellings have to be read, so the two paths are
 *  normalised here rather than at every call site. */
const Json& objectOrJson(const Json& j) {
  if (j.type() == Json::Type::Object) return j;
  static thread_local Json parsed;  // a miss returns this empty one
  parsed = Json();
  if (j.type() != Json::Type::String) return parsed;
  std::string err;
  parsed = Json::parse(j.str(), &err);
  if (!err.empty()) parsed = Json();
  return parsed;
}

/** bilibili hands out these image URLs as plain http even though the same path answers over https
 *  (measured: identical 15906-byte PNG both ways, see docs/internals.md). Fetching them as
 *  advertised would put every picture on the wire in cleartext and would break outright behind any
 *  network that filters port 80, which then looks exactly like "the picture never arrived". */
std::string httpsUrl(const std::string& url) {
  constexpr const char* kPlain = "http://";
  if (url.compare(0, strlen(kPlain), kPlain) != 0) return url;
  return "https://" + url.substr(strlen(kPlain));
}

/** Decodes base64, standard or URL-safe. False on anything that is not clean base64.
 *
 *  EVP_DecodeBlock wants a NUL-terminated buffer and does not skip whitespace, so both are dealt
 *  with here. The `pb` field arrives as one unbroken line, but the value is data from the network
 *  and a stray newline must not turn a perfectly good event into a dropped one.
 *
 *  The two alphabets are folded into one here rather than left to fail. `EVP_DecodeBlock` rejects
 *  `-` and `_`, so a payload that arrived URL-safe encoded would decode to nothing and both
 *  protobuf events would be dropped -- silently, and with exactly the symptom this milestone was
 *  built to eliminate: a panel that looks like it is not being sent anything. Three characters of
 *  translation buy the whole feature immunity to that one packaging change.
 *
 *  OpenSSL rather than hand-rolled because it is already a dependency of this file (md5, and the
 *  TLS layer) and a base64 decoder is exactly the kind of thing that has four subtly different
 *  implementations to choose between. */
bool base64Decode(std::string_view in, std::string& out) {
  std::string packed;
  packed.reserve(in.size());
  for (const char ch : in) {
    if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
    // URL-safe alphabet back onto the standard one. Only these two differ; `=` padding is shared.
    if (ch == '-') {
      packed.push_back('+');
      continue;
    }
    if (ch == '_') {
      packed.push_back('/');
      continue;
    }
    packed.push_back(ch);
  }
  // Four characters encode three bytes; anything else cannot be a whole number of groups.
  if (packed.empty() || packed.size() % 4 != 0) {
    out.clear();
    return false;
  }
  out.assign(packed.size() / 4 * 3 + 1, '\0');
  const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(packed.data()),
                                static_cast<int>(packed.size()));
  if (n < 0) {
    out.clear();
    return false;
  }
  size_t len = static_cast<size_t>(n);
  // EVP_DecodeBlock counts the padding as if it carried data, so it reports up to two bytes more
  // than the payload really holds. Strip them rather than letting the protobuf reader meet two
  // zero bytes of noise at the end of the message.
  if (packed.back() == '=') --len;
  if (packed.size() >= 2 && packed[packed.size() - 2] == '=') --len;
  out.resize(len);
  return true;
}

/** The body of an entry row.
 *
 *  One person reads as a plain verb; several read as a count, named after the first of them. The
 *  count is of *all* the people the row stands for, including the one it is named after, so the
 *  rows of a stream add up to the number of people who actually walked in. */
std::string entryBody(int count) {
  if (count <= 1) return "进入了直播间";
  return "等 " + std::to_string(count) + " 人进入了直播间";
}

}  // namespace

void Bili::splitFragments(const std::string& text,
                           const std::vector<std::pair<std::string, Fragment>>& emotes,
                           std::vector<Fragment>& out) {
  out.clear();
  if (emotes.empty()) {
    if (!text.empty()) out.push_back(Fragment{Fragment::Kind::Text, text, "", 0});
    return;
  }
  std::string run;
  for (size_t i = 0; i < text.size();) {
    const Fragment* hit = nullptr;
    size_t hitLen = 0;
    // Only a bracketed run the platform itself advertised is treated as an emote, so ordinary
    // brackets in a message stay ordinary brackets.
    if (text[i] == '[') {
      for (const auto& e : emotes) {
        if (e.first.size() > 2 && text.compare(i, e.first.size(), e.first) == 0) {
          hit = &e.second;
          hitLen = e.first.size();
          break;
        }
      }
    }
    if (hit) {
      if (!run.empty()) {
        out.push_back(Fragment{Fragment::Kind::Text, run, "", 0});
        run.clear();
      }
      out.push_back(*hit);
      i += hitLen;
      continue;
    }
    run.push_back(text[i]);
    ++i;
  }
  if (!run.empty()) out.push_back(Fragment{Fragment::Kind::Text, run, "", 0});
}

bool Bili::parseMessage(const Json& json, Message& out) {
  const std::string cmd = json["cmd"].str();

  if (cmd == "DANMU_MSG") {
    const Json& ia = json["info"];
    // Verified on the wire: info carries 18 elements and info[0][15] is the object holding both
    // "extra" and "user". info[9] is a timestamp object and holds no extra -- reading it from
    // there, as an earlier revision did, silently loses dm_type and every role badge.
    const Json& head = ia.at(0);
    const Json& tail = head.at(15);

    out.tsMs = head.at(4).num();
    out.user = ia.at(2).at(1).str();
    const std::string text = ia.at(1).str();

    // info[0][12] is dm_type: 0 text, 1 emote, 2 voice. An emote danmaku -- an official little
    // face or one of the streamer's own uploaded ones -- is the whole body being one picture:
    // extra.emots is null in that case and there is no token to look up, so reading only that map
    // drew every emote as its name spelled out. Measured in room 545068:
    //   info[1]         "嘻嘻"                     (the name, no brackets)
    //   info[0][13]     {"emoticon_unique":"room_545068_9780","url":"http://i0.hdslb.com/...",
    //                    "width":162,"height":162,"in_player_area":1,"bulge_display":1,...}
    // The official ones carry "official_<n>" in emoticon_unique instead of "room_<id>_<n>"; the
    // picture is in the same field either way.
    const int64_t dmType = head.at(12).num();
    if (dmType == 1) {
      const Json& opts = objectOrJson(head.at(13));
      Fragment f;
      f.kind = Fragment::Kind::Emote;
      f.text = text;  // what the sender picked; drawn as text if the picture never arrives
      f.url = httpsUrl(opts["url"].str());
      f.px = static_cast<int>(opts["height"].num());
      out.parts.push_back(std::move(f));
    } else {
      // A text body may still carry emotes inline: the platform then names them in extra.emots,
      // keyed by the literal bracketed token that appears in the text.
      std::vector<std::pair<std::string, Fragment>> emotes;
      const std::string extra = tail["extra"].str();
      if (!extra.empty()) {
        std::string err;
        const Json ex = Json::parse(extra, &err);
        if (err.empty()) {
          for (const auto& kv : ex["emots"].items()) {
            Fragment f;
            f.kind = Fragment::Kind::Emote;
            f.text = kv.first;
            f.url = httpsUrl(kv.second["url"].str());
            f.px = static_cast<int>(kv.second["height"].num());
            emotes.emplace_back(kv.first, std::move(f));
          }
        }
      }
      splitFragments(text, emotes, out.parts);
    }

    // The user object is what carries the avatar, the name colour and the guard tier. It is
    // available without a login: only the nickname is masked, not the face.
    const Json& user = tail["user"];
    if (user.type() == Json::Type::Object) {
      const Json& base = user["base"];
      if (!base["name"].str().empty()) out.user = base["name"].str();
      out.avatarUrl = base["face"].str();
      const int64_t nc = base["name_color"].num();
      if (nc > 0) out.userColor = static_cast<uint32_t>(nc) & 0xFFFFFFu;
      // guard_level 1 总督, 2 提督, 3 舰长. Only the membership tier is distinguishable here:
      // the owner and moderator badges are not part of this payload.
      const int64_t guard = user["guard"]["guard_level"].num();
      const int64_t medalGuard = user["medal"]["guard_level"].num();
      if (guard >= 3 || medalGuard >= 3) out.type = UserType::Member;
    }
    return true;
  }

  // Two of the events stopped being JSON. INTERACT_WORD_V2 (somebody entered the room) and
  // SEND_GIFT_V2 (a gift) now carry exactly one field besides `cmd` and `dmscore`: `data.pb`, a
  // base64 protobuf message. That is the whole payload, so a reader that only knew about JSON saw
  // these documents as empty and dropped them -- which is what happened before this milestone, and
  // why the events looked like a network problem rather than a parser one.
  if (cmd == "INTERACT_WORD_V2") {
    std::string blob;
    if (!base64Decode(json["data"]["pb"].str(), blob)) return false;
    const pb::Message root(blob);
    // Field 2 is the nickname, 8 the millisecond timestamp. Both were present in 110 of 110
    // captures in room 21852; the field numbers are pinned by tests/bili_test.cpp.
    out.user = std::string(root.str(2));
    // A row with no nickname has nothing to identify the viewer with, so it is dropped rather than
    // shown as an unattributed notice.
    if (out.user.empty()) return false;
    out.kind = MsgKind::Entry;
    out.tsMs = static_cast<int64_t>(root.num(8));
    out.parts.push_back(Fragment{Fragment::Kind::Text, entryBody(1), "", 0});
    // No face on purpose. The one is at field 22.2.2, and it is not read: entries arrive several
    // times a second and each is a different viewer, so the column would be a strip of
    // single-use faces the avatar cache could never keep -- mostly the site's default one.
    return true;
  }

  if (cmd == "SEND_GIFT_V2") {
    std::string blob;
    if (!base64Decode(json["data"]["pb"].str(), blob)) return false;
    const pb::Message root(blob);
    // Field 10 is the gift itself: 1 the id, 2 the name, 3 how many, 5 the price in gold, 10 the
    // send time in seconds, 18 the verb the site chose for this gift, 35 the picture (1 a PNG,
    // 2 a webp). Field 2 is the sender and 3 the sender's face. Field 13.1 is how many of this
    // gift the sender has given inside the current combo run, which is not this message's own
    // count and so is not what the row shows.
    const pb::Message gift = root.sub(10);
    const std::string giftName(gift.str(2));
    // Dropped without a name: the row would have nothing on it but the verb, because the picture
    // is only a picture -- and the renderer falls back to the name when it is still downloading.
    if (giftName.empty()) return false;
    out.user = std::string(root.str(2));
    if (out.user.empty()) return false;

    out.kind = MsgKind::Gift;
    out.avatarUrl = httpsUrl(std::string(root.str(3)));
    out.tsMs = static_cast<int64_t>(gift.num(10)) * 1000;

    std::string verb(gift.str(18));
    if (verb.empty()) verb = "送出";  // measured as "投喂" on every capture, one gift family
    // Marked as a verb rather than left as body text: it is what the site says happened, and drawn
    // in the body colour it read as a line somebody had typed. The panel picks the accent; this
    // layer only says which run is the verb.
    Fragment verbRun;
    verbRun.text = verb + " ";
    verbRun.verb = true;
    out.parts.push_back(std::move(verbRun));

    Fragment icon;
    icon.kind = Fragment::Kind::Emote;
    // Carried as an emote fragment rather than as a new field: the picture sits in the text flow
    // next to the name, which is exactly what a Fragment is, and it is fetched and cached by the
    // same store as the emotes -- a small closed set that every viewer in the room reuses.
    icon.text = giftName;
    // Field 1 only. Field 2 is the same picture as a webp and is not read even when 1 is missing:
    // whether gdk-pixbuf can decode one depends on the distribution's pixbuf loaders, so taking it
    // would make the icon work on some machines and silently fail on others. With no URL the
    // fragment falls back to the gift name, which is a row that still reads.
    const pb::Message info = gift.sub(35);
    icon.url = httpsUrl(std::string(info.str(1)));
    out.parts.push_back(std::move(icon));

    const uint64_t count = gift.num(3);
    // Written through Message::withCount(), which is the same call GiftMerger rebuilds a row with,
    // so the text on the row and the number the merger adds up cannot come to disagree.
    out = out.withCount(count > 0 ? static_cast<int64_t>(count) : 1);

    // What makes two of these the same gift to a viewer: the same gift, from the same person, again.
    // GiftMerger keys on this, together with the sender, so that a viewer tapping one button ten
    // times gets one row counting ten. The id is the gift's own identity and the name is what the
    // row actually says, so both go in: any disagreement between them -- the site renaming a gift,
    // a payload that arrives without an id at all -- leaves the rows apart instead of folding two
    // different things together. That is the direction to fail in, two rows where one would do,
    // rather than a row claiming one viewer sent a gift they did not.
    out.mergeKey = "gift/" + std::to_string(gift.num(1)) + "/" + giftName;
    return true;
  }

  if (cmd == "LIKE_INFO_V3_CLICK") {
    // The one of the three that still arrives as JSON, and the only one whose nickname is *not*
    // masked on an anonymous connection: 6 of 6 captures here had a real name, against 106 of 110
    // masked on the entry notices from the same socket. Whatever the inconsistency is, this event
    // hands the name over, so it is the one place a masked room still names its viewers.
    const Json& d = json["data"];
    out.user = d["uname"].str();
    if (out.user.empty()) out.user = d["uinfo"]["base"]["name"].str();
    if (out.user.empty()) return false;
    out.kind = MsgKind::Like;
    out.avatarUrl = httpsUrl(d["uinfo"]["base"]["face"].str());
    // like_text was "为主播点赞了" on every capture; it is a field rather than a constant here
    // because the site does vary it (a mystery viewer's like reads differently), and a constant
    // would quietly discard that.
    std::string text = d["like_text"].str();
    if (text.empty()) text = "为主播点赞了";
    out.parts.push_back(Fragment{Fragment::Kind::Text, text, "", 0});
    // tsMs is left at 0 on purpose, and this is the one handler that does not set it: none of the
    // captures carried a time field, so any value here would be invented. Nothing reads it for this
    // kind -- the dwell clock only ever sees MsgKind::Paid -- so an unset timestamp costs nothing
    // here, whereas a guessed one would be a wrong number nothing could correct later.
    // uinfo.guard.level is the guard tier and would set UserType::Member. Not read: every capture
    // had 0, so the mapping from that field's non-zero values is a guess, and this project does not
    // record guesses. See docs/internals.md.
    return true;
  }

  // The two card kinds below follow the documented field tables but have not yet been observed on
  // this machine, so every field is read optionally: a document that does not match yields a
  // message with less in it rather than being dropped.
  static const std::vector<std::pair<std::string, Fragment>> kNoEmotes;

  if (cmd == "SUPER_CHAT_MESSAGE") {
    const Json& d = json["data"];
    out.kind = MsgKind::Paid;
    // The sender sits under data.user_info, not at the top level of data: user_info.uname is the
    // nickname, user_info.face the avatar, and user_info.guard_level the 大航海 tier (1 总督,
    // 2 提督, 3 舰长). The documented table has no data.uname / data.face at all, so reading those
    // keys left both empty and the card was drawn with no name line on it -- the one line that says
    // who paid. user_info is present in every documented sample; a payload without it still yields
    // a card with the price and the body rather than being dropped.
    const Json& user = d["user_info"];
    out.user = user["uname"].str();
    out.avatarUrl = httpsUrl(user["face"].str());
    out.tsMs = static_cast<int64_t>(d["start_time"].num() * 1000);
    const int64_t price = d["price"].num();
    if (price > 0) {
      out.amount = "CN¥" + std::to_string(price);
      // The number as well as the string: the pinned layer times how long this stays up from it.
      out.amountValue = price;
    }
    splitFragments(d["message"].str(), kNoEmotes, out.parts);
    // medal_info is null when the sender wears no fan medal, hence the second read: either field
    // marks them a guard, which is how the danmaku path reads its equivalent pair.
    const int64_t guard = user["guard_level"].num();
    const int64_t medalGuard = d["medal_info"]["guard_level"].num();
    if (guard >= 3 || medalGuard >= 3) out.type = UserType::Member;
    return true;
  }

  if (cmd == "GUARD_BUY") {
    const Json& d = json["data"];
    out.kind = MsgKind::Membership;
    out.user = d["username"].str();
    out.tsMs = static_cast<int64_t>(d["start_time"].num() * 1000);
    // price is in gold, and 1000 gold is one yuan.
    const int64_t gold = d["price"].num();
    const int64_t level = d["guard_level"].num();
    std::string label;
    if (level == 3) label = "舰长";
    else if (level == 2) label = "提督";
    else if (level == 1) label = "总督";
    if (gold > 0) {
      const std::string money = "CN¥" + std::to_string(gold / 1000);
      out.amount = label.empty() ? money : label + " · " + money;
    } else {
      out.amount = label;
    }
    if (level >= 3) out.type = UserType::Member;
    return true;
  }

  return false;
}

bool EntryMerger::flush(int64_t nowMs, Message& out) {
  if (windowMs_ > 0 && nowMs < nextRowAtMs_) return false;
  if (!flushAll(out)) return false;
  // The next window starts when the row went out, and only once it has: an empty flush must not
  // open a window that then swallows the notices arriving immediately after it.
  if (windowMs_ > 0) nextRowAtMs_ = nowMs + windowMs_;
  return true;
}

bool EntryMerger::flushAll(Message& out) {
  if (pending_ == 0) return false;

  // The row stands for every notice since the previous one, and it is named after the first of
  // them. The count therefore includes the one named on the row, which is what makes the rows of a
  // stream add up to the number of people who walked in rather than to something one less each.
  out = head_;
  out.parts.clear();
  out.parts.push_back(Fragment{Fragment::Kind::Text, entryBody(pending_), "", 0});

  if (windowMs_ > 0) {
    pending_ = 0;
    // head_ is deliberately left alone: it is unreachable the moment pending_ hits 0, because
    // push() overwrites it before the next notice is counted, so clearing it would cost a Message
    // assignment per window for nothing. flushAll() has no clock and so no cooldown to move -- that
    // is flush()'s job, and at end of stream there is no next window to start.
  } else {
    // Unmerged: one notice in, one row out, so a burst still produces a row for each of them and
    // the next push starts a fresh head. head_ is only cleared once it has been used up, which is
    // what stops the second notice of a burst being labelled with the first one's name.
    if (--pending_ == 0) head_ = Message();
  }
  return true;
}

}  // namespace dwm