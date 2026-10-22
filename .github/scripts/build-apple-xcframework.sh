#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-"$ROOT/build/mobile/apple"}"
DIST_DIR="${DIST_DIR:-"$ROOT/dist"}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-12.0}"
TVOS_DEPLOYMENT_TARGET="${TVOS_DEPLOYMENT_TARGET:-12.0}"
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
VERSION="$(tr -d '[:space:]' < "$ROOT/RELEASEVERSION")"
PACKAGE_ROOT="$BUILD_ROOT/package/libass-apple-xcframework-$VERSION"
HEADERS_DIR="$BUILD_ROOT/package/headers"

die() {
    echo "error: $*" >&2
    exit 1
}

ensure_wraps() {
    cd "$ROOT"
    mkdir -p subprojects
    meson wrap update-db

    local wrap
    for wrap in freetype2 fribidi harfbuzz; do
        if [ ! -f "subprojects/$wrap.wrap" ]; then
            meson wrap install "$wrap"
        fi
    done

    meson subprojects download freetype2 fribidi harfbuzz
}

slice_settings() {
    case "$1" in
        ios-arm64)
            SDK="iphoneos"
            ARCH="arm64"
            CPU_FAMILY="aarch64"
            CPU="arm64"
            MIN_VERSION_FLAG="-mios-version-min=$IOS_DEPLOYMENT_TARGET"
            ;;
        ios-simulator-arm64)
            SDK="iphonesimulator"
            ARCH="arm64"
            CPU_FAMILY="aarch64"
            CPU="arm64"
            MIN_VERSION_FLAG="-mios-simulator-version-min=$IOS_DEPLOYMENT_TARGET"
            ;;
        ios-simulator-x86_64)
            SDK="iphonesimulator"
            ARCH="x86_64"
            CPU_FAMILY="x86_64"
            CPU="x86_64"
            MIN_VERSION_FLAG="-mios-simulator-version-min=$IOS_DEPLOYMENT_TARGET"
            ;;
        tvos-arm64)
            SDK="appletvos"
            ARCH="arm64"
            CPU_FAMILY="aarch64"
            CPU="arm64"
            MIN_VERSION_FLAG="-mtvos-version-min=$TVOS_DEPLOYMENT_TARGET"
            ;;
        tvos-simulator-arm64)
            SDK="appletvsimulator"
            ARCH="arm64"
            CPU_FAMILY="aarch64"
            CPU="arm64"
            MIN_VERSION_FLAG="-mtvos-simulator-version-min=$TVOS_DEPLOYMENT_TARGET"
            ;;
        tvos-simulator-x86_64)
            SDK="appletvsimulator"
            ARCH="x86_64"
            CPU_FAMILY="x86_64"
            CPU="x86_64"
            MIN_VERSION_FLAG="-mtvos-simulator-version-min=$TVOS_DEPLOYMENT_TARGET"
            ;;
        macos-arm64)
            SDK="macosx"
            ARCH="arm64"
            CPU_FAMILY="aarch64"
            CPU="arm64"
            MIN_VERSION_FLAG="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
            ;;
        macos-x86_64)
            SDK="macosx"
            ARCH="x86_64"
            CPU_FAMILY="x86_64"
            CPU="x86_64"
            MIN_VERSION_FLAG="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
            ;;
        *)
            die "unknown Apple slice: $1"
            ;;
    esac
}

quote_array() {
    local out=""
    local item

    for item in "$@"; do
        if [ -n "$out" ]; then
            out+=", "
        fi
        out+="'$item'"
    done

    printf '[%s]' "$out"
}

write_cross_file() {
    local slice="$1"
    local cross_file="$2"
    local sdk_path
    local clang
    local clangxx
    local ar
    local ranlib
    local strip
    local cflags

    slice_settings "$slice"
    sdk_path="$(xcrun --sdk "$SDK" --show-sdk-path)"
    clang="$(xcrun --sdk "$SDK" --find clang)"
    clangxx="$(xcrun --sdk "$SDK" --find clang++)"
    ar="$(xcrun --sdk "$SDK" --find ar)"
    ranlib="$(xcrun --sdk "$SDK" --find ranlib)"
    strip="$(xcrun --sdk "$SDK" --find strip)"
    cflags=(-arch "$ARCH" -isysroot "$sdk_path" "$MIN_VERSION_FLAG" -fPIC)

    cat > "$cross_file" <<EOF
[binaries]
c = '$clang'
cpp = '$clangxx'
ar = '$ar'
ranlib = '$ranlib'
strip = '$strip'
nasm = 'nasm'
pkgconfig = 'false'

[host_machine]
system = 'darwin'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = []

[built-in options]
c_args = $(quote_array "${cflags[@]}")
c_link_args = $(quote_array "${cflags[@]}")
cpp_args = $(quote_array "${cflags[@]}")
cpp_link_args = $(quote_array "${cflags[@]}")
EOF
}

common_meson_options() {
    printf '%s\n' \
        --buildtype=release \
        --wrap-mode=forcefallback \
        -Ddefault_library=static \
        -Db_staticpic=true \
        -Dtest=disabled \
        -Dcompare=disabled \
        -Dprofile=disabled \
        -Dfuzz=disabled \
        -Dcheckasm=disabled \
        -Dfontconfig=disabled \
        -Ddirectwrite=disabled \
        -Dcoretext=enabled \
        -Drequire-system-font-provider=true \
        -Dharfbuzz:chafa=disabled \
        -Dharfbuzz:docs=disabled \
        -Dharfbuzz:icu=disabled \
        -Dharfbuzz:raster=disabled \
        -Dharfbuzz:subset=disabled \
        -Dharfbuzz:utilities=disabled \
        -Dharfbuzz:vector=disabled \
        -Dharfbuzz:with_libstdcxx=false \
        -Dfreetype2:zlib=disabled \
        -Dfreetype2:bzip2=disabled \
        -Dfreetype2:png=disabled \
        -Dfreetype2:brotli=disabled \
        -Dfreetype2:harfbuzz=disabled \
        -Dfreetype2:tests=disabled \
        -Dfribidi:docs=false \
        -Dfribidi:tests=false
}

collect_archives() {
    local build_dir="$1"

    find "$build_dir" -type f -name '*.a' \
        ! -path '*/meson-private/*' \
        ! -path '*/meson-uninstalled/*' \
        | sort
}

combine_archives() {
    local out="$1"
    shift

    rm -f "$out"
    xcrun libtool -static -o "$out" "$@"
    xcrun ranlib "$out"
}

copy_licenses() {
    local dest="$1"
    local subdir
    local name

    mkdir -p "$dest/libass"
    cp "$ROOT/COPYING" "$dest/libass/"

    for name in freetype fribidi harfbuzz; do
        subdir="$(find "$ROOT/subprojects" -mindepth 1 -maxdepth 1 -type d -name "$name*" | sort | head -n 1)"
        [ -n "$subdir" ] || continue
        mkdir -p "$dest/$name"
        find "$subdir" -maxdepth 3 -type f \( \
            -iname 'COPYING*' -o \
            -iname 'LICENSE*' -o \
            -iname 'AUTHORS*' -o \
            -iname 'FTL.TXT' -o \
            -iname 'GPLv2.TXT' \
        \) -exec cp {} "$dest/$name/" \;
    done
}

write_readme() {
    cat > "$PACKAGE_ROOT/README.md" <<EOF
# libass Apple XCFramework $VERSION

This package contains \`libass.xcframework\` with static slices for iOS, tvOS,
and macOS:

- iOS device arm64
- iOS simulator arm64 and x86_64
- tvOS device arm64
- tvOS simulator arm64 and x86_64
- macOS arm64 and x86_64

The static libraries are combined archives built from libass plus the WrapDB
FreeType, FriBidi, and HarfBuzz fallback dependencies used by the CI build.
Public libass headers are embedded in the XCFramework. Consumers still need to
link the Apple system font frameworks used by libass: CoreText and
CoreFoundation on iOS/tvOS, and ApplicationServices plus CoreFoundation on
macOS.
EOF
}

build_slice() {
    local slice="$1"
    local build_dir="$BUILD_ROOT/build-$slice"
    local install_dest="$BUILD_ROOT/install-$slice"
    local prefix="/libass-apple-$slice"
    local cross_file="$BUILD_ROOT/cross-$slice.ini"
    local archive_list="$BUILD_ROOT/archives-$slice.txt"
    local out_dir="$BUILD_ROOT/slices/$slice"

    echo "Building Apple $slice"
    mkdir -p "$BUILD_ROOT" "$out_dir"
    write_cross_file "$slice" "$cross_file"

    rm -rf "$build_dir" "$install_dest"
    meson setup "$build_dir" "$ROOT" \
        --cross-file "$cross_file" \
        --prefix "$prefix" \
        $(common_meson_options)
    meson compile -C "$build_dir"
    DESTDIR="$install_dest" meson install -C "$build_dir"

    collect_archives "$build_dir" > "$archive_list"
    if [ ! -s "$archive_list" ]; then
        die "no static archives were produced for $slice"
    fi

    archives=()
    while IFS= read -r archive; do
        archives+=("$archive")
    done < "$archive_list"
    combine_archives "$out_dir/libass.a" "${archives[@]}"
    xcrun strip -S "$out_dir/libass.a"

    if [ ! -d "$HEADERS_DIR" ]; then
        cp -R "$install_dest$prefix/include" "$HEADERS_DIR"
    fi
}

create_universal_library() {
    local out="$1"
    shift

    mkdir -p "$(dirname "$out")"
    rm -f "$out"
    xcrun lipo -create "$@" -output "$out"
    xcrun ranlib "$out"
}

main() {
    command -v xcodebuild >/dev/null || die "xcodebuild is required"

    rm -rf "$PACKAGE_ROOT" "$HEADERS_DIR"
    mkdir -p "$PACKAGE_ROOT" "$BUILD_ROOT/package"

    ensure_wraps

    local slice
    for slice in \
        ios-arm64 \
        ios-simulator-arm64 \
        ios-simulator-x86_64 \
        tvos-arm64 \
        tvos-simulator-arm64 \
        tvos-simulator-x86_64 \
        macos-arm64 \
        macos-x86_64
    do
        build_slice "$slice"
    done

    create_universal_library \
        "$BUILD_ROOT/platforms/ios-simulator/libass.a" \
        "$BUILD_ROOT/slices/ios-simulator-arm64/libass.a" \
        "$BUILD_ROOT/slices/ios-simulator-x86_64/libass.a"

    create_universal_library \
        "$BUILD_ROOT/platforms/tvos-simulator/libass.a" \
        "$BUILD_ROOT/slices/tvos-simulator-arm64/libass.a" \
        "$BUILD_ROOT/slices/tvos-simulator-x86_64/libass.a"

    create_universal_library \
        "$BUILD_ROOT/platforms/macos/libass.a" \
        "$BUILD_ROOT/slices/macos-arm64/libass.a" \
        "$BUILD_ROOT/slices/macos-x86_64/libass.a"

    xcodebuild -create-xcframework \
        -library "$BUILD_ROOT/slices/ios-arm64/libass.a" -headers "$HEADERS_DIR" \
        -library "$BUILD_ROOT/platforms/ios-simulator/libass.a" -headers "$HEADERS_DIR" \
        -library "$BUILD_ROOT/slices/tvos-arm64/libass.a" -headers "$HEADERS_DIR" \
        -library "$BUILD_ROOT/platforms/tvos-simulator/libass.a" -headers "$HEADERS_DIR" \
        -library "$BUILD_ROOT/platforms/macos/libass.a" -headers "$HEADERS_DIR" \
        -output "$PACKAGE_ROOT/libass.xcframework"

    copy_licenses "$PACKAGE_ROOT/licenses"
    write_readme

    mkdir -p "$DIST_DIR"
    (
        cd "$BUILD_ROOT/package"
        zip -qry "$DIST_DIR/libass-apple-xcframework-$VERSION.zip" "libass-apple-xcframework-$VERSION"
    )
    echo "Wrote $DIST_DIR/libass-apple-xcframework-$VERSION.zip"
}

main "$@"
