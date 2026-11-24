#include "server.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "net.h"
#include "protocol.h"

namespace schat {
namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

constexpr int64_t kIdleTimeoutMs = 120000;   // drop silent clients after 2 min
constexpr int64_t kHelloTimeoutMs = 15000;   // must say HELLO within 15 s
constexpr double kBurst = 12, kRefillPerSec = 3;  // flood control
constexpr size_t kMaxOutBuf = 64u * 1024 * 1024;
constexpr size_t kMaxRooms = 256;

struct Conn {
  int fd = -1;
  std::string addr;
  std::string nick;  // empty until HELLO accepted
  LineBuffer in;
  std::string out;
  std::set<std::string> rooms;
  int64_t connectedMs = 0, lastSeenMs = 0, joinedAt = 0;
  double tokens = kBurst;
  int64_t lastRefill = 0;
  bool closing = false;  // flush output, then close
  std::string away;
};

struct Room {
  std::string topic;
  std::string topicBy;
  std::set<int> members;
  std::deque<std::string> history;  // encoded lines
};

class Server {
 public:
  explicit Server(const ServerOptions& o) : opt_(o) {}
  int run();

 private:
  void log(const std::string& line);
  void console(const std::string& s);  // print to server console
  void accept();
  void readFrom(Conn& c);
  void writeTo(Conn& c);
  void handle(Conn& c, const std::vector<std::string>& f);
  void handleConsole(const std::string& line);
  void handleDiscovery();
  void drop(int fd, const std::string& reason);
  void send(Conn& c, const std::vector<std::string>& f) { queue(c, encode(f)); }
  void queue(Conn& c, const std::string& line);
  void err(Conn& c, const std::string& code, const std::string& msg) { send(c, {"ERR", code, msg}); }
  void info(Conn& c, const std::string& msg) { send(c, {"INFO", msg}); }
  void toRoom(const std::string& room, const std::string& line, int except = -1);
  void toAllSharing(Conn& c, const std::string& line, bool includeSelf);
  void join(Conn& c, const std::string& room);
  void part(Conn& c, const std::string& room, const std::string& reason, bool quit);
  void addHistory(Room& r, const std::string& line);
  bool allow(Conn& c);
  Conn* byNick(const std::string& nick);
  std::string uniqueNick(const std::string& want);
  size_t userCount() const;
  void sendWho(Conn& c, const std::string& room);
  void sendRooms(Conn& c);

  ServerOptions opt_;
  int listenFd_ = -1, udpFd_ = -1;
  bool stdinOpen_ = true;
  std::map<int, std::unique_ptr<Conn>> conns_;
  std::map<std::string, Room> rooms_;
  std::ofstream logf_;
  int64_t started_ = 0;
  LineBuffer stdinBuf_;
};

void Server::console(const std::string& s) {
  std::printf("%s %s\n", clockStr(nowSec()).c_str(), s.c_str());
  std::fflush(stdout);
}

void Server::log(const std::string& line) {
  if (logf_.is_open()) {
    logf_ << '[' << dateTimeStr(nowSec()) << "] " << line << '\n';
    logf_.flush();
  }
}

size_t Server::userCount() const {
  size_t n = 0;
  for (auto& [fd, c] : conns_)
    if (!c->nick.empty()) ++n;
  return n;
}

Conn* Server::byNick(const std::string& nick) {
  std::string l = lower(nick);
  for (auto& [fd, c] : conns_)
    if (!c->nick.empty() && lower(c->nick) == l) return c.get();
  return nullptr;
}

std::string Server::uniqueNick(const std::string& want) {
  if (!byNick(want) && lower(want) != "server") return want;
  for (int i = 2;; ++i) {
    std::string base = want.substr(0, 20 - std::to_string(i).size() - 1);
    std::string n = base + "_" + std::to_string(i);
    if (!byNick(n)) return n;
  }
}

void Server::queue(Conn& c, const std::string& line) {
  if (c.fd < 0) return;
  c.out += line;
  if (c.out.size() > kMaxOutBuf) {
    c.out.clear();
    c.closing = true;  // too slow to keep up; drop
  }
}

void Server::toRoom(const std::string& room, const std::string& line, int except) {
  auto it = rooms_.find(room);
  if (it == rooms_.end()) return;
  for (int fd : it->second.members)
    if (fd != except) queue(*conns_[fd], line);
}

void Server::toAllSharing(Conn& c, const std::string& line, bool includeSelf) {
  std::set<int> targets;
  for (auto& r : c.rooms)
    for (int fd : rooms_[r].members) targets.insert(fd);
  if (includeSelf) targets.insert(c.fd); else targets.erase(c.fd);
  for (int fd : targets) queue(*conns_[fd], line);
}

void Server::addHistory(Room& r, const std::string& line) {
  if (opt_.historySize == 0) return;
  r.history.push_back(line);
  while (r.history.size() > opt_.historySize) r.history.pop_front();
}

bool Server::allow(Conn& c) {
  int64_t now = nowMs();
  c.tokens = std::min(kBurst, c.tokens + (now - c.lastRefill) / 1000.0 * kRefillPerSec);
  c.lastRefill = now;
  if (c.tokens < 1) return false;
  c.tokens -= 1;
  return true;
}

void Server::sendWho(Conn& c, const std::string& room) {
  auto it = rooms_.find(room);
  if (it == rooms_.end()) { err(c, "NOROOM", "No such room " + room); return; }
  std::vector<std::string> f = {"WHO", room};
  for (int fd : it->second.members) {
    auto& m = *conns_[fd];
    f.push_back(m.nick + (m.away.empty() ? "" : " (away)"));
  }
  send(c, f);
}

void Server::sendRooms(Conn& c) {
  std::vector<std::string> f = {"ROOMS"};
  for (auto& [name, r] : rooms_)
    f.push_back(name + " " + std::to_string(r.members.size()) + " " + r.topic);
  send(c, f);
}

void Server::join(Conn& c, const std::string& room) {
  if (!validRoom(room)) { err(c, "BADROOM", "Room names look like #name (a-z, 0-9, _ -, max 24)"); return; }
  if (c.rooms.count(room)) { send(c, {"SWITCH", room}); return; }
  if (!rooms_.count(room) && rooms_.size() >= kMaxRooms) { err(c, "FULL", "Too many rooms on this server"); return; }
  if (c.rooms.size() >= 20) { err(c, "FULL", "You are in too many rooms"); return; }
  Room& r = rooms_[room];
  r.members.insert(c.fd);
  c.rooms.insert(room);
  toRoom(room, encode({"JOINED", room, std::to_string(nowSec()), c.nick}));
  if (!r.history.empty()) {
    send(c, {"HISTORY_BEGIN", room, std::to_string(r.history.size())});
    for (auto& h : r.history) queue(c, h);
    send(c, {"HISTORY_END", room});
  }
  if (!r.topic.empty()) send(c, {"TOPIC", room, r.topicBy, r.topic});
  sendWho(c, room);
  log(room + " -> " + c.nick + " joined");
}

void Server::part(Conn& c, const std::string& room, const std::string& reason, bool quit) {
  auto it = rooms_.find(room);
  if (it == rooms_.end() || !c.rooms.count(room)) {
    if (!quit) err(c, "NOTIN", "You are not in " + room);
    return;
  }
  if (!quit) toRoom(room, encode({"PARTED", room, std::to_string(nowSec()), c.nick, reason}));
  it->second.members.erase(c.fd);
  c.rooms.erase(room);
  // Forget empty rooms that never had any history, except the lobby.
  if (it->second.members.empty() && it->second.history.empty() && room != kDefaultRoom) rooms_.erase(it);
  if (!quit) log(room + " <- " + c.nick + " left (" + reason + ")");
}

void Server::drop(int fd, const std::string& reason) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  Conn& c = *it->second;
  if (!c.nick.empty()) {
    toAllSharing(c, encode({"QUIT", std::to_string(nowSec()), c.nick, reason}), false);
    for (auto r : std::set<std::string>(c.rooms)) part(c, r, reason, true);
    console(c.nick + " (" + c.addr + ") disconnected: " + reason);
    log(c.nick + " disconnected: " + reason);
  }
  ::close(fd);
  conns_.erase(it);
}

void Server::accept() {
  sockaddr_in a{};
  socklen_t l = sizeof a;
  int fd = ::accept(listenFd_, (sockaddr*)&a, &l);
  if (fd < 0) return;
  if (conns_.size() >= opt_.maxClients) {
    sendAll(fd, encode({"ERR", "FULL", "Server is full"}));
    ::close(fd);
    return;
  }
  setNonBlocking(fd);
  auto c = std::make_unique<Conn>();
  c->fd = fd;
  c->addr = peerAddress(fd);
  c->connectedMs = c->lastSeenMs = c->lastRefill = nowMs();
  conns_[fd] = std::move(c);
}

void Server::readFrom(Conn& c) {
  char buf[65536];
  while (true) {
    ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    if (n > 0) {
      c.in.append(buf, static_cast<size_t>(n));
      c.lastSeenMs = nowMs();
      if (n < (ssize_t)sizeof buf) break;
      continue;
    }
    if (n == 0) { drop(c.fd, "connection closed"); return; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
    drop(c.fd, "connection error");
    return;
  }
  if (c.in.overflow()) { drop(c.fd, "message too large"); return; }
  int fd = c.fd;
  std::string line;
  while (conns_.count(fd) && c.in.next(line)) {
    if (line.empty()) continue;
    handle(c, decode(line));
    if (!conns_.count(fd) || c.closing) break;
  }
}

void Server::writeTo(Conn& c) {
  while (!c.out.empty()) {
    ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
    if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    drop(c.fd, "write error");
    return;
  }
}

void Server::handle(Conn& c, const std::vector<std::string>& f) {
  const std::string& cmd = f[0];
  auto arg = [&](size_t i) -> std::string { return i < f.size() ? f[i] : std::string(); };

  if (cmd == "PING") { send(c, {"PONG", arg(1)}); return; }

  if (c.nick.empty()) {
    if (cmd != "HELLO") { err(c, "AUTH", "Say HELLO first"); c.closing = true; return; }
    // HELLO <protocol> <nick> <password>
    if (std::atoi(arg(1).c_str()) != kProtocolVersion) {
      err(c, "VERSION", "Protocol mismatch: server speaks v" + std::to_string(kProtocolVersion));
      c.closing = true; return;
    }
    if (!opt_.password.empty() && arg(3) != opt_.password) {
      err(c, "PASSWORD", "Wrong password");
      console("rejected " + c.addr + ": wrong password");
      c.closing = true; return;
    }
    if (!validNick(arg(2))) {
      err(c, "BADNICK", "Nicknames: 1-20 chars of A-Z a-z 0-9 _ -");
      c.closing = true; return;
    }
    c.nick = uniqueNick(arg(2));
    c.joinedAt = nowSec();
    send(c, {"WELCOME", c.nick, opt_.name, opt_.motd, kVersion, std::to_string(userCount())});
    console(c.nick + " connected from " + c.addr);
    log(c.nick + " connected from " + c.addr);
    return;
  }

  bool chatty = cmd == "MSG" || cmd == "ME" || cmd == "PRIV" || cmd == "FILE" || cmd == "NICK" ||
                cmd == "TOPIC" || cmd == "JOIN";
  if (chatty && !allow(c)) { err(c, "FLOOD", "You are sending too fast - slow down"); return; }

  if (cmd == "MSG" || cmd == "ME") {  // MSG <room> <text>
    std::string room = arg(1), text = arg(2);
    if (!c.rooms.count(room)) { err(c, "NOTIN", "You are not in " + room); return; }
    if (text.empty()) return;
    if (text.size() > kMaxText) { err(c, "TOOLONG", "Message too long (max " + std::to_string(kMaxText) + ")"); return; }
    std::string line = encode({cmd, room, std::to_string(nowSec()), c.nick, text});
    addHistory(rooms_[room], line);
    toRoom(room, line);
    log(room + (cmd == "ME" ? " * " + c.nick + " " : " <" + c.nick + "> ") + text);
  } else if (cmd == "PRIV") {  // PRIV <nick> <text>
    Conn* t = byNick(arg(1));
    if (!t) { err(c, "NONICK", "No such user " + arg(1)); return; }
    if (arg(2).empty() || arg(2).size() > kMaxText) return;
    std::string line = encode({"PRIV", std::to_string(nowSec()), c.nick, t->nick, arg(2)});
    queue(*t, line);
    if (t != &c) queue(c, line);
    if (!t->away.empty()) info(c, t->nick + " is away: " + t->away);
  } else if (cmd == "JOIN") {
    join(c, normalizeRoom(arg(1)));
  } else if (cmd == "PART") {
    part(c, normalizeRoom(arg(1)), arg(2).empty() ? "left" : arg(2), false);
    send(c, {"PARTOK", normalizeRoom(arg(1))});
  } else if (cmd == "NICK") {
    std::string want = arg(1);
    if (!validNick(want)) { err(c, "BADNICK", "Nicknames: 1-20 chars of A-Z a-z 0-9 _ -"); return; }
    Conn* other = byNick(want);
    if ((other && other != &c) || lower(want) == "server") { err(c, "NICKUSED", want + " is already taken"); return; }
    std::string old = c.nick;
    c.nick = want;
    toAllSharing(c, encode({"NICK", old, want}), true);
    log(old + " is now known as " + want);
  } else if (cmd == "WHO") {
    sendWho(c, normalizeRoom(arg(1)));
  } else if (cmd == "ROOMS") {
    sendRooms(c);
  } else if (cmd == "USERS") {
    std::vector<std::string> out = {"USERS"};
    for (auto& [fd, o] : conns_)
      if (!o->nick.empty()) out.push_back(o->nick + (o->away.empty() ? "" : " (away)"));
    send(c, out);
  } else if (cmd == "WHOIS") {
    Conn* t = byNick(arg(1));
    if (!t) { err(c, "NONICK", "No such user " + arg(1)); return; }
    std::string rooms;
    for (auto& r : t->rooms) rooms += r + " ";
    send(c, {"WHOIS", t->nick, t->addr, std::to_string(t->joinedAt), rooms, t->away});
  } else if (cmd == "TOPIC") {  // TOPIC <room> [text]
    std::string room = normalizeRoom(arg(1));
    if (!c.rooms.count(room)) { err(c, "NOTIN", "You are not in " + room); return; }
    if (f.size() < 3) {
      auto& r = rooms_[room];
      if (r.topic.empty()) info(c, room + " has no topic");
      else send(c, {"TOPIC", room, r.topicBy, r.topic});
      return;
    }
    if (arg(2).size() > 300) { err(c, "TOOLONG", "Topic too long"); return; }
    rooms_[room].topic = arg(2);
    rooms_[room].topicBy = c.nick;
    toRoom(room, encode({"TOPIC", room, c.nick, arg(2)}));
    log(room + " topic set by " + c.nick + ": " + arg(2));
  } else if (cmd == "AWAY") {
    c.away = arg(1);
    info(c, c.away.empty() ? "You are back" : "You are marked away: " + c.away);
  } else if (cmd == "FILE") {  // FILE <nick> <name> <size> <base64>
    Conn* t = byNick(arg(1));
    if (!t) { err(c, "NONICK", "No such user " + arg(1)); return; }
    if (arg(4).size() > kMaxFile / 3 * 4 + 8) { err(c, "TOOLONG", "File too large"); return; }
    queue(*t, encode({"FILE", std::to_string(nowSec()), c.nick, arg(2), arg(3), arg(4)}));
    info(c, "Sent " + arg(2) + " (" + humanSize(std::strtoull(arg(3).c_str(), nullptr, 10)) + ") to " + t->nick);
    log(c.nick + " sent file " + arg(2) + " to " + t->nick);
  } else if (cmd == "QUIT") {
    drop(c.fd, arg(1).empty() ? "quit" : "quit: " + arg(1));
  } else {
    err(c, "UNKNOWN", "Unknown command " + cmd);
  }
}

void Server::handleDiscovery() {
  char buf[512];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  ssize_t n = ::recvfrom(udpFd_, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
  if (n <= 0) return;
  std::string line(buf, static_cast<size_t>(n));
  if (line.rfind("SCHAT_DISCOVER", 0) != 0) return;
  std::string reply = encode({"SCHAT_HERE", std::to_string(kProtocolVersion), opt_.name,
                              std::to_string(opt_.port), std::to_string(userCount()),
                              opt_.password.empty() ? "0" : "1", kVersion});
  ::sendto(udpFd_, reply.data(), reply.size(), 0, (sockaddr*)&from, fl);
}

void Server::handleConsole(const std::string& raw) {
  std::string line = raw;
  if (line.empty()) return;
  std::string cmd = line, rest;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) { cmd = line.substr(0, sp); rest = line.substr(sp + 1); }
  if (cmd == "/help") {
    console("Console commands:\n"
            "  /users                  list connected users\n"
            "  /rooms                  list rooms\n"
            "  /say <text>             announce to everyone\n"
            "  /kick <nick> [reason]   disconnect a user\n"
            "  /topic <#room> <text>   set a room topic\n"
            "  /motd <text>            change the message of the day\n"
            "  /stats                  uptime and counters\n"
            "  /quit                   shut the server down");
  } else if (cmd == "/users") {
    for (auto& [fd, c] : conns_)
      if (!c->nick.empty()) {
        std::string rs;
        for (auto& r : c->rooms) rs += r + " ";
        console("  " + c->nick + "  " + c->addr + "  " + rs);
      }
    console(std::to_string(userCount()) + " user(s)");
  } else if (cmd == "/rooms") {
    for (auto& [n, r] : rooms_)
      console("  " + n + "  " + std::to_string(r.members.size()) + " user(s)  " + r.topic);
  } else if (cmd == "/say") {
    std::string l = encode({"ANNOUNCE", std::to_string(nowSec()), rest});
    for (auto& [fd, c] : conns_) if (!c->nick.empty()) queue(*c, l);
    log("ANNOUNCE " + rest);
    console("announced: " + rest);
  } else if (cmd == "/kick") {
    std::string nick = rest, reason = "kicked by server";
    size_t s2 = rest.find(' ');
    if (s2 != std::string::npos) { nick = rest.substr(0, s2); reason = "kicked: " + rest.substr(s2 + 1); }
    Conn* t = byNick(nick);
    if (!t) { console("no such user " + nick); return; }
    int fd = t->fd;
    err(*t, "KICKED", reason);
    writeTo(*t);
    drop(fd, reason);
  } else if (cmd == "/topic") {
    size_t s2 = rest.find(' ');
    std::string room = normalizeRoom(rest.substr(0, s2));
    std::string text = s2 == std::string::npos ? "" : rest.substr(s2 + 1);
    if (!validRoom(room)) { console("bad room"); return; }
    rooms_[room].topic = text;
    rooms_[room].topicBy = "server";
    toRoom(room, encode({"TOPIC", room, "server", text}));
  } else if (cmd == "/motd") {
    opt_.motd = rest;
    console("motd set");
  } else if (cmd == "/stats") {
    int64_t up = nowSec() - started_;
    console("uptime " + std::to_string(up / 3600) + "h " + std::to_string(up / 60 % 60) + "m, " +
            std::to_string(conns_.size()) + " connection(s), " + std::to_string(rooms_.size()) + " room(s)");
  } else if (cmd == "/quit") {
    g_stop = 1;
  } else {
    console("unknown command, try /help");
  }
}

int Server::run() {
  std::string e;
  listenFd_ = tcpListen(opt_.port, opt_.bindAddr, e);
  if (listenFd_ < 0) {
    std::fprintf(stderr, "error: cannot listen on port %u: %s\n", opt_.port, e.c_str());
    return 1;
  }
  setNonBlocking(listenFd_);
  if (opt_.discovery) {
    udpFd_ = udpSocket(opt_.discoveryPort, false, e);
    if (udpFd_ < 0)
      std::fprintf(stderr, "warning: discovery disabled (UDP port %u: %s)\n", opt_.discoveryPort, e.c_str());
  }
  if (!opt_.logFile.empty()) {
    logf_.open(opt_.logFile, std::ios::app);
    if (!logf_) std::fprintf(stderr, "warning: cannot open log file %s\n", opt_.logFile.c_str());
  }
  std::signal(SIGINT, onStop);
  std::signal(SIGTERM, onStop);
  std::signal(SIGPIPE, SIG_IGN);
  started_ = nowSec();
  rooms_[kDefaultRoom];

  const char* b = opt_.color ? "\033[1;32m" : "";
  const char* r = opt_.color ? "\033[0m" : "";
  std::printf("%sschat server \"%s\" v%s%s\n", b, opt_.name.c_str(), kVersion, r);
  std::printf("  listening on TCP port %u%s\n", opt_.port,
              opt_.password.empty() ? "" : " (password required)");
  auto ips = localIPv4Addresses();
  for (auto& ip : ips) std::printf("  connect with:  schat client %s:%u\n", ip.c_str(), opt_.port);
  if (udpFd_ >= 0) std::printf("  LAN discovery on UDP port %u (clients can just run: schat client)\n", opt_.discoveryPort);
  std::printf("  type /help for console commands, Ctrl-C to stop\n\n");
  std::fflush(stdout);
  log("server started on port " + std::to_string(opt_.port));

  while (!g_stop) {
    std::vector<pollfd> pfds;
    pfds.push_back({listenFd_, POLLIN, 0});
    if (udpFd_ >= 0) pfds.push_back({udpFd_, POLLIN, 0});
    if (stdinOpen_) pfds.push_back({STDIN_FILENO, POLLIN, 0});
    size_t base = pfds.size();
    for (auto& [fd, c] : conns_)
      pfds.push_back({fd, static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});

    int n = pollFds(pfds.data(), pfds.size(), 1000);
    if (n < 0 && errno != EINTR) break;

    for (size_t i = 0; i < base; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP))) continue;
      if (pfds[i].fd == listenFd_) accept();
      else if (pfds[i].fd == udpFd_) handleDiscovery();
      else {
        char buf[4096];
        ssize_t k = ::read(STDIN_FILENO, buf, sizeof buf);
        if (k <= 0) { stdinOpen_ = false; continue; }
        stdinBuf_.append(buf, static_cast<size_t>(k));
        std::string l;
        while (stdinBuf_.next(l)) handleConsole(l);
      }
    }
    for (size_t i = base; i < pfds.size(); ++i) {
      int fd = pfds[i].fd;
      if (!conns_.count(fd)) continue;
      if (pfds[i].revents & POLLIN) readFrom(*conns_[fd]);
      if (!conns_.count(fd)) continue;
      if (pfds[i].revents & (POLLERR | POLLNVAL)) { drop(fd, "connection error"); continue; }
      if (!conns_[fd]->out.empty()) writeTo(*conns_[fd]);
    }

    // Close finished/idle connections.
    int64_t now = nowMs();
    std::vector<std::pair<int, std::string>> doomed;
    for (auto& [fd, c] : conns_) {
      if (c->closing && c->out.empty()) doomed.push_back({fd, "closed"});
      else if (c->nick.empty() && now - c->connectedMs > kHelloTimeoutMs) doomed.push_back({fd, "no hello"});
      else if (now - c->lastSeenMs > kIdleTimeoutMs) doomed.push_back({fd, "ping timeout"});
    }
    for (auto& [fd, why] : doomed) {
      if (conns_.count(fd)) writeTo(*conns_[fd]);
      drop(fd, why);
    }
  }

  std::string bye = encode({"ERR", "SHUTDOWN", "Server is shutting down"});
  for (auto& [fd, c] : conns_) { c->out += bye; writeTo(*c); }
  while (!conns_.empty()) {
    int fd = conns_.begin()->first;
    ::close(fd);
    conns_.erase(fd);
  }
  ::close(listenFd_);
  if (udpFd_ >= 0) ::close(udpFd_);
  log("server stopped");
  std::printf("\nserver stopped\n");
  return 0;
}

}  // namespace

int runServer(const ServerOptions& opt) { return Server(opt).run(); }

}  // namespace schat
