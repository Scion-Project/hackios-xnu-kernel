# Scion's platform for the gfxstream Vulkan driver (tools/gfxstream/VULKAN.md), copied into
# src/gfxstream/guest/platform/scion by tools/gfxstream/mk-vk.sh.

inc_platform_scion = include_directories('.')

libplatform_virtgpu_scion = static_library(
   'platform_virtgpu_scion',
   files('ScionVirtGpu.cpp', 'ScionStream.cpp', 'scion_vk_transport.c'),
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform_virtgpu, inc_platform_scion, inc_guest_iostream, inc_src,
                         inc_include],
)

# the core Vulkan commands as exported symbols: there is no loader on iOS
scion_vk_exports = custom_target(
  'scion_vk_exports',
  input : ['mkvkexports.py', vk_api_xml],
  output : 'scion_vk_exports.c',
  command : [prog_python, '@INPUT0@', '--xml', '@INPUT1@',
             '--mesa-util', join_paths(meson.project_source_root(), 'src/vulkan/util'),
             '--out', '@OUTPUT@'],
  depend_files : vk_entrypoints_gen_depend_files,
)
