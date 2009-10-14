/*	$OpenBSD: mainbus.c,v 1.2 2009/10/14 20:21:16 miod Exp $ */

/*
 * Copyright (c) 2001-2003 Opsycon AB  (www.opsycon.se / www.opsycon.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/reboot.h>

#include <mips64/archtype.h>
#include <machine/autoconf.h>

/* Definition of the mainbus driver. */
int	mbmatch(struct device *, void *, void *);
void	mbattach(struct device *, struct device *, void *);
int	mbprint(void *, const char *);

const struct cfattach mainbus_ca = {
	sizeof(struct device), mbmatch, mbattach
};

struct cfdriver mainbus_cd = {
	NULL, "mainbus", DV_DULL
};

int
mbmatch(struct device *parent, void *cfdata, void *aux)
{
	static int mbattached = 0;

	if (mbattached != 0)
		return 0;

	return mbattached = 1;
}

void
mbattach(struct device *parent, struct device *self, void *aux)
{
	struct confargs nca;
	extern char *hw_prod;

	if (hw_prod != NULL)
		printf(": %s", hw_prod);
	printf("\n");

	/*
	 * On IP27 and IP35 system, delegate everything to the IP-specific
	 * code.
	 */
	switch (sys_config.system_type) {
#if defined(TGT_ORIGIN200) || defined(TGT_ORIGIN2000)
	case SGI_O200:
	case SGI_O300:
		ip27_autoconf(self);
		return;
#endif
	default:
		break;
	}

	/*
	 * On other systems, attach the CPU we are running on early;
	 * other processors, if any, will get attached as they are
	 * discovered.
	 */

	bzero(&nca, sizeof nca);
	nca.ca_name = "cpu";
	config_found(self, &nca, mbprint);
	nca.ca_name = "clock";
	config_found(self, &nca, mbprint);

	switch (sys_config.system_type) {
#ifdef TGT_O2
	case SGI_O2:
		nca.ca_name = "macebus";
		config_found(self, &nca, mbprint);
		nca.ca_name = "gbe";
		config_found(self, &nca, mbprint);
		break;
#endif
#ifdef TGT_OCTANE
	case SGI_OCTANE:
		nca.ca_name = "xbow";
		config_found(self, &nca, mbprint);
		break;
#endif
	default:
		break;
	}
}

int
mbprint(void *aux, const char *pnp)
{
	if (pnp)
		return (QUIET);
	return (UNCONF);
}
