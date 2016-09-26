# History of changes for LIBCx

#### Version 0.3.1 (2016-09-26)

* mmap: Fix crashes when reading from PROT_READ mappings bound to files.

#### Version 0.3 (2016-09-22)

* Automatically install EXCEPTQ trap report generator (if available) on all application threads.
* Support trace groups in debug builds (controlled with LIBCX_TRACE environment var).
* Add complete POSIX `mmap()` implementation (also includes `msync()`, `munmap()`, `mprotect()`, `madvise()`, `posix_madvise()`).
* Provide `libcx.a` import library in addition to `libcxN.a`.

#### Version 0.2.1 (2016-08-19)

* Don't prevent real close from be called after the close hook (also caused failed return values).
* Fix committed memory overgrowth under heavy load (caused ENOMEM in fcntl locks).

#### Version 0.2 (2016-07-18)

* fcntl: Implement joining adjacent lock regions which greatly reduces memory footprint.
* Implement dynamic memory allocation for internal data (currently limited to 2MB maximum).
* Add select() implementation that supports regular files.
* Add poll() emulation using select() including support for POLLRDNORM and friends.
* Add libcx-stats.exe to print live memory usage statistics.
* fcntl: Implement releasing all file locks of a process on file close.

#### Version 0.1 (2016-06-10)

* Initial release.
