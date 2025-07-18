#!/bin/bash

set -e

APP_NAME="TascamControlPanel"
PROJECT_DIR=$(pwd)
BUILD_DIR="${PROJECT_DIR}/build"

TOOLS_DIR="${PROJECT_DIR}/.tools"
LINUXDEPLOY_FILENAME="linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_PATH="${TOOLS_DIR}/${LINUXDEPLOY_FILENAME}"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"


echo "--- Checking for linuxdeploy tool ---"
if [ ! -f "${LINUXDEPLOY_PATH}" ]; then
    echo "linuxdeploy not found. Downloading..."
    mkdir -p "${TOOLS_DIR}"
    wget -O "${LINUXDEPLOY_PATH}" "${LINUXDEPLOY_URL}"
    echo "Making linuxdeploy executable..."
    chmod +x "${LINUXDEPLOY_PATH}"
else
    echo "linuxdeploy found at ${LINUXDEPLOY_PATH}"
fi


echo "--- Building the C++ application ---"
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}
cmake ..
make -j$(nproc)
cd ${PROJECT_DIR}


echo "--- Running linuxdeploy to create the AppImage ---"
rm -rf AppDir

"${LINUXDEPLOY_PATH}" --appdir AppDir \
-e "${BUILD_DIR}/${APP_NAME}" \
-i "${PROJECT_DIR}/resources/tascam-control-panel.png" \
-d "${PROJECT_DIR}/tascam-control-panel.desktop" \
--output appimage


echo ""
echo "--- DONE ---"
echo "AppImage created successfully!"
