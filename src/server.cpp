#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
struct Job {
  std::mutex mutex;
  std::thread worker;
  std::string id;
  std::string state = "idle";
  std::string error;
  json result;
  fs::path project;
} job;

std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::string make_id() {
  static std::atomic<unsigned long long> seq{0};
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++seq);
}
std::string token_value() {
  std::random_device rd;
  std::ostringstream s;
  for (int i = 0; i < 4; ++i) s << std::hex << rd();
  return s.str();
}
bool local_host(const httplib::Request &req) {
  if (!req.has_header("Host")) return false;
  const auto host = req.get_header_value("Host");
  return host == "127.0.0.1" || host.rfind("127.0.0.1:", 0) == 0 || host == "localhost" || host.rfind("localhost:", 0) == 0;
}
bool authorized(const httplib::Request &req, const std::string &token) {
  return local_host(req) && req.has_header("X-JSFAV-Token") && req.get_header_value("X-JSFAV-Token") == token;
}
void json_response(httplib::Response &res, const json &body, int status = 200) {
  res.status = status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
  res.set_header("Cache-Control", "no-store");
}
bool valid_profile(const std::string &s) { return s == "literal" || s == "modern"; }

#ifdef _WIN32
std::wstring wide(const std::string &s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  if (!out.empty()) out.pop_back();
  return out;
}
std::string pick_path(bool folder) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IFileOpenDialog *dialog = nullptr;
  std::string picked;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem *item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
          std::string bytes(static_cast<size_t>(n), '\0');
          WideCharToMultiByte(CP_UTF8, 0, path, -1, bytes.data(), n, nullptr, nullptr);
          if (!bytes.empty()) bytes.pop_back();
          picked = bytes;
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return picked;
}
void open_browser(const std::string &url) { ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
std::wstring quote_windows(const std::wstring &s) {
  std::wstring out = L"\"";
  unsigned slashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') out.append(slashes * 2 + 1, L'\\');
    else out.append(slashes, L'\\');
    out.push_back(c); slashes = 0;
  }
  out.append(slashes * 2, L'\\'); out.push_back(L'"');
  return out;
}
int run_scan_process(const std::vector<std::string> &args, const fs::path &cwd, std::string &output) {
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_pipe = nullptr, write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return -1;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = write_pipe; si.hStdError = write_pipe; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring command;
  for (const auto &arg : args) { if (!command.empty()) command.push_back(L' '); command += quote_windows(wide(arg)); }
  std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
  BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.wstring().c_str(), &si, &pi);
  CloseHandle(write_pipe);
  if (!ok) { CloseHandle(read_pipe); return -1; }
  char buffer[8192]; DWORD received = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &received, nullptr) && received) output.append(buffer, received);
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 2; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return static_cast<int>(code);
}
#else
std::string pick_path(bool folder) {
  int pipefd[2]; if (pipe(pipefd) != 0) return {};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO); close(pipefd[0]); close(pipefd[1]);
    if (folder) execlp("zenity", "zenity", "--file-selection", "--directory", "--title=Choose project folder", static_cast<char *>(nullptr));
    else execlp("zenity", "zenity", "--file-selection", "--title=Choose file", static_cast<char *>(nullptr));
    _exit(127);
  }
  close(pipefd[1]); std::string out; char buf[1024]; ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  close(pipefd[0]); int status = 0; waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back(); return out;
}
void open_browser(const std::string &url) {
  pid_t pid = fork(); if (pid == 0) { execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr)); _exit(127); }
}
int run_scan_process(const std::vector<std::string> &args, const fs::path &cwd, std::string &output) {
  int pipefd[2]; if (pipe(pipefd) != 0) return -1;
  std::vector<char *> argv; for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str())); argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    if (chdir(cwd.c_str()) != 0) _exit(126);
    dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]);
    execv(argv[0], argv.data()); _exit(127);
  }
  close(pipefd[1]); if (pid < 0) { close(pipefd[0]); return -1; }
  char buffer[8192]; ssize_t received = 0;
  while ((received = read(pipefd[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<size_t>(received));
  close(pipefd[0]); int status = 0; if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

json make_sarif(const json &scan, const json &catalog) {
  json rules = json::array(), results = json::array();
  if (scan.value("partial", false)) rules.push_back({{"id", "COVERAGE"}, {"shortDescription", {{"text", "Limited scan coverage"}}}});
  for (const auto &r : catalog.at("rules")) rules.push_back({{"id", "AV-" + r.at("id").get<std::string>()},
    {"shortDescription", {{"text", r.at("description")}}}, {"properties", {{"pdfPage", r.at("pdf_page")}, {"detection", r.at("detection")}}}});
  for (const auto &f : scan.at("results")) {
    const std::string id = f.value("ruleId", "");
    json item = {{"ruleId", id.empty() ? "COVERAGE" : "AV-" + id},
      {"level", f.value("classification", "") == "violation" ? "warning" : "note"},
      {"message", {{"text", f.value("message", "")}}},
      {"properties", {{"classification", f.value("classification", "")}, {"profile", f.value("profile", "")},
                       {"evidence", f.value("evidence", "")}, {"suppressed", f.value("suppressed", false)}}}};
    if (f.contains("location")) { const auto &loc = f.at("location"); item["locations"] = json::array({{{"physicalLocation", {
      {"artifactLocation", {{"uri", loc.value("path", "")}}}, {"region", {{"startLine", loc.value("line", 1)}, {"startColumn", loc.value("column", 1)}}}}}}}); }
    if (f.value("suppressed", false) && f.contains("deviation")) {
      item["suppressions"] = json::array({{{"kind", "external"}, {"justification", f.at("deviation").value("reason", "")}}});
      item["properties"]["deviation"] = f.at("deviation");
    }
    results.push_back(std::move(item));
  }
  return {{"version", "2.1.0"}, {"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
          {"runs", json::array({{{"tool", {{"driver", {{"name", "jsfav"}, {"version", JSFAV_VERSION}, {"rules", rules}}}}}, {"results", results}}})}};
}
} // namespace

int run_web_server(const fs::path &executable, const fs::path &catalog_path) {
  const fs::path web = fs::absolute(executable).parent_path() / "web";
  if (!fs::exists(web / "index.html")) throw std::runtime_error("Web UI files missing beside executable at " + web.string());
  const json catalog = json::parse(read_text(catalog_path));
  const std::string token = token_value();
  httplib::Server server;
  server.set_payload_max_length(1024 * 1024);
  server.set_default_headers({{"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "no-referrer"},
                              {"Content-Security-Policy", "default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; base-uri 'none'; frame-ancestors 'none'"}});
  auto protected_route = [&token](const httplib::Request &req, httplib::Response &res) {
    if (!authorized(req, token)) { json_response(res, {{"error", "This local page is not authorized. Reopen the browser using the address printed by jsfav."}}, 403); return false; }
    return true;
  };
  server.Get("/", [web](const httplib::Request &, httplib::Response &res) { res.set_file_content((web / "index.html").string(), "text/html; charset=utf-8"); });
  server.Get("/style.css", [web](const httplib::Request &, httplib::Response &res) { res.set_file_content((web / "style.css").string(), "text/css; charset=utf-8"); });
  server.Get("/app.js", [web](const httplib::Request &, httplib::Response &res) { res.set_file_content((web / "app.js").string(), "application/javascript; charset=utf-8"); });
  server.Get("/api/v1/status", [&, protected_route](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return;
    std::lock_guard<std::mutex> lock(job.mutex);
    json body = {{"apiVersion", 1}, {"state", job.state}}; if (!job.project.empty()) body["projectPath"] = job.project.string(); json_response(res, body);
  });
  server.Get("/api/v1/rules", [&, protected_route](const httplib::Request &req, httplib::Response &res) { if (protected_route(req, res)) json_response(res, catalog); });
  server.Get("/api/v1/pick", [protected_route](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return;
    const bool folder = req.get_param_value("kind") != "file";
    const auto path = pick_path(folder);
    json_response(res, {{"path", path}, {"available", !path.empty()}, {"hint", path.empty() ? "Picker canceled or unavailable; enter the path in the field." : ""}});
  });
  server.Post("/api/v1/scans", [&, protected_route, executable](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return;
    json input;
    try { input = json::parse(req.body); } catch (...) { json_response(res, {{"error", "Request must be valid JSON."}}, 400); return; }
    if (!input.is_object() || !input.contains("projectPath") || !input["projectPath"].is_string()) { json_response(res, {{"error", "Choose a project folder."}}, 400); return; }
    const std::string profile = input.value("profile", "literal"); if (!valid_profile(profile)) { json_response(res, {{"error", "Profile must be literal or modern."}}, 400); return; }
    std::error_code ec; fs::path project = fs::canonical(fs::path(input["projectPath"].get<std::string>()), ec);
    if (ec || !fs::is_directory(project)) { json_response(res, {{"error", "Project path is not an accessible folder."}}, 400); return; }
    fs::path compdb;
    const std::string requested_compdb = input.value("compdbPath", "");
    if (!requested_compdb.empty()) {
      compdb = fs::absolute(requested_compdb, ec); if (ec) { json_response(res, {{"error", "Compilation database path is invalid."}}, 400); return; }
      if (fs::is_regular_file(compdb)) { if (compdb.filename() != "compile_commands.json") { json_response(res, {{"error", "Select compile_commands.json or its containing build folder."}}, 400); return; } }
      else if (!fs::exists(compdb / "compile_commands.json")) { json_response(res, {{"error", "compile_commands.json was not found in that folder."}}, 400); return; }
    } else {
      for (const auto &candidate : {project / "compile_commands.json", project / "build" / "compile_commands.json"}) if (fs::exists(candidate)) { compdb = candidate; break; }
    }
    fs::path config; const std::string config_text = input.value("configPath", "");
    if (!config_text.empty()) { config = fs::absolute(config_text, ec); if (ec || !fs::is_regular_file(config)) { json_response(res, {{"error", "Deviation file is not an accessible file."}}, 400); return; } }
    std::lock_guard<std::mutex> lock(job.mutex);
    if (job.state == "running") { json_response(res, {{"error", "A scan is already running. Wait for it to finish before starting another."}}, 409); return; }
    if (job.worker.joinable()) job.worker.join();
    job.id = make_id(); job.project = project; job.state = "running"; job.error.clear(); job.result = nullptr;
    const std::string id = job.id;
    std::vector<std::string> args{fs::absolute(executable).string(), "scan", project.string(), "--format", "json", "--profile", profile};
    if (!compdb.empty()) { args.push_back("-p"); args.push_back(compdb.string()); }
    if (!config.empty()) { args.push_back("--config"); args.push_back(config.string()); }
    job.worker = std::thread([args = std::move(args), project, id]() {
      std::string output;
      const int code = run_scan_process(args, project, output);
      std::lock_guard<std::mutex> done(job.mutex);
      if (job.id != id) return;
      if (code != 0) { job.state = "failed"; job.error = output.empty() ? "Scanner exited with code " + std::to_string(code) + "." : output; return; }
      try { job.result = json::parse(output); job.result["scanId"] = id; job.state = "completed"; }
      catch (const std::exception &e) { job.state = "failed"; job.error = std::string("Could not read scan results: ") + e.what(); }
    });
    json_response(res, {{"id", job.id}, {"state", job.state}} , 202);
  });
  server.Get(R"(/api/v1/scans/([A-Za-z0-9-]+))", [protected_route](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return; const auto id = req.matches[1].str(); std::lock_guard<std::mutex> lock(job.mutex);
    if (id != job.id) { json_response(res, {{"error", "Scan not found."}}, 404); return; }
    json body = {{"id", id}, {"state", job.state}}; if (job.state == "failed") body["error"] = job.error; if (job.state == "completed") body["result"] = job.result; json_response(res, body);
  });
  server.Get(R"(/api/v1/scans/([A-Za-z0-9-]+)/export)", [protected_route, catalog](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return; const auto id = req.matches[1].str(); std::lock_guard<std::mutex> lock(job.mutex);
    if (id != job.id || job.state != "completed") { json_response(res, {{"error", "Completed scan not found."}}, 404); return; }
    const auto format = req.get_param_value("format");
    if (format == "json") { res.set_content(job.result.dump(2) + "\n", "application/json; charset=utf-8"); res.set_header("Content-Disposition", "attachment; filename=jsfav-results.json"); }
    else if (format == "sarif") { res.set_content(make_sarif(job.result, catalog).dump(2) + "\n", "application/sarif+json; charset=utf-8"); res.set_header("Content-Disposition", "attachment; filename=jsfav-results.sarif"); }
    else json_response(res, {{"error", "Format must be json or sarif."}}, 400);
  });
  server.Get("/api/v1/source", [protected_route](const httplib::Request &req, httplib::Response &res) {
    if (!protected_route(req, res)) return;
    if (!req.has_param("path") || !req.has_param("line")) { json_response(res, {{"error", "Source path and line are required."}}, 400); return; }
    std::lock_guard<std::mutex> lock(job.mutex); if (job.state != "completed") { json_response(res, {{"error", "No completed scan is available."}}, 404); return; }
    std::error_code ec; fs::path input(req.get_param_value("path")); fs::path target = fs::canonical(input.is_absolute() ? input : job.project / input, ec);
    if (ec || !fs::is_regular_file(target)) { json_response(res, {{"error", "Source file is unavailable."}}, 404); return; }
    auto rel = fs::relative(target, job.project, ec); if (ec || rel.empty() || *rel.begin() == "..") { json_response(res, {{"error", "Source is outside the selected project."}}, 403); return; }
    unsigned line = 0; try { line = static_cast<unsigned>(std::stoul(req.get_param_value("line"))); } catch (...) { json_response(res, {{"error", "Invalid line number."}}, 400); return; }
    std::ifstream in(target); std::vector<std::string> lines; std::string text; while (std::getline(in, text)) lines.push_back(text);
    if (!line || line > lines.size()) { json_response(res, {{"error", "Source line is outside the file."}}, 404); return; }
    json out = json::array(); const unsigned first = line > 3 ? line - 3 : 1, last = std::min<unsigned>(static_cast<unsigned>(lines.size()), line + 3);
    for (unsigned n = first; n <= last; ++n) out.push_back({{"line", n}, {"text", lines[n - 1]}}); json_response(res, {{"lines", out}});
  });

  if (!server.bind_to_port("127.0.0.1", 8765)) {
    if (job.worker.joinable()) job.worker.join();
    throw std::runtime_error("Could not start local web server on 127.0.0.1:8765 (the port may already be in use).");
  }
  const std::string url = "http://127.0.0.1:8765/?token=" + token;
  std::cout << "JSF AV Checker is ready at " << url << "\nPress Ctrl+C in this window to stop.\n" << std::flush;
  if (std::getenv("JSFAV_NO_BROWSER") == nullptr) open_browser(url);
  server.listen_after_bind();
  if (job.worker.joinable()) job.worker.join();
  return 0;
}
