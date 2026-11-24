#include "terminal.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace schat {

static termios g_orig;
static bool g_rawActive = false;

static void restoreTerminal() {
  if (g_rawActive) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_rawActive = false;
  }
}

static void onFatalSignal(int sig) {
  restoreTerminal();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

static void out(const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

LineEditor::LineEditor(bool interactive) : interactive_(interactive) {
  if (!interactive_) return;
  if (tcgetattr(STDIN_FILENO, &g_orig) != 0) { interactive_ = false; return; }
  termios raw = g_orig;
  raw.c_lflag &= ~(ICANON | ECHO | IEXTEN);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { interactive_ = false; return; }
  g_rawActive = raw_ = true;
  std::atexit(restoreTerminal);
  for (int s : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT}) std::signal(s, onFatalSignal);
}

LineEditor::~LineEditor() { finish(); }

void LineEditor::finish() {
  if (raw_) {
    out("\r\033[K");
    restoreTerminal();
    raw_ = false;
  }
}

void LineEditor::setPrompt(const std::string& p) {
  prompt_ = p;
  if (interactive_) redraw();
}

size_t LineEditor::prevChar(size_t pos) const {
  if (pos == 0) return 0;
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(buf_[pos]) & 0xC0) == 0x80) --pos;
  return pos;
}

size_t LineEditor::nextChar(size_t pos) const {
  if (pos >= buf_.size()) return buf_.size();
  ++pos;
  while (pos < buf_.size() && (static_cast<unsigned char>(buf_[pos]) & 0xC0) == 0x80) ++pos;
  return pos;
}

size_t LineEditor::displayWidth(size_t from, size_t to) const {
  size_t w = 0;
  for (size_t i = from; i < to; ++i)
    if ((static_cast<unsigned char>(buf_[i]) & 0xC0) != 0x80) ++w;
  return w;
}

void LineEditor::redraw() {
  if (!interactive_) return;
  std::string s = "\r\033[K" + prompt_ + buf_;
  size_t back = displayWidth(cursor_, buf_.size());
  if (back) s += "\033[" + std::to_string(back) + "D";
  out(s);
}

void LineEditor::print(const std::string& text) {
  if (!interactive_) {
    out(text + "\n");
    return;
  }
  std::string t;
  for (char c : text) t += c == '\n' ? std::string("\r\n") : std::string(1, c);
  out("\r\033[K" + t + "\r\n");
  redraw();
}

void LineEditor::clearScreen() {
  if (interactive_) out("\033[2J\033[H");
  redraw();
}

void LineEditor::insert(const std::string& s) {
  buf_.insert(cursor_, s);
  cursor_ += s.size();
}

void LineEditor::feed(const char* data, size_t len, std::vector<std::string>& lines) {
  if (!interactive_) {
    plainPending_.append(data, len);
    size_t p;
    while ((p = plainPending_.find('\n')) != std::string::npos) {
      std::string l = plainPending_.substr(0, p);
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
      plainPending_.erase(0, p + 1);
    }
    return;
  }
  for (size_t i = 0; i < len; ++i) handleByte(static_cast<unsigned char>(data[i]), lines);
  redraw();
}

void LineEditor::handleByte(unsigned char c, std::vector<std::string>& lines) {
  if (inEsc_) {
    esc_ += static_cast<char>(c);
    // Sequences look like ESC [ ... final  or  ESC O x
    if (esc_.size() == 1 && c != '[' && c != 'O') { inEsc_ = false; esc_.clear(); return; }
    if (esc_.size() >= 2 && c >= 0x40 && c <= 0x7E) {
      handleEscape(esc_);
      inEsc_ = false;
      esc_.clear();
    } else if (esc_.size() > 8) {
      inEsc_ = false;
      esc_.clear();
    }
    return;
  }
  switch (c) {
    case 27: inEsc_ = true; esc_.clear(); return;
    case '\r':
    case '\n': {
      std::string line = buf_;
      out("\r\033[K");
      if (!line.empty() && (history_.empty() || history_.back() != line)) history_.push_back(line);
      if (history_.size() > 500) history_.erase(history_.begin());
      histPos_ = history_.size();
      buf_.clear();
      cursor_ = 0;
      lines.push_back(line);
      return;
    }
    case 127:
    case 8: {  // backspace
      size_t p = prevChar(cursor_);
      buf_.erase(p, cursor_ - p);
      cursor_ = p;
      return;
    }
    case 1: cursor_ = 0; return;              // Ctrl-A
    case 5: cursor_ = buf_.size(); return;    // Ctrl-E
    case 2: cursor_ = prevChar(cursor_); return;  // Ctrl-B
    case 6: cursor_ = nextChar(cursor_); return;  // Ctrl-F
    case 4:                                   // Ctrl-D: delete or EOF on empty line
      if (buf_.empty()) { lines.push_back("/quit"); return; }
      buf_.erase(cursor_, nextChar(cursor_) - cursor_);
      return;
    case 11: buf_.erase(cursor_); return;     // Ctrl-K
    case 12: out("\033[2J\033[H"); return;    // Ctrl-L
    case 21: buf_.erase(0, cursor_); cursor_ = 0; return;  // Ctrl-U
    case 23: {                                // Ctrl-W: delete previous word
      size_t p = cursor_;
      while (p > 0 && buf_[p - 1] == ' ') --p;
      while (p > 0 && buf_[p - 1] != ' ') --p;
      buf_.erase(p, cursor_ - p);
      cursor_ = p;
      return;
    }
    case '\t': complete(); return;
    default:
      if (c >= 32 || c >= 0x80) insert(std::string(1, static_cast<char>(c)));
  }
}

void LineEditor::handleEscape(const std::string& seq) {
  char f = seq.back();
  if (f == 'A' || f == 'B') {  // history up / down
    if (history_.empty()) return;
    if (histPos_ == history_.size()) histSaved_ = buf_;
    if (f == 'A' && histPos_ > 0) --histPos_;
    else if (f == 'B' && histPos_ < history_.size()) ++histPos_;
    buf_ = histPos_ == history_.size() ? histSaved_ : history_[histPos_];
    cursor_ = buf_.size();
  } else if (f == 'C') {
    cursor_ = nextChar(cursor_);
  } else if (f == 'D') {
    cursor_ = prevChar(cursor_);
  } else if (f == 'H' || seq == "[1~" || seq == "[7~") {
    cursor_ = 0;
  } else if (f == 'F' || seq == "[4~" || seq == "[8~") {
    cursor_ = buf_.size();
  } else if (seq == "[3~") {
    buf_.erase(cursor_, nextChar(cursor_) - cursor_);
  }
}

void LineEditor::complete() {
  if (!completer_) return;
  size_t start = cursor_;
  while (start > 0 && buf_[start - 1] != ' ') --start;
  std::string word = buf_.substr(start, cursor_ - start);
  bool first = buf_.find_first_not_of(' ') >= start;
  auto cands = completer_(word, first);
  if (cands.empty()) { out("\a"); return; }
  if (cands.size() == 1) {
    std::string c = cands[0];
    bool atLineStartNick = first && start == 0 && !c.empty() && c[0] != '/' && c[0] != '#';
    c += atLineStartNick ? ": " : " ";
    buf_.replace(start, cursor_ - start, c);
    cursor_ = start + c.size();
    return;
  }
  // Extend to the longest common prefix, then list candidates.
  std::string pre = cands[0];
  for (auto& c : cands) {
    size_t i = 0;
    while (i < pre.size() && i < c.size() && pre[i] == c[i]) ++i;
    pre.resize(i);
  }
  if (pre.size() > word.size()) {
    buf_.replace(start, cursor_ - start, pre);
    cursor_ = start + pre.size();
  } else {
    std::string list;
    for (auto& c : cands) list += c + "  ";
    print(list);
  }
}

}  // namespace schat
