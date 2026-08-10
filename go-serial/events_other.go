//go:build !windows

package main

import "go.bug.st/serial"

// eventWatcher is a no-op on non-Windows platforms: comm-event notification
// (WaitCommEvent) has no portable equivalent, so eventsCallback never fires
// here. Disconnects still surface as errors from Read/Write.
type eventWatcher struct{}

func startEventWatcher(p serial.Port, emit func(err error, errorCode int, event int)) *eventWatcher {
	return nil
}

func (w *eventWatcher) Stop() {}
