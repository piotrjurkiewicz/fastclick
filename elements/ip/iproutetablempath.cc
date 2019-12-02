// -*- c-basic-offset: 4 -*-
/*
 * iproutetablempath.{cc,hh} -- looks up next-hop address in route table
 * Benjie Chen, Eddie Kohler
 *
 * Computational batching support
 * by Georgios Katsikas
 *
 * Multipath support
 * by Piotr Jurkiewicz
 *
 * Copyright (c) 2001 Massachusetts Institute of Technology
 * Copyright (c) 2002 International Computer Science Institute
 * Copyright (c) 2016 KTH Royal Institute of Technology
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
#include <click/args.hh>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include <click/router.hh>
#include "iproutetablempath.hh"
CLICK_DECLS

bool
cp_ip_route_mpath(String s, IPRouteMPath *r_store, bool remove_route, Element *context)
{
    IPRouteMPath r;
    if (!IPPrefixArg(true).parse(cp_shift_spacevec(s), r.addr, r.mask, context))
        return false;
    r.addr &= r.mask;

    String word = cp_shift_spacevec(s);

  next_pair:
    IPAddress gw;
    int port = -1;
    if (word == "-")
        /* null gateway; do nothing */;
    else if (IPAddressArg().parse(word, gw, context))
        /* do nothing */;
    else
        goto two_words;

    word = cp_shift_spacevec(s);

  two_words:
    if (IntArg().parse(word, port) || (!word && remove_route)) {
        if (port > -1)
            r.gwports.push_back({gw, port});
        if (word = cp_shift_spacevec(s)) {
            goto next_pair;
        } else { // nothing left
            *r_store = r;
            return true;
        }
    }

    return false;
}

StringAccum&
IPRouteMPath::unparse(StringAccum& sa, bool tabs) const
{
    int l = sa.length();
    char tab = (tabs ? '\t' : ' ');
    bool first = true;
    sa << addr.unparse_with_mask(mask) << tab;
    if (sa.length() < l + 17 && tabs)
        sa << '\t';
    if (!real())
        sa << "-1";
    else
        for (auto &gp: gwports) {
            if (!first) {
                sa << ' ';
            }
            if (gp.gw)
                sa << gp.gw << tab;
            else
                sa << '-' << tab;
            sa << gp.port;
            first = false;
        }
    return sa;
}

String
IPRouteMPath::unparse() const
{
    StringAccum sa;
    sa << *this;
    return sa.take_string();
}


void *
IPRouteTableMPath::cast(const char *name)
{
    if (strcmp(name, "IPRouteTableMPath") == 0)
        return (void *)this;
    else
        return Element::cast(name);
}

int
IPRouteTableMPath::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int r = 0, r1, eexist = 0;
    IPRouteMPath route;
    if (conf.empty()) {
        errh->error("MODE not specified");
        return -1;
    }
    String mode = conf[0];
    _mode = MODE_PACKET;
    if (mode == "single")
        _mode = MODE_SINGLE;
    else if (mode == "addr")
        _mode = MODE_ADDR;
    else if (mode == "port")
        _mode = MODE_PORT;
    else if (mode == "packet")
        _mode = MODE_PACKET;
    else
        errh->warning("MODE %s unknown, should be single, addr, port or packet", mode.c_str());

    _salt = click_random();

    for (int i = 1; i < conf.size(); i++) {
        if (!cp_ip_route_mpath(conf[i], &route, false, this)) {
            errh->error("argument %d should be %<ADDR/MASK [GATEWAY] OUTPUT [[GATEWAY] OUTPUT]...%>", i+1);
            r = -EINVAL;
        } else if ((r1 = add_route(route, false, 0, errh)) < 0) {
            if (r1 == -EEXIST)
                ++eexist;
            else
                r = r1;
        }
    }
    if (eexist)
        errh->warning("%d %s replaced by later versions", eexist, eexist > 1 ? "routes" : "route");
    return r;
}

int
IPRouteTableMPath::add_route(const IPRouteMPath&, bool, IPRouteMPath*, ErrorHandler *errh)
{
    // by default, cannot add routes
    return errh->error("cannot add routes to this routing table");
}

int
IPRouteTableMPath::remove_route(const IPRouteMPath&, IPRouteMPath*, ErrorHandler *errh)
{
    // by default, cannot remove routes
    return errh->error("cannot delete routes from this routing table");
}

int
IPRouteTableMPath::add_route(const IPRoute& route, bool set, IPRoute* old_route, ErrorHandler *errh)
{
    IPRouteMPath route_mpath, old_route_mpath;
    route_mpath.addr = route.addr;
    route_mpath.mask = route.mask;
    route_mpath.extra = route.extra;
    if (route.port > -1)
        route_mpath.gwports.push_back({route.gw, route.port});

    IPRouteMPath* old_route_mpath_ptr = NULL;
    if (old_route != NULL)
        old_route_mpath_ptr = &old_route_mpath;

    int result = add_route(route_mpath, set, old_route_mpath_ptr, errh);

    if (old_route != NULL) {
        old_route->addr = old_route_mpath.addr;
        old_route->mask = old_route_mpath.mask;
        old_route->extra = old_route_mpath.extra;
        if (old_route_mpath.gwports.size() > 0) {
            old_route->gw = old_route_mpath.gwports[0].gw;
            old_route->port = old_route_mpath.gwports[0].port;
        }
    }

    return result;
}

int
IPRouteTableMPath::remove_route(const IPRoute& route, IPRoute* old_route, ErrorHandler *errh)
{
    IPRouteMPath route_mpath, old_route_mpath;
    route_mpath.addr = route.addr;
    route_mpath.mask = route.mask;
    route_mpath.extra = route.extra;
    if (route.port > -1)
        route_mpath.gwports.push_back({route.gw, route.port});

    IPRouteMPath* old_route_mpath_ptr = NULL;
    if (old_route != NULL)
        old_route_mpath_ptr = &old_route_mpath;

    int result = remove_route(route_mpath, old_route_mpath_ptr, errh);

    if (old_route != NULL) {
        old_route->addr = old_route_mpath.addr;
        old_route->mask = old_route_mpath.mask;
        old_route->extra = old_route_mpath.extra;
        if (old_route_mpath.gwports.size() > 0) {
            old_route->gw = old_route_mpath.gwports[0].gw;
            old_route->port = old_route_mpath.gwports[0].port;
        }
    }

    return result;
}

int
IPRouteTableMPath::lookup_route(IPAddress, IPAddress&, uint32_t) const
{
    return -1;                  // by default, route lookups fail
}

int
IPRouteTableMPath::lookup_route(IPAddress addr, IPAddress& gw) const
{
    return lookup_route(addr, gw, 0);
}

String
IPRouteTableMPath::dump_routes()
{
    return String();
}

inline uint32_t
IPRouteTableMPath::calc_hash(Packet *p)
{
    if (_mode == MODE_SINGLE)
        return 0;
    else if (_mode == MODE_ADDR || _mode == MODE_PORT) {
        const click_ip *iph = p->ip_header();
        uint32_t a = (iph->ip_src.s_addr * 59) ^ iph->ip_dst.s_addr;
        a ^= _salt;
        if (_mode == MODE_PORT && IP_FIRSTFRAG(iph) && (iph->ip_p == IP_PROTO_TCP || iph->ip_p == IP_PROTO_UDP)) {
            a ^= *((const uint16_t *) (p->transport_header()));
            a ^= *((const uint16_t *) (p->transport_header() + 2)) << 16;
        }
        // Bob Jenkins http://burtleburtle.net/bob/hash/integer.html
        a = (a + 0x7ed55d16) + (a << 12);
        a = (a ^ 0xc761c23c) ^ (a >> 19);
        a = (a + 0x165667b1) + (a << 5);
        a = (a + 0xd3a2646c) ^ (a << 9);
        a = (a + 0xfd7046c5) + (a << 3);
        a = (a ^ 0xb55a4f09) ^ (a >> 16);
        return a;
    } else
        return click_random();
}

inline int
IPRouteTableMPath::process(Packet *p)
{
    IPAddress gw;
    int port = lookup_route(p->dst_ip_anno(), gw, calc_hash(p));
    if (port >= 0) {
        assert(port < noutputs());
        if (gw)
            p->set_dst_ip_anno(gw);
        return port;
    }
    else {
        static int complained = 0;
        if (++complained <= 5)
            click_chatter("IPRouteTableMPath: no route for %s", p->dst_ip_anno().unparse().c_str());
        return -1;
    }
}

void
IPRouteTableMPath::push(int, Packet *p)
{
    int output_port = process(p);
    if ( output_port < 0 ) {
        p->kill();
        return;
    }

    output(output_port).push(p);
}

#if HAVE_BATCH
void
IPRouteTableMPath::push_batch(int, PacketBatch *batch)
{
    CLASSIFY_EACH_PACKET(noutputs() + 1, process, batch, checked_output_push_batch);
}
#endif

int
IPRouteTableMPath::run_command(int command, const String &str, Vector<IPRouteMPath>* old_routes, ErrorHandler *errh)
{
    IPRouteMPath route, old_route;
    if (!cp_ip_route_mpath(str, &route, command == CMD_REMOVE, this))
        return errh->error("expected %<ADDR/MASK [GATEWAY%s%>", (command == CMD_REMOVE ? " OUTPUT]" : "] OUTPUT"));

    int r, before = errh->nerrors();
    if (command == CMD_ADD)
        r = add_route(route, false, &old_route, errh);
    else if (command == CMD_SET)
        r = add_route(route, true, &old_route, errh);
    else
        r = remove_route(route, &old_route, errh);

    // save old route if in a transaction
    if (r >= 0 && old_routes) {
        if (old_route.gwports.empty()) { // must come from add_route
            old_route = route;
            old_route.extra = CMD_ADD;
        } else
            old_route.extra = command;
        old_routes->push_back(old_route);
    }

    // report common errors
    if (r == -EEXIST && errh->nerrors() == before)
        errh->error("conflict with existing route %<%s%>", old_route.unparse().c_str());
    if (r == -ENOENT && errh->nerrors() == before)
        errh->error("route %<%s%> not found", route.unparse().c_str());
    if (r == -ENOMEM && errh->nerrors() == before)
        errh->error("no memory to store route %<%s%>", route.unparse().c_str());
    return r;
}


int
IPRouteTableMPath::add_route_handler(const String &conf, Element *e, void *thunk, ErrorHandler *errh)
{
    IPRouteTableMPath *table = static_cast<IPRouteTableMPath *>(e);
    return table->run_command((thunk ? CMD_SET : CMD_ADD), conf, 0, errh);
}

int
IPRouteTableMPath::remove_route_handler(const String &conf, Element *e, void *, ErrorHandler *errh)
{
    IPRouteTableMPath *table = static_cast<IPRouteTableMPath *>(e);
    return table->run_command(CMD_REMOVE, conf, 0, errh);
}

int
IPRouteTableMPath::ctrl_handler(const String &conf_in, Element *e, void *, ErrorHandler *errh)
{
    IPRouteTableMPath *table = static_cast<IPRouteTableMPath *>(e);
    String conf = cp_uncomment(conf_in);
    const char* s = conf.begin(), *end = conf.end();

    Vector<IPRouteMPath> old_routes;
    int r = 0;

    while (s < end) {
        const char* nl = find(s, end, '\n');
        String line = conf.substring(s, nl);

        String first_word = cp_shift_spacevec(line);
        int command;
        if (first_word == "add")
            command = CMD_ADD;
        else if (first_word == "remove")
            command = CMD_REMOVE;
        else if (first_word == "set" || first_word == "setm")
            command = CMD_SET;
        else if (!first_word)
            continue;
        else {
            r = errh->error("bad command %<%#s%>", first_word.c_str());
            goto rollback;
        }

        if ((r = table->run_command(command, line, &old_routes, errh)) < 0)
            goto rollback;

        s = nl + 1;
    }
    return 0;

  rollback:
    while (old_routes.size()) {
        const IPRouteMPath& rt = old_routes.back();
        if (rt.extra == CMD_REMOVE)
            table->add_route(rt, false, 0, errh);
        else if (rt.extra == CMD_ADD)
            table->remove_route(rt, 0, errh);
        else
            table->add_route(rt, true, 0, errh);
        old_routes.pop_back();
    }
    return r;
}

String
IPRouteTableMPath::table_handler(Element *e, void *)
{
    IPRouteTableMPath *r = static_cast<IPRouteTableMPath*>(e);
    return r->dump_routes();
}

int
IPRouteTableMPath::lookup_handler(int, String& s, Element* e, const Handler*, ErrorHandler* errh)
{
    IPRouteTableMPath *table = static_cast<IPRouteTableMPath*>(e);
    IPAddress a;
    if (IPAddressArg().parse(s, a, table)) {
        IPAddress gw;
        int port = table->lookup_route(a, gw);
        if (gw)
            s = String(port) + " " + gw.unparse();
        else
            s = String(port);
        return 0;
    } else
        return errh->error("expected IP address");
}

void
IPRouteTableMPath::add_handlers()
{
    add_write_handler("add", add_route_handler, 0);
    add_write_handler("set", add_route_handler, 1);
    add_write_handler("setm", add_route_handler, 1);
    add_write_handler("remove", remove_route_handler);
    add_write_handler("ctrl", ctrl_handler);
    add_read_handler("table", table_handler, 0, Handler::f_expensive);
    set_handler("lookup", Handler::f_read | Handler::f_read_param, lookup_handler);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(IPRouteTable)
ELEMENT_PROVIDES(IPRouteTableMPath)
