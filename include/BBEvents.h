#ifndef BB_EVENTS_H
#define BB_EVENTS_H

#include <stdbool.h>
#include <stddef.h>

#include "BBEngineIO.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Server-push events from the MobileSMS bridge to every connected client.
 * This path is deliberately independent of BBRequestTable: an event carries
 * no requestID, is never matched against a pending request, and cannot
 * complete or overwrite one. */

#define BB_EVENT_MAX_NAME_BYTES 48U

/* Event names the first bridge profile is allowed to emit. Anything else
 * is refused so unproven event families are never advertised. */
bool bb_events_name_supported(const char *eventName);

/* Emit 42["<eventName>",<payloadJSON>] to every session in the connected
 * state. payloadJSON must be valid JSON. Returns the number of sessions the
 * event was queued or sent to; unsupported names or invalid payloads
 * deliver to none. */
size_t bb_events_broadcast(const BBEIOSessionTable *sessions,
                           const char *eventName,
                           const char *payloadJSON,
                           size_t payloadLength);

#ifdef __cplusplus
}
#endif

#endif
