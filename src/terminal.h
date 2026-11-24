// Minimal raw-mode line editor so incoming messages never clobber what the
// user is typing. Falls back to plain line reading when stdin is not a TTY.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace schat {

class LineEditor {
 public:
  using Completer = std::function<std::vector<std::string>(const std::string& word, bool firstWord)>;

  explicit LineEditor(bool interactive);
  ~LineEditor();

  bool interactive() const { return interactive_; }
  void setPrompt(const std::string& p);
  void setCompleter(Completer c) { completer_ = std::move(c); }

  // Feed raw bytes read from stdin; completed lines are appended to `lines`.
  void feed(const char* data, size_t len, std::vector<std::string>& lines);
  // Print a message above the input line, then redraw the input line.
  void print(const std::string& text);
  void redraw();
  void clearScreen();
  void finish();  // restore terminal; idempotent

 private:
  void handleByte(unsigned char c, std::vector<std::string>& lines);
  void handleEscape(const std::string& seq);
  void complete();
  void insert(const std::string& s);
  size_t prevChar(size_t pos) const;
  size_t nextChar(size_t pos) const;
  size_t displayWidth(size_t from, size_t to) const;

  bool interactive_;
  bool raw_ = false;
  std::string prompt_;
  std::string buf_;
  size_t cursor_ = 0;  // byte offset into buf_
  std::string esc_;    // partial escape sequence
  bool inEsc_ = false;
  std::string plainPending_;
  std::vector<std::string> history_;
  size_t histPos_ = 0;
  std::string histSaved_;
  Completer completer_;
};

}  // namespace schat
