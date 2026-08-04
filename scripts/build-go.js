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

const env = { ...process.env, CGO_ENABLED: "1" };

// Match the addon's macOS deployment target so the linker doesn't warn about
// the Go objects being built for a newer OS.
if (process.platform === "darwin") {
  const min = "-mmacosx-version-min=10.13";
  env.CGO_CFLAGS = (env.CGO_CFLAGS ? env.CGO_CFLAGS + " " : "") + min;
  env.CGO_LDFLAGS = (env.CGO_LDFLAGS ? env.CGO_LDFLAGS + " " : "") + min;
}

execFileSync("go", ["build", "-buildmode=c-archive", "-o", out, "."], {
  cwd: path.join(root, "go-serial"),
  stdio: "inherit",
  env,
});

console.log("built " + out);
