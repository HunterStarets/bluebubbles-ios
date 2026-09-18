#ifndef BB_MULTIPART_H
#define BB_MULTIPART_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Incremental multipart/form-data reader (RFC 7578 over RFC 2046). Bytes
 * are fed in any fragmentation; each part's head is parsed into a bounded
 * record and its body is handed on as it arrives, so a file part never
 * has to fit in memory. The parser keeps back at most one delimiter's
 * worth of bytes between calls to decide whether they start a boundary.
 * Nothing is allocated. */

#define BB_MULTIPART_MAX_BOUNDARY_BYTES 70U      /* RFC 2046 */
#define BB_MULTIPART_MAX_DELIMITER_BYTES (4U + BB_MULTIPART_MAX_BOUNDARY_BYTES)
#define BB_MULTIPART_MAX_PART_HEAD_BYTES 1024U
#define BB_MULTIPART_MAX_NAME_BYTES 64U
#define BB_MULTIPART_MAX_FILENAME_BYTES 256U
#define BB_MULTIPART_MAX_CONTENT_TYPE_BYTES 128U
#define BB_MULTIPART_MAX_PARTS 32U

typedef struct {
    char name[BB_MULTIPART_MAX_NAME_BYTES];             /* form field name */
    bool hasFilename;                                   /* a file part */
    char filename[BB_MULTIPART_MAX_FILENAME_BYTES];     /* empty without */
    char contentType[BB_MULTIPART_MAX_CONTENT_TYPE_BYTES]; /* empty without */
} BBMultipartPart;

/* Return false from any callback to stop: bb_multipart_feed then fails and
 * the parser refuses further input. */
typedef struct {
    bool (*partBegin)(void *context, const BBMultipartPart *part);
    bool (*partData)(void *context, const unsigned char *bytes, size_t length);
    bool (*partEnd)(void *context);
    void *context;
} BBMultipartCallbacks;

typedef enum {
    BBMultipartStatePreamble = 0,   /* before the first delimiter */
    BBMultipartStateAfterDelimiter, /* expecting CRLF (part) or "--" (close) */
    BBMultipartStatePartHead,
    BBMultipartStatePartData,
    BBMultipartStateDone,           /* close delimiter seen; epilogue ignored */
    BBMultipartStateFailed
} BBMultipartState;

typedef struct {
    BBMultipartCallbacks callbacks;
    BBMultipartState state;
    unsigned char delimiter[BB_MULTIPART_MAX_DELIMITER_BYTES]; /* CRLF "--" boundary */
    size_t delimiterLength;
    /* Bytes that may begin a delimiter, kept until the next feed decides. */
    unsigned char hold[BB_MULTIPART_MAX_DELIMITER_BYTES];
    size_t holdLength;
    unsigned char tail[2];          /* the two bytes after a delimiter */
    size_t tailLength;
    unsigned char head[BB_MULTIPART_MAX_PART_HEAD_BYTES];
    size_t headLength;
    BBMultipartPart part;
    size_t partCount;
} BBMultipart;

/* contentType is the request's Content-Type value; it must be
 * multipart/form-data with a boundary parameter (quoted or bare). */
bool bb_multipart_init(BBMultipart *parser, const char *contentType,
                       const BBMultipartCallbacks *callbacks);

/* Consume bytes. False on malformed input, limits, or a refusing
 * callback; the parser is then failed for good. */
bool bb_multipart_feed(BBMultipart *parser, const unsigned char *bytes, size_t length);

/* True only if the close delimiter was seen and every part was closed. */
bool bb_multipart_finish(const BBMultipart *parser);

#ifdef __cplusplus
}
#endif

#endif
