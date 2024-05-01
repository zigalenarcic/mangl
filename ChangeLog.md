# ChangeLog

mangl changelog

## 1.1.4 2024-05-01
* add an icon and a .desktop file
* when using Ctrl-F, start with an empty search
* always set X11 `WM_CLASS` to mangl and set Wayland App ID to mangl when using GLFW 3.4
* add '=' key command to toggle between best matching line length for the current window width and the original line length (in manpage display only) and add Ctrl-V paste support in the search page and in the document search box
* add `line_length` setting (default value is 78) which controls max line length in characters
* fix fractional scaling (by [@minerscale](https://github.com/minerscale))

## 1.1.3 2024-02-10
* use Apple `gl.h` on macOS (by [@forivall](https://github.com/forivall))
* man pages paths determined by executing `manpath` (by [@christian-burger](https://github.com/christian-burger))
* center text when window width exceeds the required width

## 1.1.2 2022-07-18
* better `make install` (by [@omar-polo](https://github.com/omar-polo))
* correct behavior of key commands on non-QWERTY keyboard layouts (by [@omar-polo](https://github.com/omar-polo))
* additional key commands: Ctrl-v and Alt-v (by [@omar-polo](https://github.com/omar-polo))

## 1.1.1 2022-06-22
* update mandoc library to the latest snapshot: mandoc-1.14.6 (by [@omar-polo](https://github.com/omar-polo))
* building improvements (configure script etc.) (by [@omar-polo](https://github.com/omar-polo))

## 1.1.0 2022-05-06
* change windowing library from FreeGLUT to GLFW (suport for Wayland)
* add smartcase search - if only lowercase characters are used, search case-insensitive

## 1.0.0 2019-11-24
* initial implementation

