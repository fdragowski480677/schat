#!/usr/bin/env bash
# End-to-end test: a real server plus scripted clients on localhost.
# Portable across Linux and macOS (bash 3.2, BSD grep).
set -u
BIN=${1:-build/schat}
DIR=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000)); DPORT=$((PORT + 1))
SRV=""
trap '[ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$DIR"' EXIT
fail() {
  echo "FAIL: $*"
  for f in server bad alice bob carol; do
    [ -f "$DIR/$f.out" ] && { echo "--- $f"; cat "$DIR/$f.out"; }
  done
  exit 1
}

"$BIN" server -p "$PORT" --discovery-port "$DPORT" -n testsrv -P secret \
  --log "$DIR/chat.log" --no-color < /dev/null > "$DIR/server.out" 2>&1 &
SRV=$!
sleep 1

# LAN discovery finds the server
"$BIN" discover --discovery-port "$DPORT" --no-color > "$DIR/disc.out"
grep -q "testsrv" "$DIR/disc.out" || fail "discovery did not find server"

# a wrong password is rejected
echo hi | "$BIN" client "127.0.0.1:$PORT" -u mallory -P wrong --no-color > "$DIR/bad.out" 2>&1
grep -q "Wrong password" "$DIR/bad.out" || fail "wrong password accepted"

# two clients talk (generous sleeps keep slow CI runners in order)
printf 'hello file\n' > "$DIR/note.txt"
{ sleep 1; echo "hi everyone"; echo "/me waves"; echo "/join #dev"; sleep 0.5
  echo "dev talk"; echo "/topic Shipping v2"; sleep 2.5; echo "/quit bye"; } |
  "$BIN" client "127.0.0.1:$PORT" -u alice -P secret --no-color > "$DIR/alice.out" 2>&1 &
A=$!
sleep 0.3
{ sleep 0.5; echo "/msg alice psst"; echo "/send alice $DIR/note.txt"; echo "/join #dev"; sleep 1.5
  echo "/who"; echo "/users"; sleep 1; echo "/quit"; } |
  "$BIN" client "127.0.0.1:$PORT" -u bob -P secret --no-color > "$DIR/bob.out" 2>&1 &
B=$!
wait $A $B

grep -q "\[bob -> you\] psst" "$DIR/alice.out" || fail "private message not received"
grep -q 'wants to send you "note.txt"' "$DIR/alice.out" || fail "file offer not received"
grep -q "alice: hi everyone" "$DIR/bob.out" || fail "room message not delivered"
grep -q "\* alice waves" "$DIR/bob.out" || fail "/me not delivered"
grep -q "alice: dev talk" "$DIR/bob.out" || fail "#dev message missing"
grep -q "Shipping v2" "$DIR/bob.out" || fail "topic not broadcast"
grep -q "Online (2)" "$DIR/bob.out" || fail "/users wrong"
grep -q "<alice> hi everyone" "$DIR/chat.log" || fail "log file missing message"

# a newcomer gets the room history replayed
{ sleep 1; echo "/quit"; } | "$BIN" client "127.0.0.1:$PORT" -u carol -P secret --no-color > "$DIR/carol.out" 2>&1
grep -q "~ .*alice: hi everyone" "$DIR/carol.out" || fail "history not replayed"

echo "all integration tests passed"
