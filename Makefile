build_native:
	$(MAKE) -C native
install: build_native
	mkdir -p Noita/mods/ApiMod
	cp -a native/api_mod.dll mod/init.lua mod/mod.xml Noita/mods/ApiMod
run: install
	cd Noita; env WINEPREFIX=$(CURDIR)/wineprefix wine noita.exe
run_dev: install
	cd Noita; env WINEPREFIX=$(CURDIR)/wineprefix wine noita_dev.exe
