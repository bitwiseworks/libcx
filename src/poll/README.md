## Introduction

This package implements the System V *poll*(2) system call for Unix-like
systems that do not support *poll*. For instance, the following Unix-like
operating systems do not support *poll*: *poll* provides a method for
multiplexing input and output on multiple open file descriptors; in
traditional BSD systems, that capability is provided by *select*(2). While
the semantics of *select* differ from those of *poll*, *poll* can be
readily emulated in terms of *select*, which is exactly what this small
piece of software does.

Brief documentation on this emulation can be found at the top of the
`poll.h` header file. Also, see the [home page][] for more details.

[home page]: http://software.clapper.org/poll

## Building

See the [home page][] for details.

## License and Copyright

This software is copyright &copy; 19952010 Brian M. Clapper, and is released
under a BSD license. See the [license][] file for complete details.

[license]: http://software.clapper.org/poll/license.html

## Author

Brian M. Clapper, [bmc /at/ clapper.org][]

[bmc /at/ clapper.org]: mailto:bmc@clapper.org
