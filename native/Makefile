# Nicked from Opus; this is probably a sensible set of options. It wanted std=gnu99.
# For reproducible builds, also probably want -Wl,--no-insert-timestamp,--disable-auto-image-base
#                   Optimisations  Enable warnings  Fail on warnings  Make DLL  Strip symbols  Include GCC gumf (probably missing on Windows)
BUILD_OPTS=-std=c99 -O2            -Wall            -Werror           -shared   -s             -static-libgcc
CC=i686-w64-mingw32-gcc-win32

all: api_mod.dll

api_mod.dll: api_mod.c
	$(CC) $(BUILD_OPTS) -o api_mod.dll api_mod.c
