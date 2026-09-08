// -*- c-basic-offset: 4 -*-
/*
 * rangeiplookupmpath.{cc,hh} -- binary search for output port and next-hop
 * gateway in a very compact sorted array, aiming for high CPU cache hit
 * ratios, with multipath support
 *
 * Based on rangeiplookup.{cc,hh} by Marko Zec
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
#include "rangeiplookupmpath.hh"
#include <click/ipaddress.hh>
#include <click/straccum.hh>
#include <click/router.hh>
#include <click/error.hh>
CLICK_DECLS

RangeIPLookupMPath::RangeIPLookupMPath()
    : _range_base((uint32_t *) CLICK_LALLOC((1 << KICKSTART_BITS) * sizeof(uint32_t))),
      _range_len((uint32_t *) CLICK_LALLOC((1 << KICKSTART_BITS) * sizeof(uint32_t))),
      _range_t((uint32_t *) CLICK_LALLOC(RANGES_MAX * sizeof(uint32_t))),
      _active(false)
{
}

RangeIPLookupMPath::~RangeIPLookupMPath()
{
    CLICK_LFREE(_range_base, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    CLICK_LFREE(_range_len, (1 << KICKSTART_BITS) * sizeof(uint32_t));
    CLICK_LFREE(_range_t, RANGES_MAX * sizeof(uint32_t));
}

int
RangeIPLookupMPath::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int r;
    if ((r = _helper.initialize()) < 0)
	return r;
    flush_table();
    return IPRouteTableMPath::configure(conf, errh);
}

int
RangeIPLookupMPath::initialize(ErrorHandler *)
{
    expand();
    _active = true;
    return 0;
}

void
RangeIPLookupMPath::cleanup(CleanupStage)
{
    _helper.cleanup();
}

int
RangeIPLookupMPath::lookup_route(IPAddress dest, IPAddress &gw, uint32_t hash) const
{
    uint32_t ip_addr = ntohl(dest.addr());
    uint32_t lowerbound, upperbound, middle;
    uint32_t i = ip_addr >> RANGE_SHIFT;
    uint16_t lookup_key;

    lowerbound = _range_base[i];
    upperbound = lowerbound + _range_len[i];
    i = ip_addr & RANGE_MASK;

    // Binary search for a matching range
    while (upperbound > lowerbound) {
	middle = (upperbound + lowerbound) >> 1;
	if (i < (_range_t[middle] & RANGE_MASK))
	    upperbound = middle;
	else if (i < (_range_t[middle + 1] & RANGE_MASK)) {
	    lowerbound = middle;
	    break;
	} else
	    lowerbound = middle + 1;
    }

    // MS bits of the found range contain the lookup_key
    lookup_key = _range_t[lowerbound] >> RANGE_SHIFT;
    if (lookup_key > 0 && lookup_key <= (uint16_t)_helper._lookup.size()) {
	const DirectIPLookupMPath::GWPortArr &arr = _helper._lookup[lookup_key - 1];
	int n = hash % arr.size();
	gw = arr[n].gw;
	return arr[n].port;
    } else {
	gw = 0;
	return -1;
    }
}

void
RangeIPLookupMPath::add_handlers()
{
    IPRouteTableMPath::add_handlers();
    add_write_handler("flush", flush_handler, 0, Handler::BUTTON);
}

int
RangeIPLookupMPath::add_route(const IPRouteMPath& route, bool allow_replace, IPRouteMPath* old_route, ErrorHandler *errh)
{
    int error = _helper.add_route(route, allow_replace, old_route, errh);
    if (error == 0 && _active)
	expand();
    return error;
}

int
RangeIPLookupMPath::remove_route(const IPRouteMPath& route, IPRouteMPath* old_route, ErrorHandler *errh)
{
    int error = _helper.remove_route(route, old_route, errh);
    if (error == 0 && _active)
	expand();
    return error;
}

/*
 * On each routing table update, we distill the address range based lookup
 * table from the structures provided by the DirectIPLookupMPath class.
 */
void
RangeIPLookupMPath::expand()
{
    uint32_t range_t_index = 0;
    uint32_t tbl_0_23_index = 0;
    uint32_t range_base;
    uint32_t range_len;

    for (range_base = 0; range_base < (1 << KICKSTART_BITS); range_base++) {
	uint16_t lookup_key, lookup_key1;

	lookup_key = 0xffff;
	_range_base[range_base] = range_t_index;

	for (range_len = 0;
	  tbl_0_23_index < ((range_base + 1) << (24 - KICKSTART_BITS));
	  tbl_0_23_index++) {
	    if (_helper._tbl_0_23[tbl_0_23_index] & 0x8000) {
		uint32_t tbl_24_31_index, j;
		tbl_24_31_index =
			(_helper._tbl_0_23[tbl_0_23_index] & 0x7fff) << 8;
		for (j = 0; j < 256; j++) {
		    lookup_key1 = _helper._tbl_24_31[tbl_24_31_index + j];
		    if (lookup_key != lookup_key1) {
			lookup_key = lookup_key1;
			_range_t[range_t_index] =
					lookup_key << (32 - KICKSTART_BITS) |
					(((tbl_0_23_index << 8) + j) &
					(0xffffffff >> KICKSTART_BITS));
			range_t_index++;
			range_len++;
		    }
		}
	    } else {
		lookup_key1 = _helper._tbl_0_23[tbl_0_23_index];
		if (lookup_key != lookup_key1) {
		    lookup_key = lookup_key1;
		    _range_t[range_t_index] =
					lookup_key << (32 - KICKSTART_BITS) |
					((tbl_0_23_index << 8) &
					(0xffffffff >> KICKSTART_BITS));
		    range_t_index++;
		    range_len++;
		}
	    }
	}
	_range_len[range_base] = range_len - 1;
    }
}

void
RangeIPLookupMPath::flush_table()
{
    _helper.flush();
    memset(_range_base, 0, (1 << KICKSTART_BITS) * sizeof(*_range_base));
    memset(_range_len, 0, (1 << KICKSTART_BITS) * sizeof(*_range_len));
    memset(_range_t, 0, RANGES_MAX * sizeof(*_range_t));
}

int
RangeIPLookupMPath::flush_handler(const String &, Element *e, void *,
                                ErrorHandler *)
{
    RangeIPLookupMPath *t = static_cast<RangeIPLookupMPath *>(e);
    t->write_begin();
    t->flush_table();
    t->write_end();
    return 0;
}

String
RangeIPLookupMPath::dump_routes()
{
    return _helper.dump();
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(DirectIPLookupMPath)
EXPORT_ELEMENT(RangeIPLookupMPath)
