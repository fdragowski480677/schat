#include <cstdio>
#include <string>

#include "net.h"
#include "protocol.h"

using namespace schat;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
  // escape round-trip, including every special character
  std::string nasty = "tab\there\nnew\\line\rcr \\t literal";
  auto f = decode(encode({"MSG", "#lobby", nasty, ""}).substr(0, encode({"MSG", "#lobby", nasty, ""}).size() - 1));
  CHECK(f.size() == 4);
  CHECK(f[2] == nasty);
  CHECK(f[3].empty());
  CHECK(encode({"a", "b"}) == "a\tb\n");

  // base64
  for (std::string s : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
    std::string d;
    CHECK(base64Decode(base64Encode(s), d) && d == s);
  }
  CHECK(base64Encode("foobar") == "Zm9vYmFy");
  std::string bin;
  for (int i = 0; i < 256; ++i) bin += char(i);
  std::string d;
  CHECK(base64Decode(base64Encode(bin), d) && d == bin);
  CHECK(!base64Decode("abc", d));
  CHECK(!base64Decode("a=bc", d));

  // validation
  CHECK(validNick("alice_42"));
  CHECK(!validNick(""));
  CHECK(!validNick("has space"));
  CHECK(!validNick("waytoolongnickname_12345"));
  CHECK(validRoom("#lobby"));
  CHECK(!validRoom("lobby"));
  CHECK(!validRoom("#UPPER"));
  CHECK(normalizeRoom("Games") == "#games");
  CHECK(normalizeRoom("#dev") == "#dev");

  // line buffer splits partial reads correctly
  LineBuffer lb;
  std::string l;
  lb.append("hel", 3);
  CHECK(!lb.next(l));
  lb.append("lo\nwor", 6);
  CHECK(lb.next(l) && l == "hello");
  CHECK(!lb.next(l));
  lb.append("ld\n\n", 4);
  CHECK(lb.next(l) && l == "world");
  CHECK(lb.next(l) && l.empty());

  // host:port parsing
  std::string h;
  uint16_t p = 1;
  CHECK(splitHostPort("10.0.0.5:7000", h, p) && h == "10.0.0.5" && p == 7000);
  p = 1;
  CHECK(splitHostPort("myhost", h, p) && h == "myhost" && p == 1);
  CHECK(!splitHostPort("x:99999", h, p));
  CHECK(splitHostPort("fe80::1", h, p) && h == "fe80::1");

  std::printf(failures ? "%d failure(s)\n" : "all protocol tests passed\n", failures);
  return failures ? 1 : 0;
}
