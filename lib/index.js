/* eslint-disable prettier/prettier */

const Binding = require("./bindings");

//  VALIDATION
const DATABITS = Object.freeze([5, 6, 7, 8])
const STOPBITS = Object.freeze([1, 1.5, 2])
const PARITY = Object.freeze(['none', 'even', 'mark', 'odd', 'space'])
const FLOWCONTROLS = Object.freeze(['xon', 'xoff', 'xany', 'rtscts'])

const defaultSettings = Object.freeze();

// const defaultSetFlags = Object.freeze({
//     brk: false,
//     cts: false,
//     dtr: true,
//     dts: false,
//     rts: true,
// })
class FastSerial {

    constructor(path, options) {
        this.settings = {
            autoOpen: true,
            endOnClose: false,
            baudRate: 9600,
            dataBits: 8,
            hupcl: true,
            lock: true,
            parity: 'none',
            rtscts: false,
            stopBits: 1,
            xany: false,
            xoff: false,
            xon: false,
            eventsCallback: function (a, b, c) {
                // default callback
            }
        };

        this.oneAtaTime = false;
        this.queueIsRunning = false;
    
        this.path = "";
        this.isOpen = false;
    
        this.binding = new Binding({});
    
        this._queue = [];

        const settings = { ...this.settings, ...options }
        this.settings = settings;

        if(!settings.eventsCallback || typeof settings.eventsCallback !== "function") {
            throw new Error("requires options.eventsCallback()")
        }

        if (!path) {
            throw new TypeError(`"path" is not defined: ${path}`)
        }

        this.path = path;

        if (settings.baudrate) {
            throw new TypeError(`"baudrate" is an unknown option, did you mean "baudRate"?`)
        }

        if (typeof settings.baudRate !== 'number') {
            throw new TypeError(`"baudRate" must be a number: ${settings.baudRate}`)
        }

        if (DATABITS.indexOf(settings.dataBits) === -1) {
            throw new TypeError(`"databits" is invalid: ${settings.dataBits}`)
        }

        if (STOPBITS.indexOf(settings.stopBits) === -1) {
            throw new TypeError(`"stopbits" is invalid: ${settings.stopbits}`)
        }

        if (PARITY.indexOf(settings.parity) === -1) {
            throw new TypeError(`"parity" is invalid: ${settings.parity}`)
        }

        this.binding = new Binding({
            bindingOptions: settings.bindingOptions,
        });
    }

    async runQueue(callback) {
        
        this._queue.push(callback);

        if(this.queueIsRunning) {
            return;
        }

        this.queueIsRunning = true;

        while(this._queue.length > 0) {
            const work = this._queue.shift();

            try {
                await work();
            } catch (e) {
                // ignore errors here b/c they're handled in the callback();
            }
        }

        this.queueIsRunning = false;
    }

    queue(callback) {
        return new Promise((resolve, reject) => {
            this.runQueue(() => {
                return callback().then(resolve, reject);
            });
        });
    }

    flush() {
        return this.binding.flush();
    }

    drain() {
        return this.binding.drain();
    }

    async open() {
        await this.binding.open(this.path, this.settings);
        this.isOpen = true;
    }

    update(options) {
        this.settings.baudRate = options.baudRate;
        return this.binding.update(this.settings);
    }

    close() {
        this.isOpen = false;
        return this.binding.close();
    }

    setTimeout(ms) {
        this.binding.timeout = ms;
    }

    async read(nBytes) {
        return this.queue(async () => {

            if(this.oneAtaTime) throw new Error("one at a time!");

            try {
                
                this.oneAtaTime = true;
                const i = await this.binding.read(Buffer.alloc(nBytes), 0, nBytes);
                this.oneAtaTime = false;

                return i;
            } catch (e) {
                this.oneAtaTime = false;
                throw e;
            }
        });
    }

    bufferedRead(callback, timeout = 1000) {
        return this.queue(async () => {

            if(this.oneAtaTime) throw new Error("one at a time!");

            try {

                this.oneAtaTime = true;
                const i = await this.binding.bufferedRead(callback, timeout);
                this.oneAtaTime = false;

                return i;
            } catch (e) {
                this.oneAtaTime = false;
                throw e;
            }
        });
    }

    bufferedReadExt(callback, opts = {}) {
        return this.queue(async () => {

            if(this.oneAtaTime) throw new Error("one at a time!");

            try {

                this.oneAtaTime = true;
                const i = await this.binding.bufferedReadExt(callback, opts);
                this.oneAtaTime = false;

                return i;
            } catch (e) {
                this.oneAtaTime = false;
                throw e;
            }
        });
    }

    async write(buf, echoMode = false) {

        if(Array.isArray(buf)) {
            buf = Buffer.from(buf);
        }

        return this.queue(async () => {

            if(this.oneAtaTime) throw new Error("one at a time!");

            try {
                
                this.oneAtaTime = true;
                const i = await this.binding.write(buf, echoMode);
                this.oneAtaTime = false;

                return i;
            } catch (e) {
                this.oneAtaTime = false;
                throw e;
            }
        });
    }

    static async list() {
        const list = await Binding.list();
        return list;
    }

    static configureLogging(enabled, dir) {
        Binding.configureLogging(enabled, dir);
    }
}

module.exports = FastSerial