include mandoc/Makefile.local

CFLAGS = -g -O2 -Wall $(shell pkg-config --cflags zlib gl freetype2 glfw3)
LDFLAGS = -lm $(shell pkg-config --libs zlib gl freetype2 glfw3) ${LDADD}

LIBMAN_OBJS	 = mandoc/man.o \
			   mandoc/man_macro.o \
			   mandoc/man_validate.o

LIBMDOC_OBJS	 = mandoc/att.o \
				   mandoc/lib.o \
				   mandoc/mdoc.o \
				   mandoc/mdoc_argv.o \
				   mandoc/mdoc_macro.o \
				   mandoc/mdoc_state.o \
				   mandoc/mdoc_validate.o \
				   mandoc/st.o

LIBROFF_OBJS	 = mandoc/eqn.o \
				   mandoc/roff.o \
				   mandoc/roff_validate.o \
				   mandoc/tbl.o \
				   mandoc/tbl_data.o \
				   mandoc/tbl_layout.o \
				   mandoc/tbl_opts.o

LIBMANDOC_OBJS	 = $(LIBMAN_OBJS) \
				   $(LIBMDOC_OBJS) \
				   $(LIBROFF_OBJS) \
				   mandoc/arch.o \
				   mandoc/chars.o \
				   mandoc/mandoc.o \
				   mandoc/mandoc_aux.o \
				   mandoc/mandoc_msg.o \
				   mandoc/mandoc_ohash.o \
				   mandoc/mandoc_xr.o \
				   mandoc/msec.o \
				   mandoc/preconv.o \
				   mandoc/read.o

COMPAT_OBJS	 = ${MANDOC_COBJS:%=mandoc/%}

MANGL_SOURCES = mandoc/tree.c \
				mandoc/mdoc_term.c \
				mandoc/man_term.c \
				mandoc/tbl_term.c \
				mandoc/tag.c \
				mandoc/roff_term.c \
				mandoc/eqn_term.c \
				mandoc/term_ascii.c \
				mandoc/term.c \
				mandoc/term_tab.c \
				mandoc/term_tag.c \
				mandoc/out.c \
				hashmap.c \
				main.c

mangl: $(COMPAT_OBJS) $(LIBMANDOC_OBJS) $(MANGL_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(COMPAT_OBJS) $(LIBMANDOC_OBJS) $(MANGL_SOURCES) $(LDFLAGS)

sanitizer: CFLAGS += -fsanitize=address
sanitizer: mangl

.PHONY: install
install: mangl
	cp mangl /usr/local/bin/

.PHONY: clean
clean:
	rm -f mangl
	rm -f *.o
	rm -f mandoc/*.o
