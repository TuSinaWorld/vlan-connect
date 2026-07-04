# WinTun - Layer 3 TUN Driver for Windows

Client only. Download from: https://www.wintun.net/

This source tree keeps only `wintun.h`; it does not commit prebuilt Wintun
DLLs.

For local builds or Windows binary packages, download the official Wintun
package and place the matching `wintun.dll` alongside the built GUI or CLI
client executable at runtime. Current Windows packages are expected to include
that DLL when the client needs the Wintun adapter.

License: `wintun.h` is marked `GPL-2.0 OR MIT`; VLan uses the MIT option for
the header. Prebuilt Wintun DLLs have license terms supplied in the official
Wintun ZIP package. Include the official `LICENSE.txt`, `README.md`, and
`wintun.h` notices when redistributing the DLL, and list the DLL in the
package's binary third-party notice file.
