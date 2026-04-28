import { execFileSync } from "node:child_process";
import { readFile } from "node:fs/promises";
import path from "node:path";

const root = process.cwd();
const source = "native/kvm_backend.cc";
const objectDir = "build/Release/obj.target/node_vmm_native/native";
const exclusionsPath = path.join(root, "docs", "cpp-coverage-exclusions.json");

function runGcov() {
  execFileSync("gcov", ["-b", "-c", source, "-o", objectDir], { cwd: root, stdio: "pipe" });
}

function parseLine(line) {
  const match = /^([^:]+):\s*(\d+):(.*)$/.exec(line);
  if (!match) {
    return null;
  }
  return { count: match[1].trim(), line: Number.parseInt(match[2], 10), text: match[3] };
}

const exclusions = JSON.parse(await readFile(exclusionsPath, "utf8"));
function isExcluded(line) {
  return exclusions.some((item) => item.file === source && line >= item.start && line <= item.end);
}

runGcov();
const gcov = await readFile(path.join(root, "kvm_backend.cc.gcov"), "utf8");
const uncovered = [];
for (const rawLine of gcov.split("\n")) {
  const parsed = parseLine(rawLine);
  if (!parsed || parsed.count !== "#####" || isExcluded(parsed.line)) {
    continue;
  }
  if (parsed.text.trim().length === 0 || parsed.text.includes("NODE_VMM_COV_EXCLUDE")) {
    continue;
  }
  uncovered.push(`${source}:${parsed.line}: ${parsed.text.trim()}`);
}

if (uncovered.length > 0) {
  throw new Error(`uncovered C++ lines without an explicit exclusion:\n${uncovered.join("\n")}`);
}

process.stdout.write("gcov ok: uncovered native lines are covered or explicitly excluded\n");
