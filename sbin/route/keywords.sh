#!/bin/sh
# $OpenBSD: keywords.sh,v 1.28 2015/07/18 00:05:02 phessler Exp $
# $NetBSD: keywords.sh,v 1.2 1996/11/15 18:57:21 gwr Exp $
# @(#)keywords	8.2 (Berkeley) 3/19/94
#
# WARNING!  If you change this file, re-run it!

# This program requires "new" awk (or GNU awk).
awk=${AWK:-awk}

# the following must be sorted
cat << _EOF_ | sort > _keywords.t1
add
blackhole
bgp
change
cloning
connected
delete
dst
exec
expire
flush
gateway
get
host
hopcount
iface
interface
ifa
ifp
in
inet
inet6
jumbo
label
link
llinfo
local
lock
lockrest
monitor
mpath
mpls
mplslabel
mtu
net
netmask
nojumbo
nostatic
ospf
out
pop
prefixlen
priority
proto1
proto2
push
recvpipe
reject
rip
rtt
rttvar
sa
sendpipe
show
ssthresh
static
swap
xresolve
_EOF_


################################################################
# Setup
################################################################

# This creates a stream of:
#	keyword KEYWORD
# (lower case, upper case).
tr a-z A-Z < _keywords.t1 |
paste _keywords.t1 - > _keywords.t2

################################################################
# Generate the h file
################################################################
exec > keywords.h

echo '/* $'OpenBSD'$ */

/* WARNING!  This file was generated by keywords.sh  */

struct keytab {
	char	*kt_cp;
	int	kt_i;
};

enum {
	K_NULL,'

$awk '{
	printf("\tK_%s,\n", $2);
}' < _keywords.t2

echo '};

struct keytab keywords[] = {'

$awk '{
	printf("\t{ \"%s\",\tK_%s },\n", $1, $2);
}' < _keywords.t2

echo '};
' # tail


################################################################
# Cleanup
################################################################

rm -f _keywords.t1 _keywords.t2
exit 0
