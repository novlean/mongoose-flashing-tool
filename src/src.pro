TEMPLATE = app
TARGET = "MFT"
INCLUDEPATH += .
QT += serialport network
CONFIG += c++11

CONFIG(asan) {
  QMAKE_CC = clang-3.6
  QMAKE_CFLAGS += -fsanitize=address -fcolor-diagnostics
  QMAKE_CXX = clang++-3.6
  QMAKE_CXXFLAGS += -fsanitize=address -fcolor-diagnostics
  QMAKE_LINK = clang++-3.6
  QMAKE_LFLAGS_DEBUG += -fsanitize=address
}

exists(../common) {
  COMMON_PATH = ../common
  INCLUDEPATH += ..
} else {
  COMMON_PATH = ../../common
  INCLUDEPATH += ../..
}
SPIFFS_PATH = $${COMMON_PATH}/spiffs
UTIL_PATH = $${COMMON_PATH}/util

QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.9

# Input

HEADERS += \
  app_init.h \
  cc3200.h \
  cli.h \
  config.h \
  esp8266.h \
  esp_flasher_client.h \
  esp_rom_client.h \
  file_downloader.h \
  flasher.h \
  fs.h \
  fw_bundle.h \
  fw_client.h \
  log.h \
  prompter.h \
  serial.h \
  sigsource.h \
  slip.h \
  status_qt.h

SOURCES += \
  app_init.cc \
  build_info.cc \
  cc3200.cc \
  cli.cc \
  config.cc \
  esp8266.cc \
  esp_flasher_client.cc \
  esp_rom_client.cc \
  file_downloader.cc \
  flasher.cc \
  fs.cc \
  fw_bundle.cc \
  fw_bundle_zip.cc \
  fw_client.cc \
  log.cc \
  serial.cc \
  slip.cc \
  status_qt.cc

CONFIG(cli) {
  QT -= gui
  SOURCES += main_cli.cc
  TARGET = $${TARGET}-cli
} else { # GUI
  QT += widgets
  HEADERS += about_dialog.h dialog.h gui_prompter.h log_viewer.h settings.h progress_widget/progress_widget.h wizard/wizard.h
  SOURCES += about_dialog.cc dialog.cc gui_prompter.cc log_viewer.cc main.cc settings.cc progress_widget/progress_widget.cc wizard/wizard.cc
  INCLUDEPATH += progress_widget
}

CONFIG(static):CONFIG(unix) {
  LIBS += -static
  !CONFIG(cli) {
    # These are deps of the libraries and are needed to link the GUI binary.
    EXTRA_STATIC_LIBS = -lexpat -lffi -lpcre -lXau -lxcb-util -lXdmcp -lXext
  }
  # One does not simply link statically on UNIX/Linux.
  # The resulting binary is semi-static - system libraries such as libstdc++
  # and libpthread are still dynamically loaded, but for them ABI backward
  # compatibility is usually maintained pretty well.
  # And then there is libudev, which is a special snowflake: systemd authors
  # refuse to provide static version of it.
  # To implement these tweaks, we have to rewrite the link command.
  QMAKE_LINK = ./link-semi-static.py \
    --append_static="'$${EXTRA_STATIC_LIBS}'" \
    --force_dynamic="'-ldl -lglib-2.0 -lgobject-2.0 -lgthread-2.0 -lm -lpthread -lrt -ludev'" \
    -- $${QMAKE_LINK}
}

CONFIG(static):CONFIG(win32) {
  QMAKE_LFLAGS_RELEASE += -static -static-libgcc -static-libstdc++
}

DEFINES += VERSION=\\\"$$VERSION\\\"
DEFINES += APP_NAME=\\\"$$TARGET\\\"

INCLUDEPATH += $${UTIL_PATH}
SOURCES += \
  $${UTIL_PATH}/error_codes.cc \
  $${UTIL_PATH}/logging.cc \
  $${UTIL_PATH}/status.cc

INCLUDEPATH += $${SPIFFS_PATH}
SOURCES += \
  $${SPIFFS_PATH}/spiffs_cache.c \
  $${SPIFFS_PATH}/spiffs_gc.c \
  $${SPIFFS_PATH}/spiffs_nucleus.c \
  $${SPIFFS_PATH}/spiffs_check.c \
  $${SPIFFS_PATH}/spiffs_hydrogen.c
DEFINES += SPIFFS_TEST_VISUALISATION=1 SPIFFS_HAL_CALLBACK_EXTRA=1

unix {
  SOURCES += sigsource_unix.cc
} else {
  SOURCES += sigsource_dummy.cc
}

RESOURCES = blobs.qrc images.qrc wizard/wizard.qrc
FORMS = main.ui about.ui log_viewer.ui settings.ui wizard/wizard.ui

# libftdi stuff.
macx {
  # Works for libftdi installed with Homebrew: brew install libftdi
  INCLUDEPATH += /usr/local/include/libftdi1
  LIBS += -L/usr/local/lib -lftdi1
} else:unix {
  # Works on recent Ubuntu: apt-get install libftdi-dev
  INCLUDEPATH += /usr/include
  LIBS += -lftdi -lusb
}
win32 {
 DEFINES += NO_LIBFTDI
}

macx {
  QMAKE_INFO_PLIST = Info.plist.in
  ICON = images/mg_iot.icns
}

win32 {
  QMAKE_TARGET_COMPANY = "Cesanta"
  RC_ICONS = images/mg_iot.ico
}

# Build info generation.
# It's too complicated to do under Wine, so we rely on build_info.cc to be
# generated externally.
!win32 {
  DUMMY_INPUT = .
  build_info.output = build_info.cc
  build_info.input = DUMMY_INPUT
  build_info.commands = $${COMMON_PATH}/tools/fw_meta.py gen_build_info \
    --tag_as_version=true --c_output=${QMAKE_FILE_OUT}
  build_info.name = build_info
  build_info.variable_out = DUMMY_OUTPUT
  build_info.CONFIG = no_link
  QMAKE_EXTRA_COMPILERS += build_info
}

!win32:QMAKE_CLEAN += -r $$TARGET
win32:QMAKE_CLEAN += /s /f /q $$TARGET 
