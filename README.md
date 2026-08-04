
# Faster SerialPort

This is a stripped down, more performant version of [node-serialport](https://github.com/serialport/node-serialport). Actually works normally with Electron.

Supports macOS, Linux, and Windows.

## How it works

The serial I/O is implemented in Go using [go.bug.st/serial](https://github.com/bugst/go-serial),
compiled to a C static archive (`go build -buildmode=c-archive`). A thin
[node-addon-api](https://github.com/nodejs/node-addon-api) (C++) layer links that
archive and exposes it to `lib/index.js`.

```
lib/index.js  ──▶  lib/bindings.js  ──▶  faster-serialport.node (C++)  ──▶  libserial.a (Go)  ──▶  go.bug.st/serial
```

## Building from source

Requires a **Go toolchain** (1.21+), a C++ compiler, and Node's build tools.

```sh
npm install        # runs `npm run build` (build:go + build:addon)
```

- `npm run build:go` compiles `go-serial/` into `build-go/libserial.a`
- `npm run build:addon` runs `node-gyp rebuild` to compile `src/addon.cc` and link the archive

### Notes / limitations

- Software/hardware flow control options (`rtscts`, `xon`, `xoff`, `xany`) are
  accepted for API compatibility but are not applied — `go.bug.st/serial` does
  not expose flow-control configuration.
- `eventsCallback` is accepted but not fired by the native layer.
- On Windows the Go archive (GNU `ar`) must be linked with a toolchain that can
  consume it; this path is configured in `binding.gyp` but has not been
  validated on this platform.

## API 
```js
import FasterSerialPort from "faster-serialport";

const deviceInfos = await FasterSerialPort.list();

const deviceInfo = deviceInfos.filter(d => 
    d.path.indexOf(search) === -1 ||
    d.manufacturer.indexOf(search) === -1 ||
    d.serialNumber.indexOf(search) === -1 ||
    d.pnpId.indexOf(search) === -1 ||
    d.locationId.indexOf(search) === -1 ||
    d.vendorId.indexOf(search) === -1 ||
    d.productId.indexOf(search) === -1
)[0];

const device = new FasterSerialPort(deviceInfo.path, {
    autoOpen: false,
    baudRate: 9600,
    dataBits: 8,
    parity: "none",
    stopBits: 1,
});

await device.open();

device.setTimeout(500); // return prematurely from any operation that takes longer than 500ms

function writeData() {
    const data = Buffer...

    // blocks until write has finished or timeout expires. 
    // If timeout expires, will throw in format "Timeout writing to port: %d of %d bytes written"
    await device.write(data); 
}

function waitForKnownDataSize() {
    
    // blocks until the number of bytes specified are read or the timeout expires.
    // If timeout expires, will return what ever data has been read. 
    // Will not throw if timeout expires
    const data = await device.read(256); 
    if(data.length !== 256) throw new Error("missing data");
}


function pollForAnyData() {
    device.setTimeout(10);
    
    while(true) {
        const data = await device.read(256);

        if(data.length > 0) {
            device.setTimeout(500);
            return data;
        }
    }
}
```

## Credits

This package would not be possible without the folks over at [node-serialport](https://github.com/serialport/node-serialport). This started out
as a fork of their package and has morphed into something new.

