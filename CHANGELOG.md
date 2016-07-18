# History of changes for LIBCx

Version 0.2 (2016-07-18)

* fcntl: Implement joining adjacent lock regions which greatly reduces memory footprint.
* Implement dynamic memory allocation for internal data (currently limited to 2MB maximum).
* Add select() implementation that supports regular files.
* Add poll() emulation using select() including support for POLLRDNORM and friends.
* Add libcx-stats.exe to print live memory usage statistics.
* fcntl: Implement releasing all file locks of a process on file close.

Version 0.1 (2016-06-10)

* Initial release.
