# Third-party software

The released package contains or links to the following. Each keeps its own license.

| Component | Use | License |
|---|---|---|
| FAAD2 | AAC audio decoding | GPL-2.0-or-later |
| mbed TLS | HTTPS and AES | Apache-2.0 or GPL-2.0-or-later |
| libcurl | HTTP | curl license |
| FreeType | Text rendering | FreeType License |
| zlib | Compression | zlib License |
| PSL1GHT and its libraries | PS3 SDK | MIT |
| Noto Sans | The UI font, in `app/pkgfiles/USRDIR/` | SIL Open Font License 1.1 |
| Mozilla CA certificates | HTTPS verification, in `app/pkgfiles/USRDIR/cacert.pem` | MPL-2.0 |

NVIDIA's Cg library is used only on the build machine, to compile shaders. It isn't included here
or in the package.
