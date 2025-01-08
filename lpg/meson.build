project('lpg', 'cpp', default_options : ['cpp_std=c++17'],
	version : '1.1.1')

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
configure_file(output : 'config.h', configuration : conf)

luapp = dependency('lua++', allow_fallback : true)
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
libqrencode = dependency('libqrencode')
lpg_exe = executable('lpg', 'lpg.cpp',
	install : true,
	dependencies : [luapp, cairo, pangocairo, libqrencode])

# XXX: https://github.com/mesonbuild/meson/issues/825
docdir = get_option('datadir') / 'doc' / meson.project_name()
lpg_pdf = custom_target('lpg.pdf',
	output : 'lpg.pdf',
	input : 'lpg.lua',
	command : [lpg_exe, '@INPUT@', '@OUTPUT@'],
	install_dir : docdir,
	build_by_default : true)
