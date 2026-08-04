package main

import (
	"errors"
	"math"
	"sync"

	"go.bug.st/serial"
)

type managerObj struct {
	muPorts   sync.RWMutex
	idCounter int64
	ports     map[int64]*portHandle
}

func (o *managerObj) Open(name string, mode *serial.Mode) (id int64, err error) {

	port, err := serial.Open(name, mode)
	if err != nil {
		logf("open %s failed: %v", name, err)
		return
	}

	o.muPorts.Lock()
	defer o.muPorts.Unlock()

	// id searcher
	for attempts := 0; true; attempts++ {
		o.idCounter++

		// reset id counter after max-int32
		if o.idCounter+10 > math.MaxInt32 {
			o.idCounter = 1
		}

		id = o.idCounter

		if _, ok := o.ports[id]; !ok {
			break
		}

		if attempts > 1000 {
			port.Close()
			return 0, errors.New("unable to get manager.id")
		}
	}

	o.ports[id] = newPortHandle(port, mode)
	return
}

func (o *managerObj) get(id int64) *portHandle {
	o.muPorts.RLock()
	defer o.muPorts.RUnlock()

	return o.ports[id]
}

func (o *managerObj) remove(id int64) {
	o.muPorts.Lock()
	defer o.muPorts.Unlock()

	delete(o.ports, id)
}

var manager = &managerObj{
	muPorts: sync.RWMutex{},
	ports:   map[int64]*portHandle{},
}
