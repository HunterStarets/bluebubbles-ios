#include "BBMultipart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define MAX_PARTS 8
#define PART_CAPACITY 65536

typedef struct {
    BBMultipartPart info;
    unsigned char data[PART_CAPACITY];
    size_t length;
    bool ended;
} CapturedPart;

typedef struct {
    CapturedPart parts[MAX_PARTS];
    size_t count;
    bool inPart;
    unsigned dataCalls;
    size_t refuseAt;      /* refuse partData once this many bytes arrived */
} Capture;

static bool capture_begin(void *context, const BBMultipartPart *part)
{
    Capture *capture = (Capture *)context;
    if (capture->inPart || capture->count >= MAX_PARTS) return false;
    capture->parts[capture->count].info = *part;
    capture->parts[capture->count].length = 0;
    capture->parts[capture->count].ended = false;
    capture->inPart = true;
    return true;
}

static bool capture_data(void *context, const unsigned char *bytes, size_t length)
{
    Capture *capture = (Capture *)context;
    CapturedPart *part = &capture->parts[capture->count];
    if (!capture->inPart || !length) return false;
    capture->dataCalls++;
    if (part->length + length > PART_CAPACITY) return false;
    memcpy(part->data + part->length, bytes, length);
    part->length += length;
    return !capture->refuseAt || part->length < capture->refuseAt;
}

static bool capture_end(void *context)
{
    Capture *capture = (Capture *)context;
    if (!capture->inPart) return false;
    capture->parts[capture->count].ended = true;
    capture->count++;
    capture->inPart = false;
    return true;
}

static void capture_init(Capture *capture, BBMultipartCallbacks *callbacks)
{
    memset(capture, 0, sizeof(*capture));
    callbacks->partBegin = capture_begin;
    callbacks->partData = capture_data;
    callbacks->partEnd = capture_end;
    callbacks->context = capture;
}

/* A dio-shaped body: text fields, then the file, boundary "dartBoundary". */
static const char CONTENT_TYPE[] = "multipart/form-data; boundary=dartBoundary--x9";

static size_t build_body(unsigned char *out, size_t capacity, const unsigned char *file,
                         size_t fileLength)
{
    const char head[] =
        "--dartBoundary--x9\r\n"
        "content-disposition: form-data; name=\"chatGuid\"\r\n\r\n"
        "iMessage;-;+15555550100\r\n"
        "--dartBoundary--x9\r\n"
        "content-disposition: form-data; name=\"tempGuid\"\r\n\r\n"
        "temp-1\r\n"
        "--dartBoundary--x9\r\n"
        "content-disposition: form-data; name=\"name\"\r\n\r\n"
        "photo \"one\".jpg\r\n"
        "--dartBoundary--x9\r\n"
        "content-disposition: form-data; name=\"empty\"\r\n\r\n"
        "\r\n"
        "--dartBoundary--x9\r\n"
        "content-disposition: form-data; name=\"attachment\"; filename=\"photo \\\"one\\\".jpg\"\r\n"
        "content-type: image/jpeg\r\n"
        "content-transfer-encoding: binary\r\n"
        "\r\n";
    const char tail[] = "\r\n--dartBoundary--x9--\r\n";
    size_t length = 0;
    if (strlen(head) + fileLength + strlen(tail) > capacity) return 0;
    memcpy(out, head, strlen(head)); length += strlen(head);
    memcpy(out + length, file, fileLength); length += fileLength;
    memcpy(out + length, tail, strlen(tail)); length += strlen(tail);
    return length;
}

static void check_capture(const Capture *capture, const unsigned char *file, size_t fileLength)
{
    CHECK(capture->count == 5);
    CHECK(!capture->inPart);
    if (capture->count < 5) return;
    CHECK(strcmp(capture->parts[0].info.name, "chatGuid") == 0);
    CHECK(!capture->parts[0].info.hasFilename);
    CHECK(capture->parts[0].length == 23);
    CHECK(memcmp(capture->parts[0].data, "iMessage;-;+15555550100", 23) == 0);
    CHECK(strcmp(capture->parts[1].info.name, "tempGuid") == 0);
    CHECK(capture->parts[1].length == 6 && memcmp(capture->parts[1].data, "temp-1", 6) == 0);
    CHECK(strcmp(capture->parts[2].info.name, "name") == 0);
    CHECK(capture->parts[2].length == 15 && memcmp(capture->parts[2].data, "photo \"one\".jpg", 15) == 0);
    CHECK(strcmp(capture->parts[3].info.name, "empty") == 0);
    CHECK(capture->parts[3].length == 0 && capture->parts[3].ended);
    CHECK(strcmp(capture->parts[4].info.name, "attachment") == 0);
    CHECK(capture->parts[4].info.hasFilename);
    CHECK(strcmp(capture->parts[4].info.filename, "photo \"one\".jpg") == 0);
    CHECK(strcmp(capture->parts[4].info.contentType, "image/jpeg") == 0);
    CHECK(capture->parts[4].length == fileLength);
    CHECK(capture->parts[4].length == fileLength && memcmp(capture->parts[4].data, file, fileLength) == 0);
    CHECK(capture->parts[4].ended);
}

static void test_fragmentations(void)
{
    static unsigned char file[20000];
    static unsigned char body[24000];
    size_t bodyLength;
    Capture capture;
    BBMultipartCallbacks callbacks;
    BBMultipart parser;

    /* File bytes with every kind of near-miss: bare CR, CRLF, CRLF--,
     * CRLF--dartBoundary (short of the full boundary), and the full
     * boundary text without the leading CRLF. */
    for (size_t i = 0; i < sizeof(file); i++) file[i] = (unsigned char)(i * 31 + 7);
    memcpy(file + 100, "\r\n--dartBoundary--x", 19);
    memcpy(file + 500, "\r\n--dart", 8);
    memcpy(file + 900, "\r\n--", 4);
    memcpy(file + 1300, "\r\r\r\n", 4);
    memcpy(file + 1700, "--dartBoundary--x9\r\n", 20);
    memcpy(file + sizeof(file) - 19, "\r\n--dartBoundary--x", 19);
    bodyLength = build_body(body, sizeof(body), file, sizeof(file));
    CHECK(bodyLength != 0);

    /* Whole. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, CONTENT_TYPE, &callbacks));
    CHECK(bb_multipart_feed(&parser, body, bodyLength));
    CHECK(bb_multipart_finish(&parser));
    check_capture(&capture, file, sizeof(file));

    /* One byte at a time. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, CONTENT_TYPE, &callbacks));
    for (size_t i = 0; i < bodyLength; i++) CHECK(bb_multipart_feed(&parser, body + i, 1));
    CHECK(bb_multipart_finish(&parser));
    check_capture(&capture, file, sizeof(file));

    /* Growing odd chunks, and chunks that straddle every delimiter. */
    for (unsigned seed = 1; seed < 8; seed++) {
        size_t offset = 0;
        size_t piece = seed;
        capture_init(&capture, &callbacks);
        CHECK(bb_multipart_init(&parser, CONTENT_TYPE, &callbacks));
        while (offset < bodyLength) {
            size_t take = piece < bodyLength - offset ? piece : bodyLength - offset;
            CHECK(bb_multipart_feed(&parser, body + offset, take));
            offset += take;
            piece = (piece * 5 + seed) % 977 + 1;
        }
        CHECK(bb_multipart_finish(&parser));
        check_capture(&capture, file, sizeof(file));
    }

    /* Epilogue bytes after the close delimiter are ignored. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, CONTENT_TYPE, &callbacks));
    CHECK(bb_multipart_feed(&parser, body, bodyLength));
    CHECK(bb_multipart_feed(&parser, (const unsigned char *)"trailing junk", 13));
    CHECK(bb_multipart_finish(&parser));
    CHECK(capture.count == 5);
}

static void test_rejections(void)
{
    Capture capture;
    BBMultipartCallbacks callbacks;
    BBMultipart parser;
    const char simple[] =
        "--b\r\ncontent-disposition: form-data; name=\"a\"\r\n\r\n1\r\n--b--\r\n";

    capture_init(&capture, &callbacks);
    /* Content-Type variants. */
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=\"b\"", &callbacks));
    CHECK(bb_multipart_feed(&parser, (const unsigned char *)simple, strlen(simple)));
    CHECK(bb_multipart_finish(&parser) && capture.count == 1 && capture.parts[0].length == 1);
    CHECK(bb_multipart_init(&parser, "Multipart/Form-Data; charset=utf-8; boundary=b", &callbacks));
    CHECK(!bb_multipart_init(&parser, "application/json", &callbacks));
    CHECK(!bb_multipart_init(&parser, "multipart/form-data", &callbacks));
    CHECK(!bb_multipart_init(&parser, "multipart/form-data; boundary=", &callbacks));
    CHECK(!bb_multipart_init(&parser, "multipart/form-data; boundary=has space", &callbacks) ||
          parser.delimiterLength == 7); /* bare token stops at the space */
    {
        char longBoundary[200];
        memset(longBoundary, 'x', sizeof(longBoundary));
        memcpy(longBoundary, "multipart/form-data; boundary=", 30);
        longBoundary[30 + 71] = '\0';
        CHECK(!bb_multipart_init(&parser, longBoundary, &callbacks));
        longBoundary[30 + 70] = '\0';
        CHECK(bb_multipart_init(&parser, longBoundary, &callbacks));
    }
    CHECK(!bb_multipart_init(&parser, CONTENT_TYPE, NULL));
    CHECK(!bb_multipart_init(NULL, CONTENT_TYPE, &callbacks));

    /* Missing close delimiter: finish is false. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(bb_multipart_feed(&parser, (const unsigned char *)simple, strlen(simple) - 6));
    CHECK(!bb_multipart_finish(&parser));
    CHECK(capture.count == 0 && capture.inPart); /* data may still be held back */

    /* A part without Content-Disposition, or without a name, fails. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"--b\r\ncontent-type: text/plain\r\n\r\n1\r\n--b--\r\n", 44));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"x", 1)); /* failed for good */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"--b\r\ncontent-disposition: form-data; filename=\"f\"\r\n\r\n1\r\n--b--\r\n", 62));
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"--b\r\ncontent-disposition: attachment; name=\"a\"\r\n\r\n1\r\n--b--\r\n", 60));

    /* Garbage after a delimiter fails. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"--bXX", 5));

    /* An oversized part head fails. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(bb_multipart_feed(&parser, (const unsigned char *)"--b\r\ncontent-disposition: form-data; name=\"a\"\r\nx-filler: ", 55));
    {
        static unsigned char filler[BB_MULTIPART_MAX_PART_HEAD_BYTES + 8];
        memset(filler, 'f', sizeof(filler));
        CHECK(!bb_multipart_feed(&parser, filler, sizeof(filler)));
    }

    /* A refusing data callback fails the parser. */
    capture_init(&capture, &callbacks);
    capture.refuseAt = 1;
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)simple, strlen(simple)));

    /* Too many parts fails. */
    capture_init(&capture, &callbacks);
    CHECK(bb_multipart_init(&parser, "multipart/form-data; boundary=b", &callbacks));
    {
        const char part[] = "--b\r\ncontent-disposition: form-data; name=\"a\"\r\n\r\n1\r\n";
        bool ok = true;
        for (unsigned i = 0; ok && i <= BB_MULTIPART_MAX_PARTS; i++) {
            ok = bb_multipart_feed(&parser, (const unsigned char *)part, strlen(part));
            if (capture.count == MAX_PARTS) { capture.count = 0; } /* recycle capture slots */
        }
        CHECK(!ok);
    }

    /* Feeding a failed parser stays false; finish stays false. */
    CHECK(!bb_multipart_feed(&parser, (const unsigned char *)"x", 1));
    CHECK(!bb_multipart_finish(&parser));
    CHECK(!bb_multipart_finish(NULL));
}

int main(void)
{
    test_fragmentations();
    test_rejections();
    if (failures) return 1;
    puts("multipart tests passed: fragmentation, boundary look-alikes, escapes, empty parts, rejections");
    return 0;
}
