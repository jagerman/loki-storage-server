
local default_deps_base='libsystemd-dev libboost-program-options-dev libboost-system-dev libboost-test-dev ' +
    'libsqlite3-dev libsodium-dev libssl-dev pkg-config';
local default_deps='g++ ' + default_deps_base; // g++ sometimes needs replacement

local submodules_commands = ['git fetch --tags', 'git submodule update --init --recursive --depth=1'];
local submodules = {
    name: 'submodules',
    image: 'drone/git',
    commands: submodules_commands
};

local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q';


// Regular build on a debian-like system:
local debian_pipeline(name, image,
        arch='amd64',
        deps=default_deps,
        build_type='Release',
        lto=false,
        build_tests=true,
        run_tests=false, # Runs full test suite
        cmake_extra='',
        extra_cmds=[],
        extra_steps=[],
        jobs=6,
        allow_fail=false) = {
    kind: 'pipeline',
    type: 'docker',
    name: name,
    platform: { arch: arch },
    steps: [
        submodules,
        {
            name: 'build',
            image: image,
            [if allow_fail then "failure"]: "ignore",
            environment: { SSH_KEY: { from_secret: "SSH_KEY" } },
            commands: [
                'echo "Building on ${DRONE_STAGE_MACHINE}"',
                'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
                apt_get_quiet + ' update',
                apt_get_quiet + ' install -y eatmydata',
                'eatmydata ' + apt_get_quiet + ' dist-upgrade -y',
                'eatmydata ' + apt_get_quiet + ' install -y --no-install-recommends cmake git ca-certificates ninja-build ccache '
                    + deps,
                'mkdir build',
                'cd build',
                'cmake .. -G Ninja -DCMAKE_CXX_FLAGS=-fdiagnostics-color=always -DCMAKE_BUILD_TYPE='+build_type+' ' +
                    '-DLOCAL_MIRROR=https://oxen.rocks/deps -DUSE_LTO=' + (if lto then 'ON ' else 'OFF ') +
                    (if build_tests || run_tests then '-DBUILD_TESTS=ON ' else '') +
                    cmake_extra,
                'ninja -j' + jobs + ' -v',
            ] +
            (if run_tests then ['./unit_test/Test'] else []) +
            extra_cmds,
        }
    ] + extra_steps,
};

// Macos build
local mac_builder(name,
        build_type='Release',
        lto=false,
        build_tests=true,
        run_tests=false,
        cmake_extra='',
        extra_cmds=[],
        extra_steps=[],
        jobs=6,
        allow_fail=false) = {
    kind: 'pipeline',
    type: 'exec',
    name: name,
    platform: { os: 'darwin', arch: 'amd64' },
    steps: [
        { name: 'submodules', commands: submodules_commands },
        {
            name: 'build',
            environment: { SSH_KEY: { from_secret: "SSH_KEY" } },
            commands: [
                // If you don't do this then the C compiler doesn't have an include path containing
                // basic system headers.  WTF apple:
                'export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"',
                'mkdir build',
                'cd build',
                'cmake .. -G Ninja -DCMAKE_CXX_FLAGS=-fcolor-diagnostics -DCMAKE_BUILD_TYPE='+build_type+' ' +
                    '-DLOCAL_MIRROR=https://oxen.rocks/deps -DUSE_LTO=' + (if lto then 'ON ' else 'OFF ') +
                    (if build_tests || run_tests then '-DBUILD_TESTS=ON ' else '') +
                    cmake_extra,
                'ninja -j' + jobs + ' -v'
            ] +
            (if run_tests then ['./unit_test/Test'] else []) +
            extra_cmds,
        }
    ] + extra_steps
};

local static_check_and_upload = [
    '../contrib/drone-check-static-libs.sh',
    'ninja strip',
    'ninja create_tarxz',
    '../contrib/drone-static-upload.sh'
];

local static_build_deps='autoconf automake make file libtool pkg-config patch openssh-client';


[
    // Various debian builds
    debian_pipeline("Debian (w/ tests) (amd64)", "debian:sid", lto=true, run_tests=true),
    debian_pipeline("Debian Debug (amd64)", "debian:sid", build_type='Debug'),
    debian_pipeline("Debian clang-11 (amd64)", "debian:sid", deps='clang-11 '+default_deps_base,
                    cmake_extra='-DCMAKE_C_COMPILER=clang-11 -DCMAKE_CXX_COMPILER=clang++-11 ', lto=true),
    debian_pipeline("Debian buster (i386)", "i386/debian:buster"),
    debian_pipeline("Ubuntu focal (amd64)", "ubuntu:focal"),

    // ARM builds (ARM64 and armhf)
    debian_pipeline("Debian (ARM64)", "debian:sid", arch="arm64", build_tests=false),
    debian_pipeline("Debian buster (armhf)", "arm32v7/debian:buster", arch="arm64", build_tests=false),

    // Static build (on bionic) which gets uploaded to oxen.rocks:
    debian_pipeline("Static (bionic amd64)", "ubuntu:bionic", deps='g++-8 '+static_build_deps,
                    cmake_extra='-DBUILD_STATIC_DEPS=ON -DCMAKE_C_COMPILER=gcc-8 -DCMAKE_CXX_COMPILER=g++-8',
                    build_tests=false, lto=true, extra_cmds=static_check_and_upload),

    // Macos builds:
    mac_builder('macOS (Static)', cmake_extra='-DBUILD_STATIC_DEPS=ON',
                build_tests=false, lto=true, extra_cmds=static_check_and_upload),
    mac_builder('macOS (Release)', run_tests=true),
    mac_builder('macOS (Debug)', build_type='Debug', cmake_extra='-DBUILD_DEBUG_UTILS=ON'),
]
