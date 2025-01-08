project('pdf-simple-sign', 'cpp', default_options : ['cpp_std=c++11'],
	version : '1.1.1')

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
configure_file(output : 'config.h', configuration : conf)

cryptodep = dependency('libcrypto')
executable('pdf-simple-sign', 'pdf-simple-sign.cpp',
	install : true,
	dependencies : cryptodep)

asciidoctor = find_program('asciidoctor')
foreach page : ['pdf-simple-sign']
	custom_target('manpage for ' + page,
		input : page + '.adoc', output: page + '.1',
		command : [asciidoctor, '-b', 'manpage',
			'-a', 'release-version=' + meson.project_version(),
			'@INPUT@', '-o', '@OUTPUT@'],
		install : true,
		install_dir : join_paths(get_option('mandir'), 'man1'))
endforeach
