FROM debian:9

RUN export DEBIAN_FRONTEND=noninteractive && \
    apt-get update && \
    apt-get dist-upgrade -y && \
    apt-get install --no-install-recommends -y \
            augeas-lenses \
            augeas-tools \
            autoconf \
            automake \
            autopoint \
            bash \
            bash-completion \
            ca-certificates \
            ccache \
            chrony \
            dnsmasq-base \
            dwarves \
            ebtables \
            flake8 \
            gcc \
            gdb \
            gettext \
            git \
            iproute2 \
            kmod \
            libc-dev-bin \
            libtool \
            libtool-bin \
            libxml2-utils \
            locales \
            lsof \
            lvm2 \
            make \
            net-tools \
            nfs-common \
            ninja-build \
            numad \
            open-iscsi \
            parted \
            patch \
            perl \
            pkgconf \
            policykit-1 \
            python3 \
            python3-docutils \
            python3-pip \
            python3-setuptools \
            python3-wheel \
            qemu-utils \
            radvd \
            screen \
            scrub \
            sheepdog \
            strace \
            sudo \
            vim \
            xsltproc \
            zfs-fuse && \
    apt-get autoremove -y && \
    apt-get autoclean -y && \
    sed -Ei 's,^# (en_US\.UTF-8 .*)$,\1,' /etc/locale.gen && \
    dpkg-reconfigure locales && \
    mkdir -p /usr/libexec/ccache-wrappers && \
    ln -s /usr/bin/ccache /usr/libexec/ccache-wrappers/mips-linux-gnu-cc && \
    ln -s /usr/bin/ccache /usr/libexec/ccache-wrappers/mips-linux-gnu-$(basename /usr/bin/gcc)

RUN export DEBIAN_FRONTEND=noninteractive && \
    dpkg --add-architecture mips && \
    apt-get update && \
    apt-get dist-upgrade -y && \
    apt-get install --no-install-recommends -y dpkg-dev && \
    apt-get install --no-install-recommends -y \
            gcc-mips-linux-gnu \
            glusterfs-common:mips \
            libacl1-dev:mips \
            libapparmor-dev:mips \
            libattr1-dev:mips \
            libaudit-dev:mips \
            libavahi-client-dev:mips \
            libblkid-dev:mips \
            libc6-dev:mips \
            libcap-ng-dev:mips \
            libcurl4-gnutls-dev:mips \
            libdbus-1-dev:mips \
            libdevmapper-dev:mips \
            libfuse-dev:mips \
            libglib2.0-dev:mips \
            libgnutls28-dev:mips \
            libiscsi-dev:mips \
            libncurses5-dev:mips \
            libnl-3-dev:mips \
            libnl-route-3-dev:mips \
            libnuma-dev:mips \
            libparted-dev:mips \
            libpcap0.8-dev:mips \
            libpciaccess-dev:mips \
            librbd-dev:mips \
            libreadline-dev:mips \
            libsanlock-dev:mips \
            libsasl2-dev:mips \
            libselinux1-dev:mips \
            libssh-gcrypt-dev:mips \
            libssh2-1-dev:mips \
            libtirpc-dev:mips \
            libudev-dev:mips \
            libxml2-dev:mips \
            libyajl-dev:mips \
            xfslibs-dev:mips && \
    apt-get autoremove -y && \
    apt-get autoclean -y

RUN pip3 install \
         meson==0.49.0

ENV LANG "en_US.UTF-8"

ENV MAKE "/usr/bin/make"
ENV NINJA "/usr/bin/ninja"
ENV PYTHON "/usr/bin/python3"

ENV CCACHE_WRAPPERSDIR "/usr/libexec/ccache-wrappers"

ENV ABI "mips-linux-gnu"
ENV CONFIGURE_OPTS "--host=mips-linux-gnu"
