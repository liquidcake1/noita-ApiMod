REPRODUCIBILE_BUILD_OPTIONS=-Wl,--no-insert-timestamp

EXTRA_BUILD_OPTS=

#                   Optimisations  Enable warnings  Fail on warnings  Make DLL  Strip symbols  Include GCC gumf (probably missing on Windows)
BUILD_OPTS=-std=c99 -O2            -Wall            -Werror           -shared   -s             -static-libgcc $(REPRODUCIBILE_BUILD_OPTIONS) $(EXTRA_BUILD_OPTS)

CC=i686-w64-mingw32-gcc-win32
DLLTOOL=i686-w64-mingw32-dlltool

SOURCES=api_mod.c destroy_proxy.c hooks.c proxy.c proxy_metatables.c transfer.c util.c
EXT_SOURCES=minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde32.c

all: dbroot/build/api_mod.dll

lua51.a: lua51.def
	$(DLLTOOL) --dllname lua51.dll --input-def lua51.def --output-lib lua51.a

BUILD_DEPS = *.h luajit/src/*.h minhook/include/*.h lua51.a Makefile

api_mod.dll: $(SOURCES) $(EXT_SOURCES) $(BUILD_DEPS)
	$(CC) $(BUILD_OPTS) -Iluajit/src -Iminhook/include -o api_mod.dll $(SOURCES) $(EXT_SOURCES) lua51.a

.PHONY:
clean: .PHONY
	rm -rf api_mod.dll liblua51.a dbroot

# Makes about a 1GB environment completely frozen at our point in time.
# Needs: apt install fakechroot fakeroot debootstrap
dbroot:
	fakechroot fakeroot debootstrap trixie dbroot https://snapshot.debian.org/archive/debian/20261116T000000Z/
	fakechroot fakeroot chroot dbroot apt install -yy binutils-mingw-w64-i686 gcc-mingw-w64-i686-win32 make

dbroot/build/api_mod.dll: dbroot $(SOURCES) $(EXT_SOURCES) $(BUILD_DEPS)
	rsync --delete --delete-excluded --exclude=.git -a *.[ch] lua51.def Makefile luajit minhook dbroot/build
	fakechroot fakeroot chroot dbroot bash -c 'cd /build && make api_mod.dll'
