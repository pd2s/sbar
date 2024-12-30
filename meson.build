project(
    'sbar', 'c',
    version: '0.0.1-dev',
    license: 'MIT',
    #meson_version: '',
    default_options: [
        'c_std=c18',
        'warning_level=everything',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

add_project_arguments(
	[
        '-D_POSIX_C_SOURCE=200809L',
        #'-D_XOPEN_SOURCE=700',

        '-Wno-c++-compat',
        '-Wno-documentation-deprecated-sync',
        '-Wno-disabled-macro-expansion',
        '-Wno-missing-variable-declarations',
		'-Wno-used-but-marked-unused',

        '-Wno-unsafe-buffer-usage',
        '-Wno-declaration-after-statement',
        '-Wno-covered-switch-default',
        '-Wno-padded',
        '-Wno-cast-qual',
		'-Wno-cast-align',
		'-Wno-date-time',
	],
	language: 'c',
)

cc = meson.get_compiler('c')

json_c_dep = dependency('json-c')
png_dep = dependency('libpng', required: get_option('PNG_image_blocks'))
svg_dep = cc.find_library('resvg', required: get_option('SVG_image_blocks')) # TODO: replace with dependency()
wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_protocols_dep = dependency('wayland-protocols', native: true)

deps = [
    dependency('wayland-client'),
    dependency('pixman-1'),
    dependency('fcft'),
	json_c_dep,
    #cc.find_library('m'),
    png_dep,
    svg_dep,
]

wayland_scanner = find_program(
    wayland_scanner_dep.get_variable('wayland_scanner'),
    native: true,
)
wayland_scanner_header = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)
wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')

src = [
    'sbar.c',

    wayland_scanner_code.process(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
    wayland_scanner_code.process(wayland_protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml'),
    wayland_scanner_code.process(wayland_protocols_dir / 'unstable/tablet/tablet-unstable-v2.xml'), # required by cursor-shape-v1
    wayland_scanner_code.process('wlr-layer-shell-unstable-v1.xml'),

    wayland_scanner_header.process(wayland_protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml'),
    wayland_scanner_header.process(wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml'),
    wayland_scanner_header.process('wlr-layer-shell-unstable-v1.xml'),
]

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, '--git-dir=.git', 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, '--git-dir=.git', 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"@0@-@1@ (" __DATE__ ", branch \'@2@\')"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

add_project_arguments(
	[
		'-DHAVE_PNG=@0@'.format(png_dep.found() ? 1 : 0),
		'-DHAVE_SVG=@0@'.format(svg_dep.found() ? 1 : 0),
		'-DDEBUG=@0@'.format(get_option('buildtype').startswith('debug') ? 1 : 0),
		'-DVERSION=@0@'.format(version),
	],
	language: 'c',
)

inc = include_directories('include')

executable(
    'sbar',
    src,
	c_args: ['-DLOG_PREFIX="sbar: "',],
	include_directories: inc,
    dependencies: deps,
    install: true,
)

subdir('examples')

summary({
	'PNG image blocks' : png_dep.found(),
    'SVG image blocks' : svg_dep.found(),
	'swaybar example' : get_option('swaybar_example'),
	'swaybar example sd-bus provider' : get_option('swaybar_example') ? sdbus_dep.name() : '',
}, bool_yn: true)
