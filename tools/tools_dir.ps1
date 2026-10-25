# Where the external tools live (Real-ESRGAN, stable-diffusion.cpp and its models, llvm-mingw):
# the folder in the IMP2HD_TOOLS environment variable, or tools\external next to this script.
# The scripts look for them in these sub-folders:
#   realesrgan\realesrgan-ncnn-vulkan.exe
#   sdcpp\bin\sd-cli.exe, sdcpp\models\sdxl_inpaint.safetensors, sdcpp\models\sdxl_vae_fp16_fix.safetensors
#   llvm-mingw-*\bin\i686-w64-mingw32-clang.exe (only if the compiler is not on PATH)
if ($env:IMP2HD_TOOLS) { $env:IMP2HD_TOOLS } else { Join-Path $PSScriptRoot "external" }
