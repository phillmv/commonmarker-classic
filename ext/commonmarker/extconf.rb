# frozen_string_literal: true

# mkmf is configured through global variables.
# rubocop:disable Style/GlobalVars

require "mkmf"
require "open3"
require "shellwords"

# The extension is built from commonmarker.c alone, which talks to comrak
# through the comrak_ffi static library. The cmark-gfm sources that still sit
# in this directory are not compiled.
$srcs = ["commonmarker.c"]

append_cflags(["-std=c99"])
# Lets Node wrappers move under GC compaction (Ruby 2.7+).
have_func("rb_gc_mark_movable")

ffi_dir = File.join(__dir__, "comrak_ffi")
target_dir = File.join(Dir.pwd, "comrak_ffi-target")
ffi_lib = File.join(target_dir, "release", "libcomrak_ffi.a")

cargo = ENV.fetch("CARGO", "cargo")
abort("cargo is required to build commonmarker (set $CARGO to override)") unless find_executable(cargo)

# `cargo rustc` rather than `cargo build`, so that rustc reports the system
# libraries a Rust static library needs. The Makefile reruns the same command,
# so cargo's fingerprint matches and the rebuild is a no-op unless the Rust
# sources changed.
cargo_cmd = [
  cargo, "rustc", "--release", "--locked",
  "--manifest-path", File.join(ffi_dir, "Cargo.toml"),
  "--target-dir", target_dir,
  "--", "--print", "native-static-libs"
]

message("building comrak_ffi: #{cargo_cmd.shelljoin}\n")
output, status = Open3.capture2e(*cargo_cmd)
$stdout.write(output)
abort("building comrak_ffi failed") unless status.success?

native_libs = output[/native-static-libs: (.*)$/, 1]
abort("could not determine comrak_ffi's native libraries from cargo's output") unless native_libs

$INCFLAGS << " -I#{File.join(ffi_dir, "include").shellescape}"
$LOCAL_LIBS << " #{ffi_lib.shellescape} #{native_libs}"
# Keep the Rust archive's symbols (comrak_*, rust_eh_personality, ...) out of
# the extension's dynamic symbol table.
append_ldflags("-Wl,--exclude-libs,ALL")

create_makefile("commonmarker/commonmarker")

File.open("Makefile", "a") do |mk|
  mk.puts(<<~MAKE)

    COMRAK_FFI_LIB = #{ffi_lib.shellescape}
    $(DLLIB): $(COMRAK_FFI_LIB)
    $(COMRAK_FFI_LIB): comrak_ffi
    commonmarker.o: #{File.join(ffi_dir, "include", "comrak_ffi.h").shellescape}
    .PHONY: comrak_ffi
    comrak_ffi:
    \t#{cargo_cmd.shelljoin}
  MAKE
end
# rubocop:enable Style/GlobalVars
