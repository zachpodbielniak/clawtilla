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

#define MAX_OUTPUT_BYTES (256 * 1024)

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
    gchar *ssh_host;

    guint    cpus;
    guint    memory_mb;
    gboolean snapshot_on_start;

    GSubprocess *qemu;
    gchar       *qmp_socket;
};

G_DEFINE_FINAL_TYPE(ClawtVmComputer, clawt_vm_computer, CLAWT_TYPE_COMPUTER)

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
                                guint            memory_mb)
{
    g_return_if_fail(CLAWT_IS_VM_COMPUTER(self));

    if (cpus > 0)
        self->cpus = cpus;
    if (memory_mb > 0)
        self->memory_mb = memory_mb;
}

void
clawt_vm_computer_set_ssh(ClawtVmComputer *self,
                          const gchar     *user,
                          const gchar     *key_path,
                          const gchar     *host)
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

    g_string_append(out,
        "    <interface type='user'>\n"
        "      <model type='virtio'/>\n"
        "    </interface>\n"
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

    /* User-mode networking with SSH forwarded, so commands can be run
     * without configuring a bridge the user did not ask for. */
    g_ptr_array_add(argv, g_strdup("-netdev"));
    g_ptr_array_add(argv, g_strdup("user,id=net0,hostfwd=tcp::0-:22"));
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

    if (!clawt_pod_bridge_load_module(self->bridge, "vm_virtmanager", error))
        return FALSE;

    params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(params, g_strdup("domain"), g_strdup(self->domain));

    if (self->uri != NULL)
        g_hash_table_insert(params, g_strdup("uri"), g_strdup(self->uri));

    if (extra != NULL) {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, extra);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_hash_table_insert(params, g_strdup(key), g_strdup(value));
    }

    result = clawt_pod_bridge_call(self->bridge, "vm_virtmanager", action,
                                   params, error);

    return result != NULL;
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
    g_autoptr(GSubprocess) process = NULL;
    const gchar *agent_id;

    if (self->image == NULL)
        return TRUE;

    if (self->overlay != NULL &&
        g_file_test(self->overlay, G_FILE_TEST_EXISTS))
        return TRUE;

    agent_id = clawt_computer_get_agent_id(CLAWT_COMPUTER(self));
    state_dir = g_build_filename(g_get_user_data_dir(), "clawtilla", "vms",
                                 agent_id != NULL ? agent_id : "agent", NULL);

    if (!clawt_ensure_dir(state_dir, 0700, error))
        return FALSE;

    g_free(self->overlay);
    self->overlay = g_build_filename(state_dir, "overlay.qcow2", NULL);

    if (g_file_test(self->overlay, G_FILE_TEST_EXISTS))
        return TRUE;

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_PIPE,
                               error,
                               "qemu-img", "create", "-f", "qcow2",
                               "-F", "qcow2", "-b", self->image,
                               self->overlay, NULL);

    if (process == NULL) {
        g_prefix_error(error, "creating the VM overlay: ");
        return FALSE;
    }

    if (!g_subprocess_wait_check(process, NULL, error)) {
        g_prefix_error(error, "qemu-img could not create %s: ",
                       self->overlay);
        return FALSE;
    }

    return TRUE;
}

static gboolean
vm_provision(ClawtComputer *computer, GError **error)
{
    ClawtVmComputer *self = CLAWT_VM_COMPUTER(computer);

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_PROVISIONING,
                             NULL);

    if (!ensure_overlay(self, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
        g_autoptr(GHashTable) extra = NULL;
        g_autofree gchar *xml = clawt_vm_computer_build_domain_xml(self);

        if (self->bridge == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_COMPUTER_PROVISION,
                                "the libvirt backend needs podomation's "
                                "vm_virtmanager module");
            return FALSE;
        }

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

    if (clawt_computer_get_state(computer) == CLAWT_COMPUTER_STATE_ABSENT &&
        !clawt_computer_provision(computer, error))
        return FALSE;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STARTING, NULL);

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
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

        socket_path = g_build_filename(g_get_user_runtime_dir(), "clawtilla",
                                       "vm", NULL);
        if (!clawt_ensure_dir(socket_path, 0700, error))
            return FALSE;

        g_free(self->qmp_socket);
        self->qmp_socket = g_build_filename(socket_path,
                                            self->domain, NULL);

        argv = clawt_vm_computer_build_qemu_argv(self, self->qmp_socket);

        self->qemu = g_subprocess_newv((const gchar * const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                       G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                       error);
        if (self->qemu == NULL) {
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

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPING, NULL);

    if (self->backend == CLAWT_VM_BACKEND_LIBVIRT) {
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
    } else if (self->qemu != NULL) {
        g_subprocess_send_signal(self->qemu, SIGTERM);
        g_clear_object(&self->qemu);
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

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
    g_autoptr(GString) command = NULL;
    GPtrArray *ssh_argv;
    ClawtExecResult *result;
    gboolean truncated = FALSE;
    gsize i;

    if (self->ssh_host == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_EXEC,
                            "this VM has no SSH address, so commands cannot "
                            "be run in it yet");
        return NULL;
    }

    command = g_string_new(NULL);

    if (working_dir != NULL) {
        g_autofree gchar *quoted = g_shell_quote(working_dir);

        g_string_append_printf(command, "cd %s && ", quoted);
    }

    for (i = 0; argv[i] != NULL; i++) {
        g_autofree gchar *quoted = g_shell_quote(argv[i]);

        if (i > 0)
            g_string_append_c(command, ' ');
        g_string_append(command, quoted);
    }

    ssh_argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ssh_argv, g_strdup("ssh"));
    g_ptr_array_add(ssh_argv, g_strdup("-o"));
    g_ptr_array_add(ssh_argv, g_strdup("BatchMode=yes"));
    g_ptr_array_add(ssh_argv, g_strdup("-o"));
    g_ptr_array_add(ssh_argv, g_strdup("StrictHostKeyChecking=accept-new"));

    if (timeout_seconds > 0) {
        g_ptr_array_add(ssh_argv, g_strdup("-o"));
        g_ptr_array_add(ssh_argv,
                        g_strdup_printf("ConnectTimeout=%u",
                                        MIN(timeout_seconds, 30)));
    }

    if (self->ssh_key != NULL) {
        g_ptr_array_add(ssh_argv, g_strdup("-i"));
        g_ptr_array_add(ssh_argv, g_strdup(self->ssh_key));
    }

    g_ptr_array_add(ssh_argv,
                    g_strdup_printf("%s@%s",
                                    self->ssh_user != NULL ? self->ssh_user
                                                           : "root",
                                    self->ssh_host));
    g_ptr_array_add(ssh_argv, g_strdup(command->str));
    g_ptr_array_add(ssh_argv, NULL);

    process = g_subprocess_newv((const gchar * const *)ssh_argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                error);
    g_ptr_array_unref(ssh_argv);

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
        "You have a virtual machine of your own (%s, %u CPU%s, %u MB). "
        "It is fully isolated from the host.",
        self->backend == CLAWT_VM_BACKEND_LIBVIRT ? "libvirt" : "QEMU",
        self->cpus, self->cpus == 1 ? "" : "s", self->memory_mb);

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

    g_clear_pointer(&self->domain, g_free);
    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->image, g_free);
    g_clear_pointer(&self->overlay, g_free);
    g_clear_pointer(&self->ssh_user, g_free);
    g_clear_pointer(&self->ssh_key, g_free);
    g_clear_pointer(&self->ssh_host, g_free);
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
    computer_class->exec = vm_exec;
    computer_class->describe = vm_describe;
    computer_class->get_computer_type = vm_get_computer_type;
}

static void
clawt_vm_computer_init(ClawtVmComputer *self)
{
    self->backend = CLAWT_VM_BACKEND_LIBVIRT;
    self->cpus = 2;
    self->memory_mb = 2048;
    self->ssh_user = g_strdup("root");
    self->uri = g_strdup("qemu:///session");
}
