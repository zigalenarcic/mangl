# mangl

This is a graphical man page viewer based on mandoc (https://mandoc.bsd.lv/).

![Screenshot](screenshot/screenshot.png)

It uses OpenGL to display man pages with clickable hyperlinks and smooth scrolling.

Features:
* hyperlinks to other man pages
* browsing history
* colored text
* draggable scrollbar
* keyboard and mouse interaction

## Building

Install prerequisites if necessary (zlib, OpenGL and FreeGLUT headers and
libraries). On Debian systems run:

```
sudo apt install libz-dev libgl-dev freeglut3-dev
```

First run `./configure` in mandoc folder:

```
cd mandoc
./configure
```

Then run `make` in root folder:

```
cd ..
make
```

If it fails, please check the Makefile for proper inclusion of OpenGL libraries.

Run
```
sudo make install
```
to copy the executable to `/usr/local/bin/` or copy and use the `mangl` binary as you like.

## Keyboard commands

* scrolling one step: `j`, `k`, `up-arrow`, `down-arrow`
* scrolling one whole page: `space`, `shift-space`, `page-up`, `page-down`
* scrolling to the beginning or the end of the man page: `gg`, `G`, `Home`, `End`
* to go to the previous man page: `b` or `right-mouse-click`
* to go to the next man page: `left-mouse-click` on the link or `f` to go to the page opened before going back
* to go to search screen: `Ctrl-f`
* to quit: `q`, `Ctrl-c`, `Ctrl-d`

## Command line arguments

```
mangl     - open the viewer in search mode
mangl [man page name] - open the viewer in man page mode with man page opened
mangl [section name] [man page name] - open the man page from the specified section, e.g. mangl 3 printf
```

