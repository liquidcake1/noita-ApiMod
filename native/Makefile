# Nicked from Opus; this is probably a sensible set of options. It wanted std=gnu99.
# For reproducible builds, also probably want -Wl,--no-insert-timestamp,--disable-auto-image-base
#                   Optimisations  Enable warnings  Fail on warnings  Make DLL  Strip symbols  Include GCC gumf (probably missing on Windows)
EXTRA_BUILD_OPTS=
BUILD_OPTS=-std=c99 -O2            -Wall            -Werror           -shared   -s             -static-libgcc $(EXTRA_BUILD_OPTS)
CC=i686-w64-mingw32-gcc-win32
DLLTOOL=i686-w64-mingw32-dlltool

all: api_mod.dll

lua51.a: lua51.def
	$(DLLTOOL) --dllname lua51.dll --input-def lua51.def --output-lib lua51.a

api_mod.dll: lua51.a api_mod.c
	$(CC) $(BUILD_OPTS) -Iluajit/src -Iminhook/include -o api_mod.dll api_mod.c minhook/src/*.c minhook/src/hde/hde32.c lua51.a

.PHONY:
clean: .PHONY
	rm -f api_mod.dll liblua51.a
