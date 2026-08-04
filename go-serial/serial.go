package main

import (
	"context"
	"errors"
	"fmt"
	"time"

	"go.bug.st/serial"
)

// portHandle holds an open port plus the mode it was opened with, so update()
// can change a single field (baudRate) and re-apply the whole mode.
type portHandle struct {
	port serial.Port
	mode *serial.Mode

	readConch  chan struct{}
	writeConch chan struct{}
}

func newPortHandle(port serial.Port, mode *serial.Mode) *portHandle {
	h := &portHandle{
		port:       port,
		mode:       mode,
		readConch:  make(chan struct{}, 1),
		writeConch: make(chan struct{}, 1),
	}

	// Seed each conch with a token so the first acquire succeeds. Taking the
	// token holds the conch; returning it releases it.
	h.readConch <- struct{}{}
	h.writeConch <- struct{}{}
	return h
}

var errIOTimeout = errors.New("i/o operation timed out")

// acquire takes conch, giving up with a timeout error if it isn't free before
// deadline.
func acquire(conch chan struct{}, deadline time.Time) error {
	ctx, cancel := context.WithDeadlineCause(context.Background(), deadline, errIOTimeout)
	defer cancel()

	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-conch:
		return nil
	}
}

func (p *portHandle) Close() error {
	// close is special and doesn't require locking "conch"
	return p.port.Close()
}

// Read fills up to len(dst) bytes (or until timeoutMs elapses) and returns the
// number of bytes read; a timeout is not an error and yields a partial
// (possibly zero) count.
func (p *portHandle) Read(dst []byte, timeoutMs int64) (int64, error) {
	deadline := time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)

	if err := acquire(p.readConch, deadline); err != nil {
		return 0, err
	}
	defer func() { p.readConch <- struct{}{} }()

	received := 0

	for received < len(dst) {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}

		if err := p.port.SetReadTimeout(remaining); err != nil {
			return 0, err
		}

		n, err := p.port.Read(dst[received:])

		received += n
		if err != nil {
			return 0, err
		}

		if n == 0 {
			// read timed out with no data
			break
		}
	}

	return int64(received), nil
}

// Write sends all of data (or until timeoutMs elapses). When echo is set it
// writes one byte at a time and waits to read the same byte back, so it holds
// the read conch too (half-duplex / RS-485 echoing devices).
func (p *portHandle) Write(data []byte, timeoutMs int64, echo bool) error {
	deadline := time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)

	if err := acquire(p.writeConch, deadline); err != nil {
		return err
	}

	defer func() { p.writeConch <- struct{}{} }()

	if echo {
		if err := acquire(p.readConch, deadline); err != nil {
			return err
		}

		defer func() { p.readConch <- struct{}{} }()
		return p.writeEcho(data, deadline)
	}

	return p.writeNormal(data, deadline)
}

func (p *portHandle) writeNormal(data []byte, deadline time.Time) error {
	written := 0

	for written < len(data) {
		n, err := p.port.Write(data[written:])

		written += n

		if err != nil {
			return err
		}
		if written < len(data) && time.Now().After(deadline) {
			return fmt.Errorf("Timeout writing to port: %d of %d bytes written", written, len(data))
		}
	}

	return nil
}

// writeEcho writes one byte at a time and waits to read the same byte back
// before moving on (used for half-duplex/echoing devices such as RS-485).
func (p *portHandle) writeEcho(data []byte, deadline time.Time) error {
	one := make([]byte, 1)
	for i := 0; i < len(data); i++ {

		for {
			n, err := p.port.Write(data[i : i+1])
			if err != nil {
				return err
			}

			if n == 1 {
				break
			}

			if time.Now().After(deadline) {
				return fmt.Errorf("timeout writing to port: %d of %d bytes written", i, len(data))
			}
		}

		for {
			remaining := time.Until(deadline)
			if remaining <= 0 {
				return fmt.Errorf("timeout writing to port: %d of %d bytes written", i, len(data))
			}

			if err := p.port.SetReadTimeout(remaining); err != nil {
				return err
			}

			n, err := p.port.Read(one)
			if err != nil {
				return err
			}

			if n == 1 && one[0] == data[i] {
				break
			}
		}
	}
	return nil
}

// chunkSink is the destination for BufferedRead's received data. Its buffers are
// filled by the read loop directly and handed off by ownership transfer, so the
// bytes are never copied between the port and the consumer.
type chunkSink interface {
	// newBatch returns a fresh writable buffer of size bytes for the read loop to
	// read into directly. Ownership returns to the sink via emit or free.
	newBatch(size int) []byte
	// emit hands the first n bytes of buf (obtained from newBatch) to the
	// consumer and transfers ownership; buf must not be touched afterward.
	emit(buf []byte, n int)
	// free releases a batch obtained from newBatch that was never emitted.
	free(buf []byte)
}

// bufferedReadOpts configures a BufferedRead loop.
type bufferedReadOpts struct {
	// idleAllowance is how long to wait for the first byte before returning; a
	// generous value keeps a slow-to-start device from being cut off.
	idleAllowance time.Duration

	// noDataTimeout is the idle allowance once data has started arriving.
	noDataTimeout time.Duration

	// pollTimeout bounds a single Read() poll.
	pollTimeout time.Duration
	// batchSize is the size of each buffer read into and handed to the sink.
	batchSize int
}

// BufferedRead reads from the port and hands batches of received bytes to sink
// until no new data arrives for opts.noDataTimeout. opts.idleAllowance applies
// until the first byte, so a slow-to-start device isn't cut off. Each batch is
// read into directly and handed to the sink by ownership transfer — no copy
// between the port and the consumer.
func (p *portHandle) BufferedRead(opts bufferedReadOpts, sink chunkSink) error {

	// hold the read conch for the whole loop so a concurrent read can't steal bytes
	select {
	case <-p.readConch:
	case <-time.After(min(30*time.Second, opts.idleAllowance)):
		return errIOTimeout
	}

	defer func() { p.readConch <- struct{}{} }()

	// Read straight into batch and flush it in one piece, rather than once per
	// Read, to limit how often the consumer crosses the cgo/JS boundary.
	batch := sink.newBatch(opts.batchSize)
	filled := 0

	flush := func() {
		if filled == 0 {
			return
		}
		sink.emit(batch, filled)
		batch = sink.newBatch(opts.batchSize)
		filled = 0
	}

	// done flushes any pending data, then releases the trailing (empty) batch.
	done := func(err error) error {
		flush()
		sink.free(batch)
		return err
	}

	idleDeadline := time.Now().Add(opts.noDataTimeout)

	for {
		if time.Now().After(idleDeadline) {
			return done(nil)
		}

		if err := p.port.SetReadTimeout(opts.pollTimeout); err != nil {
			return done(err)
		}

		n, err := p.port.Read(batch[filled:])
		if err != nil {
			return done(err)
		}

		if n == 0 {
			// gap in the data: emit what we have so the consumer sees it promptly
			flush()
			continue
		}

		idleDeadline = time.Now().Add(opts.idleAllowance)

		filled += n
		if filled >= len(batch) {
			flush()
		}
	}
}

// SetBaudRate updates the baud rate (ignored when 0) and re-applies the full mode.
func (p *portHandle) SetBaudRate(baud int) error {
	if baud != 0 {
		p.mode.BaudRate = baud
	}
	return p.port.SetMode(p.mode)
}

func (p *portHandle) BaudRate() int {
	return p.mode.BaudRate
}

// ApplyControlLines sets RTS/DTR when non-nil and pulses a break when brk is true.
func (p *portHandle) ApplyControlLines(rts, dtr, brk *bool) error {
	if rts != nil {
		if err := p.port.SetRTS(*rts); err != nil {
			return err
		}
	}
	if dtr != nil {
		if err := p.port.SetDTR(*dtr); err != nil {
			return err
		}
	}
	if brk != nil && *brk {
		if err := p.port.Break(250 * time.Millisecond); err != nil {
			return err
		}
	}
	return nil
}

func (p *portHandle) ModemStatus() (*serial.ModemStatusBits, error) {
	return p.port.GetModemStatusBits()
}

func (p *portHandle) Drain() error {
	return p.port.Drain()
}

// Flush discards buffered input and output.
func (p *portHandle) Flush() error {
	if err := p.port.ResetInputBuffer(); err != nil {
		return err
	}

	return p.port.ResetOutputBuffer()
}

func main() {}
