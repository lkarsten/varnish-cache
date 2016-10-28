/*-
 * Copyright (c) 2016 Varnish Software
 * All rights reserved.
 *
 * Author: Lasse Karstensen <lasse.karstensen@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Export VSC ("varnishstat") to InfluxDB over UDP.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vdef.h"
#include "vcurses.h"
#include "vapi/vsl.h"
#include "vapi/vsm.h"
#include "vapi/voptget.h"
#include "vapi/vsc.h"
#include "vas.h"
#include "vut.h"
#include "vcs.h"
#include "vsb.h"

#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "vnum.h"
#include "vtim.h"

#include "influxstat.h"

// #include "influxlog_options.h"

/* Global variables are the bestest. */
int sock = -1;
char hostname[64];

// char tags[] = "service=varnish"

static int
send_packet(const struct vsb *msg)
{

	fprintf(stderr, "%s\n", VSB_data(msg));
	return(0);
	// AN(sock);  /* global */
//	int written = write(sock, VSB_data(msg), VSB_len(msg));
//	return(written);
}

static int
do_influx_cb(void *priv, const struct VSC_point * const pt)
{
	char time_stamp[20];

	if (pt == NULL)
		return (0);

	(void)priv;
	// struct vsb *msg = priv;
	//
	AZ(strcmp(pt->desc->ctype, "uint64_t"));
	uint64_t val = *(const volatile uint64_t*)pt->ptr;

	if (strcmp(pt->section->fantom->type, "VBE"))
		/* Ignore backend counters for now. */
		return(-1);
		// printf("\t\t<ident>%s</ident>\n", sec->fantom->ident);

	time_t now = time(NULL);
	(void)strftime(time_stamp, 20, "%Y-%m-%dT%H:%M:%S", localtime(&now));

	struct vsb *msg = VSB_new_auto();
	AN(msg);

	VSB_printf(msg, "%s.%s", pt->section->fantom->ident, pt->desc->name);
	VSB_printf(msg, ",hostname=%s", hostname);
	VSB_cat(msg, ",service=varnish");
	VSB_printf(msg, " value=%ju\n", (uintmax_t)val);

	VSB_finish(msg);

	send_packet(msg);

	VSB_delete(msg);
	return (0);
}


static void
do_influx_udp(struct VSM_data *vd, const long interval)
{
	while (0) {
		(void)VSC_Iter(vd, NULL, do_influx_cb, NULL);
		VTIM_sleep(interval);
	}
}

/* List function is verbatim from varnishstat.c. */
static int
do_list_cb(void *priv, const struct VSC_point * const pt)
{
	int i;
	const struct VSC_section * sec;

	(void)priv;

	if (pt == NULL)
		return (0);

	sec = pt->section;
	i = 0;
	if (strcmp(sec->fantom->type, ""))
		i += fprintf(stderr, "%s.", sec->fantom->type);
	if (strcmp(sec->fantom->ident, ""))
		i += fprintf(stderr, "%s.", sec->fantom->ident);
	i += fprintf(stderr, "%s", pt->desc->name);
	if (i < 30)
		fprintf(stderr, "%*s", i - 30, "");
	fprintf(stderr, " %s\n", pt->desc->sdesc);
	return (0);
}


static void
list_fields(struct VSM_data *vd)
{
	fprintf(stderr, "influxstat -f option fields:\n");
	fprintf(stderr, "Field name                     Description\n");
	fprintf(stderr, "----------                     -----------\n");

	(void)VSC_Iter(vd, NULL, do_list_cb, NULL);
}

/*--------------------------------------------------------------------*/

static void
usage(void)
{
#define FMT "    %-28s # %s\n"
	fprintf(stderr, "Usage: influxstat "
	    "[lV] [-f field] [-t seconds|<off>] [-i seconds] "
	    VSC_n_USAGE "\n");
	fprintf(stderr, FMT, "-f field", "Field inclusion glob");
	fprintf(stderr, FMT, "",
	    "If it starts with '^' it is used as an exclusion list.");
	fprintf(stderr, FMT, "-i seconds", "Update interval (default 1.0s)");
	fprintf(stderr, FMT, "-V", "Display the version number and exit.");
	fprintf(stderr, FMT, "-l",
	    "Lists the available fields to use with the -f option.");
	fprintf(stderr, FMT, "-n varnish_name",
	    "The varnishd instance to get logs from.");
	fprintf(stderr, FMT, "-N filename",
	    "Filename of a stale VSM instance.");
	fprintf(stderr, FMT, "-t seconds|<off>",
	    "Timeout before returning error on initial VSM connection.");
	fprintf(stderr, FMT, "-V", "Display the version number and exit.");
#undef FMT
	exit(1);
}

int
main(int argc, char * const *argv)
{
	struct VSM_data *vd;
	double t_arg = 5.0, t_start = NAN;
	long interval = 1.0;
	int f_list = 0;
	int i, c;

	vd = VSM_New();
	AN(vd);

	while ((c = getopt(argc, argv, VSC_ARGS "1f:i:lVxjt:")) != -1) {
		switch (c) {
		case 'i':
			interval = atol(optarg);  /* seconds */
			break;
		case 'l':
			f_list = 1;
			break;
		case 't':
			if (!strcasecmp(optarg, "off"))
				t_arg = -1.;
			else {
				t_arg = VNUM(optarg);
				if (isnan(t_arg)) {
					fprintf(stderr, "-t: Syntax error");
					exit(1);
				}
				if (t_arg < 0.) {
					fprintf(stderr, "-t: Range error");
					exit(1);
				}
			}
			break;
		case 'V':
			VCS_Message("influxstat");
			exit(0);
		default:
			if (VSC_Arg(vd, c, optarg) > 0)
				break;
			fprintf(stderr, "%s\n", VSM_Error(vd));
			usage();
		}
	}

	while (1) {
		i = VSM_Open(vd);
		if (!i)
			break;
		if (isnan(t_start) && t_arg > 0.) {
			fprintf(stderr, "Can't open log -"
			    " retrying for %.0f seconds\n", t_arg);
			t_start = VTIM_real();
		}
		if (t_arg <= 0.)
			break;
		if (VTIM_real() - t_start > t_arg)
			break;
		VSM_ResetError(vd);
		VTIM_sleep(0.5);
	}

	if (f_list) {
		list_fields(vd);
	}

	AZ(gethostname(hostname, sizeof(hostname)));

	/*
	struct sockaddr *addr;
	socklen_t addrlen;   
	char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

	if (getnameinfo(addr, addrlen, hbuf, sizeof(hbuf), sbuf,
	       sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
	printf("host=%s, serv=%s\n", hbuf, sbuf);



	int sock = socket(AF_INET, SOCK_DGRAM, 0);

	struct sockaddr dst;

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	sendto

	connect(
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	*/

	do_influx_udp(vd, interval);
	exit(0);
}
