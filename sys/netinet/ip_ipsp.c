/*	$OpenBSD: ip_ipsp.c,v 1.22 1997/10/02 02:31:06 deraadt Exp $	*/

/*
 * The author of this code is John Ioannidis, ji@tla.org,
 * 	(except when noted otherwise).
 *
 * This code was written for BSD/OS in Athens, Greece, in November 1995.
 *
 * Ported to OpenBSD and NetBSD, with additional transforms, in December 1996,
 * by Angelos D. Keromytis, kermit@forthnet.gr.
 *
 * Copyright (C) 1995, 1996, 1997 by John Ioannidis and Angelos D. Keromytis.
 *	
 * Permission to use, copy, and modify this software without fee
 * is hereby granted, provided that this entire notice is included in
 * all copies of any software which is or includes a copy or
 * modification of this software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTY. IN PARTICULAR, NEITHER AUTHOR MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE
 * MERCHANTABILITY OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR
 * PURPOSE.
 */

/*
 * IPSP Processing
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/kernel.h>

#include <net/if.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>

#include <net/raw_cb.h>
#include <net/encap.h>

#include <netinet/ip_ipsp.h>
#include <netinet/ip_ah.h>
#include <netinet/ip_esp.h>

#include <dev/rndvar.h>
#include <sys/syslog.h>

int	tdb_init __P((struct tdb *, struct mbuf *));
int	ipsp_kern __P((int, char **, int));

int encdebug = 0;
u_int32_t kernfs_epoch = 0;

extern void encap_sendnotify(int, struct tdb *);

/*
 * This is the proper place to define the various encapsulation transforms.
 */

struct xformsw xformsw[] = {
    { XF_IP4,	         0,               "IPv4 Simple Encapsulation",
      ipe4_attach,       ipe4_init,       ipe4_zeroize,
      (struct mbuf * (*)(struct mbuf *, struct tdb *))ipe4_input, 
      ipe4_output, },
    { XF_OLD_AH,         XFT_AUTH,	  "Keyed Authentication, RFC 1828/1852",
      ah_old_attach,     ah_old_init,     ah_old_zeroize,
      ah_old_input,      ah_old_output, },
    { XF_OLD_ESP,        XFT_CONF,        "Simple Encryption, RFC 1829/1851",
      esp_old_attach,    esp_old_init,    esp_old_zeroize,
      esp_old_input,     esp_old_output, },
    { XF_NEW_AH,	 XFT_AUTH,	  "HMAC Authentication",
      ah_new_attach,	 ah_new_init,     ah_new_zeroize,
      ah_new_input,	 ah_new_output, },
    { XF_NEW_ESP,	 XFT_CONF|XFT_AUTH,
      "Encryption + Authentication + Replay Protection",
      esp_new_attach,	 esp_new_init,  esp_new_zeroize,
      esp_new_input,	 esp_new_output, },
};

struct xformsw *xformswNXFORMSW = &xformsw[sizeof(xformsw)/sizeof(xformsw[0])];

unsigned char ipseczeroes[IPSEC_ZEROES_SIZE]; /* zeroes! */ 


/*
 * Reserve an SPI; the SA is not valid yet though. Zero is reserved as
 * an error return value. If tspi is not zero, we try to allocate that
 * SPI. SPIs less than 255 are reserved, so we check for those too.
 */

u_int32_t
reserve_spi(u_int32_t tspi, struct in_addr src, u_int8_t proto, int *errval)
{
    struct tdb *tdbp;
    u_int32_t spi = tspi;		/* Don't change */
    
    while (1)
    {
	while (spi <= 255)		/* Get a new SPI */
	  get_random_bytes((void *) &spi, sizeof(spi));
	
	/* Check whether we're using this SPI already */
	if (gettdb(spi, src, proto) != (struct tdb *) NULL)
	{
	    if (tspi != 0)		/* If one was proposed, report error */
	    {
		(*errval) = EEXIST;
	      	return 0;
	    }

	    spi = 0;
	    continue;
	}
	
	MALLOC(tdbp, struct tdb *, sizeof(*tdbp), M_TDB, M_WAITOK);
	if (tdbp == NULL)
	{
	    (*errval) = ENOBUFS;
	    return 0;
	} 

	bzero((caddr_t) tdbp, sizeof(*tdbp));
	
	tdbp->tdb_spi = spi;
	tdbp->tdb_dst = src;
	tdbp->tdb_sproto = proto;
	tdbp->tdb_flags |= TDBF_INVALID;
	tdbp->tdb_epoch = kernfs_epoch - 1;
	
	puttdb(tdbp);
	
	return spi;
    }
}

/*
 * An IPSP SAID is really the concatenation of the SPI found in the 
 * packet, the destination address of the packet and the IPsec protocol.
 * When we receive an IPSP packet, we need to look up its tunnel descriptor
 * block, based on the SPI in the packet and the destination address (which
 * is really one of our addresses if we received the packet!
 */

struct tdb *
gettdb(u_int32_t spi, struct in_addr dst, u_int8_t proto)
{
    int hashval;
    struct tdb *tdbp;
	
    hashval = (spi + dst.s_addr + proto) % TDB_HASHMOD;
	
    for (tdbp = tdbh[hashval]; tdbp; tdbp = tdbp->tdb_hnext)
      if ((tdbp->tdb_spi == spi) && (tdbp->tdb_dst.s_addr == dst.s_addr)
	  && (tdbp->tdb_sproto == proto))
	break;
	
    return tdbp;
}

struct flow *
get_flow(void)
{
    struct flow *flow;

    MALLOC(flow, struct flow *, sizeof(struct flow), M_TDB, M_WAITOK);
    if (flow == (struct flow *) NULL)
      return (struct flow *) NULL;

    bzero(flow, sizeof(struct flow));

    return flow;
}

struct expiration *
get_expiration(void)
{
    struct expiration *exp;
    
    MALLOC(exp, struct expiration *, sizeof(struct expiration), M_TDB,
	   M_WAITOK);
    if (exp == (struct expiration *) NULL)
      return (struct expiration *) NULL;

    bzero(exp, sizeof(struct expiration));
    
    return exp;
}

void
cleanup_expirations(struct in_addr dst, u_int32_t spi, u_int8_t sproto)
{
    struct expiration *exp, *nexp;
    
    for (exp = explist; exp; exp = exp->exp_next)
      if ((exp->exp_dst.s_addr == dst.s_addr) &&
	  (exp->exp_spi == spi) && (exp->exp_sproto == sproto))
      {
	  /* Link previous to next */
	  if (exp->exp_prev == (struct expiration *) NULL)
	    explist = exp->exp_next;
	  else
	    exp->exp_prev->exp_next = exp->exp_next;
	  
	  /* Link next (if it exists) to previous */
	  if (exp->exp_next != (struct expiration *) NULL)
	    exp->exp_next->exp_prev = exp->exp_prev;
	 
	  nexp = exp;
	  exp = exp->exp_prev;
	  free(nexp, M_TDB);
      }
}

void 
handle_expirations(void *arg)
{
    struct expiration *exp;
    struct tdb *tdb;
    
    if (explist == (struct expiration *) NULL)
      return;
    
    while (1)
    {
	exp = explist;

	if (exp == (struct expiration *) NULL)
	  return;
	else
	  if (exp->exp_timeout > time.tv_sec)
	    break;
	
	/* Advance pointer */
	explist = explist->exp_next;
	if (explist)
	  explist->exp_prev = NULL;
	
	tdb = gettdb(exp->exp_spi, exp->exp_dst, exp->exp_sproto);
	if (tdb == (struct tdb *) NULL)
	{
	    free(exp, M_TDB);
	    continue;			/* TDB is gone, ignore this */
	}
	
	/* Soft expirations */
	if (tdb->tdb_flags & TDBF_SOFT_TIMER)
	  if (tdb->tdb_soft_timeout <= time.tv_sec)
	  {
	      encap_sendnotify(NOTIFY_SOFT_EXPIRE, tdb);
	      tdb->tdb_flags &= ~TDBF_SOFT_TIMER;
	  }
	  else
	    if (tdb->tdb_flags & TDBF_SOFT_FIRSTUSE)
	      if (tdb->tdb_first_use + tdb->tdb_soft_first_use <=
		  time.tv_sec)
	      {
		  encap_sendnotify(NOTIFY_SOFT_EXPIRE, tdb);
		  tdb->tdb_flags &= ~TDBF_SOFT_FIRSTUSE;
	      }
	
	/* Hard expirations */
	if (tdb->tdb_flags & TDBF_TIMER)
	  if (tdb->tdb_exp_timeout <= time.tv_sec)
	  {
	      encap_sendnotify(NOTIFY_HARD_EXPIRE, tdb);
	      tdb_delete(tdb, 0);
	  }
	  else
	    if (tdb->tdb_flags & TDBF_FIRSTUSE)
	      if (tdb->tdb_first_use + tdb->tdb_exp_first_use <=
		  time.tv_sec)
	      {
		  encap_sendnotify(NOTIFY_HARD_EXPIRE, tdb);
		  tdb_delete(tdb, 0);
	      }

	free(exp, M_TDB);
    }

    if (explist)
      timeout(handle_expirations, (void *) NULL, 
	      hz * (explist->exp_timeout - time.tv_sec));
}

void
put_expiration(struct expiration *exp)
{
    struct expiration *expt;
    int reschedflag = 0;
    
    if (exp == (struct expiration *) NULL)
    {
#ifdef ENCDEBUG
	if (encdebug)
	  log(LOG_WARNING, "put_expiration(): NULL argument\n");
#endif /* ENCDEBUG */	
	return;
    }
    
    if (explist == (struct expiration *) NULL)
    {
	explist = exp;
	reschedflag = 1;
    }
    else
      if (explist->exp_timeout > exp->exp_timeout)
      {
	  exp->exp_next = explist;
	  explist->exp_prev = exp;
	  explist = exp;
	  reschedflag = 2;
      }
      else
      {
	  for (expt = explist; expt->exp_next; expt = expt->exp_next)
	    if (expt->exp_next->exp_timeout > exp->exp_timeout)
	    {
		expt->exp_next->exp_prev = exp;
		exp->exp_next = expt->exp_next;
		expt->exp_next = exp;
		exp->exp_prev = expt;
		break;
	    }

	  if (expt->exp_next == (struct expiration *) NULL)
	  {
	      expt->exp_next = exp;
	      exp->exp_prev = expt;
	  }
      }

    switch (reschedflag)
    {
	case 1:
	    timeout(handle_expirations, (void *) NULL, 
		    hz * (explist->exp_timeout - time.tv_sec));
	    break;
	    
	case 2:
	    untimeout(handle_expirations, (void *) NULL);
	    timeout(handle_expirations, (void *) NULL,
		    hz * (explist->exp_timeout - time.tv_sec));
	    break;
	    
	default:
	    break;
    }
}

struct flow *
find_flow(struct in_addr src, struct in_addr srcmask, struct in_addr dst,
	  struct in_addr dstmask, u_int8_t proto, u_int16_t sport,
	  u_int16_t dport, struct tdb *tdb)
{
    struct flow *flow;

    for (flow = tdb->tdb_flow; flow; flow = flow->flow_next)
      if ((src.s_addr == flow->flow_src.s_addr) &&
	  (dst.s_addr == flow->flow_dst.s_addr) &&
	  (srcmask.s_addr == flow->flow_srcmask.s_addr) &&
	  (dstmask.s_addr == flow->flow_dstmask.s_addr) &&
	  (proto == flow->flow_proto) &&
	  (sport == flow->flow_sport) && (dport == flow->flow_dport))
	return flow;

    return (struct flow *) NULL;
}

struct flow *
find_global_flow(struct in_addr src, struct in_addr srcmask,
		 struct in_addr dst, struct in_addr dstmask,
		 u_int8_t proto, u_int16_t sport, u_int16_t dport)
{
    struct flow *flow;
    struct tdb *tdb;
    int i;

    for (i = 0; i < TDB_HASHMOD; i++)
      for (tdb = tdbh[i]; tdb; tdb = tdb->tdb_hnext)
	if ((flow = find_flow(src, srcmask, dst, dstmask, proto, sport,
			      dport, tdb)) != (struct flow *) NULL)
	  return flow;

    return (struct flow *) NULL;
}

void
puttdb(struct tdb *tdbp)
{
    int hashval;

    hashval = ((tdbp->tdb_sproto + tdbp->tdb_spi + tdbp->tdb_dst.s_addr)
	       % TDB_HASHMOD);
    tdbp->tdb_hnext = tdbh[hashval];
    tdbh[hashval] = tdbp;
}

void
put_flow(struct flow *flow, struct tdb *tdb)
{
    flow->flow_next = tdb->tdb_flow;
    flow->flow_prev = (struct flow *) NULL;

    tdb->tdb_flow = flow;

    flow->flow_sa = tdb;

    if (flow->flow_next)
      flow->flow_next->flow_prev = flow;
}

void
delete_flow(struct flow *flow, struct tdb *tdb)
{
    if (tdb->tdb_flow == flow)
    {
	tdb->tdb_flow = flow->flow_next;
	if (tdb->tdb_flow)
	  tdb->tdb_flow->flow_prev = (struct flow *) NULL;
    }
    else
    {
	flow->flow_prev->flow_next = flow->flow_next;
	if (flow->flow_next)
	  flow->flow_next->flow_prev = flow->flow_prev;
    }

    FREE(flow, M_TDB);
}

int
tdb_delete(struct tdb *tdbp, int delchain)
{
    struct tdb *tdbpp;
    struct flow *flow;
    int hashval;

    hashval = ((tdbp->tdb_sproto + tdbp->tdb_spi + tdbp->tdb_dst.s_addr)
	       % TDB_HASHMOD);

    if (tdbh[hashval] == tdbp)
    {
	tdbpp = tdbp;
	tdbh[hashval] = tdbp->tdb_hnext;
    }
    else
      for (tdbpp = tdbh[hashval]; tdbpp != NULL; tdbpp = tdbpp->tdb_hnext)
	if (tdbpp->tdb_hnext == tdbp)
	{
	    tdbpp->tdb_hnext = tdbp->tdb_hnext;
	    tdbpp = tdbp;
	}

    if (tdbp != tdbpp)
      return EINVAL;		/* Should never happen */

    /* If there was something before us in the chain, make it point nowhere */
    if (tdbp->tdb_inext)
      tdbp->tdb_inext->tdb_onext = NULL;

    tdbpp = tdbp->tdb_onext;

    if (tdbp->tdb_xform)
      (*(tdbp->tdb_xform->xf_zeroize))(tdbp);

    for (flow = tdbp->tdb_flow; flow; flow = tdbp->tdb_flow)
      delete_flow(flow, tdbp);

    cleanup_expirations(tdbp->tdb_dst, tdbp->tdb_spi, tdbp->tdb_sproto);
    
    FREE(tdbp, M_TDB);

    if (delchain && tdbpp)
      return tdb_delete(tdbpp, delchain);
    else
      return 0;
}

int
tdb_init(struct tdb *tdbp, struct mbuf *m)
{
    int alg;
    struct encap_msghdr *em;
    struct xformsw *xsp;
	
    em = mtod(m, struct encap_msghdr *);
    alg = em->em_alg;

    /* Record establishment time */
    tdbp->tdb_established = time.tv_sec;

    tdbp->tdb_epoch = kernfs_epoch - 1;

    for (xsp = xformsw; xsp < xformswNXFORMSW; xsp++)
      if (xsp->xf_type == alg)
	return (*(xsp->xf_init))(tdbp, xsp, m);

    if (encdebug)
      log(LOG_ERR, "tdb_init(): no alg %d for spi %08x, addr %x, proto %d\n", 
	  alg, ntohl(tdbp->tdb_spi), tdbp->tdb_dst.s_addr, tdbp->tdb_sproto);
    
    return EINVAL;
}

/*
 * Used by kernfs
 */
int
ipsp_kern(int off, char **bufp, int len)
{
    static char buffer[IPSEC_KERNFS_BUFSIZE];
    struct tdb *tdb;
    struct flow *fl;
    int l, i;

    if (off == 0)
      kernfs_epoch++;
    
    if (bufp == NULL)
      return 0;

    bzero(buffer, IPSEC_KERNFS_BUFSIZE);

    *bufp = buffer;
    
    for (i = 0; i < TDB_HASHMOD; i++)
      for (tdb = tdbh[i]; tdb; tdb = tdb->tdb_hnext)
	if (tdb->tdb_epoch != kernfs_epoch)
	{
	    tdb->tdb_epoch = kernfs_epoch;

	    l = sprintf(buffer, "SPI = %08x, Destination = %s, Sproto = %u\n",
			ntohl(tdb->tdb_spi), inet_ntoa(tdb->tdb_dst),
			tdb->tdb_sproto);
	    
	    l += sprintf(buffer + l, "\testablished %d seconds ago\n",
			 time.tv_sec - tdb->tdb_established);
	   
	    l += sprintf(buffer + l, "\tsrc = %s, flags = %08x, SAtype = %u\n",
			 inet_ntoa(tdb->tdb_src), tdb->tdb_flags,
			 tdb->tdb_satype);

	    if (tdb->tdb_xform)
	      l += sprintf(buffer + l, "\txform = <%s>\n", 
			   tdb->tdb_xform->xf_name);
	    else
	      l += sprintf(buffer + l, "\txform = <(null)>\n");

	    l += sprintf(buffer + l, "\tOSrc = %s", inet_ntoa(tdb->tdb_osrc));
	    
	    l += sprintf(buffer + l, " ODst = %s, TTL = %u\n",
			 inet_ntoa(tdb->tdb_odst), tdb->tdb_ttl);

	    if (tdb->tdb_onext)
	      l += sprintf(buffer + l, "\tNext (on output) SA: SPI = %08x, Destination = %s, Sproto = %u\n", ntohl(tdb->tdb_onext->tdb_spi), inet_ntoa(tdb->tdb_onext->tdb_dst), tdb->tdb_onext->tdb_sproto);

	    if (tdb->tdb_inext)
	      l += sprintf(buffer + l, "\tNext (on input) SA: SPI = %08x, Destination = %s, Sproto = %u\n", ntohl(tdb->tdb_inext->tdb_spi), inet_ntoa(tdb->tdb_inext->tdb_dst), tdb->tdb_inext->tdb_sproto);

	    /* XXX We can reuse variable i, we're not going to loop again */
	    for (i = 0, fl = tdb->tdb_flow; fl; fl = fl->flow_next)
	      i++;

	    l += sprintf(buffer + l, "\t%u flows counted (use netstat -r for  more information)\n", i);
	    
	    l += sprintf(buffer + l, "\tExpirations:\n");

	    if (tdb->tdb_flags & TDBF_TIMER)
	      l += sprintf(buffer + l, "\t\tHard expiration(1) in %u seconds\n",
			   tdb->tdb_exp_timeout - time.tv_sec);
	    
	    if (tdb->tdb_flags & TDBF_SOFT_TIMER)
	      l += sprintf(buffer + l, "\t\tSoft expiration(1) in %u seconds\n",
			   tdb->tdb_soft_timeout - time.tv_sec);
	    
	    if (tdb->tdb_flags & TDBF_BYTES)
	      l += sprintf(buffer + l, "\t\tHard expiration after %qu bytes\n",
			   tdb->tdb_exp_bytes);
	    
	    if (tdb->tdb_flags & TDBF_SOFT_BYTES)
	      l += sprintf(buffer + l, "\t\tSoft expiration after %qu bytes\n",
			   tdb->tdb_soft_bytes);

	    l += sprintf(buffer + l, "\t\tCurrently %qu bytes processed\n",
			 tdb->tdb_cur_bytes);
	    
	    if (tdb->tdb_flags & TDBF_PACKETS)
	      l += sprintf(buffer + l,
			   "\t\tHard expiration after %qu packets\n",
			   tdb->tdb_exp_packets);
	    
	    if (tdb->tdb_flags & TDBF_SOFT_PACKETS)
	      l += sprintf(buffer + l,
			   "\t\tSoft expiration after %qu packets\n",
			   tdb->tdb_soft_packets);

	    l += sprintf(buffer + l, "\t\tCurrently %qu packets processed\n",
			 tdb->tdb_cur_packets);
	    
	    if (tdb->tdb_flags & TDBF_FIRSTUSE)
	      l += sprintf(buffer + l, "\t\tHard expiration(2) in %u seconds\n",
			   (tdb->tdb_established + tdb->tdb_exp_first_use) -
			   time.tv_sec);
	    
	    if (tdb->tdb_flags & TDBF_SOFT_FIRSTUSE)
	      l += sprintf(buffer + l, "\t\tSoft expiration(2) in %u seconds\n",
			   (tdb->tdb_established + tdb->tdb_soft_first_use) -
			   time.tv_sec);

	    if (!(tdb->tdb_flags & (TDBF_TIMER | TDBF_SOFT_TIMER | TDBF_BYTES |
				    TDBF_SOFT_PACKETS | TDBF_PACKETS |
				    TDBF_SOFT_BYTES | TDBF_FIRSTUSE |
				    TDBF_SOFT_FIRSTUSE)))
	      l += sprintf(buffer + l, "\t\t(none)\n");

	    l += sprintf(buffer + l, "\n");
	    
	    return l;
	}
    
    return 0;
}
