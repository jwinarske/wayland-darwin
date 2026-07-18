project('wlr-darwin', 'c',
	version: '0.1.0',
	license: 'MIT',
	default_options: ['c_std=c11', 'warning_level=2'],
)

if host_machine.system() != 'darwin'
	error('libwlr-darwin targets macOS (Darwin) only')
endif

# Objective-C is added explicitly so this stays scoped to the out-of-tree
# project (D7); ARC is enabled for the ObjC sources only.
add_languages('objc', native: false)

add_project_arguments('-DWLR_USE_UNSTABLE', language: ['c', 'objc'])
add_project_arguments('-fobjc-arc', language: 'objc')

wlroots = dependency('wlroots-0.21')
wayland_server = dependency('wayland-server')

frameworks = [
	'-framework', 'AppKit',
	'-framework', 'QuartzCore',
	'-framework', 'CoreVideo',
	'-framework', 'CoreGraphics',
	'-framework', 'CoreFoundation',
	'-framework', 'IOSurface',
	'-framework', 'Metal',
	'-framework', 'Foundation',
]

# ../wlroots-darwin/compat provides the vendored <linux/input-event-codes.h>
# used by the kVK->evdev keymap.
inc = include_directories('include', 'src', '../wlroots-darwin/compat')

lib_wlr_darwin = library('wlr-darwin',
	[
		'src/backend.c', 'src/output.c', 'src/allocator.c', 'src/keymap.c',
		'src/renderer.c', 'src/cocoa.m', 'src/metal.m',
	],
	include_directories: inc,
	dependencies: [wlroots, wayland_server],
	link_args: frameworks,
	install: true,
)

install_headers('include/wlr-darwin.h')

wlr_darwin = declare_dependency(
	link_with: lib_wlr_darwin,
	include_directories: include_directories('include'),
	dependencies: [wlroots, wayland_server],
)

if get_option('examples')
	executable('darwin-smoke', 'example/darwin-smoke.c',
		dependencies: [wlr_darwin],
		link_args: frameworks,
	)
	# Headless — CI runs this to verify the Metal renderer actually renders.
	executable('metal-smoke', 'example/metal-smoke.c',
		dependencies: [wlr_darwin],
		link_args: frameworks,
	)
	# tinywl: wlroots' reference compositor, adapted to the Darwin backend.
	executable('darwin-tinywl', 'example/tinywl.c',
		dependencies: [wlr_darwin, dependency('xkbcommon')],
		link_args: frameworks,
	)
endif
