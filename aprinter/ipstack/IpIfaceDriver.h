/*
 * Copyright (c) 2016 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef APRINTER_IPSTACK_IP_IFACE_DRIVER_H
#define APRINTER_IPSTACK_IP_IFACE_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include <aprinter/ipstack/Buf.h>
#include <aprinter/ipstack/IpAddr.h>
#include <aprinter/ipstack/Err.h>

#include <aprinter/BeginNamespace.h>

struct IpIfaceIp4Addrs {
    Ip4Addr addr;
    Ip4Addr netmask;
    Ip4Addr netaddr;
    Ip4Addr bcastaddr;
    uint8_t prefix;
};

class IpIfaceDriverCallback;

class IpIfaceDriver {
public:
    virtual void setCallback (IpIfaceDriverCallback *callback) = 0;
    virtual size_t getIpMtu () = 0;
    virtual IpErr sendIp4Packet (IpBufRef pkt, Ip4Addr ip_addr) = 0;
};

class IpIfaceDriverCallback {
public:
    virtual IpIfaceIp4Addrs const * getIp4Addrs () = 0;
    virtual void recvIp4Packet (IpBufRef pkt) = 0;
};

#include <aprinter/EndNamespace.h>

#endif
