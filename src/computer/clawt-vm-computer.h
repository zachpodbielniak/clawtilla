/*
 * clawt-vm-computer.h - A virtual machine per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two backends behind one type:
 *
 *   libvirt (the default) goes through podomation's vm_virtmanager module
 *   and gets snapshots, device hotplug and migration with it.
 *
 *   qemu drives qemu-system-* directly over QMP, for hosts with no
 *   libvirtd, and trades that surface for having no daemon to install.
 *
 * Commands run in the guest over SSH either way: there is no equivalent of
 * `podman exec` for a VM, and a guest agent would be one more thing to
 * install inside every image.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include "computer/clawt-computer.h"
#include "computer/clawt-pod-bridge.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_VM_COMPUTER (clawt_vm_computer_get_type())

G_DECLARE_FINAL_TYPE(ClawtVmComputer, clawt_vm_computer,
                     CLAWT, VM_COMPUTER, ClawtComputer)

/**
 * clawt_vm_computer_new:
 * @agent_id: the agent this belongs to
 * @backend: libvirt or qemu
 * @bridge: (transfer none) (nullable): the podomation bridge, needed for
 *   the libvirt backend
 *
 * Returns: (transfer full): a new #ClawtVmComputer
 */
ClawtComputer *clawt_vm_computer_new(const gchar     *agent_id,
                                     ClawtVmBackend   backend,
                                     ClawtPodBridge  *bridge);

void clawt_vm_computer_set_domain(ClawtVmComputer *self, const gchar *domain);

/**
 * clawt_vm_computer_set_desktop:
 * @self: a #ClawtVmComputer
 * @desktop: (nullable): a graphical session to build in the guest
 *
 * A cloud image has no desktop at all, so an agent granted one on a VM
 * needs it installed before there is anything to drive.  The session is
 * described here and built by cloud-init on the guest's first boot.
 */
void clawt_vm_computer_set_desktop(ClawtVmComputer   *self,
                                   ClawtGuestDesktop *desktop);

/**
 * clawt_vm_computer_get_desktop:
 * @self: a #ClawtVmComputer
 *
 * Returns: (transfer none) (nullable): the guest's desktop, or %NULL
 */
ClawtGuestDesktop *clawt_vm_computer_get_desktop(ClawtVmComputer *self);

/**
 * clawt_vm_computer_build_desktop_argv:
 * @self: a #ClawtVmComputer
 *
 * The command that reaches the MCP server inside the guest, over SSH as
 * the account the desktop is logged in as.
 *
 * A pure function, so the argv can be asserted on without a hypervisor.
 * It returns %NULL rather than a command that dials nowhere when there is
 * no desktop or no address that reaches the guest.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the argv
 */
GStrv clawt_vm_computer_build_desktop_argv(ClawtVmComputer *self);
void clawt_vm_computer_set_uri(ClawtVmComputer *self, const gchar *uri);
void clawt_vm_computer_set_image(ClawtVmComputer *self, const gchar *image);

/**
 * clawt_vm_emulator_path:
 * @binary: the emulator's file name, such as `qemu-system-x86_64`
 * @roots: (nullable) (array zero-terminated=1): directories to look in
 *   before falling back to `PATH`, or %NULL for the system one
 *
 * Finds the QEMU binary a VM should be run with, preferring a system
 * location over whatever `PATH` happens to resolve.
 *
 * Which binary runs is not a detail. libvirt fills in an `<emulator>` of
 * its own when a domain names none, and it finds it by searching the
 * *session daemon's* `PATH` -- so a host with a package manager ahead of
 * /usr/bin gets a domain pointing at, say,
 * `/var/home/linuxbrew/.linuxbrew/bin/qemu-system-x86_64`. SELinux then
 * refuses it: libvirt's `svirt_t` cannot entrypoint a binary labelled
 * `user_home_t`, so the domain defines, starts, and dies with an AVC
 * denial in the audit log and nothing anywhere naming a path. The same
 * applies to the qemu backend, which spawns the emulator itself.
 *
 * Nothing found anywhere returns %NULL, which leaves each backend as it
 * was: no `<emulator>` element for libvirt to override, and the bare
 * name for the qemu argv. A path that does not exist would be worse than
 * no path at all -- it turns a working default into a domain that will
 * not define.
 *
 * Returns: (transfer full) (nullable): an absolute path, or %NULL
 */
gchar *clawt_vm_emulator_path(const gchar         *binary,
                              const gchar * const *roots);

/**
 * clawt_vm_computer_set_emulator:
 * @self: a #ClawtVmComputer
 * @emulator: (nullable): an absolute path to the QEMU binary, or %NULL
 *   to name none
 *
 * Pins the emulator this VM runs under.
 *
 * Resolved once in clawt_vm_computer_new() so the ordinary path and the
 * tested path are the same one; `computer.vm.emulator` overrides it for
 * a host whose QEMU is somewhere else.
 */
void clawt_vm_computer_set_emulator(ClawtVmComputer *self,
                                    const gchar     *emulator);

/**
 * clawt_vm_computer_get_emulator:
 * @self: a #ClawtVmComputer
 *
 * Returns: (transfer none) (nullable): the emulator, or %NULL when the
 *   backend is left to find its own
 */
const gchar *clawt_vm_computer_get_emulator(ClawtVmComputer *self);

/**
 * clawt_vm_computer_set_packages:
 * @self: a #ClawtVmComputer
 * @packages: (nullable) (array zero-terminated=1): what to install
 *
 * Packages the guest installs at first boot, from
 * `computer.vm.packages`.
 *
 * Read once, by cloud-init, from a seed built before the guest ever
 * ran -- so adding one to a VM that exists needs its computer rebuilt,
 * like every other thing in that seed.
 */
void clawt_vm_computer_set_packages(ClawtVmComputer     *self,
                                    const gchar * const *packages);
/**
 * clawt_vm_computer_parse_resolution:
 * @text: (nullable): a resolution as `WIDTHxHEIGHT`
 * @width: (out) (optional): the width
 * @height: (out) (optional): the height
 *
 * Reads a screen size written by a person.
 *
 * Pure, and separate from anything that needs a hypervisor, so a typo
 * can be refused while somebody is still looking at the line they typed
 * rather than when a domain fails to define.
 *
 * Returns: %TRUE when @text is a resolution
 */
gboolean clawt_vm_computer_parse_resolution(const gchar *text,
                                            guint       *width,
                                            guint       *height);

/**
 * clawt_vm_computer_set_resolution:
 * @self: a #ClawtVmComputer
 * @resolution: (nullable): `WIDTHxHEIGHT`, or %NULL for the default
 *
 * The size the virtual GPU reports as its preferred mode.
 *
 * Set on the *host* rather than inside the guest: GNOME takes the
 * preferred mode when nothing says otherwise, so there is no
 * distribution-specific file to write and nothing that has to happen at
 * first boot -- which means it applies on the VM's next boot rather than
 * needing the machine rebuilt.
 */
void clawt_vm_computer_set_resolution(ClawtVmComputer *self,
                                      const gchar     *resolution);

void clawt_vm_computer_set_resources(ClawtVmComputer *self,
                                     guint            cpus,
                                     guint            memory_mb,
                                     guint            disk_gb);
/**
 * clawt_vm_computer_set_ssh:
 * @self: a #ClawtVmComputer
 * @user: (nullable): the login to use in the guest
 * @key_path: (nullable): the private key to authenticate with
 * @host: (nullable): the guest's address
 * @port: the guest's SSH port, or 0 to leave it alone
 *
 * @host is what the user configured.  Leaving it unset is the ordinary
 * case: provisioning then forwards a host port to the guest and fills
 * both in itself.
 */
void clawt_vm_computer_set_ssh(ClawtVmComputer *self,
                               const gchar     *user,
                               const gchar     *key_path,
                               const gchar     *host,
                               guint            port);

/**
 * clawt_vm_computer_set_cloud_init:
 * @self: a #ClawtVmComputer
 * @enabled: whether to build a NoCloud seed for the guest
 *
 * On by default.  Turn it off for an image that already has a login and
 * an authorized key of its own.
 */
void clawt_vm_computer_set_cloud_init(ClawtVmComputer *self,
                                      gboolean         enabled);

/**
 * clawt_vm_computer_set_port_forward:
 * @self: a #ClawtVmComputer
 * @host_port: a port on 127.0.0.1 to forward to the guest's SSH, or 0
 *
 * Set by provisioning once it has picked a port.  Exposed because the
 * domain XML and the qemu argv are both pure functions of it, which is
 * what lets them be tested without a hypervisor.
 */
void clawt_vm_computer_set_port_forward(ClawtVmComputer *self,
                                        guint            host_port);

/**
 * clawt_vm_computer_set_uuid:
 * @self: a #ClawtVmComputer
 * @uuid: (nullable): the domain's UUID, or %NULL to derive one
 *
 * Set from the domain libvirt already holds, if it holds one.  libvirt
 * refuses to redefine a name under a different UUID, so a domain defined
 * before clawtilla supplied one keeps whatever libvirt invented then.
 */
void clawt_vm_computer_set_uuid(ClawtVmComputer *self, const gchar *uuid);

/**
 * clawt_vm_computer_set_seed_iso:
 * @self: a #ClawtVmComputer
 * @path: (nullable): the cloud-init seed to attach as a CD-ROM
 */
void clawt_vm_computer_set_seed_iso(ClawtVmComputer *self,
                                    const gchar     *path);

/**
 * clawt_vm_computer_get_ssh_host:
 * @self: a #ClawtVmComputer
 *
 * Returns: (nullable): the address commands are run through, or %NULL
 *   when nothing reaches the guest
 */
const gchar *clawt_vm_computer_get_ssh_host(ClawtVmComputer *self);

/**
 * clawt_vm_computer_get_ssh_port:
 * @self: a #ClawtVmComputer
 *
 * Returns: the port commands are run through
 */
guint clawt_vm_computer_get_ssh_port(ClawtVmComputer *self);

/**
 * clawt_vm_computer_set_snapshot_on_start:
 * @self: a #ClawtVmComputer
 * @snapshot: whether to snapshot each time the VM starts
 *
 * libvirt only.  Lets a session be rolled back after an agent does
 * something regrettable.
 */
void clawt_vm_computer_set_snapshot_on_start(ClawtVmComputer *self,
                                             gboolean         snapshot);

ClawtVmBackend clawt_vm_computer_get_backend(ClawtVmComputer *self);

/**
 * clawt_vm_computer_build_ssh_argv:
 * @self: a #ClawtVmComputer
 * @command_argv: (array zero-terminated=1): the command to run in the guest
 * @working_dir: (nullable): a directory to run it in
 * @timeout_seconds: how long to wait for the connection, or 0
 *
 * Builds the `ssh` command line, including the guest's port and a
 * known_hosts file belonging to this agent alone.
 *
 * Returns: (transfer full) (nullable): the argv, or %NULL when nothing
 *   reaches the guest
 */
GStrv clawt_vm_computer_build_ssh_argv(ClawtVmComputer     *self,
                                       const gchar * const *command_argv,
                                       const gchar         *working_dir,
                                       guint                timeout_seconds);

/**
 * clawt_vm_computer_build_domain_xml:
 * @self: a #ClawtVmComputer
 *
 * Renders the libvirt domain XML, including a virtiofs filesystem device
 * per mount.
 *
 * Exposed so it can be tested without libvirtd: getting the shared-memory
 * backing or an accessmode wrong is silent until a guest cannot see a
 * share, and that is worth a unit test rather than an integration one.
 *
 * Returns: (transfer full): the domain XML
 */
gchar *clawt_vm_computer_build_domain_xml(ClawtVmComputer *self);

/**
 * clawt_vm_computer_build_qemu_argv:
 * @self: a #ClawtVmComputer
 * @qmp_socket: where QEMU should expose its monitor
 *
 * Renders the qemu-system-* command line for the direct backend.
 *
 * Exposed for the same reason as the domain XML.
 *
 * Returns: (transfer full) (array zero-terminated=1): the command
 */
GStrv clawt_vm_computer_build_qemu_argv(ClawtVmComputer *self,
                                        const gchar     *qmp_socket);

G_END_DECLS
