// -*- c-basic-offset: 4 -*-
#include <click/config.h>
#include <click/error.hh>
#include "rangeiplookupmpathmt.hh"
CLICK_DECLS

RangeIPLookupMPathMT::Snapshot::Snapshot()
    : range_base((uint32_t *) CLICK_LALLOC((1 << KICKSTART_BITS) * sizeof(uint32_t))),
      range_len((uint32_t *) CLICK_LALLOC((1 << KICKSTART_BITS) * sizeof(uint32_t))),
      range_t((uint32_t *) CLICK_LALLOC(RANGES_MAX * sizeof(uint32_t)))
{
}

RangeIPLookupMPathMT::Snapshot::~Snapshot()
{
    CLICK_LFREE(range_base, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    CLICK_LFREE(range_len, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    CLICK_LFREE(range_t, RANGES_MAX * sizeof(uint32_t));
}

RangeIPLookupMPathMT::RangeIPLookupMPathMT()
    : _read_snapshot(0)
{
    _snapshot.initialize(&_snapshots[0]);
}

RangeIPLookupMPathMT::~RangeIPLookupMPathMT()
{
}

bool
RangeIPLookupMPathMT::snapshots_allocated() const
{
    return _snapshots[0].range_base && _snapshots[0].range_len && _snapshots[0].range_t
        && _snapshots[1].range_base && _snapshots[1].range_len && _snapshots[1].range_t;
}

void
RangeIPLookupMPathMT::copy_snapshot(Snapshot &snapshot)
{
    memcpy(snapshot.range_base, _range_base, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    memcpy(snapshot.range_len, _range_len, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    memcpy(snapshot.range_t, _range_t, RANGES_MAX * sizeof(uint32_t));
    snapshot.lookup = _helper._lookup;
}

void
RangeIPLookupMPathMT::publish_snapshot()
{
    int token;
    Snapshot *current = _snapshot.read();
    Snapshot *next_snapshot = current == &_snapshots[0] ? &_snapshots[1] : &_snapshots[0];
    Snapshot *&next = _snapshot.write_begin(token);
    copy_snapshot(*next_snapshot);
    next = next_snapshot;
    _snapshot.write_commit(token);
}

int
RangeIPLookupMPathMT::initialize(ErrorHandler *errh)
{
    if (!snapshots_allocated())
        return errh->error("out of memory");
    int result = RangeIPLookupMPath::initialize(errh);
    if (result < 0)
        return result;
    copy_snapshot(_snapshots[0]);
    copy_snapshot(_snapshots[1]);
    return 0;
}

int
RangeIPLookupMPathMT::add_route(const IPRouteMPath &route, bool allow_replace,
                                IPRouteMPath *old_route, ErrorHandler *errh)
{
    int result = _helper.add_route(route, allow_replace, old_route, errh);
    if (result == 0 && _active) {
        expand();
        publish_snapshot();
    }
    return result;
}

int
RangeIPLookupMPathMT::remove_route(const IPRouteMPath &route,
                                   IPRouteMPath *old_route, ErrorHandler *errh)
{
    int result = _helper.remove_route(route, old_route, errh);
    if (result == 0 && _active) {
        expand();
        publish_snapshot();
    }
    return result;
}

int
RangeIPLookupMPathMT::lookup_route(IPAddress dest, IPAddress &gw, uint32_t hash) const
{
    Snapshot *snapshot = *_read_snapshot;
    uint32_t ip_addr = ntohl(dest.addr());
    uint32_t i = ip_addr >> RANGE_SHIFT;
    uint32_t lowerbound = snapshot->range_base[i];
    uint32_t upperbound = lowerbound + snapshot->range_len[i];
    i = ip_addr & RANGE_MASK;

    while (upperbound > lowerbound) {
        uint32_t middle = (upperbound + lowerbound) >> 1;
        if (i < (snapshot->range_t[middle] & RANGE_MASK))
            upperbound = middle;
        else if (i < (snapshot->range_t[middle + 1] & RANGE_MASK)) {
            lowerbound = middle;
            break;
        } else
            lowerbound = middle + 1;
    }

    uint16_t lookup_key = snapshot->range_t[lowerbound] >> RANGE_SHIFT;
    if (lookup_key > 0 && lookup_key <= (uint16_t) snapshot->lookup.size()) {
        const DirectIPLookupMPath::GWPortArr &arr = snapshot->lookup[lookup_key - 1];
        int n = hash % arr.size();
        gw = arr[n].gw;
        return arr[n].port;
    }
    gw = 0;
    return -1;
}

void
RangeIPLookupMPathMT::read_begin(int &token) const
{
    _read_snapshot = _snapshot.read_begin(token);
}

void
RangeIPLookupMPathMT::read_end(int &token) const
{
    _read_snapshot = 0;
    _snapshot.read_end(token);
}

void
RangeIPLookupMPathMT::write_begin()
{
    _writer_lock.acquire();
}

void
RangeIPLookupMPathMT::write_end()
{
    _writer_lock.release();
}

void
RangeIPLookupMPathMT::flush_table()
{
    RangeIPLookupMPath::flush_table();
    if (_active)
        publish_snapshot();
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(RangeIPLookupMPath)
EXPORT_ELEMENT(RangeIPLookupMPathMT)
