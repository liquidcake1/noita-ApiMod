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
