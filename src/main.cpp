// pw-live-danmaku (native) -- bilibili live chat as a PipeWire video node.
//
// Thread split, which is the part that is hardest to change later:
//   main thread    : pwvideo::VideoNode::run(); the frame callback renders from a snapshot
//   site thread    : HTTP bootstrap, websocket, protocol decode; appends to Shared::pending
//   avatar thread  : fetches and decodes face images, because AssetCache::get blocks
//
// The frame callback never touches the network and never waits on the site thread: it drains the
// pending queue under a short lock, hands the new messages to the panel, and copies the panel's
// static layer into the frame.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "images.hpp"
#include "bili.hpp"
#include "broadcast.hpp"
#include "cairo_util.hpp"
#include "demo_faces.hpp"
#include "history.hpp"
#include "message.hpp"
#include "notice.hpp"
#include "panel.hpp"
#include "pwvideo.hpp"
#include "ws.hpp"

namespace {

using namespace dwm;

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** Expand a leading `~` to $HOME. A shell does this before the program ever sees the argument, so
 *  it is redundant there -- but the systemd unit does not, and passes `~/.config/...` through as a
 *  literal string. The unit could write `%h` instead, but a path that only resolves when it comes
 *  from a shell is a trap: the same unit file, the same argument, works in a terminal and fails
 *  under systemd with a bare `cannot read cookie file`. Doing it here makes the argument mean the
 *  same thing no matter who expanded it. `~/` and a bare `~` both mean $HOME, as in a shell. */
std::string expandTilde(const std::string& path) {
  if (path != "~" && path.compare(0, 2, "~/") != 0) return path;
  const char* home = std::getenv("HOME");
  if (!home || !*home) return path;  // Nothing to expand against; let the open fail on its own terms
  return std::string(home) + path.substr(1);
}

/** Whether a whole string is decimal digits. Used for --ws's optional port: only a digits-only
 *  next argument is consumed as a value, so `--ws --room 545068` cannot swallow the room number
 *  and `--ws -1` is a port the flag never meant to accept. */
bool allDigits(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p)
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  return true;
}

/** Font chain. The comma form is a pango fallback chain resolved per character, so latin glyphs
 *  can come from one family and CJK from another. */
constexpr const char* kFontChain = "Noto Sans CJK SC,Sarasa Mono CL,DejaVu Sans,sans-serif";

/** Bounds how much work one frame can be asked to do. A burst of messages arriving while nobody was
 *  consuming frames is drained over several frames rather than all at once, so a flood cannot make
 *  a single frame arbitrarily expensive. */
constexpr size_t kMaxPerFrame = 40;

/** Bounds the queue when no consumer is attached. Without this an overlay left running for hours
 *  would accumulate every message that arrived. */
constexpr size_t kPendingCap = 400;

/** How many rows --dump waits for when --count was not given: enough for the panel to show
 *  something worth looking at, few enough to stay quick. Rows, not documents -- the same thing
 *  --count counts, so a folded entry notice does not bring the dump closer to being taken. */
constexpr int kDumpMessages = 6;

PanelTokens defaultTokens() {
  PanelTokens t;
  t.font = kFontChain;
  return t;
}

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "pw-live-danmaku (native)\n"
      "\n"
      "  --room ID|URL      Bilibili room, e.g. 545068 or https://live.bilibili.com/545068\n"
      "                     Required unless --demo is given. A b23.tv short link also works.\n"
      "  --cookie STR       Raw cookie header from the browser, e.g.\n"
      "                     \"SESSDATA=...; bili_jct=...; DedeUserID=...\".\n"
      "                     Optional: anonymous works but masks nicknames. Visible in ps(1),\n"
      "                     so prefer --cookie-file.\n"
      "  --cookie-file PATH Read the cookie from a file instead; recommended, since the file can\n"
      "                     be chmod 600 and the value never reaches the process arguments.\n"
      "                     A leading ~ is expanded to $HOME, which matters for the systemd unit:\n"
      "                     systemd does not do it, so the unit writes %%h and this is belt and\n"
      "                     braces.\n"
      "  --font NAME[,...]  Font family chain for the panel, default a CJK-capable fallback chain\n"
      "  --font-size N      Chat text size in px, for the username and the body. Default 28.\n"
      "                     The avatar box follows it: it is always one line tall.\n"
      "  --card-font-size N Card text size in px, for the paid and membership card lines.\n"
      "                     Default 30, the same size the chat uses plus 2.\n"
      "  --font-file PATH   Register a font file (or a directory) with fontconfig at startup;\n"
      "                     repeatable. Without --font its own family name is used\n"
      "  --entry-merge MS   Combine the entry notices into one row per this many milliseconds,\n"
      "                     labelled with the number of people it stands for. Measured in a room\n"
      "                     with a million watchers they arrive at 1.6/s against 0.36/s of chat,\n"
      "                     so a row each pushes most of what was typed off the top. Default 5000;\n"
      "                     0 gives every notice its own row as it arrives\n"
      "  --gift-merge MS    Combine repeated gifts from one viewer into one row per this many\n"
      "                     milliseconds, counted. Keyed on the viewer *and* the gift, so a viewer\n"
      "                     tapping one gift button ten times gets one row counting ten instead of\n"
      "                     ten rows saying the same thing. The row waits out the window, so a lone\n"
      "                     gift appears this long late. Default 3000; 0 gives every gift its own\n"
      "                     row as it arrives\n"
      "  --node NAME        PipeWire node name, default pw-live-danmaku\n"
      "  --desc TEXT        Node description (this is what the OBS dropdown shows), default\n"
      "                     \"Live Chat\"\n"
      "  --size WxH         Output size, default 480x1080. The OBS source size must match:\n"
      "                     a smaller negotiated size is clipped, not scaled.\n"
      "  --fps N            Frame-rate ceiling, default 30\n"
      "  --dump FILE        Render one sample frame to PNG and exit\n"
      "  --count N          Exit after drawing N rows (0 = never). Counts the rows that reach the\n"
      "                     panel, so entry notices folded into a combined row by --entry-merge\n"
      "                     and gifts folded into a counted row by --gift-merge do not count\n"
      "                     towards it\n"
      "  --history N        Keep the last N rows on disk and paint them back at the next start,\n"
      "                     above a rule so the two are not read as one conversation. Default 0,\n"
      "                     which is off: the overlay starts empty and writes nothing. Paid\n"
      "                     messages are not kept -- they are pinned with a dwell clock, and\n"
      "                     re-pinning yesterday's would be a lie about when it was paid\n"
      "  --history-file PATH  Where --history keeps its file, default\n"
      "                     $XDG_STATE_HOME/pw-live-danmaku/history.json. A leading ~ is expanded\n"
      "                     to $HOME. The file is written atomically and read on startup; one that\n"
      "                     is missing or unreadable simply starts the overlay with no history\n"
      "  --history-label TEXT  Text on the rule between restored and live rows, default 上次.\n"
      "                     Empty draws the rule alone\n"
      "  --ws [PORT]        Serve the same messages on a local WebSocket, so a second program can\n"
      "                     read the chat without watching the picture. One text frame per\n"
      "                     message, three tab-separated fields: avatar URL, name, body. The port\n"
      "                     is optional and defaults to 8765. The socket is bound to 127.0.0.1\n"
      "                     and has no TLS and no authentication: it is a pipe for a consumer on\n"
      "                     this machine, and must not be exposed past it\n"
      "  --ws-rtf           Wrap the name and body fields in Resonite rich-text tags (the wiki's\n"
      "                     Text_formatting list) instead of leaving them plain: the role colour on\n"
      "                     the name, the gift accent on a gift's verb and name, dimming on entry\n"
      "                     and like rows, and <noparse> around whatever the viewer typed so chat\n"
      "                     text cannot inject tags. Requires --ws\n"
      "  --seconds N        Exit after N seconds (0 = never)\n"
      "  --demo             Draw a fixed set of messages and no network at all, for tuning the\n"
      "                     layout without a live room. Overrides --room.\n"
      "  --verbose, -v      Log the protocol handshake and every parsed message\n"
      "  --help, -h\n");
}

/** Argument parsing has three outcomes, not two: a bad flag is not a request for help. Same
 *  reasoning as the sibling projects -- under Restart=always an unknown flag that exited 0 would
 *  be a silent restart loop reporting SUCCESS. */
enum class Args { Ok, Help, Error };

struct Options {
  std::string room, cookie, cookieFile, node = "pw-live-danmaku", desc = "Live Chat";
  std::string dump;
  std::vector<std::string> fontFiles;
  std::string font;
  int width = 480, height = 1080, fps = 30, count = 0, seconds = 0;
  // --history's window size, in rows: what is kept, what is written and what is restored. 0 is off.
  // The bound is not arbitrary -- it is a cap on the file as much as on the memory, and a window
  // taller than the panel could hold would restore rows nobody ever saw.
  int historyRows = 0;
  std::string historyFile;
  // optional rather than a plain string because the empty string is a real value here: it is how
  // the rule is asked to carry no label at all.
  std::optional<std::string> historyLabel;
  // The default is EntryMerger's own rather than a literal here, so the number cannot drift between
  // the two. Not clamped to a minimum: 0 is a real value meaning "no combining", so a lower bound
  // would make it unreachable exactly when someone asks for it.
  int entryMergeMs = static_cast<int>(EntryMerger::kDefaultWindowMs);
  // Same arrangement for the gift window: the default is the merger's own, so the number cannot
  // drift between the two.
  int giftMergeMs = static_cast<int>(GiftMerger::kDefaultWindowMs);
  // 0 means "not given", so an unset flag leaves PanelTokens' own default in place rather than
  // restating it here: the two would otherwise drift apart the first time one of them is edited.
  double fontSize = 0.0, cardFontSize = 0.0;
  bool verbose = false, demo = false;
  // 0 means "off", the same convention --history uses. --ws alone leaves the port at its default;
  // --ws-rtf only changes what the feed carries, so it is an error without --ws rather than a flag
  // that quietly does nothing.
  int wsPort = 0;
  bool wsRtf = false;
};

Args parseArgs(int argc, char** argv, Options& o) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return Args::Help;
    } else if (a == "--room") {
      o.room = next(i);
    } else if (a == "--cookie") {
      o.cookie = next(i);
    } else if (a == "--cookie-file") {
      o.cookieFile = next(i);
    } else if (a == "--font") {
      o.font = next(i);
    } else if (a == "--font-size") {
      // Clamped rather than trusted: a size that small makes pango refuse to lay out at all, and a
      // size that large silently produces one word per line. Both are worse than a range error.
      o.fontSize = std::clamp(std::stod(next(i)), 8.0, 200.0);
    } else if (a == "--card-font-size") {
      o.cardFontSize = std::clamp(std::stod(next(i)), 8.0, 200.0);
    } else if (a == "--font-file") {
      o.fontFiles.push_back(next(i));
    } else if (a == "--entry-merge") {
      o.entryMergeMs = std::max(0, std::stoi(next(i)));
    } else if (a == "--gift-merge") {
      o.giftMergeMs = std::max(0, std::stoi(next(i)));
    } else if (a == "--node") {
      o.node = next(i);
    } else if (a == "--desc") {
      o.desc = next(i);
    } else if (a == "--size") {
      const std::string v = next(i);
      const size_t x = v.find_first_of("xX*");
      if (x == std::string::npos) {
        o.width = o.height = std::max(64, std::stoi(v));
      } else {
        o.width = std::max(64, std::stoi(v.substr(0, x)));
        o.height = std::max(64, std::stoi(v.substr(x + 1)));
      }
    } else if (a == "--fps") {
      o.fps = std::max(1, std::stoi(next(i)));
    } else if (a == "--dump") {
      o.dump = next(i);
    } else if (a == "--count") {
      o.count = std::max(0, std::stoi(next(i)));
    } else if (a == "--history") {
      o.historyRows = std::clamp(std::stoi(next(i)), 0, 20000);
    } else if (a == "--history-file") {
      o.historyFile = next(i);
    } else if (a == "--history-label") {
      o.historyLabel = next(i);
    } else if (a == "--ws") {
      // An optional value: `--ws` takes the default port, `--ws 9000` the given one. Only a
      // digits-only next argument is consumed, so the value stays optional without a flag that
      // follows being mistaken for one.
      o.wsPort = kDefaultWsPort;
      if (i + 1 < argc && allDigits(argv[i + 1]))
        o.wsPort = std::clamp(std::stoi(argv[++i]), 1, 65535);
    } else if (a == "--ws-rtf") {
      o.wsRtf = true;
    } else if (a == "--seconds") {
      o.seconds = std::max(0, std::stoi(next(i)));
    } else if (a == "--demo") {
      o.demo = true;
    } else if (a == "--verbose" || a == "-v") {
      o.verbose = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n", a.c_str());
      usage(stderr);
      return Args::Error;
    }
  }
  return Args::Ok;
}

/** State shared between the site thread and the frame callback. */
struct Shared {
  mutable std::mutex mu;
  std::string state = "starting";
  std::string roomLabel;
  std::string lastEvent;
  std::deque<Message> pending;
  uint64_t total = 0, dropped = 0;
  /** The on-disk history, when --history is on. Null otherwise. Recorded here rather than in the
   *  frame callback on purpose: this runs whether or not a consumer is attached, so the rows are on
   *  disk even when the overlay spent the whole stream with nothing watching it. The write itself
   *  happens outside the lock below, since add() does no I/O and only says when one is due. */
  HistoryStore* history = nullptr;
  // Why a message is showing the placeholder disc. The fetch counters cannot answer this: a user
  // with no face URL is never requested, so it looks identical to a face that has not arrived yet.
  uint64_t avatarEmpty = 0, avatarHttps = 0, avatarHttp = 0, avatarProtoRel = 0, avatarOther = 0;

  /** Classifies an avatar URL by shape. The protocol-relative case matters: a bare "//host/path"
   *  is not a URL the HTTP client will accept, and it would fail in a way that looks like a network
   *  problem rather than a malformed field. */
  void noteAvatar(const std::string& url) {
    std::lock_guard<std::mutex> lk(mu);
    if (url.empty()) ++avatarEmpty;
    else if (url.rfind("https://", 0) == 0) ++avatarHttps;
    else if (url.rfind("http://", 0) == 0) ++avatarHttp;
    else if (url.rfind("//", 0) == 0) ++avatarProtoRel;
    else ++avatarOther;
  }

  void append(Message m) {
    bool flush = false;
    {
      std::lock_guard<std::mutex> lk(mu);
      ++total;
      // Recorded before the move into the queue, so the copy is made from the message as the site
      // built it rather than from what is left of it.
      if (history && m.kind != MsgKind::Paid) flush = history->add(m, steadyMs());
      pending.push_back(std::move(m));
      while (pending.size() > kPendingCap) {
        pending.pop_front();
        ++dropped;
      }
    }
    if (flush) history->save();
  }
  void note(const std::string& event) {
    std::lock_guard<std::mutex> lk(mu);
    lastEvent = event;
  }
  void setState(std::string s) {
    std::lock_guard<std::mutex> lk(mu);
    state = std::move(s);
  }
};

/** Moves at most kMaxPerFrame messages into out, so one frame's cost stays bounded however large
 *  the backlog is. The lock is held only for the moves. */
void drain(Shared& sh, std::vector<Message>& out) {
  out.clear();
  std::lock_guard<std::mutex> lk(sh.mu);
  while (!sh.pending.empty() && out.size() < kMaxPerFrame) {
    out.push_back(std::move(sh.pending.front()));
    sh.pending.pop_front();
  }
}

/** What --verbose prints in front of a message, so the log says which kind of row it was.
 *
 *  Needed since the kinds went past two: "anything that is not Text" printed [sub] for gifts,
 *  entries and likes alike, which is worse than no tag because it looks like a classification. */
const char* kindTag(MsgKind k) {
  switch (k) {
    case MsgKind::Paid: return "[paid] ";
    case MsgKind::Membership: return "[sub] ";
    case MsgKind::Gift: return "[gift] ";
    case MsgKind::Entry: return "[entry] ";
    case MsgKind::Like: return "[like] ";
    default: return "";
  }
}

void siteLoop(Shared& sh, Bili& bili, ImageStore& avatars, ImageStore& emoteImages,
              const std::string& roomInput, int entryMergeMs, int giftMergeMs, bool verbose,
              std::atomic<bool>& stop, pwvideo::VideoNode* video, std::atomic<int>& received,
              int count, WsServer* feed, bool wsRtf) {
  try {
    sh.setState("resolving room");
    const int64_t roomId = bili.resolveRoom(roomInput);
    if (verbose)
      std::fprintf(stderr, "[site] real room id: %lld\n", static_cast<long long>(roomId));

    sh.setState("fetching endpoint");
    const DanmuEndpoint ep = bili.danmuEndpoint(roomId);
    if (verbose)
      std::fprintf(stderr, "[site] endpoint: %s:%d token=%zu bytes\n", ep.host.c_str(), ep.wssPort,
                   ep.token.size());

    WsOptions opt;
    opt.host = ep.host;
    opt.port = ep.wssPort;
    opt.path = "/sub";
    opt.headers.emplace_back("User-Agent", bili.userAgent());
    opt.headers.emplace_back("Origin", "https://live.bilibili.com");
    opt.headers.emplace_back("Cookie", bili.cookieHeader());

    WsClient ws;
    sh.setState("connecting");
    ws.connect(opt);
    sh.setState("authenticating");
    // The account mid comes from the cookie's DedeUserID. Sending 0 authenticates as a guest,
    // which connects fine but is exactly why nicknames arrive masked.
    const int64_t mid = bili.accountMid();
    if (verbose)
      std::fprintf(stderr, "[site] uid=%lld (%s)\n", static_cast<long long>(mid),
                   mid ? "authenticated" : "guest");
    if (!ws.sendBinary(Bili::authPacket(roomId, ep.token, mid))) {
      sh.setState("auth send failed: " + ws.lastError());
      return;
    }

    // Wait for the reply rather than assuming it: an unauthenticated socket still receives
    // danmaku, so "connected" alone would prove nothing.
    std::string msg;
    bool authed = false;
    for (int i = 0; i < 60 && !stop.load(); ++i) {
      const WsClient::Event ev = ws.next(msg, 500);
      if (ev == WsClient::Event::Timeout) continue;
      if (ev == WsClient::Event::Error || ev == WsClient::Event::Close) {
        sh.setState("connection lost during auth: " + ws.lastError());
        return;
      }
      std::string reply;
      Bili::forEachJson(msg, [&](std::string_view doc) {
        reply.assign(doc);
        return true;
      });
      if (reply.empty()) continue;
      std::string err;
      const Json root = Json::parse(reply, &err);
      if (err.empty() && root["code"].num() == 0) {
        authed = true;
        break;
      }
      // The reply is a short JSON blob from the server and carries no user data.
      sh.setState("auth rejected: " + reply);
      return;
    }
    if (!authed) {
      sh.setState("no auth reply within 30 s");
      return;
    }
    if (verbose) std::fprintf(stderr, "[site] authenticated\n");
    sh.setState("streaming");

    // Entry notices and gifts are both folded on the way out, so both policies live with the only
    // loop that has a clock -- a row from either of them is published when a window ends, not when
    // the message that would have filled it arrives. Likes need nothing like it: they are one per
    // viewer per click, an order of magnitude slower than chat, and there is nobody to fold them
    // with.
    EntryMerger entries(entryMergeMs);
    GiftMerger gifts(giftMergeMs);
    if (verbose) {
      std::fprintf(stderr, "[site] entry notices: %s\n",
                   (entryMergeMs > 0
                        ? "combined into one row per " + std::to_string(entryMergeMs) + " ms"
                        : std::string("one row each"))
                       .c_str());
      std::fprintf(stderr, "[site] gifts: %s\n",
                   (giftMergeMs > 0
                        ? "one row per viewer and gift per " + std::to_string(giftMergeMs) + " ms"
                        : std::string("one row each"))
                       .c_str());
    }

    /** Hands one finished message to the render side. False once --count has been reached, which
     *  is the forEachJson visitor's signal to stop walking this websocket message. */
    auto publish = [&](Message m) {
      // Entry notices carry no face on purpose -- one per viewer at 1.6/s is the wrong thing to
      // spend an avatar cache on -- so they must not land in this counter. The counter answers
      // "why is a row that has an avatar box showing a placeholder disc", and an entry notice has
      // no avatar box to be wrong about.
      if (m.kind != MsgKind::Entry) sh.noteAvatar(m.avatarUrl);
      if (!m.avatarUrl.empty()) {
        // A URL we already have means two users share one picture. When that picture is the site's
        // default, several different people appear with the same flat disc, which reads as a
        // rendering fault and is not one.
        if (verbose && avatars.isCached(m.avatarUrl))
          std::fprintf(stderr, "[avatar] shared pic: %s  (user %s)\n", m.avatarUrl.c_str(),
                       m.user.c_str());
        avatars.request(m.avatarUrl);
      }
      // Emotes are fetched from the platform's own CDN URLs, and so are gift pictures: both are a
      // small set the whole room reuses all evening, unlike faces which are one per viewer. They go
      // in a separate store for exactly that reason -- a bigger cache budget, and neither evicts
      // the other.
      for (const Fragment& part : m.parts) {
        if (part.kind == Fragment::Kind::Emote && !part.url.empty()) emoteImages.request(part.url);
      }
      if (verbose)
        std::fprintf(stderr, "[msg] %s%s: %s\n", kindTag(m.kind), m.user.c_str(),
                     m.plainText().c_str());
      // The feed carries the row that reached the panel, after the mergers: a viewer who tapped a
      // gift button ten times is one row counting ten in the feed too, not ten rows the panel
      // never drew. Paid messages are included -- they are rows on screen like any other.
      if (feed) feed->broadcast(broadcastLine(m, wsRtf));
      sh.append(std::move(m));
      const int n = received.fetch_add(1) + 1;
      if (count > 0 && n >= count) {
        if (verbose) std::fprintf(stderr, "[site] reached --count %d\n", count);
        stop.store(true);
        if (video) video->quit();
        return false;
      }
      return true;
    };

    /** Publishes whatever is still batched, without waiting out the window.
     *
     *  For the paths where no further notice is ever coming -- the socket is gone, so nothing will
     *  ever close the batch. Those people did walk in, and the gifts were really sent, and the rest
     *  of the panel is still on screen showing whatever it rendered before the drop, so the rows
     *  belong in it: folding them away at that point would make the row count disagree with what
     *  the stream actually carried, which is the one thing the rows exist to be accurate about.
     *
     *  Deliberately silent when stopping: on SIGINT, --seconds or --count there is no point drawing
     *  one more row, and the notice that tripped --count has already been refused by the window. */
    auto publishPendingBatches = [&] {
      if (stop.load()) return;
      Message due;
      if (entries.flushAll(due)) publish(std::move(due));
      // A vector rather than one Message: unlike the entry notices, which fold into a single row,
      // several gift runs can come due at once -- two viewers sending different gifts are not each
      // other's duplicate. publish() can refuse the last of them at --count, and the rest of the
      // walk is then abandoned with it.
      std::vector<Message> rows;
      if (gifts.flushAll(rows)) {
        for (Message& row : rows)
          if (!publish(std::move(row))) return;
      }
    };

    // The read deadline is what drives the heartbeat timer: one thread, no second timer.
    int64_t nextHeartbeat = steadyMs() + 30000;
    while (!stop.load()) {
      const WsClient::Event ev = ws.next(msg, 1000);
      const int64_t now = steadyMs();
      if (now >= nextHeartbeat) {
        nextHeartbeat = now + 30000;
        if (!ws.sendBinary(Bili::heartbeatPacket())) {
          // A send that fails means the socket is gone, which is the same situation as a Close or
          // an Error arriving: the same batch has to go out before leaving, or the reason depends
          // on which of the three notices the socket happened to drop on.
          sh.setState("heartbeat failed: " + ws.lastError());
          publishPendingBatches();
          return;
        }
      }
      if (ev == WsClient::Event::Timeout) {
        // A read deadline is the only clock this loop has, so it is also where a finished batch gets
        // published. Waiting for the next message instead would leave the last few of a burst
        // uncounted until somebody else walked in or sent a gift, which in a quiet room could be
        // never -- the row would show up late or not at all, from a mechanism whose whole point is
        // that it does not have to wait. A row is therefore at most one read deadline (1 s) past
        // its window, which is why neither window has to be longer than that to look immediate.
        Message due;
        if (!stop.load() && entries.flush(now, due)) publish(std::move(due));
        std::vector<Message> rows;
        if (!stop.load() && gifts.flush(now, rows)) {
          for (Message& row : rows)
            if (!publish(std::move(row))) return;
        }
        continue;
      }
      if (ev == WsClient::Event::Error || ev == WsClient::Event::Close) {
        sh.setState(std::string(ev == WsClient::Event::Close ? "closed by peer" : "error") + ": " +
                    ws.lastError());
        publishPendingBatches();
        return;
      }

      Bili::forEachJson(msg, [&](std::string_view doc) {
        std::string err;
        const Json json = Json::parse(doc, &err);
        if (!err.empty()) return true;
        Message m;
        if (!Bili::parseMessage(json, m)) {
          const std::string cmd = json["cmd"].str();
          if (!cmd.empty()) sh.note(cmd);
          return true;
        }
        if (m.kind == MsgKind::Entry) {
          // Folded rather than shown: this notice may only end up inside a summary row that comes
          // later, or in one that has already been drawn, in which case it is counted into nothing
          // that is on screen yet.
          Message row;
          if (!entries.push(m, steadyMs(), row)) return true;
          return publish(std::move(row));
        }
        if (m.kind == MsgKind::Gift) {
          // Folded the same way, on a different key: this viewer sending this gift again inside the
          // window is one row counted twice, not two rows. Nothing is emitted here while a window is
          // open -- push() hands the row straight back only when there is no window or no key, which
          // is why this can hand over whatever it collected instead of a single row.
          std::vector<Message> rows;
          gifts.push(m, steadyMs(), rows);
          for (Message& row : rows)
            if (!publish(std::move(row))) return false;
          return true;
        }
        return publish(std::move(m));
      });
    }
  } catch (const std::exception& e) {
    sh.setState(std::string("failed: ") + e.what());
  }
}

/** A fixed set of messages covering every case the panel has to draw: latin and CJK names, an
 *  emoji, a line that wraps, each role colour, a paid card, a membership card, and two strings that
 *  must be rendered as literal text. The last two are the important ones: markup arriving in chat
 *  has to be drawn as glyphs, and cairo plus pango do that by construction because there is no
 *  markup interpretation anywhere in this path. */
std::vector<Message> demoMessages() {
  struct Spec {
    const char* user;
    UserType type;
    const char* text;
  };
  const Spec plain[] = {
      {"博丽灵梦", UserType::Normal, "DU↗DU→DU↗DU↓ Max Verstappen"},
      {"Jim Hacker", UserType::Moderator, "Remember... no Russian"},
      {"Makarov", UserType::Normal, "🔨⚓ 我不做人了，JOJO"},
      {"Rick Astley", UserType::Member, "🎉 让我看看"},
      {"孙悟空", UserType::Normal, "🎉 23333"},
      {"哈基米", UserType::Normal, "⚓ <img src=1 onerror=\"alert('CHECK YOUR CODE')\">"},
      {"孙悟空", UserType::Normal, "你这猴儿，真令我欢喜"},
      {"Tifa Lockhart", UserType::Moderator, "🔨🎉 Remember... no Russian"},
      {"五条悟", UserType::Normal, "<script>alert(\"CHECK YOUR CODE\")</script>"},
      {"長崎そよ", UserType::Normal, "逃げるんだよ！"},
      {"柚木つばめ", UserType::Normal, "会員的"},
      {"御剑传奇", UserType::Normal, "無駄無駄無駄無駄無駄無駄無駄無駄無駄無駄"},
  };
  std::vector<Message> out;
  for (const Spec& s : plain) {
    Message m;
    m.user = s.user;
    m.type = s.type;
    m.parts.emplace_back(Fragment{Fragment::Kind::Text, s.text, "", 0});
    out.push_back(std::move(m));
  }

  // Paid messages are not here: they live in the pinned layer, and the demo builds those separately
  // so both halves of the frame can be checked at once.
  Message sub;
  sub.kind = MsgKind::Membership;
  sub.user = "xfgryujk";
  sub.amount = "新会员";
  out.push_back(sub);

  Message after;
  after.user = "友好的益生菌";
  after.parts.emplace_back(Fragment{Fragment::Kind::Text, "弹幕姬启动", "", 0});
  out.push_back(after);

  // The three ambient kinds, interleaved with chat rather than listed at the end: the point of the
  // demo is what the column looks like with them mixed in, and a block of them at the bottom would
  // not show that. Two entries, because one row per viewer is the case that matters (no count) and
  // the combined row is the other half of it. Two gifts, one with a count, because the count is the
  // only part of that row that varies. The gift picture URL is left as it arrives in the demo: the
  // offline path has no fetcher for it, so the icon falls back to the gift name as text -- which is
  // the same thing the renderer does when the picture is merely late, and so is worth seeing here.
  Message gift;
  gift.kind = MsgKind::Gift;
  gift.user = "氧***";
  gift.parts.push_back(Fragment{Fragment::Kind::Text, "投喂 ", "", 0, true});
  gift.parts.push_back(Fragment{Fragment::Kind::Emote, "人气票", "", 0});
  out.push_back(gift);

  Message entry;
  entry.kind = MsgKind::Entry;
  entry.user = "十四的茶";
  entry.parts.push_back(Fragment{Fragment::Kind::Text, "进入了直播间", "", 0});
  out.push_back(entry);

  Message like;
  like.kind = MsgKind::Like;
  like.user = "strangeLex";
  like.parts.push_back(Fragment{Fragment::Kind::Text, "为主播点赞了", "", 0});
  out.push_back(like);

  Message giftMany;
  giftMany.kind = MsgKind::Gift;
  giftMany.user = "串***";
  giftMany.parts.push_back(Fragment{Fragment::Kind::Text, "投喂 ", "", 0, true});
  giftMany.parts.push_back(Fragment{Fragment::Kind::Emote, "小番茄", "", 0});
  giftMany.parts.push_back(Fragment{Fragment::Kind::Text, " ×6", "", 0});
  out.push_back(std::move(giftMany));

  Message entryMany;
  entryMany.kind = MsgKind::Entry;
  entryMany.user = "懿曙";
  entryMany.parts.push_back(Fragment{Fragment::Kind::Text, "等 7 人进入了直播间", "", 0});
  out.push_back(entryMany);

  // Every chat row asks for a synthetic face. Without this the demo draws no avatar at all, and a
  // missing avatar is indistinguishable from one drawn into the wrong box -- which is the whole
  // thing the red-and-green grids exist to reveal. The cards keep an empty URL, matching a live
  // paid message, which has no face. The entry notices keep an empty one as well: that is what a
  // live entry notice carries, and the row is laid out from that, not from a hypothetical face.
  int face = 0;
  for (Message& m : out) {
    if (m.kind == MsgKind::Entry || isCard(m.kind)) continue;
    m.avatarUrl = demoFaceKey(face++);
  }
  return out;
}

/** The pinned layer's demo content.
 *
 *  Two notices, because one cannot show either thing that matters here: a short one proves the card
 *  is sized to its content, and a long one proves the body wraps instead of being ellipsized. The
 *  amounts are set on amountValue as well as the display string, since the dwell is read from the
 *  number -- without it both would fall into the cheapest band and expire after a minute. */
std::vector<Message> demoNotices() {
  std::vector<Message> out;

  Message big;
  big.kind = MsgKind::Paid;
  big.user = "五条悟";
  big.amount = "CN¥500";
  big.amountValue = 500;
  // Deliberately longer than the panel is wide, and without spaces for pango to break on, so the only
  // way it can fit is by wrapping: an ellipsized card would cut this off after one line.
  big.parts.emplace_back(Fragment{
      Fragment::Kind::Text,
      "醒目留言会自动换行显示完整内容不会被省略掉所以这里故意写得很长很长很长很长很长很长很长"
      "很长很长很长很长很长很长很长很长很长很长很长很长",
      "", 0});
  out.push_back(big);

  Message small;
  small.kind = MsgKind::Paid;
  small.user = "ディオ・ブランドー";
  small.amount = "CN¥30";
  small.amountValue = 30;
  small.parts.emplace_back(Fragment{Fragment::Kind::Text, "短的一条", "", 0});
  out.push_back(small);

  return out;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  try {
    switch (parseArgs(argc, argv, o)) {
      case Args::Help: return 0;
      case Args::Error: return 2;
      case Args::Ok: break;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n\n", e.what());
    usage(stderr);
    return 2;
  }

  if (o.room.empty() && !o.demo) {
    std::fprintf(stderr, "--room is required, or --demo for the offline layout\n");
    return 2;
  }
  if (o.wsRtf && o.wsPort == 0) {
    std::fprintf(stderr, "--ws-rtf applies to the --ws feed; add --ws, optionally with a port\n");
    return 2;
  }
  if (o.demo && o.wsPort > 0)
    std::fprintf(stderr,
                 "warning: --demo draws a fixed frame and publishes nothing, so the --ws feed\n"
                 "         will accept clients and stay silent\n");
  if (!o.cookie.empty() && !o.cookieFile.empty()) {
    std::fprintf(stderr, "--cookie and --cookie-file are mutually exclusive\n");
    return 2;
  }
  if (!o.cookie.empty()) {
    // Not a refusal: the value is genuinely useful where writing a secret file is awkward. The
    // warning is about visibility to other processes, which the file option avoids.
    std::fprintf(stderr,
                 "warning: --cookie puts the session in the process arguments, where any local user\n"
                 "         can read it from ps(1). Prefer --cookie-file with a chmod 600 file.\n");
  }
  if (!o.cookieFile.empty()) {
    o.cookieFile = expandTilde(o.cookieFile);
    std::FILE* f = std::fopen(o.cookieFile.c_str(), "rb");
    if (!f) {
      // The resolved path, not the argument: the argument may have been a ~ that this process
      // expanded, and printing the unresolved form sends whoever is reading the log looking for
      // a file at a path that cannot exist.
      std::fprintf(stderr, "cannot read cookie file: %s\n", o.cookieFile.c_str());
      return 1;
    }
    char buf[8192];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    o.cookie = buf;
    // A text file usually ends in a newline; it would otherwise become part of the header value.
    while (!o.cookie.empty() && (o.cookie.back() == '\n' || o.cookie.back() == '\r'))
      o.cookie.pop_back();
  }

  BiliConfig cfg;
  cfg.cookie = o.cookie;
  Bili bili(cfg);

  PanelTokens tokens = defaultTokens();
  if (!o.font.empty()) tokens.font = o.font;
  if (o.fontSize > 0.0) {
    tokens.fontUser = o.fontSize;
    tokens.fontBody = o.fontSize;
    // An emote is a picture sitting in the text flow, so its box follows the text it sits in; left
    // at its own default it would shrink to a stamp as soon as the font grew.
    tokens.emote = o.fontSize;
    // The card keeps its step above the chat text, which is what the defaults describe: one size
    // knob for the chat and one for the cards, rather than every knob having to know about the
    // others.
    tokens.fontCardName = tokens.fontBody + 2.0;
    tokens.fontCardAmount = tokens.fontBody;
  }
  if (o.cardFontSize > 0.0) {
    tokens.fontCardName = o.cardFontSize;
    tokens.fontCardAmount = o.cardFontSize;
  }
  if (o.historyLabel) tokens.dividerLabel = *o.historyLabel;

  // --history: the last N rows are kept in this file as they arrive and painted back at the next
  // start. Off unless asked for, and off entirely when the path cannot be resolved, in which case
  // the store says nothing and simply does nothing.
  if (!o.historyFile.empty()) o.historyFile = expandTilde(o.historyFile);
  HistoryStore history(o.historyFile.empty() ? HistoryStore::defaultPath() : o.historyFile,
                       static_cast<size_t>(o.historyRows));
  history.setRoom(o.room);

  Shared sh;
  sh.roomLabel = o.demo ? std::string("demo") : o.room;
  sh.history = &history;

  // The --ws feed, bound before any thread starts so a port that is already taken is a startup
  // error rather than a background thread dying quietly behind a picture that looks fine. Loopback
  // by construction: the flag exists to hand the chat to another program on this machine, and the
  // feed has no authentication that would protect it if it were reachable from anywhere else.
  WsServer feed;
  if (o.wsPort > 0) {
    try {
      feed.listen("127.0.0.1", o.wsPort);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "cannot serve the --ws feed: %s\n", e.what());
      return 1;
    }
    feed.setVerbose(o.verbose);
  }

  // The panel is built before the image stores because the stores have to be told how large to
  // decode: the avatar box is one line of body text, so it is only known once the font is measured,
  // and a face decoded at the wrong size is either soft or needlessly large in memory.
  Panel panel(tokens);
  panel.resize(o.width, o.height);
  // Paid messages do not go into the scrolling panel; they are held at the top of the frame on their
  // own clock. Both layers read the same tokens and are drawn in this order, so a notice sits over
  // the chat rather than being scrolled by it.
  PinnedLayer pinned(tokens);
  pinned.resize(o.width, o.height);

  // Decoded at twice the box, so the face stays crisp on a hidpi canvas without storing four times
  // the pixels it needs. The capacity is separate and only bounds memory: a busy room shows roughly
  // 36 rows, and a visible row's avatar has to outlive the messages that push it off.
  const int facePx = std::clamp(static_cast<int>(panel.avatarBox() * 2.0), 32, 256);
  ImageStore avatars(facePx, 256, bili.userAgent());
  // Emotes are a small closed set reused by everyone, so they are worth far more entries than a
  // face is; 48 px is enough at the 24 px box they are drawn into.
  ImageStore emoteImages(48, 512, bili.userAgent());
  panel.setImageStore(&avatars);
  panel.setEmoteStore(&emoteImages);

  // The previous run's rows, painted before anything else arrives so the rule lands between them and
  // the first live row rather than under it. --demo is excluded: it is a fixed layout fixture, and a
  // run that draws nothing but itself must not write or read a history either.
  //
  //  Below the stores rather than above them, because a restored row is a real row and carries real
  //  asset URLs, and nothing else will ever ask for them: the only request() calls in the program are
  //  in publish(), which sees live traffic only. Left unrequested, every restored face stayed a
  //  placeholder disc -- the most visible row type in the panel, all of them grey.
  //
  //  Avatars are fixed by this alone, because drawAvatars() re-reads the store on every frame and a
  //  face that lands after the row was laid out shows up on the next one. Emote and gift pictures are
  //  baked into the layer by paintRow() and are not repainted, so a restored row whose picture has
  //  not arrived by this line keeps drawing the token as text until something shifts the list. That
  //  is the same bargain a live row makes, and requesting here is what lets a warm cache serve the
  //  common case; making the layer repaint itself is a larger change than this fix.
  size_t restored = 0;
  if (!o.demo && history.enabled()) {
    const std::vector<Message> past = history.restore();
    if (!past.empty()) {
      for (const Message& m : past) {
        if (!m.avatarUrl.empty()) avatars.request(m.avatarUrl);
        for (const Fragment& f : m.parts) {
          if (f.kind == Fragment::Kind::Emote && !f.url.empty()) emoteImages.request(f.url);
        }
      }
      panel.rebuildLayer(past, true);
      restored = past.size();
      std::fprintf(stderr, "[history] restored %zu row(s) from %s\n", restored,
                   history.path().c_str());
    }
  }

  /** The demo's synthetic faces, keyed the way demoMessages() asks for them. Installed only for
   *  --demo: in a live run the store is the source of truth and this table stays empty, so a real
   *  face can never be shadowed by a test pattern. */
  auto installDemoFaces = [&] {
    std::map<std::string, pwvideo::SurfacePtr> table;
    const std::vector<pwvideo::SurfacePtr> faces = demoFaces();
    for (size_t i = 0; i < faces.size(); ++i)
      table[demoFaceKey(static_cast<int>(i))] = faces[i];
    panel.setDemoFaces(std::move(table));
  };
  pwvideo::CairoFrame frame(o.width, o.height);

  // Reused across frames so the render path does not allocate.
  std::vector<Message> fresh;
  // Each batch is split by destination: paid messages go to the pinned layer and everything else to
  // the scrolling panel. A paid message left in the list would scroll off within a second, which is
  // the opposite of what it is for.
  std::vector<Message> chat, paid;

  auto drawOnce = [&](int64_t now) {
    drain(sh, fresh);
    chat.clear();
    paid.clear();
    for (const Message& m : fresh) {
      if (m.kind == MsgKind::Paid)
        paid.push_back(m);
      else
        chat.push_back(m);
    }
    // Updated before the panel so a notice that arrives on this frame is on it, not the next one.
    // render() then retires anything whose dwell ran out, which needs no new message to happen.
    pinned.update(frame.cr(), paid, now);
    panel.update(chat, now);
    panel.render(frame.cr(), now);
    pinned.render(frame.cr(), now);
  };

  /** Renders a short run and leaves the settled state in the frame.
   *
   *  The entrance animation lasts --animMs, so a single frame taken at the instant a message lands
   *  samples it at alpha 0 and shows nothing at all. A viewer never sees that; they see the row
   *  after the fade. This is the same reasoning the sibling project applies to its own stateful
   *  effects: advance the clock past the transient and keep the last frame. */
  auto settleForDump = [&](int frames = 12) {
    const int64_t t0 = steadyMs();
    for (int i = 0; i < frames; ++i) {
      panel.render(frame.cr(), t0 + (i + 1) * 33);
      pinned.render(frame.cr(), t0 + (i + 1) * 33);
    }
  };

  if (o.demo) {
    // Everything is known up front, so paint the list in one go rather than row by row.
    sh.setState("demo (no network)");
    installDemoFaces();
    panel.rebuildLayer(demoMessages());
    // Handed to the pinned layer all at once, the way a live batch arrives. Its dwell is minutes to
    // hours, so nothing here expires during a dump.
    pinned.update(frame.cr(), demoNotices(), steadyMs());
    if (!o.dump.empty()) {
      panel.render(frame.cr(), steadyMs());
      pinned.render(frame.cr(), steadyMs());
      settleForDump();
      if (!frame.writePng(o.dump)) {
        std::fprintf(stderr, "PNG write failed: %s\n", o.dump.c_str());
        return 1;
      }
      std::printf("Wrote %s (%dx%d)\n", o.dump.c_str(), o.width, o.height);
      return 0;
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<int> received{0};

  if (o.dump.empty()) {
    try {
      pwvideo::Options opt;
      opt.width = o.width;
      opt.height = o.height;
      opt.fpsCap = o.fps;
      opt.nodeName = o.node;
      opt.nodeDescription = o.desc;
      opt.appName = "pw-live-danmaku";
      opt.verbose = o.verbose;

      pwvideo::VideoNode video(opt, [&](uint8_t* dst, int stride, int w, int h) {
        drawOnce(steadyMs());
        // The negotiated w/h may be smaller than the frame; blitTo clips, which is why the OBS
        // source size has to match --size rather than being free to differ.
        frame.blitTo(dst, stride, w, h);
      });
      video.start();

      std::printf(
          "pw-live-danmaku (native) started\n"
          "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
          "  Size: %dx%d @ %d fps\n",
          o.node.c_str(), o.width, o.height, o.fps);
      if (o.demo)
        std::printf("  Mode:  demo (no network)\n");
      else
        std::printf("  Room:  %s\n  Auth:  %s\n", o.room.c_str(),
                    o.cookie.empty() ? "anonymous (nicknames masked)" : "cookie");
      if (history.enabled())
        std::printf("  History: %s  [window %d rows, %zu restored]\n", history.path().c_str(),
                    o.historyRows, restored);
      if (o.wsPort > 0)
        std::printf("  WebSocket: ws://127.0.0.1:%d/  [%s, one text frame per message]\n",
                    feed.port(), o.wsRtf ? "Resonite RTF" : "avatar URL, name, body");
      std::fflush(stdout);

      std::thread avatarThread([&avatars] { avatars.runWorker(); });
      std::thread emoteThread([&emoteImages] { emoteImages.runWorker(); });

      // Before the site thread, so the feed is accepting by the time the first message is
      // published; the accept loop and the render loop are independent threads and neither waits
      // on the other.
      std::thread feedThread;
      if (o.wsPort > 0) feedThread = std::thread([&feed] { feed.run(); });

      std::thread site;
      if (!o.demo) {
        site = std::thread(siteLoop, std::ref(sh), std::ref(bili), std::ref(avatars),
                           std::ref(emoteImages), o.room, o.entryMergeMs, o.giftMergeMs, o.verbose,
                           std::ref(stop), &video, std::ref(received), o.count,
                           o.wsPort > 0 ? &feed : nullptr, o.wsRtf);
      }

      std::thread killer;
      if (o.seconds > 0) {
        killer = std::thread([&] {
          const int64_t until = steadyMs() + static_cast<int64_t>(o.seconds) * 1000LL;
          while (!stop.load() && steadyMs() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          if (!stop.load()) {
            std::fprintf(stderr, "[site] reached --seconds %d\n", o.seconds);
            stop.store(true);
            video.quit();
          }
        });
      }

      // Avatar bookkeeping is invisible from the picture: a message whose avatar has not arrived
      // yet looks exactly like one whose user has no picture. --verbose prints the counters and the
      // last failing URL so the two can be told apart.
      std::thread stats;
      if (o.verbose) {
        stats = std::thread([&] {
          while (!stop.load()) {
            for (int i = 0; i < 10 && !stop.load(); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (stop.load()) break;
            const std::string err = avatars.lastError();
            std::fprintf(stderr,
                         "[avatar] cached=%zu pending=%zu fetched=%llu failed=%llu%s%s\n"
                         "[avatar] url shapes: empty=%llu https=%llu http=%llu proto-rel=%llu other=%llu\n",
                         avatars.cached(), avatars.pending(),
                         static_cast<unsigned long long>(avatars.fetched()),
                         static_cast<unsigned long long>(avatars.failed()),
                         err.empty() ? "" : "  last-fail: ", err.c_str(),
                         static_cast<unsigned long long>(sh.avatarEmpty),
                         static_cast<unsigned long long>(sh.avatarHttps),
                         static_cast<unsigned long long>(sh.avatarHttp),
                         static_cast<unsigned long long>(sh.avatarProtoRel),
                         static_cast<unsigned long long>(sh.avatarOther));
            if (o.wsPort > 0)
              std::fprintf(stderr, "[ws] clients=%zu messages=%llu dropped=%llu\n", feed.clients(),
                           static_cast<unsigned long long>(feed.messages()),
                           static_cast<unsigned long long>(feed.dropped()));
          }
        });
      }
      video.run();  // Blocks until SIGINT/SIGTERM, --count, --seconds, or the site loop gives up
      stop.store(true);
      if (site.joinable()) site.join();
      // After the site thread: nothing else calls broadcast(), and a feed still running would be
      // one more thread holding a socket open while the process unwinds.
      feed.stop();
      if (feedThread.joinable()) feedThread.join();
      avatars.stop();
      emoteImages.stop();
      if (avatarThread.joinable()) avatarThread.join();
      if (emoteThread.joinable()) emoteThread.join();
      if (stats.joinable()) stats.join();
      if (killer.joinable()) killer.join();
      // After the joins, so the last thing the site thread recorded is on disk rather than lost to a
      // clean exit: the throttle means the tail of a quiet room can be a few rows deep.
      history.save();
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "startup failed: %s\n", e.what());
      return 1;
    }
  }

  // --dump against a live room: no PipeWire node at all, because with no consumer attached the
  // render callback is never invoked and the PNG would come out blank. The avatar worker runs here
  // too, so the dump shows real faces instead of placeholder discs -- which is what makes it useful
  // for judging a layout rather than only for reading the connection state.
  std::thread avatarThread([&avatars] { avatars.runWorker(); });
  std::thread emoteThread([&emoteImages] { emoteImages.runWorker(); });
  std::thread feedThread;
  if (o.wsPort > 0) feedThread = std::thread([&feed] { feed.run(); });
  std::thread site(siteLoop, std::ref(sh), std::ref(bili), std::ref(avatars), std::ref(emoteImages),
                   o.room, o.entryMergeMs, o.giftMergeMs, o.verbose, std::ref(stop),
                   static_cast<pwvideo::VideoNode*>(nullptr), std::ref(received),
                   o.count > 0 ? o.count : kDumpMessages, o.wsPort > 0 ? &feed : nullptr, o.wsRtf);
  if (o.wsPort > 0)
    std::printf("WebSocket: ws://127.0.0.1:%d/  [%s]\n", feed.port(),
                o.wsRtf ? "Resonite RTF" : "avatar URL, name, body");
  const int64_t until = steadyMs() + 90000;
  // Wait for a few messages rather than just the first: one row is not enough to judge a layout,
  // and a panel with real avatars in it needs more than one row to show them.
  const int want = o.count > 0 ? o.count : kDumpMessages;
  while (!stop.load() && received.load() < want && steadyMs() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);
  site.join();
  // After the site thread, the same as the long-running path: nothing else broadcasts.
  feed.stop();
  if (feedThread.joinable()) feedThread.join();
  // Same reasoning as the long-running path: the dump collected real rows, so they are worth
  // keeping even though this run was only meant to produce one PNG.
  history.save();
  // Bounded wait for the fetches already queued; a slow CDN must not hang the dump. The condition is
  // "there is still a backlog", not "nothing has arrived yet" -- the latter returns after the
  // first avatar and leaves the rest as placeholder discs.
  const int64_t avatarUntil = steadyMs() + 8000;
  while ((avatars.pending() > 0 || emoteImages.pending() > 0) && steadyMs() < avatarUntil)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  avatars.stop();
  emoteImages.stop();
  if (avatarThread.joinable()) avatarThread.join();
  if (emoteThread.joinable()) emoteThread.join();
  // pending() clears when the worker picks a URL up, so allow the last in-flight one to land.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  drawOnce(steadyMs());
  settleForDump();
  if (!frame.writePng(o.dump)) {
    std::fprintf(stderr, "PNG write failed: %s\n", o.dump.c_str());
    return 1;
  }
  std::printf("Wrote %s (%dx%d), received %d messages\n", o.dump.c_str(), o.width, o.height,
              received.load());
  return 0;
}