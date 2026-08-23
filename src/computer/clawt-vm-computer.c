/*
 * clawt-vm-computer.c - A virtual machine per agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-vm-computer.h"

#include <string.h>
#include <glib/gstdio.h>

#define MAX_OUTPUT_BYTES (256 * 1024)

/*
 * How long a guest gets to act on a shutdown before it is powered off.
 * Long enough for a real system to stop its services, short enough that
 * stopping an agent is not something you go and make tea during.
 */
#define SHUTDOWN_GRACE_SECONDS (30)

struct _ClawtVmComputer {
    ClawtComputer parent_instance;

    ClawtVmBackend  backend;
    ClawtPodBridge *bridge;

    gchar *domain;
    gchar *uri;
    gchar *image;
    gchar *overlay;
    gchar *ssh_user;
    gchar *ssh_key;
    gchar *ssh_pubkey;
    gchar *ssh_host;
    gchar *seed_iso;
    gchar *uuid;

    /*
     * The graphical session to build inside the guest, or NULL for a
     * headless VM -- which is every VM unless the agent was granted a
     * desktop.
     */
    ClawtGuestDesktop *desktop;

    guint    cpus;
    guint    memory_mb;
    guint    disk_gb;
    guint    ssh_port;

    /*
     * The host port forwarded to the guest's SSH.  Zero means nothing is
     * forwarded, which is the case when the address came from the config
     * and the user has their own route to the guest.
     */
    guint    forward_port;

    gboolean snapshot_on_start;
    gboolean cloud_init;

    GSubprocess *qemu;
    gchar       *qmp_socket;
};

G_DEFINE_FINAL_TYPE(ClawtVmComputer, clawt_vm_computer, CLAWT_TYPE_COMPUTER)

static gboolean libvirt_has_domain(ClawtVmComputer *self);
static GStrv    build_ssh_argv_as(ClawtVmComputer *self,
                                  const gchar     *login,
                                  const gchar     *command,
                                  guint            timeout_seconds,
                                  gboolean         interactive_stream);

ClawtComputer *
clawt_vm_computer_new(const gchar    *agent_id,
                      ClawtVmBackend  backend,
                      ClawtPodBridge *bridge)
{
    ClawtVmComputer *self = g_object_new(CLAWT_TYPE_VM_COMPUTER, NULL);

    self->backend = backend;

    if (bridge != NULL)
        self->bridge = g_object_ref(bridge);

    self->domain = g_strdup_printf("clawt-%s",
                                   agent_id != NULL ? agent_id : "agent");

    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    return CLAWT_COMPUTER(self);
}

#define SETTER(name, field)                                            \
    void                                                               \
    clawt_vm_computer_set_##name(ClawtVmComputer *self,                \
                                 const gchar     *value)               \
    {                                                                  \
        g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));                  \
        if (value == NULL)                                             \
            return;                                                    \
        g_free(self->field);                                           \
        self->field = g_strdup(value);                                 \
    }

SETTER(domain, domain)
SETTER(uri, uri)
SETTER(image, image)

#undef SETTER

void
clawt_vm_computer_set_resources(ClawtVmComputer *self,
                                guint            cpus,
                                guint            memory_mb,
                                guint            disk_gb)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    if (cpus > 0)
        self->cpus = cpus;
    if (memory_mb > 0)
        self->memory_mb = memory_mb;
    if (disk_gb > 0)
        self->disk_gb = disk_gb;
}

void
clawt_vm_computer_set_ssh(ClawtVmComputer *self,
                          const gchar     *user,
                          const gchar     *key_path,
                          const gchar     *host,
                          guint            port)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    if (user != NULL) {
        g_free(self->ssh_user);
        self->ssh_user = g_strdup(user);
    }

    if (key_path != NULL) {
        g_free(self->ssh_key);
        self->ssh_key = clawt_expand_path(key_path);
    }

    if (host != NULL) {
        g_free(self->ssh_host);
        self->ssh_host = g_strdup(host);
    }

    if (port > 0)
        self->ssh_port = port;
}

void
clawt_vm_computer_set_desktop(ClawtVmComputer   *self,
                              ClawtGuestDesktop *desktop)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    g_clear_pointer(&self->desktop, clawt_guest_desktop_unref);

    if (desktop != NULL)
        self->desktop = clawt_guest_desktop_ref(desktop);
}

ClawtGuestDesktop *
clawt_vm_computer_get_desktop(ClawtVmComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);

    return self->desktop;
}

void
clawt_vm_computer_set_cloud_init(ClawtVmComputer *self, gboolean enabled)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    self->cloud_init = enabled;
}

void
clawt_vm_computer_set_port_forward(ClawtVmComputer *self, guint host_port)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    self->forward_port = host_port;
}

const gchar *
clawt_vm_computer_get_ssh_host(ClawtVmComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);

    return self->ssh_host;
}

guint
clawt_vm_computer_get_ssh_port(ClawtVmComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), 0);

    return self->ssh_port;
}

void
clawt_vm_computer_set_uuid(ClawtVmComputer *self, const gchar *uuid)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    g_free(self->uuid);
    self->uuid = g_strdup(uuid);
}

void
clawt_vm_computer_set_seed_iso(ClawtVmComputer *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    g_free(self->seed_iso);
    self->seed_iso = g_strdup(path);
}

void
clawt_vm_computer_set_snapshot_on_start(ClawtVmComputer *self,
                                        gboolean         snapshot)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    self->snapshot_on_start = snapshot;
}

ClawtVmBackend
clawt_vm_computer_get_backend(ClawtVmComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), CLAWT_VM_BACKEND_LIBVIRT);

    return self->backend;
}

/* ── Domain XML ──────────────────────────────────────────────────── */

/*
 * A UUID derived from the domain's name, so it is the same every time.
 *
 * Without one libvirt invents a fresh UUID on each define and then
 * refuses the name it already holds -- "domain 'clawt-x' already exists
 * with uuid ..." -- so provisioning worked exactly once and every later
 * start of the same agent failed. Naming the UUID makes define a
 * redefine, which is what it was always meant to be.
 *
 * Built like a version 5 UUID: SHA-1 of the name, with the version and
 * variant bits set so libvirt accepts it as well formed.
 */
static gchar *
stable_uuid(const gchar *name)
{
    g_autofree gchar *digest = NULL;
    gchar hex[33];

    digest = g_compute_checksum_for_string(G_CHECKSUM_SHA1, name, -1);

    memcpy(hex, digest, 32);
    hex[32] = '\0';

    hex[12] = '5';
    hex[16] = '8';

    return g_strdup_printf("%.8s-%.4s-%.4s-%.4s-%.12s",
                           hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

gchar *
clawt_vm_computer_build_domain_xml(ClawtVmComputer *self)
{
    g_autoptr(GString) out = NULL;
    g_autofree gchar *escaped_domain = NULL;
    GPtrArray *mounts;
    guint i;

    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);

    out = g_string_new(NULL);
    escaped_domain = g_markup_escape_text(self->domain, -1);
    mounts = clawt_computer_get_mounts(CLAWT_COMPUTER(self));

    g_string_append(out, "<domain type='kvm'>\n");
    g_string_append_printf(out, "  <name>%s</name>\n", escaped_domain);

    {
        /*
         * A domain libvirt already knows keeps the UUID it already has;
         * only a new one gets the derived value.
         */
        g_autofree gchar *derived = NULL;
        const gchar *uuid = self->uuid;

        if (uuid == NULL) {
            derived = stable_uuid(self->domain);
            uuid = derived;
        }

        g_string_append_printf(out, "  <uuid>%s</uuid>\n", uuid);
    }

    g_string_append_printf(out, "  <memory unit='MiB'>%u</memory>\n",
                           self->memory_mb);
    g_string_append_printf(out, "  <vcpu>%u</vcpu>\n", self->cpus);

    /*
     * virtiofs needs shared memory backing, and libvirt rejects the device
     * without it.  Emitted whenever there is a mount at all, since the
     * failure is otherwise a rejected domain definition whose message does
     * not obviously point here.
     */
    if (mounts != NULL && mounts->len > 0) {
        g_string_append(out,
            "  <memoryBacking>\n"
            "    <source type='memfd'/>\n"
            "    <access mode='shared'/>\n"
            "  </memoryBacking>\n");
    }

    g_string_append(out,
        "  <os>\n"
        "    <type arch='x86_64' machine='q35'>hvm</type>\n"
        "    <boot dev='hd'/>\n"
        "  </os>\n"
        "  <features><acpi/><apic/></features>\n"
        "  <devices>\n");

    if (self->overlay != NULL || self->image != NULL) {
        g_autofree gchar *disk = g_markup_escape_text(
            self->overlay != NULL ? self->overlay : self->image, -1);

        g_string_append_printf(out,
            "    <disk type='file' device='disk'>\n"
            "      <driver name='qemu' type='qcow2'/>\n"
            "      <source file='%s'/>\n"
            "      <target dev='vda' bus='virtio'/>\n"
            "    </disk>\n", disk);
    }

    /*
     * The seed is a CD-ROM rather than a disk because that is what every
     * cloud image's NoCloud datasource expects to find, and because a
     * read-only device cannot be scribbled over by the guest.
     */
    if (self->seed_iso != NULL) {
        g_autofree gchar *seed = g_markup_escape_text(self->seed_iso, -1);

        g_string_append_printf(out,
            "    <disk type='file' device='cdrom'>\n"
            "      <driver name='qemu' type='raw'/>\n"
            "      <source file='%s'/>\n"
            "      <target dev='sda' bus='sata'/>\n"
            "      <readonly/>\n"
            "    </disk>\n", seed);
    }

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        g_autofree gchar *source = clawt_mount_resolved_source(mount);
        g_autofree gchar *escaped_source = NULL;
        g_autofree gchar *escaped_target = NULL;

        if (source == NULL)
            continue;

        escaped_source = g_markup_escape_text(source, -1);

        /*
         * The guest mounts by tag, not by path, so the tag is derived from
         * the target and kept stable.
         */
        escaped_target = g_markup_escape_text(
            clawt_mount_get_target(mount), -1);

        g_string_append_printf(out,
            "    <filesystem type='mount' accessmode='passthrough'>\n"
            "      <driver type='virtiofs'/>\n"
            "      <source dir='%s'/>\n"
            "      <target dir='%s'/>\n"
            "%s"
            "    </filesystem>\n",
            escaped_source, escaped_target,
            clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RO
                ? "      <readonly/>\n" : "");
    }

    /*
     * A GPU, and only when there is a desktop to draw with it.
     *
     * Without one the guest has no /dev/dri at all, and GDM says so --
     * "It appears that your system does not have a primary GPU!" -- and
     * then starts nothing. Everything above it looks healthy: cloud-init
     * finishes, gdm.service is active, the default target is graphical,
     * and the console sits on the boot log for ever because the boot log
     * is the only console there is.
     *
     * This was invisible for as long as it was, because the qemu backend
     * gets a VGA adapter from qemu by default and the libvirt backend
     * builds its XML from nothing. The feature worked on the backend it
     * was tested on and not on the default one.
     *
     * virtio rather than the older models: it is what a current GNOME on
     * Wayland wants, and every cloud image has the driver.
     */
    g_string_append(out,
        "    <video>\n"
        "      <model type='virtio' heads='1' primary='yes'/>\n"
        "    </video>\n");

    /*
     * Given to every VM, not only the ones with a desktop.
     *
     * A guest with no display device has no /dev/dri and no graphical
     * console, so virt-manager offers only the serial one -- which is the
     * boot log, for ever, whether or not anything is wrong. Somebody
     * looking at that cannot tell a broken guest from a working headless
     * one, and on a guest that *does* have a desktop it looked exactly
     * like GNOME failing to start.
     *
     * A headless VM costs a few megabytes for a framebuffer nobody reads,
     * and gains a console that shows its login prompt. That is the better
     * trade in both directions.
     *
     * Bound to loopback. A VM's screen is a live view of whatever the
     * agent is doing, and that does not belong on the network because a
     * default was convenient.
     */
    g_string_append(out,
        "    <graphics type='vnc' port='-1' autoport='yes' "
        "listen='127.0.0.1'/>\n");

    /*
     * User-mode networking either way, so no bridge appears on the host
     * that the user did not ask for.  Forwarding a port needs the passt
     * backend: libvirt's <portForward> is not supported for the SLIRP
     * one, where a domain asking for it is rejected outright.
     */
    if (self->forward_port > 0) {
        g_string_append_printf(out,
            "    <interface type='user'>\n"
            "      <backend type='passt'/>\n"
            "      <model type='virtio'/>\n"
            "      <portForward proto='tcp' address='127.0.0.1'>\n"
            "        <range start='%u' to='22'/>\n"
            "      </portForward>\n"
            "    </interface>\n", self->forward_port);
    } else {
        g_string_append(out,
            "    <interface type='user'>\n"
            "      <model type='virtio'/>\n"
            "    </interface>\n");
    }

    g_string_append(out,
        "    <console type='pty'/>\n"
        "  </devices>\n"
        "</domain>\n");

    return g_string_free(g_steal_pointer(&out), FALSE);
}

GStrv
clawt_vm_computer_build_qemu_argv(ClawtVmComputer *self,
                                  const gchar     *qmp_socket)
{
    GPtrArray *argv;
    GPtrArray *mounts;
    guint i;

    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);

    argv = g_ptr_array_new_with_free_func(g_free);
    mounts = clawt_computer_get_mounts(CLAWT_COMPUTER(self));

    g_ptr_array_add(argv, g_strdup("qemu-system-x86_64"));
    g_ptr_array_add(argv, g_strdup("-machine"));
    g_ptr_array_add(argv, g_strdup("q35,accel=kvm:tcg"));
    g_ptr_array_add(argv, g_strdup("-smp"));
    g_ptr_array_add(argv, g_strdup_printf("%u", self->cpus));
    g_ptr_array_add(argv, g_strdup("-m"));
    g_ptr_array_add(argv, g_strdup_printf("%u", self->memory_mb));

    /* Headless: nothing here needs a display, and one would grab focus. */
    g_ptr_array_add(argv, g_strdup("-nographic"));

    /*
     * The GPU is named rather than left to qemu's default.
     *
     * -nographic disables the *host* window and leaves the guest a VGA
     * adapter, which is why a desktop worked here while the libvirt
     * backend -- which builds its devices from nothing -- gave the guest
     * no /dev/dri and no session at all. An accidental default that
     * happens to be right is what kept that hidden, so both backends now
     * say what they mean.
     */
    g_ptr_array_add(argv, g_strdup("-vga"));
    g_ptr_array_add(argv, g_strdup("none"));
    g_ptr_array_add(argv, g_strdup("-device"));
    g_ptr_array_add(argv, g_strdup("virtio-gpu-pci"));
    g_ptr_array_add(argv, g_strdup("-no-reboot"));

    if (mounts != NULL && mounts->len > 0) {
        /*
         * Same shared-memory requirement as the libvirt path, spelled the
         * way QEMU wants it.
         */
        g_ptr_array_add(argv, g_strdup("-object"));
        g_ptr_array_add(argv,
            g_strdup_printf("memory-backend-memfd,id=mem,size=%uM,share=on",
                            self->memory_mb));
        g_ptr_array_add(argv, g_strdup("-numa"));
        g_ptr_array_add(argv, g_strdup("node,memdev=mem"));
    }

    if (self->overlay != NULL || self->image != NULL) {
        g_ptr_array_add(argv, g_strdup("-drive"));
        g_ptr_array_add(argv,
            g_strdup_printf("file=%s,if=virtio,format=qcow2",
                            self->overlay != NULL ? self->overlay
                                                  : self->image));
    }

    if (self->seed_iso != NULL) {
        g_ptr_array_add(argv, g_strdup("-drive"));
        g_ptr_array_add(argv,
            g_strdup_printf("file=%s,media=cdrom,readonly=on",
                            self->seed_iso));
    }

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        g_ptr_array_add(argv, g_strdup("-chardev"));
        g_ptr_array_add(argv,
            g_strdup_printf("socket,id=virtiofs%u,path=%s-virtiofs%u.sock",
                            i, self->qmp_socket != NULL ? self->qmp_socket
                                                        : "/tmp/clawt",
                            i));
        g_ptr_array_add(argv, g_strdup("-device"));
        g_ptr_array_add(argv,
            g_strdup_printf("vhost-user-fs-pci,chardev=virtiofs%u,tag=%s",
                            i, clawt_mount_get_target(mount)));
    }

    /*
     * User-mode networking, with SSH forwarded from a port picked on the
     * host, so commands can be run without configuring a bridge the user
     * did not ask for.  The port is chosen before qemu starts rather than
     * left to qemu -- hostfwd with port 0 does let the kernel pick, but
     * nothing then reports which port it picked.
     */
    g_ptr_array_add(argv, g_strdup("-netdev"));

    if (self->forward_port > 0)
        g_ptr_array_add(argv,
            g_strdup_printf("user,id=net0,hostfwd=tcp:127.0.0.1:%u-:22",
                            self->forward_port));
    else
        g_ptr_array_add(argv, g_strdup("user,id=net0"));

    g_ptr_array_add(argv, g_strdup("-device"));
    g_ptr_array_add(argv, g_strdup("virtio-net-pci,netdev=net0"));

    if (qmp_socket != NULL) {
        g_ptr_array_add(argv, g_strdup("-qmp"));
        g_ptr_array_add(argv,
            g_strdup_printf("unix:%s,server=on,wait=off", qmp_socket));
    }

    g_ptr_array_add(argv, NULL);

    return (GStrv)g_ptr_array_free(argv, FALSE);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static gboolean
libvirt_call(ClawtVmComputer  *self,
             const gchar      *action,
             GHashTable       *extra,
             GError          **error)
{
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;

    /*
     * The connection is made when the module instance is *constructed*, so
     * the URI has to be given here rather than with the action.  Passed as
     * an ordinary parameter it is silently ignored, the module falls back
     * to qemu:///system -- which an unprivileged user cannot reach -- and
     * every single action fails with "not connected to libvirt", including
     * the define that would have created the domain.
     */
    if (!clawt_pod_bridge_load_module_for(self->bridge, "vm_virtmanager",
                                          self->uri, error))
        return FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("domain"), g_strdup(self->domain));

    if (extra != NULL) {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, extra);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_hash_table_insert(params, g_strdup(key), g_strdup(value));
    }

    result = clawt_pod_bridge_call_for(self->bridge, "vm_virtmanager",
                                       self->uri, action, params, error);

    return result != NULL;
}

/*
 * Where this VM's overlay, generated key, seed and remembered host keys
 * live.  One directory per agent, so removing an agent removes its guest.
 */
static gchar *
vm_state_dir(ClawtVmComputer *self)
{
    const gchar *agent_id = clawt_computer_get_agent_id(CLAWT_COMPUTER(self));

    return g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                            agent_id != NULL ? agent_id : "agent", NULL);
}

/*
 * The base image an overlay was built on, as qcow2 recorded it.
 *
 * Asked of qemu-img rather than remembered separately, because the
 * overlay is the thing that has to be right: a note beside it could say
 * one image while the file itself referenced another.
 *
 * The return value says whether the question could be *answered*, which
 * is not the same as the answer.  qemu-img refuses an image a running
 * guest holds -- `Failed to get shared "write" lock` -- and treating
 * that as "the backing file is something else" deleted the disk of a
 * running VM. Unknown is not different.
 */
static gboolean
overlay_backing_file(const gchar  *overlay,
                     gchar       **backing,
                     guint64      *virtual_size,
                     gchar       **why_not)
{
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(JsonParser) parser = NULL;
    g_autofree gchar *output = NULL;
    g_autofree gchar *failure = NULL;
    JsonObject *root;

    *backing = NULL;

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL,
                               "qemu-img", "info", "--output=json", overlay,
                               NULL);

    if (process == NULL) {
        if (why_not != NULL)
            *why_not = g_strdup("qemu-img could not be run");
        return FALSE;
    }

    if (!g_subprocess_communicate_utf8(process, NULL, NULL, &output,
                                       &failure, NULL) ||
        g_subprocess_get_exit_status(process) != 0) {
        if (why_not != NULL)
            *why_not = g_strdup(
                (failure != NULL && *g_strstrip(failure) != '\0')
                    ? failure : "qemu-img could not read it");
        return FALSE;
    }

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, output, -1, NULL)) {
        if (why_not != NULL)
            *why_not = g_strdup("qemu-img said something unparseable");
        return FALSE;
    }

    root = json_node_get_object(json_parser_get_root(parser));

    if (root == NULL) {
        if (why_not != NULL)
            *why_not = g_strdup("qemu-img said something unparseable");
        return FALSE;
    }

    /*
     * full-backing-filename is the resolved path; backing-filename is
     * whatever was written into the file, which may be relative.  A
     * plain image with neither is answered as NULL, which is a real
     * answer: it has no base.
     */
    if (json_object_has_member(root, "full-backing-filename"))
        *backing = g_strdup(
            json_object_get_string_member(root, "full-backing-filename"));
    else if (json_object_has_member(root, "backing-filename"))
        *backing = g_strdup(
            json_object_get_string_member(root, "backing-filename"));

    /*
     * The size the guest sees, which for an overlay is its own and not
     * the base's -- an overlay may be larger, and that is how a 5 GB
     * cloud image ends up as a disk worth installing anything on.
     */
    if (virtual_size != NULL && json_object_has_member(root, "virtual-size"))
        *virtual_size =
            (guint64)json_object_get_int_member(root, "virtual-size");

    return TRUE;
}

/*
 * What libvirt thinks the domain is doing, or %NULL if it does not know
 * of one.
 */
static gchar *
libvirt_domain_state(ClawtVmComputer *self)
{
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    const gchar *state;

    if (self->bridge == NULL ||
        !clawt_pod_bridge_load_module_for(self->bridge, "vm_virtmanager",
                                          self->uri, NULL))
        return NULL;

    /* Same reason: asking after a domain that is gone is loud. */
    if (!libvirt_has_domain(self))
        return NULL;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("domain"), g_strdup(self->domain));

    result = clawt_pod_bridge_call_for(self->bridge, "vm_virtmanager",
                                       self->uri, "get_info", params, NULL);

    if (result == NULL)
        return NULL;

    state = g_hash_table_lookup(result, "state");

    return state != NULL ? g_strdup(state) : NULL;
}

/*
 * The UUID libvirt already holds for this domain, if it holds one.
 *
 * libvirt refuses to redefine a name under a different UUID, and a
 * domain defined before clawtilla started supplying one has whatever
 * libvirt invented at the time. Adopting it is what lets an agent
 * created by an older build carry on working -- the alternative is
 * undefining somebody's domain, which is a great deal ruder.
 */
/*
 * Whether libvirt has a domain of this name.
 *
 * Asked with list_domains rather than by looking the name up, because
 * looking up one that is not there is *loud*: the module tries the name,
 * then tries the same string as a UUID, and libvirt logs both failures
 * to stderr. On a first provision -- the most ordinary path there is --
 * that put two alarming lines on the daemon's console:
 *
 *   Domain not found: no domain with matching name 'clawt-x'
 *   Invalid UUID
 *
 * Neither meant anything was wrong.
 */
static gboolean
libvirt_has_domain(ClawtVmComputer *self)
{
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    g_autoptr(JsonParser) parser = NULL;
    const gchar *domains;
    JsonArray *array;
    guint i;

    if (self->bridge == NULL ||
        !clawt_pod_bridge_load_module_for(self->bridge, "vm_virtmanager",
                                          self->uri, NULL))
        return FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("state"), g_strdup("all"));

    result = clawt_pod_bridge_call_for(self->bridge, "vm_virtmanager",
                                       self->uri, "list_domains", params,
                                       NULL);

    if (result == NULL)
        return FALSE;

    domains = g_hash_table_lookup(result, "domains");

    if (domains == NULL)
        return FALSE;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, domains, -1, NULL))
        return FALSE;

    if (json_node_get_node_type(json_parser_get_root(parser)) !=
        JSON_NODE_ARRAY)
        return FALSE;

    array = json_node_get_array(json_parser_get_root(parser));

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonObject *entry = json_array_get_object_element(array, i);

        if (entry != NULL && json_object_has_member(entry, "name") &&
            g_strcmp0(json_object_get_string_member(entry, "name"),
                      self->domain) == 0)
            return TRUE;
    }

    return FALSE;
}

static gchar *
libvirt_domain_uuid(ClawtVmComputer *self)
{
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(GHashTable) result = NULL;
    const gchar *xml;
    const gchar *open;
    const gchar *close;

    if (self->bridge == NULL ||
        !clawt_pod_bridge_load_module_for(self->bridge, "vm_virtmanager",
                                          self->uri, NULL))
        return NULL;

    /*
     * Only ask about a domain that is there. A VM the user removed --
     * or one that never existed -- is a normal thing to find, and is
     * answered by building it from scratch rather than by complaining.
     */
    if (!libvirt_has_domain(self))
        return NULL;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("domain"), g_strdup(self->domain));

    result = clawt_pod_bridge_call_for(self->bridge, "vm_virtmanager",
                                       self->uri, "get_xml", params, NULL);

    if (result == NULL)
        return NULL;

    xml = g_hash_table_lookup(result, "xml");

    if (xml == NULL)
        return NULL;

    open = strstr(xml, "<uuid>");

    if (open == NULL)
        return NULL;

    open += strlen("<uuid>");
    close = strstr(open, "</uuid>");

    if (close == NULL)
        return NULL;

    return g_strndup(open, (gsize)(close - open));
}

/*
 * The configured disk size in bytes.
 */
static guint64
disk_bytes(ClawtVmComputer *self)
{
    return (guint64)self->disk_gb * 1024 * 1024 * 1024;
}

/*
 * Brings an existing overlay up to the configured size.
 *
 * Growing a qcow2 is safe and instant -- it writes a larger size into the
 * header and nothing else -- and the guest's own filesystem follows on the
 * next boot, because cloud-init runs growpart every time rather than only
 * on the first.
 *
 * Shrinking is refused rather than done.  qemu-img will happily discard
 * everything past the new end, and a number being edited downwards in a
 * config file is not consent to lose whatever was living there.  The VM
 * keeps the disk it has and says why.
 */
static gboolean
resize_overlay(ClawtVmComputer *self, guint64 have, GError **error)
{
    g_autoptr(GSubprocess) process = NULL;
    g_autofree gchar *size = NULL;
    guint64 want = disk_bytes(self);

    /*
     * A size qemu-img did not report reads as zero, and guessing from it
     * would mean resizing on no information at all.
     */
    if (have == 0 || want == have)
        return TRUE;

    if (want < have) {
        g_warning("vm %s: computer.vm.disk_gb is %u GB but the disk is "
                  "already %.0f GB. It is left as it is -- shrinking a "
                  "disk throws away whatever was past the new end, which "
                  "is not something a config edit should do silently.",
                  self->domain, self->disk_gb,
                  (gdouble)have / (1024.0 * 1024.0 * 1024.0));
        return TRUE;
    }

    size = g_strdup_printf("%" G_GUINT64_FORMAT, want);

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE,
                               error,
                               "qemu-img", "resize", self->overlay, size,
                               NULL);

    if (process == NULL) {
        g_prefix_error(error, "growing the VM disk: ");
        return FALSE;
    }

    if (!g_subprocess_wait_check(process, NULL, error)) {
        g_prefix_error(error, "qemu-img could not grow %s to %u GB: ",
                       self->overlay, self->disk_gb);
        return FALSE;
    }

    g_message("vm %s: disk grown to %u GB; the guest's filesystem follows "
              "on its next boot", self->domain, self->disk_gb);

    return TRUE;
}

/*
 * Creates the writable overlay.
 *
 * The base image is never written to, so several agents can share one and a
 * botched session is thrown away by deleting one file rather than
 * reinstalling.
 */
static gboolean
ensure_overlay(ClawtVmComputer *self, GError **error)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *known_hosts = NULL;
    g_autofree gchar *superseded = NULL;
    g_autofree gchar *size = NULL;
    g_autoptr(GSubprocess) process = NULL;

    if (self->image == NULL)
        return TRUE;

    state_dir = vm_state_dir(self);

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    g_free(self->overlay);
    self->overlay = g_build_filename(state_dir, "overlay.qcow2", NULL);
    known_hosts = g_build_filename(state_dir, "known_hosts", NULL);

    /*
     * A base that is not there cannot back anything, and the error qemu
     * gives later names a backing file rather than the setting that
     * chose it.
     */
    if (!g_file_test(self->image, G_FILE_TEST_EXISTS)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                    "the disk image %s is not there. Fetch it with "
                    "`clawtilla image vm get`, or point "
                    "computer.vm.image at one that exists.", self->image);
        return FALSE;
    }

    if (g_file_test(self->overlay, G_FILE_TEST_EXISTS)) {
        g_autofree gchar *backing = NULL;
        g_autofree gchar *why_not = NULL;
        guint64 have = 0;

        /*
         * Cannot tell?  Then leave it alone.
         *
         * The commonest reason by far is that a guest is running and
         * holding the image, and qemu-img refuses to open it. Reading
         * that as "the base changed" deleted the disk out from under a
         * running VM -- which kept going on the unlinked inode while the
         * file on disk was replaced by an empty one. Unknown is not
         * different, and nothing here is worth destroying a guest over.
         */
        if (!overlay_backing_file(self->overlay, &backing, &have, &why_not)) {
            g_message("vm %s: leaving the existing disk alone; its base "
                      "could not be read (%s)",
                      self->domain,
                      why_not != NULL ? why_not : "no reason given");
            return TRUE;
        }

        if (g_strcmp0(backing, self->image) == 0)
            return resize_overlay(self, have, error);

        /*
         * Genuinely a different base.  Keeping the overlay would leave
         * the VM booting the old one for ever while the config said
         * otherwise -- a setting that silently does nothing.
         *
         * The old disk is moved aside rather than deleted. Everything
         * installed inside that VM is in it, and a config line changing
         * is a poor reason for that to be unrecoverable. An agent's own
         * work lives in its workspace on the host, which this never
         * touches.
         */
        superseded = g_build_filename(state_dir, "overlay-superseded.qcow2",
                                      NULL);
        g_unlink(superseded);

        if (g_rename(self->overlay, superseded) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                        "the disk image changed, but the old guest could "
                        "not be moved aside: %s", g_strerror(errno));
            return FALSE;
        }

        g_warning("vm %s: the disk image changed from %s to %s. The old "
                  "guest is at %s -- delete it when you are sure you do "
                  "not want it. Its workspace on the host is untouched.",
                  self->domain,
                  backing != NULL ? backing : "an image with no base",
                  self->image, superseded);

        g_unlink(known_hosts);
    }

    /*
     * The size is given explicitly rather than inherited from the base.
     *
     * A cloud image's own disk is a few gigabytes, which is enough to
     * boot and nothing else: a desktop, a toolchain and a couple of
     * container images fill it and the guest starts failing in ways that
     * look nothing like a full disk. qcow2 only occupies what is
     * written, so asking for more costs nothing until it is used, and
     * cloud-init's growpart extends the guest's filesystem to match on
     * first boot.
     */
    size = g_strdup_printf("%" G_GUINT64_FORMAT, disk_bytes(self));

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE,
                               error,
                               "qemu-img", "create", "-f", "qcow2",
                               "-F", "qcow2", "-b", self->image,
                               self->overlay, size, NULL);

    if (process == NULL) {
        g_prefix_error(error, "creating the VM overlay: ");
        return FALSE;
    }

    if (!g_subprocess_wait_check(process, NULL, error)) {
        g_prefix_error(error, "qemu-img could not create %s: ",
                       self->overlay);
        return FALSE;
    }

    /*
     * A new overlay is a new guest with a new host key.  Keeping the old
     * one would make every later connection look like an attack and be
     * refused, which reads as "SSH broke" rather than "the VM is new".
     */
    g_unlink(known_hosts);

    return TRUE;
}

/*
 * Picks the host port that reaches the guest's SSH, once, and remembers it.
 *
 * It has to survive a restart: a libvirt domain is defined with the port
 * written into its XML, so choosing a fresh one next time would leave the
 * daemon dialling a port nothing is listening on.
 */
static gboolean
ensure_ssh_forward(ClawtVmComputer *self, GError **error)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *port_path = NULL;
    g_autofree gchar *recorded = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GSocket) probe = NULL;
    g_autoptr(GInetAddress) loopback = NULL;
    g_autoptr(GSocketAddress) wanted = NULL;
    g_autoptr(GSocketAddress) bound = NULL;
    guint64 parsed;
    guint port;

    state_dir = vm_state_dir(self);

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    port_path = g_build_filename(state_dir, "ssh-port", NULL);

    if (g_file_get_contents(port_path, &recorded, NULL, NULL) &&
        g_ascii_string_to_unsigned(g_strstrip(recorded), 10, 1, 65535,
                                   &parsed, NULL)) {
        self->forward_port = (guint)parsed;
        self->ssh_port = (guint)parsed;
        return TRUE;
    }

    /*
     * Bind port 0, read back what the kernel handed out, then let go.
     * qemu's own hostfwd accepts port 0 and will do the same thing, but
     * nothing then reports which port it chose -- which is precisely how
     * this went unreachable in the first place.
     *
     * Something else can take the port in the window before the VM binds
     * it.  Losing that race is loud: the VM refuses to start.
     */
    probe = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_TCP, error);

    if (probe == NULL)
        return FALSE;

    loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    wanted = g_inet_socket_address_new(loopback, 0);

    if (!g_socket_bind(probe, wanted, TRUE, error))
        return FALSE;

    bound = g_socket_get_local_address(probe, error);

    if (bound == NULL)
        return FALSE;

    port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_socket_close(probe, NULL);

    text = g_strdup_printf("%u\n", port);

    if (!clawt_write_file_atomic(port_path, text, -1, 0600, FALSE, error))
        return FALSE;

    self->forward_port = port;
    self->ssh_port = port;

    return TRUE;
}

/*
 * Arranges for there to be an address that reaches the guest.
 */
static gboolean
ensure_ssh_route(ClawtVmComputer *self, GError **error)
{
    /*
     * libvirt can only forward a port through passt: <portForward> is not
     * supported for the SLIRP backend.  Without passt the VM is still
     * worth having -- it boots, it has its mounts, it can be snapshotted
     * -- so this is an unreachable guest and a warning saying so, not a
     * refusal to provision.
     */
    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autofree gchar *passt = g_find_program_in_path("passt");

        if (passt == NULL) {
            g_warning("vm %s: passt is not installed, so no port can be "
                      "forwarded to the guest and commands will not run in "
                      "it. Install passt (Fedora: passt), or set "
                      "computer.vm.ssh_host to an address that already "
                      "reaches this VM.", self->domain);
            return TRUE;
        }
    }

    if (!ensure_ssh_forward(self, error))
        return FALSE;

    g_free(self->ssh_host);
    self->ssh_host = g_strdup("127.0.0.1");

    return TRUE;
}

/*
 * Gives the guest a login.
 *
 * A distribution's cloud image ships with no account, no password and no
 * authorized key: it expects a datasource to hand it those on first boot.
 * Skipping this leaves a VM that boots perfectly and cannot be entered,
 * which looks exactly like a VM that failed to boot.
 */
/*
 * Works out which key reaches the guest, without touching the guest.
 *
 * Separate from the seed because the two have very different
 * preconditions.  Building a seed is only meaningful before a guest has
 * booted; knowing where its key file is matters every time a command is
 * run, including against a VM that has been up for days.
 *
 * They were one function, and provisioning skips seed-building entirely
 * for a running domain -- correctly, since rebuilding a running guest's
 * disk destroys it. But that skipped this too, so a daemon restarted
 * against a running VM held no key path at all, built an ssh command with
 * no -i, and every exec came back "Permission denied (publickey)" for a
 * key sitting in the state directory that worked perfectly by hand.
 */
static gboolean
ensure_ssh_key(ClawtVmComputer *self, GError **error)
{
    g_autofree gchar *state_dir = NULL;

    if (self->ssh_pubkey != NULL)
        return TRUE;

    state_dir = vm_state_dir(self);

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    if (self->ssh_key != NULL) {
        /*
         * A key named in the config belongs to the user.  It is read, and
         * never generated over.
         */
        g_free(self->ssh_pubkey);
        self->ssh_pubkey = clawt_cloud_init_public_key(self->ssh_key, error);

        return self->ssh_pubkey != NULL;
    }

    {
        const gchar *agent_id =
            clawt_computer_get_agent_id(CLAWT_COMPUTER(self));
        g_autofree gchar *comment = NULL;
        gchar *key_path = NULL;
        gchar *public_key = NULL;

        comment = g_strdup_printf("clawtilla-%s",
                                  agent_id != NULL ? agent_id : "agent");

        if (!clawt_cloud_init_ensure_key(state_dir, comment, &key_path,
                                         &public_key, error))
            return FALSE;

        g_free(self->ssh_key);
        self->ssh_key = key_path;
        g_free(self->ssh_pubkey);
        self->ssh_pubkey = public_key;
    }

    return TRUE;
}

static gboolean
ensure_cloud_init(ClawtVmComputer *self, GError **error)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *seed = NULL;

    if (!self->cloud_init)
        return TRUE;

    state_dir = vm_state_dir(self);

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    if (!ensure_ssh_key(self, error))
        return FALSE;

    seed = clawt_cloud_init_write_seed(state_dir, self->domain,
                                       self->ssh_user != NULL
                                           ? self->ssh_user : "root",
                                       self->ssh_pubkey, self->domain,
                                       self->desktop, error);

    if (seed == NULL)
        return FALSE;

    clawt_vm_computer_set_seed_iso(self, seed);

    return TRUE;
}

/*
 * Waits for evidence that qemu is actually running.
 *
 * g_subprocess_newv() reports only that the exec happened, and qemu
 * rejects its own command line milliseconds later with the reason on a
 * stderr nobody reads.  Marking the computer RUNNING there is how a VM
 * that never started reported itself as up: the first symptom was SSH
 * refusing a connection to a port qemu had never bound.
 *
 * The QMP socket is the evidence, because qemu creates it as it starts
 * and it is there whether or not a port is being forwarded.
 */
static gboolean
qemu_came_up(ClawtVmComputer *self, GError **error)
{
    g_autofree gchar *failure = NULL;
    guint attempt;

    for (attempt = 0; attempt < 20; attempt++) {
        if (g_file_test(self->qmp_socket, G_FILE_TEST_EXISTS))
            return TRUE;

        g_usleep(150 * 1000);
    }

    /*
     * Take qemu's own words for the error.  Anything this code invented
     * would be a guess at what qemu already said plainly.
     */
    g_subprocess_force_exit(self->qemu);
    g_subprocess_communicate_utf8(self->qemu, NULL, NULL, NULL, &failure,
                                  NULL);

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                "qemu did not start: %s",
                (failure != NULL && *g_strstrip(failure) != '\0')
                    ? failure : "it exited without saying why");

    g_clear_object(&self->qemu);

    return FALSE;
}

static gboolean
vm_provision(ClawtComputer *computer, GError **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    /*
     * Refused here rather than defined and left to fail.
     *
     * Without an image the domain XML carries no disk at all, so the VM
     * defines, starts, boots nothing, and shows a black console -- while
     * SSH to it answers "connection reset" because the port forward
     * reaches a guest with no sshd. Three symptoms, one missing setting,
     * and nothing anywhere connecting them.
     */
    if (self->image == NULL) {
        g_set_error_literal(error, CLAWT_ERROR,
                            CLAWT_ERROR_COMPUTER_PROVISION,
                            "this VM has no disk image, so there would be "
                            "nothing for it to boot. Set computer.vm.image "
                            "to a qcow2 -- `clawtilla image vm get "
                            "fedora-44` fetches one, or pick one under "
                            "Settings in the GTK client.");
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (*error)->message);
        return FALSE;
    }

    /*
     * A running guest's disk is not ours to touch.
     *
     * Restarting the daemon re-provisions every agent, and a libvirt
     * domain outlives the daemon -- so this runs routinely against a VM
     * that is up. Rebuilding its overlay or its seed underneath it is at
     * best pointless and at worst destroys the guest, which is exactly
     * what happened. The address still gets worked out, because that is
     * read from a file and is how commands reach the thing.
     */
    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autofree gchar *running = libvirt_domain_state(self);

        if (g_strcmp0(running, "running") == 0) {
            if (self->ssh_host == NULL && !ensure_ssh_route(self, error)) {
                clawt_computer_set_state(computer,
                                         CLAWT_COMPUTER_STATE_ERROR,
                                         (error != NULL && *error != NULL)
                                         ? (*error)->message : NULL);
                return FALSE;
            }

            /*
             * The key as well as the address.  Both are read from files
             * and neither touches the guest, and without the key the ssh
             * command is built with no -i at all -- so every exec against
             * a VM that outlived its daemon came back "Permission denied
             * (publickey)" for a key sitting in the state directory that
             * worked by hand.
             */
            if (!ensure_ssh_key(self, error)) {
                clawt_computer_set_state(computer,
                                         CLAWT_COMPUTER_STATE_ERROR,
                                         (error != NULL && *error != NULL)
                                         ? (*error)->message : NULL);
                return FALSE;
            }

            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING,
                                     NULL);
            return TRUE;
        }
    }

    if (!ensure_overlay(self, error) ||
        !ensure_cloud_init(self, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    /*
     * An address in the config means the user has their own route to the
     * guest -- a bridge, a real network, a VM that already existed -- and
     * clawtilla has no business forwarding a port alongside it.
     */
    if (self->ssh_host == NULL && !ensure_ssh_route(self, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autoptr(GHashTable) extra = NULL;
        g_autofree gchar *xml = NULL;

        if (self->bridge == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_COMPUTER_PROVISION,
                                "the libvirt backend needs podomation's "
                                "vm_virtmanager module");
            return FALSE;
        }

        /*
         * Asked before the XML is built, because the answer goes into it.
         */
        if (self->uuid == NULL)
            self->uuid = libvirt_domain_uuid(self);

        xml = clawt_vm_computer_build_domain_xml(self);

        extra = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(extra, g_strdup("xml"), g_strdup(xml));

        if (!libvirt_call(self, "define_xml", extra, error)) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
            return FALSE;
        }
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

static gboolean
vm_start(ClawtComputer *computer, GError **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);

    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_RUNNING)
        return TRUE;

    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_ABSENT &&
        !clawt_computer_provision(computer, error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STARTING, NULL);

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autofree gchar *state = libvirt_domain_state(self);

        /*
         * A libvirt domain outlives the daemon, so restarting clawtilla
         * finds one already running and asking it to start again is an
         * error -- "Domain is already active" -- which failed the whole
         * agent. The VM being up is the thing we wanted; say so and carry
         * on.
         *
         * A config change made while it runs reaches the domain
         * definition, which libvirt applies at its next boot, the same
         * way cpus and memory always have.
         */
        if (g_strcmp0(state, "running") == 0) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING,
                                     NULL);
            return TRUE;
        }

        if (!libvirt_call(self, "start", NULL, error)) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
            return FALSE;
        }

        if (self->snapshot_on_start) {
            g_autoptr(GHashTable) extra = NULL;
            g_autoptr(GError) snapshot_error = NULL;

            extra = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
            g_hash_table_insert(extra, g_strdup("name"),
                                clawt_generate_id("session"));

            /*
             * A snapshot that fails is not a reason to refuse to start.
             * The VM is up and useful; the user loses the ability to roll
             * back, and is told so.
             */
            if (!libvirt_call(self, "create_snapshot", extra,
                              &snapshot_error))
                g_warning("vm %s: could not take a start snapshot: %s",
                          self->domain, snapshot_error->message);
        }
    } else {
        g_auto(GStrv) argv = NULL;
        g_autofree gchar *socket_path = NULL;

        /*
         * Two qemus writing one qcow2 corrupts it, so an existing child
         * is the answer rather than a reason to spawn another.
         */
        if (self->qemu != NULL) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING,
                                     NULL);
            return TRUE;
        }

        socket_path = g_build_filename(g_get_user_runtime_dir(), "clawtilla",
                                       "vm", NULL);
        if (!clawt_ensure_dir(socket_path, 0700, error))
            return FALSE;

        g_free(self->qmp_socket);
        self->qmp_socket = g_build_filename(socket_path,
                                            self->domain, NULL);

        /*
         * sockaddr_un.sun_path is 108 bytes and qemu refuses a longer one
         * outright -- so the whole VM fails to start, and the message
         * names a socket nobody asked for rather than the VM.
         */
        if (!clawt_check_socket_path(self->qmp_socket, error)) {
            g_prefix_error(error, "the VM's QMP socket: ");
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
            return FALSE;
        }

        argv = clawt_vm_computer_build_qemu_argv(self, self->qmp_socket);

        self->qemu = g_subprocess_newv((const gchar * const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                       G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                       error);
        if (self->qemu == NULL || !qemu_came_up(self, error)) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
            return FALSE;
        }
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    return TRUE;
}

static gboolean
vm_stop(ClawtComputer *computer, GError **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);
    guint waited;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPING, NULL);

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autofree gchar *state = libvirt_domain_state(self);

        /* Already down, or never defined: nothing to ask of libvirt. */
        if (state == NULL || g_strcmp0(state, "running") != 0) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED,
                                     NULL);
            return TRUE;
        }

        /*
         * shutdown rather than destroy: the guest gets to flush its
         * filesystems.  A VM pulled out from under a running write is how a
         * disk image becomes unbootable.
         */
        if (!libvirt_call(self, "shutdown", NULL, error)) {
            clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                     NULL);
            return FALSE;
        }

        /*
         * Shutdown is a request, and only a guest that is listening
         * answers it.  One that has hung, or never booted -- a VM with
         * no disk is exactly that -- ignores it for ever, and the agent
         * could then not be stopped through clawtilla at all: the only
         * way out was virsh destroy by hand.
         *
         * So: wait, and pull the plug if it does not go. Pulling the
         * plug is what destroy is for, and a guest that never wrote
         * anything has nothing to lose by it.
         */
        for (waited = 0; waited < SHUTDOWN_GRACE_SECONDS; waited++) {
            g_autofree gchar *state_now = NULL;

            g_usleep(G_USEC_PER_SEC);
            state_now = libvirt_domain_state(self);

            if (g_strcmp0(state_now, "running") != 0)
                break;
        }

        {
            g_autofree gchar *final_state = libvirt_domain_state(self);

            if (g_strcmp0(final_state, "running") == 0) {
                g_autoptr(GError) destroy_error = NULL;

                g_warning("vm %s: it ignored a shutdown for %d seconds, so "
                          "it is being powered off. A guest that never "
                          "booted -- one with no disk image, say -- can "
                          "never answer one.",
                          self->domain, SHUTDOWN_GRACE_SECONDS);

                if (!libvirt_call(self, "destroy", NULL, &destroy_error)) {
                    g_propagate_error(error,
                                      g_steal_pointer(&destroy_error));
                    clawt_computer_set_state(computer,
                                             CLAWT_COMPUTER_STATE_ERROR,
                                             NULL);
                    return FALSE;
                }
            }
        }
    } else if (self->qemu != NULL) {
        g_subprocess_send_signal(self->qemu, SIGTERM);
        g_clear_object(&self->qemu);
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

/*
 * Builds the ssh command line that runs @command_argv in the guest.
 *
 * Separated out because it is the part that was wrong: nothing ever set an
 * address, every exec failed, and there was no pure function to notice it
 * in.
 */
GStrv
clawt_vm_computer_build_ssh_argv(ClawtVmComputer     *self,
                                 const gchar * const *command_argv,
                                 const gchar         *working_dir,
                                 guint                timeout_seconds)
{
    g_autoptr(GString) command = NULL;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);
    g_return_val_if_fail(command_argv != NULL, NULL);

    if (self->ssh_host == NULL)
        return NULL;

    command = g_string_new(NULL);

    if (working_dir != NULL) {
        g_autofree gchar *quoted = g_shell_quote(working_dir);

        g_string_append_printf(command, "cd %s && ", quoted);
    }

    for (i = 0; command_argv[i] != NULL; i++) {
        g_autofree gchar *quoted = g_shell_quote(command_argv[i]);

        if (i > 0)
            g_string_append_c(command, ' ');
        g_string_append(command, quoted);
    }

    return build_ssh_argv_as(self, self->ssh_user, command->str,
                             timeout_seconds, FALSE);
}

/*
 * The argv that reaches the guest's MCP server.
 *
 * A separate login from the one commands run as, and it has to be: the
 * server talks to GNOME Shell over the session bus of whoever is logged
 * in at the screen, and root -- the default ssh_user -- is by definition
 * not that account, because GDM will not log root in.
 *
 * The remote command is one bare word.  Everything that has to be worked
 * out inside the guest is worked out by the launcher installed there,
 * rather than being threaded through argv quoting here, ssh's own
 * re-parsing and a remote shell.
 */
GStrv
clawt_vm_computer_build_desktop_argv(ClawtVmComputer *self)
{
    const gchar *session_user;

    g_return_val_if_fail(CLAWT_IS_VM_COMPUTER(self), NULL);

    if (self->desktop == NULL || self->ssh_host == NULL)
        return NULL;

    session_user = clawt_guest_desktop_get_session_user(self->desktop);

    /*
     * No timeout at all on the session itself.  This is a stdio pipe an
     * MCP client holds open for as long as the agent runs, so a
     * ConnectTimeout is the only bound that makes sense -- and that one
     * is set inside, because a VM that is not up should fail rather than
     * leave the client waiting on a pipe nothing will ever write to.
     */
    return build_ssh_argv_as(self, session_user,
                             CLAWT_GUEST_DESKTOP_LAUNCHER, 0, TRUE);
}

/*
 * Assembles the ssh command line.
 *
 * Shared by the two callers because everything except the login, the
 * remote command and whether the pipe is long-lived is the same, and two
 * copies of the option list would drift.
 */
static GStrv
build_ssh_argv_as(ClawtVmComputer *self,
                  const gchar     *login,
                  const gchar     *command,
                  guint            timeout_seconds,
                  gboolean         interactive_stream)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *known_hosts = NULL;
    GPtrArray *ssh_argv;

    state_dir = vm_state_dir(self);
    known_hosts = g_build_filename(state_dir, "known_hosts", NULL);

    ssh_argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ssh_argv, g_strdup("ssh"));
    g_ptr_array_add(ssh_argv, g_strdup("-o"));
    g_ptr_array_add(ssh_argv, g_strdup("BatchMode=yes"));
    g_ptr_array_add(ssh_argv, g_strdup("-o"));
    g_ptr_array_add(ssh_argv, g_strdup("StrictHostKeyChecking=accept-new"));

    /*
     * The agent's guests get a known_hosts of their own.  A VM rebuilt
     * from the same base image answers on the same address with a
     * different host key, and the user's own file would report that as an
     * attack and refuse to connect -- besides which, clawtilla has no
     * business writing into it.
     */
    g_ptr_array_add(ssh_argv, g_strdup("-o"));
    g_ptr_array_add(ssh_argv,
                    g_strdup_printf("UserKnownHostsFile=%s", known_hosts));

    if (self->ssh_port > 0) {
        g_ptr_array_add(ssh_argv, g_strdup("-p"));
        g_ptr_array_add(ssh_argv, g_strdup_printf("%u", self->ssh_port));
    }

    if (timeout_seconds > 0) {
        g_ptr_array_add(ssh_argv, g_strdup("-o"));
        g_ptr_array_add(ssh_argv,
                        g_strdup_printf("ConnectTimeout=%u",
                                        MIN(timeout_seconds, 30)));
    }

    if (interactive_stream) {
        /*
         * -T because this carries JSON-RPC frames rather than a session
         * for a person.  A pty would translate line endings and act on
         * control characters in the middle of a message, and the damage
         * would surface as a protocol error a long way from here.
         */
        g_ptr_array_add(ssh_argv, g_strdup("-T"));
        g_ptr_array_add(ssh_argv, g_strdup("-o"));
        g_ptr_array_add(ssh_argv, g_strdup("ConnectTimeout=10"));

        /*
         * A VM that goes away leaves the pipe open and silent otherwise,
         * and the client waits on it for ever.
         */
        g_ptr_array_add(ssh_argv, g_strdup("-o"));
        g_ptr_array_add(ssh_argv, g_strdup("ServerAliveInterval=30"));
        g_ptr_array_add(ssh_argv, g_strdup("-o"));
        g_ptr_array_add(ssh_argv, g_strdup("ServerAliveCountMax=3"));
    }

    if (self->ssh_key != NULL) {
        g_ptr_array_add(ssh_argv, g_strdup("-i"));
        g_ptr_array_add(ssh_argv, g_strdup(self->ssh_key));
    }

    g_ptr_array_add(ssh_argv,
                    g_strdup_printf("%s@%s",
                                    login != NULL ? login : "root",
                                    self->ssh_host));
    g_ptr_array_add(ssh_argv, g_strdup(command));
    g_ptr_array_add(ssh_argv, NULL);

    return (GStrv)g_ptr_array_free(ssh_argv, FALSE);
}

/*
 * Removes the guest and everything clawtilla made for it.
 *
 * Never called automatically -- an agent whose daemon restarted keeps its
 * VM. It runs when somebody asks for the computer to go, and then it has
 * to actually go: this vfunc did not exist at all, and because the base
 * class answered TRUE for a missing one, removing a VM agent reported the
 * computer as destroyed while the libvirt domain stayed defined and the
 * disk stayed on disk. The person then found it in virt-manager and had
 * to undefine it by hand.
 *
 * Everything here is best-effort and additive: a domain somebody already
 * removed, or a disk already deleted, is not a reason to refuse. What is
 * a reason to refuse is being unable to remove something that is still
 * there, because that is the case the caller has to hear about.
 */
static gboolean
vm_teardown(ClawtComputer *computer, GError **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);
    g_autofree gchar *state_dir = NULL;
    g_autoptr(GError) local = NULL;
    static const gchar *const leftovers[] = {
        "overlay.qcow2", "overlay-superseded.qcow2", "seed.iso",
        "known_hosts", "ssh-port", "id_ed25519", "id_ed25519.pub", NULL
    };
    gsize i;

    /*
     * Stopped first, and by whichever route each backend has.  libvirt
     * refuses to undefine a running domain -- podomation's undefine
     * refuses it too rather than leaving a transient domain behind -- and
     * qemu has to be told to go before its files can be removed.
     */
    if (!clawt_computer_stop(computer, &local))
        g_message("vm %s: could not be stopped cleanly before removal (%s); "
                  "carrying on", self->domain, local->message);

    g_clear_error(&local);

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        /*
         * Asked only when there is something to ask about. libvirt logs
         * two alarming lines for a domain that is not there -- it tries
         * the name, then the same string as a UUID -- and this is the
         * ordinary path for an agent that was never started.
         */
        if (libvirt_has_domain(self)) {
            if (!libvirt_call(self, "undefine", NULL, error)) {
                g_prefix_error(error, "the libvirt domain %s could not be "
                                      "removed: ", self->domain);
                return FALSE;
            }
        }
    } else if (self->qemu != NULL) {
        g_subprocess_force_exit(self->qemu);
        g_clear_object(&self->qemu);
    }

    if (self->qmp_socket != NULL)
        g_unlink(self->qmp_socket);

    /*
     * The disk goes with the domain.  It is the guest: keeping it would
     * leave an agent's whole machine behind under a name nothing refers
     * to any more, and a later agent with the same id would silently
     * adopt it instead of starting clean -- which is exactly the
     * confusion this is being called to end.
     *
     * The agent's own *work* is in its workspace on the host, which this
     * never touches.
     */
    state_dir = vm_state_dir(self);

    for (i = 0; leftovers[i] != NULL; i++) {
        g_autofree gchar *path = g_build_filename(state_dir, leftovers[i],
                                                  NULL);

        if (g_unlink(path) != 0 && errno != ENOENT) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
                        "the VM is gone but %s could not be removed: %s",
                        path, g_strerror(errno));
            return FALSE;
        }
    }

    {
        g_autofree gchar *seed_dir = g_build_filename(state_dir,
                                                      "cloud-init", NULL);
        g_autofree gchar *user_data = g_build_filename(seed_dir, "user-data",
                                                       NULL);
        g_autofree gchar *meta_data = g_build_filename(seed_dir, "meta-data",
                                                       NULL);

        g_unlink(user_data);
        g_unlink(meta_data);
        g_rmdir(seed_dir);
    }

    /*
     * Left if anything else is in it.  g_rmdir only removes an empty
     * directory, so something put there by hand survives rather than
     * being swept up with the rest.
     */
    g_rmdir(state_dir);

    g_clear_pointer(&self->overlay, g_free);
    g_clear_pointer(&self->seed_iso, g_free);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ABSENT, NULL);

    return TRUE;
}

/*
 * There is no `podman exec` for a VM, so commands go over SSH.  A guest
 * agent would be an extra thing to install inside every image, and SSH is
 * already there in anything that boots.
 */
static ClawtExecResult *
vm_exec(ClawtComputer        *computer,
        const gchar * const  *argv,
        const gchar          *working_dir,
        guint                 timeout_seconds,
        GCancellable         *cancellable,
        GError              **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);
    g_autoptr(GSubprocess) process = NULL;
    g_autofree gchar *stdout_text = NULL;
    g_autofree gchar *stderr_text = NULL;
    g_autofree gchar *bounded = NULL;
    g_auto(GStrv) ssh_argv = NULL;
    ClawtExecResult *result;
    gboolean truncated = FALSE;

    ssh_argv = clawt_vm_computer_build_ssh_argv(self, argv, working_dir,
                                                timeout_seconds);

    if (ssh_argv == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_EXEC,
                            "nothing forwards a port to this VM's SSH, so "
                            "commands cannot be run in it. Install passt "
                            "(Fedora: passt) and provision it again, or set "
                            "computer.vm.ssh_host to an address that "
                            "already reaches the guest.");
        return NULL;
    }

    process = g_subprocess_newv((const gchar * const *)ssh_argv,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                error);

    if (process == NULL)
        return NULL;

    if (!g_subprocess_communicate_utf8(process, NULL, cancellable,
                                       &stdout_text, &stderr_text, error))
        return NULL;

    bounded = clawt_computer_truncate_output(stdout_text, MAX_OUTPUT_BYTES,
                                             &truncated);

    result = clawt_exec_result_new(g_subprocess_get_exit_status(process),
                                   bounded, stderr_text);
    clawt_exec_result_set_truncated(result, truncated);

    return result;
}

static gchar *
vm_describe(ClawtComputer *computer)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);
    GPtrArray *mounts = clawt_computer_get_mounts(computer);
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    g_string_append_printf(out,
        "You have a virtual machine of your own (%s, %u CPU%s, %u MB RAM, "
        "%u GB disk). It is fully isolated from the host.",
        self->backend == CLAWT_VM_BACKEND_LIBVIRT ? "libvirt" : "QEMU",
        self->cpus, self->cpus == 1 ? "" : "s", self->memory_mb,
        self->disk_gb);

    /*
     * Saying which login the commands arrive as saves the agent guessing
     * at sudo, and saying when there is no route at all saves it a turn
     * spent probing a computer it cannot reach.
     */
    if (self->ssh_host != NULL)
        g_string_append_printf(out, " Commands you run there arrive over "
                                    "SSH as %s.",
                               self->ssh_user != NULL ? self->ssh_user
                                                      : "root");
    else
        g_string_append(out, " Nothing currently forwards a port to it, so "
                             "commands cannot be run in it -- say so rather "
                             "than retrying.");

    if (mounts != NULL && mounts->len > 0) {
        g_string_append(out, " Shared from the host over virtiofs:");

        for (i = 0; i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            g_string_append_printf(out, "%s %s", i > 0 ? "," : "",
                                   clawt_mount_get_target(mount));
        }

        g_string_append_c(out, '.');
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static ClawtComputerType
vm_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_VM;
}

static void
clawt_vm_computer_dispose(GObject *object)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(object);

    if (self->qemu != NULL) {
        g_subprocess_send_signal(self->qemu, SIGTERM);
        g_clear_object(&self->qemu);
    }

    g_clear_object(&self->bridge);

    G_OBJECT_CLASS(clawt_vm_computer_parent_class)->dispose(object);
}

static void
clawt_vm_computer_finalize(GObject *object)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(object);

    g_clear_pointer(&self->desktop, clawt_guest_desktop_unref);
    g_clear_pointer(&self->domain, g_free);
    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->image, g_free);
    g_clear_pointer(&self->overlay, g_free);
    g_clear_pointer(&self->ssh_user, g_free);
    g_clear_pointer(&self->ssh_key, g_free);
    g_clear_pointer(&self->ssh_pubkey, g_free);
    g_clear_pointer(&self->ssh_host, g_free);
    g_clear_pointer(&self->seed_iso, g_free);
    g_clear_pointer(&self->uuid, g_free);
    g_clear_pointer(&self->qmp_socket, g_free);

    G_OBJECT_CLASS(clawt_vm_computer_parent_class)->finalize(object);
}

static void
clawt_vm_computer_class_init(ClawtVmComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_vm_computer_dispose;
    object_class->finalize = clawt_vm_computer_finalize;

    computer_class->provision = vm_provision;
    computer_class->start = vm_start;
    computer_class->stop = vm_stop;
    computer_class->teardown = vm_teardown;
    computer_class->exec = vm_exec;
    computer_class->describe = vm_describe;
    computer_class->get_computer_type = vm_get_computer_type;
}

static void
clawt_vm_computer_init(ClawtVmComputer *self)
{
    self->backend = CLAWT_VM_BACKEND_LIBVIRT;
    self->cpus = 2;
    self->memory_mb = 8192;
    self->disk_gb = 128;
    self->ssh_user = g_strdup("root");
    self->ssh_port = 22;
    self->cloud_init = TRUE;
    self->uri = g_strdup("qemu:///session");
}
