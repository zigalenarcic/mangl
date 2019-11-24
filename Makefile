CFLAGS = -g -O2 -Wall $(shell pkg-config --cflags zlib gl freetype2)
LDFLAGS = -lm -lglut $(shell pkg-config --libs zlib gl freetype2)

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

COMPAT_OBJS	 = mandoc/compat_err.o \
			   mandoc/compat_fts.o \
			   mandoc/compat_getline.o \
			   mandoc/compat_getsubopt.o \
			   mandoc/compat_isblank.o \
			   mandoc/compat_mkdtemp.o \
			   mandoc/compat_ohash.o \
			   mandoc/compat_progname.o \
			   mandoc/compat_reallocarray.o \
			   mandoc/compat_recallocarray.o \
			   mandoc/compat_strcasestr.o \
			   mandoc/compat_strlcat.o \
			   mandoc/compat_strlcpy.o \
			   mandoc/compat_strndup.o \
			   mandoc/compat_strsep.o \
			   mandoc/compat_strtonum.o \
			   mandoc/compat_vasprintf.o

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
				mandoc/out.c \
				hashmap.c \
				main.c

mangl: $(COMPAT_OBJS) $(LIBMANDOC_OBJS) $(MANGL_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(COMPAT_OBJS) $(LIBMANDOC_OBJS) $(MANGL_SOURCES) $(LDFLAGS)

.PHONY: install
install: mangl
	cp mangl /usr/local/bin/

.PHONY: clean
clean:
	rm -f mangl
	rm -f *.o
	rm -f mandoc/*.o
