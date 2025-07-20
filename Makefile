PROG := edge_gunshot_detector
SRCS := gunshot_detector.c

# Package configuration (v1.1.95 real audio - full PipeWire dependencies + email notifications)
PKGS = gio-2.0 gio-unix-2.0 liblarod libpipewire-0.3 libcurl

# Compiler flags
CFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith -Wbad-function-cast \
          -Wstrict-prototypes -Wmissing-prototypes -Winline -Wdisabled-optimization \
          -Wfloat-equal -Wundef -Wcast-align -Wstrict-overflow=5 -Wunreachable-code \
          -Wlogical-op -Wredundant-decls -Wold-style-definition -Wno-unused-parameter \
          -I./include

# Add LAROD API version (following vdo-larod pattern)
CFLAGS += -DLAROD_API_VERSION_3

# Use pkg-config for package flags
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

# Add math and FFTW libraries
LDFLAGS += -lm -L./lib -lfftw3f -Wl,-rpath,\$$ORIGIN/lib

# Build rules
all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

# EAP package creation (v1.1.91)
eap: $(PROG)
	cp $(PROG) LICENSE manifest.json.cv25 package.conf gunshot_model_real_audio.tflite param.conf /tmp/
	cd /tmp && tar cf $(PROG)_cv25_1_1_91_aarch64.eap \
		$(PROG) LICENSE manifest.json.cv25 package.conf gunshot_model_real_audio.tflite param.conf
	mv /tmp/$(PROG)_cv25_1_1_91_aarch64.eap ./

clean:
	rm -f $(PROG) *.o *.eap

.PHONY: all eap clean