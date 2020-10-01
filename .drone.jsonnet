local distro = "xenial";
local distro_name = 'Ubuntu 16.04';
local distro_docker = 'ubuntu:xenial';

local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q';

local repo_suffix = '/staging'; // can be /beta or /staging for non-primary repo deps

local submodules = {
    name: 'submodules',
    image: 'drone/git',
    commands: ['git fetch --tags', 'git submodule update --init --recursive --depth=1']
};

local deb_pipeline(image, buildarch='amd64', debarch='amd64', jobs=6) = {
    kind: 'pipeline',
    type: 'docker',
    name: distro_name + ' (' + debarch + ')',
    platform: { arch: buildarch },
    steps: [
        #submodules,
        {
            name: 'build',
            image: image,
            environment: { SSH_KEY: { from_secret: "SSH_KEY" } },
            commands: [
                'echo $DRONE_STAGE_MACHINE',
                'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
                'cp debian/deb.loki.network.gpg /etc/apt/trusted.gpg.d/deb.loki.network.gpg',
                'echo deb http://deb.loki.network' + repo_suffix + ' ' + distro + ' main >/etc/apt/sources.list.d/loki.list',
                apt_get_quiet + ' update',
                apt_get_quiet + ' install -y eatmydata',
                'eatmydata ' + apt_get_quiet + ' dist-upgrade -y',
                'eatmydata ' + apt_get_quiet + ' install --no-install-recommends -y git-buildpackage devscripts equivs clang++-8 ccache openssh-client curl ca-certificates gnupg apt-transport-https',
                'ln -s ../../bin/ccache /usr/lib/ccache/clang-8',
                'ln -s ../../bin/ccache /usr/lib/ccache/clang++-8',
                'curl https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor - >/etc/apt/trusted.gpg.d/kitware.gpg',
                'echo deb https://apt.kitware.com/ubuntu/ bionic main >/etc/apt/sources.list.d/kitware.list',
                'eatmydata ' + apt_get_quiet + ' update',
                'cd debian',
                'eatmydata mk-build-deps -i -r --tool="' + apt_get_quiet + ' -o Debug::pkgProblemResolver=yes --no-install-recommends -y" control',
                'cd ..',
                'eatmydata gbp buildpackage --git-no-pbuilder --git-builder=\'debuild --prepend-path=/usr/lib/ccache --preserve-envvar=CCACHE_*\' --git-upstream-tag=HEAD -us -uc -j' + jobs,
                './debian/ci-upload.sh ' + distro + ' ' + debarch,
            ],
        }
    ]
};

[
    deb_pipeline(distro_docker),
]
