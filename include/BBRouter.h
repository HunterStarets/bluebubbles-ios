#ifndef BB_ROUTER_H
#define BB_ROUTER_H

#include <stdbool.h>
#include <stddef.h>

#include "BBConnection.h"
#include "BBEngineIO.h"
#include "BBHTTP.h"
#include "BBMultipart.h"
#include "BBRequestTable.h"
#include "BBResponse.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BB_ROUTER_MAX_PASSWORD_BYTES 256U
#define BB_ROUTER_MAX_LOCAL_ADDRESSES 4U
#define BB_ROUTER_MAX_UPLOAD_BYTES (20U * 1024U * 1024U)
#define BB_ROUTER_UPLOAD_PATH_BYTES 256U

typedef struct BBRouterUpload BBRouterUpload;

/* Where a streamed attachment goes. The owner opens a private file when
 * the file part starts (recording its path in upload->uploadPath, which
 * the bridge receives), appends every run, and closes it: keep=false
 * means delete (validation failed, or the connection went away). */
typedef struct {
    bool (*openFile)(void *context, BBRouterUpload *upload,
                     const char *filename, const char *contentType);
    bool (*writeFile)(void *context, BBRouterUpload *upload,
                      const unsigned char *bytes, size_t length);
    void (*closeFile)(void *context, BBRouterUpload *upload, bool keep);
    void *context;
} BBRouterUploadSink;

/* State of one streamed POST /message/attachment. Owned by the caller
 * (one per connection); opaque apart from uploadPath and ownerHandle. */
struct BBRouterUpload {
    BBMultipart multipart;
    const struct BBRouterConfig *config;
    BBConnection *connection;
    char chatGuid[256];
    char tempGuid[128];
    char name[256];
    char fileContentType[BB_MULTIPART_MAX_CONTENT_TYPE_BYTES];
    char uploadPath[BB_ROUTER_UPLOAD_PATH_BYTES];  /* set by openFile */
    void *ownerHandle;                             /* free for the owner */
    uint64_t fileBytes;
    bool fileOpen;
    bool fileSeen;
    int field;                                     /* text field being read */
    size_t fieldLength;
    /* A claimed body that cannot be accepted is drained, then answered. */
    bool rejected;
    int rejectStatus;
    const char *rejectMessage;
    const char *rejectType;
    const char *rejectDetail;
};

/* ServerMetadataResponse inputs. The router always advertises the
 * conservative profile (server_version 0.2.0, private_api false) so the
 * unmodified client does not enable contracts iOS 9 cannot satisfy. */
typedef struct {
    const char *computerID;
    const char *osVersion;
    const char *serverVersion;
    const char *proxyService;
    const char *localIPv4[BB_ROUTER_MAX_LOCAL_ADDRESSES];
    size_t localIPv4Count;
} BBServerMetadata;

typedef struct BBRouterConfig {
    /* Configured server password. NULL or empty means authentication is
     * unavailable and every authenticated route answers 500. */
    const char *password;
    BBServerMetadata metadata;
    /* Socket.IO support. NULL sessions means /socket.io/ answers 404. */
    BBEIOSessionTable *sessions;
    BBEIOCallbacks sessionCallbacks;
    /* Bridge request correlation. NULL means every bridge-backed route
     * answers 503; the router then never leaves a request pending. */
    BBRequestTable *requests;
    uint64_t (*nowMs)(void *context);
    void *clockContext;
    /* Attachment upload sink. NULL openFile means uploads answer 501. */
    BBRouterUploadSink uploads;
} BBRouterConfig;

/* Constant-time comparison of the guid/password/token query aliases against
 * the configured password. Equal-length inputs are compared fully; unequal
 * lengths fail without comparing bytes. */
bool bb_router_authenticate(const char *query, const char *password);

/* Route one parsed request. Generated bodies are written into scratch and
 * response->body points there. Every well-formed call yields a response;
 * false only means invalid arguments or an unusably small scratch buffer.
 *
 * Bridge-backed routes validate their input, build the IPC argument JSON,
 * claim a BBRequest, and return a pending response whose handlerContext is
 * that request. The owner dispatches it from the connection's pending
 * callback (bb_request_table_dispatch) and completes it when the bridge
 * replies. connection may be NULL only when config->requests is NULL. */
bool bb_router_route(const BBRouterConfig *config,
                     BBConnection *connection,
                     const BBHTTPRequest *request,
                     const unsigned char *body,
                     size_t bodyLength,
                     char *scratch,
                     size_t scratchCapacity,
                     BBHTTPResponse *response);

/* Body streaming for POST /message/attachment (multipart/form-data with
 * the file part "attachment" plus chatGuid/tempGuid/name fields). Called
 * from the connection's streamBegin: returns true when the router claims
 * the body, after which every body byte goes to bb_router_upload_data
 * and bb_router_upload_end produces the response (a pending bridge
 * request "message.attachment" carrying uploadPath, or an error). A
 * matching route that cannot be accepted (auth, size, content type) is
 * still claimed: the body is drained so the client receives the proper
 * status. Unclaimed bodies are buffered and routed as usual. */
bool bb_router_upload_begin(const BBRouterConfig *config,
                            BBConnection *connection,
                            const BBHTTPRequest *request,
                            BBRouterUpload *upload);
/* False refuses the upload (malformed multipart, sink failure). */
bool bb_router_upload_data(BBRouterUpload *upload,
                           const unsigned char *bytes, size_t length);
bool bb_router_upload_end(BBRouterUpload *upload,
                          char *scratch, size_t scratchCapacity,
                          BBHTTPResponse *response);
/* The connection went away or refused: release the file. */
void bb_router_upload_abort(BBRouterUpload *upload);

/* Answer one Socket.IO client event. get-server-metadata is acknowledged
 * immediately; get-chats, get-chat, get-chat-messages, get-messages, and
 * get-attachment, and start-chat become dispatched bridge requests acknowledged on reply;
 * anything else gets a capability error. Events without an ACK id are
 * dropped. scratch must not alias session buffers. */
bool bb_router_socket_event(const BBRouterConfig *config,
                            BBEIOSession *session,
                            const unsigned char *packet,
                            size_t packetLength,
                            const BBSIOPacket *parsed,
                            char *scratch,
                            size_t scratchCapacity);

#ifdef __cplusplus
}
#endif

#endif
