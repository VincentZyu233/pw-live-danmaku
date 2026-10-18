// Unit tests for the bilibili protocol helpers: WBI signing, packet framing, and DANMU_MSG parsing.
//
// The vectors here are measured values, not invented ones. The mixin key is a real pair of site
// keys and the expected output is what the web client computes from them; md5 and percent-encoding
// use the published test vectors. A regression in any of these would show up as an API refusing
// the request with code -352, which is expensive to debug from the outside, so it is pinned here.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bili.hpp"
#include "json.hpp"
#include "message.hpp"
// Only for the dwell table, which is pure arithmetic and needs no cairo or panel state. Pulling the
// header in costs pango, which the rest of this binary does not otherwise need.
#include "notice.hpp"
#include "pb.hpp"

namespace {

int failures = 0;

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

/** Numeric counterpart. Deliberately a separate name rather than an overload: a template taking
 *  any type would also match const char* pairs and win over the string overload for literals. */
template <typename A, typename B>
void checkEqInt(A got, B want, const char* what) {
  if (!(got == want)) {
    std::fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", what,
                 static_cast<long long>(got), static_cast<long long>(want));
    ++failures;
  }
}

using namespace dwm;

// A real site key pair, and the mixin key derived from it. Measured: the web client turns these two
// into the string on the right, and getDanmuInfo answers code 0 with the signature built from it.
void testMixinKey() {
  const std::string img = "7cd084941338484aae1ad9425b84077c";
  const std::string sub = "4932caff0ff746eab6f01bf08b70ac45";
  checkEq(Bili::mixinKey(img, sub), "ea1db124af3c7062474693fa704f4ff8", "mixinKey: measured pair");
  checkEqInt(Bili::mixinKey(img, sub).size(), size_t(32), "mixinKey: 32 characters");
  // Short input must be refused rather than read out of bounds.
  check(Bili::mixinKey("abc", "def").empty(), "mixinKey: short input refused");
  check(Bili::mixinKey("", "").empty(), "mixinKey: empty input refused");
}

void testMd5() {
  checkEq(Bili::md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e", "md5: empty string");
  checkEq(Bili::md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72", "md5: abc");
}

void testPercentEncode() {
  checkEq(Bili::percentEncode("abcXYZ019"), "abcXYZ019", "percentEncode: unreserved kept");
  checkEq(Bili::percentEncode("a b"), "a%20b", "percentEncode: space");
  checkEq(Bili::percentEncode("/?&="), "%2F%3F%26%3D", "percentEncode: reserved escaped");
  checkEq(Bili::percentEncode("-_.~"), "-_.~", "percentEncode: the four unreserved marks");
}

/** Builds a packet the way the protocol does, so the tests read like the wire format. */
std::string makePacket(uint16_t proto, uint32_t type, const std::string& body,
                       uint16_t headerSize = 16) {
  std::string out;
  const uint32_t total = headerSize + static_cast<uint32_t>(body.size());
  auto be16 = [&](uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
  };
  auto be32 = [&](uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
  };
  be32(total);
  be16(headerSize);
  be16(proto);
  be32(type);
  be32(1);  // sequence
  out.append(body);
  return out;
}

void testPacketFraming() {
  const std::string doc1 = R"({"cmd":"DANMU_MSG"})";
  const std::string doc2 = R"({"cmd":"NOTICE_MSG"})";
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, doc1) +
                           makePacket(kProtoCommand, kTypeCommand, doc2);

  std::string_view in(wire);
  PacketHeader h;
  std::string_view body;
  int seen = 0;
  while (Bili::nextPacket(in, h, body)) {
    checkEqInt(h.type, kTypeCommand, "framing: type field");
    checkEq(std::string(body), seen == 0 ? doc1 : doc2, "framing: body in order");
    ++seen;
  }
  checkEqInt(seen, 2, "framing: both packets walked");
  check(in.empty(), "framing: buffer fully consumed");
}

void testFramingResync() {
  // A truncated tail must stop the walk, not hand back a body read from the wrong offset.
  const std::string doc = R"({"cmd":"X"})";
  std::string wire = makePacket(kProtoCommand, kTypeCommand, doc);
  wire.resize(wire.size() - 4);  // chop the last four bytes

  std::string_view in(wire);
  PacketHeader h;
  std::string_view body;
  const bool got = Bili::nextPacket(in, h, body);
  // The first header now claims more bytes than remain, so it must be refused.
  check(!got, "framing: truncated packet refused");

  // A totalSize below the header is nonsense and must not be trusted.
  std::string bogus(16, '\0');
  bogus[3] = 4;  // totalSize = 4 < headerSize
  std::string_view bin(bogus);
  check(!Bili::nextPacket(bin, h, body), "framing: totalSize below headerSize refused");
}

// The authentication reply really does arrive as proto=3 with a plain JSON body. Trusting proto
// there is what made an early revision report "auth reply could not be decompressed".
void testAuthReplyShape() {
  const std::string wire = makePacket(kProtoCommandBrotli, kTypeAuthResp, R"({"code":0})");
  std::vector<std::string> docs;
  Bili::forEachJson(wire, [&](std::string_view doc) {
    docs.emplace_back(doc);
    return true;
  });
  checkEqInt(docs.size(), size_t(1), "auth reply: one document");
  if (!docs.empty()) checkEq(docs[0], R"({"code":0})", "auth reply: body passed through as JSON");
}

void testForEachJsonBatch() {
  // A decompressed command body is a run of framed packets; each must be visited separately.
  // forEachJson takes the whole message, so the inner run is wrapped in an outer frame. Using
  // proto 0 for the wrapper keeps the test free of a compression step while exercising exactly the
  // nested walk that brotli output goes through.
  const std::string inner = makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"A"})") +
                            makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"B"})");
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, inner);

  std::vector<std::string> cmds;
  Bili::forEachJson(wire, [&](std::string_view doc) {
    cmds.push_back(Json::parse(doc)["cmd"].str());
    return true;
  });
  checkEqInt(cmds.size(), size_t(2), "batch: two documents");
  if (cmds.size() == 2) {
    checkEq(cmds[0], "A", "batch: first cmd");
    checkEq(cmds[1], "B", "batch: second cmd");
  }
}

void testForEachJsonStopsEarly() {
  const std::string inner = makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"A"})") +
                            makePacket(kProtoCommand, kTypeCommand, R"({"cmd":"B"})");
  const std::string wire = makePacket(kProtoCommand, kTypeCommand, inner);
  int visited = 0;
  Bili::forEachJson(wire, [&](std::string_view) {
    ++visited;
    return false;  // stop
  });
  checkEqInt(visited, 1, "visitor: early stop honoured");
}

// A DANMU_MSG body shaped like the one measured on the wire: one "info" array holding everything,
// with info[0][15] carrying both "extra" (a JSON string) and "user". Using the real shape matters
// here -- an earlier revision read the extras from info[9], which is a timestamp object, so every
// field it tried to read was silently absent and the tests passed against a fixture with the same
// mistake in it.
void testParseDanmaku() {
  // A custom raw-string delimiter (R"J(...)J") rather than the default: these fixtures contain
  // }] and ,) sequences, and the default R"( ... )" would terminate on the first )" it met.
  const std::string body = R"J({"cmd":"DANMU_MSG","dm_v2":"","info":[
   [0,1,25,16777215,1791046553806,-1688173992,0,"f9d986db",0,0,0,"",0,"{}","{}",
    {"mode":0,"show_player_type":0,
     "extra":"{\"mode\":1,\"dm_type\":0}",
     "user":{"base":{"face":"https://i1.hdslb.com/bfs/face/abc.jpg","name":"旱獭邮箱_",
                     "name_color":16711680},
             "medal":{"guard_level":3,"name":"德云色"}}}],
   "老丈人只为名",
   [0,"旱獭邮箱_",0,0,0,10000,1,""],
   [10,0,16777215,6406235,"{}",0],["",""],0,0,0,0,
   {"ct":"AFFF4206","ts":1791046553},0,0,null,null,0,1040,[49],null]})J";

  Message m;
  const bool parsed = Bili::parseMessage(Json::parse(body), m);
  check(parsed, "DANMU_MSG: recognised");
  if (!parsed) return;
  checkEq(m.plainText(), "老丈人只为名", "DANMU_MSG: text from info[1]");
  checkEq(m.user, "旱獭邮箱_", "DANMU_MSG: name");
  checkEq(m.avatarUrl, "https://i1.hdslb.com/bfs/face/abc.jpg", "DANMU_MSG: face from info[0][15]");
  checkEqInt(m.userColor, 16711680u, "DANMU_MSG: name colour");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Member),
            "DANMU_MSG: guard_level 3 becomes Member");
  checkEqInt(m.tsMs, 1791046553806LL, "DANMU_MSG: timestamp");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Text), "DANMU_MSG: plain text kind");
  checkEqInt(m.parts.size(), size_t(1), "DANMU_MSG: no emotes, so one text fragment");
}

// Emote danmaku: the token is literal in the body and extra.emots maps it to an image.
void testParseDanmakuEmote() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791046559000,1,0,"h",0,0,0,"",0,"{}","{}",
    {"extra":"{\"mode\":1,\"dm_type\":1,\"emots\":{\"[夏日热浪_想要]\":{\"url\":\"https://i0.hdslb.com/bfs/live/x.png\",\"width\":20,\"height\":20}}}",
     "user":{"base":{"name":"user","face":""}}}],
   "白花300块[夏日热浪_想要]",
   [0,"user",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "emote danmaku: recognised");
  // Three fragments: the leading text, the emote, and nothing trailing.
  checkEqInt(m.parts.size(), size_t(2), "emote danmaku: text split around the token");
  if (m.parts.size() == 2) {
    checkEq(m.parts[0].text, "白花300块", "emote danmaku: leading text run");
    checkEqInt(static_cast<int>(m.parts[1].kind), static_cast<int>(Fragment::Kind::Emote),
              "emote danmaku: second fragment is an emote");
    checkEq(m.parts[1].text, "[夏日热浪_想要]", "emote danmaku: token kept as typed");
    checkEq(m.parts[1].url, "https://i0.hdslb.com/bfs/live/x.png", "emote danmaku: image url");
    checkEqInt(m.parts[1].px, 20, "emote danmaku: advertised size");
  }
  // Brackets that the platform did not advertise stay literal text.
  checkEq(m.plainText(), "白花300块[夏日热浪_想要]", "emote danmaku: plain text round-trips");
}

// A streamer's own uploaded emote (主播自定义表情). Measured on the wire in room 545068, cut down to
// the fields this reads: info[0][12] is 1, info[0][13] carries the picture, and extra.emots -- the
// map the inline-token case uses -- is null. The body is the emote's name, without brackets.
void testParseDanmakuStreamerEmote() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791065364000,1,0,"h",0,0,0,"",
    1,
    {"bulge_display":1,"emoticon_unique":"room_545068_9780","height":162,"in_player_area":1,
     "is_dynamic":0,"url":"http://i0.hdslb.com/bfs/live/46ec99d09a2b8c84314f0c506a55660ee08a9069.png",
     "width":162},
    {},
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emoticon_unique\":\"room_545068_9780\",\"emots\":null}",
     "user":{"base":{"name":"钱少奢侈硬享受","face":"https://i1.hdslb.com/bfs/face/a.jpg"}}}],
   "嘻嘻",
   [0,"钱少奢侈硬享受",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "streamer emote: recognised");
  checkEqInt(m.parts.size(), size_t(1), "streamer emote: the whole body is the picture");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Emote),
              "streamer emote: one emote fragment");
    checkEq(m.parts[0].url,
            "https://i0.hdslb.com/bfs/live/46ec99d09a2b8c84314f0c506a55660ee08a9069.png",
            "streamer emote: url from info[0][13], upgraded to https");
    checkEq(m.parts[0].text, "嘻嘻", "streamer emote: name kept as the text fallback");
    checkEqInt(m.parts[0].px, 162, "streamer emote: advertised size");
  }
  checkEq(m.plainText(), "嘻嘻", "streamer emote: plain text round-trips");
}

// The same emote with info[0][13] as a JSON string instead of a nested object, which is how some
// payloads spell it (info[0][14] in the fixture above is an object where the wire had "{}").
void testParseDanmakuEmoteOptionsAsString() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1,1,0,"h",0,0,0,"",1,
    "{\"emoticon_unique\":\"official_13\",\"height\":60,\"width\":183,\"url\":\"https://i0.hdslb.com/bfs/live/a98e359.png\"}",
    "{}",
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emots\":null}","user":{"base":{"name":"u","face":""}}}],
   "妙啊",
   [0,"u",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "emote options as string: recognised");
  checkEqInt(m.parts.size(), size_t(1), "emote options as string: one emote fragment");
  if (m.parts.size() == 1) {
    checkEq(m.parts[0].url, "https://i0.hdslb.com/bfs/live/a98e359.png",
            "emote options as string: url read out of the string");
    checkEqInt(m.parts[0].px, 60, "emote options as string: advertised size");
  }
}

// A dm_type of 0 with no advertised emote must not be mistaken for one: the whole point of reading
// info[0][12] rather than trusting the body's shape.
void testParseDanmakuTextWithBrackets() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1,1,0,"h",0,0,0,"",0,"{}","{}",
    {"extra":"{\"mode\":0,\"dm_type\":0,\"emots\":null}","user":{"base":{"name":"u","face":""}}}],
   "[嘻嘻]",
   [0,"u",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "brackets without dm_type: recognised");
  checkEqInt(m.parts.size(), size_t(1), "brackets without dm_type: one text run");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Text),
              "brackets without dm_type: stays text");
    checkEq(m.parts[0].text, "[嘻嘻]", "brackets without dm_type: brackets are literal");
  }
}

// A 收藏集 / 表情包 emote: same carrier as the streamer's own, but the body keeps its brackets and
// emoticon_unique is the literal string "upower_[token]". Measured in room 1921271623, whose chat is
// full of these; the token reported in the bug this fixed was "[Neuro sama收藏集_晚安]".
void testParseDanmakuBracketedEmoteToken() {
  const std::string body = R"J({"cmd":"DANMU_MSG","info":[
   [0,1,25,16777215,1791070000000,1,0,"h",0,0,0,"",1,
    {"bulge_display":1,"emoticon_unique":"upower_[Neuro sama收藏集_晚安]","height":20,
     "in_player_area":1,"is_dynamic":0,
     "url":"https://i0.hdslb.com/bfs/garb/bf7dc14be1fe85256bb2b7237802a29d443b5664.png",
     "width":20},
    {},
    {"extra":"{\"mode\":0,\"dm_type\":1,\"emoticon_unique\":\"upower_[Neuro sama收藏集_晚安]\",\"emots\":null}",
     "user":{"base":{"name":"帕金尼","face":"https://i1.hdslb.com/bfs/face/a.jpg"}}}],
   "[Neuro sama收藏集_晚安]",
   [0,"帕金尼",0,0,0,10000,1,""],
   [10,0,0,0,"{}",0],["",""],0,0,0,0,
   {"ct":"x","ts":1},0,0,null,null,0,1,[1],null]})J";

  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "bracketed emote token: recognised");
  // One fragment, and it is a picture: the brackets are part of the emote's name, not a token to
  // look up. Treating them as text is exactly what this used to do.
  checkEqInt(m.parts.size(), size_t(1), "bracketed emote token: the whole body is the picture");
  if (m.parts.size() == 1) {
    checkEqInt(static_cast<int>(m.parts[0].kind), static_cast<int>(Fragment::Kind::Emote),
              "bracketed emote token: one emote fragment");
    checkEq(m.parts[0].url,
            "https://i0.hdslb.com/bfs/garb/bf7dc14be1fe85256bb2b7237802a29d443b5664.png",
            "bracketed emote token: url from info[0][13]");
    checkEqInt(m.parts[0].px, 20, "bracketed emote token: advertised size");
  }
  // The name is kept so the row still reads if the picture never arrives.
  checkEq(m.plainText(), "[Neuro sama收藏集_晚安]", "bracketed emote token: plain text round-trips");
}

// The two card kinds follow the documented field tables but have not been observed on this
// machine, so these tests pin the mapping rather than a capture. For the SC that mapping is
// data.user_info (uname / face / guard_level) with data.medal_info possibly null: reading a name
// off the top level of data is the mistake these checks exist to catch, since it leaves the card
// with no name line at all.
void testParsePaidCard() {
  const std::string body =
      R"J({"cmd":"SUPER_CHAT_MESSAGE","data":{"medal_info":null,"message":"已经没有什么可怕的了")J"
      R"J(,"price":30,"start_time":1791046553,"user_info":{"face":"http://i1.hdslb.com/bfs/face/x.jpg")J"
      R"J(,"guard_level":3,"uname":"五条悟"}}})J";
  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "SUPER_CHAT_MESSAGE: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Paid), "SC: kind is Paid");
  checkEq(m.user, "五条悟", "SC: user comes from data.user_info.uname");
  checkEq(m.avatarUrl, "https://i1.hdslb.com/bfs/face/x.jpg",
          "SC: face comes from data.user_info.face, and an http URL is upgraded like the others");
  checkEq(m.plainText(), "已经没有什么可怕的了", "SC: message");
  checkEq(m.amount, "CN¥30", "SC: price");
  // The pinned layer times a notice's dwell from the number, not from the string it is shown as, so
  // the price has to arrive in both forms.
  checkEqInt(m.amountValue, 30, "SC: price also carried as a number, for the dwell");
  checkEqInt(m.tsMs, 1791046553000LL, "SC: start_time is seconds, converted to ms");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Member),
             "SC: user_info.guard_level 3 is a guard, medal_info being null notwithstanding");

  // A payload that does not match the table is thinner, not dropped: the price and the body are
  // still worth drawing even with no sender block on it.
  Message bare;
  check(Bili::parseMessage(
            Json::parse(R"J({"cmd":"SUPER_CHAT_MESSAGE","data":{"message":"x","price":30}})J"),
            bare),
        "SUPER_CHAT_MESSAGE: a payload without user_info is still recognised");
  check(bare.user.empty() && bare.plainText() == "x",
        "SC: and keeps what it did carry rather than dropping the card");
}

void testPinnedDwellTable() {
  // The bands of the published price table, including the boundaries themselves: a message priced
  // exactly at a threshold belongs to the band that starts there, not to the cheaper one below it.
  struct Case {
    int64_t value;
    int64_t expectMs;
    const char* what;
  };
  const Case cases[] = {
      {0, 60 * 1000, "nothing paid falls in the shortest band rather than zero"},
      {29, 60 * 1000, "just under 30 is still the 60s band"},
      {30, 60 * 1000, "30 is 60s"},
      {49, 60 * 1000, "just under 50 is still 60s"},
      {50, 2 * 60 * 1000, "50 is 2min"},
      {99, 2 * 60 * 1000, "just under 100 is still 2min"},
      {100, 5 * 60 * 1000, "100 is 5min"},
      {500, 30 * 60 * 1000, "500 is 30min"},
      {1000, 60 * 60 * 1000, "1000 is 1h"},
      {1999, 60 * 60 * 1000, "just under 2000 is still 1h"},
      {2000, 2 * 60 * 60 * 1000, "2000 is 2h"},
      {100000, 2 * 60 * 60 * 1000, "above the top band does not run past 2h"},
  };
  for (const Case& c : cases)
    checkEqInt(PinnedLayer::dwellFor(c.value), c.expectMs, c.what);
}

void testParseMembershipCard() {
  // price is in gold, and 1000 gold is one yuan.
  const std::string body = R"J({"cmd":"GUARD_BUY","data":{"uid":1,"username":"xfgryujk","guard_level":3,"num":1,"price":198000,"start_time":1791046553}})J";
  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "GUARD_BUY: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Membership), "GUARD: kind");
  checkEq(m.user, "xfgryujk", "GUARD: user");
  checkEq(m.amount, "舰长 · CN¥198", "GUARD: tier and price converted from gold");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Member), "GUARD: membership tier");
}

void testSplitFragments() {
  std::vector<std::pair<std::string, Fragment>> emotes;
  Fragment f;
  f.kind = Fragment::Kind::Emote;
  f.text = "[笑哭]";
  emotes.emplace_back("[笑哭]", f);

  std::vector<Fragment> out;
  // The bracketed run the platform did not advertise must not be swallowed.
  Bili::splitFragments("a[not an emote]b[笑哭]c", emotes, out);
  checkEqInt(out.size(), size_t(3), "split: text, emote, trailing text");
  if (out.size() == 3) {
    checkEq(out[0].text, "a[not an emote]b", "split: unknown brackets stay in the text run");
    checkEq(out[1].text, "[笑哭]", "split: the advertised token became a fragment");
    checkEq(out[2].text, "c", "split: trailing text");
  }

  Bili::splitFragments("", emotes, out);
  checkEqInt(out.size(), size_t(0), "split: empty text yields no fragments");
  Bili::splitFragments("plain", emotes, out);
  checkEqInt(out.size(), size_t(1), "split: no match yields one text run");
}

// The three ambient events: entry notices, gifts and likes.
//
// INTERACT_WORD_V2 and SEND_GIFT_V2 do not arrive as JSON. `data` holds `dmscore` and one base64
// protobuf blob, and nothing else -- the envelope is trimmed to those two fields below, which is
// itself the finding being pinned. The blobs are verbatim captures from room 21852 on an anonymous
// connection (2026-10-04), so a change in how bilibili packs them fails here rather than turning
// into a silently empty panel.

/// Wraps a captured blob back into the envelope the server sent it in.
std::string blobDoc(const char* cmd, const std::string& pbB64) {
  return std::string(R"({"cmd":")") + cmd + R"(","data":{"dmscore":8,"pb":")" + pbB64 + R"("}})";
}

// INTERACT_WORD_V2, dmscore 4. Field 2 is the nickname, field 8 the timestamp in milliseconds, and
// field 22.2.2 a face this project deliberately does not read.
const std::string kEntryPb =
    "EgRsKioqIgEBKAEw3KoBOOm5h9YGQOncm6qQNEosCJOrIRATGgnmiZjpqazlpLQgpLqeBijAgYMGMMCBgwY4wIGDBmDcqgFo"
    "4QtiAHi8h86T1djO7RiaAQCyAcABElIKBGwqKioSSmh0dHBzOi8vaTIuaGRzbGIuY29tL2Jmcy9mYWNlL2M1NjY2MWZlZTFh"
    "MzhkZDg5NDRlOGRmYzczOGVhNjZjMjVjY2U0NmUuanBnGmQKCeaJmOmprOWktBATGMCBgwYgwIGDBijAgYMGMKS6ngY4hT1Q"
    "k6shYOELegkjOTE5Mjk4Q0OCAQkjOTE5Mjk4Q0OKAQkjOTE5Mjk4Q0OSAQcjRkZGRkZGmgEJIzkxOTI5OEU2IgIIBzIAugF6"
    "CkpodHRwczovL2kwLmhkc2xiLmNvbS9iZnMvbGl2ZS9iYjg4NzM0NTU4YzYzODNhNGNmYjVmYTE2Yzk3NDlkNTI5MGQ5NWU4"
    "LnBuZxIq5pu+57uP5rS76LeD6L+H77yM6L+R5pyf5LiO5L2g5LqS5Yqo6L6D5bCRGATCAQA=";

// SEND_GIFT_V2, one 人气票. Field 2 is the sender, 3 the face, 10 the gift, 13.1 the running combo
// count -- which is deliberately *not* what the row shows as this message's count.
const std::string kGiftPb =
    "EgbmsKcqKioaSmh0dHBzOi8vaTAuaGRzbGIuY29tL2Jmcy9mYWNlL2M5Mzg1OGRhNDIxMzkzMzkzNTg0ODI4MzAzMmFhZjZl"
    "Nzc5MTBiZDguanBnQicIk6shKAgyCeaJmOmprOWktDie9/UCQJ739QJInvf1AlCe9/UCWAFSwQUIxIkCEgnkurrmsJTnpagY"
    "ASABKGQwZDhkQgRnb2xkShM0ODI0MDU5NzQyOTM2NjM0MzY4UOy5h9YGWAFiUWJhdGNoOmdpZnQ6Y29tYm9faWQ6NzBlMWFm"
    "MDZiMGE0NjkzNTY5ZGMyMGEwOWE1OTAxMWI6NTQ2MTk1OjMzOTg4OjE3OTEwODk5MDAuNDUyNWgKcGR4BYUBAACAP4gBAZIB"
    "BuaKleWWgsABztDoaOoBDwoJ6ICB55Wq6IyEEJOrIYoCjwIIk6shEogCCgnogIHnlarojIQSSWh0dHA6Ly9pMC5oZHNsYi5j"
    "b20vYmZzL2ZhY2UvYmM1Y2ExMDEzMTNkNGRiMjIzYzM5NWQ2NDc3OWU3NmViMzQ4MmQ2MC5qcGcyVgoJ6ICB55Wq6IyEEklo"
    "dHRwOi8vaTAuaGRzbGIuY29tL2Jmcy9mYWNlL2JjNWNhMTAxMzEzZDRkYjIyM2MzOTVkNjQ3NzllNzZlYjM0ODJkNjAuanBn"
    "OlgIARJUMjAyNeeZvuWkp1VQ5Li744CBMjAyNeW5tOW6puWVhuS4muW9seWTjeWKm+WlllVQ5Li744CBMjAyMuW5tOW6puiB"
    "lOWQiOWIm+S9nOWlllVQ5Li7kgIAmgLlAQpKaHR0cHM6Ly9zMS5oZHNsYi5jb20vYmZzL2xpdmUvN2Q3ODY0NmVlNGVjYzkw"
    "MGNlYmJlNDZjYjk2M2Y0MjEwNzk1NDhmMC5wbmcSS2h0dHBzOi8vaTAuaGRzbGIuY29tL2Jmcy9saXZlLzRkNTIxNWRhNDdk"
    "MjViM2RiNzg3ZDJiYTU0NDkwMTZlZTE0MmRjODAud2VicCpKaHR0cHM6Ly9pMC5oZHNsYi5jb20vYmZzL2xpdmUvNzBkOTJh"
    "ZjBhODM3YWU2NmUyNWYyMzQ5M2E1NmUxNjkwZjc2ZWJiYS5naWaqAgBYAWoCCAZ6ngIStwEKBuawpyoqKhJKaHR0cHM6Ly9p"
    "MC5oZHNsYi5jb20vYmZzL2ZhY2UvYzkzODU4ZGE0MjEzOTMzOTM1ODQ4MjgzMDMyYWFmNmU3NzkxMGJkOC5qcGcyVAoG5rCn"
    "KioqEkpodHRwczovL2kwLmhkc2xiLmNvbS9iZnMvZmFjZS9jOTM4NThkYTQyMTM5MzM5MzU4NDgyODMwMzJhYWY2ZTc3OTEw"
    "YmQ4LmpwZzoLIP///////////wEaYgoJ5omY6ams5aS0EAgYnvf1AiCe9/UCKJ739QIwnvf1AkgBUJOrIWBmegkjNTc2MkE3"
    "OTmCAQkjNTc2MkE3OTmKAQkjNTc2MkE3OTmSAQcjRkZGRkZGmgEJIzU3NjJBN0U2";

void testParseEntryNotice() {
  Message m;
  check(Bili::parseMessage(Json::parse(blobDoc("INTERACT_WORD_V2", kEntryPb)), m),
        "INTERACT_WORD_V2: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Entry), "entry: kind");
  // The nickname arrives masked on an anonymous connection, and this is the event that carries it
  // that way: the like notices below are the ones that do not.
  checkEq(m.user, "l***", "entry: nickname from field 2 of the protobuf blob");
  checkEq(m.plainText(), "进入了直播间", "entry: body");
  checkEqInt(m.tsMs, 1791089897065LL, "entry: field 8 is already in milliseconds");
  checkEq(m.avatarUrl, "", "entry: no face is requested");
  checkEqInt(m.parts.size(), size_t(1), "entry: plain text, so one fragment");
  // An entry notice is not a card. It reads as a chat row without a bar, and a card frame here
  // would be a visible slab around a one-line ambient event.
  check(!isCard(m.kind), "entry: not a card");
}

void testParseEntryNoticeRefusesUnusableBlobs() {
  // No blob at all: the document shape is not what this event sends, and a row with no nickname in
  // it would be an unattributed notice.
  Message m;
  check(!Bili::parseMessage(Json::parse(R"({"cmd":"INTERACT_WORD_V2","data":{"dmscore":8}})"), m),
        "entry: a document with no pb is refused");
  // A well-formed envelope carrying something that is not base64.
  check(!Bili::parseMessage(Json::parse(blobDoc("INTERACT_WORD_V2", "not base64 at all")), m),
        "entry: a blob that is not base64 is refused");
  // Valid base64, but the bytes are not a protobuf message: no field 2 comes out, so no nickname,
  // so no row. This is the path a future change in packing would take, and it must be a refusal
  // rather than a blank row.
  check(!Bili::parseMessage(Json::parse(blobDoc("INTERACT_WORD_V2", "AAAAAAAAAAAA")), m),
        "entry: a blob that is not a message is refused");
}

void testProtobufBlobsDecodeFromEitherAlphabet() {
  // The captures above are in the standard alphabet. If the site ever switches the envelope to
  // URL-safe base64, the same bytes arrive with '-' and '/' swapped for '+' and '/', and
  // EVP_DecodeBlock rejects them -- which would drop every entry notice and every gift silently,
  // looking exactly like the network being down. So both alphabets have to decode to the same
  // thing, and this pins that.
  //
  // The translation is only observable on a payload that actually contains those two characters,
  // so this runs over whichever of the two captures has them and reports which one it used.
  const auto swap = [](const std::string& b64) {
    std::string out;
    for (const char ch : b64) {
      if (ch == '+') out.push_back('-');
      else if (ch == '/') out.push_back('_');
      else out.push_back(ch);
    }
    return out;
  };
  const bool entryHasBoth = kEntryPb.find_first_of("+/") != std::string::npos;
  const bool giftHasBoth = kGiftPb.find_first_of("+/") != std::string::npos;
  check(entryHasBoth || giftHasBoth,
        "base64: at least one capture exercises '+' or '/', or this test proves nothing");

  if (entryHasBoth) {
    Message a, b;
    check(Bili::parseMessage(Json::parse(blobDoc("INTERACT_WORD_V2", kEntryPb)), a),
          "base64: the standard-alphabet entry decodes");
    check(Bili::parseMessage(Json::parse(blobDoc("INTERACT_WORD_V2", swap(kEntryPb))), b),
          "base64: the same entry in the URL-safe alphabet decodes");
    checkEq(b.user, a.user, "base64: same nickname either way");
    checkEqInt(b.tsMs, a.tsMs, "base64: same timestamp either way");
  }
  if (giftHasBoth) {
    Message a, b;
    check(Bili::parseMessage(Json::parse(blobDoc("SEND_GIFT_V2", kGiftPb)), a),
          "base64: the standard-alphabet gift decodes");
    check(Bili::parseMessage(Json::parse(blobDoc("SEND_GIFT_V2", swap(kGiftPb))), b),
          "base64: the same gift in the URL-safe alphabet decodes");
    checkEq(b.plainText(), a.plainText(), "base64: same gift row either way");
    checkEq(b.parts[1].url, a.parts[1].url, "base64: same gift picture either way");
  }
}

void testParseGift() {
  Message m;
  check(Bili::parseMessage(Json::parse(blobDoc("SEND_GIFT_V2", kGiftPb)), m),
        "SEND_GIFT_V2: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Gift), "gift: kind");
  checkEq(m.user, "氧***", "gift: sender from field 2");
  checkEq(m.avatarUrl, "https://i0.hdslb.com/bfs/face/c93858da4213933935848283032aaf6e77910bd8.jpg",
          "gift: sender's face");
  // One 人气票: the verb, the picture, and no count. The count is omitted at one rather than shown
  // as "×1", which is what the site's own notice does.
  checkEq(m.plainText(), "投喂 人气票", "gift: verb, name, and no count for a single one");
  checkEqInt(m.parts.size(), size_t(2), "gift: the verb and the name-as-picture");
  // The verb is marked, and the marker is on that fragment rather than on the row: the panel colours
  // by offset into the body it already laid out, so a row whose verb did not survive the parse
  // would draw as plain white text -- which is exactly how it looked before anything marked it.
  check(m.parts[0].verb, "gift: the verb is marked, so the panel can accent it");
  check(!m.parts[1].verb, "gift: and the gift name is not -- it is a picture, not the verb");
  checkEqInt(static_cast<int>(m.parts[1].kind), static_cast<int>(Fragment::Kind::Emote),
             "gift: the picture rides the emote fragment, so the existing cache and draw path serve "
             "it");
  checkEq(m.parts[1].text, "人气票", "gift: the picture falls back to the gift name as text");
  checkEq(m.parts[1].url,
          "https://s1.hdslb.com/bfs/live/7d78646ee4ecc900cebbe46cb963f421079548f0.png",
          "gift: gift_info field 1, the PNG; field 2 is a webp and this decoder is not promised "
          "to read those");
  checkEqInt(m.tsMs, 1791089900000LL, "gift: field 10 of the gift is seconds, converted to ms");
  checkEqInt(m.count, 1LL, "gift: field 3 was 1, carried as a number so a merge can add it up");
  // The whole key, not just that there is one: gift id 33988 and the name, because a gift merged
  // under the wrong key is a row that says one of two people sent a gift the other one sent.
  checkEq(m.mergeKey, "gift/33988/人气票", "gift: the key GiftMerger groups on, id and name");
  check(!isCard(m.kind), "gift: not a card");
}

void testParseGiftCount() {
  // The same captured blob with the gift's `num` changed from 1 to 6, which is the shape a
  // multi-unit gift arrives in. No capture in this room had one above 1, so the count is pinned by
  // editing the single base64 character that carries it -- verified to change nothing else in the
  // blob -- rather than by a payload written from scratch, which would only prove that the parser
  // agrees with itself about field numbers it is also the source of.
  const std::string marker = "ASABKGQwZDhk";  // spans the byte that holds num
  const size_t at = kGiftPb.find(marker);
  check(at != std::string::npos, "gift count: located num inside the captured blob");
  if (at == std::string::npos) return;
  std::string b64 = kGiftPb;
  b64.replace(at, marker.size(), "BiABKGQwZDhk");

  Message m;
  check(Bili::parseMessage(Json::parse(blobDoc("SEND_GIFT_V2", b64)), m),
        "gift count: recognised");
  checkEq(m.plainText(), "投喂 人气票 ×6", "gift count: the count is appended, and only above one");
  checkEqInt(m.parts.size(), size_t(3), "gift count: verb, picture, count");
  checkEqInt(m.count, 6LL, "gift count: the same number, separately, for the merger to add up");
  checkEq(m.user, "氧***", "gift count: nothing else about the row moved");
}

/** A gift row as the site layer builds it: verb, icon, and a count when there is more than one.
 *  Written by hand rather than decoded from the blob, because these tests are about what happens to
 *  rows over time rather than about what one wire payload means -- that is testParseGift above. */
Message makeGift(const char* who, const char* giftKey, int64_t count, const char* face = "") {
  Message g;
  g.kind = MsgKind::Gift;
  g.user = who;
  g.avatarUrl = face;
  g.mergeKey = giftKey;
  g.parts.push_back(Fragment{Fragment::Kind::Text, "投喂 ", "", 0, true});
  // The icon carries the gift's name, taken as the last component of the key, so a fixture written
  // gift/2/小番茄 produces a row that reads like 小番茄 and not like every other fixture.
  const std::string key(giftKey);
  const size_t slash = key.rfind('/');
  g.parts.push_back(
      Fragment{Fragment::Kind::Emote, slash == std::string::npos ? key : key.substr(slash + 1),
               "", 0});
  if (count > 1)
    g.parts.push_back(Fragment{Fragment::Kind::Text, " ×" + std::to_string(count), "", 0});
  g.count = count;
  return g;
}

void testGiftMergerFoldsOneViewerTappingTheSameGift() {
  // The case this exists for: one viewer taps one gift button ten times and gets ten rows, each of
  // which pushes a line of chat off the top. One row counting ten is the whole feature.
  GiftMerger m(3000);
  std::vector<Message> rows;
  m.push(makeGift("A", "gift/1/人气票", 1), 0, rows);
  checkEqInt(rows.size(), size_t(0), "gift merger: nothing is drawn while the window is open");
  for (int i = 0; i < 9; ++i) m.push(makeGift("A", "gift/1/人气票", 1), 100 + i, rows);
  checkEqInt(rows.size(), size_t(0), "gift merger: nor by the tenth tap");
  checkEqInt(m.pending(), 10LL, "gift merger: all ten are held");
  checkEqInt(m.pendingRows(), size_t(1), "gift merger: as one run");

  check(!m.flush(2999, rows), "gift merger: not due just before the window ends");
  check(m.flush(3000, rows), "gift merger: due once it has");
  checkEqInt(rows.size(), size_t(1), "gift merger: ten gifts are one row");
  checkEq(rows[0].user, "A", "gift merger: named after the viewer, as the first one was");
  checkEq(rows[0].plainText(), "投喂 人气票 ×10", "gift merger: counting them");
  checkEqInt(rows[0].count, 10LL, "gift merger: and the number agrees with the text");
  // The mark has to survive the rebuild. withCount() rewrites the tail of the row, and a row that
  // came out of it with a plain verb would be a gift whose "投喂" went back to being chat-white.
  check(rows[0].parts[0].verb, "gift merger: the merged row still marks its verb");
  checkEqInt(m.pending(), 0LL, "gift merger: nothing left over");

  // A tap after the row went out is a new run, not an amendment to a row already on screen.
  m.push(makeGift("A", "gift/1/人气票", 1), 4000, rows);
  checkEqInt(m.pendingRows(), size_t(1), "gift merger: the next tap opens a new run");
  check(m.flush(7000, rows), "gift merger: which comes due on its own window");
  checkEqInt(rows.size(), size_t(2), "gift merger: and is a row of its own");
  checkEq(rows[1].plainText(), "投喂 人气票", "gift merger: a single gift shows no count at all");
}

void testGiftMergerAddsUpCountsNotEvents() {
  // A row for six of one gift plus one more is seven, not two. The number is summed because that is
  // what the row says; counting the events instead would under-report exactly the runs that matter.
  GiftMerger m(3000);
  std::vector<Message> rows;
  m.push(makeGift("A", "gift/1/人气票", 6), 0, rows);
  m.push(makeGift("A", "gift/1/人气票", 1), 100, rows);
  check(m.flush(3000, rows), "gift merger: due");
  checkEq(rows[0].plainText(), "投喂 人气票 ×7", "gift merger: 6 plus one is seven");
  checkEqInt(rows[0].count, 7LL, "gift merger: the number is the sum too");
  // Two sixes in a row: the rewritten text must replace the old one rather than sit after it, or
  // the row would read " ×6 ×12".
  m.push(makeGift("A", "gift/1/人气票", 6), 4000, rows);
  m.push(makeGift("A", "gift/1/人气票", 6), 4100, rows);
  check(m.flush(7000, rows), "gift merger: the next run is due");
  checkEq(rows[1].plainText(), "投喂 人气票 ×12", "gift merger: exactly one count on the row");
  checkEqInt(rows[1].parts.size(), size_t(3), "gift merger: verb, picture, one count");
}

void testGiftMergerKeysOnViewerAndGift() {
  // The three ways two gifts are *not* each other's duplicate. Each of these must stay a row, or the
  // feature starts losing gifts that really happened.
  GiftMerger m(3000);
  std::vector<Message> rows;
  m.push(makeGift("A", "gift/1/人气票", 1), 0, rows);      // the run
  m.push(makeGift("B", "gift/1/人气票", 1), 10, rows);     // another viewer, same gift
  m.push(makeGift("A", "gift/2/小番茄", 1), 20, rows);     // same viewer, another gift
  // Same masked nickname again, with another face: the part of the key that tells two people who
  // happen to share one apart.
  m.push(makeGift("A", "gift/1/人气票", 1, "https://face/b.jpg"), 30, rows);
  checkEqInt(m.pendingRows(), size_t(4), "gift merger: four runs, not one");
  checkEqInt(m.pending(), 4LL, "gift merger: four gifts held");

  check(m.flush(3000, rows), "gift merger: the first run is due on its own window");
  checkEqInt(rows.size(), size_t(1), "gift merger: the runs that opened later are not due yet -- "
                                    "each window starts at its own first gift, not at the first "
                                    "gift of the room");
  check(m.flush(3030, rows), "gift merger: the rest are due once their own windows pass");
  checkEqInt(rows.size(), size_t(4), "gift merger: all four rows come out -- a flush can publish "
                                    "more than one row");
  // Oldest run first, so the row that waited longest appears first. The map is keyed by viewer and
  // gift and knows nothing about arrival order, which is what makes this order worth pinning.
  checkEq(rows[0].user, "A", "gift merger: the run that opened first is published first");
  checkEq(rows[0].plainText(), "投喂 人气票", "gift merger: with no count of its own");
  checkEq(rows[1].user, "B", "gift merger: the other viewer's row is separate");
  checkEq(rows[2].parts[1].text, "小番茄", "gift merger: so is the same viewer sending another gift");
  checkEq(rows[3].avatarUrl, "https://face/b.jpg",
          "gift merger: and a masked nickname sharing a name is still two people");
  checkEqInt(m.pending(), 0LL, "gift merger: every run was closed");
  checkEqInt(m.pendingRows(), size_t(0), "gift merger: and none left behind");
}

void testGiftMergerWithoutAKeyOrWindow() {
  // Two rows that must never be folded into one: a site that gave no key to compare on, and the
  // window turned off. Both would otherwise lose a gift that was really sent.
  GiftMerger keyed(3000);
  std::vector<Message> rows;
  Message anonymous = makeGift("A", "人气票", 1);
  anonymous.mergeKey.clear();
  keyed.push(anonymous, 0, rows);
  checkEqInt(rows.size(), size_t(1), "no key: a row with no key is drawn at once, never held");
  checkEqInt(keyed.pending(), 0LL, "no key: and there is nothing to flush");

  GiftMerger unmerged(0);
  rows.clear();
  unmerged.push(makeGift("A", "gift/1/人气票", 1), 0, rows);
  unmerged.push(makeGift("A", "gift/1/人气票", 1), 0, rows);
  checkEqInt(rows.size(), size_t(2), "no window: a same-instant burst still gets one row each");
  checkEq(rows[0].plainText(), "投喂 人气票", "no window: first, unmerged");
  checkEq(rows[1].plainText(), "投喂 人气票", "no window: and second, unmerged");
  check(!unmerged.flush(999999, rows), "no window: nothing was held, so nothing comes out later");
  checkEqInt(rows.size(), size_t(2), "no window: and the burst is not replayed at the end");

  // The default has to be one number, not one per call site: the command line reads its own default
  // from here, so the two cannot drift apart when either is edited.
  checkEqInt(GiftMerger::kDefaultWindowMs, 3000, "gift merger: the published default window");
  checkEqInt(GiftMerger().windowMs(), GiftMerger::kDefaultWindowMs,
             "gift merger: and a default-constructed merger uses it");
}

void testGiftMergerFlushAllOnDisconnect() {
  // The socket died with a run in hand. flush() would refuse it, because its window has not elapsed
  // and no further gift is ever coming to close it -- so a gift that was really sent would be folded
  // away and the panel's account of who gave what would disagree with the stream.
  GiftMerger m(3000);
  std::vector<Message> rows;
  m.push(makeGift("A", "gift/1/人气票", 1), 0, rows);
  m.push(makeGift("A", "gift/1/人气票", 2), 100, rows);
  m.push(makeGift("B", "gift/2/小番茄", 1), 200, rows);
  checkEqInt(m.pending(), 4LL, "flushAll: three runs' worth of gifts are held back");
  check(!m.flush(300, rows), "flushAll: the windowed flush still refuses an unfinished run");
  check(m.flushAll(rows), "flushAll: but they go out anyway at the end of the stream");
  checkEqInt(rows.size(), size_t(2), "flushAll: one row per run, not per gift");
  checkEq(rows[0].plainText(), "投喂 人气票 ×3", "flushAll: counted in full");
  checkEq(rows[1].plainText(), "投喂 小番茄", "flushAll: and the other viewer is not lost either");
  checkEqInt(m.pending(), 0LL, "flushAll: nothing left over");
  check(!m.flushAll(rows), "flushAll: and nothing to repeat");
  checkEqInt(rows.size(), size_t(2), "flushAll: the rows are not replayed a second time");
}

void testParseLike() {
  // Verbatim shape from a capture, trimmed to the fields the parser reads plus uinfo.guard, whose
  // tier is deliberately left alone. Nothing about this event is masked on an anonymous
  // connection, which is the one asymmetry worth pinning: 6 of 6 captures had a real name here
  // against 106 of 110 masked on the entry notices from the same socket.
  const std::string body =
      R"({"cmd":"LIKE_INFO_V3_CLICK","data":{"dmscore":12,"like_icon":"https://i0.hdslb.com/x.png")"
      R"(,"like_text":"为主播点赞了","msg_type":6,"uid":1738797712,"uinfo":{"base":{"face":)"
      R"("https://i1.hdslb.com/bfs/face/c3b3e5bb55e0ddc7c10f6ea0443068d665dfe65c.jpg","name":)"
      R"("strangeLex","name_color":0},"guard":{"expired_str":"","level":0}}},"uname":"strangeLex"})";
  Message m;
  check(Bili::parseMessage(Json::parse(body), m), "LIKE_INFO_V3_CLICK: recognised");
  checkEqInt(static_cast<int>(m.kind), static_cast<int>(MsgKind::Like), "like: kind");
  checkEq(m.user, "strangeLex", "like: name");
  checkEq(m.plainText(), "为主播点赞了", "like: the site's own like_text, not a constant");
  checkEq(m.avatarUrl, "https://i1.hdslb.com/bfs/face/c3b3e5bb55e0ddc7c10f6ea0443068d665dfe65c.jpg",
          "like: face, unlike an entry notice");
  checkEqInt(static_cast<int>(m.type), static_cast<int>(UserType::Normal),
             "like: uinfo.guard.level was 0 in every capture, so no tier is claimed");
  checkEqInt(m.tsMs, 0LL,
             "like: tsMs stays unset on purpose -- no capture carried a time field, so any value "
             "here would be invented");
  check(!isCard(m.kind), "like: not a card");
}

void testEntryMerger() {
  EntryMerger m(5000);
  const auto entry = [](const char* who) {
    Message e;
    e.kind = MsgKind::Entry;
    e.user = who;
    e.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
    return e;
  };

  Message out;
  // The first notice of a stream is drawn at once: waiting for a full window before showing
  // anything would make an empty room look like a broken one.
  check(m.push(entry("A"), 0, out), "merger: the first notice is drawn immediately");
  checkEq(out.user, "A", "merger: it is named after the notice itself");
  checkEq(out.plainText(), "进入了直播间", "merger: one person reads as a plain verb");

  check(!m.push(entry("B"), 200, out), "merger: one inside the window is folded, not drawn");
  check(!m.push(entry("C"), 400, out), "merger: and another");
  checkEqInt(m.pending(), 2, "merger: both are counted");
  check(!m.flush(4999, out), "merger: still not due just before the window ends");

  // flush() is what publishes it, rather than the next arrival: a batch that is never flushed is a
  // batch whose last few people are not counted.
  check(m.flush(5000, out), "merger: due once the window has elapsed");
  checkEq(out.user, "B", "merger: the summary is named after the first of the batch");
  checkEq(out.plainText(), "等 2 人进入了直播间", "merger: the count covers the whole batch");
  checkEqInt(m.pending(), 0, "merger: nothing is left over");
  check(!m.flush(100000, out), "merger: nothing to publish when the batch is empty");

  // A burst spread over two windows. The notice that closes a window is counted into the row that
  // closes it and is *not* carried over to be named by the next one, or the same person would be
  // shown twice: once inside a count, once as the head of the row after it.
  check(!m.push(entry("D"), 6000, out), "merger: inside the next window, folded again");
  checkEqInt(m.pending(), 1, "merger: and counted");
  check(!m.push(entry("E"), 6100, out), "merger: folded again");
  check(m.flush(10000, out), "merger: the next window is due");
  checkEq(out.user, "D", "merger: named after the first notice of its own batch, not the earlier B");
  checkEq(out.plainText(), "等 2 人进入了直播间", "merger: and counted again");
  // Every notice of the five has now been shown exactly once: A alone, B and C together, D and E
  // together. That is the property the cooldown has to preserve.
  checkEqInt(m.pending(), 0, "merger: nothing carried across");
}

void testEntryMergerFlushAllOnDisconnect() {
  // The socket died with a batch in hand. flush() would refuse it, because its window has not
  // elapsed and no further notice is ever coming to close it -- so the people who did walk in would
  // be folded away and the row count would disagree with what the panel saw. flushAll() ignores the
  // window for exactly that moment.
  const auto entry = [](const char* who) {
    Message e;
    e.kind = MsgKind::Entry;
    e.user = who;
    e.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
    return e;
  };

  EntryMerger m(5000);
  Message out;
  // The first notice opens the window and goes out at once (as it always does), so the batch left
  // hanging at the end of a stream is everything that arrived after it.
  check(m.push(entry("A"), 0, out), "flushAll: the first notice is the one that opens the window");
  check(!m.push(entry("B"), 100, out), "flushAll: the next is folded into it");
  check(!m.push(entry("C"), 200, out), "flushAll: and the next");
  checkEqInt(m.pending(), 2, "flushAll: two notices are held back");
  // 200 is nowhere near the window that ends at 5000, so the ordinary flush still declines.
  check(!m.flush(300, out), "flushAll: the windowed flush still refuses an unfinished batch");
  check(m.flushAll(out), "flushAll: but the batch goes out anyway at the end of the stream");
  checkEq(out.user, "B", "flushAll: named after the first of the batch");
  checkEq(out.plainText(), "等 2 人进入了直播间", "flushAll: counted in full");
  checkEqInt(m.pending(), 0, "flushAll: nothing left over");
  check(!m.flushAll(out), "flushAll: and nothing to repeat");
  // The cooldown is not advanced by a forced flush -- there is no next window after the socket has
  // gone -- but flush() still owns it, and an empty flush must not open one either.
  check(!m.flush(300, out), "flushAll: an empty flush leaves the window alone");

  EntryMerger m2(5000);
  check(m2.push(entry("A"), 0, out), "flushAll: a second merger's first notice publishes");
  checkEqInt(m2.pending(), 0, "flushAll: with nothing left over");
  check(!m2.flush(10, out), "flushAll: an empty flush does not start a cooldown");
  check(!m2.push(entry("B"), 20, out), "flushAll: so the very next notice is still folded");

  // The default has to be one number, not one per call site: the command line reads its own
  // default from here, so the two cannot drift apart when either is edited.
  checkEqInt(EntryMerger::kDefaultWindowMs, 5000, "merger: the published default window");
  checkEqInt(EntryMerger().windowMs(), EntryMerger::kDefaultWindowMs,
             "merger: and a default-constructed merger uses it");
}

void testEntryMergerUnmerged() {
  // Window 0 is a real option, not a degenerate one: it is how the raw event rate was measured, and
  // it is what someone debugging the stream wants. Every notice gets its own row, named after
  // itself.
  EntryMerger m(0);
  const auto entry = [](const char* who) {
    Message e;
    e.kind = MsgKind::Entry;
    e.user = who;
    e.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
    return e;
  };
  Message out;
  check(m.push(entry("A"), 0, out), "unmerged: drawn");
  checkEq(out.user, "A", "unmerged: its own name");
  checkEq(out.plainText(), "进入了直播间", "unmerged: one person, plain verb");
  // Same millisecond as the last one: a burst back to back must still produce one row each, and
  // each must carry its own name rather than the previous one's.
  check(m.push(entry("B"), 0, out), "unmerged: the next of a same-instant burst is drawn too");
  checkEq(out.user, "B", "unmerged: and is named after itself");
  checkEq(out.plainText(), "进入了直播间", "unmerged: never a count");
  check(!m.flush(999999, out), "unmerged: nothing left over to publish");
}

void testEntryMergerWithoutAPendingBatch() {
  // An entry row arrives from the site layer already carrying its plain body. flush() rewrites it,
  // so the count must be the only thing that changes -- and the row must keep its kind and its name,
  // or the summary would stop being an entry row.
  EntryMerger m(1000);
  Message e;
  e.kind = MsgKind::Entry;
  e.user = "A";
  e.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
  Message out;
  // The very first notice is published even though its window has not passed: the cooldown starts
  // at zero, because a room that has just come up should show something immediately rather than
  // wait out a window for the first viewer.
  check(m.push(e, 0, out), "merger shape: the first notice publishes at once");
  check(!m.push(e, 100, out), "merger shape: a notice inside the window publishes nothing");
  checkEqInt(m.pending(), 1, "merger shape: but it is counted");
  check(m.push(e, 1000, out), "merger shape: due once the window has passed");
  checkEqInt(static_cast<int>(out.kind), static_cast<int>(MsgKind::Entry), "merger shape: kind kept");
  checkEqInt(out.parts.size(), size_t(1), "merger shape: the body is replaced, not appended to");
}

void testProtobufReader() {
  // A minimal encoder, so the reader is tested against bytes assembled by hand rather than against
  // its own output. Only the four wire types are here; that is all the fixtures need.
  std::string bytes;
  const auto putVarint = [](std::string& out, uint64_t v) {
    while (v >= 0x80) {
      out.push_back(static_cast<char>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  };
  const auto field = [&](int number, int wire) {
    // Every field number used below is one digit, so the key is a single byte.
    bytes.push_back(static_cast<char>((number << 3) | wire));
  };
  const auto addVarint = [&](int number, uint64_t v) {
    field(number, 0);
    putVarint(bytes, v);
  };
  const auto addLen = [&](int number, std::string_view payload) {
    field(number, 2);
    putVarint(bytes, payload.size());
    bytes.append(payload);
  };
  const auto addFixed32 = [&](int number, const char raw[4]) {
    field(number, 5);
    bytes.append(raw, 4);
  };

  addVarint(1, 300);
  addLen(2, "\xe4\xba\xba\xe6\xb0\x94\xe7\xa5\xa8");  // 人气票
  addVarint(3, 7);
  addFixed32(4, "\x00\x00\x80\x3F");                  // 1.0f
  addLen(5, "\x0a\x0cs1.hdslb.com");  // a submessage whose field 1 is 12 bytes of text
  addLen(6, std::string("\x80\xff", 2));              // not text
  addLen(7, "");

  const pb::Message m(bytes);
  checkEqInt(m.size(), size_t(7), "pb: every field found, the fixed32 one included");
  checkEqInt(static_cast<long long>(m.num(1)), 300LL, "pb: varint read");
  checkEq(std::string(m.str(2)), "人气票", "pb: CJK read as text");
  checkEqInt(static_cast<long long>(m.num(3)), 7LL, "pb: varint after a length-delimited field");
  // The point of the Wire enum being spelled out by number: this field used to end the scan, and
  // every field after it then read as absent -- which looked like the platform had stopped sending
  // them rather than like a parser that had given up half way.
  check(m.has(4), "pb: a fixed32 field is found");
  checkEq(std::string(m.sub(5).str(1)), "s1.hdslb.com", "pb: the fields after it are still there");
  checkEq(std::string(m.str(6)), "", "pb: bytes that are not UTF-8 do not read as text");
  checkEqInt(m.has(6) ? 1 : 0, 1, "pb: but the field is still there as bytes");
  checkEqInt(static_cast<long long>(m.bytes(6).size()), 2LL, "pb: its bytes are reachable");
  checkEq(std::string(m.str(7)), "", "pb: an empty payload is an empty string");
  check(m.has(7), "pb: and is still present");
  checkEqInt(static_cast<long long>(m.num(99)), 0LL, "pb: an absent field reads as 0");
  check(!m.has(99), "pb: and is not present");
  // A field of the wrong wire type reads as 0 rather than being reinterpreted: reading a
  // submessage's bytes as an integer would give a plausible wrong answer instead of an obviously
  // missing one.
  checkEqInt(static_cast<long long>(m.num(5)), 0LL, "pb: a submessage is not a number");

  // Malformed input must read as less rather than running off the end.
  const pb::Message truncated(std::string_view(bytes).substr(0, bytes.size() - 3));
  check(!truncated.has(7), "pb: a truncated payload loses its last field");
  check(truncated.has(1), "pb: but keeps the ones read before the truncation");
  const pb::Message zeroFieldNumber(std::string_view("\x00\x01", 2));
  checkEqInt(zeroFieldNumber.size(), size_t(0), "pb: field number 0 is refused");
  const pb::Message empty("");
  checkEqInt(empty.size(), size_t(0), "pb: an empty message is empty");
  checkEq(std::string(empty.str(1)), "", "pb: and every lookup on it reads as absent");
}

void testParseIgnoresOtherCmds() {
  for (const char* cmd : {"LOG_IN_NOTICE", "NOTICE_MSG", "WATCHED_CHANGE", "ONLINE_RANK_COUNT",
                          "STOP_LIVE_ROOM_LIST", "SEND_GIFT", "INTERACT_WORD", "ENTRY_EFFECT",
                          "COMBO_SEND", "DM_INTERACTION", "LIKE_INFO_V3_UPDATE",
                          "SUPER_CHAT_MESSAGE_UNKNOWN"}) {
    Message m;
    check(!Bili::parseMessage(Json::parse(std::string(R"({"cmd":")") + cmd + R"("})"), m), cmd);
  }
}

void testAuthPacketLayout() {
  const std::string p = Bili::authPacket(12345, "TOKEN");
  check(p.size() > 16, "auth packet: has a body");
  checkEq(std::string(p.substr(8, 4)), std::string("\x00\x00\x00\x07", 4), "auth packet: type is 7");
  // The body must be JSON: that is the whole finding this milestone recorded.
  const std::string body = p.substr(16);
  checkEq(body.substr(0, 1), "{", "auth packet: JSON body");
  std::string err;
  const Json j = Json::parse(body, &err);
  check(err.empty(), "auth packet: body parses as JSON");
  checkEq(std::to_string(j["roomid"].num()), "12345", "auth packet: roomid");
  checkEq(j["key"].str(), "TOKEN", "auth packet: key");
  checkEq(std::to_string(j["protover"].num()), "3", "auth packet: protover");
}

}  // namespace

int main() {
  testMixinKey();
  testMd5();
  testPercentEncode();
  testPacketFraming();
  testFramingResync();
  testAuthReplyShape();
  testForEachJsonBatch();
  testForEachJsonStopsEarly();
  testParseDanmaku();
  testParseDanmakuEmote();
  testParseDanmakuStreamerEmote();
  testParseDanmakuBracketedEmoteToken();
  testParseDanmakuEmoteOptionsAsString();
  testParseDanmakuTextWithBrackets();
  testSplitFragments();
  testParsePaidCard();
  testPinnedDwellTable();
  testParseMembershipCard();
  testParseEntryNotice();
  testParseEntryNoticeRefusesUnusableBlobs();
  testProtobufBlobsDecodeFromEitherAlphabet();
  testParseGift();
  testParseGiftCount();
  testGiftMergerFoldsOneViewerTappingTheSameGift();
  testGiftMergerAddsUpCountsNotEvents();
  testGiftMergerKeysOnViewerAndGift();
  testGiftMergerWithoutAKeyOrWindow();
  testGiftMergerFlushAllOnDisconnect();
  testParseLike();
  testEntryMerger();
  testEntryMergerFlushAllOnDisconnect();
  testEntryMergerUnmerged();
  testEntryMergerWithoutAPendingBatch();
  testProtobufReader();
  testParseIgnoresOtherCmds();
  testAuthPacketLayout();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("bili: all checks passed\n");
  return 0;
}