// -*- c-basic-offset: 4 -*-
/*
 * radixiplookupmpathmt.{cc,hh} -- looks up next-hop address in radix table
 * Eddie Kohler (earlier versions: Thomer M. Gil, Benjie Chen)
 *
 * Multipath support
 * by Piotr Jurkiewicz
 *
 * Copyright (c) 1999-2001 Massachusetts Institute of Technology
 * Copyright (c) 2002 International Computer Science Institute
 * Copyright (c) 2005 Regents of the University of California
 * Copyright (c) 2018 AGH University of Science and Technology
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, subject to the conditions listed in the Click LICENSE
 * file. These conditions include: you must preserve this copyright
 * notice, and you cannot mention the copyright holders in advertising
 * related to the Software without their permission.  The Software is
 * provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This notice is a
 * summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/ipaddress.hh>
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include "radixiplookupmpathmt.hh"
CLICK_DECLS

class RadixIPLookupMPathMT::Radix { public:

    static Radix *make_radix(int level);
    static void free_radix(Radix *r, int level);

    int change(uint32_t addr, uint32_t mask, int key, bool set, int level);


    static inline int lookup(const Radix *r, int cur, uint32_t addr, int level) {
        while (r) {
            int i1 = (addr >> _bitshift[level]) & (_nbuckets[level] - 1);
            const Child &c = r->_children[i1];
            int key = c.key;
            if (key)
                cur = key;
            r = __atomic_load_n(&c.child, __ATOMIC_ACQUIRE);
            level++;
        }
        return cur;
    }

private:


    atomic_uint32_t _default_key;

    struct Child {
        atomic_uint32_t key;
        Radix *child;
    } _children[0];

    Radix()                     { }
    ~Radix()                    { }

    atomic_uint32_t & key_for(int i, int level) {
        int n = _nbuckets[level];
        assert(i >= 2 && i < n * 2);
        if (i >= n)
            return _children[i - n].key;
        else {
            atomic_uint32_t *x = reinterpret_cast<atomic_uint32_t *>(_children + n);
            return x[i - 2];
        }
    }

    static const int _bitshift [5];
    static const int _nbuckets [5];

    friend class RadixIPLookupMPathMT;

};

const int RadixIPLookupMPathMT::Radix::_bitshift [5] = {16, 12, 8, 4, 0};

// The number of buckets each node contains
// 2^16 at the first level and 2^4 at 4 subsequent levels.
// (2^16).(2^4)^4 = 2^32.
const int RadixIPLookupMPathMT::Radix::_nbuckets [5] = {65536, 16, 16, 16, 16};

int
RadixIPLookupMPathMT::find_lookup_key(const Vector<GWPort> &gwports) {
    int lookup_size = _lookup_size;
    for (int i=0; i < lookup_size; i++) {
        if (_lookup[i].size() != gwports.size())
            continue;
        bool match = true;
        for (int j=0; j < _lookup[i].size(); j++)
            if (_lookup[i][j].gw != gwports[j].gw || _lookup[i][j].port != gwports[j].port) {
                match = false;
                break;
            }
        if (match)
            return (i + 1);
    }
    return 0;
}

RadixIPLookupMPathMT::Radix*
RadixIPLookupMPathMT::Radix::make_radix(int level)
{
    int n = _nbuckets[level];
    if (Radix* r = (Radix*) new unsigned char[sizeof(Radix) + n * sizeof(Child) + (n - 2) * sizeof(int)]) {
        r->_default_key = 0;
        memset(r->_children, 0, n * sizeof(Child) + (n - 2) * sizeof(int));
        return r;
    } else
        return 0;
}

void
RadixIPLookupMPathMT::Radix::free_radix(Radix* r, int level)
{
    int n = _nbuckets[level];
    for (int i = 0; i < n; i++)
        if (Radix *child = __atomic_load_n(&r->_children[i].child, __ATOMIC_RELAXED))
            free_radix(child, level+1);
    delete[] (unsigned char *)r;
}

int
RadixIPLookupMPathMT::Radix::change(uint32_t addr, uint32_t mask, int key, bool set, int level)
{
    int shift = _bitshift[level];
    int n = _nbuckets[level];
    int i1 = (addr >> shift) & (n - 1);

    // check if change only affects children
    if (mask & ((1U << shift) - 1)) {
        Radix *child = __atomic_load_n(&_children[i1].child, __ATOMIC_RELAXED);
        if (!child && (child = make_radix(level + 1)))
            __atomic_store_n(&_children[i1].child, child, __ATOMIC_RELEASE);
        if (child)
            return child->change(addr, mask, key, set, level+1);
        else
            return 0;
    }

    // find current key
    i1 = n + i1;
    int nmasked = n - ((mask >> shift) & (n - 1));
    for (int x = nmasked; x > 1; x /= 2)
        i1 /= 2;
    int replace_key = key_for(i1, level), prev_key = replace_key;
    if (prev_key && i1 > 3 && key_for(i1 / 2, level) == prev_key)
        prev_key = 0;

    // replace previous key with current key, if appropriate
    if (!key && i1 > 3)
        key = (key_for(i1 / 2, level));

    if (prev_key != key && (!prev_key || set)) {
        for (nmasked = 1; i1 < n * 2; i1 *= 2, nmasked *= 2)
            for (int x = i1; x < i1 + nmasked; ++x) {
                if (key_for(x, level) == replace_key) {
                    key_for(x, level) = key;
                }
            }

    }
    return prev_key;
}


RadixIPLookupMPathMT::RadixIPLookupMPathMT()
    : _vfree(-1), _read_root(0)
{
    _lookup_size = 0;
    _radix.initialize(Radix::make_radix(0));
}

RadixIPLookupMPathMT::~RadixIPLookupMPathMT()
{
}

void
RadixIPLookupMPathMT::read_begin(int &token) const
{
    _read_root = _radix.read_begin(token);
}

void
RadixIPLookupMPathMT::read_end(int &token) const
{
    _read_root = 0;
    _radix.read_end(token);
}

void
RadixIPLookupMPathMT::write_begin()
{
    _writer_lock.acquire();
}

void
RadixIPLookupMPathMT::write_end()
{
    _writer_lock.release();
}

void
RadixIPLookupMPathMT::cleanup(CleanupStage)
{
    write_begin();
    Radix *old = _radix.read();
    int token;
    Radix *&next = _radix.write_begin(token);
    next = 0;
    _radix.write_commit(token);
    Radix *&current = _radix.write_begin(token);
    current = 0;
    Radix::free_radix(old, 0);
    _radix.write_commit(token);
    _v.clear();
    write_end();
}

void
RadixIPLookupMPathMT::add_handlers()
{
    IPRouteTableMPath::add_handlers();
    add_write_handler("flush", flush_handler, 0, Handler::BUTTON);
}

String
RadixIPLookupMPathMT::dump_routes()
{
    StringAccum sa;
    for (int j = _vfree; j >= 0; j = _v[j].extra)
        _v[j].kill();
    for (int i = 0; i < _v.size(); i++)
        if (_v[i].real())
            _v[i].unparse(sa, true) << '\n';
    return sa.take_string();
}


int
RadixIPLookupMPathMT::add_route(const IPRouteMPath &route, bool set, IPRouteMPath *old_route, ErrorHandler *errh)
{
    if (route.gwports.size() > 7)
        return errh->error("multipath routes support at most 7 next hops");
    int found = (_vfree < 0 ? _v.size() : _vfree), last_key;
    int lookup_key = find_lookup_key(route.gwports);
    if (!lookup_key) {
        int lookup_size = _lookup_size;
        if (lookup_size == 255)
            return errh->error("no space for another multipath next-hop set");
        GWPortArr &collection = _lookup[lookup_size];
        collection.length = 0;
        collection.reserve(route.gwports.size());
        for (int i = 0; i < route.gwports.size() && i < collection.capacity(); i++) {
            collection[i] = route.gwports[i];
            collection.length = i + 1;
        }
        click_write_fence();
        _lookup_size = lookup_size + 1;
        lookup_key = lookup_size + 1;
    }

    Radix *root = _radix.read();
    if (route.mask) {
        uint32_t addr = ntohl(route.addr.addr());
        uint32_t mask = ntohl(route.mask.addr());
        int level = 0;
        last_key = root->change(addr, mask, combine_key(found + 1, lookup_key), set, level);
        // The key returned by change is the combined key, we need only the _v key.
        last_key = get_key(last_key);
    } else {
        last_key = get_key(root->_default_key);
        if (!last_key || set)
            root->_default_key = combine_key(found + 1, lookup_key);
    }

    if (last_key && old_route)
        *old_route = _v[last_key - 1];
    if (last_key && !set)
        return -EEXIST;

    if (found == _v.size())
        _v.push_back(route);
    else {
        _vfree = _v[found].extra;
        _v[found] = route;
    }
    _v[found].extra = -1;

    if (last_key) {
        _v[last_key - 1].extra = _vfree;
        _vfree = last_key - 1;
    }

    return 0;
}

int
RadixIPLookupMPathMT::remove_route(const IPRouteMPath& route, IPRouteMPath* old_route, ErrorHandler*)
{
    int last_key;
    Radix *root = _radix.read();
    if (route.mask) {
        uint32_t addr = ntohl(route.addr.addr());
        uint32_t mask = ntohl(route.mask.addr());
        int level = 0;
        // NB: this will never actually make changes
        last_key = get_key(root->change(addr, mask, 0, false, level));
    } else
        last_key = get_key(root->_default_key);

    if (last_key && old_route)
        *old_route = _v[last_key - 1];
    if (!last_key || !route.match(_v[last_key - 1]))
        return -ENOENT;
    _v[last_key - 1].extra = _vfree;
    _vfree = last_key - 1;

    if (route.mask) {
        uint32_t addr = ntohl(route.addr.addr());
        uint32_t mask = ntohl(route.mask.addr());
        int level = 0;
        (void) root->change(addr, mask, 0, true, level);
    } else
        root->_default_key = 0;
    return 0;
}

int
RadixIPLookupMPathMT::lookup_route(IPAddress addr, IPAddress &gw, uint32_t hash) const
{
    int level = 0;
    Radix *root = *_read_root;
    int key = Radix::lookup(root, root->_default_key, ntohl(addr.addr()), level);
    int lookup_key = get_lookup_key(key);
    if (lookup_key) {
        int n = hash % _lookup[lookup_key - 1].size();
        gw = _lookup[lookup_key - 1][n].gw;
        return _lookup[lookup_key - 1][n].port;
    } else {
        gw = 0;
        return -1;
    }
}

int
RadixIPLookupMPathMT::flush_table()
{
    Radix *fresh = Radix::make_radix(0);
    if (!fresh)
        return -ENOMEM;
    Radix *old = _radix.read();
    int token;
    Radix *&next = _radix.write_begin(token);
    next = fresh;
    _radix.write_commit(token);
    Radix *&current = _radix.write_begin(token);
    current = fresh;
    Radix::free_radix(old, 0);
    _radix.write_commit(token);
    _v.clear();
    _vfree = -1;
    return 0;
}

int
RadixIPLookupMPathMT::flush_handler(const String &, Element *e, void *, ErrorHandler *errh)
{
    RadixIPLookupMPathMT *t = static_cast<RadixIPLookupMPathMT *>(e);
    t->write_begin();
    int result = t->flush_table();
    t->write_end();
    if (result < 0)
        return errh->error("no memory to flush routing table");
    return 0;
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(IPRouteTableMPath)
EXPORT_ELEMENT(RadixIPLookupMPathMT)
