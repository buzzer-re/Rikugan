# Loaded after ../ohmyc/Makefile (make -f Makefile -f this): prints the library object list and
# the link libraries for this platform, so Rios links exactly what `make lib` would archive.
rios-lib-obj:
	@echo $(LIB_OBJ)

rios-libs:
	@echo $(LIBS)

.PHONY: rios-lib-obj rios-libs
