// -*- c-basic-offset: 4 -*-
/*
 * directiplookupmpath.{cc,hh} -- lookup for output port and next-hop gateway
 * in one to max. two DRAM accesses with potential CPU cache / TLB misses,
 * with multipath support
 *
 * Based on directiplookup.{cc,hh} by Marko Zec
 * Multipath support adapted from radixiplookupmpath.{cc,hh} by Piotr Jurkiewicz
 *
 * Copyright (c) 2005 International Computer Science Institute
 * Copyright (c) 2005 University of Zagreb
 * Copyright (c) 2018 AGH University of Science and Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "directiplookupmpath.hh"
#include <click/ipaddress.hh>
#include <click/straccum.hh>
#include <click/router.hh>
#include <click/error.hh>
CLICK_DECLS


// DIRECTIPLOOKUPMPATH::TABLE

int
DirectIPLookupMPath::Table::initialize()
{
    assert(!_tbl_0_23 && !_tbl_24_31 && !_rtable && !_rt_hashtbl
	   && !_tbl_0_23_plen && !_tbl_24_31_plen);

    _tbl_24_31_capacity = 4096;
    _rtable_capacity = 2048;

    if ((_tbl_0_23 = (uint16_t *) CLICK_LALLOC((sizeof(uint16_t) + sizeof(uint8_t)) * (1 << 24)))
	&& (_tbl_24_31 = (uint16_t *) CLICK_LALLOC((sizeof(uint16_t) + sizeof(uint8_t)) * _tbl_24_31_capacity))
	&& (_rtable = (CleartextEntry *) CLICK_LALLOC(sizeof(CleartextEntry) * _rtable_capacity))
	&& (_rt_hashtbl = (int *) CLICK_LALLOC(sizeof(int) * PREF_HASHSIZE))) {
	_tbl_0_23_plen = (uint8_t *) (_tbl_0_23 + (1 << 24));
	_tbl_24_31_plen = (uint8_t *) (_tbl_24_31 + _tbl_24_31_capacity);
	return 0;
    } else
	return -ENOMEM;
}

void
DirectIPLookupMPath::Table::cleanup()
{
    CLICK_LFREE(_tbl_0_23, (sizeof(uint16_t) + sizeof(uint8_t)) * (1 << 24));
    CLICK_LFREE(_tbl_24_31, (sizeof(uint16_t) + sizeof(uint8_t)) * _tbl_24_31_capacity);
    CLICK_LFREE(_rtable, sizeof(CleartextEntry) * _rtable_capacity);
    CLICK_LFREE(_rt_hashtbl, sizeof(int) * PREF_HASHSIZE);
    _tbl_0_23 = _tbl_24_31 = 0;
    _rtable = 0;
    _tbl_0_23_plen = _tbl_24_31_plen = 0;
    _rt_hashtbl = 0;
}


inline uint32_t
DirectIPLookupMPath::Table::prefix_hash(uint32_t prefix, uint32_t len)
{
    uint32_t hash = prefix ^ (len << 5) ^ ((prefix >> (len >> 2)) - len);
    hash ^= (hash >> 23) ^ ((hash >> 15) * len) ^ ((prefix >> 17) * 53);
    hash -= (prefix >> 3) ^ ((hash >> len) * 7) ^ ((hash >> 11) * 103);
    hash =  (hash ^ (hash >> 17)) & (PREF_HASHSIZE - 1);

    return hash;
}

void
DirectIPLookupMPath::Table::flush()
{
    _lookup.clear();

    memset(_rt_hashtbl, -1, sizeof(int) * PREF_HASHSIZE);

    // _rtable[0] is the default route entry pointing to discard (lookup_key 0)
    _rt_hashtbl[prefix_hash(0, 0)] = 0;
    _rtable[0].ll_prev = -1;
    _rtable[0].ll_next = -1;
    _rtable[0].prefix = 0;
    _rtable[0].plen = 0;
    _rtable[0].lookup_key = DISCARD_LOOKUP_KEY;
    _rtable_size = 1;
    _rt_empty_head = -1;

    // Bzeroed lookup tables resolve 0.0.0.0/0 to lookup_key 0 (discard)
    memset(_tbl_0_23, 0, (sizeof(uint16_t) + sizeof(uint8_t)) * (1 << 24));

    _tbl_24_31_size = 0;
    _tbl_24_31_empty_head = 0x8000;
}

String
DirectIPLookupMPath::Table::dump() const
{
    StringAccum sa;
    for (uint32_t i = 0; i < PREF_HASHSIZE; i++)
	for (int rt_i = _rt_hashtbl[i]; rt_i >= 0; rt_i = _rtable[rt_i].ll_next) {
	    const CleartextEntry &rt = _rtable[rt_i];
	    int16_t lk = rt.lookup_key;
	    if (lk > 0 && lk <= _lookup.size()) {
		const GWPortArr &arr = _lookup[lk - 1];
		IPRouteMPath route;
		route.addr = IPAddress(htonl(rt.prefix));
		route.mask = IPAddress::make_prefix(rt.plen);
		for (int j = 0; j < arr.size(); j++)
		    route.gwports.push_back(arr[j]);
		route.unparse(sa, true) << '\n';
	    }
	}
    return sa.take_string();
}

int
DirectIPLookupMPath::Table::find_entry(uint32_t prefix, uint32_t plen) const
{
    int hash = prefix_hash(prefix, plen);
    for (int rt_i = _rt_hashtbl[hash]; rt_i >= 0; rt_i = _rtable[rt_i].ll_next)
	if (_rtable[rt_i].prefix == prefix && _rtable[rt_i].plen == plen)
	    return rt_i;
    return -1;
}

int
DirectIPLookupMPath::Table::find_lookup_key(const Vector<GWPort> &gwports)
{
    for (int i = 0; i < _lookup.size(); i++) {
	if (_lookup[i].size() != gwports.size())
	    continue;
	bool match = true;
	for (int j = 0; j < _lookup[i].size(); j++)
	    if (_lookup[i][j].gw != gwports[j].gw || _lookup[i][j].port != gwports[j].port) {
		match = false;
		break;
	    }
	if (match)
	    return (i + 1);
    }
    return 0;
}

int
DirectIPLookupMPath::Table::add_route(const IPRouteMPath& route, bool allow_replace, IPRouteMPath* old_route, ErrorHandler *errh)
{
    uint32_t prefix = ntohl(route.addr.addr());
    uint32_t plen = route.prefix_len();

    // Find or allocate lookup_key for the gwports set
    int lookup_key = find_lookup_key(route.gwports);
    if (!lookup_key) {
	GWPortArr collection;
	collection.length = 0;
	collection.reserve(route.gwports.size());
	for (int i = 0; i < route.gwports.size() && i < collection.capacity(); i++) {
	    collection[i] = route.gwports[i];
	    collection.length = i + 1;
	}
	_lookup.push_back(collection);
	lookup_key = _lookup.size();
    }

    int rt_i = find_entry(prefix, plen);
    if (rt_i >= 0) {
	// Attempt to replace an existing route.
	if ((rt_i != 0 || (rt_i == 0 && _rtable[0].lookup_key != DISCARD_LOOKUP_KEY)) &&
	    old_route) {
	    old_route->addr = IPAddress(htonl(_rtable[rt_i].prefix));
	    old_route->mask = IPAddress::make_prefix(_rtable[rt_i].plen);
	    old_route->gwports.clear();
	    int16_t oldk = _rtable[rt_i].lookup_key;
	    if (oldk > 0 && oldk <= _lookup.size()) {
		const GWPortArr &arr = _lookup[oldk - 1];
		for (int j = 0; j < arr.size(); j++)
		    old_route->gwports.push_back(arr[j]);
	    }
	}
	if (rt_i == 0) {
	    if (_rtable[0].lookup_key != DISCARD_LOOKUP_KEY && !allow_replace)
		return -EEXIST;
	    _rtable[0].lookup_key = lookup_key;
	    // Update tbl_0_23 for default route
	    // The default route spans all of tbl_0_23 with plen=0.
	    // We need to update entries that currently have plen==0.
	    for (uint32_t i = 0; i < (1U << 24); i++) {
		if (_tbl_0_23[i] & 0x8000) {
		    int sec_i = (_tbl_0_23[i] & 0x7fff) << 8;
		    for (int j = sec_i; j < sec_i + 256; j++) {
			if (_tbl_24_31_plen[j] == 0) {
			    _tbl_24_31[j] = lookup_key;
			}
		    }
		} else {
		    if (_tbl_0_23_plen[i] == 0) {
			_tbl_0_23[i] = lookup_key;
		    }
		}
	    }
	    return 0;
	}
	if (!allow_replace)
	    return -EEXIST;

    } else {
	// Attempt to allocate a new _rtable[] entry.
	if (_rt_empty_head < 0 && _rtable_size == _rtable_capacity) {
	    CleartextEntry *new_rtable = (CleartextEntry *) CLICK_LALLOC(sizeof(CleartextEntry) * _rtable_capacity * 2);
	    if (!new_rtable)
		return -ENOMEM;
	    memcpy(new_rtable, _rtable, sizeof(CleartextEntry) * _rtable_capacity);
	    CLICK_LFREE(_rtable, sizeof(CleartextEntry) * _rtable_capacity);
	    _rtable = new_rtable;
	    _rtable_capacity *= 2;
	}
	if (_rt_empty_head < 0) {
	    _rtable[_rtable_size].ll_next = _rt_empty_head;
	    _rt_empty_head = _rtable_size;
	    ++_rtable_size;
	}
	rt_i = _rt_empty_head;
    }

    // find overflow table space
    int start = prefix >> 8;
    int end = start + (plen < 24 ? 1 << (24 - plen) : 1);
    if (plen > 24 && !(_tbl_0_23[start] & 0x8000)
	&& (_tbl_24_31_empty_head & 0x8000) != 0) {
	if (_tbl_24_31_size == _tbl_24_31_capacity
	    && _tbl_24_31_capacity >= tbl_24_31_capacity_limit)
	    return -ENOMEM;
	if (_tbl_24_31_size == _tbl_24_31_capacity) {
	    uint16_t *new_tbl = (uint16_t *) CLICK_LALLOC((sizeof(uint16_t) + sizeof(uint8_t)) * 2 * _tbl_24_31_capacity);
	    if (!new_tbl)
		return -ENOMEM;
	    memcpy(new_tbl, _tbl_24_31, sizeof(uint16_t) * _tbl_24_31_capacity);
	    memcpy(new_tbl + _tbl_24_31_capacity, _tbl_24_31_plen, sizeof(uint8_t) * _tbl_24_31_capacity);
	    CLICK_LFREE(_tbl_24_31, (sizeof(uint16_t) + sizeof(uint8_t)) * _tbl_24_31_capacity);
	    _tbl_24_31 = new_tbl;
	    _tbl_24_31_plen = (uint8_t *) (new_tbl + 2 * _tbl_24_31_capacity);
	    _tbl_24_31_capacity *= 2;
	}
	_tbl_24_31_empty_head = _tbl_24_31_size >> 8;
	_tbl_24_31[_tbl_24_31_empty_head << 8] = 0x8000;
	_tbl_24_31_size += 256;
    }

    // At this point we have successfully allocated all memory.
    if (rt_i == _rt_empty_head) {
	_rt_empty_head = _rtable[rt_i].ll_next;

	_rtable[rt_i].prefix = prefix;
	_rtable[rt_i].plen = plen;

	// Insert the new entry in our hashtable
	uint32_t hash = prefix_hash(prefix, plen);
	_rtable[rt_i].ll_prev = -1;
	_rtable[rt_i].ll_next = _rt_hashtbl[hash];
	if (_rt_hashtbl[hash] >= 0)
	    _rtable[_rt_hashtbl[hash]].ll_prev = rt_i;
	_rt_hashtbl[hash] = rt_i;
    }

    _rtable[rt_i].lookup_key = lookup_key;

    for (int i = start; i < end; i++) {
	if (_tbl_0_23[i] & 0x8000) {
	    // Entries with plen > 24 already there in _tbl_24_31[]!
	    int sec_i = (_tbl_0_23[i] & 0x7fff) << 8, sec_start, sec_end;
	    if (plen > 24) {
		sec_start = prefix & 0xFF;
		sec_end = sec_start + (1 << (32 - plen));
	    } else {
		sec_start = 0;
		sec_end = 256;
	    }
	    for (int j = sec_i + sec_start; j < sec_i + sec_end; j++) {
		if (plen > _tbl_24_31_plen[j]) {
		    _tbl_24_31[j] = lookup_key;
		    _tbl_24_31_plen[j] = plen;
		} else if (plen < _tbl_24_31_plen[j]) {
		    if (_tbl_24_31_plen[j] > 24) {
			j |= 0x000000ff >> (_tbl_24_31_plen[j] - 24);
		    } else {
			i |= 0x00ffffff >> _tbl_24_31_plen[j];
			break;
		    }
		} else if (allow_replace) {
		    _tbl_24_31[j] = lookup_key;
		} else {
		    return errh->error("BUG: _tbl_24_31[%08X] collision", j);
		}
	    }
	} else {
	    if (plen > _tbl_0_23_plen[i]) {
		if (plen > 24) {
		    assert(!(_tbl_24_31_empty_head & 0x8000));
		    int sec_i = _tbl_24_31_empty_head << 8;
		    _tbl_24_31_empty_head = _tbl_24_31[sec_i];
		    int sec_start = prefix & 0xFF;
		    int sec_end = sec_start + (1 << (32 - plen));
		    for (int j = 0; j < 256; j++) {
			if (j >= sec_start && j < sec_end) {
			    _tbl_24_31[sec_i + j] = lookup_key;
			    _tbl_24_31_plen[sec_i + j] = plen;
			} else {
			    _tbl_24_31[sec_i + j] = _tbl_0_23[i];
			    _tbl_24_31_plen[sec_i + j] = _tbl_0_23_plen[i];
			}
		    }
		    _tbl_0_23[i] = (sec_i >> 8) | 0x8000;
		} else {
		    _tbl_0_23[i] = lookup_key;
		    _tbl_0_23_plen[i] = plen;
		}
	    } else if (plen < _tbl_0_23_plen[i]) {
		i |= 0x00ffffff >> _tbl_0_23_plen[i];
	    } else if (allow_replace) {
		_tbl_0_23[i] = lookup_key;
	    } else {
		return errh->error("BUG: _tbl_0_23[%08X] collision", i);
	    }
	}
    }

    return 0;
}

int
DirectIPLookupMPath::Table::remove_route(const IPRouteMPath& route, IPRouteMPath* old_route, ErrorHandler *errh)
{
    uint32_t prefix = ntohl(route.addr.addr());
    uint32_t plen = route.prefix_len();
    int rt_i = find_entry(prefix, plen);

    if (rt_i < 0 || (rt_i == 0 && _rtable[0].lookup_key == DISCARD_LOOKUP_KEY))
	return -ENOENT;

    // Build the found route for matching
    IPRouteMPath found_route;
    found_route.addr = IPAddress(htonl(_rtable[rt_i].prefix));
    found_route.mask = IPAddress::make_prefix(_rtable[rt_i].plen);
    int16_t lk = _rtable[rt_i].lookup_key;
    if (lk > 0 && lk <= _lookup.size()) {
	const GWPortArr &arr = _lookup[lk - 1];
	for (int j = 0; j < arr.size(); j++)
	    found_route.gwports.push_back(arr[j]);
    }
    if (!route.match(found_route))
	return -ENOENT;

    if (old_route)
	*old_route = found_route;

    if (plen == 0) {
	// Default route: point to discard
	_rtable[0].lookup_key = DISCARD_LOOKUP_KEY;
	// Update tbl_0_23 entries with plen==0
	for (uint32_t i = 0; i < (1U << 24); i++) {
	    if (_tbl_0_23[i] & 0x8000) {
		int sec_i = (_tbl_0_23[i] & 0x7fff) << 8;
		for (int j = sec_i; j < sec_i + 256; j++) {
		    if (_tbl_24_31_plen[j] == 0) {
			_tbl_24_31[j] = DISCARD_LOOKUP_KEY;
		    }
		}
	    } else {
		if (_tbl_0_23_plen[i] == 0) {
		    _tbl_0_23[i] = DISCARD_LOOKUP_KEY;
		}
	    }
	}
    } else {
	uint32_t start, end, i, j, sec_i, sec_start, sec_end;
	int newent = -1;
	int newmask, prev, next;

	// Prune our entry from the prefix/len hashtable
	prev = _rtable[rt_i].ll_prev;
	next = _rtable[rt_i].ll_next;
	if (prev >= 0)
	    _rtable[prev].ll_next = next;
	else
	    _rt_hashtbl[prefix_hash(prefix, plen)] = next;
	if (next >= 0)
	    _rtable[next].ll_prev = prev;

	// Add entry to the list of empty _rtable entries
	_rtable[rt_i].ll_next = _rt_empty_head;
	_rt_empty_head = rt_i;

	// Find an entry covering current prefix/len with the longest prefix.
	for (newmask = plen - 1 ; newmask >= 0 ; newmask--)
	    if (newmask == 0) {
		newent = 0;
		break;
	    } else {
		newent = find_entry(prefix & (0xffffffff << (32 - newmask)),
				    newmask);
		if (newent > 0)
		    break;
	    }

	int16_t new_lookup_key = _rtable[newent].lookup_key;

	// Replace prefix/plen with newent/mask in lookup tables
	start = prefix >> 8;
	if (plen >= 24)
	    end = start + 1;
	else
	    end = start + (1 << (24 - plen));
	for (i = start; i < end; i++) {
	    if (_tbl_0_23[i] & 0x8000) {
		sec_i = (_tbl_0_23[i] & 0x7fff) << 8;
		if (plen > 24) {
		    sec_start = prefix & 0xFF;
		    sec_end = sec_start + (1 << (32 - plen));
		} else {
		    sec_start = 0;
		    sec_end = 256;
		}
		for (j = sec_i + sec_start; j < sec_i + sec_end; j++) {
		    if (plen == _tbl_24_31_plen[j]) {
			_tbl_24_31[j] = new_lookup_key;
			_tbl_24_31_plen[j] = newmask;
		    } else if (plen < _tbl_24_31_plen[j]) {
			if (_tbl_24_31_plen[j] > 24) {
			    j |= 0x000000ff >> (_tbl_24_31_plen[j] - 24);
			} else {
			    i |= 0x00ffffff >> _tbl_24_31_plen[j];
			    break;
			}
		    } else {
			return
			  errh->error("BUG: _tbl_24_31[%08X] inconsistency", j);
		    }
		}
		// Check if we can prune the entire secondary table range
		for (j = sec_i ; j < sec_i + 255; j++)
		    if (_tbl_24_31_plen[j] != _tbl_24_31_plen[j+1])
			break;
		if (j == sec_i + 255) {
		    _tbl_0_23[i] = _tbl_24_31[sec_i];
		    _tbl_0_23_plen[i] = _tbl_24_31_plen[sec_i];
		    _tbl_24_31[sec_i] = _tbl_24_31_empty_head;
		    _tbl_24_31_empty_head = sec_i >> 8;
		}
	    } else {
		if (plen == _tbl_0_23_plen[i]) {
		    _tbl_0_23[i] = new_lookup_key;
		    _tbl_0_23_plen[i] = newmask;
		} else if (plen < _tbl_0_23_plen[i]) {
		    i |= 0x00ffffff >> _tbl_0_23_plen[i];
		}
	    }
	}
    }
    return 0;
}


// DIRECTIPLOOKUPMPATH

DirectIPLookupMPath::DirectIPLookupMPath()
{
}

DirectIPLookupMPath::~DirectIPLookupMPath()
{
}

int
DirectIPLookupMPath::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int r;
    if ((r = _t.initialize()) < 0)
	return r;
    _t.flush();
    return IPRouteTableMPath::configure(conf, errh);
}

void
DirectIPLookupMPath::cleanup(CleanupStage)
{
    _t.cleanup();
}

int
DirectIPLookupMPath::lookup_route(IPAddress dest, IPAddress &gw, uint32_t hash) const
{
    uint32_t ip_addr = ntohl(dest.addr());
    uint16_t lookup_key = _t._tbl_0_23[ip_addr >> 8];

    if (lookup_key & 0x8000)
	lookup_key = _t._tbl_24_31[((lookup_key & 0x7fff) << 8) | (ip_addr & 0xff)];

    if (lookup_key > 0 && lookup_key <= (uint16_t)_t._lookup.size()) {
	const GWPortArr &arr = _t._lookup[lookup_key - 1];
	int n = hash % arr.size();
	gw = arr[n].gw;
	return arr[n].port;
    } else {
	gw = 0;
	return -1;
    }
}

int
DirectIPLookupMPath::add_route(const IPRouteMPath& route, bool allow_replace, IPRouteMPath* old_route, ErrorHandler *errh)
{
    return _t.add_route(route, allow_replace, old_route, errh);
}

int
DirectIPLookupMPath::remove_route(const IPRouteMPath& route, IPRouteMPath* old_route, ErrorHandler *errh)
{
    return _t.remove_route(route, old_route, errh);
}

int
DirectIPLookupMPath::flush_handler(const String &, Element *e, void *,
				ErrorHandler *)
{
    DirectIPLookupMPath *t = static_cast<DirectIPLookupMPath *>(e);
    t->write_begin();
    t->_t.flush();
    t->write_end();
    return 0;
}

String
DirectIPLookupMPath::dump_routes()
{
    return _t.dump();
}

void
DirectIPLookupMPath::add_handlers()
{
    IPRouteTableMPath::add_handlers();
    add_write_handler("flush", flush_handler, 0, Handler::BUTTON);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(IPRouteTableMPath userlevel|bsdmodule)
EXPORT_ELEMENT(DirectIPLookupMPath)
