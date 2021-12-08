// vim: set sw=2 ts=2 sts=2 et tw=100:
//
// pdf-simple-sign: simple PDF signer
//
// Copyright (c) 2017 - 2020, Přemysl Eric Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

#include <cmath>
#include <cstdio>
#undef NDEBUG
#include <cassert>

#include <map>
#include <memory>
#include <regex>
#include <set>
#include <vector>

#if defined __GLIBCXX__ && __GLIBCXX__ < 20140422
#error Need libstdc++ >= 4.9 for <regex>
#endif

#include <getopt.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include "config.h"

// -------------------------------------------------------------------------------------------------

using uint = unsigned int;
using ushort = unsigned short;

static std::string concatenate(const std::vector<std::string>& v, const std::string& delim) {
  std::string res;
  if (v.empty())
    return res;
  for (const auto& s : v)
    res += s + delim;
  return res.substr(0, res.length() - delim.length());
}

template<typename... Args>
std::string ssprintf(const std::string& format, Args... args) {
  size_t size = std::snprintf(nullptr, 0, format.c_str(), args...) + 1;
  std::unique_ptr<char[]> buf(new char[size]);
  std::snprintf(buf.get(), size, format.c_str(), args...);
  return std::string(buf.get(), buf.get() + size - 1);
}

// -------------------------------------------------------------------------------------------------

/// PDF token/object thingy.  Objects may be composed either from one or a sequence of tokens.
/// The PDF Reference doesn't actually speak of tokens, though ISO 32000-1:2008 does.
struct pdf_object {
  enum type {
    END, NL, COMMENT, NIL, BOOL, NUMERIC, KEYWORD, NAME, STRING,
    // Simple tokens
    B_ARRAY, E_ARRAY, B_DICT, E_DICT,
    // Higher-level objects
    ARRAY, DICT, OBJECT, REFERENCE,
  } type = END;

  std::string string;                      ///< END (error message), COMMENT/KEYWORD/NAME/STRING
  double number = 0.;                      ///< BOOL, NUMERIC
  std::vector<pdf_object> array;           ///< ARRAY, OBJECT
  std::map<std::string, pdf_object> dict;  ///< DICT, in the future also STREAM
  uint n = 0, generation = 0;              ///< OBJECT, REFERENCE

  pdf_object(enum type type = END)                          : type(type) {}
  pdf_object(enum type type, double v)                      : type(type), number(v) {}
  pdf_object(enum type type, const std::string& v)          : type(type), string(v) {}
  pdf_object(enum type type, uint n, uint g)                : type(type), n(n), generation(g) {}
  pdf_object(const std::vector<pdf_object>& array)          : type(ARRAY), array(array) {}
  pdf_object(const std::map<std::string, pdf_object>& dict) : type(DICT), dict(dict) {}

  pdf_object(const pdf_object&)            = default;
  pdf_object(pdf_object&&)                 = default;
  pdf_object& operator=(const pdf_object&) = default;
  pdf_object& operator=(pdf_object&&)      = default;

  /// Return whether this is a number without a fractional part
  bool is_integer() const {
    double tmp;
    return type == NUMERIC && std::modf(number, &tmp) == 0.;
  }
};

/// Basic lexical analyser for the Portable Document Format, giving limited error information
struct pdf_lexer {
  const unsigned char* p;
  pdf_lexer(const char* s) : p(reinterpret_cast<const unsigned char*>(s)) {}

  static constexpr const char* oct_alphabet = "01234567";
  static constexpr const char* dec_alphabet = "0123456789";
  static constexpr const char* hex_alphabet = "0123456789abcdefABCDEF";
  static constexpr const char* whitespace = "\t\n\f\r ";
  static constexpr const char* delimiters = "()<>[]{}/%";

  bool eat_newline(int ch) {
    if (ch == '\r') {
      if (*p == '\n') p++;
      return true;
    }
    return ch == '\n';
  }

  pdf_object string() {
    std::string value;
    int parens = 1;
    while (1) {
      if (!*p) return {pdf_object::END, "unexpected end of string"};
      auto ch = *p++;
      if (eat_newline(ch)) ch = '\n';
      else if (ch == '(') { parens++; }
      else if (ch == ')') { if (!--parens) break; }
      else if (ch == '\\') {
        if (!*p) return {pdf_object::END, "unexpected end of string"};
        switch ((ch = *p++)) {
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        default:
          if (eat_newline(ch))
            continue;
          std::string octal;
          if (ch && strchr(oct_alphabet, ch)) {
            octal += ch;
            if (*p && strchr(oct_alphabet, *p)) octal += *p++;
            if (*p && strchr(oct_alphabet, *p)) octal += *p++;
            ch = std::stoi(octal, nullptr, 8);
          }
        }
      }
      value += ch;
    }
    return {pdf_object::STRING, value};
  }

  pdf_object string_hex() {
    std::string value, buf;
    while (*p != '>') {
      if (!*p) return {pdf_object::END, "unexpected end of hex string"};
      if (!strchr(hex_alphabet, *p))
        return {pdf_object::END, "invalid hex string"};
      buf += *p++;
      if (buf.size() == 2) {
        value += char(std::stoi(buf, nullptr, 16));
        buf.clear();
      }
    }
    p++;
    if (!buf.empty()) value += char(std::stoi(buf + '0', nullptr, 16));
    return {pdf_object::STRING, value};
  }

  pdf_object name() {
    std::string value;
    while (!strchr(whitespace, *p) && !strchr(delimiters, *p)) {
      auto ch = *p++;
      if (ch == '#') {
        std::string hexa;
        if (*p && strchr(hex_alphabet, *p)) hexa += *p++;
        if (*p && strchr(hex_alphabet, *p)) hexa += *p++;
        if (hexa.size() != 2)
          return {pdf_object::END, "invalid name hexa escape"};
        ch = char(std::stoi(hexa, nullptr, 16));
      }
      value += ch;
    }
    if (value.empty()) return {pdf_object::END, "unexpected end of name"};
    return {pdf_object::NAME, value};
  }

  pdf_object comment() {
    std::string value;
    while (*p && *p != '\r' && *p != '\n')
      value += *p++;
    return {pdf_object::COMMENT, value};
  }

  // XXX maybe invalid numbers should rather be interpreted as keywords
  pdf_object number() {
    std::string value;
    if (*p == '-')
      value += *p++;
    bool real = false, digits = false;
    while (*p) {
      if (strchr(dec_alphabet, *p))
        digits = true;
      else if (*p == '.' && !real)
        real = true;
      else
        break;
      value += *p++;
    }
    if (!digits) return {pdf_object::END, "invalid number"};
    return {pdf_object::NUMERIC, std::stod(value, nullptr)};
  }

  pdf_object next() {
    if (!*p)
      return {pdf_object::END};
    if (strchr("-0123456789.", *p))
      return number();

    // {} end up being keywords, we might want to error out on those
    std::string value;
    while (!strchr(whitespace, *p) && !strchr(delimiters, *p))
      value += *p++;
    if (!value.empty()) {
      if (value == "null")  return {pdf_object::NIL};
      if (value == "true")  return {pdf_object::BOOL, 1};
      if (value == "false") return {pdf_object::BOOL, 0};
      return {pdf_object::KEYWORD, value};
    }

    switch (char ch = *p++) {
    case '/': return name();
    case '%': return comment();
    case '(': return string();
    case '[': return {pdf_object::B_ARRAY};
    case ']': return {pdf_object::E_ARRAY};
    case '<':
      if (*p++ == '<')
        return {pdf_object::B_DICT};
      p--;
      return string_hex();
    case '>':
      if (*p++ == '>')
        return {pdf_object::E_DICT};
      p--;
      return {pdf_object::END, "unexpected '>'"};
    default:
      if (eat_newline(ch))
        return {pdf_object::NL};
      if (strchr(whitespace, ch))
        return next();
      return {pdf_object::END, "unexpected input"};
    }
  }
};

// FIXME lines /should not/ be longer than 255 characters, some wrapping is in order
static std::string pdf_serialize(const pdf_object& o) {
  switch (o.type) {
  case pdf_object::NL:      return "\n";
  case pdf_object::NIL:     return "null";
  case pdf_object::BOOL:    return o.number ? "true" : "false";
  case pdf_object::NUMERIC: {
    if (o.is_integer()) return std::to_string((long long) o.number);
    return std::to_string(o.number);
  }
  case pdf_object::KEYWORD: return o.string;
  case pdf_object::NAME: {
    std::string escaped = "/";
    for (char c : o.string) {
      if (c == '#' || strchr(pdf_lexer::delimiters, c) || strchr(pdf_lexer::whitespace, c))
        escaped += ssprintf("#%02x", c);
      else
        escaped += c;
    }
    return escaped;
  }
  case pdf_object::STRING: {
    std::string escaped;
    for (char c : o.string) {
      if (c == '\\' || c == '(' || c == ')')
        escaped += '\\';
      escaped += c;
    }
    return "(" + escaped + ")";
  }
  case pdf_object::B_ARRAY: return "[";
  case pdf_object::E_ARRAY: return "]";
  case pdf_object::B_DICT:  return "<<";
  case pdf_object::E_DICT:  return ">>";
  case pdf_object::ARRAY: {
    std::vector<std::string> v;
    for (const auto& i : o.array)
      v.push_back(pdf_serialize(i));
    return "[ " + concatenate(v, " ") + " ]";
  }
  case pdf_object::DICT: {
    std::string s;
    for (const auto i : o.dict)
      // FIXME the key is also supposed to be escaped by pdf_serialize()
      s += " /" + i.first + " " + pdf_serialize(i.second);
    return "<<" + s + " >>";
  }
  case pdf_object::OBJECT:
    return ssprintf("%u %u obj\n", o.n, o.generation) + pdf_serialize(o.array.at(0)) + "\nendobj";
  case pdf_object::REFERENCE:
    return ssprintf("%u %u R", o.n, o.generation);
  default:
    assert(!"unsupported token for serialization");
  }
}

// -------------------------------------------------------------------------------------------------

/// Utility class to help read and possibly incrementally update PDF files
class pdf_updater {
  struct ref {
    size_t offset = 0;     ///< File offset or N of the next free entry
    uint generation = 0;   ///< Object generation
    bool free = true;      ///< Whether this N has been deleted
  };

  std::vector<ref> xref;   ///< Cross-reference table
  size_t xref_size = 0;    ///< Current cross-reference table size, correlated to xref.size()
  std::set<uint> updated;  ///< List of updated objects

  pdf_object parse_obj(pdf_lexer& lex, std::vector<pdf_object>& stack) const;
  pdf_object parse_R(std::vector<pdf_object>& stack) const;
  pdf_object parse(pdf_lexer& lex, std::vector<pdf_object>& stack) const;
  std::string load_xref(pdf_lexer& lex, std::set<uint>& loaded_entries);

public:
  /// The new trailer dictionary to be written, initialized with the old one
  std::map<std::string, pdf_object> trailer;

  std::string& document;
  pdf_updater(std::string& document) : document(document) {}

  /// Build the cross-reference table and prepare a new trailer dictionary
  std::string initialize();
  /// Try to extract the claimed PDF version as a positive decimal number, e.g. 17 for PDF 1.7.
  /// Returns zero on failure.
  int version(const pdf_object& root) const;
  /// Retrieve an object by its number and generation -- may return NIL or END with an error
  pdf_object get(uint n, uint generation) const;
  /// Allocate a new object number
  uint allocate();
  /// Append an updated object to the end of the document
  void update(uint n, std::function<void()> fill);
  /// Write an updated cross-reference table and trailer
  void flush_updates();
};

// -------------------------------------------------------------------------------------------------

/// If the object is an error, forward its message, otherwise return err.
static std::string pdf_error(const pdf_object& o, const char* err) {
  if (o.type != pdf_object::END || o.string.empty()) return err;
  return o.string;
}

pdf_object pdf_updater::parse_obj(pdf_lexer& lex, std::vector<pdf_object>& stack) const {
  if (stack.size() < 2)
    return {pdf_object::END, "missing object ID pair"};

  auto g = stack.back(); stack.pop_back();
  auto n = stack.back(); stack.pop_back();
  if (!g.is_integer() || g.number < 0 || g.number > UINT_MAX ||
      !n.is_integer() || n.number < 0 || n.number > UINT_MAX)
    return {pdf_object::END, "invalid object ID pair"};

  pdf_object obj{pdf_object::OBJECT};
  obj.n = n.number;
  obj.generation = g.number;

  while (1) {
    auto object = parse(lex, obj.array);
    if (object.type == pdf_object::END)
      return {pdf_object::END, pdf_error(object, "object doesn't end")};
    if (object.type == pdf_object::KEYWORD && object.string == "endobj")
      break;
    obj.array.push_back(std::move(object));
  }
  return obj;
}

pdf_object pdf_updater::parse_R(std::vector<pdf_object>& stack) const {
  if (stack.size() < 2)
    return {pdf_object::END, "missing reference ID pair"};

  auto g = stack.back(); stack.pop_back();
  auto n = stack.back(); stack.pop_back();
  if (!g.is_integer() || g.number < 0 || g.number > UINT_MAX ||
      !n.is_integer() || n.number < 0 || n.number > UINT_MAX)
    return {pdf_object::END, "invalid reference ID pair"};

  pdf_object ref{pdf_object::REFERENCE};
  ref.n = n.number;
  ref.generation = g.number;
  return ref;
}

/// Read an object at the lexer's position.  Not a strict parser.
pdf_object pdf_updater::parse(pdf_lexer& lex, std::vector<pdf_object>& stack) const {
  auto token = lex.next();
  switch (token.type) {
  case pdf_object::NL:
  case pdf_object::COMMENT:
    // These are not important to parsing, not even for this procedure's needs
    return parse(lex, stack);
  case pdf_object::B_ARRAY: {
    std::vector<pdf_object> array;
    while (1) {
      auto object = parse(lex, array);
      if (object.type == pdf_object::END)
        return {pdf_object::END, pdf_error(object, "array doesn't end")};
      if (object.type == pdf_object::E_ARRAY)
        break;
      array.push_back(std::move(object));
    }
    return array;
  }
  case pdf_object::B_DICT: {
    std::vector<pdf_object> array;
    while (1) {
      auto object = parse(lex, array);
      if (object.type == pdf_object::END)
        return {pdf_object::END, pdf_error(object, "dictionary doesn't end")};
      if (object.type == pdf_object::E_DICT)
        break;
      array.push_back(std::move(object));
    }
    if (array.size() % 2)
      return {pdf_object::END, "unbalanced dictionary"};
    std::map<std::string, pdf_object> dict;
    for (size_t i = 0; i < array.size(); i += 2) {
      if (array[i].type != pdf_object::NAME)
        return {pdf_object::END, "invalid dictionary key type"};
      dict.insert({array[i].string, std::move(array[i + 1])});
    }
    return dict;
  }
  case pdf_object::KEYWORD:
    // Appears in the document body, typically needs to access the cross-reference table
    // TODO use the xref to read /Length etc. once we actually need to read such objects;
    //   presumably streams can use the pdf_object::string member
    if (token.string == "stream") return {pdf_object::END, "streams are not supported yet"};
    if (token.string == "obj")    return parse_obj(lex, stack);
    if (token.string == "R")      return parse_R(stack);
    return token;
  default:
    return token;
  }
}

std::string pdf_updater::load_xref(pdf_lexer& lex, std::set<uint>& loaded_entries) {
  std::vector<pdf_object> throwaway_stack;
  {
    auto keyword = parse(lex, throwaway_stack);
    if (keyword.type != pdf_object::KEYWORD || keyword.string != "xref")
      return "invalid xref table";
  }
  while (1) {
    auto object = parse(lex, throwaway_stack);
    if (object.type == pdf_object::END)
      return pdf_error(object, "unexpected EOF while looking for the trailer");
    if (object.type == pdf_object::KEYWORD && object.string == "trailer")
      break;

    auto second = parse(lex, throwaway_stack);
    if (!object.is_integer() || object.number < 0 || object.number > UINT_MAX ||
        !second.is_integer() || second.number < 0 || second.number > UINT_MAX)
      return "invalid xref section header";

    const size_t start = object.number;
    const size_t count = second.number;
    for (size_t i = 0; i < count; i++) {
      auto off = parse(lex, throwaway_stack);
      auto gen = parse(lex, throwaway_stack);
      auto key = parse(lex, throwaway_stack);
      if (!off.is_integer() || off.number < 0 || off.number > document.length() ||
          !gen.is_integer() || gen.number < 0 || gen.number > 65535 ||
          key.type != pdf_object::KEYWORD)
        return "invalid xref entry";

      bool free = true;
      if (key.string == "n")
        free = false;
      else if (key.string != "f")
        return "invalid xref entry";

      auto n = start + i;
      if (loaded_entries.count(n))
        continue;
      if (n >= xref.size())
        xref.resize(n + 1);
      loaded_entries.insert(n);

      auto& ref = xref[n];
      ref.generation = gen.number;
      ref.offset = off.number;
      ref.free = free;
    }
  }
  return "";
}

// -------------------------------------------------------------------------------------------------

std::string pdf_updater::initialize() {
  // We only need to look for startxref roughly within the last kibibyte of the document
  static std::regex haystack_re(R"([\s\S]*\sstartxref\s+(\d+)\s+%%EOF)");
  std::string haystack = document.substr(document.length() < 1024 ? 0 : document.length() - 1024);

  std::smatch m;
  if (!std::regex_search(haystack, m, haystack_re, std::regex_constants::match_continuous))
    return "cannot find startxref";

  size_t xref_offset = std::stoul(m.str(1)), last_xref_offset = xref_offset;
  std::set<size_t> loaded_xrefs;
  std::set<uint> loaded_entries;

  std::vector<pdf_object> throwaway_stack;
  while (1) {
    if (loaded_xrefs.count(xref_offset))
      return "circular xref offsets";
    if (xref_offset >= document.length())
      return "invalid xref offset";

    pdf_lexer lex(document.c_str() + xref_offset);
    auto err = load_xref(lex, loaded_entries);
    if (!err.empty()) return err;

    auto trailer = parse(lex, throwaway_stack);
    if (trailer.type != pdf_object::DICT)
      return pdf_error(trailer, "invalid trailer dictionary");
    if (loaded_xrefs.empty())
      this->trailer = trailer.dict;
    loaded_xrefs.insert(xref_offset);

    const auto prev_offset = trailer.dict.find("Prev");
    if (prev_offset == trailer.dict.end())
      break;
    // FIXME do not read offsets and sizes as floating point numbers
    if (!prev_offset->second.is_integer() || prev_offset->second.number < 0)
      return "invalid Prev offset";
    xref_offset = prev_offset->second.number;
  }

  trailer["Prev"] = {pdf_object::NUMERIC, double(last_xref_offset)};
  const auto last_size = trailer.find("Size");
  if (last_size == trailer.end() || !last_size->second.is_integer() ||
      last_size->second.number <= 0)
    return "invalid or missing cross-reference table Size";

  xref_size = last_size->second.number;
  return "";
}

int pdf_updater::version(const pdf_object& root) const {
  auto version = root.dict.find("Version");
  if (version != root.dict.end() && version->second.type == pdf_object::NAME) {
    const auto& v = version->second.string;
    if (isdigit(v[0]) && v[1] == '.' && isdigit(v[2]) && !v[3])
      return (v[0] - '0') * 10 + (v[2] - '0');
  }

  // We only need to look for the comment roughly within the first kibibyte of the document
  static std::regex version_re(R"((?:^|[\r\n])%(?:!PS-Adobe-\d\.\d )?PDF-(\d)\.(\d)[\r\n])");
  std::string haystack = document.substr(0, 1024);

  std::smatch m;
  if (std::regex_search(haystack, m, version_re, std::regex_constants::match_default))
    return std::stoul(m.str(1)) * 10 + std::stoul(m.str(2));

  return 0;
}

pdf_object pdf_updater::get(uint n, uint generation) const {
  if (n >= xref_size)
    return {pdf_object::NIL};

  const auto& ref = xref[n];
  if (ref.free || ref.generation != generation || ref.offset >= document.length())
    return {pdf_object::NIL};

  pdf_lexer lex(document.c_str() + ref.offset);
  std::vector<pdf_object> stack;
  while (1) {
    auto object = parse(lex, stack);
    if (object.type == pdf_object::END)
      return object;
    if (object.type != pdf_object::OBJECT)
      stack.push_back(std::move(object));
    else if (object.n != n || object.generation != generation)
      return {pdf_object::END, "object mismatch"};
    else
      return std::move(object.array.at(0));
  }
}

uint pdf_updater::allocate() {
  assert(xref_size < UINT_MAX);

  auto n = xref_size++;
  if (xref.size() < xref_size)
    xref.resize(xref_size);

  // We don't make sure it gets a subsection in the update yet because we
  // make no attempts at fixing the linked list of free items either
  return n;
}

void pdf_updater::update(uint n, std::function<void()> fill) {
  auto& ref = xref.at(n);
  ref.offset = document.length() + 1;
  ref.free = false;
  updated.insert(n);

  document += ssprintf("\n%u %u obj\n", n, ref.generation);
  // Separately so that the callback can use document.length() to get the current offset
  fill();
  document += "\nendobj";
}

void pdf_updater::flush_updates() {
  std::map<uint, size_t> groups;
  for (auto i = updated.cbegin(); i != updated.cend(); ) {
    size_t start = *i, count = 1;
    while (++i != updated.cend() && *i == start + count)
      count++;
    groups[start] = count;
  }

  // Taking literally "Each cross-reference section begins with a line containing the keyword xref.
  // Following this line are one or more cross-reference subsections." from 3.4.3 in PDF Reference
  if (groups.empty())
    groups[0] = 0;

  auto startxref = document.length() + 1;
  document += "\nxref\n";
  for (const auto& g : groups) {
    document += ssprintf("%u %zu\n", g.first, g.second);
    for (size_t i = 0; i < g.second; i++) {
      auto& ref = xref[g.first + i];
      document += ssprintf("%010zu %05u %c \n", ref.offset, ref.generation, "nf"[!!ref.free]);
    }
  }

  trailer["Size"] = {pdf_object::NUMERIC, double(xref_size)};
  document +=
    "trailer\n" + pdf_serialize(trailer) + ssprintf("\nstartxref\n%zu\n%%%%EOF\n", startxref);
}

// -------------------------------------------------------------------------------------------------

/// Make a PDF object representing the given point in time
static pdf_object pdf_date(time_t timestamp) {
  struct tm parts;
  assert(localtime_r(&timestamp, &parts));

  char buf[64];
  assert(strftime(buf, sizeof buf, "D:%Y%m%d%H%M%S", &parts));

  std::string offset = "Z";
  auto offset_min = parts.tm_gmtoff / 60;
  if (parts.tm_gmtoff < 0)
    offset = ssprintf("-%02ld'%02ld'", -offset_min / 60, -offset_min % 60);
  if (parts.tm_gmtoff > 0)
    offset = ssprintf("+%02ld'%02ld'", +offset_min / 60, +offset_min % 60);
  return {pdf_object::STRING, buf + offset};
}

static pdf_object pdf_get_first_page(pdf_updater& pdf, uint node_n, uint node_generation) {
  auto obj = pdf.get(node_n, node_generation);
  if (obj.type != pdf_object::DICT)
    return {pdf_object::NIL};

  // Out of convenience; these aren't filled normally
  obj.n = node_n;
  obj.generation = node_generation;

  auto type = obj.dict.find("Type");
  if (type == obj.dict.end() || type->second.type != pdf_object::NAME)
    return {pdf_object::NIL};
  if (type->second.string == "Page")
    return obj;
  if (type->second.string != "Pages")
    return {pdf_object::NIL};

  // XXX technically speaking, this may be an indirect reference.  The correct way to solve this
  //   seems to be having "pdf_updater" include a wrapper around "obj.dict.find"
  auto kids = obj.dict.find("Kids");
  if (kids == obj.dict.end() || kids->second.type != pdf_object::ARRAY ||
      kids->second.array.empty() ||
      kids->second.array.at(0).type != pdf_object::REFERENCE)
    return {pdf_object::NIL};

  // XXX nothing prevents us from recursing in an evil circular graph
  return pdf_get_first_page(pdf, kids->second.array.at(0).n, kids->second.array.at(0).generation);
}

// -------------------------------------------------------------------------------------------------

static std::string pkcs12_path, pkcs12_pass;

// /All/ bytes are checked, except for the signature hexstring itself
static std::string pdf_fill_in_signature(std::string& document, size_t sign_off, size_t sign_len) {
  size_t tail_off = sign_off + sign_len, tail_len = document.size() - tail_off;
  if (pkcs12_path.empty())
    return "undefined path to the signing key";

  auto pkcs12_fp = fopen(pkcs12_path.c_str(), "r");
  if (!pkcs12_fp)
    return pkcs12_path + ": " + strerror(errno);

  // Abandon hope, all ye who enter OpenSSL!  Half of it is undocumented.
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();
  ERR_clear_error();

  PKCS12* p12 = nullptr;
  EVP_PKEY* private_key = nullptr;
  X509* certificate = nullptr;
  STACK_OF(X509)* chain = nullptr;
  PKCS7* p7 = nullptr;
  int len = 0, sign_flags = PKCS7_DETACHED | PKCS7_BINARY | PKCS7_NOSMIMECAP | PKCS7_PARTIAL;
  BIO* p7bio = nullptr;
  unsigned char* buf = nullptr;

  // OpenSSL error reasons will usually be of more value than any distinction I can come up with
  std::string err = "OpenSSL failure";

  if (!(p12 = d2i_PKCS12_fp(pkcs12_fp, nullptr)) ||
      !PKCS12_parse(p12, pkcs12_pass.c_str(), &private_key, &certificate, &chain)) {
    err = pkcs12_path + ": parse failure";
    goto error;
  }
  if (!private_key || !certificate) {
    err = pkcs12_path + ": must contain a private key and a valid certificate chain";
    goto error;
  }
  // Prevent useless signatures -- makes pdfsig from poppler happy at least (and NSS by extension)
  if (!(X509_get_key_usage(certificate) & (KU_DIGITAL_SIGNATURE | KU_NON_REPUDIATION))) {
    err = "the certificate's key usage must include digital signatures or non-repudiation";
    goto error;
  }
  if (!(X509_get_extended_key_usage(certificate) & (XKU_SMIME | XKU_ANYEKU))) {
    err = "the certificate's extended key usage must include S/MIME";
    goto error;
  }
#if 0  // This happily ignores XKU_ANYEKU and I want my tiny world to make a tiny bit more sense
  if (X509_check_purpose(certificate, X509_PURPOSE_SMIME_SIGN, false /* not a CA certificate */)) {
    err = "the certificate can't be used for S/MIME digital signatures";
    goto error;
  }
#endif

  // The default digest is SHA1, which is mildly insecure now -- hence using PKCS7_sign_add_signer
  if (!(p7 = PKCS7_sign(nullptr, nullptr, nullptr, nullptr, sign_flags)) ||
      !PKCS7_sign_add_signer(p7, certificate, private_key, EVP_sha256(), sign_flags))
    goto error;
  // For RFC 3161, this is roughly how a timestamp token would be attached (see Appendix A):
  //   PKCS7_add_attribute(signer_info, NID_id_smime_aa_timeStampToken, V_ASN1_SEQUENCE, value)
  for (int i = 0; i < sk_X509_num(chain); i++)
    if (!PKCS7_add_certificate(p7, sk_X509_value(chain, i)))
      goto error;

  // Adaptation of the innards of the undocumented PKCS7_final() -- I didn't feel like making
  // a copy of the whole document.  Hopefully this writes directly into a digest BIO.
  if (!(p7bio = PKCS7_dataInit(p7, nullptr)) ||
      (ssize_t) sign_off != BIO_write(p7bio, document.data(), sign_off) ||
      (ssize_t) tail_len != BIO_write(p7bio, document.data() + tail_off, tail_len) ||
      BIO_flush(p7bio) != 1 || !PKCS7_dataFinal(p7, p7bio))
    goto error;

#if 0
  {
    // Debugging: openssl cms -inform PEM -in pdf_signature.pem -noout -cmsout -print
    // Context: https://stackoverflow.com/a/29253469
    auto fp = fopen("pdf_signature.pem", "wb");
    assert(PEM_write_PKCS7(fp, p7) && !fclose(fp));
  }
#endif

  if ((len = i2d_PKCS7(p7, &buf)) < 0)
    goto error;
  if (size_t(len) * 2 > sign_len - 2 /* hexstring quotes */) {
    // The obvious solution is to increase the allocation... or spend a week reading specifications
    // while losing all faith in humanity as a species, and skip the PKCS7 API entirely
    err = ssprintf("not enough space reserved for the signature (%zu nibbles vs %zu nibbles)",
                   sign_len - 2, size_t(len) * 2);
    goto error;
  }
  for (int i = 0; i < len; i++) {
    document[sign_off + 2 * i + 1] = "0123456789abcdef"[buf[i] / 16];
    document[sign_off + 2 * i + 2] = "0123456789abcdef"[buf[i] % 16];
  }
  err.clear();

error:
  OPENSSL_free(buf);
  BIO_free_all(p7bio);
  PKCS7_free(p7);
  sk_X509_pop_free(chain, X509_free);
  X509_free(certificate);
  EVP_PKEY_free(private_key);
  PKCS12_free(p12);

  // In any case, clear the error stack (it's a queue, really) to avoid confusion elsewhere
  while (auto code = ERR_get_error())
    if (auto reason = ERR_reason_error_string(code))
      err = err + "; " + reason;

  fclose(pkcs12_fp);
  return err;
}

// -------------------------------------------------------------------------------------------------

/// The presumption here is that the document is valid and that it doesn't employ cross-reference
/// streams from PDF 1.5, or at least constitutes a hybrid-reference file.  The results with
/// PDF 2.0 (2017) are currently unknown as the standard costs money.
///
/// https://www.adobe.com/devnet-docs/acrobatetk/tools/DigSig/Acrobat_DigitalSignatures_in_PDF.pdf
/// https://www.adobe.com/content/dam/acom/en/devnet/acrobat/pdfs/pdf_reference_1-7.pdf
/// https://www.adobe.com/content/dam/acom/en/devnet/acrobat/pdfs/PPKAppearances.pdf
static std::string pdf_sign(std::string& document, ushort reservation) {
  pdf_updater pdf(document);
  auto err = pdf.initialize();
  if (!err.empty())
    return err;

  auto root_ref = pdf.trailer.find("Root");
  if (root_ref == pdf.trailer.end() || root_ref->second.type != pdf_object::REFERENCE)
    return "trailer does not contain a reference to Root";
  auto root = pdf.get(root_ref->second.n, root_ref->second.generation);
  if (root.type != pdf_object::DICT)
    return "invalid Root dictionary reference";

  // 8.7 Digital Signatures - /signature dictionary/
  auto sigdict_n = pdf.allocate();
  size_t byterange_off = 0, byterange_len = 0, sign_off = 0, sign_len = 0;
  pdf.update(sigdict_n, [&] {
    // The timestamp is important for Adobe Acrobat Reader DC.  The ideal would be to use RFC 3161.
    pdf.document.append("<< /Type/Sig /Filter/Adobe.PPKLite /SubFilter/adbe.pkcs7.detached\n"
                        "   /M" + pdf_serialize(pdf_date(time(nullptr))) + " /ByteRange ");
    byterange_off = pdf.document.size();
    pdf.document.append((byterange_len = 32 /* fine for a gigabyte */), ' ');
    pdf.document.append("\n   /Contents <");
    sign_off = pdf.document.size();
    pdf.document.append((sign_len = reservation * 2), '0');
    pdf.document.append("> >>");

    // We actually need to exclude the hexstring quotes from signing
    sign_off -= 1;
    sign_len += 2;
  });

  // 8.6.3 Field Types - Signature Fields
  pdf_object sigfield{pdf_object::DICT};
  sigfield.dict.insert({"FT", {pdf_object::NAME, "Sig"}});
  sigfield.dict.insert({"V", {pdf_object::REFERENCE, sigdict_n, 0}});
  // 8.4.5 Annotations Types - Widget Annotations
  // We can merge the Signature Annotation and omit Kids here
  sigfield.dict.insert({"Subtype", {pdf_object::NAME, "Widget"}});
  sigfield.dict.insert({"F", {pdf_object::NUMERIC, 2 /* Hidden */}});
  sigfield.dict.insert({"T", {pdf_object::STRING, "Signature1"}});
  sigfield.dict.insert({"Rect", {std::vector<pdf_object>{
    {pdf_object::NUMERIC, 0},
    {pdf_object::NUMERIC, 0},
    {pdf_object::NUMERIC, 0},
    {pdf_object::NUMERIC, 0},
  }}});

  auto sigfield_n = pdf.allocate();
  pdf.update(sigfield_n, [&] { pdf.document += pdf_serialize(sigfield); });

  auto pages_ref = root.dict.find("Pages");
  if (pages_ref == root.dict.end() || pages_ref->second.type != pdf_object::REFERENCE)
    return "invalid Pages reference";
  auto page = pdf_get_first_page(pdf, pages_ref->second.n, pages_ref->second.generation);
  if (page.type != pdf_object::DICT)
    return "invalid or unsupported page tree";

  auto& annots = page.dict["Annots"];
  if (annots.type != pdf_object::ARRAY) {
    // TODO indirectly referenced arrays might not be that hard to support
    if (annots.type != pdf_object::END)
      return "unexpected Annots";

    annots = {pdf_object::ARRAY};
  }
  annots.array.emplace_back(pdf_object::REFERENCE, sigfield_n, 0);
  pdf.update(page.n, [&] { pdf.document += pdf_serialize(page); });

  // 8.6.1 Interactive Form Dictionary
  if (root.dict.count("AcroForm"))
    return "the document already contains forms, they would be overwritten";

  root.dict["AcroForm"] = {std::map<std::string, pdf_object>{
    {"Fields", {std::vector<pdf_object>{
      {pdf_object::REFERENCE, sigfield_n, 0}
    }}},
    {"SigFlags", {pdf_object::NUMERIC, 3 /* SignaturesExist | AppendOnly */}}
  }};

  // Upgrade the document version for SHA-256 etc.
  if (pdf.version(root) < 16)
    root.dict["Version"] = {pdf_object::NAME, "1.6"};

  pdf.update(root_ref->second.n, [&] { pdf.document += pdf_serialize(root); });
  pdf.flush_updates();

  // Now that we know the length of everything, store byte ranges of what we're about to sign,
  // which must be everything but the resulting signature itself
  size_t tail_off = sign_off + sign_len, tail_len = pdf.document.size() - tail_off;
  auto ranges = ssprintf("[0 %zu %zu %zu]", sign_off, tail_off, tail_len);
  if (ranges.length() > byterange_len)
    return "not enough space reserved for /ByteRange";
  pdf.document.replace(byterange_off, std::min(ranges.length(), byterange_len), ranges);
  return pdf_fill_in_signature(pdf.document, sign_off, sign_len);
}

// -------------------------------------------------------------------------------------------------

__attribute__((format(printf, 2, 3)))
static void die(int status, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  if (isatty(fileno(stderr)))
    vfprintf(stderr, ssprintf("\x1b[31m%s\x1b[0m\n", format).c_str(), ap);
  else
    vfprintf(stderr, format, ap);
  va_end(ap);
  exit(status);
}

int main(int argc, char* argv[]) {
  auto invocation_name = argv[0];
  auto usage = [=] {
    die(1, "Usage: %s [-h] [-r RESERVATION] INPUT-FILENAME OUTPUT-FILENAME PKCS12-PATH PKCS12-PASS",
        invocation_name);
  };

  static struct option opts[] = {
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'V'},
    {"reservation", required_argument, 0, 'r'},
    {nullptr, 0, 0, 0},
  };

  // Reserved space in bytes for the certificate, digest, encrypted digest, ...
  long reservation = 4096;
  while (1) {
    int option_index = 0;
    auto c = getopt_long(argc, const_cast<char* const*>(argv), "hVr:", opts, &option_index);
    if (c == -1)
      break;

    char* end = nullptr;
    switch (c) {
    case 'r':
      errno = 0, reservation = strtol(optarg, &end, 10);
      if (errno || *end || reservation <= 0 || reservation > USHRT_MAX)
        die(1, "%s: must be a positive number", optarg);
      break;
    case 'V':
      die(0, "%s", PROJECT_NAME " " PROJECT_VERSION);
      break;
    case 'h':
    default:
      usage();
    }
  }

  argv += optind;
  argc -= optind;

  if (argc != 4)
    usage();

  const char* input_path  = argv[0];
  const char* output_path = argv[1];
  pkcs12_path = argv[2];
  pkcs12_pass = argv[3];

  std::string pdf_document;
  if (auto fp = fopen(input_path, "rb")) {
    int c;
    while ((c = fgetc(fp)) != EOF)
      pdf_document += c;
    if (ferror(fp))
      die(1, "%s: %s", input_path, strerror(errno));
    fclose(fp);
  } else {
    die(1, "%s: %s", input_path, strerror(errno));
  }

  auto err = pdf_sign(pdf_document, ushort(reservation));
  if (!err.empty()) {
    die(2, "Error: %s", err.c_str());
  }

  if (auto fp = fopen(output_path, "wb")) {
    auto written = fwrite(pdf_document.c_str(), pdf_document.size(), 1, fp);
    if (fclose(fp) || written != 1) {
      (void) unlink(output_path);
      die(3, "%s: %s", output_path, strerror(errno));
    }
  } else {
    die(3, "%s: %s", output_path, strerror(errno));
  }
  return 0;
}
