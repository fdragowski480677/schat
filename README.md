# schat 2 — LAN chat for the terminal

A fast, dependency-free chat for your local network. One person runs a
server and everyone else joins with a single command. You don't need to know
IP addresses, because clients find servers on the LAN automatically.

```
14:02 #lobby alice: anyone up for lunch?
14:02 #lobby * bob waves
14:03 [carol -> you] psst, check #dev
-- 14:03 dave joined
#lobby> sure, 12:30?_
```

## Features

- **Many users at once**: one server handles up to 128 clients (configurable).
- **Zero-config discovery**: `schat client` with no address scans the LAN (UDP broadcast) and connects.
- **Rooms**: `#lobby` by default. Join and create any number of rooms, switch between them, and set topics.
- **Private messages**: `/msg` and `/r` to reply. You hear a bell on a PM or when someone says your nick.
- **File transfer**: `/send bob photo.jpg` sends up to 8 MiB. The recipient decides with `/accept` or `/reject`; nothing is saved without consent.
- **History**: anyone who joins a room sees its last 50 messages.
- **A real line editor**: incoming messages never break the line you are typing. You get Tab completion for nicks, rooms and commands, Up/Down history, and Ctrl-A/E/U/K/W.
- **Coloured nicks**: each nick gets a stable colour, mentions are highlighted, and timestamps are shown.
- **Away status, `/whois`, and a client-side `/ignore`.**
- **Server admin console**: kick users, broadcast announcements, change the MOTD and topics, view stats.
- **Optional password**, **flood protection**, **idle timeouts**, and an optional **transcript log file**.
- Works in scripts too: pipe lines into the client and it sends them (no TTY needed).

## Build

You need a C++17 compiler and CMake 3.16 or newer. There are no other dependencies. It runs on Linux, macOS and WSL.

```bash
git clone https://github.com/fdragowski480677/schat.git && cd schat
cmake -S . -B build
cmake --build build -j
sudo cmake --install build      # optional: installs `schat` into /usr/local/bin
```

Run the tests (unit tests plus an end-to-end test with a real server and clients):

```bash
cd build && ctest --output-on-failure
```

## Quick start

On one machine:

```bash
schat server
```

```
schat server "office-pc" v2.0.0
  listening on TCP port 6969
  connect with:  schat client 192.168.1.20:6969
  LAN discovery on UDP port 6970 (clients can just run: schat client)
```

On every other machine on the same network:

```bash
schat client
```

It finds the server, asks for a nickname and puts you in `#lobby`. Type a
message and press Enter. Type `/help` at any time.

If discovery is blocked (some Wi-Fi networks drop broadcasts), connect
directly using the address the server printed:

```bash
schat client 192.168.1.20            # default port 6969
schat client 192.168.1.20:7000       # custom port
schat client office-pc.local -u alice -r '#dev'
```

To see which servers are around without connecting:

```bash
schat discover
```

```
  #  NAME                   ADDRESS               USERS  PASSWORD VERSION
  1  office-pc              192.168.1.20:6969     4      no       2.0.0
  2  games-night            192.168.1.31:6969     2      yes      2.0.0
```

If there are several servers, `schat client` shows this list and asks which one to join.

## Chat commands

| Command | What it does |
|---|---|
| *text* | Send to the current room |
| `/me <action>` | Emote: `/me waves` → `* alice waves` |
| `/msg <nick> <text>` | Private message (also `/m`) |
| `/r <text>` | Reply to whoever last sent you a private message |
| `/join <#room>` | Join a room (it's created if needed) and switch to it (also `/j`) |
| `/switch [#room]` | Talk in another joined room. With no argument, cycles through rooms (also `/sw`) |
| `/part [#room] [reason]` | Leave a room (default: the current one) |
| `/rooms` | List all rooms with user counts and topics |
| `/who [#room]` | Who is in a room |
| `/users` | Everyone online |
| `/whois <nick>` | Address, online-since, rooms and away status |
| `/topic [text]` | Show or set the current room's topic |
| `/nick <name>` | Change nickname |
| `/away [reason]`, `/back` | Mark yourself away (others see it in `/who` and when they PM you) |
| `/ignore [nick]`, `/unignore <nick>` | Hide someone's messages (local only). `/ignore` alone lists them |
| `/send <nick> <path>` | Offer a file (max 8 MiB) |
| `/files` | Files waiting for you |
| `/accept [id]`, `/reject <id>` | Save (to `./schat-downloads/`) or discard an offered file |
| `/clear` | Clear the screen |
| `/quit [reason]` | Leave (Ctrl-D on an empty line also works) |

Start a line with `//` to send a message that begins with `/`.

When you are in several rooms, each message is tagged with its room and the
prompt shows where you are talking (`#dev> `).

### Keyboard

| Key | Action |
|---|---|
| Tab | Complete a nick (`al⇥` → `alice: `), a `#room` or a `/command` |
| ↑ / ↓ | Previous / next line you typed |
| ← → Home End | Move the cursor |
| Ctrl-A / Ctrl-E | Start / end of line |
| Ctrl-U / Ctrl-K | Delete to start / end of line |
| Ctrl-W | Delete the previous word |
| Ctrl-L | Clear the screen |
| Ctrl-D | Quit (on an empty line) |

## Running a server

```bash
schat server --name "games-night" --password hunter2 \
             --motd "Be nice. Rules in #rules." \
             --log chat.log --history 100
```

| Option | Default | Meaning |
|---|---|---|
| `-p, --port N` | 6969 | TCP port for chat |
| `-n, --name NAME` | hostname | Name shown in discovery and on connect |
| `-P, --password PASS` | none | Require a password (or set `SCHAT_PASSWORD`) |
| `--motd TEXT` | welcome text | Shown to everyone on connect |
| `--log FILE` | off | Append a timestamped transcript |
| `--history N` | 50 | Messages per room replayed on join (0 = off) |
| `--max-clients N` | 128 | Connection limit |
| `--bind ADDR` | all | Listen on one interface only, e.g. `--bind 192.168.1.20` |
| `--no-discovery` | | Don't answer discovery probes (the server stays reachable by address) |
| `--discovery-port N` | 6970 | UDP port for discovery (must match on clients) |

### Admin console

Type these into the terminal where the server runs:

| Command | Action |
|---|---|
| `/users` | Connected users with addresses and rooms |
| `/rooms` | All rooms |
| `/say <text>` | Red banner announcement to everyone |
| `/kick <nick> [reason]` | Disconnect a user |
| `/topic <#room> <text>` | Set a topic as the server |
| `/motd <text>` | Change the message of the day |
| `/stats` | Uptime and counters |
| `/quit` | Shut down (Ctrl-C works too) |

To run the server in the background, use `schat server < /dev/null > schat.log &`.
The console is disabled automatically when stdin is closed.

## Client options

| Option | Meaning |
|---|---|
| `host[:port]` | Server to join. Omit it to auto-discover |
| `-u, --nick NAME` | Nickname (1–20 chars: letters, digits, `_`, `-`). If it's taken, the server appends `_2` |
| `-P, --password PASS` | Server password (or `SCHAT_PASSWORD`). If missing, you are asked |
| `-r, --room ROOM` | First room to join (default `#lobby`) |
| `-d, --downloads DIR` | Where accepted files go (default `./schat-downloads`) |
| `--no-bell` | Silence the bell for mentions and PMs |
| `--no-time` | Hide timestamps |
| `--no-color` | Plain output (also respects `NO_COLOR`) |

### Scripting

When stdin is not a terminal, the client sends each input line and prints
incoming messages as plain lines, so you can automate it:

```bash
echo "Build #512 finished ✅" | schat client 192.168.1.20 -u ci-bot -r '#dev'
```

## Troubleshooting

- **`No schat servers found`**: the firewall or network drops UDP broadcast. Connect with `schat client <ip>`, or open UDP 6970 and TCP 6969 on the server (`sudo ufw allow 6969/tcp && sudo ufw allow 6970/udp`).
- **`cannot listen on port 6969: Address already in use`**: another server is running. Pick another port with `-p 7000`.
- **WSL2**: WSL2 sits behind NAT, so machines on the LAN can't reach a server inside WSL without port forwarding (`netsh interface portproxy ...`) or WSL "mirrored" networking. Clients inside WSL can connect out normally.
- **Garbled colours**: use `--no-color`.

## Security notes

schat is for trusted local networks. Traffic, **including the password, is not
encrypted**. Anyone on the same network segment could read it. Don't use it
over the internet or for secrets. If you need that, tunnel it:
`ssh -L 6969:localhost:6969 server-host`, then run `schat client localhost`.

## Protocol (for hackers)

Each message is one line of TAB-separated fields. TAB, newline, CR and `\` inside
fields are backslash-escaped. The client opens with
`HELLO <protocol> <nick> <password>`. After that the client sends commands like
`MSG #room text`, `PRIV nick text`, `JOIN #room` and `FILE nick name size base64`,
and the server sends events like `MSG #room ts nick text`, `JOINED`, `PARTED`,
`TOPIC` and `ERR code message`.
Discovery is a UDP `SCHAT_DISCOVER` probe, answered with `SCHAT_HERE`. See
`src/protocol.h`, `src/server.cpp` and `src/client.cpp`.

## Project layout

```
src/protocol.*   framing, escaping, base64, validation
src/net.*        sockets, interfaces, broadcast addresses
src/server.*     single-threaded poll() server, rooms, admin console
src/client.*     client logic and chat commands
src/terminal.*   raw-mode line editor (completion, history)
src/discovery.*  LAN server discovery
tests/           unit tests + end-to-end integration script
```

## License and credits

MIT, see [LICENSE](LICENSE).

schat 2 is inspired by the original minimalist
[schat](https://github.com/adam-podkowinski/schat), a one-to-one chat. It is a
ground-up rewrite with a new protocol and contains none of the original code.
