#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace schat {

struct ServerInfo {
  std::string host;
  uint16_t port = 0;
  std::string name;
  int users = 0;
  bool needsPassword = false;
  std::string version;
};

// Broadcasts a discovery probe on every interface and collects replies.
std::vector<ServerInfo> discoverServers(uint16_t discoveryPort, int timeoutMs);
void printServers(const std::vector<ServerInfo>& servers, bool color);

}  // namespace schat
