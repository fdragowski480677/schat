#pragma once
#include <cstdint>
#include <string>

namespace schat {

struct ClientOptions {
  std::string host;  // empty = discover on LAN
  uint16_t port = 6969;
  uint16_t discoveryPort = 6970;
  std::string nick;
  std::string password;
  std::string room = "#lobby";
  std::string downloadDir = "schat-downloads";
  bool color = true;
  bool bell = true;
  bool timestamps = true;
};

int runClient(ClientOptions opt);

}  // namespace schat
