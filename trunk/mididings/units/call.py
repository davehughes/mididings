# -*- coding: utf-8 -*-
#
# mididings
#
# Copyright (C) 2008-2009  Dominic Sacré  <dominic.sacre@gmx.de>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#

import _mididings

from mididings.units.base import _Unit

from mididings import event as _event

import thread as _thread
import subprocess as _subprocess


class _CallBase(_Unit):
    def __init__(self, fun, async, cont):
        self.fun = fun
        _Unit.__init__(self, _mididings.Call(self.do_call, async, cont))
    def do_call(self, ev):
        # add additional properties
        ev.__class__ = _event.MidiEvent
        return self.fun(ev)


class Call(_CallBase):
    def __init__(self, fun):
        _CallBase.__init__(self, fun, False, False)


class CallAsync(_CallBase):
    def __init__(self, fun):
        _CallBase.__init__(self, fun, True, True)


class CallThread(_CallBase):
    def __init__(self, fun):
        self.fun_thread = fun
        _CallBase.__init__(self, self.do_thread, True, True)
    def do_thread(self, ev):
        # need to make a copy of the event.
        # the underlying C++ object will become invalid when this function returns
        ev_copy = _event.MidiEvent(ev.type_, ev.port_, ev.channel_, ev.data1, ev.data2)
        _thread.start_new_thread(self.fun_thread, (ev_copy,))


class System(CallAsync):
    def __init__(self, cmd):
        self.cmd = cmd
        CallAsync.__init__(self, self.do_system)
    def do_system(self, ev):
        cmd = self.cmd(ev) if callable(self.cmd) else self.cmd
        _subprocess.Popen(cmd, shell=True)
