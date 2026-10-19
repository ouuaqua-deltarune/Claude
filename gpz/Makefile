CC ?= cc
CFLAGS ?= -O2
LDLIBS = -ldl

gpz: gpz.c codec.h common.h cmcore.h codec_cl.inc gpz2.h gpucm.h arch.h
	$(CC) $(CFLAGS) -std=gnu99 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -o $@ gpz.c $(LDLIBS) -lpthread

# embed codec.h as a C string so the OpenCL kernels need no extra files
codec_cl.inc: common.h codec.h cmcore.h
	( echo 'static const char GPZ_CL_SRC[] ='; \
	  cat common.h codec.h cmcore.h | grep -v '^#include "' | \
	    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/'; \
	  echo ';' ) > $@

test: gpz
	sh test.sh

clean:
	rm -f gpz codec_cl.inc
