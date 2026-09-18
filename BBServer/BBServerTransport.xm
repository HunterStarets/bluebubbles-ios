#import "BBServerTransport.h"

#import <Foundation/NSDistributedNotificationCenter.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CFNetwork/CFSocketStream.h>
#import <Security/SecImportExport.h>
#import <Security/SecRandom.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#import "BBTrace.h"

#include "BBConnection.h"
#include "BBEngineIO.h"
#include "BBEvents.h"
#include "BBRequestTable.h"
#include "BBRouter.h"

/* Every protocol decision is made by the C modules. This file only moves
 * bytes between sockets and those modules, supplies time and identifiers,
 * and exchanges dictionaries with the MobileSMS bridge. It uses no
 * Objective-C exceptions, no C++, and no aggregate initializers that could
 * lower into memset. */

NSString * const BBServerBridgeRequestNotificationName = @"com.bluebubbles.bridge.request";
NSString * const BBServerBridgeReplyMessageName = @"reply";
NSString * const BBServerBridgeEventMessageName = @"event";

#define BB_SERVER_DEFAULT_PORT 1234
#define BB_SERVER_MAX_CONNECTIONS 8
#define BB_SERVER_INPUT_BYTES (64U * 1024U)
#define BB_SERVER_SCRATCH_BYTES (256U * 1024U)
#define BB_SERVER_MAX_PENDING_OUTPUT_BYTES (24U * 1024U * 1024U)
/* Largest attachment the transport reads fully into memory for a download.
 * iOS 9 media is well under this; larger transfers get a truthful error
 * until a streamed/ranged path exists. */
#define BB_SERVER_MAX_DOWNLOAD_BYTES (20U * 1024U * 1024U)
/* Staged attachment uploads: private files the bridge reads and deletes. */
#define BB_SERVER_UPLOAD_PATH_PREFIX "/tmp/bbupload-"
#define BB_SERVER_SESSION_SLOTS 4U
#define BB_SERVER_FRAMES_BYTES (16U * 1024U)
#define BB_SERVER_MESSAGE_BYTES (64U * 1024U)
#define BB_SERVER_OUTBOUND_BYTES (128U * 1024U)
#define BB_SERVER_REQUEST_SLOTS 16U
#define BB_SERVER_REQUEST_SCRATCH_BYTES (256U * 1024U)
#define BB_SERVER_EVENT_SCRATCH_BYTES (64U * 1024U)
#define BB_SERVER_TICK_SECONDS 5.0
/* A connection that has sent no complete request for this long is dropped
 * (a stalled client, a scanner, a TLS handshake against the plain port).
 * Upgraded WebSocket sessions have their own heartbeat instead. */
#define BB_SERVER_IDLE_TIMEOUT_MS UINT64_C(60000)
#define BB_SERVER_MAX_LOCAL_ADDRESSES 4U
#define BB_SERVER_PASSWORD_BYTES 256U

/* Settings live in the preferences domain the Settings pane edits. The
 * pane posts BBServerSettingsChangedNotification after every change; the
 * transport reloads and, when the listener configuration changed, reopens
 * the listener without a respring. */
#define BB_SERVER_SETTINGS_DOMAIN CFSTR("com.hunterstarets.bluebubbles-ios")
#define BB_SERVER_SETTINGS_NOTIFICATION CFSTR("com.hunterstarets.bluebubbles-ios/settings")
#define BB_SERVER_GENERATED_PASSWORD_LENGTH 12
/* Optional TLS: off unless the preference asks for it and the identity
 * loads. The stock BlueBubbles server is plain HTTP on the LAN too. */
static NSString * const BBServerDefaultCertificatePath =
    @"/var/mobile/Library/BlueBubbles/server.p12";

@class BBServerTransport;

static uint64_t BBServerNowMs(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u;
}

/* ---- connection ------------------------------------------------------ */

@interface BBServerConnection : NSObject <NSStreamDelegate> {
@public
    BBConnection _state;
    unsigned char *_input;
    char *_scratch;
    NSInputStream *_inputStream;
    NSOutputStream *_outputStream;
    NSMutableData *_pendingOutput;
    BBServerTransport *_transport;   /* unretained owner */
    BBEIOSession *_session;          /* WebSocket session after a 101 */
    BOOL _closed;
    BOOL _closeWhenDrained;
    BBRouterUpload _upload;          /* streamed POST /message/attachment */
    int _uploadFd;                   /* -1 when no file is staged */
    uint64_t _lastActivityMs;        /* last read or write, for the idle timeout */
}
- (id)initWithNativeSocket:(CFSocketNativeHandle)nativeSocket
              certificates:(NSArray *)certificates
                 transport:(BBServerTransport *)transport;
- (void)open;
- (void)close;
- (BOOL)appendOutput:(const unsigned char *)bytes length:(size_t)length;
- (void)writeAvailable;
@end

/* ---- transport ------------------------------------------------------- */

@interface BBServerTransport () {
@public
    NSThread *_thread;
    BOOL _ready;
    CFSocketRef _listener;
    CFRunLoopSourceRef _listenerSource;
    NSArray *_certificates;
    NSMutableArray *_connections;
    NSTimer *_tickTimer;
    char _password[BB_SERVER_PASSWORD_BYTES];
    BOOL _enabled;
    unsigned _port;
    BOOL _tlsWanted;
    NSString *_certificatePath;
    NSString *_certificatePassphrase;
    char _localAddresses[BB_SERVER_MAX_LOCAL_ADDRESSES][INET_ADDRSTRLEN];
    BBRouterConfig _config;
    BBEIOSession _sessions[BB_SERVER_SESSION_SLOTS];
    unsigned char *_sessionFrames[BB_SERVER_SESSION_SLOTS];
    unsigned char *_sessionMessage[BB_SERVER_SESSION_SLOTS];
    unsigned char *_sessionOutbound[BB_SERVER_SESSION_SLOTS];
    BBServerConnection *_sessionOwners[BB_SERVER_SESSION_SLOTS]; /* unretained */
    BBEIOSessionTable _sessionTable;
    BBRequest _requests[BB_SERVER_REQUEST_SLOTS];
    BBRequestTable _requestTable;
    char *_requestScratch;
    char *_eventScratch;
}
- (void)acceptNativeSocket:(CFSocketNativeHandle)nativeSocket;
- (void)connectionDidClose:(BBServerConnection *)connection;
- (BOOL)openListener;
- (void)closeListener;
- (void)settingsDidChange;
- (BBRouterConfig *)routerConfig;
- (BBRequestTable *)requestTable;
- (BBEIOSessionTable *)sessionTable;
- (void)sessionDidOpen:(BBEIOSession *)session onConnection:(BBServerConnection *)connection;
- (void)completePollingWaiterForSession:(BBEIOSession *)session;
- (void)completeDownload:(uint64_t)requestID path:(NSString *)path contentType:(id)contentTypeValue ephemeral:(BOOL)ephemeral;
- (void)failDownload:(uint64_t)requestID status:(int)status detail:(const char *)detail;
@end

/* ---- BBConnection callbacks (context: BBServerConnection) ------------- */

static bool BBServerHandleRequest(void *context, BBConnection *state,
                                  const BBHTTPRequest *request,
                                  const unsigned char *body, size_t bodyLength,
                                  char *scratch, size_t scratchCapacity,
                                  BBHTTPResponse *response)
{
    BBServerConnection *connection = (BBServerConnection *)context;
    return bb_router_route([connection->_transport routerConfig], state, request,
                           body, bodyLength, scratch, scratchCapacity, response);
}

static bool BBServerEmit(void *context, BBConnection *state,
                         const unsigned char *bytes, size_t length)
{
    (void)state;
    return [(BBServerConnection *)context appendOutput:bytes length:length];
}

static void BBServerUpgradedData(void *context, BBConnection *state,
                                 const unsigned char *bytes, size_t length)
{
    BBServerConnection *connection = (BBServerConnection *)context;
    (void)state;
    if (!connection->_session) return;
    (void)bb_eio_session_receive_bytes(connection->_session, bytes, length,
                                       BBServerNowMs());
}

static void BBServerUpgraded(void *context, BBConnection *state, void *handlerContext)
{
    BBServerConnection *connection = (BBServerConnection *)context;
    (void)state;
    [connection->_transport sessionDidOpen:(BBEIOSession *)handlerContext
                              onConnection:connection];
}

/* ---- attachment upload streaming ------------------------------------- */

static bool BBServerStreamBegin(void *context, BBConnection *state,
                                const BBHTTPRequest *request, void **streamContext)
{
    BBServerConnection *connection = (BBServerConnection *)context;
    if (!bb_router_upload_begin([connection->_transport routerConfig], state, request,
                                &connection->_upload)) return false;
    connection->_upload.ownerHandle = connection;   /* the sink finds its fd here */
    *streamContext = &connection->_upload;
    return true;
}

static bool BBServerStreamData(void *context, BBConnection *state, void *streamContext,
                               const unsigned char *bytes, size_t length)
{
    (void)context;
    (void)state;
    return bb_router_upload_data((BBRouterUpload *)streamContext, bytes, length);
}

static bool BBServerStreamEnd(void *context, BBConnection *state, void *streamContext,
                              char *scratch, size_t scratchCapacity, BBHTTPResponse *response)
{
    (void)context;
    (void)state;
    return bb_router_upload_end((BBRouterUpload *)streamContext, scratch, scratchCapacity, response);
}

static void BBServerStreamAbort(void *context, BBConnection *state, void *streamContext)
{
    (void)context;
    (void)state;
    bb_router_upload_abort((BBRouterUpload *)streamContext);
}

/* Upload sink (config context: BBServerTransport). The file is created
 * O_EXCL with mode 0600 under /tmp; the bridge (same user) opens it by
 * path and unlinks it once the send is dispatched. */
static bool BBServerUploadOpen(void *context, BBRouterUpload *upload,
                               const char *filename, const char *contentType)
{
    static unsigned counter;
    BBServerConnection *connection = (BBServerConnection *)upload->ownerHandle;
    (void)context;
    (void)filename;
    (void)contentType;
    if (!connection || connection->_uploadFd >= 0) return false;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int fd;
        int written = snprintf(upload->uploadPath, sizeof(upload->uploadPath),
                               BB_SERVER_UPLOAD_PATH_PREFIX "%ld-%llu-%u",
                               (long)getpid(), (unsigned long long)BBServerNowMs(), ++counter);
        if (written <= 0 || (size_t)written >= sizeof(upload->uploadPath)) return false;
        fd = open(upload->uploadPath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            connection->_uploadFd = fd;
            return true;
        }
        if (errno != EEXIST) break;
    }
    upload->uploadPath[0] = '\0';
    return false;
}

static bool BBServerUploadWrite(void *context, BBRouterUpload *upload,
                                const unsigned char *bytes, size_t length)
{
    BBServerConnection *connection = (BBServerConnection *)upload->ownerHandle;
    (void)context;
    if (!connection || connection->_uploadFd < 0) return false;
    while (length) {
        ssize_t count = write(connection->_uploadFd, bytes, length);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (!count) return false;
        bytes += (size_t)count;
        length -= (size_t)count;
    }
    return true;
}

static void BBServerUploadClose(void *context, BBRouterUpload *upload, bool keep)
{
    BBServerConnection *connection = (BBServerConnection *)upload->ownerHandle;
    (void)context;
    if (!connection || connection->_uploadFd < 0) return;
    close(connection->_uploadFd);
    connection->_uploadFd = -1;
    if (!keep && upload->uploadPath[0]) unlink(upload->uploadPath);
}

static void BBServerPending(void *context, BBConnection *state, void *handlerContext)
{
    BBServerConnection *connection = (BBServerConnection *)context;
    BBRequestTable *table = [connection->_transport requestTable];
    BBRequest *first = table->requests;
    BBRequest *last = table->requests + table->capacity;
    (void)state;
    if (!handlerContext) return;
    if ((BBRequest *)handlerContext >= first && (BBRequest *)handlerContext < last) {
        /* A bridge request: the connection is now pending, so it may leave. */
        (void)bb_request_table_dispatch(table, (BBRequest *)handlerContext);
        return;
    }
    /* Otherwise a held polling GET on an Engine.IO session. */
    ((BBEIOSession *)handlerContext)->pollingWaiter = connection;
}

/* ---- Engine.IO session callbacks (context: BBServerTransport) --------- */

static size_t BBServerSlotIndex(BBServerTransport *transport, const BBEIOSession *session)
{
    return (size_t)(session - [transport sessionTable]->sessions);
}

static bool BBServerSendFrame(void *context, BBEIOSession *session,
                              const unsigned char *bytes, size_t length)
{
    BBServerTransport *transport = (BBServerTransport *)context;
    BBServerConnection *connection = nil;
    size_t index = BBServerSlotIndex(transport, session);
    if (index >= BB_SERVER_SESSION_SLOTS) return false;
    connection = ((BBServerTransport *)transport)->_sessionOwners[index];
    if (!connection) return false;
    return bb_connection_send_upgraded(&connection->_state, bytes, length);
}

static void BBServerSessionEvent(void *context, BBEIOSession *session,
                                 const unsigned char *packet, size_t packetLength,
                                 const BBSIOPacket *parsed)
{
    BBServerTransport *transport = (BBServerTransport *)context;
    (void)bb_router_socket_event([transport routerConfig], session, packet,
                                 packetLength, parsed, transport->_eventScratch,
                                 BB_SERVER_EVENT_SCRATCH_BYTES);
}

static void BBServerPollingReady(void *context, BBEIOSession *session)
{
    [(BBServerTransport *)context completePollingWaiterForSession:session];
}

static void BBServerSessionClosed(void *context, BBEIOSession *session,
                                  BBEIOCloseReason reason)
{
    BBServerTransport *transport = (BBServerTransport *)context;
    size_t index = BBServerSlotIndex(transport, session);
    BBServerConnection *owner = nil;
    (void)reason;
    bb_trace(BBTraceSpringBoard, BBTraceSessionClosed);
    bb_request_table_cancel_session([transport requestTable], session);
    if (session->transport == BBEIOTransportPolling) {
        [transport completePollingWaiterForSession:session];
    }
    if (index < BB_SERVER_SESSION_SLOTS) {
        owner = transport->_sessionOwners[index];
        transport->_sessionOwners[index] = nil;
    }
    bb_eio_table_release(session);
    if (owner) {
        owner->_session = NULL;
        [owner close];
    }
}

/* ---- request table dispatch (context: BBServerTransport) -------------- */

static bool BBServerDispatch(void *context, BBRequestTable *table, const BBRequest *request)
{
    NSString *requestID;
    NSString *operation;
    NSString *arguments;
    NSDictionary *userInfo;
    (void)context;
    (void)table;
    requestID = [NSString stringWithFormat:@"%llu", (unsigned long long)request->requestID];
    operation = [NSString stringWithUTF8String:request->operation];
    arguments = [[[NSString alloc] initWithBytes:request->argumentsJSON
                                          length:request->argumentsLength
                                        encoding:NSUTF8StringEncoding] autorelease];
    if (!requestID || !operation || !arguments) return false;
    userInfo = [NSDictionary dictionaryWithObjectsAndKeys:
                requestID, @"requestID",
                operation, @"operation",
                arguments, @"argumentsJSON",
                @"envelope", @"replyKind",
                nil];
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:BBServerBridgeRequestNotificationName
                      object:nil userInfo:userInfo deliverImmediately:YES];
    bb_trace(BBTraceSpringBoard, BBTraceRequestDispatched);
    return true;
}

static bool BBServerGenerateSid(void *context, char *out, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char random[16];
    (void)context;
    if (!out || capacity < 33) return false;
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        out[i * 2] = digits[random[i] >> 4];
        out[i * 2 + 1] = digits[random[i] & 0x0f];
    }
    out[32] = '\0';
    return true;
}

static uint64_t BBServerClock(void *context)
{
    (void)context;
    return BBServerNowMs();
}

static void BBServerAcceptCallback(CFSocketRef socket,
                                   CFSocketCallBackType callbackType,
                                   CFDataRef address,
                                   const void *data,
                                   void *info)
{
    (void)socket;
    (void)address;
    if (callbackType != kCFSocketAcceptCallBack || !data || !info) return;
    CFSocketNativeHandle nativeSocket = *(const CFSocketNativeHandle *)data;
    [(BBServerTransport *)info acceptNativeSocket:nativeSocket];
}

/* ---- settings ---------------------------------------------------------- */

static id BBServerSettingValue(CFStringRef key)
{
    CFPropertyListRef value = CFPreferencesCopyAppValue(key, BB_SERVER_SETTINGS_DOMAIN);
    return value ? [(id)value autorelease] : nil;
}

static BOOL BBServerSettingBool(CFStringRef key, BOOL fallback)
{
    id value = BBServerSettingValue(key);
    return [value respondsToSelector:@selector(boolValue)] ? [value boolValue] : fallback;
}

static NSString *BBServerSettingString(CFStringRef key)
{
    id value = BBServerSettingValue(key);
    return [value isKindOfClass:[NSString class]] ? value : nil;
}

static void BBServerSetSetting(CFStringRef key, id value)
{
    CFPreferencesSetAppValue(key, (CFPropertyListRef)value, BB_SERVER_SETTINGS_DOMAIN);
    CFPreferencesAppSynchronize(BB_SERVER_SETTINGS_DOMAIN);
}

/* A fresh password from an unambiguous alphabet (no 0/O, 1/l/I). */
static NSString *BBServerGeneratePassword(void)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    const unsigned alphabetLength = sizeof(alphabet) - 1;
    uint8_t random[BB_SERVER_GENERATED_PASSWORD_LENGTH];
    char password[BB_SERVER_GENERATED_PASSWORD_LENGTH + 1];
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof(random), random) != 0) {
        for (unsigned i = 0; i < sizeof(random); i++) random[i] = (uint8_t)arc4random_uniform(256);
    }
    for (unsigned i = 0; i < sizeof(random); i++) password[i] = alphabet[random[i] % alphabetLength];
    password[sizeof(random)] = '\0';
    return [NSString stringWithUTF8String:password];
}

/* Import a PKCS#12 identity for the TLS listener. nil when the file is
 * absent or unreadable; the caller then serves plain HTTP. */
static NSArray *BBServerImportCertificates(NSString *path, NSString *passphrase)
{
    NSData *pkcs12 = [path length] ? [NSData dataWithContentsOfFile:path] : nil;
    NSDictionary *options;
    CFArrayRef importedItems = NULL;
    CFDictionaryRef item;
    SecIdentityRef identity;
    NSArray *certificates = nil;
    if (!pkcs12) return nil;
    options = [NSDictionary dictionaryWithObject:passphrase ?: @""
                                          forKey:(id)kSecImportExportPassphrase];
    if (SecPKCS12Import((CFDataRef)pkcs12, (CFDictionaryRef)options, &importedItems) != errSecSuccess ||
        !importedItems || CFArrayGetCount(importedItems) == 0) {
        if (importedItems) CFRelease(importedItems);
        return nil;
    }
    item = (CFDictionaryRef)CFArrayGetValueAtIndex(importedItems, 0);
    identity = item ? (SecIdentityRef)CFDictionaryGetValue(item, kSecImportItemIdentity) : NULL;
    if (identity) certificates = [NSArray arrayWithObject:(id)identity];
    CFRelease(importedItems);
    return certificates;
}

/* ---- connection implementation --------------------------------------- */

@implementation BBServerConnection

- (id)initWithNativeSocket:(CFSocketNativeHandle)nativeSocket
              certificates:(NSArray *)certificates
                 transport:(BBServerTransport *)transport
{
    BBConnectionCallbacks callbacks;
    CFReadStreamRef readStream = NULL;
    CFWriteStreamRef writeStream = NULL;

    self = [super init];
    if (!self) return nil;
    _transport = transport;
    _pendingOutput = [[NSMutableData alloc] init];
    _input = (unsigned char *)malloc(BB_SERVER_INPUT_BYTES);
    _scratch = (char *)malloc(BB_SERVER_SCRATCH_BYTES);
    if (!_input || !_scratch) {
        [self release];
        return nil;
    }
    callbacks.handleRequest = BBServerHandleRequest;
    callbacks.emit = BBServerEmit;
    callbacks.upgradedData = BBServerUpgradedData;
    callbacks.upgraded = BBServerUpgraded;
    callbacks.pending = BBServerPending;
    callbacks.streamBegin = BBServerStreamBegin;
    callbacks.streamData = BBServerStreamData;
    callbacks.streamEnd = BBServerStreamEnd;
    callbacks.streamAbort = BBServerStreamAbort;
    callbacks.context = self;
    _uploadFd = -1;
    _lastActivityMs = BBServerNowMs();
    if (!bb_connection_init(&_state, _input, BB_SERVER_INPUT_BYTES, _scratch,
                            BB_SERVER_SCRATCH_BYTES, &callbacks)) {
        [self release];
        return nil;
    }

    CFStreamCreatePairWithSocket(kCFAllocatorDefault, nativeSocket,
                                 &readStream, &writeStream);
    if (!readStream || !writeStream) {
        if (readStream) CFRelease(readStream);
        if (writeStream) CFRelease(writeStream);
        [self release];
        return nil;
    }
    _inputStream = [(NSInputStream *)readStream retain];
    _outputStream = [(NSOutputStream *)writeStream retain];
    CFRelease(readStream);
    CFRelease(writeStream);

    if (certificates) {
        NSDictionary *sslSettings = [NSDictionary dictionaryWithObjectsAndKeys:
            (id)kCFBooleanTrue, (id)kCFStreamSSLIsServer,
            certificates, (id)kCFStreamSSLCertificates,
            (id)kCFBooleanFalse, (id)kCFStreamSSLValidatesCertificateChain,
            nil];
        [_inputStream setProperty:sslSettings forKey:(id)kCFStreamPropertySSLSettings];
        [_outputStream setProperty:sslSettings forKey:(id)kCFStreamPropertySSLSettings];
    }
    [_inputStream setProperty:(id)kCFBooleanTrue
                       forKey:(id)kCFStreamPropertyShouldCloseNativeSocket];
    return self;
}

- (void)open
{
    if (_closed || !_inputStream || !_outputStream) return;
    [_inputStream setDelegate:self];
    [_outputStream setDelegate:self];
    [_inputStream scheduleInRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    [_outputStream scheduleInRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    [_inputStream open];
    [_outputStream open];
}

- (void)close
{
    BBServerTransport *transport;
    BBEIOSession *session;
    if (_closed) return;
    _closed = YES;
    bb_trace(BBTraceSpringBoard, BBTraceConnectionClosed);
    [_inputStream setDelegate:nil];
    [_outputStream setDelegate:nil];
    [_inputStream close];
    [_outputStream close];
    [_inputStream removeFromRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    [_outputStream removeFromRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];

    transport = _transport;
    session = _session;
    _session = NULL;
    /* A half-received upload dies with its socket (file deleted). */
    bb_connection_abort(&_state);
    if (_uploadFd >= 0) {
        close(_uploadFd);
        _uploadFd = -1;
        if (_upload.uploadPath[0]) unlink(_upload.uploadPath);
    }
    if (transport) {
        /* Nothing the bridge still owes this socket can be delivered. */
        bb_request_table_cancel_connection([transport requestTable], &_state);
        /* A WebSocket session dies with its socket. */
        if (session) bb_eio_session_close(session, BBEIOCloseTransportFailed);
        /* A held polling GET on any session must forget this connection. */
        {
            BBEIOSessionTable *table = [transport sessionTable];
            for (size_t i = 0; i < table->capacity; i++) {
                if (table->sessions[i].pollingWaiter == self)
                    table->sessions[i].pollingWaiter = NULL;
            }
        }
        /* Removal from the owner array is the last reference. Defer it to
         * the run loop so a close triggered from inside a C module call
         * (an emit failure during bb_connection_complete or a heartbeat
         * frame) never frees this object while that call is still using
         * its state. performSelector retains self until it fires. */
        [transport performSelector:@selector(connectionDidClose:)
                        withObject:self afterDelay:0.0];
    }
}

- (BOOL)appendOutput:(const unsigned char *)bytes length:(size_t)length
{
    if (_closed || !_outputStream) return NO;
    if (length > BB_SERVER_MAX_PENDING_OUTPUT_BYTES - [_pendingOutput length]) {
        bb_trace(BBTraceSpringBoard, BBTraceOutputFailed);
        return NO;
    }
    [_pendingOutput appendBytes:bytes length:length];
    [self writeAvailable];
    return YES;
}

- (void)writeAvailable
{
    const uint8_t *bytes;
    NSUInteger length;
    NSUInteger offset = 0;
    if (_closed) return;
    bytes = (const uint8_t *)[_pendingOutput bytes];
    length = [_pendingOutput length];
    while (offset < length && [_outputStream hasSpaceAvailable]) {
        NSInteger written = [_outputStream write:bytes + offset maxLength:length - offset];
        if (written < 0) {
            bb_trace(BBTraceSpringBoard, BBTraceOutputFailed);
            [self close];
            return;
        }
        if (written == 0) break;
        offset += (NSUInteger)written;
    }
    if (offset) {
        [_pendingOutput replaceBytesInRange:NSMakeRange(0, offset) withBytes:NULL length:0];
        _lastActivityMs = BBServerNowMs();
    }
    if ([_pendingOutput length] == 0 && _closeWhenDrained) [self close];
}

- (void)readAvailable
{
    uint8_t buffer[4096];
    while (!_closed && [_inputStream hasBytesAvailable]) {
        NSInteger count = [_inputStream read:buffer maxLength:sizeof(buffer)];
        if (count <= 0) {
            if (count < 0) [self close];
            return;
        }
        _lastActivityMs = BBServerNowMs();
        if (!bb_connection_receive(&_state, buffer, (size_t)count)) {
            /* The response (if any) is already queued; close once it is out. */
            _closeWhenDrained = YES;
            [self writeAvailable];
            return;
        }
    }
}

- (void)stream:(NSStream *)stream handleEvent:(NSStreamEvent)eventCode
{
    (void)stream;
    [self retain];
    if (!_closed) {
        if (eventCode & NSStreamEventHasBytesAvailable) [self readAvailable];
        if (eventCode & NSStreamEventHasSpaceAvailable) [self writeAvailable];
        if (eventCode & (NSStreamEventErrorOccurred | NSStreamEventEndEncountered)) {
            [self close];
        }
    }
    [self release];
}

- (void)dealloc
{
    /* The owner array holds the last reference and only drops it from
     * close, so this is normally already closed. Never re-enter the owner. */
    if (!_closed) {
        _transport = nil;
        [self close];
    }
    [_inputStream release];
    [_outputStream release];
    [_pendingOutput release];
    free(_input);
    free(_scratch);
    [super dealloc];
}

@end

/* ---- transport implementation ---------------------------------------- */

@implementation BBServerTransport

+ (instancetype)sharedTransport
{
    static BBServerTransport *transport;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        transport = [[self alloc] init];
    });
    return transport;
}

- (id)init
{
    self = [super init];
    if (!self) return nil;
    _connections = [[NSMutableArray alloc] init];
    return self;
}

- (BBRouterConfig *)routerConfig { return &_config; }
- (BBRequestTable *)requestTable { return &_requestTable; }
- (BBEIOSessionTable *)sessionTable { return &_sessionTable; }

- (BOOL)isReady
{
    return _ready;
}

/* Copy the configured password into a fixed buffer, generating and
 * storing one on first launch so the Settings pane can show it. Nothing
 * else keeps a reference to the preference value. */
- (BOOL)loadPassword
{
    NSString *value;
    const char *bytes;
    size_t length;
    CFPreferencesAppSynchronize(BB_SERVER_SETTINGS_DOMAIN);
    value = BBServerSettingString(CFSTR("password"));
    if (![value length]) {
        value = BBServerGeneratePassword();
        BBServerSetSetting(CFSTR("password"), value);
    }
    bytes = [value UTF8String];
    if (!bytes) return NO;
    length = strlen(bytes);
    if (!length || length >= BB_SERVER_PASSWORD_BYTES) return NO;
    for (size_t i = 0; i <= length; i++) _password[i] = bytes[i];
    return YES;
}

/* Listener settings: enabled, port, and TLS (opt-in: "tls" true plus a
 * loadable identity at "certificatePath"; anything else serves HTTP). */
- (void)loadListenerSettings
{
    NSString *path;
    NSString *passphrase;
    id port;
    CFPreferencesAppSynchronize(BB_SERVER_SETTINGS_DOMAIN);
    _enabled = BBServerSettingBool(CFSTR("enabled"), YES);
    port = BBServerSettingValue(CFSTR("port"));
    _port = BB_SERVER_DEFAULT_PORT;
    if ([port respondsToSelector:@selector(intValue)] && [port intValue] > 0 && [port intValue] < 65536)
        _port = (unsigned)[port intValue];
    _tlsWanted = BBServerSettingBool(CFSTR("tls"), NO);
    path = BBServerSettingString(CFSTR("certificatePath"));
    passphrase = BBServerSettingString(CFSTR("certificatePassphrase"));
    [_certificatePath release];
    _certificatePath = [([path length] ? path : BBServerDefaultCertificatePath) copy];
    [_certificatePassphrase release];
    _certificatePassphrase = [(passphrase ?: @"") copy];
}

- (void)loadCertificates
{
    [_certificates release];
    _certificates = nil;
    if (!_tlsWanted) return;
    _certificates = [BBServerImportCertificates(_certificatePath, _certificatePassphrase) retain];
}

/* Does a reloaded configuration need the listener reopened? */
- (BOOL)listenerConfigurationEquals:(BOOL)enabled port:(unsigned)port tls:(BOOL)tls
                                path:(NSString *)path passphrase:(NSString *)passphrase
{
    return _enabled == enabled && _port == port && _tlsWanted == tls &&
           [_certificatePath isEqualToString:path] &&
           [_certificatePassphrase isEqualToString:passphrase];
}

/* The Settings pane changed something. Runs on the transport thread. */
- (void)settingsDidChange
{
    BOOL enabled = _enabled;
    unsigned port = _port;
    BOOL tls = _tlsWanted;
    NSString *path = [[_certificatePath copy] autorelease];
    NSString *passphrase = [[_certificatePassphrase copy] autorelease];
    (void)[self loadPassword];
    [self loadListenerSettings];
    if ([self listenerConfigurationEquals:enabled port:port tls:tls path:path passphrase:passphrase])
        return;
    [self closeListener];
    if (_enabled) {
        [self loadCertificates];
        if ([self openListener]) bb_trace(BBTraceSpringBoard, BBTraceTransportListening);
        else bb_trace(BBTraceSpringBoard, BBTraceTransportStartFailed);
    }
}

static void BBServerSettingsNotification(CFNotificationCenterRef center, void *observer,
                                         CFStringRef name, const void *object,
                                         CFDictionaryRef userInfo)
{
    (void)center; (void)name; (void)object; (void)userInfo;
    [(BBServerTransport *)observer settingsDidChange];
}

- (void)loadLocalAddresses
{
    struct ifaddrs *interfaces = NULL;
    size_t count = 0;
    if (getifaddrs(&interfaces) != 0) return;
    for (struct ifaddrs *cursor = interfaces; cursor && count < BB_SERVER_MAX_LOCAL_ADDRESSES;
         cursor = cursor->ifa_next) {
        const struct sockaddr_in *address;
        if (!cursor->ifa_addr || cursor->ifa_addr->sa_family != AF_INET) continue;
        address = (const struct sockaddr_in *)cursor->ifa_addr;
        if (address->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
        if (!inet_ntop(AF_INET, &address->sin_addr, _localAddresses[count],
                       sizeof(_localAddresses[count]))) continue;
        _config.metadata.localIPv4[count] = _localAddresses[count];
        count++;
    }
    _config.metadata.localIPv4Count = count;
    freeifaddrs(interfaces);
}

- (BOOL)configureTables
{
    BBEIOCallbacks sessionCallbacks;
    _requestScratch = (char *)malloc(BB_SERVER_REQUEST_SCRATCH_BYTES);
    _eventScratch = (char *)malloc(BB_SERVER_EVENT_SCRATCH_BYTES);
    if (!_requestScratch || !_eventScratch) return NO;
    for (size_t i = 0; i < BB_SERVER_SESSION_SLOTS; i++) {
        _sessionFrames[i] = (unsigned char *)malloc(BB_SERVER_FRAMES_BYTES);
        _sessionMessage[i] = (unsigned char *)malloc(BB_SERVER_MESSAGE_BYTES);
        _sessionOutbound[i] = (unsigned char *)malloc(BB_SERVER_OUTBOUND_BYTES);
        if (!_sessionFrames[i] || !_sessionMessage[i] || !_sessionOutbound[i]) return NO;
        bb_eio_session_set_buffers(&_sessions[i], _sessionFrames[i], BB_SERVER_FRAMES_BYTES,
                                   _sessionMessage[i], BB_SERVER_MESSAGE_BYTES,
                                   _sessionOutbound[i], BB_SERVER_OUTBOUND_BYTES);
    }
    _sessionTable.sessions = _sessions;
    _sessionTable.capacity = BB_SERVER_SESSION_SLOTS;
    _sessionTable.generateSid = BBServerGenerateSid;
    _sessionTable.context = self;

    if (!bb_request_table_init(&_requestTable, _requests, BB_SERVER_REQUEST_SLOTS,
                               ((uint64_t)arc4random() << 32) | 1u,
                               BB_REQUEST_DEFAULT_TIMEOUT_MS, BBServerDispatch, self,
                               _requestScratch, BB_SERVER_REQUEST_SCRATCH_BYTES)) return NO;

    sessionCallbacks.event = BBServerSessionEvent;
    sessionCallbacks.sendFrame = BBServerSendFrame;
    sessionCallbacks.pollingReady = BBServerPollingReady;
    sessionCallbacks.closed = BBServerSessionClosed;
    sessionCallbacks.context = self;

    _config.password = _password;
    /* Fixed identifiers: nothing device-specific leaves through server/info. */
    _config.metadata.computerID = "bluebubbles-ios-bridge";
    _config.metadata.osVersion = "9.3.5";
    _config.metadata.serverVersion = "0.2.0";
    _config.metadata.proxyService = "Direct";
    _config.sessions = &_sessionTable;
    _config.sessionCallbacks.event = sessionCallbacks.event;
    _config.sessionCallbacks.sendFrame = sessionCallbacks.sendFrame;
    _config.sessionCallbacks.pollingReady = sessionCallbacks.pollingReady;
    _config.sessionCallbacks.closed = sessionCallbacks.closed;
    _config.sessionCallbacks.context = sessionCallbacks.context;
    _config.requests = &_requestTable;
    _config.nowMs = BBServerClock;
    _config.clockContext = self;
    _config.uploads.openFile = BBServerUploadOpen;
    _config.uploads.writeFile = BBServerUploadWrite;
    _config.uploads.closeFile = BBServerUploadClose;
    _config.uploads.context = self;
    [self loadLocalAddresses];
    return YES;
}

- (BOOL)openListener
{
    CFSocketContext context = { 0, self, NULL, NULL, NULL };
    struct sockaddr_in address;
    CFDataRef addressData;
    CFSocketError socketError;
    int reuseAddress = 1;

    _listener = CFSocketCreate(kCFAllocatorDefault, PF_INET, SOCK_STREAM, IPPROTO_TCP,
                               kCFSocketAcceptCallBack, BBServerAcceptCallback, &context);
    if (!_listener) return NO;
    setsockopt(CFSocketGetNative(_listener), SOL_SOCKET, SO_REUSEADDR,
               &reuseAddress, sizeof(reuseAddress));

    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_zero[0] = 0;
    address.sin_zero[1] = 0;
    address.sin_zero[2] = 0;
    address.sin_zero[3] = 0;
    address.sin_zero[4] = 0;
    address.sin_zero[5] = 0;
    addressData = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)&address, sizeof(address));
    socketError = addressData ? CFSocketSetAddress(_listener, addressData) : kCFSocketError;
    if (addressData) CFRelease(addressData);
    if (socketError != kCFSocketSuccess) {
        CFRelease(_listener);
        _listener = NULL;
        return NO;
    }
    _listenerSource = CFSocketCreateRunLoopSource(kCFAllocatorDefault, _listener, 0);
    if (!_listenerSource) {
        CFSocketInvalidate(_listener);
        CFRelease(_listener);
        _listener = NULL;
        return NO;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), _listenerSource, kCFRunLoopCommonModes);
    _ready = YES;
    return YES;
}

/* Stop accepting and drop every client; the server object stays alive. */
- (void)closeListener
{
    NSArray *connections = [[_connections copy] autorelease];
    for (BBServerConnection *connection in connections) [connection close];
    if (_listenerSource) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), _listenerSource, kCFRunLoopCommonModes);
        CFRelease(_listenerSource);
        _listenerSource = NULL;
    }
    if (_listener) {
        CFSocketInvalidate(_listener);
        CFRelease(_listener);
        _listener = NULL;
    }
    _ready = NO;
}

- (BOOL)start
{
    if (_thread) return YES;
    if (![self loadPassword]) return NO;
    [self loadListenerSettings];
    [self loadCertificates];
    if (![self configureTables]) return NO;
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(runTransport:) object:nil];
    if (!_thread) return NO;
    [_thread setName:@"BBServerTransport"];
    [_thread start];
    return YES;
}

- (void)runTransport:(id)unused
{
    (void)unused;
    @autoreleasepool {
        /* Settings changes arrive on this thread's run loop. */
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), self,
                                        BBServerSettingsNotification,
                                        BB_SERVER_SETTINGS_NOTIFICATION, NULL,
                                        CFNotificationSuspensionBehaviorCoalesce);
        if (_enabled) {
            if ([self openListener]) bb_trace(BBTraceSpringBoard, BBTraceTransportListening);
            else bb_trace(BBTraceSpringBoard, BBTraceTransportStartFailed);
        }
        _tickTimer = [[NSTimer scheduledTimerWithTimeInterval:BB_SERVER_TICK_SECONDS
                                                       target:self
                                                     selector:@selector(tick:)
                                                     userInfo:nil
                                                      repeats:YES] retain];
        for (;;) {
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:30.0]];
            }
        }
    }
}

- (void)tick:(NSTimer *)timer
{
    uint64_t now = BBServerNowMs();
    NSArray *connections = [[_connections copy] autorelease];
    (void)timer;
    for (BBServerConnection *connection in connections) {
        BBConnectionState state = bb_connection_state(&connection->_state);
        if ((state == BBConnectionStateHTTP || state == BBConnectionStateStreaming) &&
            now - connection->_lastActivityMs > BB_SERVER_IDLE_TIMEOUT_MS) {
            [connection close];
        }
    }
    for (size_t i = 0; i < BB_SERVER_SESSION_SLOTS; i++) {
        if (bb_eio_session_is_live(&_sessions[i])) bb_eio_session_tick(&_sessions[i], now);
    }
    if (bb_request_table_expire(&_requestTable, now)) {
        bb_trace(BBTraceSpringBoard, BBTraceRequestExpired);
    }
}

- (void)acceptNativeSocket:(CFSocketNativeHandle)nativeSocket
{
    BBServerConnection *connection;
    if (!_listener || [_connections count] >= BB_SERVER_MAX_CONNECTIONS) {
        bb_trace(BBTraceSpringBoard, BBTraceConnectionRefused);
        close(nativeSocket);
        return;
    }
    connection = [[BBServerConnection alloc] initWithNativeSocket:nativeSocket
                                                     certificates:_certificates
                                                        transport:self];
    if (!connection) {
        close(nativeSocket);
        return;
    }
    bb_trace(BBTraceSpringBoard, BBTraceConnectionAccepted);
    [_connections addObject:connection];
    [connection open];
    [connection release];
}

- (void)connectionDidClose:(BBServerConnection *)connection
{
    [_connections removeObjectIdenticalTo:connection];
}

- (void)sessionDidOpen:(BBEIOSession *)session onConnection:(BBServerConnection *)connection
{
    size_t index = BBServerSlotIndex(self, session);
    if (index >= BB_SERVER_SESSION_SLOTS) return;
    _sessionOwners[index] = connection;
    connection->_session = session;
    bb_trace(BBTraceSpringBoard, BBTraceSessionOpened);
    (void)bb_eio_session_start(session);
}

- (void)completePollingWaiterForSession:(BBEIOSession *)session
{
    BBServerConnection *connection = (BBServerConnection *)session->pollingWaiter;
    BBHTTPResponse response;
    size_t length = 0;
    if (!connection) return;
    session->pollingWaiter = NULL;
    if (!bb_eio_session_drain(session, (unsigned char *)connection->_scratch,
                              BB_SERVER_SCRATCH_BYTES, &length)) return;
    bb_response_init(&response);
    response.contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response.body = (const unsigned char *)connection->_scratch;
    response.bodyLength = length;
    if (!bb_connection_complete(&connection->_state, &response)) {
        connection->_closeWhenDrained = YES;
        [connection writeAvailable];
    }
}

/* ---- bridge replies and events --------------------------------------- */

- (void)deliverBridgeReply:(NSDictionary *)userInfo
{
    NSDictionary *copy;
    if (![userInfo isKindOfClass:[NSDictionary class]] || !_thread) return;
    copy = [userInfo copy];
    [self performSelector:@selector(handleBridgeReply:) onThread:_thread
               withObject:copy waitUntilDone:NO];
    [copy release];
}

- (void)deliverBridgeEvent:(NSDictionary *)userInfo
{
    NSDictionary *copy;
    if (![userInfo isKindOfClass:[NSDictionary class]] || !_thread) return;
    copy = [userInfo copy];
    [self performSelector:@selector(handleBridgeEvent:) onThread:_thread
               withObject:copy waitUntilDone:NO];
    [copy release];
}

static const char *BBServerOptionalJSON(NSDictionary *userInfo, NSString *key, size_t *length)
{
    id value = [userInfo objectForKey:key];
    const char *bytes;
    *length = 0;
    if (![value isKindOfClass:[NSString class]]) return NULL;
    bytes = [value UTF8String];
    if (!bytes) return NULL;
    *length = strlen(bytes);
    return bytes;
}

- (void)handleBridgeReply:(NSDictionary *)userInfo
{
    id requestIDValue = [userInfo objectForKey:@"requestID"];
    id statusValue = [userInfo objectForKey:@"status"];
    id messageValue = [userInfo objectForKey:@"message"];
    uint64_t requestID;
    int status;
    const char *data;
    size_t dataLength = 0;
    const char *metadata;
    size_t metadataLength = 0;
    const char *error;
    size_t errorLength = 0;
    const char *message = NULL;

    bb_trace(BBTraceSpringBoard, BBTraceBridgeReplyReceived);
    if (![requestIDValue isKindOfClass:[NSString class]] ||
        ![statusValue isKindOfClass:[NSNumber class]]) {
        bb_trace(BBTraceSpringBoard, BBTraceBridgeReplyRejected);
        return;
    }
    requestID = strtoull([requestIDValue UTF8String], NULL, 10);
    status = [statusValue intValue];
    /* Attachment download: the bridge sends a local file path instead of a
     * JSON envelope; stream the raw bytes. */
    {
        id filePathValue = [userInfo objectForKey:@"filePath"];
        if ([filePathValue isKindOfClass:[NSString class]] && [filePathValue length]) {
            BOOL ephemeral = [[userInfo objectForKey:@"ephemeral"] boolValue];
            [self completeDownload:requestID
                              path:filePathValue
                       contentType:[userInfo objectForKey:@"contentType"]
                         ephemeral:ephemeral];
            return;
        }
    }
    if ([messageValue isKindOfClass:[NSString class]]) message = [messageValue UTF8String];
    data = BBServerOptionalJSON(userInfo, @"dataJSON", &dataLength);
    metadata = BBServerOptionalJSON(userInfo, @"metadataJSON", &metadataLength);
    error = BBServerOptionalJSON(userInfo, @"errorJSON", &errorLength);
    if (!bb_request_table_complete(&_requestTable, requestID, status, message,
                                   data, dataLength, metadata, metadataLength,
                                   error, errorLength)) {
        bb_trace(BBTraceSpringBoard, BBTraceBridgeReplyRejected);
    }
}

/* Read the bridge-supplied file fully (bounded) and stream it as a raw
 * download body, or complete with a truthful error envelope. */
- (void)completeDownload:(uint64_t)requestID
                    path:(NSString *)path
             contentType:(id)contentTypeValue
               ephemeral:(BOOL)ephemeral
{
    const char *pathBytes = [path fileSystemRepresentation];
    const char *contentType = [contentTypeValue isKindOfClass:[NSString class]] &&
        [contentTypeValue length] ? [contentTypeValue UTF8String] : "application/octet-stream";
    int fd = pathBytes ? open(pathBytes, O_RDONLY | O_NONBLOCK) : -1;
    struct stat info;
    unsigned char *buffer = NULL;
    size_t total = 0;
    size_t filled = 0;

    /* Staged reply files are removed as soon as they are opened; the fd keeps
     * the bytes readable. */
    if (ephemeral && pathBytes) unlink(pathBytes);
    if (fd < 0) {
        [self failDownload:requestID status:404 detail:"Attachment file is unavailable"];
        return;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (unsigned long long)info.st_size > BB_SERVER_MAX_DOWNLOAD_BYTES) {
        close(fd);
        [self failDownload:requestID status:(info.st_size > 0 ? 413 : 404)
                    detail:(info.st_size > 0 ? "Attachment is too large for this build" :
                            "Attachment file is unavailable")];
        return;
    }
    total = (size_t)info.st_size;
    buffer = total ? (unsigned char *)malloc(total) : (unsigned char *)malloc(1);
    if (!buffer) {
        close(fd);
        [self failDownload:requestID status:500 detail:"Out of memory reading attachment"];
        return;
    }
    while (filled < total) {
        ssize_t got = pread(fd, buffer + filled, total - filled, (off_t)filled);
        if (got > 0) { filled += (size_t)got; continue; }
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        break;
    }
    close(fd);
    if (filled != total) {
        free(buffer);
        [self failDownload:requestID status:500 detail:"Attachment read failed"];
        return;
    }
    if (!bb_request_table_complete_raw(&_requestTable, requestID, 200, contentType,
                                       buffer, total)) {
        bb_trace(BBTraceSpringBoard, BBTraceBridgeReplyRejected);
    }
    free(buffer);
}

- (void)failDownload:(uint64_t)requestID status:(int)status detail:(const char *)detail
{
    char json[256];
    BBJSONWriter writer;
    size_t length = 0;
    bb_trace(BBTraceSpringBoard, BBTraceBridgeReplyRejected);
    bb_json_writer_init(&writer, json, sizeof(json));
    if (bb_json_write_raw(&writer, "{\"type\":\"Database Error\",\"message\":") &&
        bb_json_write_string(&writer, detail) && bb_json_write_raw(&writer, "}") &&
        bb_json_writer_finish(&writer, &length)) {
        (void)bb_request_table_complete(&_requestTable, requestID, status,
                                        status == 404 ? "Not Found" : "Server Error",
                                        NULL, 0, NULL, 0, json, length);
    }
}

- (void)handleBridgeEvent:(NSDictionary *)userInfo
{
    id nameValue = [userInfo objectForKey:@"event"];
    const char *payload;
    size_t payloadLength = 0;
    bb_trace(BBTraceSpringBoard, BBTraceBridgeEventReceived);
    if (![nameValue isKindOfClass:[NSString class]]) return;
    payload = BBServerOptionalJSON(userInfo, @"payloadJSON", &payloadLength);
    if (!payload) return;
    (void)bb_events_broadcast(&_sessionTable, [nameValue UTF8String], payload, payloadLength);
}

@end
