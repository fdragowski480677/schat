#pragma once
#include <poll.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SIGPIPE is ignored process-wide instead
#endif

namespace schat {

int tcpListen(uint16_t port, const std::string& bindAddr, std::string& err);
int tcpConnect(const std::string& host, uint16_t port, std::string& err);
int udpSocket(uint16_t bindPort, bool broadcast, std::string& err);
bool setNonBlocking(int fd);
bool sendAll(int fd, const std::string& data);  // blocking send helper
std::string peerAddress(int fd);
std::vector<std::string> localIPv4Addresses();  // non-loopback interfaces
std::vector<std::string> broadcastAddresses();  // per-interface + 255.255.255.255
// Parses "host", "host:port" or ":port"; returns false on bad port.
bool splitHostPort(const std::string& in, std::string& host, uint16_t& port);
// poll() that also works on terminals. macOS's poll() does not support devices
// such as a TTY, so select() is used there (or when SCHAT_USE_SELECT is defined).
int pollFds(pollfd* fds, size_t count, int timeoutMs);

}  // namespace schat
