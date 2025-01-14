/*
 * Copyright (c) 2003  Jeremie Miller <jer@jabber.org>
 * Copyright (c) 2016-2022  Joachim Wiberg <troglobit@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holders nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <getopt.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "mdnsd.h"

#define SYS_INTERVAL 10		/* System inteface poll interval */

volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload = 0;
char *prognm      = PACKAGE_NAME;
char *ifname      = NULL;
char *path        = NULL;
int   background  = 1;
int   logging     = 1;
int   ttl         = 255;

static int multicast_socket(struct iface *iface, unsigned char ttl);


void mdnsd_conflict(char *name, int type, void *arg)
{
	struct iface *iface = (struct iface *)arg;

	WARN("%s: conlicting name detected %s for type %d, reloading config ...", iface->ifname, name, type);
	if (!reload) {
		iface->hostid++;
		reload = 1;
	}
}

static void record_received(const struct resource *r, void *data)
{
	char ipinput[INET_ADDRSTRLEN];

	switch(r->type) {
	case QTYPE_A:
		inet_ntop(AF_INET, &(r->known.a.ip), ipinput, INET_ADDRSTRLEN);
		DBG("Got %s: A %s->%s", r->name, r->known.a.name, ipinput);
		break;

	case QTYPE_NS:
		DBG("Got %s: NS %s", r->name, r->known.ns.name);
		break;

	case QTYPE_CNAME:
		DBG("Got %s: CNAME %s", r->name, r->known.cname.name);
		break;

	case QTYPE_PTR:
		DBG("Got %s: PTR %s", r->name, r->known.ptr.name);
		break;

	case QTYPE_TXT:
		DBG("Got %s: TXT %s", r->name, r->rdata);
		break;

	case QTYPE_SRV:
		DBG("Got %s: SRV %d %d %d %s", r->name, r->known.srv.priority,
		    r->known.srv.weight, r->known.srv.port, r->known.srv.name);
		break;

	default:
		DBG("Got %s: unknown", r->name);

	}
}

static void free_iface(struct iface *iface)
{
	mdnsd_shutdown(iface->mdns);
	mdnsd_free(iface->mdns);
	if (iface->sd >= 0)
		close(iface->sd);
}

static void setup_iface(struct iface *iface)
{
	if (!iface->changed)
		return;

	if (iface->unused) {
		free_iface(iface);
		return;
	}

	if (!iface->mdns) {
		iface->mdns = mdnsd_new(QCLASS_IN, 1000);
		if (!iface->mdns) {
			ERR("Failed creating mDNS context for iface %s: %s", iface->ifname, strerror(errno));
			exit(1);
		}

		conf_init(iface, path);
		mdnsd_register_receive_callback(iface->mdns, record_received, NULL);
	}

	if (iface->sd < 0) {
		iface->sd = multicast_socket(iface, (unsigned char)ttl);
		if (iface->sd < 0) {
			ERR("Failed creating socket: %s", strerror(errno));
			exit(1);
		}
	}

	mdnsd_set_address(iface->mdns, iface->inaddr);
	iface->changed = 0;
}

static int sys_timeout(int *timeout)
{
	static struct timespec before;
	struct timespec now;

	if (*timeout == 0) {
		clock_gettime(CLOCK_MONOTONIC, &before);
		*timeout = SYS_INTERVAL;
	} else {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (before.tv_sec + SYS_INTERVAL <= now.tv_sec) {
			before = now;
			return 1;
		}
	}

	return 0;
}

static void sys_init(void)
{
	struct iface *iface;

	/* Initialize or check if IP address changed, needed to update A records */
	iface_init(ifname);

	for (iface = iface_iterator(1); iface; iface = iface_iterator(0))
		setup_iface(iface);
}

static void done(int signo)
{
	running = 0;
}

static void reconf(int signo)
{
	reload = 1;
}

static void sig_init(void)
{
	signal(SIGINT, done);
	signal(SIGHUP, reconf);
	signal(SIGQUIT, done);
	signal(SIGTERM, done);
}

/*
 * Create a multicast socket and bind it to the given interface.
 * Conclude by joining 224.0.0.251:5353 to hear others.
 */
static int multicast_socket(struct iface *iface, unsigned char ttl)
{
#ifdef HAVE_STRUCT_IP_MREQN_IMR_IFINDEX
	struct ip_mreqn imr = { .imr_ifindex = iface->ifindex };
#else
	struct ip_mreq imr;
#endif
	struct sockaddr_in sin;
	socklen_t len;
	int unicast_ttl = 255;
	unsigned char on = 1;
	int bufsiz, flag = 1;
	int sd;

	sd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (sd < 0) {
		ERR("Failed creating UDP socket: %s", strerror(errno));
		return -1;
	}

#ifdef SO_REUSEPORT
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)))
		WARN("Failed setting SO_REUSEPORT: %s", strerror(errno));
#endif
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)))
		WARN("Failed setting SO_REUSEADDR: %s", strerror(errno));

	/* Double the size of the receive buffer (getsockopt() returns the double) */
	len = sizeof(bufsiz);
	if (!getsockopt(sd, SOL_SOCKET, SO_RCVBUF, &bufsiz, &len)) {
		if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &bufsiz, sizeof(bufsiz)))
			INFO("Failed doubling the size of the receive buffer: %s", strerror(errno));
	}

	if (setsockopt(sd, IPPROTO_IP, IP_PKTINFO, &flag, sizeof(flag)))
		WARN("Failed setting %s IP_PKTINFO: %s", iface->ifindex, strerror(errno));

	/* Set interface for outbound multicast */
#ifdef HAVE_STRUCT_IP_MREQN_IMR_IFINDEX
	if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_IF, &imr, sizeof(imr)))
		WARN("Failed setting IP_MULTICAST_IF %d: %s", iface->ifindex, strerror(errno));
#else
	if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_IF, &ina, sizeof(ina)))
		WARN("Failed setting IP_MULTICAST_IF to %s: %s",
		     inet_ntoa(ina), strerror(errno));
#endif

	if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof(on)))
		WARN("Failed disabling IP_MULTICAST_LOOP on %s: %s", iface->ifname, strerror(errno));

	flag = 0;
	if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_ALL, &flag, sizeof(flag)))
		WARN("Failed disabling IP_MULTICAST_LOOP on %s: %s", iface->ifname, strerror(errno));

	/*
	 * All traffic on 224.0.0.* is link-local only, so the default
	 * TTL is set to 1.  Some users may however want to route mDNS.
	 */
	if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)))
		WARN("Failed setting IP_MULTICAST_TTL to %d: %s", ttl, strerror(errno));

	/* mDNS also supports unicast, so we need a relevant TTL there too */
	if (setsockopt(sd, IPPROTO_IP, IP_TTL, &unicast_ttl, sizeof(unicast_ttl)))
		WARN("Failed setting IP_TTL to %d: %s", unicast_ttl, strerror(errno));

	/* Filter inbound traffic from anyone (ANY) to port 5353 on ifname */
	if (setsockopt(sd, SOL_SOCKET, SO_BINDTODEVICE, &iface->ifname, strlen(iface->ifname)))
		WARN("Failed setting SO_BINDTODEVICE: %s", strerror(errno));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(5353);
	if (bind(sd, (struct sockaddr *)&sin, sizeof(sin))) {
		close(sd);
		return -1;
	}
	INFO("Bound to *:5353 on iface %s", iface->ifname);

	/*
	 * Join mDNS link-local group on the given interface, that way
	 * we can receive multicast without a proper net route (default
	 * route or a 224.0.0.0/24 net route).
	 */
	imr.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
#ifdef HAVE_STRUCT_IP_MREQN_IMR_IFINDEX
	imr.imr_ifindex   = iface->ifindex;
#else
	imr.imr_interface = iface->inaddr;
#endif
	if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(imr)))
		WARN("Failed joining mDMS group 224.0.0.251: %s", strerror(errno));

	return sd;
}

static int usage(int code)
{
	printf("Usage: %s [-hnsv] [-i IFACE] [-l LEVEL] [-t TTL] [PATH]\n"
	       "\n"
	       "Options:\n"
	       "    -h        This help text\n"
	       "    -i IFACE  Interface to announce services on, and get address from\n"
	       "    -l LEVEL  Set log level: none, err, notice (default), info, debug\n"
	       "    -n        Run in foreground, do not detach from controlling terminal\n"
	       "    -s        Use syslog even if running in foreground\n"
	       "    -t TTL    Set TTL of mDNS packets, default: 1 (link-local only)\n"
	       "    -v        Show program version\n"
	       "\n"
	       "Arguments:\n"
	       "    PATH      Path to mDNS-SD .service files, default: /etc/mdns.d\n"
	       "\n"
	       "Bug report address: %-40s\n", prognm, PACKAGE_BUGREPORT);

	return code;
}

static char *progname(char *arg0)
{
       char *nm;

       nm = strrchr(arg0, '/');
       if (nm)
	       nm++;
       else
	       nm = arg0;

       return nm;
}

int main(int argc, char *argv[])
{
	struct timeval tv = { 0 };
	struct iface *iface;
	fd_set fds;
	int timeout = 0;
	int c, rc;

	prognm = progname(argv[0]);
	while ((c = getopt(argc, argv, "hi:l:nst:v?")) != EOF) {
		switch (c) {
		case 'h':
		case '?':
			return usage(0);

		case 'i':
			ifname = optarg;
			break;

		case 'l':
			if (-1 == mdnsd_log_level(optarg))
				return usage(1);
			break;

		case 'n':
			background = 0;
			logging--;
			break;

		case 's':
			logging++;
			break;

		case 't':
			/* XXX: Use strtonum() instead */
			ttl = atoi(optarg);
			if (ttl < 1 || ttl > 255)
				return usage(1);
			break;

		case 'v':
			puts(PACKAGE_VERSION);
			return 0;

		default:
			break;
		}
	}

	if (optind < argc)
		path = argv[optind];
	else
		path = "/etc/mdns.d";

	if (logging > 0)
		mdnsd_log_open(prognm);

	if (background) {
		DBG("Daemonizing ...");
		if (-1 == daemon(0, 0)) {
			ERR("Failed daemonizing: %s", strerror(errno));
			return 1;
		}
	}

	NOTE("%s starting.", PACKAGE_STRING);
	sig_init();
	sys_init();
	pidfile(PACKAGE_NAME);

	while (running) {
		int nfds = 0;

		FD_ZERO(&fds);
		for (iface = iface_iterator(1); iface; iface = iface_iterator(0)) {
			if (iface->sd < 0 || iface->unused)
				continue;

			FD_SET(iface->sd, &fds);
			if (iface->sd > nfds)
				nfds = iface->sd;
		}

		if (nfds > 0)
			nfds++;

		DBG("Going to sleep for %d sec ...", (int)tv.tv_sec);
		rc = select(nfds, &fds, NULL, NULL, &tv);
		if ((rc < 0 && EINTR == errno) || reload) {
			if (!running)
				break;
			if (reload) {
				sys_init();
				for (iface = iface_iterator(1); iface; iface = iface_iterator(0)) {
					records_clear(iface->mdns);
					conf_init(iface, path);
				}
				pidfile(PACKAGE_NAME);
				reload = 0;
			}

			continue;
		}

		if (sys_timeout(&timeout))
		    sys_init();

		tv.tv_sec = timeout;
		for (iface = iface_iterator(1); iface; iface = iface_iterator(0)) {
			struct timeval next;

			DBG("Checking iface %s for activity ...", iface->ifname);
			if (iface->unused || iface->sd < 0)
				continue;

			rc = mdnsd_step(iface->mdns, iface->sd, FD_ISSET(iface->sd, &fds), true, &next);
			if (!rc) {
				if (tv.tv_sec > next.tv_sec)
					tv = next;
				continue;
			}

			if (rc == 1)
				ERR("Failed reading from socket %d: %s", errno, strerror(errno));
			if (rc == 2)
				ERR("Failed writing to socket: %s", strerror(errno));

			free_iface(iface);
		}
	}

	NOTE("%s exiting.", PACKAGE_STRING);
	for (iface = iface_iterator(1); iface; iface = iface_iterator(0))
		free_iface(iface);
	iface_exit();

	return 0;
}
