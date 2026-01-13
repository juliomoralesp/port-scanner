# GEMINI.md

## Project Overview

This project is a small C utility named `ports` that scans and lists network ports and their owning daemons. It works by parsing `/proc/net/{tcp,tcp6,udp,udp6}` and mapping socket inodes to processes by scanning `/proc/*/fd` links. The tool is intended for local troubleshooting and demonstration purposes.

The main source code is in `ports.c`. The project uses a `Makefile` for building the executable.

## Building and Running

### Building

To build the project, run the `make` command:

```bash
make
```

This will compile `ports.c` and create an executable file named `ports` in the current directory.

### Running

To run the program, execute the compiled binary:

```bash
./ports
```

The program supports several command-line options:

*   `./ports -p <port>`: Search for a specific port.
*   `./ports -n <name>`: Search for processes by a name substring.
*   `./ports -s <field>`: Sort the output by `port`, `pid`, or `proto`.
*   `./ports -r`: Reverse the sort order.
*   `./ports -a`: Show all entries, not just those in a listening state.

For more accurate owner information, it's recommended to run the tool with `sudo`.

### Installation

The project includes an installation script. To install the `ports` binary and its man page, run:

```bash
./install.sh
```

This will install the binary to `/usr/local/bin` and the man page to `/usr/local/share/man/man1`.

## Development Conventions

### Building and Testing

The project uses a `Makefile` for building. The default target `all` builds the `ports` executable.

The `.github/workflows/c-cpp.yml` file defines a CI pipeline that runs on every push and pull request to the `main` branch. The pipeline executes the following commands:

*   `make`
*   `make check`
*   `make distcheck`

### Coding Style

The coding style can be inferred from `ports.c`. It uses a consistent indentation style and includes comments to explain the code's functionality.
