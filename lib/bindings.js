const path = require('path')
const serialNumParser = require('./win32-sn-parser')
const { wrapWithHiddenComName } = require('./legacy')

let _binding
// getBinding loads the compiled node-addon-api module lazily so callers can
// configure logging or catch load errors before the first operation.
function getBinding() {
  if (_binding) return _binding

  const candidates = [
    path.join(__dirname, '..', 'build', 'Release', 'faster-serialport.node'),
    path.join(__dirname, '..', 'build', 'Debug', 'faster-serialport.node'),
  ]

  let lastErr
  for (const candidate of candidates) {
    try {
      _binding = require(candidate)
      return _binding
    } catch (err) {
      lastErr = err
    }
  }

  throw lastErr
}

// promisify adapts the native callback-last (err, result) functions into
// promise-returning functions.
function promisify(fxLookup) {
  return function () {
    const args = [...arguments]
    const fx = fxLookup()

    return new Promise((resolve, reject) => {
      args.push((err, obj) => {
        if (err === null) {
          resolve(obj)
          return
        }

        reject(err)
      })

      fx.call(null, ...args)
    })
  }
}

const asyncList = promisify(() => getBinding().list)
const asyncOpen = promisify(() => getBinding().open)
const asyncClose = promisify(() => getBinding().close)
const asyncRead = promisify(() => getBinding().read)
const asyncBufferedRead = promisify(() => getBinding().bufferedRead)
const asyncBufferedReadExt = promisify(() => getBinding().bufferedReadExt)
const asyncWrite = promisify(() => getBinding().write)
const asyncUpdate = promisify(() => getBinding().update)
const asyncSet = promisify(() => getBinding().set)
const asyncGet = promisify(() => getBinding().get)
const asyncGetBaudRate = promisify(() => getBinding().getBaudRate)
const asyncDrain = promisify(() => getBinding().drain)
const asyncFlush = promisify(() => getBinding().flush)

class SerialBinding {

  static async list() {
    const ports = await asyncList()

    // Recover the serial number from the pnp id when the backend didn't supply
    // one directly (Windows).
    return wrapWithHiddenComName(
      ports.map(port => {
        if (port.pnpId && !port.serialNumber) {
          const serialNumber = serialNumParser(port.pnpId)
          if (serialNumber) {
            return {
              ...port,
              serialNumber,
            }
          }
        }
        return port
      })
    )
  }

  static configureLogging(enabled, dir) {
    getBinding().configureLogging(enabled, dir)
  }

  constructor(opt = {}) {
    this.timeout = opt.timeout === undefined ? 10000 : opt.timeout
    this.bindingOptions = { ...opt.bindingOptions }
    this.fd = null
    this.writeOperation = null
  }

  get isOpen() {
    return this.fd !== null
  }

  async open(path, options) {
    this.openOptions = { ...this.bindingOptions, ...options }
    this.fd = await asyncOpen(path, this.openOptions)
  }

  async close() {
    const fd = this.fd
    this.fd = null
    return asyncClose(fd)
  }

  async read(buffer, offset, length) {
    try {
      const bytesRead = await asyncRead(this.fd, buffer, offset, length, this.timeout)
      return buffer.slice(0, bytesRead)
    } catch (err) {
      if (!this.isOpen) {
        err.canceled = true
      }

      throw err
    }
  }

  async bufferedRead(callback, timeout) {
    await asyncBufferedRead(this.fd, timeout, callback)
  }

  // opts: { idleAllowanceMs?, noDataTimeoutMs?, pollTimeoutMs?, batchSize? }
  async bufferedReadExt(callback, opts) {
    await asyncBufferedReadExt(this.fd, opts, callback)
  }

  async write(buffer, echoMode) {
    this.writeOperation = (async () => {
      if (buffer.length === 0) {
        return
      }

      await asyncWrite(this.fd, buffer, this.timeout, echoMode)
    })()

    return this.writeOperation
  }

  async update(options) {
    return asyncUpdate(this.fd, options)
  }

  async set(options) {
    return asyncSet(this.fd, options)
  }

  async get() {
    return asyncGet(this.fd)
  }

  async getBaudRate() {
    return asyncGetBaudRate(this.fd)
  }

  async drain() {
    await this.writeOperation
    return asyncDrain(this.fd)
  }

  async flush() {
    return asyncFlush(this.fd)
  }
}

module.exports = SerialBinding
