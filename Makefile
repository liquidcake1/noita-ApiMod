.PHONY:
package: .PHONY
	$(MAKE) -C ApiMod package
	$(MAKE) -C ApiMod-pollnet package

# Refresh the sha256sums so that package can run
.PHONY:
sha256sums: .PHONY
	$(MAKE) -C ApiMod sha256sums
	$(MAKE) -C ApiMod-pollnet sha256sums

install: package
	rsync --delete -a ApiMod/package/* Noita/mods/ApiMod/
	rsync --delete -a ApiMod-pollnet/package/* Noita/mods/ApiMod-pollnet/

run: install
	cd Noita; env WINEPREFIX=$(CURDIR)/wineprefix wine noita.exe
run_dev: install
	cd Noita; env WINEPREFIX=$(CURDIR)/wineprefix wine noita_dev.exe

.PHONY:
ApiMod/native/test.exe: .PHONY
	$(MAKE) -C ApiMod/native test.exe

TESTS = ${shell find tests -name '*.lua' -printf "../%p\n" | sort}

# Needs Noita/lua51.dll but nothing else.
test: ApiMod/native/test.exe install
	cd Noita && wine ../ApiMod/native/test.exe ../ApiMod/mod/init.lua ../ApiMod-pollnet/mod/init.lua $(TESTS)
