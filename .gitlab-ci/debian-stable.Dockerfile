FROM debian:buster

RUN apt-get update -qq && apt-get install --no-install-recommends -qq -y \
    appstream \
    appstream-util \
    clang \
    clang-tools-7 \
    dbus \
    desktop-file-utils \
    docbook-xsl \
    gcc \
    g++ \
    gettext \
    git \
    gnome-pkg-tools \
    gobject-introspection \
    gsettings-desktop-schemas-dev \
    gtk-doc-tools \
    itstool \
    lcov \
    libaccountsservice-dev \
    libappstream-glib-dev \
    libflatpak-dev \
    libfwupd-dev \
    libgirepository1.0-dev \
    libglib2.0-dev \
    libgnome-desktop-3-dev \
    libgoa-1.0-dev \
    libgspell-1-dev \
    libgtk-3-dev \
    libgudev-1.0-dev \
    libjson-glib-dev \
    libpackagekit-glib2-dev \
    libpam0g-dev \
    libpolkit-gobject-1-dev \
    libsoup2.4-dev \
    libxmlb-dev \
    libxml2-utils \
    ninja-build \
    packagekit \
    pkg-config \
    policykit-1 \
    python3 \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    shared-mime-info \
    sudo \
    unzip \
    valgrind \
    wget \
    xsltproc \
    xz-utils \
 && rm -rf /usr/share/doc/* /usr/share/man/*

RUN pip3 install meson==0.50.0

# Enable passwordless sudo for sudo users
RUN sed -i -e '/%sudo/s/ALL$/NOPASSWD: ALL/' /etc/sudoers

ARG HOST_USER_ID=5555
ENV HOST_USER_ID ${HOST_USER_ID}
RUN useradd -u $HOST_USER_ID -G sudo -ms /bin/bash user

USER user
WORKDIR /home/user

COPY cache-subprojects.sh .
RUN ./cache-subprojects.sh

ENV LANG=C.UTF-8 LANGUAGE=C.UTF-8 LC_ALL=C.UTF-8
