#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 <ncnn-20260526-source> <install-directory>" >&2
	exit 2
fi

NCNN_SOURCE=$1
NCNN_INSTALL=$2
NCNN_OPENMP=${NCNN_OPENMP:-OFF}
NCNN_SIMPLEOMP=${NCNN_SIMPLEOMP:-OFF}

case "${NCNN_OPENMP}" in
	ON|OFF)
		;;
	*)
		echo "NCNN_OPENMP must be ON or OFF (got: ${NCNN_OPENMP})" >&2
		exit 2
		;;
esac

case "${NCNN_SIMPLEOMP}" in
	ON|OFF)
		;;
	*)
		echo "NCNN_SIMPLEOMP must be ON or OFF (got: ${NCNN_SIMPLEOMP})" >&2
		exit 2
		;;
esac

if [ "${NCNN_SIMPLEOMP}" = "ON" ] && [ "${NCNN_OPENMP}" != "ON" ]; then
	echo "NCNN_SIMPLEOMP=ON requires NCNN_OPENMP=ON" >&2
	exit 2
fi

if [ "${NCNN_SIMPLEOMP}" = "ON" ]; then
	NCNN_VARIANT=simpleomp
elif [ "${NCNN_OPENMP}" = "ON" ]; then
	NCNN_VARIANT=openmp
else
	NCNN_VARIANT=serial
fi

# Keep serial and OpenMP CMake caches separate.  Reusing the original cache
# can otherwise leave stale compiler checks or OpenMP results in the build.
NCNN_BUILD=${NCNN_INSTALL}-build-armv7-${NCNN_VARIANT}
SCRIPT_DIRECTORY=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLCHAIN_FILE=${SCRIPT_DIRECTORY}/petalinux-zynq-toolchain.cmake

if [ ! -f "${NCNN_SOURCE}/CMakeLists.txt" ]; then
	echo "Not an NCNN source directory: ${NCNN_SOURCE}" >&2
	exit 2
fi
if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ] || [ -z "${SDKTARGETSYSROOT:-}" ]; then
	echo "Source the PetaLinux SDK environment before running this script." >&2
	exit 2
fi
if [ ! -f "${TOOLCHAIN_FILE}" ]; then
	echo "Missing toolchain wrapper: ${TOOLCHAIN_FILE}" >&2
	exit 2
fi

echo "CC=${CC}"
echo "CXX=${CXX}"
echo "SDKTARGETSYSROOT=${SDKTARGETSYSROOT}"
echo "NCNN_SOURCE=${NCNN_SOURCE}"
echo "NCNN_INSTALL=${NCNN_INSTALL}"
echo "NCNN_OPENMP=${NCNN_OPENMP}"
echo "NCNN_SIMPLEOMP=${NCNN_SIMPLEOMP}"
echo "TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"

# NCNN_OPENMP defaults to OFF for the correctness build.  Native GNU OpenMP
# requires omp.h and libgomp in the SDK.  PetaLinux 2020.2 minimal SDKs often
# omit them; NCNN_SIMPLEOMP=ON uses NCNN's pthread-based OpenMP ABI emulation
# instead and introduces no target-side libgomp dependency.
cmake -S "${NCNN_SOURCE}" -B "${NCNN_BUILD}" \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${NCNN_INSTALL}" \
	-DNCNN_VULKAN=OFF \
	-DNCNN_OPENMP="${NCNN_OPENMP}" \
	-DNCNN_SIMPLEOMP="${NCNN_SIMPLEOMP}" \
	-DNCNN_BUILD_TOOLS=OFF \
	-DNCNN_BUILD_EXAMPLES=OFF \
	-DNCNN_BUILD_BENCHMARK=OFF \
	-DNCNN_BUILD_TESTS=OFF \
	-DNCNN_PYTHON=OFF \
	-DNCNN_SHARED_LIB=OFF

cmake --build "${NCNN_BUILD}" --parallel 2
cmake --install "${NCNN_BUILD}"

echo "NCNN installed to ${NCNN_INSTALL}"
