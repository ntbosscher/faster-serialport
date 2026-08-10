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
