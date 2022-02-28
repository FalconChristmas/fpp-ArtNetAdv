
SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-ArtNetAdv.$(SHLIB_EXT)
debug: all

CFLAGS+=-I.
OBJECTS_fpp_ArtNetAdv_so += src/FPPArtNetAdv.o
LIBS_fpp_ArtNetAdv_so += -L$(SRCDIR) -lfpp -ljsoncpp
CXXFLAGS_src/FPPArtNetAdv.o += -I$(SRCDIR)


%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-ArtNetAdv.$(SHLIB_EXT): $(OBJECTS_fpp_ArtNetAdv_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_ArtNetAdv_so) $(LIBS_fpp_ArtNetAdv_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-ArtNetAdv.so $(OBJECTS_fpp_ArtNetAdv_so)

