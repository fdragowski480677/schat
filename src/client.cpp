#include "client.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "discovery.h"
#include "net.h"
#include "protocol.h"
#include "terminal.h"

namespace schat {
namespace {

struct PendingFile {
  int id;
  std::string from, name;
  std::string data;
};

const std::vector<std::string> kCommands = {
    "/help", "/join", "/part", "/switch", "/rooms", "/who", "/users", "/whois", "/nick", "/msg",
    "/r", "/me", "/topic", "/away", "/back", "/send", "/accept", "/reject", "/files", "/ignore",
    "/unignore", "/clear", "/quit"};

class Client {
 public:
  explicit Client(ClientOptions o) : opt_(std::move(o)), ed_(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {}
  int run();

 private:
  // presentation
  std::string c(const char* code) const { return opt_.color ? std::string("\033[") + code + "m" : ""; }
  std::string reset() const { return c("0"); }
  std::string ts(int64_t t) const { return opt_.timestamps ? c("2") + clockStr(t) + reset() + " " : ""; }
  std::string nickColor(const std::string& n) const;
  std::string roomTag(const std::string& room) const;
  void show(const std::string& s) { ed_.print(s); }
  void sys(const std::string& s) { show(c("33") + "-- " + s + reset()); }
  void error(const std::string& s) { show(c("1;31") + "!! " + s + reset()); }
  void updatePrompt();
  void bell() { if (opt_.bell && ed_.interactive()) (void)!::write(STDOUT_FILENO, "\a", 1); }

  void send(const std::vector<std::string>& f);
  void handleServer(const std::vector<std::string>& f);
  void handleInput(const std::string& line);
  void sendFile(const std::string& nick, const std::string& path);
  void acceptFile(int id);
  bool mentionsMe(const std::string& text) const;
  std::vector<std::string> complete(const std::string& word, bool first);
  void printHelp();

  ClientOptions opt_;
  LineEditor ed_;
  int fd_ = -1;
  bool running_ = true;
  std::string nick_;
  std::string serverName_;
  std::string current_;
  std::vector<std::string> joined_;  // in join order
  std::map<std::string, std::set<std::string>> members_;
  std::set<std::string> ignored_;  // lowercase nicks
  std::string lastPrivFrom_;
  std::vector<PendingFile> files_;
  int nextFileId_ = 1;
  bool inHistory_ = false;
  int64_t lastPing_ = 0;
};

std::string Client::nickColor(const std::string& n) const {
  static const char* palette[] = {"31", "32", "34", "35", "36", "91", "92", "94", "95", "96"};
  unsigned h = 5381;
  for (char ch : lower(n)) h = h * 33 + static_cast<unsigned char>(ch);
  return c("1") + c(palette[h % 10]) + n + reset();
}

std::string Client::roomTag(const std::string& room) const {
  if (joined_.size() <= 1 && room == current_) return "";
  return (room == current_ ? c("1;37") : c("2")) + room + reset() + " ";
}

void Client::updatePrompt() {
  std::string p = current_.empty() ? "> " : current_ + "> ";
  ed_.setPrompt(opt_.color ? "\033[1;36m" + p + "\033[0m" : p);
}

void Client::send(const std::vector<std::string>& f) {
  if (fd_ >= 0 && !sendAll(fd_, encode(f))) {
    error("Connection lost while sending");
    running_ = false;
  }
}

bool Client::mentionsMe(const std::string& text) const {
  if (nick_.empty()) return false;
  std::string t = lower(text), n = lower(nick_);
  for (size_t p = t.find(n); p != std::string::npos; p = t.find(n, p + 1)) {
    bool l = p == 0 || !(std::isalnum((unsigned char)t[p - 1]) || t[p - 1] == '_');
    size_t e = p + n.size();
    bool r = e >= t.size() || !(std::isalnum((unsigned char)t[e]) || t[e] == '_');
    if (l && r) return true;
  }
  return false;
}

void Client::handleServer(const std::vector<std::string>& f) {
  const std::string& cmd = f[0];
  auto a = [&](size_t i) -> std::string { return i < f.size() ? f[i] : std::string(); };
  auto t = [&](size_t i) -> int64_t { return std::atoll(a(i).c_str()); };
  auto ignored = [&](const std::string& n) { return ignored_.count(lower(n)) > 0; };

  if (cmd == "WELCOME") {
    nick_ = a(1);
    serverName_ = a(2);
    show(c("1;32") + "Connected to \"" + serverName_ + "\" (server v" + a(4) + ") as " + nick_ + reset());
    if (!a(3).empty()) show(c("1") + a(3) + reset());
    send({"JOIN", opt_.room});
  } else if (cmd == "MSG" || cmd == "ME") {
    if (ignored(a(3))) return;
    std::string room = a(1), text = a(4);
    bool hl = !inHistory_ && a(3) != nick_ && mentionsMe(text);
    std::string body = cmd == "MSG" ? nickColor(a(3)) + ": " + (hl ? c("1;43;30") + text + reset() : text)
                                    : c("35") + "* " + reset() + nickColor(a(3)) + " " + text;
    std::string line = ts(t(2)) + roomTag(room) + body;
    if (inHistory_) line = c("2") + "~ " + reset() + line;
    show(line);
    if (hl) bell();
  } else if (cmd == "PRIV") {
    std::string from = a(2), to = a(3);
    if (from != nick_ && ignored(from)) return;
    if (from == nick_) {
      show(ts(t(1)) + c("1;35") + "[you -> " + to + "] " + reset() + a(4));
    } else {
      lastPrivFrom_ = from;
      show(ts(t(1)) + c("1;35") + "[" + from + " -> you] " + reset() + a(4));
      bell();
    }
  } else if (cmd == "JOINED") {
    std::string room = a(1), who = a(3);
    members_[room].insert(who);
    if (who == nick_) {
      if (std::find(joined_.begin(), joined_.end(), room) == joined_.end()) joined_.push_back(room);
      current_ = room;
      updatePrompt();
      sys("You joined " + room);
    } else if (!ignored(who)) {
      sys(ts(t(2)) + roomTag(room) + who + " joined");
    }
  } else if (cmd == "PARTED") {
    std::string room = a(1), who = a(3);
    members_[room].erase(who);
    if (who != nick_ && !ignored(who)) sys(ts(t(2)) + roomTag(room) + who + " left (" + a(4) + ")");
  } else if (cmd == "PARTOK") {
    std::string room = a(1);
    joined_.erase(std::remove(joined_.begin(), joined_.end(), room), joined_.end());
    members_.erase(room);
    sys("You left " + room);
    if (current_ == room) current_ = joined_.empty() ? "" : joined_.back();
    updatePrompt();
  } else if (cmd == "SWITCH") {
    current_ = a(1);
    updatePrompt();
    sys("Now talking in " + current_);
  } else if (cmd == "QUIT") {
    for (auto& [r, m] : members_) m.erase(a(2));
    if (!ignored(a(2))) sys(ts(t(1)) + a(2) + " disconnected (" + a(3) + ")");
  } else if (cmd == "NICK") {
    for (auto& [r, m] : members_)
      if (m.erase(a(1))) m.insert(a(2));
    if (a(1) == nick_) { nick_ = a(2); sys("You are now known as " + nick_); }
    else sys(a(1) + " is now known as " + a(2));
  } else if (cmd == "WHO") {
    std::string room = a(1), list;
    auto& m = members_[room];
    m.clear();
    for (size_t i = 2; i < f.size(); ++i) {
      std::string n = f[i].substr(0, f[i].find(' '));
      m.insert(n);
      list += (i > 2 ? ", " : "") + nickColor(n) + (f[i].size() > n.size() ? c("2") + " (away)" + reset() : "");
    }
    sys("In " + room + " (" + std::to_string(f.size() - 2) + "): " + reset() + list);
  } else if (cmd == "USERS") {
    std::string list;
    for (size_t i = 1; i < f.size(); ++i) list += (i > 1 ? ", " : "") + f[i];
    sys("Online (" + std::to_string(f.size() - 1) + "): " + list);
  } else if (cmd == "ROOMS") {
    sys("Rooms on " + serverName_ + ":");
    for (size_t i = 1; i < f.size(); ++i) {
      std::istringstream ss(f[i]);
      std::string name, count, topic;
      ss >> name >> count;
      std::getline(ss, topic);
      show("   " + c("1") + name + reset() + "  " + count + " user(s)" + (topic.size() > 1 ? "  -" + topic : ""));
    }
  } else if (cmd == "WHOIS") {
    sys(a(1) + " @ " + a(2) + ", online since " + dateTimeStr(t(3)) + ", in: " + a(4) +
        (a(5).empty() ? "" : ", away: " + a(5)));
  } else if (cmd == "TOPIC") {
    sys(roomTag(a(1)) + "Topic of " + a(1) + (a(2).empty() ? "" : " (set by " + a(2) + ")") + ": " + c("1") + a(3) + reset());
  } else if (cmd == "HISTORY_BEGIN") {
    inHistory_ = true;
    sys("Last " + a(2) + " message(s) in " + a(1) + ":");
  } else if (cmd == "HISTORY_END") {
    inHistory_ = false;
    sys("End of history");
  } else if (cmd == "ANNOUNCE") {
    show(ts(t(1)) + c("1;41;37") + " SERVER " + reset() + " " + c("1") + a(2) + reset());
    bell();
  } else if (cmd == "FILE") {  // FILE ts from name size b64
    if (ignored(a(2))) return;
    PendingFile pf;
    pf.id = nextFileId_++;
    pf.from = a(2);
    pf.name = a(3);
    if (!base64Decode(a(5), pf.data)) { error("Received a corrupt file from " + pf.from); return; }
    sys(ts(t(1)) + pf.from + " wants to send you \"" + pf.name + "\" (" + humanSize(pf.data.size()) +
        ").  /accept " + std::to_string(pf.id) + "  or  /reject " + std::to_string(pf.id));
    files_.push_back(std::move(pf));
    bell();
  } else if (cmd == "INFO") {
    sys(a(1));
  } else if (cmd == "ERR") {
    error(a(2));
    if (a(1) == "KICKED" || a(1) == "SHUTDOWN" || a(1) == "PASSWORD" || a(1) == "VERSION" ||
        a(1) == "BADNICK" || (a(1) == "FULL" && nick_.empty()))
      running_ = false;
  } else if (cmd == "PONG") {
  }
}

void Client::sendFile(const std::string& nick, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { error("Cannot open " + path); return; }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() > kMaxFile) { error("File too large (max " + humanSize(kMaxFile) + ")"); return; }
  std::string name = path.substr(path.find_last_of('/') + 1);
  send({"FILE", nick, name, std::to_string(data.size()), base64Encode(data)});
}

static std::string safeName(std::string n) {
  n = n.substr(n.find_last_of("/\\") + 1);
  for (auto& ch : n)
    if (ch < 32 || ch == ':') ch = '_';
  if (n.empty() || n == "." || n == "..") n = "file";
  if (n[0] == '.') n = "_" + n;
  return n;
}

void Client::acceptFile(int id) {
  auto it = std::find_if(files_.begin(), files_.end(), [&](auto& f) { return f.id == id || id == 0; });
  if (it == files_.end()) { error("No pending file #" + std::to_string(id)); return; }
  ::mkdir(opt_.downloadDir.c_str(), 0755);
  std::string base = safeName(it->name), path = opt_.downloadDir + "/" + base;
  for (int i = 1; access(path.c_str(), F_OK) == 0; ++i) {
    size_t dot = base.find_last_of('.');
    std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
    std::string ext = dot == std::string::npos || dot == 0 ? "" : base.substr(dot);
    path = opt_.downloadDir + "/" + stem + " (" + std::to_string(i) + ")" + ext;
  }
  std::ofstream out(path, std::ios::binary);
  out.write(it->data.data(), static_cast<std::streamsize>(it->data.size()));
  if (!out) { error("Could not write " + path); return; }
  sys("Saved " + path + " (" + humanSize(it->data.size()) + ") from " + it->from);
  files_.erase(it);
}

void Client::printHelp() {
  auto row = [&](const std::string& cmd, const std::string& d) {
    std::string pad = cmd.size() < 26 ? std::string(26 - cmd.size(), ' ') : " ";
    show("  " + c("1") + cmd + reset() + pad + d);
  };
  show(c("1;36") + "Chatting" + reset());
  row("<text>", "message the current room");
  row("/me <action>", "emote, e.g. /me waves");
  row("/msg <nick> <text>", "private message");
  row("/r <text>", "reply to the last private message");
  show(c("1;36") + "Rooms" + reset());
  row("/join <#room>", "join (or create) a room and switch to it");
  row("/switch <#room>", "talk in another joined room  (also Alt: /sw)");
  row("/part [#room] [reason]", "leave a room (default: current)");
  row("/rooms", "list all rooms on the server");
  row("/who [#room]", "who is in a room");
  row("/topic [text]", "show or set the current room's topic");
  show(c("1;36") + "People" + reset());
  row("/users", "everyone online");
  row("/whois <nick>", "details about a user");
  row("/nick <name>", "change your nickname");
  row("/away [reason] | /back", "mark yourself away / back");
  row("/ignore <nick> | /unignore", "hide (or unhide) someone's messages");
  show(c("1;36") + "Files" + reset());
  row("/send <nick> <path>", "send a file (max " + humanSize(kMaxFile) + ")");
  row("/files", "list files offered to you");
  row("/accept [id] | /reject <id>", "save or discard an offered file");
  show(c("1;36") + "Other" + reset());
  row("/clear", "clear the screen (or Ctrl-L)");
  row("/quit [reason]", "leave (or Ctrl-D on an empty line)");
  show(c("2") + "Keys: Tab completes nicks/rooms/commands, Up/Down history, Ctrl-A/E/U/K/W edit. Start a line with // to send a literal '/'." + reset());
}

void Client::handleInput(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && line.back() == ' ') line.pop_back();
  if (line.empty()) return;
  if (line[0] != '/' || line.rfind("//", 0) == 0) {
    if (current_.empty()) { error("You are not in a room. /join #lobby"); return; }
    send({"MSG", current_, line[0] == '/' ? line.substr(1) : line});
    return;
  }
  std::string cmd = line, rest;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) {
    cmd = line.substr(0, sp);
    rest = line.substr(line.find_first_not_of(' ', sp) == std::string::npos ? line.size() : line.find_first_not_of(' ', sp));
  }
  cmd = lower(cmd);
  auto firstWord = [](const std::string& s, std::string& remainder) {
    size_t p = s.find(' ');
    remainder = p == std::string::npos ? "" : s.substr(s.find_first_not_of(' ', p) == std::string::npos ? s.size() : s.find_first_not_of(' ', p));
    return s.substr(0, p);
  };
  std::string tail;

  if (cmd == "/help" || cmd == "/?") printHelp();
  else if (cmd == "/quit" || cmd == "/exit" || cmd == "/q") { send({"QUIT", rest}); running_ = false; }
  else if (cmd == "/join" || cmd == "/j") {
    if (rest.empty()) { error("Usage: /join #room"); return; }
    send({"JOIN", normalizeRoom(firstWord(rest, tail))});
  } else if (cmd == "/switch" || cmd == "/sw") {
    std::string r = normalizeRoom(firstWord(rest, tail));
    if (rest.empty()) {  // cycle through joined rooms
      if (joined_.empty()) return;
      auto it = std::find(joined_.begin(), joined_.end(), current_);
      current_ = (it == joined_.end() || ++it == joined_.end()) ? joined_.front() : *it;
    } else if (std::find(joined_.begin(), joined_.end(), r) == joined_.end()) {
      error("You are not in " + r + " - use /join " + r); return;
    } else current_ = r;
    updatePrompt();
    sys("Now talking in " + current_);
  } else if (cmd == "/part" || cmd == "/leave") {
    std::string r = current_;
    if (!rest.empty() && (rest[0] == '#')) r = normalizeRoom(firstWord(rest, tail));
    else tail = rest;
    if (r.empty()) { error("You are not in a room"); return; }
    send({"PART", r, tail});
  } else if (cmd == "/rooms" || cmd == "/list") send({"ROOMS"});
  else if (cmd == "/who" || cmd == "/names") send({"WHO", rest.empty() ? current_ : normalizeRoom(rest)});
  else if (cmd == "/users") send({"USERS"});
  else if (cmd == "/whois") {
    if (rest.empty()) { error("Usage: /whois <nick>"); return; }
    send({"WHOIS", rest});
  } else if (cmd == "/nick") {
    if (rest.empty()) { sys("You are " + nick_); return; }
    send({"NICK", rest});
  } else if (cmd == "/msg" || cmd == "/m" || cmd == "/query") {
    std::string who = firstWord(rest, tail);
    if (who.empty() || tail.empty()) { error("Usage: /msg <nick> <text>"); return; }
    send({"PRIV", who, tail});
  } else if (cmd == "/r" || cmd == "/reply") {
    if (lastPrivFrom_.empty()) { error("Nobody has messaged you yet"); return; }
    if (!rest.empty()) send({"PRIV", lastPrivFrom_, rest});
  } else if (cmd == "/me") {
    if (current_.empty() || rest.empty()) return;
    send({"ME", current_, rest});
  } else if (cmd == "/topic") {
    if (current_.empty()) return;
    if (rest.empty()) send({"TOPIC", current_});
    else send({"TOPIC", current_, rest});
  } else if (cmd == "/away") send({"AWAY", rest.empty() ? "away" : rest});
  else if (cmd == "/back") send({"AWAY", ""});
  else if (cmd == "/ignore") {
    if (rest.empty()) {
      std::string l;
      for (auto& n : ignored_) l += n + " ";
      sys(ignored_.empty() ? "You are not ignoring anyone" : "Ignoring: " + l);
    } else { ignored_.insert(lower(rest)); sys("Ignoring " + rest); }
  } else if (cmd == "/unignore") { ignored_.erase(lower(rest)); sys("No longer ignoring " + rest); }
  else if (cmd == "/send") {
    std::string who = firstWord(rest, tail);
    if (who.empty() || tail.empty()) { error("Usage: /send <nick> <path>"); return; }
    if (tail.size() > 1 && tail[0] == '~' && getenv("HOME")) tail = std::string(getenv("HOME")) + tail.substr(1);
    sendFile(who, tail);
  } else if (cmd == "/files") {
    if (files_.empty()) sys("No files waiting");
    for (auto& f : files_)
      sys("#" + std::to_string(f.id) + "  " + f.name + "  " + humanSize(f.data.size()) + "  from " + f.from);
  } else if (cmd == "/accept") acceptFile(rest.empty() ? 0 : std::atoi(rest.c_str()));
  else if (cmd == "/reject") {
    int id = std::atoi(rest.c_str());
    auto n = files_.size();
    files_.erase(std::remove_if(files_.begin(), files_.end(), [&](auto& f) { return f.id == id; }), files_.end());
    if (n == files_.size()) error("No pending file #" + rest); else sys("Rejected file #" + rest);
  } else if (cmd == "/clear" || cmd == "/cls") ed_.clearScreen();
  else error("Unknown command " + cmd + " - try /help");
}

std::vector<std::string> Client::complete(const std::string& word, bool first) {
  std::vector<std::string> out;
  std::set<std::string> pool;
  if (!word.empty() && word[0] == '/' && first) pool.insert(kCommands.begin(), kCommands.end());
  else if (!word.empty() && word[0] == '#') {
    for (auto& [r, m] : members_) pool.insert(r);
  } else {
    for (auto& [r, m] : members_) pool.insert(m.begin(), m.end());
    pool.erase(nick_);
  }
  std::string lw = lower(word);
  for (auto& p : pool)
    if (lower(p).rfind(lw, 0) == 0) out.push_back(p);
  return out;
}

int Client::run() {
  std::signal(SIGPIPE, SIG_IGN);

  if (opt_.host.empty()) {
    std::printf("Searching the LAN for schat servers...\n");
    auto servers = discoverServers(opt_.discoveryPort, 1200);
    if (servers.empty()) {
      std::printf("No servers found. Start one with `schat server`, or connect directly:\n"
                  "  schat client <ip>[:port]\n");
      return 1;
    }
    size_t pick = 0;
    if (servers.size() > 1) {
      printServers(servers, opt_.color);
      std::printf("Connect to which server [1-%zu]? ", servers.size());
      std::fflush(stdout);
      std::string l;
      if (!std::getline(std::cin, l)) return 1;
      pick = static_cast<size_t>(std::atoi(l.c_str()));
      if (pick < 1 || pick > servers.size()) { std::printf("Invalid choice\n"); return 1; }
      --pick;
    }
    opt_.host = servers[pick].host;
    opt_.port = servers[pick].port;
    std::printf("Found \"%s\" at %s:%u\n", servers[pick].name.c_str(), opt_.host.c_str(), opt_.port);
    if (servers[pick].needsPassword && opt_.password.empty()) {
      std::printf("This server needs a password: ");
      std::fflush(stdout);
      if (!std::getline(std::cin, opt_.password)) return 1;
    }
  }

  if (opt_.nick.empty()) {
    const char* u = getenv("USER");
    std::string guess = u ? u : "guest";
    if (!validNick(guess)) guess = "guest";
    if (isatty(STDIN_FILENO)) {
      std::printf("Nickname [%s]: ", guess.c_str());
      std::fflush(stdout);
      std::string l;
      std::getline(std::cin, l);
      opt_.nick = l.empty() ? guess : l;
    } else {
      opt_.nick = guess;
    }
  }
  if (!validNick(opt_.nick)) {
    std::fprintf(stderr, "Invalid nickname \"%s\": use 1-20 chars of A-Z a-z 0-9 _ -\n", opt_.nick.c_str());
    return 1;
  }
  opt_.room = normalizeRoom(opt_.room);

  std::string e;
  std::printf("Connecting to %s:%u...\n", opt_.host.c_str(), opt_.port);
  std::fflush(stdout);
  fd_ = tcpConnect(opt_.host, opt_.port, e);
  if (fd_ < 0) {
    std::fprintf(stderr, "Could not connect to %s:%u: %s\n", opt_.host.c_str(), opt_.port, e.c_str());
    return 1;
  }

  ed_.setCompleter([this](const std::string& w, bool f) { return complete(w, f); });
  updatePrompt();
  send({"HELLO", std::to_string(kProtocolVersion), opt_.nick, opt_.password});
  lastPing_ = nowMs();

  LineBuffer inbuf;
  bool stdinOpen = true;
  int64_t quitAt = 0;  // piped input ended: linger briefly for server replies
  int exitCode = 0;
  while (running_) {
    if (quitAt && nowMs() > quitAt) { send({"QUIT", ""}); break; }
    pollfd p[2] = {{fd_, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    int n = pollFds(p, stdinOpen ? 2 : 1, quitAt ? 100 : 1000);
    if (n < 0 && errno != EINTR) break;
    if (nowMs() - lastPing_ > 30000) { send({"PING", std::to_string(nowSec())}); lastPing_ = nowMs(); }
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      char buf[65536];
      ssize_t k = ::recv(fd_, buf, sizeof buf, 0);
      if (k <= 0) {
        error("Disconnected from server");
        exitCode = nick_.empty() ? 1 : 0;
        break;
      }
      inbuf.append(buf, static_cast<size_t>(k));
      std::string l;
      while (inbuf.next(l))
        if (!l.empty()) handleServer(decode(l));
      if (!running_ && nick_.empty()) exitCode = 1;
    }
    if (stdinOpen && (p[1].revents & (POLLIN | POLLHUP))) {
      char buf[4096];
      ssize_t k = ::read(STDIN_FILENO, buf, sizeof buf);
      std::vector<std::string> lines;
      if (k <= 0) {
        stdinOpen = false;
        quitAt = nowMs() + 1000;
      } else {
        ed_.feed(buf, static_cast<size_t>(k), lines);
      }
      for (auto& l : lines) {
        if (!running_) break;
        handleInput(l);
      }
    }
  }
  ed_.finish();
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_WR);
    ::close(fd_);
  }
  std::printf("Bye!\n");
  return exitCode;
}

}  // namespace

int runClient(ClientOptions opt) { return Client(std::move(opt)).run(); }

}  // namespace schat
