#include "discovery.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "net.h"
#include "protocol.h"

namespace schat {

std::vector<ServerInfo> discoverServers(uint16_t discoveryPort, int timeoutMs) {
  std::vector<ServerInfo> found;
  std::string err;
  int fd = udpSocket(0, true, err);
  if (fd < 0) {
    std::fprintf(stderr, "discovery: %s\n", err.c_str());
    return found;
  }
  std::string probe = encode({"SCHAT_DISCOVER", std::to_string(kProtocolVersion)});
  for (const auto& b : broadcastAddresses()) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(discoveryPort);
    inet_pton(AF_INET, b.c_str(), &a.sin_addr);
    ::sendto(fd, probe.data(), probe.size(), 0, (sockaddr*)&a, sizeof a);
  }
  int64_t deadline = nowMs() + timeoutMs;
  while (true) {
    int left = static_cast<int>(deadline - nowMs());
    if (left <= 0) break;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, left) <= 0) break;
    char buf[2048];
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    ssize_t n = ::recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
    if (n <= 0) continue;
    std::string line(buf, static_cast<size_t>(n));
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    auto f = decode(line);
    if (f.size() < 7 || f[0] != "SCHAT_HERE") continue;
    ServerInfo s;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    s.host = ip;
    s.name = f[2];
    s.port = static_cast<uint16_t>(std::atoi(f[3].c_str()));
    s.users = std::atoi(f[4].c_str());
    s.needsPassword = f[5] == "1";
    s.version = f[6];
    bool dup = false;
    for (auto& o : found) {
      // Same server answering via several interfaces: prefer the non-loopback address.
      if (o.name == s.name && o.port == s.port &&
          (o.host == s.host || o.host == "127.0.0.1" || s.host == "127.0.0.1")) {
        if (o.host == "127.0.0.1") o.host = s.host;
        dup = true;
      }
    }
    if (!dup) found.push_back(s);
  }
  ::close(fd);
  return found;
}

void printServers(const std::vector<ServerInfo>& servers, bool color) {
  const char* b = color ? "\033[1m" : "";
  const char* r = color ? "\033[0m" : "";
  if (servers.empty()) {
    std::printf("No schat servers found on the local network.\n");
    return;
  }
  std::printf("%s  #  %-22s %-21s %-6s %-8s %s%s\n", b, "NAME", "ADDRESS", "USERS", "PASSWORD", "VERSION", r);
  int i = 1;
  for (const auto& s : servers) {
    std::string addr = s.host + ":" + std::to_string(s.port);
    std::printf("%3d  %-22s %-21s %-6d %-8s %s\n", i++, s.name.c_str(), addr.c_str(), s.users,
                s.needsPassword ? "yes" : "no", s.version.c_str());
  }
}

}  // namespace schat
