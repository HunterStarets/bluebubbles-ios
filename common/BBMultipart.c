#include "BBMultipart.h"

/* Volatile loops keep the older armv7 Clang from lowering these into the
 * libc calls the SpringBoard target must not import. */
static void bb_multipart_memory_set(void *destination, unsigned char value, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static void bb_multipart_memory_copy(void *destination, const void *source, size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
}

static size_t bb_multipart_string_length(const char *value)
{
    size_t length = 0;
    if (!value) return 0;
    while (value[length]) length++;
    return length;
}

static unsigned char bb_multipart_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool bb_multipart_equal_case(const unsigned char *value, size_t length, const char *wanted)
{
    size_t i = 0;
    for (; i < length; i++) {
        if (!wanted[i] || bb_multipart_lower(value[i]) != bb_multipart_lower((unsigned char)wanted[i]))
            return false;
    }
    return wanted[i] == '\0';
}

static bool bb_multipart_is_space(unsigned char c)
{
    return c == ' ' || c == '\t';
}

/* Copy a parameter value (quoted, with backslash escapes, or a bare token)
 * that starts at *cursor. Fails when it does not fit. */
static bool bb_multipart_read_value(const unsigned char *text, size_t length, size_t *cursor,
                                    char *out, size_t capacity)
{
    size_t i = *cursor;
    size_t written = 0;
    if (i < length && text[i] == '"') {
        i++;
        while (i < length && text[i] != '"') {
            unsigned char c = text[i];
            if (c == '\\' && i + 1 < length) { c = text[i + 1]; i++; }
            if (written + 1 >= capacity) return false;
            out[written++] = (char)c;
            i++;
        }
        if (i >= length) return false; /* unterminated quote */
        i++;
    } else {
        while (i < length && text[i] != ';' && !bb_multipart_is_space(text[i])) {
            if (written + 1 >= capacity) return false;
            out[written++] = (char)text[i++];
        }
    }
    out[written] = '\0';
    *cursor = i;
    return true;
}

/* Walk "value; name=v; name2=v2" parameters. matchName receives each
 * parameter name; the value lands in the buffer chosen by the caller. */
typedef struct {
    const char *name;
    char *out;
    size_t capacity;
    bool *present;
} BBMultipartParameter;

static bool bb_multipart_read_parameters(const unsigned char *text, size_t length,
                                         const BBMultipartParameter *parameters,
                                         size_t parameterCount)
{
    size_t i = 0;
    /* Skip the leading media type / disposition token. */
    while (i < length && text[i] != ';') i++;
    while (i < length) {
        size_t nameStart;
        size_t nameLength;
        bool consumed = false;
        if (text[i] != ';') return false;
        i++;
        while (i < length && bb_multipart_is_space(text[i])) i++;
        nameStart = i;
        while (i < length && text[i] != '=' && text[i] != ';' && !bb_multipart_is_space(text[i])) i++;
        nameLength = i - nameStart;
        while (i < length && bb_multipart_is_space(text[i])) i++;
        if (i >= length || text[i] != '=') {
            /* A bare parameter without a value; tolerated and skipped. */
            if (!nameLength) return false;
            continue;
        }
        i++;
        while (i < length && bb_multipart_is_space(text[i])) i++;
        for (size_t p = 0; p < parameterCount; p++) {
            if (bb_multipart_equal_case(text + nameStart, nameLength, parameters[p].name)) {
                if (!bb_multipart_read_value(text, length, &i, parameters[p].out,
                                             parameters[p].capacity)) return false;
                if (parameters[p].present) *parameters[p].present = true;
                consumed = true;
                break;
            }
        }
        if (!consumed) {
            char discard[BB_MULTIPART_MAX_FILENAME_BYTES];
            if (!bb_multipart_read_value(text, length, &i, discard, sizeof(discard))) return false;
        }
        while (i < length && bb_multipart_is_space(text[i])) i++;
    }
    return true;
}

bool bb_multipart_init(BBMultipart *parser, const char *contentType,
                       const BBMultipartCallbacks *callbacks)
{
    size_t length = bb_multipart_string_length(contentType);
    char boundary[BB_MULTIPART_MAX_BOUNDARY_BYTES + 1];
    bool present = false;
    BBMultipartParameter parameter;
    size_t boundaryLength;
    size_t i = 0;

    if (!parser) return false;
    bb_multipart_memory_set(parser, 0, sizeof(*parser));
    parser->state = BBMultipartStateFailed;
    if (!contentType || !callbacks || !callbacks->partBegin || !callbacks->partData ||
        !callbacks->partEnd) return false;
    while (i < length && contentType[i] != ';' && !bb_multipart_is_space(contentType[i])) i++;
    if (!bb_multipart_equal_case((const unsigned char *)contentType, i, "multipart/form-data"))
        return false;
    parameter.name = "boundary";
    parameter.out = boundary;
    parameter.capacity = sizeof(boundary);
    parameter.present = &present;
    if (!bb_multipart_read_parameters((const unsigned char *)contentType, length, &parameter, 1) ||
        !present) return false;
    boundaryLength = bb_multipart_string_length(boundary);
    if (!boundaryLength || boundaryLength > BB_MULTIPART_MAX_BOUNDARY_BYTES) return false;
    for (i = 0; i < boundaryLength; i++) {
        unsigned char c = (unsigned char)boundary[i];
        if (c < 0x21 || c > 0x7e || c == '"') return false;
    }
    parser->callbacks = *callbacks;
    parser->delimiter[0] = '\r';
    parser->delimiter[1] = '\n';
    parser->delimiter[2] = '-';
    parser->delimiter[3] = '-';
    bb_multipart_memory_copy(parser->delimiter + 4, boundary, boundaryLength);
    parser->delimiterLength = 4 + boundaryLength;
    /* The first delimiter has no CRLF before it: pretend one was seen. */
    parser->hold[0] = '\r';
    parser->hold[1] = '\n';
    parser->holdLength = 2;
    parser->state = BBMultipartStatePreamble;
    return true;
}

static bool bb_multipart_fail(BBMultipart *parser)
{
    parser->state = BBMultipartStateFailed;
    return false;
}

/* Deliver part bytes (no-op in the preamble). */
static bool bb_multipart_emit(BBMultipart *parser, const unsigned char *bytes, size_t length)
{
    if (!length || parser->state == BBMultipartStatePreamble) return true;
    return parser->callbacks.partData(parser->callbacks.context, bytes, length);
}

/* Byte i of the virtual buffer hold + bytes. */
static unsigned char bb_multipart_at(const BBMultipart *parser, const unsigned char *bytes, size_t i)
{
    return i < parser->holdLength ? parser->hold[i] : bytes[i - parser->holdLength];
}

/* Emit virtual range [0, end) as at most two runs. */
static bool bb_multipart_emit_prefix(BBMultipart *parser, const unsigned char *bytes, size_t end)
{
    size_t fromHold = end < parser->holdLength ? end : parser->holdLength;
    if (!bb_multipart_emit(parser, parser->hold, fromHold)) return false;
    return bb_multipart_emit(parser, bytes, end - fromHold);
}

/* Parse the accumulated part head into parser->part. */
static bool bb_multipart_parse_head(BBMultipart *parser)
{
    const unsigned char *head = parser->head;
    size_t length = parser->headLength;
    size_t cursor = 0;
    bool sawDisposition = false;
    bb_multipart_memory_set(&parser->part, 0, sizeof(parser->part));
    while (cursor < length) {
        size_t lineEnd = cursor;
        size_t colon;
        size_t valueStart;
        while (lineEnd + 1 < length && !(head[lineEnd] == '\r' && head[lineEnd + 1] == '\n')) lineEnd++;
        if (lineEnd + 1 >= length) lineEnd = length; /* last line, no CRLF */
        colon = cursor;
        while (colon < lineEnd && head[colon] != ':') colon++;
        if (colon == lineEnd || colon == cursor) return false;
        valueStart = colon + 1;
        while (valueStart < lineEnd && bb_multipart_is_space(head[valueStart])) valueStart++;
        if (bb_multipart_equal_case(head + cursor, colon - cursor, "content-disposition")) {
            BBMultipartParameter parameters[2];
            size_t tokenEnd = valueStart;
            bool hasName = false;
            while (tokenEnd < lineEnd && head[tokenEnd] != ';' && !bb_multipart_is_space(head[tokenEnd]))
                tokenEnd++;
            if (!bb_multipart_equal_case(head + valueStart, tokenEnd - valueStart, "form-data"))
                return false;
            parameters[0].name = "name";
            parameters[0].out = parser->part.name;
            parameters[0].capacity = sizeof(parser->part.name);
            parameters[0].present = &hasName;
            parameters[1].name = "filename";
            parameters[1].out = parser->part.filename;
            parameters[1].capacity = sizeof(parser->part.filename);
            parameters[1].present = &parser->part.hasFilename;
            if (!bb_multipart_read_parameters(head + valueStart, lineEnd - valueStart,
                                              parameters, 2)) return false;
            if (!hasName || !parser->part.name[0]) return false;
            sawDisposition = true;
        } else if (bb_multipart_equal_case(head + cursor, colon - cursor, "content-type")) {
            size_t valueLength = lineEnd - valueStart;
            if (valueLength + 1 > sizeof(parser->part.contentType)) return false;
            bb_multipart_memory_copy(parser->part.contentType, head + valueStart, valueLength);
            parser->part.contentType[valueLength] = '\0';
        }
        /* Other part headers (Content-Transfer-Encoding, ...) are ignored. */
        cursor = lineEnd + 2;
    }
    return sawDisposition;
}

/* Scan the virtual buffer for the delimiter, emitting data before it.
 * Returns the virtual index just after a found delimiter, or SIZE_MAX
 * with the undecidable tail moved into hold. */
static bool bb_multipart_scan(BBMultipart *parser, const unsigned char *bytes, size_t length,
                              size_t *afterDelimiter)
{
    size_t total = parser->holdLength + length;
    size_t i = 0;
    *afterDelimiter = (size_t)-1;
    while (i < total) {
        /* Cheap first-byte test; the delimiter always starts with CR. */
        if (bb_multipart_at(parser, bytes, i) == '\r') {
            size_t available = total - i;
            size_t compare = available < parser->delimiterLength ? available : parser->delimiterLength;
            size_t k = 1;
            while (k < compare && bb_multipart_at(parser, bytes, i + k) == parser->delimiter[k]) k++;
            if (k == parser->delimiterLength) {
                if (!bb_multipart_emit_prefix(parser, bytes, i)) return false;
                *afterDelimiter = i + parser->delimiterLength;
                parser->holdLength = 0;
                return true;
            }
            if (k == compare) {
                /* A prefix of the delimiter runs to the end: hold it. */
                if (!bb_multipart_emit_prefix(parser, bytes, i)) return false;
                {
                    unsigned char kept[BB_MULTIPART_MAX_DELIMITER_BYTES];
                    for (size_t j = 0; j < available; j++) kept[j] = bb_multipart_at(parser, bytes, i + j);
                    bb_multipart_memory_copy(parser->hold, kept, available);
                    parser->holdLength = available;
                }
                return true;
            }
        }
        i++;
    }
    if (!bb_multipart_emit_prefix(parser, bytes, total)) return false;
    parser->holdLength = 0;
    return true;
}

bool bb_multipart_feed(BBMultipart *parser, const unsigned char *bytes, size_t length)
{
    size_t offset = 0;
    if (!parser || (length && !bytes)) return false;
    if (parser->state == BBMultipartStateFailed) return false;
    while (offset < length || (parser->state == BBMultipartStateAfterDelimiter && parser->tailLength == 2)) {
        switch (parser->state) {
            case BBMultipartStatePreamble:
            case BBMultipartStatePartData: {
                size_t after = 0;
                size_t previousHold = parser->holdLength;
                /* The rest of this feed is scanned as a virtual buffer
                 * behind whatever the hold carries. */
                if (!bb_multipart_scan(parser, bytes + offset, length - offset, &after))
                    return bb_multipart_fail(parser);
                if (after == (size_t)-1) return true; /* consumed or held */
                if (parser->state == BBMultipartStatePartData &&
                    !parser->callbacks.partEnd(parser->callbacks.context))
                    return bb_multipart_fail(parser);
                /* The delimiter always ends inside bytes (the hold is
                 * shorter than it), so this never underflows. */
                offset += after - previousHold;
                parser->state = BBMultipartStateAfterDelimiter;
                parser->tailLength = 0;
                break;
            }
            case BBMultipartStateAfterDelimiter:
                while (parser->tailLength < 2 && offset < length)
                    parser->tail[parser->tailLength++] = bytes[offset++];
                if (parser->tailLength < 2) return true;
                if (parser->tail[0] == '\r' && parser->tail[1] == '\n') {
                    if (parser->partCount >= BB_MULTIPART_MAX_PARTS) return bb_multipart_fail(parser);
                    parser->state = BBMultipartStatePartHead;
                    parser->headLength = 0;
                } else if (parser->tail[0] == '-' && parser->tail[1] == '-') {
                    parser->state = BBMultipartStateDone;
                } else {
                    return bb_multipart_fail(parser);
                }
                parser->tailLength = 0;
                break;
            case BBMultipartStatePartHead:
                while (offset < length) {
                    if (parser->headLength >= sizeof(parser->head)) return bb_multipart_fail(parser);
                    parser->head[parser->headLength++] = bytes[offset++];
                    if (parser->headLength >= 4 &&
                        parser->head[parser->headLength - 4] == '\r' &&
                        parser->head[parser->headLength - 3] == '\n' &&
                        parser->head[parser->headLength - 2] == '\r' &&
                        parser->head[parser->headLength - 1] == '\n') {
                        parser->headLength -= 4;
                        if (!bb_multipart_parse_head(parser)) return bb_multipart_fail(parser);
                        parser->partCount++;
                        if (!parser->callbacks.partBegin(parser->callbacks.context, &parser->part))
                            return bb_multipart_fail(parser);
                        parser->state = BBMultipartStatePartData;
                        parser->holdLength = 0;
                        break;
                    }
                }
                break;
            case BBMultipartStateDone:
                return true; /* epilogue ignored */
            default:
                return false;
        }
    }
    return true;
}

bool bb_multipart_finish(const BBMultipart *parser)
{
    return parser && parser->state == BBMultipartStateDone;
}
