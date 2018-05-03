// -*- c-basic-offset: 4 -*-
#ifndef CLICK_RADIXIPLOOKUPMPATH_HH
#define CLICK_RADIXIPLOOKUPMPATH_HH
#include <click/glue.hh>
#include <click/element.hh>
#include "iproutetablempath.hh"
CLICK_DECLS

/*
=c

RadixIPLookupMPath(MODE, ADDR1/MASK1 [GW11] OUT11 [[GW12] OUT12]..., ADDR2/MASK2 [GW21] OUT21 [[GW22] OUT22]..., ...)

=s iproute

IP lookup using a radix trie

=d

Performs IP lookup using a radix trie.  The first level of the trie has 256
buckets; each succeeding level has 16.  The maximum number of levels that will
be traversed is thus 7.

Expects a destination IP address annotation with each packet. Looks up that
address in its routing table, using longest-prefix-match, sets the destination
annotation to the corresponding GW (if specified), and emits the packet on the
indicated OUTput port.

Each argument is a route, specifying a destination and mask, an optional
gateway IP address, and an output port.

Uses the IPRouteTable interface; see IPRouteTable for description.

=h table read-only

Outputs a human-readable version of the current routing table.

=h lookup read-only

Reports the OUTput port and GW corresponding to an address.

=h add write-only

Adds a route to the table. Format should be `C<ADDR/MASK [GW] OUT>'. Should
fail if a route for C<ADDR/MASK> already exists, but currently does not.

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

=n

See IPRouteTable for a performance comparison of the various IP routing
elements.

=a IPRouteTable, DirectIPLookup, RangeIPLookup, StaticIPLookup,
LinearIPLookup, SortedIPLookup, LinuxIPLookup
*/


class RadixIPLookupMPath : public IPRouteTableMPath { public:

    RadixIPLookupMPath() CLICK_COLD;
    ~RadixIPLookupMPath() CLICK_COLD;

    const char *class_name() const              { return "RadixIPLookupMPath"; }
    const char *port_count() const              { return "1/-"; }
    const char *processing() const              { return PUSH; }


    void cleanup(CleanupStage) CLICK_COLD;
    void add_handlers() CLICK_COLD;

    int add_route(const IPRouteMPath&, bool, IPRouteMPath*, ErrorHandler *);
    int remove_route(const IPRouteMPath&, IPRouteMPath*, ErrorHandler *);
    int lookup_route(IPAddress, IPAddress&, uint32_t) const;
    int find_lookup_key(const Vector<GWPort>&);
    String dump_routes();

  private:

    static inline int32_t combine_key(int32_t key, int32_t lookup_key) {
        assert(lookup_key <= 0xff);
        assert(key <= 0x00ffffff);
        return ((lookup_key) << 24 | key);
    }

    static inline int32_t get_key(int32_t comb) {
        return (comb & 0x00ffffff);
    }

    static inline int32_t get_lookup_key(int32_t comb) {
        return ((comb & 0xff000000) >> 24);
    }

    void flush_table();

    static int flush_handler(const String &, Element *, void *, ErrorHandler *);

    class Radix;

    // Simple routing table
    Vector<IPRouteMPath> _v;
    int _vfree;

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

    // Compressed routing table holding unique values of (gw, port).
    Vector<GWPortArr> _lookup;

    int _default_key;
    Radix *_radix;

};


CLICK_ENDDECLS
#endif
