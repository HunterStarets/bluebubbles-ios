CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -I../include

TESTS = websocket_test http_test socketio_test json_test serialize_test multipart_test connection_test engineio_test bridge_test

.PHONY: all check sanitize clean

all: $(TESTS)

check: all
	./websocket_test
	./http_test
	./socketio_test
	./json_test
	./serialize_test
	./multipart_test
	./connection_test
	./engineio_test
	./bridge_test
	node --test bluebubbles-contract-test.js

websocket_test: websocket_test.c ../common/BBWebSocket.c ../include/BBWebSocket.h
	$(CC) $(CFLAGS) websocket_test.c ../common/BBWebSocket.c -o websocket_test

http_test: http_test.c ../common/BBHTTP.c ../include/BBHTTP.h
	$(CC) $(CFLAGS) http_test.c ../common/BBHTTP.c -o http_test

socketio_test: socketio_test.c ../common/BBSocketIO.c ../include/BBSocketIO.h
	$(CC) $(CFLAGS) socketio_test.c ../common/BBSocketIO.c -o socketio_test

json_test: json_test.c ../common/BBJSON.c ../include/BBJSON.h
	$(CC) $(CFLAGS) json_test.c ../common/BBJSON.c -o json_test

serialize_test: serialize_test.c ../common/BBSerialize.c ../common/BBResponse.c ../common/BBJSON.c ../include/BBSerialize.h
	$(CC) $(CFLAGS) serialize_test.c ../common/BBSerialize.c ../common/BBResponse.c ../common/BBJSON.c -o serialize_test

multipart_test: multipart_test.c ../common/BBMultipart.c ../include/BBMultipart.h
	$(CC) $(CFLAGS) multipart_test.c ../common/BBMultipart.c -o multipart_test

TRANSPORT_SOURCES = ../common/BBConnection.c ../common/BBRouter.c ../common/BBResponse.c ../common/BBMultipart.c \
	../common/BBHTTP.c ../common/BBEngineIO.c ../common/BBSocketIO.c ../common/BBWebSocket.c \
	../common/BBJSON.c ../common/BBRequestTable.c ../common/BBEvents.c
TRANSPORT_HEADERS = ../include/BBConnection.h ../include/BBRouter.h ../include/BBResponse.h \
	../include/BBHTTP.h ../include/BBEngineIO.h ../include/BBSocketIO.h ../include/BBWebSocket.h \
	../include/BBJSON.h ../include/BBRequestTable.h ../include/BBEvents.h

connection_test: connection_test.c $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS)
	$(CC) $(CFLAGS) connection_test.c $(TRANSPORT_SOURCES) -o connection_test

engineio_test: engineio_test.c $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS)
	$(CC) $(CFLAGS) engineio_test.c $(TRANSPORT_SOURCES) -o engineio_test

bridge_test: bridge_test.c $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS)
	$(CC) $(CFLAGS) bridge_test.c $(TRANSPORT_SOURCES) -o bridge_test

# The same C suites under AddressSanitizer/UBSan. Node tests are unchanged.
SANITIZE_FLAGS = -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE_FLAGS)" check
	$(MAKE) clean

clean:
	rm -f $(TESTS)
