dte
===

dte is a small and easy to use console text editor.

Features
--------

* Multiple buffers/tabs
* Unlimited undo/redo
* Search and replace
* Syntax highlighting
* Customizable color schemes
* Customizable key bindings
* Command language with auto-completion
* Jump to definition (using [ctags])
* Jump to compiler error

Screenshot
----------

![dte screenshot](https://craigbarnes.gitlab.io/dte/screenshot.png)

Installation
------------

To build `dte` from source, first download and extract the latest
release tarball:

    curl -LO https://craigbarnes.gitlab.io/dist/dte/dte-1.5.tar.gz
    tar -xzf dte-1.5.tar.gz
    cd dte-1.5

Then install the following dependencies:

* [GCC] or [Clang]
* [GNU Make] `>= 3.81`
* [ncurses]

...which can (optionally) be done via the included [script][install-deps.sh]:

    sudo mk/install-deps.sh

Then compile and install:

    make -j8 && sudo make install

The default installation [`prefix`] is `/usr/local` and [`DESTDIR`]
works as usual.

Documentation
-------------

After installing, you can access the documentation in man page format
via `man 1 dte`, `man 5 dterc` and `man 5 dte-syntax`.

These pages are also available online in HTML and PDF formats:

* [dte.1.html](https://craigbarnes.gitlab.io/dte/dte.1.html)
* [dterc.5.html](https://craigbarnes.gitlab.io/dte/dterc.5.html)
* [dte-syntax.5.html](https://craigbarnes.gitlab.io/dte/dte-syntax.5.html)
* [dte.1.pdf](https://craigbarnes.gitlab.io/dte/dte.1.pdf)
* [dterc.5.pdf](https://craigbarnes.gitlab.io/dte/dterc.5.pdf)
* [dte-syntax.5.pdf](https://craigbarnes.gitlab.io/dte/dte-syntax.5.pdf)

Testing
-------

`dte` is tested on the following platforms:

* Debian ([CI][GitLab CI])
* CentOS ([CI][GitLab CI])
* Alpine Linux ([CI][GitLab CI])
* Ubuntu ([CI][GitLab CI])
* Mac OS X ([CI][Travis CI])
* OpenBSD (occasional, manual testing)
* FreeBSD (occasional, manual testing)

Other [POSIX 2008] compatible platforms should also work, but may
require build system fixes. [Bug reports] and/or [pull requests] are
welcome.

License
-------

Copyright (C) 2017 Craig Barnes.  
Copyright (C) 2010-2015 Timo Hirvonen.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU [General Public License version 2], as published
by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License version 2 for more details.


[ctags]: https://en.wikipedia.org/wiki/Ctags
[GCC]: https://gcc.gnu.org/
[Clang]: https://clang.llvm.org/
[GNU Make]: https://www.gnu.org/software/make/
[ncurses]: https://www.gnu.org/software/ncurses/
[install-deps.sh]: https://github.com/craigbarnes/dte/blob/master/mk/install-deps.sh
[`GNUmakefile`]: https://github.com/craigbarnes/dte/blob/master/GNUmakefile
[`prefix`]: https://www.gnu.org/prep/standards/html_node/Directory-Variables.html
[`DESTDIR`]: https://www.gnu.org/prep/standards/html_node/DESTDIR.html
[POSIX 2008]: http://pubs.opengroup.org/onlinepubs/9699919799/
[GitLab CI]: https://gitlab.com/craigbarnes/dte/pipelines
[Travis CI]: https://travis-ci.org/craigbarnes/dte
[Bug reports]: https://github.com/craigbarnes/dte/issues
[pull requests]: https://github.com/craigbarnes/dte/pulls
[General Public License version 2]: https://www.gnu.org/licenses/gpl-2.0.html
