#include "BBEvents.h"

#include "BBJSON.h"

static bool bb_events_string_equal(const char *left, const char *right)
{
    size_t i = 0;
    if (!left || !right) return false;
    while (left[i] && right[i]) {
        if (left[i] != right[i]) return false;
        i++;
    }
    return left[i] == right[i];
}

bool bb_events_name_supported(const char *eventName)
{
    /* typing-indicator joins this list only once the native selector is
     * proven on the device; group mutation events stay out until then. */
    static const char *const supported[] = {
        "new-message",
        "updated-message",
        "message-send-error",
        "chat-read-status-changed",
        "hello-world"
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (bb_events_string_equal(eventName, supported[i])) return true;
    }
    return false;
}

size_t bb_events_broadcast(const BBEIOSessionTable *sessions,
                           const char *eventName,
                           const char *payloadJSON,
                           size_t payloadLength)
{
    BBJSONValue payload;
    size_t delivered = 0;
    if (!sessions || !sessions->sessions || !bb_events_name_supported(eventName))
        return 0;
    if (!payloadJSON || !payloadLength ||
        !bb_json_parse(payloadJSON, payloadLength, &payload)) return 0;
    for (size_t i = 0; i < sessions->capacity; i++) {
        BBEIOSession *session = &sessions->sessions[i];
        if (session->state != BBEIOSessionStateConnected) continue;
        if (bb_eio_session_emit(session, eventName, payloadJSON, payloadLength))
            delivered++;
    }
    return delivered;
}
