/*
 * clawt-image-catalog.c - Container images a client can offer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-image-catalog.h"

/*
 * Every reference carries its registry.
 *
 * A bare "fedora:44" is resolved through podman's unqualified-search
 * list, which is per-machine configuration: the same clawtilla.yaml then
 * pulls a different image on two hosts, and the failure -- if there is
 * one -- arrives much later as a missing package.
 */
static const ClawtImageInfo images[] = {
    { "registry.fedoraproject.org/fedora:44", "Fedora 44",
      "the default; what clawtilla is developed and tested on", "Fedora" },
    { "registry.fedoraproject.org/fedora:43", "Fedora 43",
      "the previous release", "Fedora" },
    { "registry.fedoraproject.org/fedora-minimal:44", "Fedora 44 minimal",
      "microdnf instead of dnf; smaller, fewer tools", "Fedora" },
    { "registry.fedoraproject.org/fedora-toolbox:44", "Fedora Toolbox 44",
      "Fedora with the interactive tooling already installed", "Fedora" },

    { "quay.io/centos/centos:stream10", "CentOS Stream 10",
      "closest to RHEL", "Enterprise Linux" },
    { "docker.io/library/rockylinux:9", "Rocky Linux 9",
      "RHEL-compatible", "Enterprise Linux" },

    { "docker.io/library/debian:stable-slim", "Debian stable (slim)",
      "small, and apt is familiar", "Debian and Ubuntu" },
    { "docker.io/library/debian:testing-slim", "Debian testing (slim)",
      "newer packages", "Debian and Ubuntu" },
    { "docker.io/library/ubuntu:24.04", "Ubuntu 24.04 LTS",
      NULL, "Debian and Ubuntu" },

    { "docker.io/library/alpine:latest", "Alpine",
      "tiny, musl libc; glibc binaries will not run", "Small" },
    { "docker.io/library/busybox:latest", "BusyBox",
      "smallest useful thing; a shell and little else", "Small" },

    { "docker.io/library/archlinux:latest", "Arch Linux",
      "rolling, and the AUR is a build away", "Rolling" },
    { "registry.opensuse.org/opensuse/tumbleweed:latest",
      "openSUSE Tumbleweed", "rolling", "Rolling" },

    { "docker.io/library/python:3-slim", "Python 3",
      "Python already installed", "Language toolchains" },
    { "docker.io/library/node:22-slim", "Node 22",
      "Node and npm already installed", "Language toolchains" },
    { "docker.io/library/golang:1-alpine", "Go",
      "the Go toolchain", "Language toolchains" },
    { "docker.io/library/rust:1-slim", "Rust",
      "cargo and rustc", "Language toolchains" }
};

const ClawtImageInfo *
clawt_image_catalog_get(gsize *n_images)
{
    g_return_val_if_fail(n_images != NULL, NULL);

    *n_images = G_N_ELEMENTS(images);

    return images;
}

const gchar *
clawt_image_catalog_default(void)
{
    return images[0].reference;
}
