#!/bin/bash

set -e

APP_NAME="TascamControlPanel"
PROJECT_DIR=$(pwd)
BUILD_DIR="${PROJECT_DIR}/build"

TOOLS_DIR="${PROJECT_DIR}/.tools"

LINUXDEPLOY_FILENAME="linuxdeploy-xnormal.AppImage"
LINUXDEPLOY_PATH="${TOOLS_DIR}/${LINUXDEPLOY_FILENAME}"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"

QT_PLUGIN_FILENAME="linuxdeploy-plugin-qt"
QT_PLUGIN_PATH="${TOOLS_DIR}/${QT_PLUGIN_FILENAME}"
QT_PLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

echo "--- Checking for deployment tools ---"
mkdir -p "${TOOLS_DIR}"

if [ ! -f "${LINUXDEPLOY_PATH}" ]; then
    echo "linuxdeploy not found. Downloading..."
    wget -c -O "${LINUXDEPLOY_PATH}" "${LINUXDEPLOY_URL}"
    chmod +x "${LINUXDEPLOY_PATH}"
fi

if [ ! -f "${QT_PLUGIN_PATH}" ]; then
    echo "linuxdeploy-plugin-qt not found. Downloading..."
    wget -c -O "${QT_PLUGIN_PATH}" "${QT_PLUGIN_URL}"
    chmod +x "${QT_PLUGIN_PATH}"
fi
echo "All tools are ready."


echo "--- Building the C++ application ---"
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}
cmake ..
make -j$(nproc)
cd ${PROJECT_DIR}

echo "--- Running linuxdeploy to create the AppImage ---"
rm -rf AppDir

export NO_STRIP=1

export PATH="${TOOLS_DIR}:${PATH}"

echo "--- Detecting Qt6 qmake ---"
if command -v qmake6 &> /dev/null; then
    export QMAKE=$(command -v qmake6)
elif command -v qt6-qmake &> /dev/null; then
    export QMAKE=$(command -v qt6-qmake)
elif command -v qmake &> /dev/null && qmake -v | grep -q "Qt version 6"; then
    export QMAKE=$(command -v qmake)
else
    echo "ERROR: Could not find a Qt6 qmake executable."
    echo "Please install the Qt6 development package for your distribution."
    echo "(e.g., 'sudo pacman -S qt6-base' or 'sudo apt install qt6-base-dev')"
    exit 1
fi
echo "Found qmake at: ${QMAKE}"

"${LINUXDEPLOY_PATH}" --appdir AppDir \
    -e "${BUILD_DIR}/${APP_NAME}" \
    -i "${PROJECT_DIR}/resources/tascam-control-panel.png" \
    -d "${PROJECT_DIR}/tascam-control-panel.desktop" \
    --plugin qt \
    --output appimage

echo ""
echo "--- DONE ---"
echo "AppImage created successfully!"
