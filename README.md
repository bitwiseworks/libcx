# LIBCx - kLIBC Extension Library

The kLIBC Extension Library extends the functionality of the [kLIBC library](http://trac.netlabs.org/libc) by adding a number of high demand features required by modern applications. The kLIBC library is not actively maintained any more and extending it with the needed functionality is not always a trivial task. LIBCx is a good place to deploy such extensions as it does not require touching complex kLIBC internals and therefore cannot introduce new bugs and regressions in a sensitive piece of software the base C runtime library naturally is.

It is no doubt that all the functionality provided by LIBCx logically belongs to the C runtime and should eventually migrate to kLIBC (or to its probable successor) and this is the final goal of this project. Until then, applications should be manually linked with LIBCx (in addition to the implicit linking with kLIBC performed by the GCC compiler under the hood) in order to use all the implemented extensions.

Currently, LIBCx provides the following extensions:

 - Improved advisory file locking using the `fcntl()` API. The implementation provided by kLIBC uses `DosSetFileLocks` and is broken as it does not guarantee atomicity of lock/unlock operations in many cases (like overlapping lock regions etc.) and does not have deadlock detection.
 - Improved positional read/write operations provided by `pread()` and `pwrite()` APIs that guarantee atomic behavior. kLIBC emulates these functions using a pair of `lseek` and `read()/write()` calls in non-atomic manner which leads to data corruption when accessing the same file from multiple threads or processes.
