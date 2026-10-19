CC ?= cc
CFLAGS ?= -O2
LDLIBS = -ldl

gpz: gpz.c codec.h codec_cl.inc cm.h gpz2.h
	$(CC) $(CFLAGS) -std=c99 -Wall -Wextra -o $@ gpz.c $(LDLIBS)

# embed codec.h as a C string so the OpenCL kernels need no extra files
codec_cl.inc: codec.h
	( echo 'static const char GPZ_CL_SRC[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' codec.h; \
	  echo ';' ) > $@

test: gpz
	sh test.sh

clean:
	rm -f gpz codec_cl.inc
