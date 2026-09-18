# Point THEOS at your Theos checkout (with the iPhoneOS 9.3 SDK installed);
# see docs/BUILDING.md.
export THEOS ?= $(HOME)/theos

ARCHS = armv7
TARGET = iphone:clang:9.3:9.0

PACKAGE_VERSION := $(shell sed -n 's/^Version: //p' control)
# The count-only trace lines and /tmp/bbdiag-<version>-* file names carry it.
BB_VERSION_CFLAGS = -DBB_BUILD_VERSION='"$(PACKAGE_VERSION)"'

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = BBServer BBBridge

# SpringBoard: the HTTP/Socket.IO server, built from the host-tested BB*
# C modules (no Foundation) plus the CFSocket/CFStream owner.
BBServer_FILES = BBServer/Tweak.xm BBServer/SpringBoardHook.xm BBServer/BBServerTransport.xm \
	common/BBHTTP.c common/BBSocketIO.c common/BBMultipart.c common/BBResponse.c \
	common/BBRouter.c common/BBConnection.c common/BBEngineIO.c common/BBJSON.c \
	common/BBRequestTable.c common/BBEvents.c common/BBWebSocket.c common/BBTrace.c
BBServer_CFLAGS = -I$(THEOS)/vendor/include -Iinclude -fobjc-exceptions -fno-builtin $(BB_VERSION_CFLAGS)
BBServer_FRAMEWORKS = Foundation CoreFoundation CFNetwork Security UIKit
BBServer_PRIVATE_FRAMEWORKS = AppSupport
BBServer_LIBRARIES = rocketbootstrap commonCrypto
BBServer_LDFLAGS = -L$(THEOS)/sdks/iPhoneOS9.3.sdk/usr/lib/system

# MobileSMS: the IMCore/ChatKit bridge answering typed IPC requests.
BBBridge_FILES = BBBridge/Tweak.xm BBBridge/BBBridgeRequests.xm \
	common/BBSerialize.c common/BBJSON.c common/BBResponse.c common/BBTrace.c
BBBridge_CFLAGS = -I$(THEOS)/vendor/include -Iinclude -fobjc-exceptions -fno-builtin $(BB_VERSION_CFLAGS)
BBBridge_FRAMEWORKS = Foundation CoreFoundation AVFoundation AddressBook
BBBridge_PRIVATE_FRAMEWORKS = AppSupport ChatKit IMCore
BBBridge_LIBRARIES = rocketbootstrap

include $(THEOS_MAKE_PATH)/tweak.mk

SUBPROJECTS = Preferences
include $(THEOS_MAKE_PATH)/aggregate.mk

after-stage::
	@mkdir -p $(THEOS_STAGING_DIR)/DEBIAN
