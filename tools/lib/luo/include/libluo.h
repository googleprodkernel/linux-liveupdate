// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file libluo.h
 * @brief Library for interacting with the Linux Live Update Orchestrator (LUO)
 *
 * This library provides a simple interface for applications to interact with
 * the Linux Live Update Orchestrator (LUO) subsystem, allowing them to preserve
 * and restore file descriptors across live kernel updates.
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 */

#ifndef _LIBLUO_H
#define _LIBLUO_H

#include <stdint.h>
#include <stdbool.h>
#include <liveupdate.h>

/**
 * @brief Initialize the LUO library
 *
 * Opens the LUO device file and prepares the library for use.
 *
 * @return 0 on success, negative error code on failure
 */
int luo_init(void);

/**
 * @brief Clean up and release resources used by the LUO library
 *
 * Closes the LUO device file and releases any resources allocated by the
 * library.
 */
void luo_cleanup(void);

/**
 * @brief Get the current state of the LUO subsystem
 *
 * @param[out] state Pointer to store the current LUO state
 * @return 0 on success, negative error code on failure
 */
int luo_get_state(enum liveupdate_state *state);

/**
 * @brief Preserve a file descriptor for restoration after a live update
 *
 * Marks the specified file descriptor for preservation across a live update.
 * The kernel validates if the FD type is supported for preservation.
 *
 * @param[in] fd The file descriptor to preserve
 * @param[in] token Token to associate fd with. Must be unique.
 * @return 0 on success, negative error code on failure
 */
int luo_fd_preserve(int fd, uint64_t token);

/**
 * @brief Cancel preservation of a previously preserved file descriptor
 *
 * Removes a file descriptor from the preservation list using its token.
 *
 * @param[in] token The token used to preserve fd previously.
 * @return 0 on success, negative error code on failure
 */
int luo_fd_unpreserve(uint64_t token);

/**
 * @brief Restore a previously preserved file descriptor
 *
 * Restores a file descriptor that was preserved before the live update.
 * This must be called after the system has rebooted into the new kernel.
 *
 * @param[in] token The token returned by luo_fd_preserve before the update
 * @param[out] fd Pointer to store the new file descriptor
 * @return 0 on success, negative error code on failure
 */
int luo_fd_restore(uint64_t token, int *fd);

/**
 * @brief Initiate the preparation phase for a live update
 *
 * Triggers the PREPARE phase in the LUO subsystem, which begins the
 * state saving process for items marked for preservation.
 *
 * @return 0 on success, negative error code on failure
 */
int luo_prepare(void);

/**
 * @brief Cancel the live update preparation phase
 *
 * Aborts the preparation sequence and returns the system to normal state.
 *
 * @return 0 on success, negative error code on failure
 */
int luo_cancel(void);

/**
 * @brief Signal completion of restoration after a live update
 *
 * Notifies the LUO subsystem that all necessary restoration actions
 * have been completed in the new kernel.
 *
 * @return 0 on success, negative error code on failure
 */
int luo_finish(void);

/**
 * @brief Check if the LUO subsystem is available
 *
 * Tests if the LUO device file exists and can be opened.
 *
 * @return true if LUO is available, false otherwise
 */
bool luo_is_available(void);

/**
 * @brief Convert a liveupdate_state enum value to a string
 *
 * Returns a string representation of the given LUO state.
 *
 * @param[in] state The LUO state to convert
 * @return A constant string representing the state
 */
const char *luo_state_to_string(enum liveupdate_state state);

#endif /* _LIBLUO_H */
