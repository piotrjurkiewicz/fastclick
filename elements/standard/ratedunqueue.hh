// -*- c-basic-offset: 4 -*-
#ifndef CLICK_RATEDUNQUEUE_HH
#define CLICK_RATEDUNQUEUE_HH
#include <click/batchelement.hh>
#include <click/tokenbucket.hh>
#include <click/task.hh>
#include <click/timer.hh>
#include <click/notifier.hh>
CLICK_DECLS

/*
 * =c
 * RatedUnqueue(RATE, I[<KEYWORDS>])
 * =s shaping
 * pull-to-push converter
 * =d
 *
 * Pulls packets at the given RATE in packets per second, and pushes them out
 * its single output.  It is implemented with a token bucket.  The capacity of
 * this token bucket defaults to 20 milliseconds worth of tokens, but can be
 * customized by setting BURST_DURATION or BURST_SIZE.
 *
 * Keyword arguments are:
 *
 * =over 8
 *
 * =item RATE
 *
 * Integer.  Token bucket fill rate in packets per second.
 *
 * =item BURST_DURATION
 *
 * Time.  If specified, the capacity of the token bucket is calculated as
 * rate * burst_duration.
 *
 * =item BURST_SIZE
 *
 * Integer.  If specified, the capacity of the token bucket is set to this
 * value.
 *
 * =item THRESHOLD
 *
 * Integer.  The element will wait until it is
 * possible to pull at least threshold packets in order to reduce
 * runs and empty runs numbers.
 *
 * Defaults is 1, the element will pull from queue as soon
 * as it is possible to pull single packet.  This can result in
 * single-packet batches and increased number of empty runs.
 *
 * =h rate read/write
 *
 * =a BandwidthRatedUnqueue, Unqueue, Shaper, RatedSplitter */

class RatedUnqueue : public BatchElement { public:

    RatedUnqueue() CLICK_COLD;

    const char *class_name() const	{ return "RatedUnqueue"; }
    const char *port_count() const	{ return PORTS_1_1; }
    const char *processing() const	{ return PULL_TO_PUSH; }
    bool is_bandwidth() const		{ return class_name()[0] == 'B'; }

    int configure(Vector<String> &, ErrorHandler *) CLICK_COLD;
    static int configure_helper(TokenBucket *tb, bool is_bandwidth, Element *elt, Vector<String> &conf, ErrorHandler *errh);
    enum { tb_bandwidth_thresh = 131072 };

    bool can_live_reconfigure() const	{ return true; }
    int initialize(ErrorHandler *) CLICK_COLD;
    void add_handlers() CLICK_COLD;

    bool run_task(Task *);

  protected:

    TokenBucket _tb;
    Task _task;
    Timer _timer;
    NotifierSignal _signal;
    uint32_t _runs;
    uint32_t _packets;
    uint32_t _pushes;
    uint32_t _failed_pulls;
    uint32_t _empty_runs;
    uint32_t _burst;

    enum { h_calls, h_rate };

    static String read_handler(Element *e, void *thunk) CLICK_COLD;

    bool _active;
    int _thresh;
};

CLICK_ENDDECLS
#endif
