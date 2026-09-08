// -*- c-basic-offset: 4 -*-
#ifndef CLICK_DIRECTIPLOOKUPMPATH_HH
#define CLICK_DIRECTIPLOOKUPMPATH_HH
#include "iproutetablempath.hh"
CLICK_DECLS

/*
=c

DirectIPLookupMPath(MODE, ADDR1/MASK1 [GW11] OUT11 [[GW12] OUT12]..., ADDR2/MASK2 [GW21] OUT21 [[GW22] OUT22]..., ...)

=s iproute

IP routing lookup using direct-indexed tables with multipath support

=d

Expects a destination IP address annotation with each packet. Looks up that
address in its routing table, using longest-prefix-match, sets the destination
annotation to the corresponding GW (if specified), and emits the packet on the
indicated OUTput port.

Each argument is a route, specifying a destination and mask, one or more
optional gateway IP addresses with output ports.

DirectIPLookupMPath is optimized for lookup speed at the expense of extensive RAM
usage. Each longest-prefix lookup is accomplished in one to maximum two DRAM
accesses, regardless on the number of routing table entries.

DirectIPLookupMPath implements the I<DIR-24-8-BASIC> lookup scheme with
multipath support.

=h table read-only

Outputs a human-readable version of the current routing table.

=h lookup read-only, requires parameters

Reports the OUTput port and GW corresponding to an address.

=h add write-only

Adds a route to the table. Format should be `C<ADDR/MASK [GW] OUT>'.
Fails if a route for C<ADDR/MASK> already exists.

=h set write-only

Sets a route, whether or not a route for the same prefix already exists.

=h setm write-only

Sets a multipath route, whether or not a route for the same prefix already exists.

=h remove write-only

Removes a route from the table. Format should be `C<ADDR/MASK>'.

=h ctrl write-only

Adds or removes a group of routes. Write `C<add>/C<set ADDR/MASK [GW] OUT>' to
add a route, and `C<remove ADDR/MASK>' to remove a route. You can supply
multiple commands, one per line; all commands are executed as one atomic
operation.

=h flush write-only

Clears the entire routing table in a single atomic operation.

=n

See IPRouteTable for a performance comparison of the various IP routing
elements.

=a IPRouteTable, DirectIPLookup, RadixIPLookupMPath, RangeIPLookupMPath
*/

class RangeIPLookupMPath;

class DirectIPLookupMPath : public IPRouteTableMPath { public:

    DirectIPLookupMPath() CLICK_COLD;
    ~DirectIPLookupMPath() CLICK_COLD;

    const char *class_name() const	{ return "DirectIPLookupMPath"; }
    const char *port_count() const	{ return "1/-"; }
    const char *processing() const	{ return PUSH; }

    int configure(Vector<String> &conf, ErrorHandler *errh) CLICK_COLD;
    void cleanup(CleanupStage stage) CLICK_COLD;
    void add_handlers() CLICK_COLD;

    int add_route(const IPRouteMPath&, bool, IPRouteMPath*, ErrorHandler *);
    int remove_route(const IPRouteMPath&, IPRouteMPath*, ErrorHandler *);
    int lookup_route(IPAddress, IPAddress&, uint32_t) const;
    String dump_routes();

    static int flush_handler(const String &, Element *, void *, ErrorHandler *);

    enum {
	RT_SIZE_MAX = 256 * 1024,
	tbl_24_31_capacity_limit = 32768 * 256,
	vport_capacity_limit = 32768,
	PREF_HASHSIZE = 64 * 1024,
	DISCARD_LOOKUP_KEY = 0
    };

    struct CleartextEntry {
	int ll_next;
	int ll_prev;
	uint32_t prefix;
	uint16_t plen;
	int16_t lookup_key;
    };

    struct GWPortArr
    {
	typedef int size_type;

	size_type length;
	GWPort data[7];

	GWPort& operator[](size_type i) {
	    assert((unsigned) i < (unsigned) capacity());
	    return data[i];
	}

	const GWPort& operator[](size_type i) const {
	    assert((unsigned) i < (unsigned) capacity());
	    return data[i];
	}

	size_type size() const {
	    return length;
	}

	size_type capacity() const {
	    return 7;
	}

	void reserve(size_type) const {
	    return;
	}
    };

    struct Table {
	// Structures used for IP lookup
	uint16_t *_tbl_0_23;
	uint16_t *_tbl_24_31;

	// Compressed routing table holding unique (gw, port) sets
	Vector<GWPortArr> _lookup;

	// Structures used for lookup table maintenance (add/remove operations)
	CleartextEntry *_rtable;
	int *_rt_hashtbl;
	uint8_t *_tbl_0_23_plen;
	uint8_t *_tbl_24_31_plen;

	uint32_t _rtable_size;
	uint32_t _tbl_24_31_size;
	int _rt_empty_head;
	uint16_t _tbl_24_31_empty_head;

	uint32_t _rtable_capacity;
	uint32_t _tbl_24_31_capacity;

	Table()
	    : _tbl_0_23(0), _tbl_24_31(0), _rtable(0),
	      _rt_hashtbl(0), _tbl_0_23_plen(0), _tbl_24_31_plen(0) {
	}

	~Table() {
	    cleanup();
	}

	int initialize();
	void cleanup();

	static inline uint32_t prefix_hash(uint32_t, uint32_t);

	int find_entry(uint32_t, uint32_t) const;
	String dump() const;

	int find_lookup_key(const Vector<GWPort> &gwports);

	int add_route(const IPRouteMPath&, bool, IPRouteMPath*, ErrorHandler *);
	int remove_route(const IPRouteMPath&, IPRouteMPath*, ErrorHandler *);
	void flush();

    };

  protected:

    Table _t;

    friend class RangeIPLookupMPath;

};

CLICK_ENDDECLS
#endif
