package main

/*
#include <stdlib.h>

// fsp_emit_chunk is implemented on the C++ (node-addon-api) side. The Go
// bufferedRead loop calls it once per chunk of received data. The C++ side must
// copy the bytes before returning; Go reuses the underlying buffer afterwards.
#ifdef __cplusplus
extern "C" {
#endif
extern void fsp_emit_chunk(int cbId, void* data, int len);
#ifdef __cplusplus
}
#endif
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
	"unsafe"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// portHandle holds an open port plus the mode it was opened with, so update()
// can change a single field (baudRate) and re-apply the whole mode.
type portHandle struct {
	port serial.Port
	mode *serial.Mode
}

var (
	handlesMu  sync.RWMutex
	handles    = map[int64]*portHandle{}
	nextHandle int64
)

func storeHandle(h *portHandle) int64 {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	nextHandle++
	id := nextHandle
	handles[id] = h
	return id
}

func getHandle(id int64) *portHandle {
	handlesMu.RLock()
	defer handlesMu.RUnlock()
	return handles[id]
}

func removeHandle(id int64) *portHandle {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	h := handles[id]
	delete(handles, id)
	return h
}

// cErr converts a Go error into a malloc'd C string (freed by the C++ side via
// fsp_free). A nil error yields a NULL pointer, which signals success.
func cErr(err error) *C.char {
	if err == nil {
		return nil
	}
	return C.CString(err.Error())
}

func cErrf(format string, args ...any) *C.char {
	return C.CString(fmt.Sprintf(format, args...))
}

//export fsp_free
func fsp_free(p unsafe.Pointer) {
	C.free(p)
}

// ---- logging -------------------------------------------------------------

var (
	logMu      sync.Mutex
	logEnabled bool
	logFile    *os.File
)

//export fsp_configure_logging
func fsp_configure_logging(enabled C.int, dir *C.char) {
	logMu.Lock()
	defer logMu.Unlock()

	if logFile != nil {
		logFile.Close()
		logFile = nil
	}

	logEnabled = enabled != 0
	if !logEnabled {
		return
	}

	path := filepath.Join(C.GoString(dir), "faster-serialport-go.log")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		logEnabled = false
		return
	}
	logFile = f
}

func logf(format string, args ...any) {
	logMu.Lock()
	defer logMu.Unlock()
	if !logEnabled || logFile == nil {
		return
	}
	fmt.Fprintf(logFile, time.Now().Format("15:04:05.000")+" "+format+"\n", args...)
}

// ---- list ----------------------------------------------------------------

type portInfo struct {
	Path         string `json:"path"`
	Manufacturer string `json:"manufacturer,omitempty"`
	SerialNumber string `json:"serialNumber,omitempty"`
	VendorID     string `json:"vendorId,omitempty"`
	ProductID    string `json:"productId,omitempty"`
}

//export fsp_list
func fsp_list(outJSON **C.char) *C.char {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return cErr(err)
	}

	list := []portInfo{}
	for _, p := range ports {
		item := portInfo{Path: p.Name}
		if p.IsUSB {
			item.Manufacturer = p.Manufacturer
			item.SerialNumber = p.SerialNumber
			item.VendorID = p.VID
			item.ProductID = p.PID
		}
		list = append(list, item)
	}

	data, err := json.Marshal(list)
	if err != nil {
		return cErr(err)
	}

	*outJSON = C.CString(string(data))
	return nil
}

// ---- open ----------------------------------------------------------------

type openOpts struct {
	BaudRate int     `json:"baudRate"`
	DataBits int     `json:"dataBits"`
	Parity   string  `json:"parity"`
	StopBits float64 `json:"stopBits"`
}

func modeFromOpts(o openOpts) (*serial.Mode, *C.char) {
	mode := &serial.Mode{
		BaudRate: o.BaudRate,
		DataBits: o.DataBits,
	}

	if mode.BaudRate == 0 {
		mode.BaudRate = 9600
	}
	if mode.DataBits == 0 {
		mode.DataBits = 8
	}

	switch o.Parity {
	case "", "none":
		mode.Parity = serial.NoParity
	case "odd":
		mode.Parity = serial.OddParity
	case "even":
		mode.Parity = serial.EvenParity
	case "mark":
		mode.Parity = serial.MarkParity
	case "space":
		mode.Parity = serial.SpaceParity
	default:
		return nil, cErrf("invalid parity setting %q", o.Parity)
	}

	switch o.StopBits {
	case 0, 1:
		mode.StopBits = serial.OneStopBit
	case 1.5:
		mode.StopBits = serial.OnePointFiveStopBits
	case 2:
		mode.StopBits = serial.TwoStopBits
	default:
		return nil, cErrf("invalid stop bits setting %v", o.StopBits)
	}

	return mode, nil
}

//export fsp_open
func fsp_open(path *C.char, optsJSON *C.char, outHandle *C.longlong) *C.char {
	opts := openOpts{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		return cErr(err)
	}

	mode, cerr := modeFromOpts(opts)
	if cerr != nil {
		return cerr
	}

	name := C.GoString(path)
	port, err := serial.Open(name, mode)
	if err != nil {
		logf("open %s failed: %v", name, err)
		return cErr(err)
	}

	id := storeHandle(&portHandle{port: port, mode: mode})
	logf("open %s -> handle %d", name, id)
	*outHandle = C.longlong(id)
	return nil
}

//export fsp_close
func fsp_close(id C.longlong) *C.char {
	h := removeHandle(int64(id))
	if h == nil {
		return nil
	}
	logf("close handle %d", int64(id))
	return cErr(h.port.Close())
}

// ---- read ----------------------------------------------------------------

// fsp_read fills up to length bytes (or until timeoutMs elapses) into the
// caller-owned buffer. It returns however many bytes were read; a timeout is
// not an error and yields a partial (possibly zero) count.
//
//export fsp_read
func fsp_read(id C.longlong, buf unsafe.Pointer, length C.int, timeoutMs C.int, outN *C.int) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	dst := unsafe.Slice((*byte)(buf), int(length))
	deadline := time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)

	received := 0
	for received < len(dst) {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		if err := h.port.SetReadTimeout(remaining); err != nil {
			*outN = C.int(received)
			return cErr(err)
		}

		n, err := h.port.Read(dst[received:])
		received += n
		if err != nil {
			*outN = C.int(received)
			return cErr(err)
		}
		if n == 0 {
			// read timed out with no data
			break
		}
	}

	*outN = C.int(received)
	return nil
}

// ---- write ---------------------------------------------------------------

//export fsp_write
func fsp_write(id C.longlong, buf unsafe.Pointer, length C.int, timeoutMs C.int, echoMode C.int) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	data := unsafe.Slice((*byte)(buf), int(length))
	deadline := time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)

	if echoMode != 0 {
		return writeEcho(h, data, deadline)
	}
	return writeNormal(h, data, deadline)
}

func writeNormal(h *portHandle, data []byte, deadline time.Time) *C.char {
	written := 0
	for written < len(data) {
		n, err := h.port.Write(data[written:])
		written += n
		if err != nil {
			return cErr(err)
		}
		if written < len(data) && time.Now().After(deadline) {
			return cErrf("Timeout writing to port: %d of %d bytes written", written, len(data))
		}
	}
	return nil
}

// writeEcho writes one byte at a time and waits to read the same byte back
// before moving on (used for half-duplex/echoing devices such as RS-485).
func writeEcho(h *portHandle, data []byte, deadline time.Time) *C.char {
	one := make([]byte, 1)
	for i := 0; i < len(data); i++ {

		for {
			n, err := h.port.Write(data[i : i+1])
			if err != nil {
				return cErr(err)
			}

			if n == 1 {
				break
			}

			if time.Now().After(deadline) {
				return cErrf("Timeout writing to port: %d of %d bytes written", i, len(data))
			}
		}

		for {
			remaining := time.Until(deadline)
			if remaining <= 0 {
				return cErrf("Timeout writing to port: %d of %d bytes written", i, len(data))
			}
			if err := h.port.SetReadTimeout(remaining); err != nil {
				return cErr(err)
			}

			n, err := h.port.Read(one)
			if err != nil {
				return cErr(err)
			}
			if n == 1 && one[0] == data[i] {
				break
			}
		}
	}
	return nil
}

// ---- bufferedRead --------------------------------------------------------

const (
	// readChunkSize bounds a single Read() from the port.
	readChunkSize = 4096

	// bufferedHighWater is the accumulated size at which a batch is flushed to
	// the callback even without a gap in the incoming data.
	bufferedHighWater = 8192
)

// fsp_buffered_read streams incoming data to the C++ side (via fsp_emit_chunk)
// until no new data arrives for noDataTimeoutMs. The initial idle allowance is
// 10s so a slow-to-start device isn't cut off; after the first byte the caller's
// timeout applies.
//
//export fsp_buffered_read
func fsp_buffered_read(id C.longlong, noDataTimeoutMs C.int, cbID C.int) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	// Accumulate incoming data and flush it to the callback in batches, rather
	// than once per Read, to limit how often we cross the cgo/JS boundary.
	var buf bytes.Buffer
	tmp := make([]byte, readChunkSize)

	// flush emits whatever has accumulated. The C++ side copies the bytes before
	// returning, so reusing buf's storage afterwards is safe.
	flush := func() {
		if buf.Len() == 0 {
			return
		}
		data := buf.Bytes()
		C.fsp_emit_chunk(cbID, unsafe.Pointer(&data[0]), C.int(len(data)))
		buf.Reset()
	}

	idleAllowance := 10 * time.Second
	pollTimeout := 50 * time.Millisecond
	idle := time.Duration(0)

	for {
		if idle > idleAllowance {
			flush()
			return nil
		}

		if err := h.port.SetReadTimeout(pollTimeout); err != nil {
			flush()
			return cErr(err)
		}

		n, err := h.port.Read(tmp)
		if err != nil {
			flush()
			return cErr(err)
		}

		if n == 0 {
			// gap in the data: emit what we have so the consumer sees it promptly
			flush()
			idle += pollTimeout
			continue
		}

		buf.Write(tmp[:n])
		if buf.Len() >= bufferedHighWater {
			flush()
		}

		idle = 0
		idleAllowance = time.Duration(noDataTimeoutMs) * time.Millisecond
	}
}

// ---- update / set / get / getBaudRate ------------------------------------

//export fsp_update
func fsp_update(id C.longlong, optsJSON *C.char) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	opts := struct {
		BaudRate int `json:"baudRate"`
	}{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		return cErr(err)
	}

	if opts.BaudRate != 0 {
		h.mode.BaudRate = opts.BaudRate
	}
	return cErr(h.port.SetMode(h.mode))
}

//export fsp_set
func fsp_set(id C.longlong, optsJSON *C.char) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	opts := struct {
		Rts *bool `json:"rts"`
		Dtr *bool `json:"dtr"`
		Brk *bool `json:"brk"`
	}{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		return cErr(err)
	}

	if opts.Rts != nil {
		if err := h.port.SetRTS(*opts.Rts); err != nil {
			return cErr(err)
		}
	}
	if opts.Dtr != nil {
		if err := h.port.SetDTR(*opts.Dtr); err != nil {
			return cErr(err)
		}
	}
	if opts.Brk != nil && *opts.Brk {
		if err := h.port.Break(250 * time.Millisecond); err != nil {
			return cErr(err)
		}
	}
	return nil
}

//export fsp_get
func fsp_get(id C.longlong, outJSON **C.char) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}

	bits, err := h.port.GetModemStatusBits()
	if err != nil {
		return cErr(err)
	}

	data, err := json.Marshal(map[string]bool{
		"cts": bits.CTS,
		"dsr": bits.DSR,
		"dcd": bits.DCD,
	})
	if err != nil {
		return cErr(err)
	}

	*outJSON = C.CString(string(data))
	return nil
}

//export fsp_get_baud_rate
func fsp_get_baud_rate(id C.longlong, outBaud *C.int) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}
	*outBaud = C.int(h.mode.BaudRate)
	return nil
}

// ---- drain / flush -------------------------------------------------------

//export fsp_drain
func fsp_drain(id C.longlong) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}
	return cErr(h.port.Drain())
}

//export fsp_flush
func fsp_flush(id C.longlong) *C.char {
	h := getHandle(int64(id))
	if h == nil {
		return cErrf("invalid handle %d", int64(id))
	}
	if err := h.port.ResetInputBuffer(); err != nil {
		return cErr(err)
	}
	return cErr(h.port.ResetOutputBuffer())
}

func main() {}
