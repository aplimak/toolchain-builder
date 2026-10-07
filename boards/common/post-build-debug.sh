#!/bin/bash
set -eu

# The target directory is passed as the first argument by Buildroot.
TARGET_DIR="$1"

# Define the archive path and name.
# BINARIES_DIR is an environment variable provided by Buildroot (output/images).
ARCHIVE_PATH="${BINARIES_DIR}/rootfs-debug.tar.gz"

echo "========================================="
echo "Running custom post-build script..."
echo "Target directory: ${TARGET_DIR}"
echo "========================================="

# 1. Archive the unstripped target directory.
echo "Archiving unstripped rootfs to: ${ARCHIVE_PATH}"
tar -czf "${ARCHIVE_PATH}" -C "${TARGET_DIR}" .

# 2. Strip all the binaries in the target directory.
# These commands are copied exactly from Buildroot's target-finalize step.
# See: https://gitlab.com/buildroot.org/buildroot/-/blob/master/Makefile#L500

# Strip standard executables and shared libraries.
find "${TARGET_DIR}" \
    \( -name 'ld-*.so*' -o -name 'libpthread*.so*' \) -prune -o \
    -type f \( -perm /111 -o -name '*.so*' \) \
    -not \( -name 'libpthread*.so*' -o -name 'ld-*.so*' -o -name '*.ko' \) \
    -print0 | xargs -0 -r "${TARGET_STRIP}" \
    --remove-section=.comment --remove-section=.note 2>/dev/null || true

# Strip special libraries (libpthread, ld.so) with debug-only stripping.
find "${TARGET_DIR}" \
    \( -name 'ld-*.so*' -o -name 'libpthread*.so*' \) \
    -print0 | xargs -0 -r "${TARGET_STRIP}" \
    --remove-section=.comment --remove-section=.note --strip-debug 2>/dev/null || true

echo "Post-build script finished successfully."
