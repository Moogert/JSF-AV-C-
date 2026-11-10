#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/Index.h>
#include <nlohmann/json.hpp>
#include "server.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string cxstr(CXString s) {
  std::string out = clang_getCString(s) ? clang_getCString(s) : "";
  clang_disposeString(s);
  return out;
}

static std::string slash(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

static std::string norm(const fs::path &p) {
  return slash(fs::absolute(p).lexically_normal().string());
}

static bool under(const std::string &path, const std::string &root) {
#ifdef _WIN32
  std::string a = path, b = root;
  std::transform(a.begin(), a.end(), a.begin(), ::tolower);
  std::transform(b.begin(), b.end(), b.begin(), ::tolower);
  return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/');
#else
  return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
#endif
}

static std::string read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read " + p.string());
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static json read_json(const fs::path &p) {
  try { return json::parse(read_file(p)); }
  catch (const json::exception &e) { throw std::runtime_error("Invalid JSON in " + p.string() + ": " + e.what()); }
}

static void write_file(const fs::path &p, const std::string &s) {
  std::ofstream out(p, std::ios::binary);
  if (!out) throw std::runtime_error("Cannot write " + p.string());
  out << s;
  if (!out) throw std::runtime_error("Write failed: " + p.string());
}

struct Options {
  std::string command;
  std::vector<fs::path> paths;
  fs::path compdb, config, output, catalog;
  std::string profile = "literal", format = "text";
};

static void help() {
  std::cout << "jsfav " JSFAV_VERSION " - JSF Air Vehicle C++ rule checker\n"
            << "Usage:\n"
            << "  jsfav scan [paths...] [-p build-dir-or-compile_commands.json] [--profile literal|modern]\n"
            << "             [--format text|json|sarif] [--output file] [--config deviations.json]\n"
            << "  jsfav rules [--format text|json]\n"
            << "  jsfav explain RULE_ID [--format text|json]\n"
            << "  jsfav serve                 Start the local browser interface\n"
            << "  jsfav version\n"
            << "Options: --catalog rules.json, --help\n"
            << "Exit codes: 0 scan completed (even with findings); 2 input or analysis error.\n";
}

static Options parse_options(int argc, char **argv) {
  Options o;
  if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { help(); std::exit(0); }
  o.command = argv[1];
  if (o.command != "scan" && o.command != "rules" && o.command != "explain" && o.command != "version" && o.command != "serve")
    throw std::runtime_error("Unknown command: " + o.command);
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { help(); std::exit(0); }
    auto value = [&]() -> std::string {
      if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
      return argv[i];
    };
    if (arg == "-p" || arg == "--compdb") o.compdb = value();
    else if (arg == "--profile") o.profile = value();
    else if (arg == "--format") o.format = value();
    else if (arg == "--output") o.output = value();
    else if (arg == "--config") o.config = value();
    else if (arg == "--catalog") o.catalog = value();
    else if (!arg.empty() && arg[0] == '-') throw std::runtime_error("Unknown option: " + arg);
    else o.paths.emplace_back(arg);
  }
  if (o.profile != "literal" && o.profile != "modern") throw std::runtime_error("Profile must be literal or modern");
  if (o.format != "text" && o.format != "json" && o.format != "sarif") throw std::runtime_error("Format must be text, json, or sarif");
  if (o.command != "scan" && o.format == "sarif") throw std::runtime_error("SARIF is available only for scan");
  if (o.command == "explain" && o.paths.size() != 1) throw std::runtime_error("explain requires one rule ID");
  if (o.command == "rules" && !o.paths.empty()) throw std::runtime_error("rules takes no paths");
  if ((o.command == "serve" || o.command == "version") && !o.paths.empty()) throw std::runtime_error(o.command + " takes no paths");
  if (o.command == "scan" && o.paths.empty()) o.paths.emplace_back(fs::current_path());
  return o;
}

static fs::path find_catalog(const Options &o, const fs::path &exe) {
  if (!o.catalog.empty()) return o.catalog;
  fs::path dir = fs::absolute(exe).parent_path();
  for (int i = 0; i < 4; ++i, dir = dir.parent_path()) {
    fs::path candidate = dir / "data" / "rules.json";
    if (fs::exists(candidate)) return candidate;
  }
  throw std::runtime_error("rules.json not found; pass --catalog PATH");
}

static bool source_file(const fs::path &p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c++" || e == ".h" || e == ".hpp" || e == ".hh" || e == ".hxx";
}

static bool header_file(const fs::path &p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e == ".h" || e == ".hpp" || e == ".hh" || e == ".hxx";
}

struct Finding {
  std::string rule, classification, message, evidence, path;
  unsigned line = 0, column = 0;
  bool suppressed = false;
  json deviation;
};

static json finding_json(const Finding &f, const std::string &profile) {
  json j = {{"ruleId", f.rule}, {"profile", profile}, {"classification", f.classification},
            {"message", f.message}, {"evidence", f.evidence}, {"suppressed", f.suppressed}};
  if (!f.path.empty()) j["location"] = {{"path", f.path}, {"line", f.line}, {"column", f.column}};
  if (f.suppressed) j["deviation"] = f.deviation;
  return j;
}

static std::string finding_key(const Finding &f) {
  return f.rule + "|" + f.classification + "|" + f.path + "|" + std::to_string(f.line) + "|" + std::to_string(f.column);
}

struct Scan {
  Options options;
  json catalog, deviations;
  std::vector<Finding> findings;
  std::set<std::string> keys;
  std::set<std::string> selected;
  std::set<std::string> parsed_files;
  std::vector<std::string> roots;
  std::map<std::string, std::string> source_text;
  std::map<std::string, std::set<unsigned>> declaration_columns;
  std::map<std::string, unsigned> enum_members;
  std::set<std::string> language_standards;
  fs::path resource_dir;
  bool partial = false;

  void add(Finding f) {
    if (keys.insert(finding_key(f)).second) findings.push_back(std::move(f));
  }

  bool selected_path(const std::string &p) const {
    if (selected.count(p)) return true;
    for (const auto &root : roots) if (under(p, root)) return true;
    return false;
  }

  std::string display(const std::string &p) const {
    try { return slash(fs::relative(fs::path(p), fs::current_path()).string()); }
    catch (...) { return p; }
  }

  void violation(const std::string &rule, const std::string &path, unsigned line,
                 unsigned col, const std::string &message, const std::string &evidence) {
    Finding f{rule, "violation", message, evidence, display(path), line, col};
    for (const auto &d : deviations) {
      if (d.at("ruleId").get<std::string>() != rule) continue;
      const std::string scope = slash(d.at("path").get<std::string>());
      if (scope != "*" && scope != f.path && scope != path) continue;
      if (d.contains("line") && d.at("line").get<unsigned>() != line) continue;
      f.suppressed = true;
      f.deviation = d;
      break;
    }
    add(std::move(f));
  }
};

static std::vector<std::string> lines_of(const std::string &s) {
  std::vector<std::string> lines;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

static size_t utf8_columns(const std::string &s, size_t bytes) {
  size_t columns = 0;
  for (size_t i = 0; i < std::min(bytes, s.size()); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++columns;
  return columns;
}

static void scan_lines(Scan &scan, const std::string &path) {
  const std::string text = read_file(path);
  scan.source_text[path] = text;
  const auto lines = lines_of(text);
  const bool modern = scan.options.profile == "modern";
  const fs::path file(path);
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (header_file(file)) {
    if (ext != ".h" && !modern)
      scan.violation("53", path, 1, 1, "Header filename must end in .h", file.filename().string());
  } else if (ext != ".cpp" && !modern) {
    scan.violation("54", path, 1, 1, "Implementation filename must end in .cpp", file.filename().string());
  }
  const std::regex directive(R"(^\s*#\s*([A-Za-z_][A-Za-z_0-9]*))");
  const std::regex include(R"(^\s*#\s*include\s*([<\"])([^>\"]+)[>\"])");
  const std::regex function_macro(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z_0-9]*)\()");
  const std::regex guard_ifndef(R"(^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z_0-9]*))");
  const std::regex macro_define(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z_0-9]*)(.*)$)");
  const std::regex pragma_once_re(R"(^\s*#\s*pragma\s+once\b)");
  const std::regex endif_re(R"(^\s*#\s*endif\b)");
  const std::regex trigraph(R"(\?\?[=/'()!<>-])");
  std::set<std::string> guards;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::smatch m;
    if (!std::regex_search(lines[i], m, guard_ifndef)) continue;
    const std::string name = m[1];
    for (size_t j = i + 1; j < std::min(i + 6, lines.size()); ++j) {
      if (std::regex_search(lines[j], m, macro_define) && m[1] == name) {
        guards.insert(name);
        break;
      }
    }
  }
  if (header_file(file)) {
    const bool pragma_once = std::any_of(lines.begin(), lines.end(), [&](const std::string &s) { return std::regex_search(s, pragma_once_re); });
    const bool endif = std::any_of(lines.begin(), lines.end(), [&](const std::string &s) { return std::regex_search(s, endif_re); });
    if (!(modern && pragma_once) && (guards.empty() || !endif))
      scan.violation("35", path, 1, 1, "Header lacks a complete multiple-inclusion guard", file.filename().string());
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    const unsigned n = static_cast<unsigned>(i + 1);
    const size_t length = utf8_columns(line, line.size());
    if (length > 120)
      scan.violation("41", path, n, 121, "Source line exceeds 120 characters", std::to_string(length) + " characters");
    const auto tab = line.find('\t');
    if (tab != std::string::npos)
      scan.violation("43", path, n, static_cast<unsigned>(utf8_columns(line, tab) + 1), "Tab character in source", "tab");
    std::smatch tri;
    if (std::regex_search(line, tri, trigraph))
      scan.violation("11", path, n, static_cast<unsigned>(tri.position() + 1), "Trigraph sequence is prohibited", tri.str());
    std::smatch m;
    if (std::regex_search(line, m, directive)) {
      const std::string name = m[1];
      const bool allowed = name == "ifndef" || name == "define" || name == "endif" || name == "include" ||
                           (modern && name == "pragma" && line.find("once") != std::string::npos);
      if (!allowed)
        scan.violation("26", path, n, 1, "Preprocessor directive is outside the allowed set", "#" + name);
    }
    if (std::regex_search(line, m, function_macro))
      scan.violation("29", path, n, 1, "Function-like macro should be an inline function", m[1]);
    if (std::regex_search(line, m, macro_define) && !guards.count(m[1].str())) {
      scan.violation("31", path, n, 1, "#define is permitted only for inclusion guards", m[1]);
      if (!m[2].str().empty() && m[2].str()[0] != '(')
        scan.violation("30", path, n, 1, "Constant macro should be a const object", m[1]);
    }
    if (std::regex_search(line, m, directive) && m[1] == "include" &&
        !std::regex_search(line, include) && !modern)
      scan.violation("33", path, n, 1, "Include must use an angle-bracket header name", line);
    if (std::regex_search(line, m, include)) {
      const std::string target = m[2];
      if (!modern && fs::path(target).extension() != ".h")
        scan.violation("32", path, n, 1, "Include target must be a .h header", target);
      if (!modern && m[1] != "<")
        scan.violation("33", path, n, 1, "Include must use angle brackets", target);
    }
  }
}

static void source_location(CXSourceLocation loc, std::string &path, unsigned &line, unsigned &col) {
  CXFile file;
  unsigned offset;
  clang_getSpellingLocation(loc, &file, &line, &col, &offset);
  path = file ? norm(cxstr(clang_getFileName(file))) : "";
}

struct Token {
  std::string spelling, path;
  CXTokenKind kind;
  unsigned line = 0, col = 0;
};

static std::vector<Token> tokens_for(CXTranslationUnit tu, const std::string &path, size_t length) {
  std::vector<Token> result;
  CXFile file = clang_getFile(tu, path.c_str());
  if (!file) return result;
  CXSourceRange range = clang_getRange(clang_getLocationForOffset(tu, file, 0),
                                       clang_getLocationForOffset(tu, file, static_cast<unsigned>(length)));
  CXToken *tokens = nullptr;
  unsigned count = 0;
  clang_tokenize(tu, range, &tokens, &count);
  for (unsigned i = 0; i < count; ++i) {
    Token t;
    t.spelling = cxstr(clang_getTokenSpelling(tu, tokens[i]));
    t.kind = clang_getTokenKind(tokens[i]);
    source_location(clang_getTokenLocation(tu, tokens[i]), t.path, t.line, t.col);
    if (t.path == path) result.push_back(std::move(t));
  }
  clang_disposeTokens(tu, tokens, count);
  return result;
}

static size_t after_parens(const std::vector<Token> &t, size_t open) {
  if (open >= t.size() || t[open].spelling != "(") return t.size();
  int depth = 0;
  for (size_t i = open; i < t.size(); ++i) {
    if (t[i].spelling == "(") ++depth;
    else if (t[i].spelling == ")" && --depth == 0) return i + 1;
  }
  return t.size();
}

static void scan_tokens(Scan &scan, CXTranslationUnit tu, const std::string &path) {
  const auto it = scan.source_text.find(path);
  if (it == scan.source_text.end()) return;
  const auto raw = tokens_for(tu, path, it->second.size());
  std::vector<Token> t;
  for (const auto &token : raw) {
    if (token.kind == CXToken_Comment) {
      if (token.spelling.rfind("/*", 0) == 0)
        scan.violation("126", path, token.line, token.col, "C-style comment is prohibited", "/* ... */");
    } else t.push_back(token);
  }
  for (size_t i = 0; i < t.size(); ++i) {
    const Token &x = t[i];
    if (x.spelling == "<%" || x.spelling == "%>" || x.spelling == "<:" || x.spelling == ":>" ||
        x.spelling == "%:" || x.spelling == "%:%:")
      scan.violation("12", path, x.line, x.col, "Digraph token is prohibited", x.spelling);
    if (x.spelling == "offsetof")
      scan.violation("18", path, x.line, x.col, "offsetof macro is prohibited", x.spelling);
    if (x.kind == CXToken_Literal) {
      std::smatch suffix;
      const std::regex decimal(R"(^(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?([uUlLfF]+)$)");
      const std::regex hexadecimal(R"(^0[xX][0-9a-fA-F]+([uUlL]+)$)");
      if ((std::regex_match(x.spelling, suffix, decimal) || std::regex_match(x.spelling, suffix, hexadecimal)) &&
          std::any_of(suffix[1].first, suffix[1].second, [](char c) { return c >= 'a' && c <= 'z'; }))
        scan.violation("14", path, x.line, x.col, "Literal suffix must use uppercase letters", x.spelling);
    }
    if (x.spelling == "if" || x.spelling == "for" || x.spelling == "while") {
      const size_t open = x.spelling == "if" && i + 1 < t.size() && t[i + 1].spelling == "constexpr" ? i + 2 : i + 1;
      size_t body = after_parens(t, open);
      if (body < t.size() && t[body].spelling != "{" && t[body].spelling != ";") {
        // The while in a do-while statement does not introduce another body.
        bool do_while = x.spelling == "while" && i > 0 && t[i - 1].spelling == "}";
        if (!do_while)
          scan.violation("59", path, x.line, x.col, "Control statement body needs braces", x.spelling);
      }
      if (x.spelling == "for" && body < t.size()) {
        const size_t start = i + 2;
        int depth = 0;
        std::vector<size_t> semis;
        for (size_t j = start; j + 1 < body; ++j) {
          if (t[j].spelling == "(") ++depth;
          else if (t[j].spelling == ")") --depth;
          else if (t[j].spelling == ";" && depth == 0) semis.push_back(j);
        }
        if (semis.size() == 2 && (semis[0] == start || semis[1] + 1 == body - 1))
          scan.violation("200", path, x.line, x.col, "For loop has an empty initializer or increment", "for (...)");
      }
    } else if (x.spelling == "do" || x.spelling == "else") {
      if (i + 1 < t.size() && t[i + 1].spelling != "{" && !(x.spelling == "else" && t[i + 1].spelling == "if"))
        scan.violation("59", path, x.line, x.col, "Control statement body needs braces", x.spelling);
    }
  }
}

struct AstContext { Scan *scan; int breakable; int namespace_depth; };

static unsigned count_returns(CXCursor function) {
  unsigned count = 0;
  struct State { unsigned *count; } state{&count};
  clang_visitChildren(function, [](CXCursor child, CXCursor, CXClientData data) {
    auto *state = static_cast<State *>(data);
    if (clang_getCursorKind(child) == CXCursor_LambdaExpr) return CXChildVisit_Continue;
    if (clang_getCursorKind(child) == CXCursor_ReturnStmt) ++*state->count;
    return CXChildVisit_Recurse;
  }, &state);
  return count;
}

static CXChildVisitResult visit_ast(CXCursor cursor, CXCursor, CXClientData data) {
  auto ctx = *static_cast<AstContext *>(data);
  const CXCursorKind kind = clang_getCursorKind(cursor);
  std::string path;
  unsigned line = 0, col = 0;
  source_location(clang_getCursorLocation(cursor), path, line, col);
  if (!path.empty() && ctx.scan->selected_path(path)) {
    auto report = [&](const std::string &rule, const std::string &message, const std::string &evidence) {
      ctx.scan->violation(rule, path, line, col, message, evidence);
    };
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod || kind == CXCursor_Constructor ||
        kind == CXCursor_Destructor || kind == CXCursor_FunctionTemplate) {
      const int n = clang_Cursor_getNumArguments(cursor);
      if (n > 7) report("110", "Function has more than seven arguments", std::to_string(n) + " arguments");
      if (clang_Cursor_isVariadic(cursor)) report("108", "Variadic function is prohibited", cxstr(clang_getCursorSpelling(cursor)));
      if (clang_isCursorDefinition(cursor) && count_returns(cursor) > 1)
        report("113", "Function has more than one return statement", cxstr(clang_getCursorSpelling(cursor)));
    }
    if (kind == CXCursor_Namespace && ctx.namespace_depth >= 2)
      report("99", "Namespace nesting exceeds two levels", cxstr(clang_getCursorSpelling(cursor)));
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod || kind == CXCursor_VarDecl ||
        kind == CXCursor_ParmDecl || kind == CXCursor_FieldDecl || kind == CXCursor_StructDecl ||
        kind == CXCursor_ClassDecl || kind == CXCursor_EnumDecl || kind == CXCursor_TypedefDecl ||
        kind == CXCursor_Namespace) {
      const std::string name = cxstr(clang_getCursorSpelling(cursor));
      if (!name.empty() && name[0] == '_') report("47", "Identifier begins with an underscore", name);
      if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod || kind == CXCursor_VarDecl ||
          kind == CXCursor_ParmDecl || kind == CXCursor_FieldDecl) {
        if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; }))
          report("51", "Function or variable identifier contains uppercase letters", name);
      }
      if ((kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl || kind == CXCursor_EnumDecl ||
           kind == CXCursor_TypedefDecl || kind == CXCursor_Namespace) && !name.empty() &&
          ((name[0] >= 'a' && name[0] <= 'z') ||
           std::any_of(name.begin() + 1, name.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; })))
        report("50", "Type or namespace name needs one initial uppercase letter and lowercase thereafter", name);
    }
    if (kind == CXCursor_EnumConstantDecl ||
        (kind == CXCursor_VarDecl && clang_isConstQualifiedType(clang_getCursorType(cursor)))) {
      const std::string name = cxstr(clang_getCursorSpelling(cursor));
      if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; }))
        report("52", "Constant or enumerator name contains uppercase letters", name);
    }
    if (kind == CXCursor_VarDecl && clang_Cursor_getStorageClass(cursor) == CX_SC_Register)
      report("140", "register storage class is prohibited", "register");
    if (kind == CXCursor_VarDecl) {
      const std::string key = path + "|" + std::to_string(line);
      auto &cols = ctx.scan->declaration_columns[key];
      if (!cols.empty() && !cols.count(col)) report("152", "Multiple variable declarations on one line", "line " + std::to_string(line));
      cols.insert(col);
    }
    if (kind == CXCursor_UnionDecl) report("153", "Union declarations are prohibited", cxstr(clang_getCursorSpelling(cursor)));
    if (kind == CXCursor_FieldDecl && clang_Cursor_isBitField(cursor)) {
      const CXTypeKind type = clang_getCanonicalType(clang_getCursorType(cursor)).kind;
      if (type != CXType_Enum && type != CXType_UChar && type != CXType_UShort && type != CXType_UInt &&
          type != CXType_ULong && type != CXType_ULongLong && type != CXType_UInt128)
        report("154", "Bit-field needs an unsigned integral or enumeration type", cxstr(clang_getCursorSpelling(cursor)));
    }
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod) {
      const std::string name = cxstr(clang_getCursorSpelling(cursor));
      if (name == "operator&&" || name == "operator||" ||
          (name == "operator&" && clang_Cursor_getNumArguments(cursor) == (kind == CXCursor_CXXMethod ? 0 : 1)))
        report("159", "Overloading logical operators or unary address-of is prohibited", name);
    }
    if (kind == CXCursor_CStyleCastExpr) report("185", "C-style cast is prohibited", "C-style cast");
    if (kind == CXCursor_LabelStmt) report("188", "Label outside switch is prohibited", cxstr(clang_getCursorSpelling(cursor)));
    if (kind == CXCursor_IntegerLiteral) {
      CXToken *tokens = nullptr; unsigned count = 0;
      clang_tokenize(clang_Cursor_getTranslationUnit(cursor), clang_getCursorExtent(cursor), &tokens, &count);
      if (count) {
        std::string literal = cxstr(clang_getTokenSpelling(clang_Cursor_getTranslationUnit(cursor), tokens[0]));
        if (std::regex_match(literal, std::regex(R"(0[0-7]+[uUlL]*)")))
          report("149", "Nonzero octal integer literal is prohibited", literal);
        if (std::regex_match(literal, std::regex(R"(0[xX][0-9a-fA-F]+[uUlL]*)"))) {
          const auto suffix_start = literal.find_first_of("uUlL", 2);
          const std::string digits = literal.substr(2, suffix_start == std::string::npos ? std::string::npos : suffix_start - 2);
          if (literal[1] == 'x' ||
              std::any_of(digits.begin(), digits.end(), [](unsigned char c) { return c >= 'a' && c <= 'f'; }))
            report("150", "Hexadecimal digits must be uppercase", literal);
        }
      }
      clang_disposeTokens(clang_Cursor_getTranslationUnit(cursor), tokens, count);
    }
    if (kind == CXCursor_GotoStmt) report("189", "goto statement is prohibited", "goto");
    if (kind == CXCursor_ContinueStmt) report("190", "continue statement is prohibited", "continue");
    if (kind == CXCursor_BreakStmt && ctx.breakable != 2)
      report("191", "break is permitted only to terminate a switch case", "break");
    if (kind == CXCursor_CXXThrowExpr || kind == CXCursor_CXXTryStmt || kind == CXCursor_CXXCatchStmt)
      report("208", "C++ exceptions are prohibited", cxstr(clang_getCursorKindSpelling(kind)));
    if (kind == CXCursor_CompoundStmt) {
      auto check_brace = [&](CXSourceLocation location) {
        std::string brace_path; unsigned brace_line = 0, brace_col = 0;
        source_location(location, brace_path, brace_line, brace_col);
        auto it = ctx.scan->source_text.find(brace_path);
        if (it == ctx.scan->source_text.end() || brace_line == 0) return;
        const auto lines = lines_of(it->second);
        if (brace_line > lines.size()) return;
        const std::string &text = lines[brace_line - 1];
        const size_t pos = brace_col - 1;
        if (pos >= text.size()) return;
        const std::string before = text.substr(0, pos), after = text.substr(pos + 1);
        const bool prefix = std::all_of(before.begin(), before.end(), [](unsigned char c) { return std::isspace(c); });
        const size_t next = after.find_first_not_of(" \t");
        const bool suffix = next == std::string::npos || after.compare(next, 2, "//") == 0 || after.compare(next, 2, "/*") == 0;
        if (!prefix || !suffix) ctx.scan->violation("61", brace_path, brace_line, brace_col,
                                                     "Block brace must be alone on its line", text);
      };
      const CXSourceRange range = clang_getCursorExtent(cursor);
      check_brace(clang_getRangeStart(range));
      check_brace(clang_getRangeEnd(range));
    }
  }
  if (kind == CXCursor_SwitchStmt) ctx.breakable = 2;
  else if (kind == CXCursor_ForStmt || kind == CXCursor_WhileStmt || kind == CXCursor_DoStmt) ctx.breakable = 1;
  if (kind == CXCursor_Namespace) ++ctx.namespace_depth;
  clang_visitChildren(cursor, visit_ast, &ctx);
  return CXChildVisit_Continue;
}

static std::vector<std::string> collect_files(Scan &scan) {
  std::vector<std::string> files;
  for (const auto &path : scan.options.paths) {
    if (!fs::exists(path)) throw std::runtime_error("Input path does not exist: " + path.string());
    if (fs::is_regular_file(path)) {
      if (!source_file(path)) throw std::runtime_error("Not a C++ source or header file: " + path.string());
      files.push_back(norm(path));
    } else if (fs::is_directory(path)) {
      scan.roots.push_back(norm(path));
      for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied), end; it != end; ++it) {
        if (it->is_directory()) {
          const std::string name = it->path().filename().string();
          if (name == ".git" || name == "dist" || name == "node_modules" || name == "build" || name.rfind("build-", 0) == 0)
            it.disable_recursion_pending();
        } else if (it->is_regular_file() && source_file(it->path())) files.push_back(norm(it->path()));
      }
    } else throw std::runtime_error("Unsupported input path: " + path.string());
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  if (files.empty()) throw std::runtime_error("No C++ source or header files found");
  scan.selected.insert(files.begin(), files.end());
  return files;
}

static std::vector<std::string> compile_arguments(CXCompileCommand command, const std::string &file, const fs::path &dir) {
  std::vector<std::string> args;
  const unsigned n = clang_CompileCommand_getNumArgs(command);
  for (unsigned i = 1; i < n; ++i) {
    std::string a = cxstr(clang_CompileCommand_getArg(command, i));
    if (a == "-o" || a == "--output" || a == "-MF" || a == "-MT" || a == "-MQ" || a == "/Fo") { ++i; continue; }
    if (a == "-c" || a == "/c" || a == "-MMD" || a == "-MD" || a == "-MP" || a == "-fsyntax-only") continue;
    if (a.rfind("/Fo", 0) == 0 || a.rfind("-o", 0) == 0 && a.size() > 2) continue;
    if (a == file || (!a.empty() && a[0] != '-' && a[0] != '/' && norm(dir / a) == file)) continue;
    if (a == "/I") a = "-I";
    else if (a == "/D") a = "-D";
    if (a.rfind("/I", 0) == 0 && a.size() > 2) a = "-I" + a.substr(2);
    else if (a.rfind("/D", 0) == 0 && a.size() > 2) a = "-D" + a.substr(2);
    else if (a.rfind("/std:c++", 0) == 0) a = "-std=" + a.substr(5);
    else if (a == "/EHsc" || a == "/nologo" || a == "/W3" || a == "/W4") continue;
    args.push_back(a);
  }
  return args;
}

static void analyze_tu(Scan &scan, CXIndex index, const std::string &file,
                       const std::vector<std::string> &args, const fs::path &working_dir) {
  std::vector<std::string> actual_args = args;
  actual_args.push_back("-resource-dir=" + scan.resource_dir.string());
  std::vector<const char *> argv;
  for (const auto &arg : actual_args) argv.push_back(arg.c_str());
  const fs::path old_dir = fs::current_path();
  CXTranslationUnit tu = nullptr;
  CXErrorCode status;
  try {
    fs::current_path(working_dir);
    status = clang_parseTranslationUnit2(index, file.c_str(), argv.data(), static_cast<int>(argv.size()),
                                         nullptr, 0, CXTranslationUnit_DetailedPreprocessingRecord, &tu);
    fs::current_path(old_dir);
  } catch (...) {
    fs::current_path(old_dir);
    throw;
  }
  if (status != CXError_Success || !tu) throw std::runtime_error("libclang could not parse " + file);
  std::vector<std::string> errors;
  for (unsigned i = 0; i < clang_getNumDiagnostics(tu); ++i) {
    CXDiagnostic d = clang_getDiagnostic(tu, i);
    if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error)
      errors.push_back(cxstr(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions())));
    clang_disposeDiagnostic(d);
  }
  if (!errors.empty()) {
    clang_disposeTranslationUnit(tu);
    throw std::runtime_error("Incomplete analysis of " + file + ": " + errors.front());
  }
  AstContext ctx{&scan, 0, 0};
  clang_visitChildren(clang_getTranslationUnitCursor(tu), visit_ast, &ctx);
  for (const auto &item : scan.source_text) {
    if (clang_getFile(tu, item.first.c_str())) {
      scan.parsed_files.insert(item.first);
      scan_tokens(scan, tu, item.first);
    }
  }
  clang_disposeTranslationUnit(tu);
}

static void analyze(Scan &scan, const std::vector<std::string> &files) {
  for (const auto &file : files) scan_lines(scan, file);
  if (scan.options.compdb.empty()) {
    scan.partial = true;
    scan.language_standards.insert("unknown-no-compdb");
    scan.add({"", "coverage-warning", "No compile database: only file and line checks were run",
              "Pass -p build-directory for AST and token checks", "", 0, 0});
    return;
  }
  CXIndex index = clang_createIndex(0, 0);
  if (!index) throw std::runtime_error("libclang initialization failed");
  try {
    {
      fs::path db_dir = scan.options.compdb;
      if (fs::is_regular_file(db_dir)) db_dir = db_dir.parent_path();
      if (!fs::exists(db_dir / "compile_commands.json"))
        throw std::runtime_error("compile_commands.json not found at " + db_dir.string());
      CXCompilationDatabase_Error error;
      CXCompilationDatabase db = clang_CompilationDatabase_fromDirectory(db_dir.string().c_str(), &error);
      if (!db || error != CXCompilationDatabase_NoError) throw std::runtime_error("Could not load compilation database");
      CXCompileCommands commands = clang_CompilationDatabase_getAllCompileCommands(db);
      std::set<std::string> parsed;
      for (unsigned i = 0; i < clang_CompileCommands_getSize(commands); ++i) {
        CXCompileCommand command = clang_CompileCommands_getCommand(commands, i);
        fs::path dir = cxstr(clang_CompileCommand_getDirectory(command));
        fs::path source = cxstr(clang_CompileCommand_getFilename(command));
        if (source.is_relative()) source = dir / source;
        const std::string file = norm(source);
        if (!scan.selected.count(file) || header_file(file) || !parsed.insert(file).second) continue;
        auto args = compile_arguments(command, file, dir);
        bool has_standard = false;
        for (const auto &arg : args) if (arg.rfind("-std=", 0) == 0) {
          scan.language_standards.insert(arg.substr(5));
          has_standard = true;
        }
        if (!has_standard) scan.language_standards.insert("compiler-default");
        analyze_tu(scan, index, file, args, dir);
      }
      clang_CompileCommands_dispose(commands);
      clang_CompilationDatabase_dispose(db);
      for (const auto &file : files) {
        if (!header_file(file) && !parsed.count(file))
          throw std::runtime_error("No compile command for " + file);
      }
      for (const auto &file : files) if (!scan.parsed_files.count(file)) {
        scan.partial = true;
        scan.add({"", "coverage-warning", "Some selected headers were not part of a parsed translation unit",
                  "Unparsed header: " + scan.display(file), "", 0, 0});
        break;
      }
    }
  } catch (...) {
    clang_disposeIndex(index);
    throw;
  }
  clang_disposeIndex(index);
}

static void validate_deviations(Scan &scan) {
  scan.deviations = json::array();
  if (scan.options.config.empty()) return;
  json config = read_json(scan.options.config);
  if (!config.is_object() || !config.contains("deviations") || !config["deviations"].is_array())
    throw std::runtime_error("Config must contain a deviations array");
  std::set<std::string> ids;
  for (const auto &rule : scan.catalog.at("rules")) ids.insert(rule.at("id").get<std::string>());
  for (const auto &d : config["deviations"]) {
    if (!d.is_object() || !d.contains("ruleId") || !d.contains("path") || !d.contains("reason") || !d.contains("approval") ||
        !d["ruleId"].is_string() || !d["path"].is_string() || !d["reason"].is_string() || !d["approval"].is_string() ||
        d["path"].get<std::string>().empty() || d["reason"].get<std::string>().empty() || d["approval"].get<std::string>().empty())
      throw std::runtime_error("Each deviation requires ruleId, path, reason, and approval strings");
    if (!ids.count(d["ruleId"].get<std::string>())) throw std::runtime_error("Unknown deviation rule ID");
    if (d.contains("line") && (!d["line"].is_number_unsigned() || d["line"].get<unsigned>() == 0))
      throw std::runtime_error("Deviation line must be a positive integer");
    scan.deviations.push_back(d);
  }
}

static void add_reviews(Scan &scan) {
  const std::set<std::string> lexical = {"11", "26", "29", "30", "31", "32", "33", "35", "41", "43", "53", "54"};
  for (const auto &rule : scan.catalog.at("rules")) {
    const std::string id = rule.at("id").get<std::string>();
    const bool skipped = scan.partial && rule.at("detection") == "automated" && !lexical.count(id);
    if (rule.at("detection") == "review" || skipped) {
      std::string evidence = rule.at("description").get<std::string>() + " Review reason: " +
                             (skipped ? "Compile database absent; this check was not run." : rule.at("review_reason").get<std::string>());
      if (scan.options.profile == "modern" && rule.at("modern_note") != "Same as literal profile.")
        evidence += " Modern interpretation: " + rule.at("modern_note").get<std::string>();
      scan.add({id, "review", "Review AV Rule " + id, evidence, "", 0, 0});
    }
  }
}

static json scan_json(const Scan &scan) {
  json results = json::array();
  unsigned violations = 0, reviews = 0, warnings = 0, suppressed = 0;
  for (const auto &f : scan.findings) {
    results.push_back(finding_json(f, scan.options.profile));
    if (f.classification == "violation") ++violations;
    else if (f.classification == "review") ++reviews;
    else ++warnings;
    if (f.suppressed) ++suppressed;
  }
  unsigned automated = 0;
  for (const auto &rule : scan.catalog.at("rules")) if (rule.at("detection") == "automated") ++automated;
  return {{"schemaVersion", 1}, {"tool", {{"name", "jsfav"}, {"version", JSFAV_VERSION}}},
          {"profile", scan.options.profile}, {"partial", scan.partial},
          {"languageStandards", scan.language_standards},
          {"coverage", {{"automatedRules", automated}, {"reviewRules", 233 - automated},
                         {"executedAutomatedRules", scan.partial ? 12 : automated}}},
          {"results", results},
          {"summary", {{"violations", violations}, {"reviews", reviews},
                       {"coverageWarnings", warnings}, {"suppressed", suppressed}}}};
}

static json sarif_json(const Scan &scan) {
  json rules = json::array(), results = json::array();
  if (scan.partial) rules.push_back({{"id", "COVERAGE"}, {"shortDescription", {{"text", "Limited scan coverage"}}}});
  for (const auto &r : scan.catalog.at("rules")) {
    rules.push_back({{"id", "AV-" + r.at("id").get<std::string>()},
                     {"shortDescription", {{"text", r.at("description")}}},
                     {"properties", {{"pdfPage", r.at("pdf_page")}, {"detection", r.at("detection")}}}});
  }
  for (const auto &f : scan.findings) {
    json item = {{"ruleId", f.rule.empty() ? "COVERAGE" : "AV-" + f.rule},
                 {"level", f.classification == "violation" ? "warning" : "note"},
                 {"message", {{"text", f.message}}},
                 {"properties", {{"classification", f.classification}, {"profile", scan.options.profile},
                                 {"evidence", f.evidence}, {"suppressed", f.suppressed}}}};
    if (!f.path.empty()) item["locations"] = json::array({{{"physicalLocation", {
      {"artifactLocation", {{"uri", f.path}}}, {"region", {{"startLine", f.line}, {"startColumn", f.column}}}}}}});
    if (f.suppressed) {
      item["suppressions"] = json::array({{{"kind", "external"}, {"justification", f.deviation.at("reason")}}});
      item["properties"]["deviation"] = f.deviation;
    }
    results.push_back(item);
  }
  return {{"version", "2.1.0"}, {"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
          {"runs", json::array({{{"tool", {{"driver", {{"name", "jsfav"}, {"version", JSFAV_VERSION}, {"rules", rules}}}}},
                                 {"results", results}}})}};
}

static std::string text_scan(const Scan &scan) {
  std::ostringstream out;
  for (const auto &f : scan.findings) {
    out << (f.path.empty() ? "<project>" : f.path);
    if (f.line) out << ':' << f.line << ':' << f.column;
    out << " [" << f.classification << "] ";
    if (!f.rule.empty()) out << "AV-" << f.rule << ' ';
    out << f.message;
    if (f.suppressed) out << " (deviation: " << f.deviation.at("approval").get<std::string>() << ')';
    out << '\n';
  }
  out << "Results: " << scan.findings.size() << "; profile: " << scan.options.profile << '\n';
  return out.str();
}

int main(int argc, char **argv) {
  try {
    Options o = parse_options(argc, argv);
    if (o.command == "version") { std::cout << "jsfav " JSFAV_VERSION "\n"; return 0; }
    json catalog = read_json(find_catalog(o, argv[0]));
    if (!catalog.contains("rules") || !catalog["rules"].is_array() || catalog["rules"].size() != 233)
      throw std::runtime_error("Catalog must contain exactly 233 rules");
    if (o.command == "serve") return run_web_server(fs::absolute(argv[0]), find_catalog(o, argv[0]));
    std::string output;
    if (o.command == "rules") {
      if (o.format == "json") output = catalog.dump(2) + "\n";
      else for (const auto &rule : catalog["rules"])
        output += "AV-" + rule.at("id").get<std::string>() + " [" + rule.at("detection").get<std::string>() + "] " +
                  rule.at("description").get<std::string>() + "\n";
    } else if (o.command == "explain") {
      bool found = false;
      for (const auto &rule : catalog["rules"]) if (rule.at("id") == o.paths[0].string()) {
        found = true;
        if (o.format == "json") output = rule.dump(2) + "\n";
        else output = "AV-" + rule.at("id").get<std::string>() + " (PDF page " +
                      std::to_string(rule.at("pdf_page").get<int>()) + ")\n" +
                      rule.at("description").get<std::string>() + "\nDetection: " +
                      rule.at("detection").get<std::string>() + "\nModern profile: " +
                      rule.at("modern_note").get<std::string>() + "\n";
        break;
      }
      if (!found) throw std::runtime_error("Unknown rule ID: " + o.paths[0].string());
    } else {
      Scan scan; scan.options = o; scan.catalog = std::move(catalog);
      scan.resource_dir = fs::absolute(argv[0]).parent_path() / "clang-resource";
      if (!fs::exists(scan.resource_dir / "include"))
        throw std::runtime_error("Clang resource headers missing beside executable");
      validate_deviations(scan);
      auto files = collect_files(scan);
      analyze(scan, files);
      add_reviews(scan);
      std::sort(scan.findings.begin(), scan.findings.end(), [](const Finding &a, const Finding &b) {
        return finding_key(a) < finding_key(b);
      });
      output = o.format == "json" ? scan_json(scan).dump(2) + "\n" :
               o.format == "sarif" ? sarif_json(scan).dump(2) + "\n" : text_scan(scan);
    }
    if (o.output.empty()) std::cout << output;
    else write_file(o.output, output);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "jsfav: " << e.what() << '\n';
    return 2;
  }
}
