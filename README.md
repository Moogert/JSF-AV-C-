# ✈️ JSF AV C++ Checker

**A practical, cross-platform checker for the Joint Strike Fighter Air Vehicle C++ Coding Standards.** Scan from the command line or use the friendly local web interface to inspect findings without spelunking through a wall of text.

The checker keeps score honestly: every one of the **233 distinct rule IDs** is in the catalog. **40 rules have validated automatic checks**; the other **193 are surfaced for human review**. A review item means “please verify this,” not “everything is fine.”

> The goal is a smoother code review—not a certification decision. This tool helps you spot issues; it does not replace engineering judgment, safety review, or project approval.

## 🧭 Pick your cockpit

| Want to… | Start here |
|---|---|
| Click through a scan and inspect findings | [Local web interface](#-the-local-web-interface) |
| Run a repeatable command in CI or an agent workflow | [Command-line quick start](#-command-line-quick-start) |
| Build the checker yourself | [Build from source](#-build-from-source) |
| Understand what results mean | [Coverage and limitations](#-coverage-and-limitations) |

## 🖥️ The local web interface

The UI runs on your computer, listens only on `127.0.0.1`, and makes no external requests. Your source files stay where they are.

1. Download and extract the release archive for Windows or Linux.
2. Double-click `start-jsfav-ui.bat` on Windows or `start-jsfav-ui.sh` on Linux. You can also run `jsfav serve` from a terminal.
3. Pick your project folder. If you have a compilation database, choose its build folder too. Press **Start scan**.
4. Filter results, select a finding for its explanation and evidence, then download JSON or SARIF if you need a report.

Keep the terminal window open while you use the UI. It prints the local browser address and stops the server when you press `Ctrl+C`. If no browser opens automatically, copy that address into your browser.

**Picking folders:** Windows uses its native folder picker. Linux uses Zenity when installed; if it is missing, type or paste a path into the field. The path fields remain editable on both platforms. The UI can show nearby source lines only for files inside the selected project.

### Local API for automation

The browser UI uses a small versioned API under `/api/v1/` for asynchronous scans, scan status, catalog access, source context, and JSON/SARIF export. API requests require the one-session token from the printed browser URL in the `X-JSFAV-Token` header. Results are held in memory until the server exits; downloading a report is the only save action in the UI.

For unattended workflows, prefer the CLI below. It is noninteractive and has stable JSON output.

## ⌨️ Command-line quick start

Create a Clang `compile_commands.json` for the project if you want semantic checks. With CMake, add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` when configuring your project.

```sh
# Full scan with a compilation database; write machine-readable results
jsfav scan /path/to/project -p /path/to/build --format json --output findings.json

# Use the documented modern interpretation and export SARIF
jsfav scan /path/to/project -p /path/to/build --profile modern --format sarif --output findings.sarif

# Inspect a rule or browse the catalog
jsfav explain 59
jsfav rules --format json
```

`-p` accepts either the build directory or the `compile_commands.json` file itself. If your build folder is the project's `build/` directory, the web UI can find the database automatically. The CLI defaults to a path-only scan when `-p` is omitted; the output will include a coverage warning.

Paths with spaces are supported; quote them for your shell:

```sh
jsfav scan "/work/My C++ Project" -p "/work/My C++ Project/build" --format json
```

### Commands at a glance

| Command | What it does |
|---|---|
| `jsfav scan [paths...]` | Scan source paths or a project; accepts `-p`, `--profile`, `--format`, `--output`, and `--config`. |
| `jsfav rules` | List all catalog entries as text or JSON. |
| `jsfav explain RULE_ID` | Show a rule's description, PDF page, detection status, and profile note. |
| `jsfav serve` | Start the local browser interface. |
| `jsfav version` | Print the tool version. |

**Exit codes:** `0` means the requested scan completed, even if it found violations or review items. `2` means an input or analysis error occurred. A green exit code is not a clean bill of compliance—read the findings and coverage summary.

## 📊 Coverage and limitations

| Catalog status | Count | Meaning |
|---|---:|---|
| Validated automatic checks | 40 | Source patterns the checker can identify, with compliant and violating fixtures. |
| Human review | 193 | Design intent, project evidence, or organizational judgment is needed. |
| **Total rule IDs** | **233** | Every distinct rule ID from the reference standard is accounted for. |

Automatic checks include preprocessor and source-layout restrictions, names and declarations, namespaces and functions, comments, casts, and control flow. The catalog is the source of truth for a rule's status and PDF page; run `jsfav rules --format json` to inspect it.

With a compilation database, the checker uses libclang to parse translation units and perform semantic checks. Without one, only the supported file and line checks run; skipped checks move into the review queue and a coverage warning is added. A successful scan never means all 233 rules passed.

Some project standards require evidence the source code cannot provide, such as safety analysis, testing records, or an approved design decision. Those stay review items—no pretending a source scanner can read minds.

### Rule interpretation profiles

- **`literal`** (default) follows the supplied PDF's wording.
- **`modern`** follows each file's configured C++ language level and applies only the explicit modern interpretations recorded in `data/rules.json` (`modern_note`). Rules without an override retain their PDF meaning.

Modern mode permits common C++ header and implementation suffixes, quoted includes, and `#pragma once` where documented. Rule 8 and newer fixed-width type choices remain review items.

## 🧾 Results and approved deviations

JSON uses `schemaVersion: 1` and includes the profile, language standards, partial-coverage flag, coverage counts, summary, and individual results. Each result carries its rule ID, classification (`violation`, `review`, or `coverage-warning`), evidence, suppression state, and a location when one is available. SARIF export uses version 2.1.0.

Approved deviations are supplied as a JSON file; the checker preserves suppressed violations and their approval metadata in the output. Each entry needs a rule ID, path scope, reason, and approval reference. Use `"*"` for project-wide scope; `line` is optional.

```json
{
  "deviations": [
    {
      "ruleId": "59",
      "path": "src/legacy.cpp",
      "line": 42,
      "reason": "Reviewed exception in generated adapter",
      "approval": "ARCH-123"
    }
  ]
}
```

Run the CLI with `--config deviations.json`. The checker records the supplied approval reference; it cannot independently verify an external approval.

## 🛠️ Build from source

### Requirements

- CMake 3.24 or newer
- A C++17 compiler
- Clang and libclang development files (headers and library)
- Python 3 for the test suite

On Ubuntu 24.04, install the dependencies with:

```sh
sudo apt update
sudo apt install cmake g++ clang libclang-dev python3
```

On Windows, install Visual Studio 2022 with the C++ workload and the LLVM Windows installer. CMake must be able to find LLVM's headers, libclang library, Clang executable, and builtin resource headers. If automatic discovery fails, set `LLVM_HOME` or pass `LIBCLANG_INCLUDE_DIR` and `LIBCLANG_LIBRARY` to CMake.

### Configure, build, test

Linux:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows PowerShell with Visual Studio 2022:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The CMake build copies the rule catalog, web assets, and Clang resource headers beside the executable. `cpp-httplib` and `nlohmann/json` are vendored, so building the web server does not need a network connection. Source builds still require the LLVM/libclang installation described above.

### Package a release archive

```sh
# Linux
cpack --config build/CPackConfig.cmake -G TGZ -B dist

# Windows PowerShell
cpack --config build/CPackConfig.cmake -C Release -G ZIP -B dist
```

GitHub Actions builds and tests Windows and Linux packages. Tag-based GitHub Release publishing is configured in `.github/workflows/`; it becomes active when a version tag is pushed to this repository.

## 🤖 Agent-friendly use

The CLI is designed for scripts and coding agents: commands never prompt, source files are never modified, JSON has a stable schema, and findings do not change the scan's exit code. See the short [agent usage guide](docs/agent-usage.md) for a reliable workflow and interpretation reminders.

Future coding agents should start with [AGENTS.md](AGENTS.md), then use the [compliance writing guide](docs/writing-compliant-cpp.md) to plan, document, and review changes against the selected profile. Maintainers can follow the [architecture notes](docs/architecture.md) for scanner flow, concurrency, path boundaries, and export behavior.

Quick machine-readable scan:

```sh
jsfav scan /path/to/project -p /path/to/build --format json
```

Always check `partial`, `coverage`, `summary`, and each result's `classification`. Treat `review` as work still to do. A suppressed violation is still visible and should be considered with its deviation metadata.

## 🧪 Contributing

Contributions are welcome. When proposing a new automatic rule check, include a targeted violating fixture and a compliant fixture in `tests/test_cli.py`; keep rules that need human or project-specific evidence labeled for review. Preserve all 233 rule IDs, and do not treat a zero exit code as proof of compliance.

Before submitting a change, build and run CTest on Windows and Linux when available. The test suite covers the CLI, profile behavior, catalog coverage, deviations, JSON/SARIF, path handling, and the local web server.

## 📚 Reference and notices

The catalog was generated from the supplied public-release JSF Air Vehicle C++ Coding Standards PDF (document 2RDU00001, revision C, December 2005). The checked-in catalog is sufficient at runtime; the original PDF is not required to run the checker. If an extracted description is ambiguous, consult the original standard.

The project includes license notices for LLVM/libclang, `nlohmann/json`, and `cpp-httplib` under `licenses/`. The checker is a review aid and does not certify a project against the standard.

---

Happy scanning! May your findings be few and your build take off on the first try. 🚀
