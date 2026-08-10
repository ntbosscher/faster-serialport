//go:build windows

package main

import (
	"errors"
	"reflect"
	"syscall"
	"unsafe"

	"go.bug.st/serial"
	"golang.org/x/sys/windows"
)

// commEventMask is the set of line-status changes we ask the driver to report.
// The values are raw Win32 EV_* masks, surfaced to JS unchanged so callers can
// test against them directly (e.g. event === 64 is EV_BREAK).
const commEventMask = windows.EV_BREAK | windows.EV_CTS | windows.EV_DSR |
	windows.EV_RLSD | windows.EV_RING | windows.EV_ERR

// eventWatcher runs a WaitCommEvent loop against an open port's handle and
// reports each event (or a fatal error) through emit until Stop is called or the
// device goes away.
type eventWatcher struct {
	stopEvent windows.Handle
	done      chan struct{}
}

// handleFromPort reaches the unexported `handle windows.Handle` field of
// go.bug.st/serial's Windows port via reflection. This depends on that library's
// private struct layout (serial_windows.go) and must be revisited on upgrade.
func handleFromPort(p serial.Port) (windows.Handle, bool) {
	v := reflect.ValueOf(p)
	if v.Kind() != reflect.Ptr || v.IsNil() {
		return 0, false
	}

	v = v.Elem()
	if v.Kind() != reflect.Struct {
		return 0, false
	}

	f := v.FieldByName("handle")
	if !f.IsValid() {
		return 0, false
	}

	// handle is unexported: read it through its address so reflect will yield it.
	f = reflect.NewAt(f.Type(), unsafe.Pointer(f.UnsafeAddr())).Elem()
	h, ok := f.Interface().(windows.Handle)
	return h, ok
}

// startEventWatcher begins watching p for comm events, invoking emit on the
// watcher goroutine. It returns nil (nothing started) if the port handle can't
// be reached, so the caller can release its callback slot.
func startEventWatcher(p serial.Port, emit func(err error, errorCode int, event int)) *eventWatcher {
	handle, ok := handleFromPort(p)
	if !ok {
		return nil
	}

	stopEvent, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return nil
	}

	w := &eventWatcher{stopEvent: stopEvent, done: make(chan struct{})}
	go w.run(handle, emit)
	return w
}

// Stop signals the watcher loop and waits for it to exit.
func (w *eventWatcher) Stop() {
	if w == nil {
		return
	}

	windows.SetEvent(w.stopEvent)
	<-w.done
	windows.CloseHandle(w.stopEvent)
}

func errno(err error) int {
	var e syscall.Errno
	if errors.As(err, &e) {
		return int(e)
	}
	return 0
}

// isFatal reports whether an error means the loop can't continue — the device
// was removed or the handle was closed.
func isFatal(err error) bool {
	switch errno(err) {
	case int(windows.ERROR_ACCESS_DENIED),
		int(windows.ERROR_INVALID_HANDLE),
		int(windows.ERROR_OPERATION_ABORTED):
		return true
	}
	return false
}

func (w *eventWatcher) run(handle windows.Handle, emit func(err error, errorCode int, event int)) {
	defer close(w.done)

	if err := windows.SetCommMask(handle, commEventMask); err != nil {
		emit(err, errno(err), 0)
		return
	}

	for {
		hEvent, err := windows.CreateEvent(nil, 1, 0, nil)
		if err != nil {
			emit(err, errno(err), 0)
			return
		}

		ov := &windows.Overlapped{HEvent: hEvent}

		var evtMask uint32
		err = windows.WaitCommEvent(handle, &evtMask, ov)

		if err != nil && errors.Is(err, windows.ERROR_IO_PENDING) {
			ret, werr := windows.WaitForMultipleObjects(
				[]windows.Handle{hEvent, w.stopEvent}, false, windows.INFINITE)
			if werr != nil {
				windows.CloseHandle(hEvent)
				emit(werr, errno(werr), 0)
				return
			}

			// stop signalled: cancel the pending wait and exit
			if ret == windows.WAIT_OBJECT_0+1 {
				windows.CancelIoEx(handle, ov)
				windows.CloseHandle(hEvent)
				return
			}

			var n uint32
			err = windows.GetOverlappedResult(handle, ov, &n, false)
		}

		windows.CloseHandle(hEvent)

		if err != nil {
			emit(err, errno(err), 0)
			if isFatal(err) {
				return
			}
			continue
		}

		if evtMask != 0 {
			emit(nil, 0, int(evtMask))
		}
	}
}
