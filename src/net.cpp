#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace schat {

int tcpListen(uint16_t port, const std::string& bindAddr, std::string& err) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { err = std::strerror(errno); return -1; }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (bindAddr.empty()) a.sin_addr.s_addr = INADDR_ANY;
  else if (inet_pton(AF_INET, bindAddr.c_str(), &a.sin_addr) != 1) {
    err = "invalid bind address " + bindAddr; ::close(fd); return -1;
  }
  if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 64) < 0) {
    err = std::strerror(errno); ::close(fd); return -1;
  }
  return fd;
}

int tcpConnect(const std::string& host, uint16_t port, std::string& err) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (rc != 0) { err = gai_strerror(rc); return -1; }
  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    err = std::strerror(errno);
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd >= 0) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  }
  return fd;
}

int udpSocket(uint16_t bindPort, bool broadcast, std::string& err) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { err = std::strerror(errno); return -1; }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (broadcast) ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  if (bindPort) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(bindPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(fd, (sockaddr*)&a, sizeof a) < 0) {
      err = std::strerror(errno); ::close(fd); return -1;
    }
  }
  return fd;
}

bool setNonBlocking(int fd) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool sendAll(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

std::string peerAddress(int fd) {
  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  if (::getpeername(fd, (sockaddr*)&ss, &len) < 0) return "?";
  char buf[INET6_ADDRSTRLEN] = "?";
  if (ss.ss_family == AF_INET)
    inet_ntop(AF_INET, &((sockaddr_in*)&ss)->sin_addr, buf, sizeof buf);
  else if (ss.ss_family == AF_INET6)
    inet_ntop(AF_INET6, &((sockaddr_in6*)&ss)->sin6_addr, buf, sizeof buf);
  return buf;
}

template <typename F>
static void forEachIPv4(F f) {
  ifaddrs* ifs = nullptr;
  if (::getifaddrs(&ifs) != 0) return;
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
    f(i);
  }
  ::freeifaddrs(ifs);
}

std::vector<std::string> localIPv4Addresses() {
  std::vector<std::string> out;
  forEachIPv4([&](ifaddrs* i) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, buf, sizeof buf);
    out.push_back(buf);
  });
  return out;
}

std::vector<std::string> broadcastAddresses() {
  std::vector<std::string> out;
  forEachIPv4([&](ifaddrs* i) {
    if (!(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) return;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_broadaddr)->sin_addr, buf, sizeof buf);
    out.push_back(buf);
  });
  out.push_back("255.255.255.255");
  out.push_back("127.0.0.1");  // finds a server on this same machine
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool splitHostPort(const std::string& in, std::string& host, uint16_t& port) {
  size_t c = in.rfind(':');
  if (c == std::string::npos || in.find(':') != c) {  // no port, or bare IPv6
    host = in;
    return true;
  }
  host = in.substr(0, c);
  std::string p = in.substr(c + 1);
  if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
  long v = std::stol(p);
  if (v < 1 || v > 65535) return false;
  port = static_cast<uint16_t>(v);
  return true;
}

#if defined(__APPLE__) && !defined(SCHAT_USE_SELECT)
#define SCHAT_USE_SELECT 1
#endif

int pollFds(pollfd* fds, size_t count, int timeoutMs) {
#ifdef SCHAT_USE_SELECT
  fd_set rd, wr;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  int maxFd = -1, invalid = 0;
  for (size_t i = 0; i < count; ++i) {
    fds[i].revents = 0;
    int fd = fds[i].fd;
    if (fd < 0) continue;
    if (fd >= FD_SETSIZE) { fds[i].revents = POLLNVAL; ++invalid; continue; }
    if (fds[i].events & POLLIN) FD_SET(fd, &rd);
    if (fds[i].events & POLLOUT) FD_SET(fd, &wr);
    maxFd = std::max(maxFd, fd);
  }
  timeval tv{};
  if (!invalid && timeoutMs > 0) {
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
  }
  int rc = ::select(maxFd + 1, &rd, &wr, nullptr, (timeoutMs < 0 && !invalid) ? nullptr : &tv);
  if (rc < 0) return rc;
  int ready = invalid;
  for (size_t i = 0; i < count; ++i) {
    int fd = fds[i].fd;
    if (fd < 0 || fd >= FD_SETSIZE) continue;
    if (FD_ISSET(fd, &rd)) fds[i].revents |= POLLIN;
    if (FD_ISSET(fd, &wr)) fds[i].revents |= POLLOUT;
    if (fds[i].revents) ++ready;
  }
  return ready;
#else
  return ::poll(fds, static_cast<nfds_t>(count), timeoutMs);
#endif
}

}  // namespace schat
