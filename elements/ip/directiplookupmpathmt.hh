// -*- c-basic-offset: 4 -*-
#ifndef CLICK_DIRECTIPLOOKUPMPATHMT_HH
#define CLICK_DIRECTIPLOOKUPMPATHMT_HH
#include <click/glue.hh>
#include <click/multithread.hh>
#include "directiplookupmpath.hh"
CLICK_DECLS

class DirectIPLookupMPathMT : public DirectIPLookupMPath { public:

    DirectIPLookupMPathMT() CLICK_COLD;
    ~DirectIPLookupMPathMT() CLICK_COLD;

    const char *class_name() const { return "DirectIPLookupMPathMT"; }

  protected:

    void read_begin(int&) const;
    void read_end(int&) const;
    void write_begin();
    void write_end();

  private:

    mutable RWLock _lock;

};

CLICK_ENDDECLS
#endif
