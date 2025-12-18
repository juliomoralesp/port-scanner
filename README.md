# ports — small tool to list ports and owning daemons

ports is a tiny C program that inspects /proc/net/{tcp,tcp6,udp,udp6} and tries to map
socket inodes to processes by scanning /proc/*/fd links. It prints the protocol, local
port and the owner process (pid/name) when available.

Build
-----

Standard build:

    make

For Synology NAS (static build for better portability):

    make synology

For cross-compilation to Synology NAS:

    make synology-cross CC_SYNOLOGY=<your-cross-compiler>

Example for ARM-based Synology:

    make synology-cross CC_SYNOLOGY=arm-linux-gnueabihf-gcc

Example for x86_64-based Synology:

    make synology-cross CC_SYNOLOGY=x86_64-pc-linux-gnu-gcc

Synology NAS Installation
--------------------------

1. **Build on the Synology device** (recommended):
   - Enable SSH on your Synology NAS
   - Install the development tools via Package Center (look for "Development Tools" or similar)
   - Copy the source files to your NAS
   - Run `make synology` to create a static binary
   - Copy the binary to a location in your PATH (e.g., `/usr/local/bin/`)

2. **Cross-compile on your development machine**:
   - Install the appropriate Synology toolchain for your NAS model
   - Run `make synology-cross CC_SYNOLOGY=<toolchain-gcc>`
   - Transfer the `ports` binary to your Synology NAS via SCP
   - Make it executable: `chmod +x ports`
   - Move to a suitable location: `mv ports /usr/local/bin/`

3. **Running on Synology**:
   - SSH into your Synology NAS
   - Run with `sudo ./ports` for best results (to see all process owners)
   - The tool should work on most Synology DSM versions

**Note**: The Synology build uses POSIX-compliant functions only and avoids GNU-specific 
extensions, ensuring compatibility with Synology's toolchain (typically based on older GCC 
versions).

Usage examples
--------------

List known listening ports (default):

    ./ports

Search for a specific port and sorting examples:

    # show entries for port 22
    ./ports -p 22

    # sort by pid
    ./ports -s pid

    # reverse sort by port
    ./ports -s port -r

Search processes by name substring (case-insensitive):

    ./ports -n ssh

Search processes by name substring (case-insensitive):

    ./ports -n ssh

Show all entries (not only those in LISTEN state in /proc/net):

    ./ports -a

Notes
-----
- The program needs permission to read /proc/<pid>/fd and /proc/<pid>/comm for other processes —
  you may see missing owners for system processes when not run as root.
- This is a simple utility intended for local troubleshooting and demonstration.

How matching works
------------------

The program identifies socket owners using /proc/*/fd symlinks — it looks for links
that point to "socket:[INODE]" and matches that inode to the entries found in
/proc/net/* that the program parses. Scanning /proc/<pid>/net/* for ports is
not used because those files present a system-wide view and can produce many
false-positive matches.

Tip: run the tool as root (via sudo) to get the most accurate owner information.

New options
-----------

- -s sort_field: choose how to sort the output. Possible values:
    - port (default) — sort by port number
    - pid — sort by owning pid (smallest pid when multiple owners)
    - proto — sort by protocol (tcp/tcp6/udp...) then port
- -r: reverse the sort order (descending)
