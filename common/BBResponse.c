#include "BBResponse.h"

/* Manual loops are volatile so the older armv7 Clang pass cannot lower them
 * into libc calls that the SpringBoard target must not import. */
static void bb_response_memory_set(void *destination, unsigned char value,
                                   size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static size_t bb_response_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

void bb_response_init(BBHTTPResponse *response)
{
    if (!response) return;
    bb_response_memory_set(response, 0, sizeof(*response));
    response->status = 200;
    response->cors = true;
}

const char *bb_response_status_text(int status)
{
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

/* ---- JSON writer ------------------------------------------------------ */

void bb_json_writer_init(BBJSONWriter *writer, char *out, size_t capacity)
{
    if (!writer) return;
    writer->out = out;
    writer->capacity = capacity;
    writer->length = 0;
    writer->overflow = !out || capacity == 0;
    if (!writer->overflow) out[0] = '\0';
}

static bool bb_json_put(BBJSONWriter *writer, char value)
{
    if (!writer || writer->overflow) return false;
    /* Keep one byte for the terminator written by finish. */
    if (writer->length + 1 >= writer->capacity) {
        writer->overflow = true;
        return false;
    }
    writer->out[writer->length++] = value;
    return true;
}

bool bb_json_write_bytes(BBJSONWriter *writer, const char *bytes, size_t length)
{
    if (!writer || writer->overflow || (length && !bytes)) return false;
    for (size_t i = 0; i < length; i++) {
        if (!bb_json_put(writer, bytes[i])) return false;
    }
    return true;
}

bool bb_json_write_raw(BBJSONWriter *writer, const char *text)
{
    return bb_json_write_bytes(writer, text, bb_response_string_length(text));
}

static const char bb_json_hex_digits[] = "0123456789abcdef";

bool bb_json_write_string(BBJSONWriter *writer, const char *value)
{
    if (!writer) return false;
    if (!value) return bb_json_write_raw(writer, "null");
    if (!bb_json_put(writer, '"')) return false;
    for (size_t i = 0; value[i]; i++) {
        unsigned char byte = (unsigned char)value[i];
        bool ok;
        switch (byte) {
            case '"': ok = bb_json_put(writer, '\\') && bb_json_put(writer, '"'); break;
            case '\\': ok = bb_json_put(writer, '\\') && bb_json_put(writer, '\\'); break;
            case '\b': ok = bb_json_put(writer, '\\') && bb_json_put(writer, 'b'); break;
            case '\f': ok = bb_json_put(writer, '\\') && bb_json_put(writer, 'f'); break;
            case '\n': ok = bb_json_put(writer, '\\') && bb_json_put(writer, 'n'); break;
            case '\r': ok = bb_json_put(writer, '\\') && bb_json_put(writer, 'r'); break;
            case '\t': ok = bb_json_put(writer, '\\') && bb_json_put(writer, 't'); break;
            default:
                if (byte < 0x20 || byte == 0x7f) {
                    ok = bb_json_put(writer, '\\') && bb_json_put(writer, 'u') &&
                         bb_json_put(writer, '0') && bb_json_put(writer, '0') &&
                         bb_json_put(writer, bb_json_hex_digits[byte >> 4]) &&
                         bb_json_put(writer, bb_json_hex_digits[byte & 0x0f]);
                } else {
                    /* Bytes >= 0x80 are passed through as UTF-8. */
                    ok = bb_json_put(writer, (char)byte);
                }
                break;
        }
        if (!ok) return false;
    }
    return bb_json_put(writer, '"');
}

bool bb_json_write_bool(BBJSONWriter *writer, bool value)
{
    return bb_json_write_raw(writer, value ? "true" : "false");
}

bool bb_json_write_null(BBJSONWriter *writer)
{
    return bb_json_write_raw(writer, "null");
}

bool bb_json_write_unsigned(BBJSONWriter *writer, uint64_t value)
{
    /* Repeated subtraction of powers of ten avoids the armv7 64-bit division
     * runtime helper while staying bounded at 20 digits. */
    static const uint64_t powers[] = {
        UINT64_C(10000000000000000000), UINT64_C(1000000000000000000),
        UINT64_C(100000000000000000), UINT64_C(10000000000000000),
        UINT64_C(1000000000000000), UINT64_C(100000000000000),
        UINT64_C(10000000000000), UINT64_C(1000000000000),
        UINT64_C(100000000000), UINT64_C(10000000000),
        UINT64_C(1000000000), UINT64_C(100000000), UINT64_C(10000000),
        UINT64_C(1000000), UINT64_C(100000), UINT64_C(10000), UINT64_C(1000),
        UINT64_C(100), UINT64_C(10), UINT64_C(1)
    };
    bool started = false;
    if (!writer) return false;
    for (size_t i = 0; i < sizeof(powers) / sizeof(powers[0]); i++) {
        unsigned int digit = 0;
        while (value >= powers[i]) {
            value -= powers[i];
            digit++;
        }
        if (digit || started || powers[i] == 1) {
            started = true;
            if (!bb_json_put(writer, (char)('0' + digit))) return false;
        }
    }
    return true;
}

bool bb_json_write_signed(BBJSONWriter *writer, int64_t value)
{
    if (!writer) return false;
    if (value < 0) {
        if (!bb_json_put(writer, '-')) return false;
        /* -(INT64_MIN) is not representable; negate in unsigned space. */
        return bb_json_write_unsigned(writer, (uint64_t)0 - (uint64_t)value);
    }
    return bb_json_write_unsigned(writer, (uint64_t)value);
}

bool bb_json_write_key(BBJSONWriter *writer, const char *key)
{
    if (!key) return false;
    return bb_json_write_string(writer, key) && bb_json_put(writer, ':');
}

bool bb_json_writer_finish(BBJSONWriter *writer, size_t *outLength)
{
    if (!writer || writer->overflow) return false;
    writer->out[writer->length] = '\0';
    if (outLength) *outLength = writer->length;
    return true;
}

/* ---- Envelopes -------------------------------------------------------- */

bool bb_response_success_envelope(char *out,
                                  size_t capacity,
                                  size_t *outLength,
                                  const char *message,
                                  const char *dataJSON)
{
    BBJSONWriter writer;
    if (!dataJSON) return false;
    bb_json_writer_init(&writer, out, capacity);
    return bb_json_write_raw(&writer, "{\"status\":200,\"message\":") &&
           bb_json_write_string(&writer, message ? message : "Success") &&
           bb_json_write_raw(&writer, ",\"data\":") &&
           bb_json_write_raw(&writer, dataJSON) &&
           bb_json_write_raw(&writer, "}") &&
           bb_json_writer_finish(&writer, outLength);
}

bool bb_response_envelope(char *out,
                          size_t capacity,
                          size_t *outLength,
                          int status,
                          const char *message,
                          const char *dataJSON, size_t dataLength,
                          const char *metadataJSON, size_t metadataLength,
                          const char *errorJSON, size_t errorLength)
{
    BBJSONWriter writer;
    if (status < 100 || status > 599) return false;
    if ((dataLength && !dataJSON) || (metadataLength && !metadataJSON) ||
        (errorLength && !errorJSON)) return false;
    bb_json_writer_init(&writer, out, capacity);
    if (!bb_json_write_raw(&writer, "{\"status\":") ||
        !bb_json_write_unsigned(&writer, (uint64_t)status) ||
        !bb_json_write_raw(&writer, ",\"message\":") ||
        !bb_json_write_string(&writer, message ? message :
                              bb_response_status_text(status))) return false;
    if (errorLength) {
        if (!bb_json_write_raw(&writer, ",\"error\":") ||
            !bb_json_write_bytes(&writer, errorJSON, errorLength)) return false;
        if (dataLength && (!bb_json_write_raw(&writer, ",\"data\":") ||
                           !bb_json_write_bytes(&writer, dataJSON, dataLength)))
            return false;
    } else {
        if (!bb_json_write_raw(&writer, ",\"data\":")) return false;
        if (dataLength) {
            if (!bb_json_write_bytes(&writer, dataJSON, dataLength)) return false;
        } else if (!bb_json_write_raw(&writer, "null")) {
            return false;
        }
    }
    if (metadataLength && (!bb_json_write_raw(&writer, ",\"metadata\":") ||
                           !bb_json_write_bytes(&writer, metadataJSON, metadataLength)))
        return false;
    return bb_json_write_raw(&writer, "}") && bb_json_writer_finish(&writer, outLength);
}

bool bb_response_error_envelope(char *out,
                                size_t capacity,
                                size_t *outLength,
                                int status,
                                const char *message,
                                const char *type,
                                const char *detail)
{
    BBJSONWriter writer;
    if (status < 100 || status > 599) return false;
    bb_json_writer_init(&writer, out, capacity);
    return bb_json_write_raw(&writer, "{\"status\":") &&
           bb_json_write_unsigned(&writer, (uint64_t)status) &&
           bb_json_write_raw(&writer, ",\"message\":") &&
           bb_json_write_string(&writer, message ? message :
                                bb_response_status_text(status)) &&
           bb_json_write_raw(&writer, ",\"error\":{\"type\":") &&
           bb_json_write_string(&writer, type ? type : "Server Error") &&
           bb_json_write_raw(&writer, ",\"message\":") &&
           bb_json_write_string(&writer, detail ? detail :
                                bb_response_status_text(status)) &&
           bb_json_write_raw(&writer, "}}") &&
           bb_json_writer_finish(&writer, outLength);
}

/* ---- Response head ---------------------------------------------------- */

typedef struct {
    char *out;
    size_t capacity;
    size_t length;
    bool overflow;
} BBHeadWriter;

static bool bb_head_write(BBHeadWriter *writer, const char *text)
{
    size_t length = bb_response_string_length(text);
    if (writer->overflow) return false;
    if (length > writer->capacity - writer->length - 1) {
        writer->overflow = true;
        return false;
    }
    volatile char *out = (volatile char *)writer->out + writer->length;
    for (size_t i = 0; i < length; i++) out[i] = text[i];
    writer->length += length;
    return true;
}

static bool bb_head_write_decimal(BBHeadWriter *writer, size_t value)
{
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && count < sizeof(digits));
    if (writer->overflow || count > writer->capacity - writer->length - 1) {
        writer->overflow = true;
        return false;
    }
    while (count) writer->out[writer->length++] = digits[--count];
    return true;
}

static bool bb_response_extra_headers_valid(const char *extra)
{
    /* Each line must be "Name: value\r\n" with no bare CR/LF or controls. */
    size_t i = 0;
    if (!extra) return true;
    while (extra[i]) {
        size_t start = i;
        bool colon = false;
        while (extra[i] && extra[i] != '\r') {
            unsigned char byte = (unsigned char)extra[i];
            if (byte == '\n' || (byte < 0x20 && byte != '\t') || byte == 0x7f)
                return false;
            if (byte == ':') colon = true;
            i++;
        }
        if (!colon || i == start || extra[i] != '\r' || extra[i + 1] != '\n')
            return false;
        i += 2;
    }
    return true;
}

bool bb_response_write_head(const BBHTTPResponse *response,
                            bool keepAlive,
                            char *out,
                            size_t capacity,
                            size_t *outLength)
{
    BBHeadWriter writer;
    if (!response || !out || capacity == 0) return false;
    if (response->status < 100 || response->status > 599) return false;
    if (!bb_response_extra_headers_valid(response->extraHeaders)) return false;
    if (response->bodyLength && !response->body) return false;
    writer.out = out;
    writer.capacity = capacity;
    writer.length = 0;
    writer.overflow = false;

    bool upgrade = response->status == 101;
    if (!bb_head_write(&writer, "HTTP/1.1 ") ||
        !bb_head_write_decimal(&writer, (size_t)response->status) ||
        !bb_head_write(&writer, " ") ||
        !bb_head_write(&writer, bb_response_status_text(response->status)) ||
        !bb_head_write(&writer, "\r\n")) return false;
    if (response->contentType) {
        if (!bb_head_write(&writer, "Content-Type: ") ||
            !bb_head_write(&writer, response->contentType) ||
            !bb_head_write(&writer, "\r\n")) return false;
    }
    if (!upgrade) {
        if (!bb_head_write(&writer, "Content-Length: ") ||
            !bb_head_write_decimal(&writer, response->bodyLength) ||
            !bb_head_write(&writer, "\r\n")) return false;
    }
    if (upgrade) {
        if (!bb_head_write(&writer, "Connection: Upgrade\r\n")) return false;
    } else if (keepAlive && !response->closeConnection) {
        if (!bb_head_write(&writer, "Connection: keep-alive\r\n")) return false;
    } else {
        if (!bb_head_write(&writer, "Connection: close\r\n")) return false;
    }
    if (response->cors) {
        if (!bb_head_write(&writer,
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type,Range\r\n"))
            return false;
    }
    if (response->extraHeaders && !bb_head_write(&writer, response->extraHeaders))
        return false;
    if (!bb_head_write(&writer, "\r\n")) return false;
    out[writer.length] = '\0';
    if (outLength) *outLength = writer.length;
    return true;
}
