// Builds the Go serial backend as a C static archive that binding.gyp links
// into the node-addon-api module. Output lands in build-go/ (kept separate
// from node-gyp's build/ so `node-gyp clean` doesn't wipe it).

const { execFileSync } = require("child_process");
const path = require("path");
const fs = require("fs");

const root = path.resolve(__dirname, "..");
const outDir = path.join(root, "build-go");
fs.mkdirSync(outDir, { recursive: true });

const out = path.join(outDir, "libserial.a");

// Target arch defaults to the host; CI sets TARGET_ARCH to build one prebuild
// per arch. Keep the mapping in sync with scripts/copy-prebuild.js.
const targetArch = process.env.TARGET_ARCH || process.arch;
const goarch = { x64: "amd64", ia32: "386", arm64: "arm64" }[targetArch];
if (!goarch) throw new Error("unsupported target arch: " + targetArch);

const env = { ...process.env, CGO_ENABLED: "1", GOARCH: goarch };

// Match the addon's macOS deployment target so the linker doesn't warn about
// the Go objects being built for a newer OS, and pin the arch so cgo's clang
// invocation agrees with GOARCH when cross-building x64 on an arm64 host.
if (process.platform === "darwin") {
  const arch = targetArch === "x64" ? "x86_64" : "arm64";
  const flags = "-arch " + arch + " -mmacosx-version-min=10.13";
  env.CGO_CFLAGS = (env.CGO_CFLAGS ? env.CGO_CFLAGS + " " : "") + flags;
  env.CGO_LDFLAGS = (env.CGO_LDFLAGS ? env.CGO_LDFLAGS + " " : "") + flags;
}

execFileSync("go", ["build", "-buildmode=c-archive", "-o", out, "."], {
  cwd: path.join(root, "go-serial"),
  stdio: "inherit",
  env,
});

console.log("built " + out);
