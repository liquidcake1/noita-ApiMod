clean:
	$(MAKE) -C native clean
	rm -rf package

MOD_SOURCES := mod/init.lua mod/mod.xml native/dbroot/build/api_mod.dll

.PHONY:
native/dbroot/build/api_mod.dll: .PHONY
	$(MAKE) -C native dbroot/build/api_mod.dll

# Strictly, depends on sha256sums but if it does then it will always pass...
.PHONY:
check_sha256sums: .PHONY
	sha256sum --check sha256sums

sha256sums: $(MOD_SOURCES)
	sha256sum $(MOD_SOURCES) > sha256sums

package: $(MOD_SOURCES) check_sha256sums
	rsync --delete -r $(MOD_SOURCES) package/
