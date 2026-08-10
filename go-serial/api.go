package main

/*
#include <stdlib.h>

// fsp_emit_chunk is implemented on the C++ (node-addon-api) side. The Go
// bufferedRead loop calls it once per chunk of received data. data is a
// malloc'd buffer whose ownership transfers to the C++ side: it wraps it in a
// JS Buffer without copying and frees it (via fsp_free) when V8 collects that
// Buffer.
//
// fsp_complete is also implemented on the C++ side. Each fsp_*_async wrapper
// calls it exactly once to report its result: err is non-NULL on failure,
// resultJSON holds the JSON-encoded success value (or NULL when the operation
// has no result). The C++ side takes ownership of both strings and frees them.
#ifdef __cplusplus
extern "C" {
#endif
extern void fsp_emit_chunk(int cbId, void* data, int len);
extern void fsp_complete(int cbId, char* err, char* resultJson);
extern void fsp_complete_num(int cbId, long long value);

// fsp_emit_event delivers one comm event to the port's eventsCallback: err is
// non-NULL (with errorCode set) on a failure, otherwise event holds the Win32
// EV_* mask. The C++ side takes ownership of err and frees it. fsp_release_event
// releases the persistent events callback slot when the port closes.
extern void fsp_emit_event(int cbId, char* err, int errorCode, int event);
extern void fsp_release_event(int cbId);
#ifdef __cplusplus
}
#endif
*/
import "C"
import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime/debug"
	"sync"
	"time"
	"unsafe"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

//export fsp_free
func fsp_free(p unsafe.Pointer) {
	C.free(p)
}

// ---- completion ----------------------------------------------------------

// completeErr reports an operation with no success value: err is nil on success.
// A non-nil error is converted to a malloc'd C string that the C++ side frees.
func completeErr(cbID C.int, err error) {
	if err == nil {
		C.fsp_complete(cbID, nil, nil)
		return
	}
	C.fsp_complete(cbID, C.CString(err.Error()), nil)
}

// completeNum reports a numeric success value directly as a JS number (no JSON
// round-trip). On error the value is dropped.
func completeNum(cbID C.int, err error, n int64) {

	if err != nil {
		completeErr(cbID, err)
		return
	}

	C.fsp_complete_num(cbID, C.longlong(n))
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

//export fsp_list_async
func fsp_list_async(cbID C.int) {
	go func() {
		defer autoRecover(cbID)

		ports, err := enumerator.GetDetailedPortsList()
		if err != nil {
			completeErr(cbID, err)
			return
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
			completeErr(cbID, err)
			return
		}

		C.fsp_complete(cbID, nil, C.CString(string(data)))
	}()
}

// ---- open ----------------------------------------------------------------

type openOpts struct {
	BaudRate int     `json:"baudRate"`
	DataBits int     `json:"dataBits"`
	Parity   string  `json:"parity"`
	StopBits float64 `json:"stopBits"`
}

func modeFromOpts(o openOpts) (*serial.Mode, error) {
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
		return nil, fmt.Errorf("invalid parity setting %q", o.Parity)
	}

	switch o.StopBits {
	case 0, 1:
		mode.StopBits = serial.OneStopBit
	case 1.5:
		mode.StopBits = serial.OnePointFiveStopBits
	case 2:
		mode.StopBits = serial.TwoStopBits
	default:
		return nil, fmt.Errorf("invalid stop bits setting %v", o.StopBits)
	}

	return mode, nil
}

//export fsp_open_async
func fsp_open_async(path *C.char, optsJSON *C.char, eventCbID C.int, cbID C.int) {
	opts := openOpts{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		C.fsp_release_event(eventCbID)
		completeErr(cbID, err)
		return
	}

	mode, err := modeFromOpts(opts)
	if err != nil {
		C.fsp_release_event(eventCbID)
		completeErr(cbID, err)
		return
	}

	name := string(C.GoString(path))

	go func() {
		defer autoRecover(cbID)

		id, err := manager.Open(name, mode)
		logf("open %s -> handle %d", name, id)
		if err != nil {
			C.fsp_release_event(eventCbID)
			completeErr(cbID, err)
			return
		}

		// Attach the comm-event watcher. If none started (non-Windows, or the
		// handle couldn't be reached), release the callback slot now so it
		// doesn't leak — the port stays open, just without events.
		h := manager.get(id)
		emit := func(err error, errorCode int, event int) {
			var cErr *C.char
			if err != nil {
				cErr = C.CString(err.Error())
			}
			C.fsp_emit_event(eventCbID, cErr, C.int(errorCode), C.int(event))
		}
		if h == nil || !h.StartEvents(int(eventCbID), emit) {
			C.fsp_release_event(eventCbID)
		}

		completeNum(cbID, nil, id)
	}()
}

//export fsp_close_async
func fsp_close_async(id C.longlong, cbID C.int) {

	port := manager.get(int64(id))
	if port == nil {
		completeErr(cbID, errors.New("invalid handle"))
		return
	}

	eventCbID := port.eventCbID

	go func() {
		defer autoRecover(cbID)

		err := port.Close()
		manager.remove(int64(id))

		// Close stopped the watcher (no more emits); release its callback slot.
		if eventCbID != 0 {
			C.fsp_release_event(C.int(eventCbID))
		}

		logf("close handle %d", int64(id))
		completeErr(cbID, err)
	}()
}

// ---- read ----------------------------------------------------------------

// fsp_read_async fills up to length bytes (or until timeoutMs elapses) into the
// caller-owned buffer and reports the number of bytes read; a timeout is not an
// error and yields a partial (possibly zero) count.
//
//export fsp_read_async
func fsp_read_async(id C.longlong, buf unsafe.Pointer, length C.int, timeoutMs C.int, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	dst := unsafe.Slice((*byte)(buf), int(length))

	go func() {
		defer autoRecover(cbID)

		n, err := h.Read(dst, int64(timeoutMs))
		completeNum(cbID, err, n)
	}()
}

// ---- write ---------------------------------------------------------------

//export fsp_write_async
func fsp_write_async(id C.longlong, buf unsafe.Pointer, length C.int, timeoutMs C.int, echoMode C.int, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	data := unsafe.Slice((*byte)(buf), int(length))

	go func() {
		defer autoRecover(cbID)
		completeErr(cbID, h.Write(data, int64(timeoutMs), echoMode != 0))
	}()
}

// ---- bufferedRead --------------------------------------------------------

// cChunkSink feeds BufferedRead's data to dataCbID. Its batches are malloc'd C
// buffers read into directly; emit hands each one's ownership to the C++ side
// via fsp_emit_chunk, so the bytes are never copied on the way to JS.
type cChunkSink struct {
	dataCbID C.int
}

func (s cChunkSink) newBatch(size int) []byte {
	p := C.malloc(C.size_t(size))
	if p == nil {
		panic("fsp: out of memory allocating read batch")
	}
	return unsafe.Slice((*byte)(p), size)
}

func (s cChunkSink) emit(buf []byte, n int) {
	C.fsp_emit_chunk(s.dataCbID, unsafe.Pointer(&buf[0]), C.int(n))
}

func (s cChunkSink) free(buf []byte) {
	C.free(unsafe.Pointer(&buf[0]))
}

// defaultBufferedReadOpts is the tuning used when the caller doesn't override a
// given field.
func defaultBufferedReadOpts() bufferedReadOpts {
	return bufferedReadOpts{
		idleAllowance: 10 * time.Second,
		noDataTimeout: 10 * time.Second,
		pollTimeout:   50 * time.Millisecond,
		batchSize:     8192,
	}
}

// fsp_buffered_read_async streams incoming data to dataCbID (via fsp_emit_chunk)
// until no new data arrives for noDataTimeoutMs, then reports the loop's exit to
// doneCbID.
//
//export fsp_buffered_read_async
func fsp_buffered_read_async(id C.longlong, noDataTimeoutMs C.int, dataCbID C.int, doneCbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(doneCbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	opts := defaultBufferedReadOpts()
	opts.noDataTimeout = time.Duration(noDataTimeoutMs) * time.Millisecond

	go func() {
		defer autoRecover(doneCbID)
		completeErr(doneCbID, h.BufferedRead(opts, cChunkSink{dataCbID: dataCbID}))
	}()
}

// fsp_buffered_read_ext_async is fsp_buffered_read_async with all tuning exposed
// to the caller as JSON. Any field left zero/absent keeps its default. Durations
// are milliseconds; batchSize is bytes.
//
//export fsp_buffered_read_ext_async
func fsp_buffered_read_ext_async(id C.longlong, optsJSON *C.char, dataCbID C.int, doneCbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(doneCbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	in := struct {
		IdleAllowanceMs int `json:"idleAllowanceMs"`
		NoDataTimeoutMs int `json:"noDataTimeoutMs"`
		PollTimeoutMs   int `json:"pollTimeoutMs"`
		BatchSize       int `json:"batchSize"`
	}{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &in); err != nil {
		completeErr(doneCbID, err)
		return
	}

	opts := defaultBufferedReadOpts()
	if in.IdleAllowanceMs > 0 {
		opts.idleAllowance = time.Duration(in.IdleAllowanceMs) * time.Millisecond
	}
	if in.NoDataTimeoutMs > 0 {
		opts.noDataTimeout = time.Duration(in.NoDataTimeoutMs) * time.Millisecond
	}
	if in.PollTimeoutMs > 0 {
		opts.pollTimeout = time.Duration(in.PollTimeoutMs) * time.Millisecond
	}
	if in.BatchSize > 0 {
		opts.batchSize = in.BatchSize
	}

	go func() {
		defer autoRecover(doneCbID)
		completeErr(doneCbID, h.BufferedRead(opts, cChunkSink{dataCbID: dataCbID}))
	}()
}

// ---- update / set / get / getBaudRate ------------------------------------

//export fsp_update_async
func fsp_update_async(id C.longlong, optsJSON *C.char, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	opts := struct {
		BaudRate int `json:"baudRate"`
	}{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		completeErr(cbID, err)
		return
	}

	go func() {
		defer autoRecover(cbID)
		completeErr(cbID, h.SetBaudRate(opts.BaudRate))
	}()
}

//export fsp_set_async
func fsp_set_async(id C.longlong, optsJSON *C.char, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	opts := struct {
		Rts *bool `json:"rts"`
		Dtr *bool `json:"dtr"`
		Brk *bool `json:"brk"`
	}{}
	if err := json.Unmarshal([]byte(C.GoString(optsJSON)), &opts); err != nil {
		completeErr(cbID, err)
		return
	}

	go func() {
		defer autoRecover(cbID)
		completeErr(cbID, h.ApplyControlLines(opts.Rts, opts.Dtr, opts.Brk))
	}()
}

//export fsp_get_async
func fsp_get_async(id C.longlong, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	go func() {
		defer autoRecover(cbID)

		bits, err := h.ModemStatus()
		if err != nil {
			completeErr(cbID, err)
			return
		}

		data, err := json.Marshal(map[string]bool{
			"cts": bits.CTS,
			"dsr": bits.DSR,
			"dcd": bits.DCD,
		})
		if err != nil {
			completeErr(cbID, err)
			return
		}

		C.fsp_complete(cbID, nil, C.CString(string(data)))
	}()
}

//export fsp_get_baud_rate_async
func fsp_get_baud_rate_async(id C.longlong, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}
	completeNum(cbID, nil, int64(h.BaudRate()))
}

// ---- drain / flush -------------------------------------------------------

//export fsp_drain_async
func fsp_drain_async(id C.longlong, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	go func() {
		defer autoRecover(cbID)
		completeErr(cbID, h.Drain())
	}()
}

//export fsp_flush_async
func fsp_flush_async(id C.longlong, cbID C.int) {
	h := manager.get(int64(id))
	if h == nil {
		completeErr(cbID, fmt.Errorf("invalid handle %d", int64(id)))
		return
	}

	go func() {
		defer autoRecover(cbID)
		completeErr(cbID, h.Flush())
	}()
}

// autoRecover recovers a panic in an async goroutine and reports it to the
// callback, so one failed operation can't crash the process. Defer it only on
// paths where completeErr hasn't already run for this callback.
func autoRecover(cbID C.int) {
	r := recover()
	if r == nil {
		return
	}

	err, ok := r.(error)
	if !ok {
		err = fmt.Errorf("%v", r)
	}

	logf("recovered panic on cb %d: %v\n%s", int64(cbID), r, debug.Stack())
	completeErr(cbID, err)
}
