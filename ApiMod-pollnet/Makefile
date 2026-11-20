all: package

# Makes about a 1GB environment completely frozen at our point in time.
# Needs: apt install fakechroot fakeroot debootstrap
dbroot:
	# Of course, we're using rustup so we have no idea how reproducible any
	# of the rust stuff is as it'll use the latest index and we can't lock
	# to a SHA or anything like this.
	fakechroot fakeroot debootstrap trixie dbroot https://snapshot.debian.org/archive/debian/20261116T000000Z/
	fakechroot fakeroot chroot dbroot apt install -yy rustup gcc-mingw-w64-i686-win32 gcc
	#fakechroot fakeroot chroot dbroot rustup rustup default 1.100.0
	#fakechroot fakeroot chroot dbroot rustup target add i686-pc-windows-gnu
	# Rustup docs do not explain how to install only a cross compile; the above will install native too.
	# I had to steal the below from Claude as rustup options are a mystery to me.
	fakechroot fakeroot chroot dbroot rustup toolchain install 1.100.0 --profile minimal --target i686-pc-windows-gnu

RUST_SOURCES := $(shell find pollnet/src pollnet/Cargo.* -type f)

pollnet.dll: dbroot $(RUST_SOURCES)
	echo $(RUST_SOURCES)
	rsync --delete -r --exclude=target pollnet/. dbroot/build/
	# Woo, hidden only half-documented environment variables! I had to steal these from
	# Claude, too; there seems to be really scant docs on how to do this stuff.
	fakechroot fakeroot chroot dbroot bash -c 'cd /build && RUSTUP_TOOLCHAIN=1.100.0 CARGO_TARGET_I686_PC_WINDOWS_GNU_LINKER=i686-w64-mingw32-gcc RUSTFLAGS="-C link-arg=-Wl,--no-insert-timestamp" cargo build --target=i686-pc-windows-gnu --release --locked'
	rsync dbroot/build/target/i686-pc-windows-gnu/release/pollnet.dll .

# Strictly, depends on sha256sums but if it does then it will always pass...
.PHONY:
check_sha256sums: .PHONY
	sha256sum --check sha256sums

PACKAGE_CONTENTS=mod/mod.xml mod/init.lua pollnet/bindings/luajit/pollnet.lua pollnet.dll

sha256sums: $(PACKAGE_CONTENTS)
	sha256sum $(PACKAGE_CONTENTS) > sha256sums

.PHONY:
clean: .PHONY
	rm -rf package dbroot pollnet.dll

package: $(PACKAGE_CONTENTS) check_sha256sums
	rsync $(PACKAGE_CONTENTS) package/
