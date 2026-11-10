/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Google Cast control channel (CASTV2): TLS to the receiver (port 8009),
 * length-prefixed CastMessage protobufs carrying JSON payloads.
 *
 * The receiver's certificate is not verified: Cast receivers present
 * self-signed device certificates whose check (deviceauth namespace) needs
 * Google's root certificates; like other open senders, the console trusts
 * the device the user picked on the local network.
 */
#ifndef NUUBOS_CAST_CASTV2_H
#define NUUBOS_CAST_CASTV2_H

#include <stdbool.h>
#include <stddef.h>

#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA "urn:x-cast:com.google.cast.media"
#define NS_WEBRTC "urn:x-cast:com.google.cast.webrtc"

struct castv2;

struct castv2_message {
	char source[96];
	char destination[96];
	char ns[96];
	char *payload; /* NUL terminated UTF-8, owned by the channel */
};

struct castv2 *castv2_connect(const char *address, int port, int timeout_ms);
void castv2_close(struct castv2 *ch);
int castv2_fd(const struct castv2 *ch);
int castv2_send(struct castv2 *ch, const char *source, const char *destination, const char *ns,
		const char *payload);
/* After POLLIN: reads what is available. Returns -1 when the connection
 * ended. */
int castv2_read(struct castv2 *ch);
/* Next complete message (valid until the next call), false when none. */
bool castv2_next(struct castv2 *ch, struct castv2_message *out);
/* TLS may hold decrypted bytes the socket no longer signals. */
bool castv2_pending(const struct castv2 *ch);

#endif
