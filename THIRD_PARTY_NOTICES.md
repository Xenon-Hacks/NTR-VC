# Third-party notices

NTR-VC is based on [NTRViewer-HR](https://github.com/xzn/ntrviewer-hr) (MIT License, Copyright (c) 2025 xzn). It contains or links the third-party components below. The full license texts are in the source zip (`ntrviewer-hr-frame-share.zip`) at the paths shown, and in the `licenses/` folder of every release zip.

## Bundled in the source

| Component | License | License file in source |
| --- | --- | --- |
| [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) (modified) | MIT or Public Domain (Unlicense), at your choice | `nuklear/LICENSE-original`, `nuklear/LICENSE.md` |
| [KCP](https://github.com/skywind3000/kcp) | MIT, Copyright (c) 2017 Lin Wei | `LICENSE-kcp` |
| [Fecal](https://github.com/catid/fecal) (modified) | BSD 3-Clause, Copyright (c) 2017 Christopher A. Taylor | `fecal/LICENSE-original.md`, `fecal/LICENSE.md` |
| [libjpeg-turbo](https://libjpeg-turbo.org/) 3.1.1 (embedded) | IJG License and BSD 3-Clause; SIMD code under zlib | `jpeg_turbo/LICENSE.md`, plus `licenses/libjpeg-turbo-README.ijg` in this repo |
| [stb_image](https://github.com/nothings/stb) | Public Domain or MIT | end of `stb_image.h` |
| [Clarity icons](https://v12.clarity.design/) (modified SVG cursors) | MIT | `clarity/LICENSE.md` |
| [volk](https://github.com/zeux/volk) | MIT | end of `volk.h` |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT | top of `vk_mem_alloc.h` |
| [RapidJSON](https://github.com/Tencent/rapidjson) | MIT | `rapidjson/license.txt` |
| [Real-CUGAN ncnn models](https://github.com/nihui/realcugan-ncnn-vulkan) (`models-se`) | MIT, Copyright (c) 2019 nihui | `models-se/LICENSE` |
| glad / Khronos headers (`include/`) | MIT / Apache-2.0 (per file headers) | headers in `include/` |

Not every component is compiled into the "lite" Windows build; all are listed because they ship in the source.

**This software is based in part on the work of the Independent JPEG Group.**

## Linked into the Windows build (from MSYS2)

| Component | License |
| --- | --- |
| [SDL 3](https://www.libsdl.org/) (static) | zlib |
| [GNU libiconv](https://www.gnu.org/software/libiconv/) (static, if pulled in by SDL) | LGPL-2.1-or-later; source at https://ftp.gnu.org/pub/gnu/libiconv/ |
| MinGW-w64 runtime and winpthreads | ZPL / MIT-style |
| GCC runtime (libgcc, libstdc++) | GPL-3.0 with GCC Runtime Library Exception (no obligations for this use) |

The release workflow copies the MSYS2 license files for these packages into the release zip's `licenses/` folder. Every release also attaches the complete application source (`ntrviewer-hr-frame-share.zip`), so the viewer can be rebuilt against a modified libiconv as the LGPL requires.
