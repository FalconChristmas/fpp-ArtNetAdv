include /opt/fpp/src/makefiles/common/setup.mk
include /opt/fpp/src/makefiles/platform/*.mk

all: libfpp-ArtNetAdv.so
debug: all

CFLAGS+=-I.
OBJECTS_fpp_ArtNetAdv_so += src/FPPArtNetAdv.o
LIBS_fpp_ArtNetAdv_so += -L/opt/fpp/src -lfpp
CXXFLAGS_src/FPPArtNetAdv.o += -I/opt/fpp/src


%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-ArtNetAdv.so: $(OBJECTS_fpp_ArtNetAdv_so) /opt/fpp/src/libfpp.so
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_ArtNetAdv_so) $(LIBS_fpp_ArtNetAdv_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-ArtNetAdv.so $(OBJECTS_fpp_ArtNetAdv_so)

