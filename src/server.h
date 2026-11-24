#pragma once
#include <cstdint>
#include <string>

namespace schat {

struct ServerOptions {
  uint16_t port = 6969;
  uint16_t discoveryPort = 6970;
  std::string bindAddr;  // empty = all interfaces
  std::string name = "schat";
  std::string password;
  std::string motd = "Welcome! Type /help for commands.";
  std::string logFile;
  size_t historySize = 50;
  size_t maxClients = 128;
  bool discovery = true;
  bool color = true;
};

int runServer(const ServerOptions& opt);

}  // namespace schat
