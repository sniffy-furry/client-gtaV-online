// Minimal JSON for the relay protocol (objects, arrays, numbers, strings, bools, null).
#pragma once
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace mp {
struct JV {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false; double n = 0; std::string s;
  std::vector<JV> a; std::map<std::string, JV> o;
  bool isNum() const { return t == Num; }
  bool isStr() const { return t == Str; }
  bool isArr() const { return t == Arr; }
  const JV& at(const std::string& k) const { static const JV nul; auto i = o.find(k); return i == o.end() ? nul : i->second; }
  double num(double d = 0) const { return t == Num ? n : (t == Bool ? (b ? 1 : 0) : d); }
  int i(int d = 0) const { return t == Num ? (int)(long long)n : d; }
  uint32_t u(uint32_t d = 0) const { return t == Num ? (uint32_t)(long long)n : d; }
  std::string str(const std::string& d = "") const { return t == Str ? s : d; }
};
class JParser {
 public:
  static bool parse(const char* p, size_t n, JV& out) { JParser j(p, p + n); j.ws(); if (!j.val(out, 0)) return false; j.ws(); return true; }
 private:
  const char* p_; const char* e_;
  JParser(const char* p, const char* e) : p_(p), e_(e) {}
  void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++; }
  bool lit(const char* w) { size_t l = strlen(w); if ((size_t)(e_ - p_) >= l && !memcmp(p_, w, l)) { p_ += l; return true; } return false; }
  bool str(std::string& out) {
    if (p_ >= e_ || *p_ != '"') return false; p_++;
    while (p_ < e_ && *p_ != '"') {
      if (*p_ == '\\') {
        if (++p_ >= e_) return false;
        switch (*p_) { case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
          case 'u': { if (e_ - p_ < 5) return false; unsigned c = (unsigned)strtoul(std::string(p_ + 1, p_ + 5).c_str(), nullptr, 16); p_ += 4; out += c < 0x80 ? (char)c : '?'; break; }
          default: out += *p_; }
        p_++;
      } else out += *p_++;
    }
    if (p_ >= e_) return false; p_++; return true;
  }
  bool val(JV& v, int depth) {
    if (depth > 12 || p_ >= e_) return false;
    char c = *p_;
    if (c == '{') {
      v.t = JV::Obj; p_++; ws();
      if (p_ < e_ && *p_ == '}') { p_++; return true; }
      for (;;) {
        ws(); std::string k; if (!str(k)) return false; ws();
        if (p_ >= e_ || *p_ != ':') return false; p_++; ws();
        if (!val(v.o[k], depth + 1)) return false; ws();
        if (p_ < e_ && *p_ == ',') { p_++; continue; }
        if (p_ < e_ && *p_ == '}') { p_++; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.t = JV::Arr; p_++; ws();
      if (p_ < e_ && *p_ == ']') { p_++; return true; }
      for (;;) {
        ws(); v.a.emplace_back(); if (!val(v.a.back(), depth + 1)) return false; ws();
        if (p_ < e_ && *p_ == ',') { p_++; continue; }
        if (p_ < e_ && *p_ == ']') { p_++; return true; }
        return false;
      }
    }
    if (c == '"') { v.t = JV::Str; return str(v.s); }
    if (lit("true")) { v.t = JV::Bool; v.b = true; return true; }
    if (lit("false")) { v.t = JV::Bool; v.b = false; return true; }
    if (lit("null")) { v.t = JV::Null; return true; }
    char* endp = nullptr; double d = strtod(p_, &endp);
    if (endp == p_ || endp > e_) return false;
    v.t = JV::Num; v.n = d; p_ = endp; return true;
  }
};
inline std::string jesc(const std::string& s) {
  std::string r; r.reserve(s.size() + 2);
  for (unsigned char c : s) { if (c == '"' || c == '\\') { r += '\\'; r += (char)c; } else if (c < 0x20) r += ' '; else r += (char)c; }
  return r;
}
}  // namespace mp
