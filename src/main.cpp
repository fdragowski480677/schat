#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "client.h"
#include "discovery.h"
#include "net.h"
#include "protocol.h"
#include "server.h"

using namespace schat;

static void usage() {
  std::printf(
      "schat %s - LAN chat with rooms, private messages, file transfer and auto-discovery\n\n"
      "Usage:\n"
      "  schat server   [options]            host a chat server\n"
      "  schat client   [host[:port]] [opts] join a server (no host = find one on the LAN)\n"
      "  schat discover [options]            list servers on the LAN\n"
      "  schat --help | --version\n\n"
      "Server options:\n"
      "  -p, --port N          TCP port (default %u)\n"
      "  -n, --name NAME       server name shown in discovery (default: hostname)\n"
      "  -P, --password PASS   require a password (env SCHAT_PASSWORD also works)\n"
      "      --motd TEXT       message shown to users when they connect\n"
      "      --log FILE        append chat transcript to FILE\n"
      "      --history N       messages replayed when joining a room (default 50, 0=off)\n"
      "      --max-clients N   connection limit (default 128)\n"
      "      --bind ADDR       listen only on this IPv4 address\n"
      "      --no-discovery    do not answer LAN discovery probes\n\n"
      "Client options:\n"
      "  -u, --nick NAME       nickname (default: asks, suggesting $USER)\n"
      "  -P, --password PASS   server password (env SCHAT_PASSWORD also works)\n"
      "  -r, --room ROOM       room to join first (default #lobby)\n"
      "  -d, --downloads DIR   where accepted files are saved (default ./schat-downloads)\n"
      "      --no-bell         no terminal bell on mentions / private messages\n"
      "      --no-time         hide timestamps\n\n"
      "Common options:\n"
      "      --discovery-port N  UDP discovery port (default %u)\n"
      "      --no-color          disable colours (also: NO_COLOR env var)\n\n"
      "Inside the client, type /help for all chat commands.\n",
      kVersion, kDefaultPort, kDiscoveryPort);
}

static bool parsePort(const char* s, uint16_t& out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!*s || *end || v < 1 || v > 65535) return false;
  out = static_cast<uint16_t>(v);
  return true;
}

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    usage();
    return args.empty() ? 1 : 0;
  }
  if (args[0] == "-v" || args[0] == "--version") {
    std::printf("schat %s (protocol %d)\n", kVersion, kProtocolVersion);
    return 0;
  }

  std::string mode = args[0];
  bool color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
  ServerOptions so;
  ClientOptions co;
  if (const char* pw = std::getenv("SCHAT_PASSWORD")) so.password = co.password = pw;
  char host[256] = "schat";
  if (gethostname(host, sizeof host) == 0) so.name = host;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return args[++i].c_str();
    };
    auto port = [&](uint16_t& p) {
      const char* v = val();
      if (!parsePort(v, p)) { std::fprintf(stderr, "error: bad port %s\n", v); std::exit(2); }
    };
    if (a == "-p" || a == "--port") { port(so.port); co.port = so.port; }
    else if (a == "--discovery-port") { port(so.discoveryPort); co.discoveryPort = so.discoveryPort; }
    else if (a == "-n" || a == "--name") so.name = val();
    else if (a == "-P" || a == "--password") so.password = co.password = val();
    else if (a == "--motd") so.motd = val();
    else if (a == "--log") so.logFile = val();
    else if (a == "--history") so.historySize = std::strtoul(val(), nullptr, 10);
    else if (a == "--max-clients") so.maxClients = std::strtoul(val(), nullptr, 10);
    else if (a == "--bind") so.bindAddr = val();
    else if (a == "--no-discovery") so.discovery = false;
    else if (a == "-u" || a == "--nick") co.nick = val();
    else if (a == "-r" || a == "--room") co.room = val();
    else if (a == "-d" || a == "--downloads") co.downloadDir = val();
    else if (a == "--no-bell") co.bell = false;
    else if (a == "--no-time") co.timestamps = false;
    else if (a == "--no-color" || a == "--no-colour") color = false;
    else if (mode == "client" && a[0] != '-' && co.host.empty()) {
      if (!splitHostPort(a, co.host, co.port)) { std::fprintf(stderr, "error: bad address %s\n", a.c_str()); return 2; }
    } else {
      std::fprintf(stderr, "error: unknown option %s (see schat --help)\n", a.c_str());
      return 2;
    }
  }
  so.color = co.color = color;
  if (so.name.size() > 40) so.name.resize(40);

  if (mode == "server" || mode == "host") return runServer(so);
  if (mode == "client" || mode == "connect" || mode == "join") return runClient(co);
  if (mode == "discover" || mode == "scan" || mode == "list") {
    printServers(discoverServers(so.discoveryPort, 1500), color);
    return 0;
  }
  std::fprintf(stderr, "error: unknown mode '%s' (expected server, client or discover)\n", mode.c_str());
  return 2;
}
