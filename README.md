FTP-3DS
=======

FTP Server for 3DS. Forked from [ftBRONY](https://github.com/mtheall/ftbrony)

Features
--------
- Appears to work well with a variety of clients.
- Also compiles for Linux.
- Supports multiple simultaneous clients.
- Cutting-edge graphics.

Build and install
------------------

You must first install and set up [devkitARM and libctru](http://3dbrew.org/wiki/Setting_up_Development_Environment).
Clone this repository and cd in the resulting directory.

    make

Create a **FTP-3DS** (double check that it is spelt **exactly** like this) directory inside the 3ds directory on the root of your SD card and copy the following files in it:
- FTP-3DS.3dsx
- FTP-3DS.smdh

I'll also upload builds whenever things change over on the [releases tab](https://github.com/iamevn/FTP-3DS/releases).

Supported Commands
------------------

- CDUP
- CWD
- DELE
- FEAT (no-op)
- LIST
- MKD
- MODE (no-op)
- NOOP
- PASS (no-op)
- PASV
- PORT
- PWD
- QUIT
- RETR
- RMD
- RNFR
- RNTO (rename syscall is broken?)
- STOR
- STRU (no-op)
- SYST
- TYPE (no-op)
- USER (no-op)
- XCUP
- XMKD
- XPWD
- XRMD

Planned Commands
----------------

- ALLO
- APPE
- NLST
- REST
- STOU
