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
void clawt_vm_computer_set_uri(ClawtVmComputer *self, const gchar *uri);
void clawt_vm_computer_set_image(ClawtVmComputer *self, const gchar *image);
void clawt_vm_computer_set_resources(ClawtVmComputer *self,
                                     guint            cpus,
                                     guint            memory_mb);
void clawt_vm_computer_set_ssh(ClawtVmComputer *self,
                               const gchar     *user,
                               const gchar     *key_path,
                               const gchar     *host);

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
