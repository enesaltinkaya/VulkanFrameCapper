#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
LIB_DIR="$HOME/.local/lib/VulkanFrameCapper"
LAYER_DIR="$HOME/.local/share/vulkan/implicit_layer.d"

cmake -S "$ROOT" -B "$BUILD"
cmake --build "$BUILD" -j"$(nproc)"

mkdir -p "$LIB_DIR" "$LAYER_DIR"
install -m 755 "$BUILD/libVulkanFrameCapper.so" "$LIB_DIR/libVulkanFrameCapper.so"

cat > "$LAYER_DIR/VK_LAYER_MEIN_frame_capper.json" <<EOF_JSON
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_MEIN_frame_capper",
    "type": "GLOBAL",
    "library_path": "$LIB_DIR/libVulkanFrameCapper.so",
    "api_version": "1.4.0",
    "implementation_version": "1",
    "description": "Vulkan frame pacer + scanline sync layer",
    "functions": {
      "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
    },
    "disable_environment": {
      "DISABLE_VULKAN_FRAME_CAPPER": "1"
    }
  }
}
EOF_JSON

echo "Installed VulkanFrameCapper as a user implicit Vulkan layer."
echo "Run with: FPS=60 <app>"
echo "Disable with: DISABLE_VULKAN_FRAME_CAPPER=1 <app>"
