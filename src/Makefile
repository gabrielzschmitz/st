# st - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

PYTHON = python3

SRC = st.c x.c rowcolumn_diacritics_helpers.c graphics.c boxdraw.c hb.c trace.c \
	render_xft.c
ifeq ($(ST_WITH_GL),1)
SRC += render_gl.c
endif
OBJ = $(SRC:.c=.o)
XCAPTURE = ../benchmarks/xcapture

all: st

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

st.o: config.h st.h win.h graphics.h trace.h render.h
x.o: arg.h config.h st.h win.h graphics.h hb.h trace.h render.h
graphics.c: graphics.h khash.h kvec.h st.h
boxdraw.o: config.h st.h boxdraw_data.h
hb.o: st.h
trace.o: trace.h
render_xft.o: render.h win.h st.h
ifeq ($(ST_WITH_GL),1)
render_gl.o: render.h render_gl.h win.h st.h
endif

$(OBJ): config.h config.mk

st: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

clean:
	rm -f st config.h $(OBJ) st-$(VERSION).tar.gz $(XCAPTURE)

$(XCAPTURE): ../benchmarks/xcapture.c
	$(CC) -o $@ ../benchmarks/xcapture.c $(INCS) -lX11

benchmark: st $(XCAPTURE)
	$(PYTHON) ../benchmarks/benchmark.py --st ./st --capture $(XCAPTURE) \
		--output ../benchmark-results.json

benchmark-xvfb: st $(XCAPTURE)
	$(PYTHON) ../benchmarks/benchmark.py --st ./st --xvfb --capture $(XCAPTURE) \
		--output ../benchmark-results.json

dist: clean
	mkdir -p st-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h st.info st.1 arg.h st.h win.h render.h render_gl.h $(SRC)\
		st-$(VERSION)
	tar -cf - st-$(VERSION) | gzip > st-$(VERSION).tar.gz
	rm -rf st-$(VERSION)

install: st
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f st $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < st.1 > $(DESTDIR)$(MANPREFIX)/man1/st.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/st.1
	tic -sx st.info
	@echo Please see the README file regarding the terminfo entry of st.
	mkdir -p $(DESTDIR)$(APPPREFIX)
	cp -f st.desktop $(DESTDIR)$(APPPREFIX)
	mkdir -p $(DESTDIR)$(ICONPREFIX)/hicolor/scalable/apps
	cp -f ../st.svg $(DESTDIR)$(ICONPREFIX)/hicolor/scalable/apps/st.svg
	mkdir -p $(DESTDIR)$(ICONPREFIX)/hicolor/256x256/apps
	cp -f st.png $(DESTDIR)$(ICONPREFIX)/hicolor/256x256/apps/st.png

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(APPPREFIX)/st.desktop
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1
	rm -f $(DESTDIR)$(ICONPREFIX)/hicolor/scalable/apps/st.svg
	rm -f $(DESTDIR)$(ICONPREFIX)/hicolor/256x256/apps/st.png

.PHONY: all benchmark benchmark-xvfb clean dist install uninstall
