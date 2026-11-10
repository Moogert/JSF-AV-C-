"""Black-box CLI checks; invoked by CTest with binary and project paths."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


BINARY = Path(sys.argv.pop(1)).resolve()
PROJECT = Path(sys.argv.pop(1)).resolve()


def run(*args, cwd=None):
    return subprocess.run([str(BINARY), *map(str, args)], cwd=cwd, text=True, encoding="utf-8",
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="jsfav test ")
        self.root = Path(self.temp.name)
        self.source = self.root / "sample.cpp"
        self.source.write_text(
            "#define BAD(x) (x)\n"
            "int f(int a,int b,int c,int d,int e,int g,int h,int j) {\n"
            "  if (a) return 01;\n"
            "  for (; a < 4; ) { continue; }\n"
            "  try { throw 1; } catch (...) { return 0; }\n"
            "}\n", encoding="utf-8")
        (self.root / "compile_commands.json").write_text(json.dumps([{
            "directory": str(self.root), "file": str(self.source),
            "arguments": ["clang++", "-std=c++17", "-I", str(self.root), "-c", str(self.source)]
        }]), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def scan(self, *extra):
        result = run("scan", self.source, "-p", self.root, "--format", "json", *extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_catalog(self):
        result = run("rules", "--format", "json")
        self.assertEqual(result.returncode, 0, result.stderr)
        rules = json.loads(result.stdout)["rules"]
        self.assertEqual(len(rules), 233)
        self.assertEqual(len({r["id"] for r in rules}), 233)
        self.assertEqual(len([r for r in rules if r["detection"] == "automated"]), 40)

    def test_scan_and_dedup(self):
        data = self.scan()
        ids = {r["ruleId"] for r in data["results"] if r["classification"] == "violation"}
        self.assertTrue({"29", "59", "110", "149", "190", "200", "208"} <= ids, ids)
        self.assertEqual(data["summary"]["reviews"], 193)
        keys = [(r["ruleId"], r.get("location", {}).get("path"),
                 r.get("location", {}).get("line"), r.get("location", {}).get("column"))
                for r in data["results"]]
        self.assertEqual(len(keys), len(set(keys)))

    def test_deviation_and_sarif(self):
        config = self.root / "deviations.json"
        config.write_text(json.dumps({"deviations": [{"ruleId": "29", "path": "*",
            "reason": "Existing vendor API", "approval": "ARCH-17"}]}), encoding="utf-8")
        data = self.scan("--config", config)
        finding = next(r for r in data["results"] if r["ruleId"] == "29")
        self.assertTrue(finding["suppressed"])
        sarif = run("scan", self.source, "-p", self.root, "--config", config,
                    "--format", "sarif")
        self.assertEqual(sarif.returncode, 0, sarif.stderr)
        doc = json.loads(sarif.stdout)
        self.assertEqual(doc["version"], "2.1.0")
        self.assertEqual(len(doc["runs"][0]["tool"]["driver"]["rules"]), 233)

    def test_profiles_and_errors(self):
        header = self.root / "sample.hpp"
        header.write_text("#pragma once\n", encoding="utf-8")
        literal = json.loads(run("scan", header, "--format", "json").stdout)
        modern = json.loads(run("scan", header, "--profile", "modern", "--format", "json").stdout)
        self.assertIn("53", {r["ruleId"] for r in literal["results"] if r["classification"] == "violation"})
        self.assertNotIn("53", {r["ruleId"] for r in modern["results"] if r["classification"] == "violation"})
        self.assertTrue(modern["partial"])
        self.assertEqual(modern["coverage"]["executedAutomatedRules"], 12)
        self.assertEqual(modern["summary"]["reviews"], 221)
        self.assertEqual(run("scan", self.source, "-p", self.root / "missing").returncode, 2)
        (self.root / "compile_commands.json").write_text("[]", encoding="utf-8")
        self.assertEqual(run("scan", self.source, "-p", self.root).returncode, 2)

    def test_limited_scan_without_build_flags(self):
        self.source.write_text("#include <missing_dependency.h>\nint f(){\treturn 0;}\n", encoding="utf-8")
        result = run("scan", self.source, "--format", "json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertTrue(data["partial"])
        self.assertIn("43", {r["ruleId"] for r in data["results"] if r["classification"] == "violation"})
        self.assertEqual(data["summary"]["coverageWarnings"], 1)

    def test_output_file_and_parse_failure(self):
        output = self.root / "findings.json"
        result = run("scan", self.source, "-p", self.root, "--format", "json", "--output", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["schemaVersion"], 1)
        self.source.write_text("int f(\n", encoding="utf-8")
        self.assertEqual(run("scan", self.source, "-p", self.root).returncode, 2)

    def test_header_dedup_and_relative_compdb_paths(self):
        self.source.unlink()
        (self.root / "common.h").write_text("/* shared */\nint CamelCase;\n", encoding="utf-8")
        commands = []
        for name in ("one.cpp", "two.cpp"):
            (self.root / name).write_text('#include "common.h"\nint f_' + name[0] + '(){return 0;}\n', encoding="utf-8")
            commands.append({"directory": str(self.root), "file": name,
                             "arguments": ["clang++", "-std=c++17", "-c", name]})
        (self.root / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
        result = run("scan", self.root, "-p", self.root, "--format", "json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        shared = [r for r in data["results"] if r["ruleId"] in ("51", "126")
                  and r.get("location", {}).get("path", "").endswith("common.h")]
        self.assertEqual(len(shared), 2, shared)
        self.assertFalse(data["partial"])

    def test_automated_rule_fixtures(self):
        # Each fixture has one targeted violation and a nearby compliant form.
        cases = {
            "11": ("// trigraph ??=\nint f(){return 0;}\n", "// ordinary comment\nint f(){return 0;}\n"),
            "12": ("int f()<% return 0; %>\n", "int f(){ return 0; }\n"),
            "14": ("int f(){return 1u;}\n", "int f(){return 1U;}\n"),
            "18": ("#include <stddef.h>\nstruct S{int x;}; int f(){return offsetof(S,x);}\n",
                   "#include <stddef.h>\nstruct S{int x;}; int f(){return 0;}\n"),
            "26": ("#ifdef FLAG\n#endif\nint f(){return 0;}\n", "#ifndef FLAG\n#define FLAG\n#endif\nint f(){return 0;}\n"),
            "29": ("#define F(x) (x)\nint f(){return F(1);}\n", "inline int f(int x){return x;}\n"),
            "30": ("#define VALUE 1\nint f(){return VALUE;}\n", "const int value=1; int f(){return value;}\n"),
            "31": ("#define VALUE 1\nint f(){return VALUE;}\n", "#ifndef VALUE\n#define VALUE\n#endif\nint f(){return 0;}\n"),
            "32": ("#include \"local.hpp\"\nint f(){return 0;}\n", "#include <local.h>\nint f(){return 0;}\n"),
            "33": ("#include \"local.h\"\nint f(){return 0;}\n", "#include <local.h>\nint f(){return 0;}\n"),
            "41": ("//" + "x" * 121 + "\nint f(){return 0;}\n", "// short\nint f(){return 0;}\n"),
            "43": ("int f(){\treturn 0;}\n", "int f(){  return 0;}\n"),
            "47": ("int _local = 0;\n", "int local = 0;\n"),
            "50": ("class bad_name {};\n", "class Good_name {};\n"),
            "51": ("int CamelCase = 0;\n", "int camel_case = 0;\n"),
            "52": ("enum E { BAD = 0 };\n", "enum E { good = 0 };\n"),
            "59": ("int f(int x){if(x) return 1; return 0;}\n", "int f(int x){if(x) {return 1;} return 0;}\n"),
            "61": ("int f(){return 0;}\n", "int f()\n{\n  return 0;\n}\n"),
            "99": ("namespace a { namespace b { namespace c {} } }\n", "namespace a { namespace b {} }\n"),
            "108": ("void f(int x, ...) {}\n", "void f(int x) {}\n"),
            "110": ("void f(int a,int b,int c,int d,int e,int g,int h,int j) {}\n", "void f(int a,int b) {}\n"),
            "113": ("int f(int x){if(x){return 1;} return 0;}\n", "int f(){return 0;}\n"),
            "126": ("/* comment */\nint f(){return 0;}\n", "// comment\nint f(){return 0;}\n"),
            "149": ("int f(){return 01;}\n", "int f(){return 1;}\n"),
            "150": ("int f(){return 0xab;}\n", "int f(){return 0XAB;}\n"),
            "152": ("int a,b;\n", "int a;\nint b;\n"),
            "153": ("union U { int x; };\n", "struct U { int x; };\n"),
            "154": ("struct S { int x:2; };\n", "struct S { unsigned int x:2; };\n"),
            "159": ("struct S { bool operator&&(const S&) const { return true; } };\n",
                    "struct S { bool operator==(const S&) const { return true; } };\n"),
            "185": ("int f(double x){return (int)x;}\n", "int f(double x){return static_cast<int>(x);}\n"),
            "188": ("int f(){again: return 0;}\n", "int f(){return 0;}\n"),
            "189": ("int f(){goto end; end: return 0;}\n", "int f(){return 0;}\n"),
            "190": ("int f(){for(int x=0;x<2;++x){continue;} return 0;}\n", "int f(){for(int x=0;x<2;++x){} return 0;}\n"),
            "191": ("int f(){while(true){break;} return 0;}\n", "int f(int x){switch(x){case 1: break; default: break;} return 0;}\n"),
            "200": ("int f(){int x=0; for(;x<2;){++x;} return x;}\n", "int f(){for(int x=0;x<2;++x){} return 0;}\n"),
            "208": ("int f(){try{throw 1;}catch(...){return 0;}}\n", "int f(){return 0;}\n"),
        }
        (self.root / "local.hpp").write_text("#pragma once\n", encoding="utf-8")
        (self.root / "local.h").write_text("#ifndef LOCAL_H\n#define LOCAL_H\n#endif\n", encoding="utf-8")
        for rule, (bad, good) in cases.items():
            with self.subTest(rule=rule):
                self.source.write_text(bad, encoding="utf-8")
                found = {r["ruleId"] for r in self.scan()["results"] if r["classification"] == "violation"}
                self.assertIn(rule, found)
                self.source.write_text(good, encoding="utf-8")
                found = {r["ruleId"] for r in self.scan()["results"] if r["classification"] == "violation"}
                self.assertNotIn(rule, found)

    def test_filename_and_register_rules(self):
        header = self.root / "extra.hpp"
        header.write_text("// header\n", encoding="utf-8")
        literal = json.loads(run("scan", header, "--format", "json").stdout)
        self.assertIn("53", {r["ruleId"] for r in literal["results"] if r["classification"] == "violation"})
        modern = json.loads(run("scan", header, "--profile", "modern", "--format", "json").stdout)
        self.assertNotIn("53", {r["ruleId"] for r in modern["results"] if r["classification"] == "violation"})

        other = self.root / "other.cc"
        other.write_text("int f(){return 0;}\n", encoding="utf-8")
        db = self.root / "compile_commands.json"
        db.write_text(json.dumps([{"directory": str(self.root), "file": str(other),
            "arguments": ["clang++", "-std=c++17", "-c", str(other)]}]), encoding="utf-8")
        literal = json.loads(run("scan", other, "-p", self.root, "--format", "json").stdout)
        self.assertIn("54", {r["ruleId"] for r in literal["results"] if r["classification"] == "violation"})
        modern = json.loads(run("scan", other, "-p", self.root, "--profile", "modern", "--format", "json").stdout)
        self.assertNotIn("54", {r["ruleId"] for r in modern["results"] if r["classification"] == "violation"})

        self.source.write_text("int f(){register int x=0; return x;}\n", encoding="utf-8")
        db.write_text(json.dumps([{"directory": str(self.root), "file": str(self.source),
            "arguments": ["clang++", "-std=c++14", "-c", str(self.source)]}]), encoding="utf-8")
        found = {r["ruleId"] for r in self.scan()["results"] if r["classification"] == "violation"}
        self.assertIn("140", found)

    def test_header_guard_rule(self):
        header = self.root / "empty.h"
        header.write_text("int x;\n", encoding="utf-8")
        bad = json.loads(run("scan", header, "--format", "json").stdout)
        self.assertIn("35", {r["ruleId"] for r in bad["results"] if r["classification"] == "violation"})
        header.write_text("#ifndef EMPTY_H\n#define EMPTY_H\nint x;\n#endif\n", encoding="utf-8")
        good = json.loads(run("scan", header, "--format", "json").stdout)
        self.assertNotIn("35", {r["ruleId"] for r in good["results"] if r["classification"] == "violation"})
        header.write_text("#pragma once\nint x;\n", encoding="utf-8")
        modern = json.loads(run("scan", header, "--profile", "modern", "--format", "json").stdout)
        self.assertNotIn("35", {r["ruleId"] for r in modern["results"] if r["classification"] == "violation"})


if __name__ == "__main__":
    unittest.main()
