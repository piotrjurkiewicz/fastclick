// -*- c-basic-offset: 4 -*-
#include <click/config.h>
#include "directiplookupmpathmt.hh"
CLICK_DECLS

DirectIPLookupMPathMT::DirectIPLookupMPathMT()
{
}

DirectIPLookupMPathMT::~DirectIPLookupMPathMT()
{
}

void
DirectIPLookupMPathMT::read_begin(int &) const
{
    _lock.read_begin();
}

void
DirectIPLookupMPathMT::read_end(int &) const
{
    _lock.read_end();
}

void
DirectIPLookupMPathMT::write_begin()
{
    _lock.write_begin();
}

void
DirectIPLookupMPathMT::write_end()
{
    _lock.write_end();
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(DirectIPLookupMPath)
EXPORT_ELEMENT(DirectIPLookupMPathMT)
