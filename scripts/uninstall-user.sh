#!/usr/bin/env bash
set -euo pipefail

rm -f "$HOME/.local/share/vulkan/implicit_layer.d/VK_LAYER_MEIN_frame_capper.json"
rm -f "$HOME/.local/share/vulkan/explicit_layer.d/VK_LAYER_MEIN_frame_capper.json"
rm -rf "$HOME/.local/lib/VulkanFrameCapper"

echo "Uninstalled VulkanFrameCapper user files."
