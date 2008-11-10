/*
 * mididings
 *
 * Copyright (C) 2008  Dominic Sacré  <dominic.sacre@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef ENABLE_JACK_MIDI

#include "backend_jack.hh"
#include "midi_event.hh"
#include "config.hh"

#include <jack/jack.h>
#include <jack/midiport.h>

#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>

#include "util/debug.hh"


#include <iostream>


/*
 * JACK backend base class
 */

BackendJack::BackendJack(std::string const & client_name,
                         std::vector<std::string> const & in_portnames,
                         std::vector<std::string> const & out_portnames)
  : _in_ports(in_portnames.size())
  , _out_ports(out_portnames.size())
  , _frame(0)
{
    ASSERT(!client_name.empty());
    ASSERT(!in_portnames.empty());
    ASSERT(!out_portnames.empty());

    // create JACK client
    if ((_client = jack_client_open(client_name.c_str(), JackNullOption, NULL)) == 0) {
        throw BackendError("can't connect to jack server");
    }

    jack_set_process_callback(_client, &process_, static_cast<void*>(this));

    // create input ports
    for (int n = 0; n < static_cast<int>(in_portnames.size()); n++) {
        _in_ports[n] = jack_port_register(_client, in_portnames[n].c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (_in_ports[n] == NULL) {
            throw BackendError("error creating input port");
        }
    }

    // create output ports
    for (int n = 0; n < static_cast<int>(out_portnames.size()); n++) {
        _out_ports[n] = jack_port_register(_client, out_portnames[n].c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (_out_ports[n] == NULL) {
            throw BackendError("error creating output port");
        }
    }

    if (jack_activate(_client)) {
        throw BackendError("can't activate client");
    }
}


BackendJack::~BackendJack()
{
    jack_deactivate(_client);
    jack_client_close(_client);
}


int BackendJack::process_(jack_nframes_t nframes, void *arg)
{
    BackendJack *this_ = static_cast<BackendJack*>(arg);

    this_->_input_port = 0;
    this_->_input_count = 0;

    int r = this_->process(nframes);

    this_->_frame += nframes;
    return r;
}


void BackendJack::clear_buffers(jack_nframes_t nframes)
{
    for (int n = 0; n < static_cast<int>(_out_ports.size()); ++n) {
        void *port_buffer = jack_port_get_buffer(_out_ports[n], nframes);
        jack_midi_clear_buffer(port_buffer);
    }
}


bool BackendJack::read_event_from_buffer(MidiEvent & ev, jack_nframes_t nframes)
{
    if (_input_port < static_cast<int>(_in_ports.size()))
    {
        void *port_buffer = jack_port_get_buffer(_in_ports[_input_port], nframes);
        int num_events = jack_midi_get_event_count(port_buffer);

        if (_input_count < num_events) {
            jack_midi_event_t jack_ev;
            jack_midi_event_get(&jack_ev, port_buffer, _input_count);

            //std::cout << "in: " << jack_ev.time << std::endl;

            ev = jack_to_midi_event(jack_ev, _input_port);
            ev.frame = _frame + jack_ev.time;

            if (++_input_count >= num_events) {
                ++_input_port;
                _input_count = 0;
            }

            return true;
        }

        ++_input_port;
    }

    return false;
}


void BackendJack::write_event_to_buffer(MidiEvent const & ev, jack_nframes_t nframes)
{
    unsigned char data[3];
    std::size_t len;
    int port;
    midi_event_to_jack(ev, data, len, port);

    if (len) {
        void *port_buffer = jack_port_get_buffer(_out_ports[port], nframes);

        jack_nframes_t f;

        if (ev.frame >= _frame) {
            // event received within current period, zero delay
            f = ev.frame - _frame;
        } else if (ev.frame >= _frame - nframes) {
            // event received during last period, exactly one period delay (minimize jitter)
            f = ev.frame - _frame + nframes;
        } else {
            // event is older, send as soon as possible (minimize latency)
            f = 0;
        }

        //std::cout << "out: " << f << std::endl;
        jack_midi_event_write(port_buffer, f, data, len);
    }
}


MidiEvent BackendJack::jack_to_midi_event(jack_midi_event_t const & jack_ev, int port)
{
    MidiEvent ev;

    jack_midi_data_t *data = jack_ev.buffer;

    ev.port = port;
    ev.channel = data[0] & 0x0f;

    switch (data[0] & 0xf0) {
      case 0x90:
        ev.type = MIDI_EVENT_NOTEON;
        ev.note.note = data[1];
        ev.note.velocity = data[2];
        break;
      case 0x80:
        ev.type = MIDI_EVENT_NOTEOFF;
        ev.note.note = data[1];
        ev.note.velocity = data[2];
        break;
      case 0xb0:
        ev.type = MIDI_EVENT_CTRL;
        ev.ctrl.param = data[1];
        ev.ctrl.value = data[2];
        break;
      case 0xe0:
        ev.type = MIDI_EVENT_PITCHBEND;
        ev.ctrl.param = 0;
        ev.ctrl.value = (data[2] << 7 | data[1]) - 8192;
        break;
      case 0xd0:
        ev.type = MIDI_EVENT_AFTERTOUCH;
        ev.ctrl.param = 0;
        ev.ctrl.value = data[1];
        break;
      case 0xc0:
        ev.type = MIDI_EVENT_PROGRAM;
        ev.ctrl.param = 0;
        ev.ctrl.value = data[1];
        break;
      default:
        ev.type = MIDI_EVENT_NONE;
        break;
    }

    return ev;
}


void BackendJack::midi_event_to_jack(MidiEvent const & ev, unsigned char *data, std::size_t & len, int & port)
{
    switch (ev.type) {
      case MIDI_EVENT_NOTEON:
        len = 3;
        data[0] = 0x90;
        data[1] = ev.note.note;
        data[2] = ev.note.velocity;
        break;
      case MIDI_EVENT_NOTEOFF:
        len = 3;
        data[0] = 0x80;
        data[1] = ev.note.note;
        data[2] = ev.note.velocity;
        break;
      case MIDI_EVENT_CTRL:
        len = 3;
        data[0] = 0xb0;
        data[1] = ev.ctrl.param;
        data[2] = ev.ctrl.value;
        break;
      case MIDI_EVENT_PITCHBEND:
        len = 3;
        data[0] = 0xe0;
        data[1] = (ev.ctrl.value + 8192) % 128;
        data[2] = (ev.ctrl.value + 8192) / 128;
        break;
      case MIDI_EVENT_AFTERTOUCH:
        len = 2;
        data[0] = 0xd0;
        data[1] = ev.ctrl.value;
        break;
      case MIDI_EVENT_PROGRAM:
        len = 2;
        data[0] = 0xc0;
        data[1] = ev.ctrl.value;
        break;
      default:
        len = 0;
    }

    data[0] |= ev.channel;

    port = ev.port;
}


/*
 * buffered JACK backend
 */

BackendJackBuffered::BackendJackBuffered(std::string const & client_name,
                                         std::vector<std::string> const & in_portnames,
                                         std::vector<std::string> const & out_portnames)
  : BackendJack(client_name, in_portnames, out_portnames)
  , _in_rb(Config::MAX_JACK_EVENTS)
  , _out_rb(Config::MAX_JACK_EVENTS)
  , _quit(false)
{
}


BackendJackBuffered::~BackendJackBuffered()
{
    _quit = true;
    _cond.notify_one();
}


void BackendJackBuffered::start(InitFunction init, CycleFunction cycle)
{
    // clear input event buffer
    _in_rb.reset();

    // start processing thread
    _thrd.reset(new boost::thread(
        (boost::lambda::bind(init), boost::lambda::bind(cycle))
    ));
}


int BackendJackBuffered::process(jack_nframes_t nframes)
{
    MidiEvent ev;

    // store all incoming events in the input ringbuffer
    while (read_event_from_buffer(ev, nframes)) {
        _in_rb.write(ev);
        _cond.notify_one();
    }

    // clear all JACK output buffers
    clear_buffers(nframes);

    // read all events from output ringbuffer, write them to JACK output buffers
    while (_out_rb.read_space()) {
        _out_rb.read(ev);
        write_event_to_buffer(ev, nframes);
    }

    return 0;
}


bool BackendJackBuffered::input_event(MidiEvent & ev)
{
    // wait until there are events to be read from the ringbuffer
    while (!_in_rb.read_space()) {
        boost::mutex::scoped_lock lock(_mutex);
        _cond.wait(lock);

        // check for program termination
        if (_quit) {
            return false;
        }
    }

    _in_rb.read(ev);

    return true;
}


void BackendJackBuffered::output_event(MidiEvent const & ev)
{
    _out_rb.write(ev);
}


/*
 * realtime JACK backend
 */

BackendJackRealtime::BackendJackRealtime(std::string const & client_name,
                                         std::vector<std::string> const & in_portnames,
                                         std::vector<std::string> const & out_portnames)
  : BackendJack(client_name, in_portnames, out_portnames)
  , _out_rb(Config::MAX_JACK_EVENTS)
{
}


void BackendJackRealtime::start(InitFunction init, CycleFunction cycle)
{
    _run_init = init;
    _run_cycle = cycle;
}


int BackendJackRealtime::process(jack_nframes_t nframes)
{
    _nframes = nframes;

    clear_buffers(nframes);

    if (_run_init) {
        _run_init();
        _run_init.clear();
    }

    // write events from ringbuffer to JACK output buffers
    while (_out_rb.read_space()) {
        MidiEvent ev;
        _out_rb.read(ev);
        write_event_to_buffer(ev, nframes);
    }

    if (_run_cycle) {
        _run_cycle();
    }

    return 0;
}


bool BackendJackRealtime::input_event(MidiEvent & ev)
{
    return read_event_from_buffer(ev, _nframes);
}


void BackendJackRealtime::output_event(MidiEvent const & ev)
{
    if (pthread_self() == jack_client_thread_id(_client)) {
        // called within process(), write directly to output buffer
        write_event_to_buffer(ev, _nframes);
    } else {
        // called elsewhere, write to ringbuffer
        _out_rb.write(ev);
    }
}


#endif // ENABLE_JACK_MIDI