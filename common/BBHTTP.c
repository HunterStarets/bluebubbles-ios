#include "BBHTTP.h"

#include <stdint.h>

static void bb_http_memory_set(void *destination, unsigned char value,
                               size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static void bb_http_memory_copy(void *destination, const void *source,
                                size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in =
        (const volatile unsigned char *)source;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
}

static bool bb_http_equal(const unsigned char *value, size_t valueLength,
                          const char *wanted)
{
    size_t index = 0;
    if (!value || !wanted) return false;
    while (wanted[index]) {
        if (index >= valueLength || value[index] != (unsigned char)wanted[index])
            return false;
        index++;
    }
    return index == valueLength;
}

static unsigned char bb_http_ascii_lower(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static bool bb_http_equal_case(const unsigned char *value, size_t valueLength,
                               const char *wanted)
{
    size_t index = 0;
    if (!value || !wanted) return false;
    while (wanted[index]) {
        if (index >= valueLength ||
            bb_http_ascii_lower(value[index]) !=
                bb_http_ascii_lower((unsigned char)wanted[index])) return false;
        index++;
    }
    return index == valueLength;
}

static bool bb_http_copy_string(const unsigned char *source, size_t length,
                                char *out, size_t capacity)
{
    if (!out || capacity == 0 || length >= capacity) return false;
    if (length && !source) return false;
    if (length) bb_http_memory_copy(out, source, length);
    out[length] = '\0';
    return true;
}

static bool bb_http_is_token(unsigned char value)
{
    if ((value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) return true;
    switch (value) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~': return true;
        default: return false;
    }
}

static size_t bb_http_find_crlf(const unsigned char *buffer, size_t start,
                                size_t limit)
{
    if (!buffer || start >= limit) return SIZE_MAX;
    for (size_t i = start; i + 1 < limit; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') return i;
    }
    return SIZE_MAX;
}

static size_t bb_http_find_header_end(const unsigned char *buffer,
                                      size_t length)
{
    if (!buffer || length < 4) return SIZE_MAX;
    for (size_t i = 0; i + 3 < length; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') return i + 4;
    }
    return SIZE_MAX;
}

static bool bb_http_parse_decimal(const unsigned char *value, size_t length,
                                  size_t *out)
{
    if (!value || !length || !out) return false;
    size_t result = 0;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') return false;
        unsigned int digit = (unsigned int)(value[i] - '0');
        if (result > (SIZE_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *out = result;
    return true;
}

static bool bb_http_token_list_contains(const unsigned char *value,
                                        size_t length, const char *wanted)
{
    size_t cursor = 0;
    while (cursor < length) {
        while (cursor < length && (value[cursor] == ' ' || value[cursor] == '\t' ||
                                   value[cursor] == ',')) cursor++;
        size_t start = cursor;
        while (cursor < length && value[cursor] != ',') cursor++;
        size_t end = cursor;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
        if (bb_http_equal_case(value + start, end - start, wanted)) return true;
    }
    return false;
}

static int bb_http_hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = bb_http_ascii_lower(value);
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static BBHTTPParseResult bb_http_scan_chunked(const unsigned char *buffer,
                                              size_t length,
                                              size_t *encodedLength,
                                              size_t *decodedLength)
{
    if (!buffer || !encodedLength || !decodedLength) return BBHTTPParseInvalid;
    size_t cursor = 0;
    size_t decoded = 0;
    for (;;) {
        size_t lineLimit = length;
        if (length - cursor > BB_HTTP_MAX_HEADER_BYTES)
            lineLimit = cursor + BB_HTTP_MAX_HEADER_BYTES;
        size_t lineEnd = bb_http_find_crlf(buffer, cursor, lineLimit);
        if (lineEnd == SIZE_MAX) return length - cursor >= BB_HTTP_MAX_HEADER_BYTES ?
            BBHTTPParseInvalid : BBHTTPParseNeedMore;
        size_t digits = 0;
        size_t chunkLength = 0;
        size_t index = cursor;
        while (index < lineEnd && buffer[index] != ';') {
            int digit = bb_http_hex_value(buffer[index]);
            if (digit < 0 || digits >= 16 ||
                chunkLength > (SIZE_MAX - (size_t)digit) / 16) return BBHTTPParseInvalid;
            chunkLength = chunkLength * 16 + (size_t)digit;
            digits++;
            index++;
        }
        if (!digits) return BBHTTPParseInvalid;
        if (index < lineEnd && buffer[index] == ';') {
            for (size_t i = index + 1; i < lineEnd; i++) {
                if (buffer[i] < 0x20 || buffer[i] == 0x7f) return BBHTTPParseInvalid;
            }
        }
        if (chunkLength > BB_HTTP_MAX_BODY_BYTES - decoded)
            return BBHTTPParseInvalid;
        cursor = lineEnd + 2;
        if (chunkLength == 0) {
            if (cursor + 1 >= length) return BBHTTPParseNeedMore;
            if (buffer[cursor] == '\r' && buffer[cursor + 1] == '\n') {
                *encodedLength = cursor + 2;
                *decodedLength = decoded;
                return BBHTTPParseReady;
            }
            size_t trailerAvailable = length - cursor;
            size_t trailerScanLength = trailerAvailable < BB_HTTP_MAX_HEADER_BYTES ?
                trailerAvailable : BB_HTTP_MAX_HEADER_BYTES;
            size_t trailerEnd = bb_http_find_header_end(buffer + cursor,
                                                        trailerScanLength);
            if (trailerEnd == SIZE_MAX) return trailerAvailable >= BB_HTTP_MAX_HEADER_BYTES ?
                BBHTTPParseInvalid : BBHTTPParseNeedMore;
            /* Reject folded/control-filled trailers. Values are ignored. */
            size_t trailerCursor = cursor;
            size_t absoluteEnd = cursor + trailerEnd - 2;
            while (trailerCursor < absoluteEnd) {
                size_t trailerLineEnd = bb_http_find_crlf(buffer, trailerCursor,
                                                          absoluteEnd + 2);
                if (trailerLineEnd == SIZE_MAX || trailerLineEnd == trailerCursor)
                    return BBHTTPParseInvalid;
                bool colon = false;
                for (size_t i = trailerCursor; i < trailerLineEnd; i++) {
                    if (buffer[i] == ':') colon = true;
                    if (buffer[i] < 0x20 && buffer[i] != '\t') return BBHTTPParseInvalid;
                }
                if (!colon) return BBHTTPParseInvalid;
                trailerCursor = trailerLineEnd + 2;
            }
            *encodedLength = cursor + trailerEnd;
            *decodedLength = decoded;
            return BBHTTPParseReady;
        }
        if (chunkLength > length - cursor) return BBHTTPParseNeedMore;
        cursor += chunkLength;
        if (cursor + 1 >= length) return BBHTTPParseNeedMore;
        if (buffer[cursor] != '\r' || buffer[cursor + 1] != '\n')
            return BBHTTPParseInvalid;
        cursor += 2;
        decoded += chunkLength;
    }
}

const char *bb_http_method_name(BBHTTPMethod method)
{
    switch (method) {
        case BBHTTPMethodGET: return "GET";
        case BBHTTPMethodPOST: return "POST";
        case BBHTTPMethodPUT: return "PUT";
        case BBHTTPMethodDELETE: return "DELETE";
        case BBHTTPMethodOPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

BBHTTPParseResult bb_http_parse_request(const unsigned char *buffer,
                                        size_t bufferLength,
                                        BBHTTPRequest *out)
{
    if (!out) return BBHTTPParseInvalid;
    bb_http_memory_set(out, 0, sizeof(*out));
    if (!buffer || !bufferLength) return BBHTTPParseNeedMore;
    /* A request line starts with a method token. Anything else (a TLS
     * ClientHello on the plain port, binary noise) can never become a
     * request, so refuse it now instead of waiting for a header end that
     * will not come and holding the connection open meanwhile. */
    if (!bb_http_is_token(buffer[0]) || buffer[0] < 'A' || buffer[0] > 'Z')
        return BBHTTPParseInvalid;
    size_t headerScanLength = bufferLength < BB_HTTP_MAX_HEADER_BYTES ?
        bufferLength : BB_HTTP_MAX_HEADER_BYTES;
    size_t headerLength = bb_http_find_header_end(buffer, headerScanLength);
    if (headerLength == SIZE_MAX) return bufferLength >= BB_HTTP_MAX_HEADER_BYTES ?
        BBHTTPParseInvalid : BBHTTPParseNeedMore;
    if (headerLength > BB_HTTP_MAX_HEADER_BYTES) return BBHTTPParseInvalid;

    size_t requestLineEnd = bb_http_find_crlf(buffer, 0, headerLength);
    if (requestLineEnd == SIZE_MAX || requestLineEnd == 0) return BBHTTPParseInvalid;
    size_t firstSpace = SIZE_MAX;
    size_t secondSpace = SIZE_MAX;
    for (size_t i = 0; i < requestLineEnd; i++) {
        if (buffer[i] == ' ') {
            if (firstSpace == SIZE_MAX) firstSpace = i;
            else if (secondSpace == SIZE_MAX) secondSpace = i;
            else return BBHTTPParseInvalid;
        } else if (buffer[i] < 0x21 || buffer[i] == 0x7f) {
            return BBHTTPParseInvalid;
        }
    }
    if (firstSpace == SIZE_MAX || secondSpace == SIZE_MAX || firstSpace == 0 ||
        secondSpace <= firstSpace + 1 || secondSpace + 1 >= requestLineEnd)
        return BBHTTPParseInvalid;

    if (bb_http_equal(buffer, firstSpace, "GET")) out->method = BBHTTPMethodGET;
    else if (bb_http_equal(buffer, firstSpace, "POST")) out->method = BBHTTPMethodPOST;
    else if (bb_http_equal(buffer, firstSpace, "PUT")) out->method = BBHTTPMethodPUT;
    else if (bb_http_equal(buffer, firstSpace, "DELETE")) out->method = BBHTTPMethodDELETE;
    else if (bb_http_equal(buffer, firstSpace, "OPTIONS")) out->method = BBHTTPMethodOPTIONS;
    else return BBHTTPParseInvalid;

    const unsigned char *target = buffer + firstSpace + 1;
    size_t targetLength = secondSpace - firstSpace - 1;
    if (!targetLength || target[0] != '/' ||
        !bb_http_copy_string(target, targetLength, out->target, sizeof(out->target)))
        return BBHTTPParseInvalid;
    const unsigned char *version = buffer + secondSpace + 1;
    size_t versionLength = requestLineEnd - secondSpace - 1;
    if (bb_http_equal(version, versionLength, "HTTP/1.1")) out->http11 = true;
    else if (!bb_http_equal(version, versionLength, "HTTP/1.0"))
        return BBHTTPParseInvalid;

    size_t question = targetLength;
    for (size_t i = 0; i < targetLength; i++) {
        if (target[i] == '#') return BBHTTPParseInvalid;
        if (target[i] == '?' && question == targetLength) question = i;
    }
    if (!bb_http_copy_string(target, question, out->path, sizeof(out->path)))
        return BBHTTPParseInvalid;
    if (question < targetLength &&
        !bb_http_copy_string(target + question + 1, targetLength - question - 1,
                             out->query, sizeof(out->query))) return BBHTTPParseInvalid;

    bool sawContentLength = false;
    bool sawTransferEncoding = false;
    bool connectionClose = false;
    bool connectionKeepAlive = false;
    bool connectionUpgrade = false;
    bool upgradeWebSocket = false;
    size_t contentLength = 0;
    size_t cursor = requestLineEnd + 2;
    while (cursor + 2 < headerLength) {
        size_t lineEnd = bb_http_find_crlf(buffer, cursor, headerLength);
        if (lineEnd == SIZE_MAX) return BBHTTPParseInvalid;
        if (lineEnd == cursor) break;
        if (buffer[cursor] == ' ' || buffer[cursor] == '\t') return BBHTTPParseInvalid;
        size_t colon = SIZE_MAX;
        for (size_t i = cursor; i < lineEnd; i++) {
            if (buffer[i] == ':' && colon == SIZE_MAX) colon = i;
            else if (colon == SIZE_MAX && !bb_http_is_token(buffer[i]))
                return BBHTTPParseInvalid;
        }
        if (colon == SIZE_MAX || colon == cursor) return BBHTTPParseInvalid;
        size_t valueStart = colon + 1;
        while (valueStart < lineEnd &&
               (buffer[valueStart] == ' ' || buffer[valueStart] == '\t')) valueStart++;
        size_t valueEnd = lineEnd;
        while (valueEnd > valueStart &&
               (buffer[valueEnd - 1] == ' ' || buffer[valueEnd - 1] == '\t')) valueEnd--;
        const unsigned char *value = buffer + valueStart;
        size_t valueLength = valueEnd - valueStart;
        for (size_t i = valueStart; i < valueEnd; i++) {
            if ((buffer[i] < 0x20 && buffer[i] != '\t') || buffer[i] == 0x7f)
                return BBHTTPParseInvalid;
        }

        if (bb_http_equal_case(buffer + cursor, colon - cursor, "content-length")) {
            size_t parsed = 0;
            if (!bb_http_parse_decimal(value, valueLength, &parsed) ||
                parsed > BB_HTTP_MAX_BODY_BYTES ||
                (sawContentLength && parsed != contentLength)) return BBHTTPParseInvalid;
            sawContentLength = true;
            contentLength = parsed;
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor,
                                      "transfer-encoding")) {
            if (sawTransferEncoding ||
                !bb_http_equal_case(value, valueLength, "chunked"))
                return BBHTTPParseInvalid;
            sawTransferEncoding = true;
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor,
                                      "connection")) {
            connectionClose |= bb_http_token_list_contains(value, valueLength, "close");
            connectionKeepAlive |= bb_http_token_list_contains(value, valueLength, "keep-alive");
            connectionUpgrade |= bb_http_token_list_contains(value, valueLength, "upgrade");
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor, "upgrade")) {
            upgradeWebSocket = bb_http_token_list_contains(value, valueLength, "websocket");
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor,
                                      "content-type")) {
            if (!bb_http_copy_string(value, valueLength, out->contentType,
                                     sizeof(out->contentType))) return BBHTTPParseInvalid;
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor,
                                      "sec-websocket-key")) {
            if (!bb_http_copy_string(value, valueLength, out->webSocketKey,
                                     sizeof(out->webSocketKey))) return BBHTTPParseInvalid;
        } else if (bb_http_equal_case(buffer + cursor, colon - cursor,
                                      "sec-websocket-version")) {
            out->webSocketVersion13 = bb_http_equal(value, valueLength, "13");
        }
        cursor = lineEnd + 2;
    }
    if (sawContentLength && sawTransferEncoding) return BBHTTPParseInvalid;

    out->headerLength = headerLength;
    out->bodyOffset = headerLength;
    out->keepAlive = out->http11 ? !connectionClose : connectionKeepAlive;
    out->upgradeWebSocket = connectionUpgrade && upgradeWebSocket;
    out->headComplete = true;
    if (sawTransferEncoding) {
        size_t encodedLength = 0;
        size_t decodedLength = 0;
        BBHTTPParseResult result = bb_http_scan_chunked(buffer + headerLength,
            bufferLength - headerLength, &encodedLength, &decodedLength);
        if (result != BBHTTPParseReady) return result;
        out->chunked = true;
        out->bodyLength = encodedLength;
        out->decodedBodyLength = decodedLength;
        out->messageLength = headerLength + encodedLength;
        return BBHTTPParseReady;
    }
    out->bodyLength = contentLength;
    out->decodedBodyLength = contentLength;
    out->messageLength = headerLength + contentLength;
    if (contentLength > bufferLength - headerLength) return BBHTTPParseNeedMore;
    return BBHTTPParseReady;
}

bool bb_http_decode_chunked_body(const unsigned char *encoded,
                                 size_t encodedLength,
                                 unsigned char *out,
                                 size_t outCapacity,
                                 size_t *outLength)
{
    size_t completeLength = 0;
    size_t decodedLength = 0;
    if (bb_http_scan_chunked(encoded, encodedLength, &completeLength,
                             &decodedLength) != BBHTTPParseReady ||
        completeLength != encodedLength || decodedLength > outCapacity ||
        (decodedLength && !out)) return false;
    size_t cursor = 0;
    size_t outputCursor = 0;
    for (;;) {
        size_t lineEnd = bb_http_find_crlf(encoded, cursor, encodedLength);
        if (lineEnd == SIZE_MAX) return false;
        size_t chunkLength = 0;
        for (size_t i = cursor; i < lineEnd && encoded[i] != ';'; i++) {
            int digit = bb_http_hex_value(encoded[i]);
            if (digit < 0) return false;
            chunkLength = chunkLength * 16 + (size_t)digit;
        }
        cursor = lineEnd + 2;
        if (!chunkLength) break;
        bb_http_memory_copy(out + outputCursor, encoded + cursor, chunkLength);
        outputCursor += chunkLength;
        cursor += chunkLength + 2;
    }
    if (outLength) *outLength = outputCursor;
    return outputCursor == decodedLength;
}

bool bb_http_decode_chunked_body_in_place(unsigned char *encoded,
                                          size_t encodedLength,
                                          size_t *outLength)
{
    /* bb_http_decode_chunked_body copies forward one byte at a time and the
     * output cursor is always behind the encoded cursor (every chunk follows
     * its own size line), so aliasing the output onto the input is safe. */
    return bb_http_decode_chunked_body(encoded, encodedLength, encoded,
                                       encodedLength, outLength);
}

bool bb_http_percent_decode(const char *source, size_t length,
                            bool plusIsSpace, char *out, size_t capacity,
                            size_t *outLength)
{
    if (!out || capacity == 0 || (length && !source)) return false;
    size_t cursor = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)source[i];
        if (value == '%') {
            if (i + 2 >= length) return false;
            int high = bb_http_hex_value((unsigned char)source[i + 1]);
            int low = bb_http_hex_value((unsigned char)source[i + 2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (value == '+' && plusIsSpace) {
            value = ' ';
        }
        if (value == 0 || cursor + 1 >= capacity) return false;
        out[cursor++] = (char)value;
    }
    out[cursor] = '\0';
    if (outLength) *outLength = cursor;
    return true;
}

static bool bb_http_decode_component(const char *source, size_t length,
                                     char *out, size_t capacity,
                                     size_t *outLength)
{
    return bb_http_percent_decode(source, length, true, out, capacity, outLength);
}

static size_t bb_http_string_length(const char *value)
{
    size_t length = 0;
    if (!value) return 0;
    while (value[length]) length++;
    return length;
}

bool bb_http_query_value(const char *query,
                         const char *name,
                         char *out,
                         size_t outCapacity,
                         size_t *outLength)
{
    if (!query || !name || !out || outCapacity == 0) return false;
    out[0] = '\0';
    size_t queryLength = bb_http_string_length(query);
    size_t wantedLength = bb_http_string_length(name);
    size_t cursor = 0;
    char decodedName[128];
    while (cursor <= queryLength) {
        size_t end = cursor;
        while (end < queryLength && query[end] != '&') end++;
        size_t equals = cursor;
        while (equals < end && query[equals] != '=') equals++;
        size_t decodedNameLength = 0;
        if (!bb_http_decode_component(query + cursor, equals - cursor,
                                      decodedName, sizeof(decodedName),
                                      &decodedNameLength)) return false;
        bool matches = decodedNameLength == wantedLength;
        for (size_t i = 0; matches && i < wantedLength; i++) {
            if ((unsigned char)decodedName[i] != (unsigned char)name[i]) matches = false;
        }
        if (matches) {
            size_t valueStart = equals < end ? equals + 1 : end;
            return bb_http_decode_component(query + valueStart, end - valueStart,
                                            out, outCapacity, outLength);
        }
        if (end == queryLength) break;
        cursor = end + 1;
    }
    return false;
}
