// -*- c-basic-offset: 4 -*-
#ifndef CLICK_RANGEIPLOOKUPMPATHMT_HH
#define CLICK_RANGEIPLOOKUPMPATHMT_HH
#include <click/glue.hh>
#include <click/multithread.hh>
#include "rangeiplookupmpath.hh"
CLICK_DECLS

class RangeIPLookupMPathMT : public RangeIPLookupMPath { public:

    RangeIPLookupMPathMT() CLICK_COLD;
    ~RangeIPLookupMPathMT() CLICK_COLD;

    const char *class_name() const { return "RangeIPLookupMPathMT"; }

    int initialize(ErrorHandler *) CLICK_COLD;
    int add_route(const IPRouteMPath&, bool, IPRouteMPath*, ErrorHandler *);
    int remove_route(const IPRouteMPath&, IPRouteMPath*, ErrorHandler *);
    int lookup_route(IPAddress, IPAddress&, uint32_t) const;

  protected:

    void read_begin(int&) const;
    void read_end(int&) const;
    void write_begin();
    void write_end();
    void flush_table();

  private:

    struct Snapshot {
        Snapshot();
        ~Snapshot();

        uint32_t *range_base;
        uint32_t *range_len;
        uint32_t *range_t;
        Vector<DirectIPLookupMPath::GWPortArr> lookup;
    };

    bool snapshots_allocated() const;
    void copy_snapshot(Snapshot&);
    void publish_snapshot();

    Snapshot _snapshots[2];
    mutable fast_rcu<Snapshot *> _snapshot;
    mutable per_thread<Snapshot *> _read_snapshot;
    Spinlock _writer_lock;

};

CLICK_ENDDECLS
#endif
