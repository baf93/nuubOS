/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-smb-browse — find SMB servers on the local network and list their
 * shared folders (EPIC-036, Add Network Location). Run by nuubos-sharesctl.
 *
 *   discover            server=<name>\t<address> ... end=1   (~3 s)
 *   shares HOST [USER]  password on stdin (no line = none):
 *                       share=<name>\t<comment> ... end=1
 *                       | ERR auth | ERR unreachable | ERR failed
 *
 * discover asks three ways at once, on the IPv4 network of every interface
 * that is up (the /24 around the console when the network is larger):
 *   - mDNS (RFC 6762, a one-shot query from an ephemeral port, answered by
 *     unicast): _smb._tcp.local, how NAS, macOS and Linux (Avahi) announce
 *     file sharing; the instance name is the server name;
 *   - NetBIOS node status (RFC 1002) to every address, as nbtscan does:
 *     Windows and Samba answer with their names; only hosts registering
 *     the Server service (<20>) share files;
 *   - WS-Discovery Probe for pub:Computer (Windows 10/11 network
 *     discovery, wsdd on Linux): the answering address.
 * NetBIOS and WS-Discovery hosts must accept TCP 445 to be listed. Results
 * are merged by address; the name comes from mDNS, else NetBIOS, else the
 * address itself.
 *
 * shares connects to IPC$ with libsmb2 and calls srvsvc NetShareEnumAll
 * (level 1): disk shares only, hidden ones ("$") left out. Without USER it
 * tries an anonymous session, then guest; a refused login is "ERR auth" so
 * the caller can ask for a user name and password.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/smb2-errors.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
#include <smb2/libsmb2-raw.h>

#define MAX_SERVERS 128
#define DISCOVER_MS 2500
#define PORT_CHECK_MS 1200
#define SHARES_TIMEOUT 8

struct server {
	uint32_t addr; /* network order */
	char name[64];
	int rank;      /* 3 mDNS, 2 NetBIOS, 1 WS-Discovery */
	bool smb;      /* known to share files (mDNS _smb, NetBIOS <20>) */
	bool wsd;
	bool open445;
};

static struct server servers[MAX_SERVERS];
static int server_count;
static uint32_t own[16];
static int own_count;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct server *server_at(uint32_t addr)
{
	for (int i = 0; i < own_count; i++)
		if (own[i] == addr)
			return NULL;
	for (int i = 0; i < server_count; i++)
		if (servers[i].addr == addr)
			return &servers[i];
	if (server_count == MAX_SERVERS)
		return NULL;
	memset(&servers[server_count], 0, sizeof(servers[0]));
	servers[server_count].addr = addr;
	return &servers[server_count++];
}

static void set_name(struct server *s, const char *name, size_t len, int rank)
{
	size_t n = 0;

	if (!s || rank < s->rank || !len)
		return;
	for (size_t i = 0; i < len && n + 1 < sizeof(s->name); i++) {
		unsigned char c = (unsigned char)name[i];

		s->name[n++] = (c < 0x20 || c == 0x7f || c == '\t') ? ' ' : (char)c;
	}
	while (n && s->name[n - 1] == ' ')
		n--;
	s->name[n] = '\0';
	if (n)
		s->rank = rank;
}

/* ------------------------------------------------------------------ */
/* mDNS                                                                */
/* ------------------------------------------------------------------ */

static const char SMB_SERVICE[] = "\x04_smb\x04_tcp\x05local";

/* A DNS name at off into out (labels joined by '.'), following
 * compression pointers; returns the offset after the name in the
 * record, -1 on a malformed packet. */
static int dns_name(const unsigned char *p, int len, int off, char *out, size_t size)
{
	int next = -1, jumps = 0;
	size_t n = 0;

	out[0] = '\0';
	while (off < len) {
		int l = p[off];

		if (l == 0) {
			off++;
			break;
		}
		if ((l & 0xc0) == 0xc0) {
			if (off + 1 >= len || ++jumps > 16)
				return -1;
			if (next < 0)
				next = off + 2;
			off = ((l & 0x3f) << 8) | p[off + 1];
			continue;
		}
		if (off + 1 + l > len)
			return -1;
		if (n && n + 1 < size)
			out[n++] = '.';
		for (int i = 0; i < l && n + 1 < size; i++)
			out[n++] = (char)p[off + 1 + i];
		out[n] = '\0';
		off += 1 + l;
	}
	return next >= 0 ? next : off;
}

static void mdns_query(int fd)
{
	unsigned char q[64];
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
	size_t n = 12;

	memset(q, 0, sizeof(q));
	q[5] = 1; /* one question */
	memcpy(q + n, SMB_SERVICE, sizeof(SMB_SERVICE)); /* with its 0 end */
	n += sizeof(SMB_SERVICE);
	q[n++] = 0; q[n++] = 12;    /* PTR */
	q[n++] = 0x80; q[n++] = 1;  /* IN, unicast response wanted */
	inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
	sendto(fd, q, n, 0, (struct sockaddr *)&to, sizeof(to));
}

/* PTR (instance) -> SRV (target host) -> A (address). */
static void mdns_answer(const unsigned char *p, int len)
{
	char instances[8][256], targets[8][520], hosts[16][256];
	uint32_t addrs[16];
	int ni = 0, nt = 0, nh = 0, off = 12;
	int records;

	if (len < 12 || !(p[2] & 0x80))
		return;
	records = (p[6] << 8 | p[7]) + (p[8] << 8 | p[9]) + (p[10] << 8 | p[11]);
	for (int q = p[4] << 8 | p[5]; q > 0 && off < len; q--) {
		char skip[256];

		off = dns_name(p, len, off, skip, sizeof(skip));
		if (off < 0)
			return;
		off += 4;
	}
	for (int r = 0; r < records && off + 10 <= len; r++) {
		char name[256], data[256];
		int type, rdlen, rdata;

		off = dns_name(p, len, off, name, sizeof(name));
		if (off < 0 || off + 10 > len)
			return;
		type = p[off] << 8 | p[off + 1];
		rdlen = p[off + 8] << 8 | p[off + 9];
		rdata = off + 10;
		if (rdata + rdlen > len)
			return;
		if (type == 12 && ni < 8 && !strcasecmp(name, "_smb._tcp.local") &&
		    dns_name(p, len, rdata, data, sizeof(data)) > 0)
			snprintf(instances[ni++], sizeof(instances[0]), "%s", data);
		else if (type == 33 && nt < 8 && rdlen > 6 &&
			 dns_name(p, len, rdata + 6, data, sizeof(data)) > 0) {
			/* SRV of an instance: "<instance>\n<target>" */
			snprintf(targets[nt++], sizeof(targets[0]), "%s\n%s", name, data);
		} else if (type == 1 && rdlen == 4 && nh < 16) {
			snprintf(hosts[nh], sizeof(hosts[0]), "%s", name);
			memcpy(&addrs[nh++], p + rdata, 4);
		}
		off = rdata + rdlen;
	}
	for (int i = 0; i < ni; i++) {
		const char *label_end = strstr(instances[i], "._smb._tcp.local");
		size_t label = label_end ? (size_t)(label_end - instances[i]) : strlen(instances[i]);

		for (int t = 0; t < nt; t++) {
			char *nl = strchr(targets[t], '\n');

			if (!nl || strncasecmp(targets[t], instances[i], (size_t)(nl - targets[t])) ||
			    (size_t)(nl - targets[t]) != strlen(instances[i]))
				continue;
			for (int h = 0; h < nh; h++)
				if (!strcasecmp(hosts[h], nl + 1)) {
					struct server *s = server_at(addrs[h]);

					if (s) {
						s->smb = true;
						set_name(s, instances[i], label, 3);
					}
				}
		}
	}
}

/* ------------------------------------------------------------------ */
/* NetBIOS node status                                                 */
/* ------------------------------------------------------------------ */

static void nbstat_send(int fd, uint32_t addr, uint16_t id)
{
	unsigned char q[50];
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(137), .sin_addr.s_addr = addr };
	size_t n = 12;

	memset(q, 0, sizeof(q));
	q[0] = (unsigned char)(id >> 8);
	q[1] = (unsigned char)id;
	q[5] = 1;
	q[n++] = 32;
	/* "*" padded with NULs, first-level encoded (each nibble + 'A'). */
	for (int i = 0; i < 16; i++) {
		unsigned char c = i == 0 ? '*' : 0;

		q[n++] = (unsigned char)('A' + (c >> 4));
		q[n++] = (unsigned char)('A' + (c & 15));
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 0x21; /* NBSTAT */
	q[n++] = 0; q[n++] = 1;    /* IN */
	sendto(fd, q, n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void nbstat_answer(const unsigned char *p, int len, uint32_t from)
{
	char skip[256];
	int off, count;
	struct server *s;

	if (len < 12 || !(p[2] & 0x80) || (p[6] << 8 | p[7]) < 1)
		return;
	off = dns_name(p, len, 12, skip, sizeof(skip));
	if (off < 0 || off + 11 > len || p[off + 1] != 0x21)
		return;
	off += 10;
	count = p[off++];
	for (int i = 0; i < count && off + 18 <= len; i++, off += 18) {
		const unsigned char *e = p + off;
		bool group = e[16] & 0x80;

		/* The Server service: a unique name with suffix 0x20. */
		if (!group && e[15] == 0x20 && (s = server_at(from))) {
			s->smb = true;
			set_name(s, (const char *)e, 15, 2);
		}
	}
}

/* ------------------------------------------------------------------ */
/* WS-Discovery                                                        */
/* ------------------------------------------------------------------ */

static void wsd_probe(int fd)
{
	char msg[1024];
	unsigned char id[16];
	int r = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(3702) };
	int n;

	if (r < 0 || read(r, id, sizeof(id)) != (ssize_t)sizeof(id))
		memset(id, 0x5a, sizeof(id));
	if (r >= 0)
		close(r);
	id[6] = (unsigned char)((id[6] & 0x0f) | 0x40);
	id[8] = (unsigned char)((id[8] & 0x3f) | 0x80);
	n = snprintf(msg, sizeof(msg),
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
		" xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
		" xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
		" xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
		"<soap:Header><wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
		"<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
		"<wsa:MessageID>urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x</wsa:MessageID>"
		"</soap:Header><soap:Body><wsd:Probe><wsd:Types>pub:Computer</wsd:Types></wsd:Probe>"
		"</soap:Body></soap:Envelope>",
		id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
		id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
	inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
	sendto(fd, msg, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void wsd_answer(const char *p, int len, uint32_t from)
{
	struct server *s;

	if (memmem(p, (size_t)len, "ProbeMatch", 10) && (s = server_at(from)))
		s->wsd = true;
}

/* ------------------------------------------------------------------ */
/* discover                                                            */
/* ------------------------------------------------------------------ */

static int udp_socket(uint32_t bind_addr, int multicast_if_addr)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = bind_addr };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	int one = 1, ttl = 1;

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	if (multicast_if_addr) {
		struct in_addr ifa = { .s_addr = bind_addr };

		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	}
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Connect to TCP 445 of every candidate at once; true on success. */
static void check_445(void)
{
	struct pollfd pfd[MAX_SERVERS];
	int idx[MAX_SERVERS], n = 0;
	long long end = now_ms() + PORT_CHECK_MS;

	for (int i = 0; i < server_count; i++) {
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(445), .sin_addr.s_addr = servers[i].addr };
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

		if (fd < 0)
			continue;
		if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
			servers[i].open445 = true;
			close(fd);
			continue;
		}
		if (errno != EINPROGRESS) {
			close(fd);
			continue;
		}
		pfd[n] = (struct pollfd){ .fd = fd, .events = POLLOUT };
		idx[n++] = i;
	}
	while (n > 0) {
		int left = (int)(end - now_ms());

		if (left <= 0 || poll(pfd, (nfds_t)n, left) <= 0)
			break;
		for (int k = 0; k < n; k++) {
			int err = 0;
			socklen_t len = sizeof(err);

			if (!pfd[k].revents)
				continue;
			if (getsockopt(pfd[k].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
				servers[idx[k]].open445 = true;
			close(pfd[k].fd);
			pfd[k] = pfd[--n];
			idx[k] = idx[n];
			k--;
		}
	}
	for (int k = 0; k < n; k++)
		close(pfd[k].fd);
}

static int discover(void)
{
	struct ifaddrs *ifs = NULL;
	struct pollfd pfd[3 * 16];
	int kinds[3 * 16], nfd = 0;
	long long end;

	if (getifaddrs(&ifs) != 0) {
		puts("end=1");
		return 0;
	}
	for (struct ifaddrs *i = ifs; i && own_count < 16; i = i->ifa_next) {
		struct sockaddr_in *a = (struct sockaddr_in *)i->ifa_addr;
		struct sockaddr_in *m = (struct sockaddr_in *)i->ifa_netmask;
		uint32_t host, mask, net;
		int fd;

		if (!a || a->sin_family != AF_INET || !m || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
			continue;
		own[own_count++] = a->sin_addr.s_addr;
		if (nfd + 3 > (int)(sizeof(pfd) / sizeof(pfd[0])))
			continue;
		/* mDNS and WS-Discovery: multicast out of this interface. */
		if ((fd = udp_socket(a->sin_addr.s_addr, 1)) >= 0) {
			mdns_query(fd);
			pfd[nfd] = (struct pollfd){ .fd = fd, .events = POLLIN };
			kinds[nfd++] = 0;
		}
		if ((fd = udp_socket(a->sin_addr.s_addr, 1)) >= 0) {
			wsd_probe(fd);
			pfd[nfd] = (struct pollfd){ .fd = fd, .events = POLLIN };
			kinds[nfd++] = 2;
		}
		/* NetBIOS node status to every address of the /24 (at most). */
		host = ntohl(a->sin_addr.s_addr);
		mask = ntohl(m->sin_addr.s_addr) | 0xffffff00u;
		net = host & mask;
		if ((fd = udp_socket(a->sin_addr.s_addr, 0)) >= 0) {
			for (uint32_t h = net + 1; h < (net | ~mask); h++)
				if (h != host)
					nbstat_send(fd, htonl(h), (uint16_t)h);
			pfd[nfd] = (struct pollfd){ .fd = fd, .events = POLLIN };
			kinds[nfd++] = 1;
		}
	}
	freeifaddrs(ifs);
	end = now_ms() + DISCOVER_MS;
	while (nfd > 0) {
		int left = (int)(end - now_ms());

		if (left <= 0 || poll(pfd, (nfds_t)nfd, left) <= 0)
			break;
		for (int k = 0; k < nfd; k++) {
			unsigned char buf[9000];
			struct sockaddr_in from;
			socklen_t flen = sizeof(from);
			ssize_t n;

			if (!(pfd[k].revents & POLLIN))
				continue;
			while ((n = recvfrom(pfd[k].fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen)) > 0) {
				if (kinds[k] == 0)
					mdns_answer(buf, (int)n);
				else if (kinds[k] == 1)
					nbstat_answer(buf, (int)n, from.sin_addr.s_addr);
				else
					wsd_answer((const char *)buf, (int)n, from.sin_addr.s_addr);
				flen = sizeof(from);
			}
		}
	}
	for (int k = 0; k < nfd; k++)
		close(pfd[k].fd);
	check_445();
	for (int i = 0; i < server_count; i++) {
		const struct server *s = &servers[i];
		char ip[INET_ADDRSTRLEN];

		/* mDNS _smb announces file sharing itself; the others must
		 * accept SMB connections. */
		if (s->rank < 3 && !(s->open445 && (s->smb || s->wsd)))
			continue;
		inet_ntop(AF_INET, &s->addr, ip, sizeof(ip));
		printf("server=%s\t%s\n", s->name[0] ? s->name : ip, ip);
	}
	puts("end=1");
	return 0;
}

/* ------------------------------------------------------------------ */
/* shares                                                              */
/* ------------------------------------------------------------------ */

static bool enum_done;
static int enum_status;

static void enum_cb(struct smb2_context *smb2, int status, void *data, void *priv)
{
	struct srvsvc_NetrShareEnum_rep *rep = data;

	(void)priv;
	enum_status = status;
	if (status == 0) {
		struct srvsvc_SHARE_INFO_1_CONTAINER *c = &rep->ses.ShareInfo.Level1;

		for (uint32_t i = 0; i < c->EntriesRead; i++) {
			const struct srvsvc_SHARE_INFO_1 *s = &c->Buffer->share_info_1[i];
			const char *name = s->netname.utf8 ? s->netname.utf8 : "";
			const char *remark = s->remark.utf8 ? s->remark.utf8 : "";

			if ((s->type & 3) != SHARE_TYPE_DISKTREE || (s->type & SHARE_TYPE_HIDDEN) ||
			    !name[0] || name[strlen(name) - 1] == '$' || strpbrk(name, "\t\n/,\""))
				continue;
			printf("share=%s\t", name);
			for (const char *r = remark; *r; r++)
				putchar(*r == '\t' || *r == '\n' ? ' ' : *r);
			putchar('\n');
		}
		smb2_free_data(smb2, rep);
	}
	enum_done = true;
}

static bool auth_error(uint32_t nt)
{
	return nt == SMB2_STATUS_LOGON_FAILURE || nt == SMB2_STATUS_WRONG_PASSWORD ||
	       nt == SMB2_STATUS_ACCESS_DENIED || nt == SMB2_STATUS_ACCOUNT_DISABLED ||
	       nt == SMB2_STATUS_ACCOUNT_RESTRICTION || nt == SMB2_STATUS_PASSWORD_EXPIRED ||
	       nt == SMB2_STATUS_NETWORK_ACCESS_DENIED || nt == SMB2_STATUS_ACCOUNT_LOCKED_OUT;
}

/* 0 listed, 1 refused login, 2 unreachable, 3 other failure. */
static int list_shares(const char *host, const char *user, const char *password)
{
	struct smb2_context *smb2 = smb2_init_context();
	long long end = now_ms() + SHARES_TIMEOUT * 1000;
	int rc = 3;

	if (!smb2)
		return 3;
	smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(smb2, SHARES_TIMEOUT);
	smb2_set_user(smb2, user);
	if (password)
		smb2_set_password(smb2, password);
	if (smb2_connect_share(smb2, host, "IPC$", user) < 0) {
		uint32_t nt = (uint32_t)smb2_get_nterror(smb2);

		fprintf(stderr, "nuubos-smb-browse: %s as \"%s\": %s (0x%08x)\n", host, user,
			smb2_get_error(smb2), nt);

		rc = auth_error(nt) ? 1 : nt ? 3 : 2;
		smb2_destroy_context(smb2);
		return rc;
	}
	enum_done = false;
	if (smb2_share_enum_async(smb2, SHARE_INFO_1, enum_cb, NULL) == 0) {
		while (!enum_done) {
			struct pollfd pfd = { .fd = smb2_get_fd(smb2), .events = (short)smb2_which_events(smb2) };
			int left = (int)(end - now_ms());

			if (left <= 0 || poll(&pfd, 1, left) <= 0 || smb2_service(smb2, pfd.revents) < 0)
				break;
		}
		if (enum_done)
			rc = enum_status == 0 ? 0 : auth_error((uint32_t)smb2_get_nterror(smb2)) ? 1 : 3;
	}
	smb2_disconnect_share(smb2);
	smb2_destroy_context(smb2);
	return rc;
}

static int shares(const char *host, const char *user)
{
	char password[256];
	bool has_password = false;
	int rc;

	if (fgets(password, sizeof(password), stdin)) {
		password[strcspn(password, "\r\n")] = '\0';
		has_password = true;
	}
	if (user && *user) {
		rc = list_shares(host, user, has_password ? password : "");
	} else {
		rc = list_shares(host, "", NULL);           /* anonymous */
		if (rc == 1 || rc == 3)
			rc = list_shares(host, "guest", ""); /* guest */
	}
	if (rc == 0)
		puts("end=1");
	else
		puts(rc == 1 ? "ERR auth" : rc == 2 ? "ERR unreachable" : "ERR failed");
	return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 2 && !strcmp(argv[1], "discover"))
		return discover();
	if (argc >= 3 && !strcmp(argv[1], "shares"))
		return shares(argv[2], argc > 3 ? argv[3] : "");
	fprintf(stderr, "Usage: nuubos-smb-browse discover | shares HOST [USER] (password on stdin)\n");
	return 2;
}
