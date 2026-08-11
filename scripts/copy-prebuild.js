// Stages the freshly built addon into prebuilds/<platform>-<arch>/ so it can be
// shipped and picked up by lib/bindings.js at runtime. Keep the arch set in sync
// with scripts/build-go.js.

const path = require("path");
const fs = require("fs");

const root = path.resolve(__dirname, "..");
const arch = process.env.TARGET_ARCH || process.arch;

const src = path.join(root, "build", "Release", "faster-serialport.node");
const destDir = path.join(root, "prebuilds", process.platform + "-" + arch);
const dest = path.join(destDir, "faster-serialport.node");

fs.mkdirSync(destDir, { recursive: true });
fs.copyFileSync(src, dest);

console.log("staged " + dest);

// Windows loads the Go backend as a sibling DLL at runtime (see src/addon.cc),
// so it must ship alongside the addon.
if (process.platform === "win32") {
  const dll = "libserial.dll";
  const dllDest = path.join(destDir, dll);
  fs.copyFileSync(path.join(root, "build-go", dll), dllDest);
  console.log("staged " + dllDest);
}
