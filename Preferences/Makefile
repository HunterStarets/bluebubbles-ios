ARCHS = armv7
TARGET = iphone:clang:9.3:9.0

include $(THEOS)/makefiles/common.mk

BUNDLE_NAME = BlueBubblesiOSPrefs

BlueBubblesiOSPrefs_FILES = BBSRootListController.m
BlueBubblesiOSPrefs_FRAMEWORKS = UIKit
BlueBubblesiOSPrefs_PRIVATE_FRAMEWORKS = Preferences
BlueBubblesiOSPrefs_INSTALL_PATH = /Library/PreferenceBundles
BlueBubblesiOSPrefs_CFLAGS = -fobjc-exceptions -Wall

include $(THEOS_MAKE_PATH)/bundle.mk
