BUILD_ARCH := $(shell uname -m)
RELEASE_NAME := "dragonfly-${BUILD_ARCH}"

HELIO_RELEASE := $(if $(HELIO_RELEASE),y,)

HELIO_RELEASE_FLAGS = -DHELIO_RELEASE_FLAGS="-g1 -gz"
HELIO_USE_STATIC_LIBS = ON
HELIO_OPENSSL_USE_STATIC_LIBS = ON
HELIO_ENABLE_GIT_VERSION = ON
HELIO_WITH_UNWIND = OFF

# Some distributions (old fedora) have incorrect dependencies for crypto
# so we add -lz for them.
LINKER_FLAGS=-lz

# equivalent to: if $(uname_m) == x86_64 || $(uname_m) == amd64
ifneq (, $(filter $(BUILD_ARCH),x86_64 amd64))
HELIO_MARCH_OPT := -march=core2 -msse4.1 -mpopcnt -mtune=skylake
endif

# For release builds we link statically libstdc++ and libgcc. Currently,
# all the release builds are performed by gcc.
ifeq ($(HELIO_RELEASE),y)
	LINKER_FLAGS += -static-libstdc++ -static-libgcc
endif

HELIO_FLAGS = $(if $(HELIO_RELEASE),-release $(HELIO_RELEASE_FLAGS),) \
			  -DCMAKE_EXE_LINKER_FLAGS="$(LINKER_FLAGS)" \
              -DBoost_USE_STATIC_LIBS=$(HELIO_USE_STATIC_LIBS) \
              -DOPENSSL_USE_STATIC_LIBS=$(HELIO_OPENSSL_USE_STATIC_LIBS) \
              -DENABLE_GIT_VERSION=$(HELIO_ENABLE_GIT_VERSION) \
              -DWITH_UNWIND=$(HELIO_WITH_UNWIND) -DMARCH_OPT="$(HELIO_MARCH_OPT)"

.PHONY: default

configure:
	./helio/blaze.sh $(HELIO_FLAGS)

build:
	cd build-opt; \
	ninja dragonfly && ldd dragonfly

build-debug:
	cd build-dbg; \
	ninja dragonfly && ldd dragonfly

package:
	cd build-opt; \
	mv dragonfly $(RELEASE_NAME); \
	tar cvfz $(RELEASE_NAME).tar.gz $(RELEASE_NAME) ../LICENSE.md

release: configure build

release-debug: configure build-debug

default: release
