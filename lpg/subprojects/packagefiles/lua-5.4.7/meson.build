project(
  'lua-5.4',
  'cpp',
  license : 'MIT',
  meson_version : '>=0.49.2',
  version : '5.4.7',
  default_options : ['c_std=c99', 'warning_level=2'],
)

cxx = meson.get_compiler('cpp')

# Skip bogus warning.
add_project_arguments(cxx.get_supported_arguments(
  '-Wno-string-plus-int', '-Wno-stringop-overflow'), language : 'cpp')

# Platform-specific defines.
is_posix = host_machine.system() in ['cygwin', 'darwin', 'dragonfly', 'freebsd',
  'gnu', 'haiku', 'linux', 'netbsd', 'openbsd', 'sunos']
if is_posix
  add_project_arguments('-DLUA_USE_POSIX', language : 'cpp')
endif

# Library dependencies.
lua_lib_deps = [cxx.find_library('m', required : false)]

if meson.version().version_compare('>= 0.62')
  dl_dep = dependency('dl', required : get_option('loadlib'))
else
  dl_dep = cxx.find_library('dl', required : get_option('loadlib'))
endif

if dl_dep.found()
  lua_lib_deps += dl_dep
  add_project_arguments('-DLUA_USE_DLOPEN', language : 'cpp')
endif

# Targets.
add_project_arguments('-DMAKE_LIB', language : 'cpp')
lua_lib = static_library(
  'lua',
  'onelua.cpp',
  dependencies : lua_lib_deps,
  implicit_include_directories : false,
)

inc = include_directories('.')
lua_dep = declare_dependency(
  link_with : lua_lib,
  include_directories : inc,
)
