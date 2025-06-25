# LibLUO - Live Update Orchestrator Library

A C library for interacting with the Linux Live Update Orchestrator (LUO) subsystem.

## Overview

LibLUO provides a set of APIs for applications to interact with LUO, avoiding
the need to directly calling the LUO ioctls. It provides APIs for controlling
the LUO state and preserve and restore file descriptors across live updates.

## Features

- Initialize and manage connection to the LUO device.
- Preserve file descriptors before a live update.
- Restore file descriptors after a live update.
- Control the live update state machine (prepare, cancel, finish).
- Query the current state of the LUO subsystem.
- The library also includes a test suite for testing both LibLUO and the kernel
  LUO interface.

## Building

```bash
make
```

This will build both static (`libluo.a`) and shared (`libluo.so`) versions of the library.

To build the tests, do

``` bash
make tests
```

This will build the `tests/test` binary.

## Installation

```bash
sudo make install
```

This will install the library to `/usr/local/lib` and the header file to `/usr/local/include`.

## Usage

### Preserving a file descriptor

```c
#include <libluo.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int ret;
    uint64_t token;
    int fd, new_fd;
    enum luo_state state;

    // Initialize the library
    ret = luo_init();
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize LibLUO: %d\n", ret);
        return 1;
    }

    // Check if LUO is available
    if (!luo_is_available()) {
        fprintf(stderr, "LUO is not available on this system\n");
        return 1;
    }

    // Get the current LUO state
    ret = luo_get_state(&state);
    if (ret < 0) {
        fprintf(stderr, "Failed to get LUO state: %d\n", ret);
        luo_cleanup();
        return 1;
    }

    printf("Current LUO state: %s\n", luo_state_to_string(state));

    // Open a file descriptor to preserve
	fd = memfd_create("luo_memfd", 0);
    if (fd < 0) {
        perror("Failed to open memfd");
        luo_cleanup();
        return 1;
    }

    // Preserve the file descriptor
    ret = luo_fd_preserve(fd, &token);
    if (ret < 0) {
        fprintf(stderr, "Failed to preserve FD: %d\n", ret);
        close(fd);
        luo_cleanup();
        return 1;
    }

    printf("FD %d preserved with token %lu\n", fd, token);

    // After a live update, restore the file descriptor
    if (state == LUO_STATE_UPDATED) {
        ret = luo_fd_restore(token, &new_fd);
        if (ret < 0) {
            fprintf(stderr, "Failed to restore FD: %d\n", ret);
        } else {
            printf("FD restored: %d\n", new_fd);
            close(new_fd);
        }

        // Signal completion of restoration
        luo_finish();
    }

    close(fd);
    luo_cleanup();
    return 0;
}
```

### Controlling the Live Update Process

```c
#include <libluo.h>
#include <stdio.h>

int main() {
    int ret;

    ret = luo_init();
    if (ret < 0) {
        return 1;
    }

    // Initiate the preparation phase
    ret = luo_prepare();
    if (ret < 0) {
        fprintf(stderr, "Failed to prepare for live update: %d\n", ret);
        luo_cleanup();
        return 1;
    }

    // At this point, the system is ready for kexec reboot
    // The freeze operation is handled internally by the kernel
    // during kexec.

    // After reboot, in the new kernel
    // Signal completion of restoration
    ret = luo_finish();
    if (ret < 0) {
        fprintf(stderr, "Failed to finish live update: %d\n", ret);
        luo_cleanup();
        return 1;
    }

    luo_cleanup();
    return 0;
}
```

## License

This library is provided under the terms of the GNU Lesser General Public
License version 3.0, or (at your option) any later version.
