#ifndef BB_TRACE_H
#define BB_TRACE_H

/* Package version, injected by the Makefile from the control file. */
#ifndef BB_BUILD_VERSION
#define BB_BUILD_VERSION "dev"
#endif

/* Count-only diagnostics: a fixed event name per line, never a payload,
 * address, guid, or credential. Files: /tmp/bbdiag-sb.log (SpringBoard)
 * and /tmp/bbdiag-sms.log (MobileSMS), capped at 64 KB. */
typedef enum { BBTraceSpringBoard, BBTraceMobileSMS } BBTraceProcess;
typedef enum {
    BBTraceLoaded,
    BBTraceCenterUnavailable,
    BBTraceCenterReady,
    BBTraceReceiverReady,
    /* MobileSMS bridge. */
    BBTraceReplySubmitting,
    BBTraceReplySubmitted,
    BBTraceReplyFailed,
    BBTraceEnumerationException,
    BBTraceSendDispatched,
    BBTraceSendFailed,
    BBTraceNewChatResolving,
    BBTraceNewChatResolved,
    BBTraceNewChatFailed,
    /* SpringBoard transport. */
    BBTraceTransportListening,
    BBTraceTransportStartFailed,
    BBTraceConnectionAccepted,
    BBTraceConnectionRefused,
    BBTraceConnectionClosed,
    BBTraceOutputFailed,
    BBTraceRequestDispatched,
    BBTraceBridgeReplyReceived,
    BBTraceBridgeReplyRejected,
    BBTraceBridgeEventReceived,
    BBTraceRequestExpired,
    BBTraceSessionOpened,
    BBTraceSessionClosed,
    BBTraceEventCount
} BBTraceEvent;

#ifdef __cplusplus
extern "C" {
#endif
void bb_trace(BBTraceProcess process, BBTraceEvent event);
#ifdef __cplusplus
}
#endif
#endif
