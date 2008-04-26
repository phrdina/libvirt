/*
 * xm_internal.h: helper routines for dealing with inactive domains
 *
 * Copyright (C) 2006-2007 Red Hat
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 *
 */

#ifdef WITH_XEN
#include <config.h>

#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <stdint.h>
#include <xen/dom0_ops.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#ifndef NAME_MAX
#define	NAME_MAX	255
#endif

#include "xen_unified.h"
#include "xm_internal.h"
#include "xend_internal.h"
#include "conf.h"
#include "hash.h"
#include "internal.h"
#include "xml.h"
#include "buf.h"
#include "uuid.h"
#include "util.h"

static int xenXMConfigSetString(virConfPtr conf, const char *setting,
                                const char *str);

typedef struct xenXMConfCache *xenXMConfCachePtr;
typedef struct xenXMConfCache {
    time_t refreshedAt;
    char filename[PATH_MAX];
    virConfPtr conf;
} xenXMConfCache;

static char configDir[PATH_MAX];
/* Config file name to config object */
static virHashTablePtr configCache = NULL;
/* Name to config file name */
static virHashTablePtr nameConfigMap = NULL;
static int nconnections = 0;
static time_t lastRefresh = 0;

char * xenXMAutoAssignMac(void);
static int xenXMAttachDisk(virDomainPtr domain, xmlXPathContextPtr ctxt, int hvm,
                            xmlNodePtr node, xenXMConfCachePtr entry);
static int xenXMAttachInterface(virDomainPtr domain, xmlXPathContextPtr ctxt, int hvm,
                                xmlNodePtr node, xenXMConfCachePtr entry);
static int xenXMDomainAttachDevice(virDomainPtr domain, const char *xml);
static int xenXMDomainDetachDevice(virDomainPtr domain, const char *xml);

#define XM_REFRESH_INTERVAL 10

#define XM_CONFIG_DIR "/etc/xen"
#define XM_EXAMPLE_PREFIX "xmexample"
#define XEND_CONFIG_FILE "xend-config.sxp"
#define XEND_PCI_CONFIG_PREFIX "xend-pci-"
#define QEMU_IF_SCRIPT "qemu-ifup"
#define XM_XML_ERROR "Invalid xml"

struct xenUnifiedDriver xenXMDriver = {
    xenXMOpen, /* open */
    xenXMClose, /* close */
    NULL, /* version */
    NULL, /* hostname */
    NULL, /* URI */
    NULL, /* nodeGetInfo */
    NULL, /* getCapabilities */
    NULL, /* listDomains */
    NULL, /* numOfDomains */
    NULL, /* domainCreateLinux */
    NULL, /* domainSuspend */
    NULL, /* domainResume */
    NULL, /* domainShutdown */
    NULL, /* domainReboot */
    NULL, /* domainDestroy */
    NULL, /* domainGetOSType */
    xenXMDomainGetMaxMemory, /* domainGetMaxMemory */
    xenXMDomainSetMaxMemory, /* domainSetMaxMemory */
    xenXMDomainSetMemory, /* domainMaxMemory */
    xenXMDomainGetInfo, /* domainGetInfo */
    NULL, /* domainSave */
    NULL, /* domainRestore */
    NULL, /* domainCoreDump */
    xenXMDomainSetVcpus, /* domainSetVcpus */
    xenXMDomainPinVcpu, /* domainPinVcpu */
    NULL, /* domainGetVcpus */
    NULL, /* domainGetMaxVcpus */
    xenXMListDefinedDomains, /* listDefinedDomains */
    xenXMNumOfDefinedDomains, /* numOfDefinedDomains */
    xenXMDomainCreate, /* domainCreate */
    xenXMDomainDefineXML, /* domainDefineXML */
    xenXMDomainUndefine, /* domainUndefine */
    xenXMDomainAttachDevice, /* domainAttachDevice */
    xenXMDomainDetachDevice, /* domainDetachDevice */
    NULL, /* domainGetAutostart */
    NULL, /* domainSetAutostart */
    NULL, /* domainGetSchedulerType */
    NULL, /* domainGetSchedulerParameters */
    NULL, /* domainSetSchedulerParameters */
};

static void
xenXMError(virConnectPtr conn, virErrorNumber error, const char *info)
{
    const char *errmsg;

    if (error == VIR_ERR_OK)
        return;

    errmsg = __virErrorMsg(error, info);
    __virRaiseError(conn, NULL, NULL, VIR_FROM_XENXM, error, VIR_ERR_ERROR,
                    errmsg, info, NULL, 0, 0, errmsg, info);
}

int
xenXMInit (void)
{
    char *envConfigDir;
    int safeMode = 0;

    /* Disable use of env variable if running setuid */
    if ((geteuid() != getuid()) ||
        (getegid() != getgid()))
        safeMode = 1;

    if (!safeMode &&
        (envConfigDir = getenv("LIBVIRT_XM_CONFIG_DIR")) != NULL) {
        strncpy(configDir, envConfigDir, PATH_MAX-1);
        configDir[PATH_MAX-1] = '\0';
    } else {
        strcpy(configDir, XM_CONFIG_DIR);
    }

    return 0;
}


/* Convenience method to grab a int from the config file object */
static int xenXMConfigGetInt(virConfPtr conf, const char *name, long *value) {
    virConfValuePtr val;
    if (!value || !name || !conf)
        return (-1);

    if (!(val = virConfGetValue(conf, name))) {
        return (-1);
    }

    if (val->type == VIR_CONF_LONG) {
        *value = val->l;
    } else if (val->type == VIR_CONF_STRING) {
        char *ret;
        if (!val->str)
            return (-1);
        *value = strtol(val->str, &ret, 10);
        if (ret == val->str)
            return (-1);
    } else {
        return (-1);
    }
    return (0);
}


/* Convenience method to grab a string from the config file object */
static int xenXMConfigGetString(virConfPtr conf, const char *name, const char **value) {
    virConfValuePtr val;
    if (!value || !name || !conf)
        return (-1);
    *value = NULL;
    if (!(val = virConfGetValue(conf, name))) {
        return (-1);
    }
    if (val->type != VIR_CONF_STRING)
        return (-1);
    if (!val->str)
        return (-1);
    *value = val->str;
    return (0);
}

/* Convenience method to grab a string UUID from the config file object */
static int xenXMConfigGetUUID(virConfPtr conf, const char *name, unsigned char *uuid) {
    virConfValuePtr val;
    if (!uuid || !name || !conf)
        return (-1);
    if (!(val = virConfGetValue(conf, name))) {
        return (-1);
    }

    if (val->type != VIR_CONF_STRING)
        return (-1);
    if (!val->str)
        return (-1);

    if (virUUIDParse(val->str, uuid) < 0)
        return (-1);

    return (0);
}


/* Ensure that a config object has a valid UUID in it,
   if it doesn't then (re-)generate one */
static int xenXMConfigEnsureIdentity(virConfPtr conf, const char *filename) {
    unsigned char uuid[VIR_UUID_BUFLEN];
    const char *name;

    /* Had better have a name...*/
    if (xenXMConfigGetString(conf, "name", &name) < 0) {
        virConfValuePtr value;
        value = malloc(sizeof(*value));
        if (!value) {
            return (-1);
        }

        /* Set name based on filename */
        value->type = VIR_CONF_STRING;
        value->str = strdup(filename);
        if (!value->str) {
            free(value);
            return (-1);
        }
        if (virConfSetValue(conf, "name", value) < 0)
            return (-1);
    }

    /* If there is no uuid...*/
    if (xenXMConfigGetUUID(conf, "uuid", uuid) < 0) {
        virConfValuePtr value;
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        value = malloc(sizeof(*value));
        if (!value) {
            return (-1);
        }

        /* ... then generate one */
        virUUIDGenerate(uuid);
        virUUIDFormat(uuid, uuidstr);

        value->type = VIR_CONF_STRING;
        value->str = strdup(uuidstr);
        if (!value->str) {
            free(value);
            return (-1);
        }

        /* And stuff the UUID back into the config file */
        if (virConfSetValue(conf, "uuid", value) < 0)
            return (-1);
    }
    return (0);
}

/* Release memory associated with a cached config object */
static void xenXMConfigFree(void *payload, const char *key ATTRIBUTE_UNUSED) {
    xenXMConfCachePtr entry = (xenXMConfCachePtr)payload;
    virConfFree(entry->conf);
    free(entry);
}


/* Remove any configs which were not refreshed recently */
static int xenXMConfigReaper(const void *payload, const char *key ATTRIBUTE_UNUSED, const void *data) {
    time_t now = *(const time_t *)data;
    xenXMConfCachePtr entry = (xenXMConfCachePtr)payload;

    if (entry->refreshedAt != now) {
        const char *olddomname;
        /* We're going to pure this config file, so check if it
           is currently mapped as owner of a named domain. */
        if (xenXMConfigGetString(entry->conf, "name", &olddomname) != -1) {
            char *nameowner = (char *)virHashLookup(nameConfigMap, olddomname);
            if (nameowner && !strcmp(nameowner, key)) {
                virHashRemoveEntry(nameConfigMap, olddomname, NULL);
            }
        }
        return (1);
    }
    return (0);
}

/* This method is called by various methods to scan /etc/xen
   (or whatever directory was set by  LIBVIRT_XM_CONFIG_DIR
   environment variable) and process any domain configs. It
   has rate-limited so never rescans more frequently than
   once every X seconds */
static int xenXMConfigCacheRefresh (virConnectPtr conn) {
    DIR *dh;
    struct dirent *ent;
    time_t now = time(NULL);
    int ret = -1;

    if (now == ((time_t)-1)) {
        xenXMError (conn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
        return (-1);
    }

    /* Rate limit re-scans */
    if ((now - lastRefresh) < XM_REFRESH_INTERVAL)
        return (0);

    lastRefresh = now;

    /* Process the files in the config dir */
    if (!(dh = opendir(configDir))) {
        xenXMError (conn, VIR_ERR_SYSTEM_ERROR, strerror (errno));
        return (-1);
    }

    while ((ent = readdir(dh))) {
        xenXMConfCachePtr entry;
        struct stat st;
        int newborn = 0;
        char path[PATH_MAX];
        const char *domname = NULL;

        /*
         * Skip a bunch of crufty files that clearly aren't config files
         */

        /* Like 'dot' files... */
        if (!strncmp(ent->d_name, ".", 1))
            continue;
        /* ...and the XenD server config file */
        if (!strncmp(ent->d_name, XEND_CONFIG_FILE, strlen(XEND_CONFIG_FILE)))
            continue;
        /* ...and random PCI config cruft */
        if (!strncmp(ent->d_name, XEND_PCI_CONFIG_PREFIX, strlen(XEND_PCI_CONFIG_PREFIX)))
            continue;
        /* ...and the example domain configs */
        if (!strncmp(ent->d_name, XM_EXAMPLE_PREFIX, strlen(XM_EXAMPLE_PREFIX)))
            continue;
        /* ...and the QEMU networking script */
        if (!strncmp(ent->d_name, QEMU_IF_SCRIPT, strlen(QEMU_IF_SCRIPT)))
            continue;

        /* ...and editor backups */
        if (ent->d_name[0] == '#')
            continue;
        if (ent->d_name[strlen(ent->d_name)-1] == '~')
            continue;

        /* Build the full file path */
        if ((strlen(configDir) + 1 + strlen(ent->d_name) + 1) > PATH_MAX)
            continue;
        strcpy(path, configDir);
        strcat(path, "/");
        strcat(path, ent->d_name);

        /* Skip anything which isn't a file (takes care of scripts/ subdir */
        if ((stat(path, &st) < 0) ||
            (!S_ISREG(st.st_mode))) {
            continue;
        }

        /* If we already have a matching entry and it is not
           modified, then carry on to next one*/
        if ((entry = virHashLookup(configCache, path))) {
            const char *olddomname = NULL;

            if (entry->refreshedAt >= st.st_mtime) {
                entry->refreshedAt = now;
                continue;
            }

            /* If we currently own the name, then release it and
               re-acquire it later - just in case it was renamed */
            if (xenXMConfigGetString(entry->conf, "name", &olddomname) != -1) {
                char *nameowner = (char *)virHashLookup(nameConfigMap, olddomname);
                if (nameowner && !strcmp(nameowner, path)) {
                    virHashRemoveEntry(nameConfigMap, olddomname, NULL);
                }
            }

            /* Clear existing config entry which needs refresh */
            virConfFree(entry->conf);
            entry->conf = NULL;
        } else { /* Completely new entry */
            newborn = 1;
            if (!(entry = malloc(sizeof(*entry)))) {
                xenXMError (conn, VIR_ERR_NO_MEMORY, strerror (errno));
                goto cleanup;
            }
            memcpy(entry->filename, path, PATH_MAX);
        }
        entry->refreshedAt = now;

        if (!(entry->conf = virConfReadFile(entry->filename)) ||
            xenXMConfigEnsureIdentity(entry->conf, ent->d_name) < 0) {
            if (!newborn) {
                virHashRemoveEntry(configCache, path, NULL);
            }
            free(entry);
            continue;
        }

        /* Lookup what domain name the conf contains */
        if (xenXMConfigGetString(entry->conf, "name", &domname) < 0) {
            if (!newborn) {
                virHashRemoveEntry(configCache, path, NULL);
            }
            free(entry);
            xenXMError (conn, VIR_ERR_INTERNAL_ERROR,
                        _("xenXMConfigCacheRefresh: name"));
            goto cleanup;
        }

        /* If its a completely new entry, it must be stuck into
           the cache (refresh'd entries are already registered) */
        if (newborn) {
            if (virHashAddEntry(configCache, entry->filename, entry) < 0) {
                virConfFree(entry->conf);
                free(entry);
                xenXMError (conn, VIR_ERR_INTERNAL_ERROR,
                            _("xenXMConfigCacheRefresh: virHashAddEntry"));
                goto cleanup;
            }
        }

        /* See if we need to map this config file in as the primary owner
         * of the domain in question
         */
        if (!virHashLookup(nameConfigMap, domname)) {
            if (virHashAddEntry(nameConfigMap, domname, entry->filename) < 0) {
                virHashRemoveEntry(configCache, ent->d_name, NULL);
                virConfFree(entry->conf);
                free(entry);
            }
        }
    }

    /* Reap all entries which were not changed, by comparing
       their refresh timestamp - the timestamp should match
       'now' if they were refreshed. If timestamp doesn't match
       then the config is no longer on disk */
    virHashRemoveSet(configCache, xenXMConfigReaper, xenXMConfigFree, (const void*) &now);
    ret = 0;

 cleanup:
    if (dh)
        closedir(dh);

    return (ret);
}


/*
 * Open a 'connection' to the config file directory ;-)
 * We just create a hash table to store config files in.
 * We only support a single directory, so repeated calls
 * to open all end up using the same cache of files
 */
int
xenXMOpen (virConnectPtr conn ATTRIBUTE_UNUSED,
           xmlURIPtr uri ATTRIBUTE_UNUSED,
           virConnectAuthPtr auth ATTRIBUTE_UNUSED,
           int flags ATTRIBUTE_UNUSED)
{
    if (configCache == NULL) {
        configCache = virHashCreate(50);
        if (!configCache)
            return (-1);
        nameConfigMap = virHashCreate(50);
        if (!nameConfigMap) {
            virHashFree(configCache, NULL);
            configCache = NULL;
            return (-1);
        }
        /* Force the cache to be reloaded next time that
         * xenXMConfigCacheRefresh is called.
         */
        lastRefresh = 0;
    }
    nconnections++;

    return (0);
}

/*
 * Free the config files in the cache if this is the
 * last connection
 */
int xenXMClose(virConnectPtr conn ATTRIBUTE_UNUSED) {
    nconnections--;
    if (nconnections <= 0) {
        virHashFree(nameConfigMap, NULL);
        nameConfigMap = NULL;
        virHashFree(configCache, xenXMConfigFree);
        configCache = NULL;
    }
    return (0);
}

/*
 * Since these are all offline domains, we only return info about
 * VCPUs and memory.
 */
int xenXMDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info) {
    const char *filename;
    xenXMConfCachePtr entry;
    long vcpus;
    long mem;
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return(-1);
    }

    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    memset(info, 0, sizeof(virDomainInfo));
    if (xenXMConfigGetInt(entry->conf, "memory", &mem) < 0 ||
        mem < 0)
        info->memory = MIN_XEN_GUEST_SIZE * 1024 * 2;
    else
        info->memory = (unsigned long)mem * 1024;
    if (xenXMConfigGetInt(entry->conf, "maxmem", &mem) < 0 ||
        mem < 0)
        info->maxMem = info->memory;
    else
        info->maxMem = (unsigned long)mem * 1024;

    if (xenXMConfigGetInt(entry->conf, "vcpus", &vcpus) < 0 ||
        vcpus < 0)
        info->nrVirtCpu = 1;
    else
        info->nrVirtCpu = (unsigned short)vcpus;
    info->state = VIR_DOMAIN_SHUTOFF;
    info->cpuTime = 0;

    return (0);

}

#define MAX_VFB 1024
/*
 * Turn a config record into a lump of XML describing the
 * domain, suitable for later feeding for virDomainCreateLinux
 */
char *xenXMDomainFormatXML(virConnectPtr conn, virConfPtr conf) {
    virBufferPtr buf;
    char *xml;
    const char *name;
    unsigned char uuid[VIR_UUID_BUFLEN];
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    const char *str;
    int hvm = 0;
    long val;
    virConfValuePtr list;
    int vnc = 0, sdl = 0;
    char vfb[MAX_VFB];
    long vncdisplay;
    long vncunused = 1;
    const char *vnclisten = NULL;
    const char *vncpasswd = NULL;
    const char *keymap = NULL;
    xenUnifiedPrivatePtr priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (xenXMConfigGetString(conf, "name", &name) < 0)
        return (NULL);
    if (xenXMConfigGetUUID(conf, "uuid", uuid) < 0)
        return (NULL);

    buf = virBufferNew(4096);

    virBufferAddLit(buf, "<domain type='xen'>\n");
    virBufferVSprintf(buf, "  <name>%s</name>\n", name);
    virUUIDFormat(uuid, uuidstr);
    virBufferVSprintf(buf, "  <uuid>%s</uuid>\n", uuidstr);

    if ((xenXMConfigGetString(conf, "builder", &str) == 0) &&
        !strcmp(str, "hvm"))
        hvm = 1;

    if (hvm) {
        const char *boot;
        virBufferAddLit(buf, "  <os>\n");
        virBufferAddLit(buf, "    <type>hvm</type>\n");
        if (xenXMConfigGetString(conf, "kernel", &str) == 0)
            virBufferVSprintf(buf, "    <loader>%s</loader>\n", str);

        if (xenXMConfigGetString(conf, "boot", &boot) < 0)
            boot = "c";

        while (*boot) {
            const char *dev;
            switch (*boot) {
            case 'a':
                dev = "fd";
                break;
            case 'd':
                dev = "cdrom";
                break;
            case 'c':
            default:
                dev = "hd";
                break;
            }
            virBufferVSprintf(buf, "    <boot dev='%s'/>\n", dev);
            boot++;
        }

        virBufferAddLit(buf, "  </os>\n");
    } else {

        if (xenXMConfigGetString(conf, "bootloader", &str) == 0)
            virBufferVSprintf(buf, "  <bootloader>%s</bootloader>\n", str);
        if (xenXMConfigGetString(conf, "bootargs", &str) == 0)
            virBufferEscapeString(buf, "  <bootloader_args>%s</bootloader_args>\n", str);
        if (xenXMConfigGetString(conf, "kernel", &str) == 0) {
            virBufferAddLit(buf, "  <os>\n");
            virBufferAddLit(buf, "    <type>linux</type>\n");
            virBufferVSprintf(buf, "    <kernel>%s</kernel>\n", str);
            if (xenXMConfigGetString(conf, "ramdisk", &str) == 0)
                virBufferVSprintf(buf, "    <initrd>%s</initrd>\n", str);
            if (xenXMConfigGetString(conf, "extra", &str) == 0)
                virBufferEscapeString(buf, "    <cmdline>%s</cmdline>\n", str);
            virBufferAddLit(buf, "  </os>\n");
        }
    }

    if (xenXMConfigGetInt(conf, "memory", &val) < 0)
        val = MIN_XEN_GUEST_SIZE * 2;
    virBufferVSprintf(buf, "  <currentMemory>%ld</currentMemory>\n",
                      val * 1024);

    if (xenXMConfigGetInt(conf, "maxmem", &val) < 0)
        if (xenXMConfigGetInt(conf, "memory", &val) < 0)
            val = MIN_XEN_GUEST_SIZE * 2;
    virBufferVSprintf(buf, "  <memory>%ld</memory>\n", val * 1024);

    virBufferAddLit(buf, "  <vcpu");
    if (xenXMConfigGetString(conf, "cpus", &str) == 0) {
        char *ranges;

        ranges = virConvertCpuSet(conn, str, 0);
        if (ranges != NULL) {
            virBufferVSprintf(buf, " cpuset='%s'", ranges);
            free(ranges);
        } else
            virBufferVSprintf(buf, " cpuset='%s'", str);
    }
    if (xenXMConfigGetInt(conf, "vcpus", &val) < 0)
        val = 1;
    virBufferVSprintf(buf, ">%ld</vcpu>\n", val);

    if (xenXMConfigGetString(conf, "on_poweroff", &str) < 0)
        str = "destroy";
    virBufferVSprintf(buf, "  <on_poweroff>%s</on_poweroff>\n", str);

    if (xenXMConfigGetString(conf, "on_reboot", &str) < 0)
        str = "restart";
    virBufferVSprintf(buf, "  <on_reboot>%s</on_reboot>\n", str);

    if (xenXMConfigGetString(conf, "on_crash", &str) < 0)
        str = "restart";
    virBufferVSprintf(buf, "  <on_crash>%s</on_crash>\n", str);


    if (hvm) {
        virBufferAddLit(buf, "  <features>\n");
        if (xenXMConfigGetInt(conf, "pae", &val) == 0 &&
            val)
            virBufferAddLit(buf, "    <pae/>\n");
        if (xenXMConfigGetInt(conf, "acpi", &val) == 0 &&
            val)
            virBufferAddLit(buf, "    <acpi/>\n");
        if (xenXMConfigGetInt(conf, "apic", &val) == 0 &&
            val)
            virBufferAddLit(buf, "    <apic/>\n");
        virBufferAddLit(buf, "  </features>\n");

        if (xenXMConfigGetInt(conf, "localtime", &val) < 0)
            val = 0;
        virBufferVSprintf(buf, "  <clock offset='%s'/>\n", val ? "localtime" : "utc");
    }

    virBufferAddLit(buf, "  <devices>\n");

    if (hvm) {
        if (xenXMConfigGetString(conf, "device_model", &str) == 0)
            virBufferVSprintf(buf, "    <emulator>%s</emulator>\n", str);
    }

    list = virConfGetValue(conf, "disk");
    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            int block = 0;
            int cdrom = 0;
            char src[PATH_MAX];
            char dev[NAME_MAX];
            char drvName[NAME_MAX] = "";
            char drvType[NAME_MAX] = "";
            char *head;
            char *offset;
            char *tmp, *tmp1;

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skipdisk;
            head = list->str;

            /*
             * Disks have 3 components, SOURCE,DEST-DEVICE,MODE
             * eg, phy:/dev/HostVG/XenGuest1,xvda,w
             * The SOURCE is usually prefixed with a driver type,
             * and optionally driver sub-type
             * The DEST-DEVICE is optionally post-fixed with disk type
             */

            /* Extract the source */
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skipdisk;
            if ((offset - head) >= (PATH_MAX-1))
                goto skipdisk;
            strncpy(src, head, (offset - head));
            src[(offset-head)] = '\0';
            head = offset + 1;

            /* Extract the dest */
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skipdisk;
            if ((offset - head) >= (PATH_MAX-1))
                goto skipdisk;
            strncpy(dev, head, (offset - head));
            dev[(offset-head)] = '\0';
            head = offset + 1;


            /* Extract source driver type */
            if (!src[0]) {
                strcpy(drvName, "phy");
                tmp = &src[0];
            } else if (!(tmp = strchr(src, ':')) || !tmp[0]) {
                    goto skipdisk;
            } else {
                strncpy(drvName, src, (tmp-src));
                drvName[tmp-src] = '\0';
            }

            /* And the source driver sub-type */
            if (!strncmp(drvName, "tap", 3)) {
                if (!(tmp1 = strchr(tmp+1, ':')) || !tmp1[0])
                    goto skipdisk;
                strncpy(drvType, tmp+1, (tmp1-(tmp+1)));
                memmove(src, src+(tmp1-src)+1, strlen(src)-(tmp1-src));
            } else {
                drvType[0] = '\0';
                if (src[0])
                        memmove(src, src+(tmp-src)+1, strlen(src)-(tmp-src));
            }

            /* phy: type indicates a block device */
            if (!strcmp(drvName, "phy")) {
                block = 1;
            }

            /* Remove legacy ioemu: junk */
            if (!strncmp(dev, "ioemu:", 6)) {
                memmove(dev, dev+6, strlen(dev)-5);
            }

            /* Check for a :cdrom/:disk postfix */
            if ((tmp = strchr(dev, ':')) != NULL) {
                if (!strcmp(tmp, ":cdrom"))
                    cdrom = 1;
                tmp[0] = '\0';
            }

            virBufferVSprintf(buf, "    <disk type='%s' device='%s'>\n",
                              block ? "block" : "file",
                              cdrom ? "cdrom" : "disk");
            if (drvType[0])
                virBufferVSprintf(buf, "      <driver name='%s' type='%s'/>\n", drvName, drvType);
            else
                virBufferVSprintf(buf, "      <driver name='%s'/>\n", drvName);
            if (src[0])
                virBufferVSprintf(buf, "      <source %s='%s'/>\n", block ? "dev" : "file", src);
            virBufferVSprintf(buf, "      <target dev='%s'/>\n", dev);
            if (!strcmp(head, "r") ||
                !strcmp(head, "ro"))
                virBufferAddLit(buf, "      <readonly/>\n");
            else if ((!strcmp(head, "w!")) ||
                     (!strcmp(head, "!")))
                virBufferAddLit(buf, "      <shareable/>\n");
            virBufferAddLit(buf, "    </disk>\n");

        skipdisk:
            list = list->next;
        }
    }

    if (hvm && priv->xendConfigVersion == 1) {
        if (xenXMConfigGetString(conf, "cdrom", &str) == 0) {
            virBufferAddLit(buf, "    <disk type='file' device='cdrom'>\n");
            virBufferAddLit(buf, "      <driver name='file'/>\n");
            virBufferVSprintf(buf, "      <source file='%s'/>\n", str);
            virBufferAddLit(buf, "      <target dev='hdc'/>\n");
            virBufferAddLit(buf, "      <readonly/>\n");
            virBufferAddLit(buf, "    </disk>\n");
        }
    }

    list = virConfGetValue(conf, "vif");
    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            int type = -1;
            char script[PATH_MAX];
            char ip[16];
            char mac[18];
            char bridge[50];
            char *key;

            bridge[0] = '\0';
            mac[0] = '\0';
            script[0] = '\0';
            ip[0] = '\0';

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skipnic;

            key = list->str;
            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');

                if (!(data = strchr(key, '=')) || (data[0] == '\0'))
                    goto skipnic;
                data++;

                if (!strncmp(key, "mac=", 4)) {
                    int len = nextkey ? (nextkey - data) : 17;
                    if (len > 17)
                        len = 17;
                    strncpy(mac, data, len);
                    mac[len] = '\0';
                } else if (!strncmp(key, "bridge=", 7)) {
                    int len = nextkey ? (nextkey - data) : sizeof(bridge)-1;
                    type = 1;
                    if (len > (sizeof(bridge)-1))
                        len = sizeof(bridge)-1;
                    strncpy(bridge, data, len);
                    bridge[len] = '\0';
                } else if (!strncmp(key, "script=", 7)) {
                    int len = nextkey ? (nextkey - data) : PATH_MAX-1;
                    if (len > (PATH_MAX-1))
                        len = PATH_MAX-1;
                    strncpy(script, data, len);
                    script[len] = '\0';
                } else if (!strncmp(key, "ip=", 3)) {
                    int len = nextkey ? (nextkey - data) : 15;
                    if (len > 15)
                        len = 15;
                    strncpy(ip, data, len);
                    ip[len] = '\0';
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }

            /* XXX Forcing to pretend its a bridge */
            if (type == -1) {
                type = 1;
            }

            virBufferAddLit(buf, "    <interface type='bridge'>\n");
            if (mac[0])
                virBufferVSprintf(buf, "      <mac address='%s'/>\n", mac);
            if (type == 1 && bridge[0])
                virBufferVSprintf(buf, "      <source bridge='%s'/>\n", bridge);
            if (script[0])
                virBufferVSprintf(buf, "      <script path='%s'/>\n", script);
            if (ip[0])
                virBufferVSprintf(buf, "      <ip address='%s'/>\n", ip);
            virBufferAddLit(buf, "    </interface>\n");

        skipnic:
            list = list->next;
        }
    }

    if (hvm) {
        if (xenXMConfigGetString(conf, "usbdevice", &str) == 0 && str) {
            if (!strcmp(str, "tablet"))
                virBufferAddLit(buf, "    <input type='tablet' bus='usb'/>\n");
            else if (!strcmp(str, "mouse"))
                virBufferAddLit(buf, "    <input type='mouse' bus='usb'/>\n");
            /* Ignore else branch - probably some other non-input device we don't
               support in libvirt yet */
        }
    }

    /* HVM guests, or old PV guests use this config format */
    if (hvm || priv->xendConfigVersion < 3) {
        if (xenXMConfigGetInt(conf, "vnc", &val) == 0 && val) {
            vnc = 1;
            if (xenXMConfigGetInt(conf, "vncunused", &vncunused) < 0)
                vncunused = 1;
            if (xenXMConfigGetInt(conf, "vncdisplay", &vncdisplay) < 0)
                vncdisplay = 0;
            if (xenXMConfigGetString(conf, "vnclisten", &vnclisten) < 0)
                vnclisten = NULL;
            if (xenXMConfigGetString(conf, "vncpasswd", &vncpasswd) < 0)
                vncpasswd = NULL;
            if (xenXMConfigGetString(conf, "keymap", &keymap) < 0)
                keymap = NULL;
        }
        if (xenXMConfigGetInt(conf, "sdl", &val) == 0 && val)
            sdl = 1;
    }
    if (!hvm && !sdl && !vnc) { /* New PV guests use this format */
        list = virConfGetValue(conf, "vfb");
        if (list && list->type == VIR_CONF_LIST &&
            list->list && list->list->type == VIR_CONF_STRING &&
            list->list->str) {

            char *key = vfb;
            strncpy(vfb, list->list->str, MAX_VFB-1);
            vfb[MAX_VFB-1] = '\0';

            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');
                char *end = nextkey;
                if (nextkey) {
                    *end = '\0';
                    nextkey++;
                }

                if (!(data = strchr(key, '=')) || (data[0] == '\0'))
                    break;
                data++;

                if (!strncmp(key, "type=sdl", 8)) {
                    sdl = 1;
                } else if (!strncmp(key, "type=vnc", 8)) {
                    vnc = 1;
                } else if (!strncmp(key, "vncunused=", 10)) {
                    vncunused = strtol(key+10, NULL, 10);
                } else if (!strncmp(key, "vnclisten=", 10)) {
                    vnclisten = key + 10;
                } else if (!strncmp(key, "vncpasswd=", 10)) {
                    vncpasswd = key + 10;
                } else if (!strncmp(key, "keymap=", 7)) {
                    keymap = key + 7;
                } else if (!strncmp(key, "vncdisplay=", 11)) {
                    vncdisplay = strtol(key+11, NULL, 10);
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }
        }
    }

    if (vnc || sdl) {
        virBufferVSprintf(buf, "    <input type='mouse' bus='%s'/>\n", hvm ? "ps2":"xen");
    }
    if (vnc) {
        virBufferVSprintf(buf,
                          "    <graphics type='vnc' port='%ld'",
                          (vncunused ? -1 : 5900+vncdisplay));
        if (vnclisten) {
            virBufferVSprintf(buf, " listen='%s'", vnclisten);
        }
        if (vncpasswd) {
            virBufferVSprintf(buf, " passwd='%s'", vncpasswd);
        }
        if (keymap) {
            virBufferVSprintf(buf, " keymap='%s'", keymap);
        }
        virBufferAddLit(buf, "/>\n");
    }
    if (sdl) {
        virBufferAddLit(buf, "    <graphics type='sdl'/>\n");
    }

    if (hvm) {
        if (xenXMConfigGetString(conf, "parallel", &str) == 0) {
            if (STRNEQ(str, "none"))
                xend_parse_sexp_desc_char(conn, buf, "parallel", 0, str, NULL);
        }
        if (xenXMConfigGetString(conf, "serial", &str) == 0) {
            if (STRNEQ(str, "none")) {
                xend_parse_sexp_desc_char(conn, buf, "serial", 0, str, NULL);
                /* Add back-compat console tag for primary console */
                xend_parse_sexp_desc_char(conn, buf, "console", 0, str, NULL);
            }
        }
    } else {
        /* Paravirt implicitly always has a single console */
        virBufferAddLit(buf, "    <console type='pty'>\n");
        virBufferAddLit(buf, "      <target port='0'/>\n");
        virBufferAddLit(buf, "    </console>\n");
    }

    virBufferAddLit(buf, "  </devices>\n");

    virBufferAddLit(buf, "</domain>\n");

    xml = buf->content;
    buf->content = NULL;
    virBufferFree(buf);
    return (xml);
}


/*
 * Turn a config record into a lump of XML describing the
 * domain, suitable for later feeding for virDomainCreateLinux
 */
char *xenXMDomainDumpXML(virDomainPtr domain, int flags ATTRIBUTE_UNUSED) {
    const char *filename;
    xenXMConfCachePtr entry;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return(NULL);
    }
    if (domain->id != -1)
        return (NULL);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (NULL);

    if (!(entry = virHashLookup(configCache, filename)))
        return (NULL);

    return xenXMDomainFormatXML(domain->conn, entry->conf);
}


/*
 * Update amount of memory in the config file
 */
int xenXMDomainSetMemory(virDomainPtr domain, unsigned long memory) {
    const char *filename;
    xenXMConfCachePtr entry;
    virConfValuePtr value;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (!(value = malloc(sizeof(*value))))
        return (-1);

    value->type = VIR_CONF_LONG;
    value->l = (memory/1024);

    if (virConfSetValue(entry->conf, "memory", value) < 0)
        return (-1);

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        return (-1);

    return (0);
}

/*
 * Update maximum memory limit in config
 */
int xenXMDomainSetMaxMemory(virDomainPtr domain, unsigned long memory) {
    const char *filename;
    xenXMConfCachePtr entry;
    virConfValuePtr value;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (!(value = malloc(sizeof(*value))))
        return (-1);

    value->type = VIR_CONF_LONG;
    value->l = (memory/1024);

    if (virConfSetValue(entry->conf, "maxmem", value) < 0)
        return (-1);

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        return (-1);

    return (0);
}

/*
 * Get max memory limit from config
 */
unsigned long xenXMDomainGetMaxMemory(virDomainPtr domain) {
    const char *filename;
    xenXMConfCachePtr entry;
    long val;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (xenXMConfigGetInt(entry->conf, "maxmem", &val) < 0 ||
        val < 0)
        if (xenXMConfigGetInt(entry->conf, "memory", &val) < 0 ||
            val < 0)
            val = MIN_XEN_GUEST_SIZE * 2;

    return (val * 1024);
}

/*
 * Set the VCPU count in config
 */
int xenXMDomainSetVcpus(virDomainPtr domain, unsigned int vcpus) {
    const char *filename;
    xenXMConfCachePtr entry;
    virConfValuePtr value;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);
    if (domain->id != -1)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (!(value = malloc(sizeof(*value))))
        return (-1);

    value->type = VIR_CONF_LONG;
    value->l = vcpus;

    if (virConfSetValue(entry->conf, "vcpus", value) < 0)
        return (-1);

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        return (-1);

    return (0);
}

/**
 * xenXMDomainPinVcpu:
 * @domain: pointer to domain object
 * @vcpu: virtual CPU number (reserved)
 * @cpumap: pointer to a bit map of real CPUs (in 8-bit bytes)
 * @maplen: length of cpumap in bytes
 *
 * Set the vcpu affinity in config
 *
 * Returns 0 for success; -1 (with errno) on error
 */
int xenXMDomainPinVcpu(virDomainPtr domain,
                       unsigned int vcpu ATTRIBUTE_UNUSED,
                       unsigned char *cpumap, int maplen)
{
    const char *filename;
    xenXMConfCachePtr entry;
    virBufferPtr mapbuf;
    char *mapstr = NULL;
    char *ranges = NULL;
    int i, j, n, comma = 0;
    int ret = -1;

    if (domain == NULL || domain->conn == NULL || domain->name == NULL
        || cpumap == NULL || maplen < 1 || maplen > (int)sizeof(cpumap_t)) {
        xenXMError(domain ? domain->conn : NULL, VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return -1;
    }
    if (domain->conn->flags & VIR_CONNECT_RO) {
        xenXMError (domain->conn, VIR_ERR_INVALID_ARG,
                    _("read only connection"));
        return -1;
    }
    if (domain->id != -1) {
        xenXMError (domain->conn, VIR_ERR_INVALID_ARG,
                    _("not inactive domain"));
        return -1;
    }

    if (!(filename = virHashLookup(nameConfigMap, domain->name))) {
        xenXMError (domain->conn, VIR_ERR_INTERNAL_ERROR, _("virHashLookup"));
        return -1;
    }
    if (!(entry = virHashLookup(configCache, filename))) {
        xenXMError (domain->conn, VIR_ERR_INTERNAL_ERROR,
                    _("can't retrieve config file for domain"));
        return -1;
    }

    /* from bit map, build character string of mapped CPU numbers */
    mapbuf = virBufferNew (16);
    if (mapbuf == NULL) {
        xenXMError (domain->conn, VIR_ERR_NO_MEMORY, __FUNCTION__);
        return -1;
    }
    for (i = 0; i < maplen; i++)
        for (j = 0; j < 8; j++)
            if ((cpumap[i] & (1 << j))) {
                n = i*8 + j;

                if (comma) {
                    if (virBufferAddLit (mapbuf, ",") == -1) {
                        xenXMError (domain->conn, VIR_ERR_NO_MEMORY, __FUNCTION__);
                        virBufferFree (mapbuf);
                    return -1;
                    }
                }
                comma = 1;

                if (virBufferVSprintf (mapbuf, "%d", n) == -1) {
                    xenXMError (domain->conn, VIR_ERR_NO_MEMORY, __FUNCTION__);
                    virBufferFree (mapbuf);
                    return -1;
                }
            }

    mapstr = virBufferContentAndFree (mapbuf);

    /* convert the mapstr to a range based string */
    ranges = virConvertCpuSet(domain->conn, mapstr, 0);

    if (ranges != NULL) {
        if (xenXMConfigSetString(entry->conf, "cpus", ranges) < 0)
            goto cleanup;
    } else
        if (xenXMConfigSetString(entry->conf, "cpus", mapstr) < 0)
            goto cleanup;

    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    free(mapstr);
    free(ranges);
    return (ret);
}

/*
 * Find an inactive domain based on its name
 */
virDomainPtr xenXMDomainLookupByName(virConnectPtr conn, const char *domname) {
    const char *filename;
    xenXMConfCachePtr entry;
    virDomainPtr ret;
    unsigned char uuid[VIR_UUID_BUFLEN];
    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (domname == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(filename = virHashLookup(nameConfigMap, domname)))
        return (NULL);

    if (!(entry = virHashLookup(configCache, filename))) {
        return (NULL);
    }


    if (xenXMConfigGetUUID(entry->conf, "uuid", uuid) < 0) {
        return (NULL);
    }

    if (!(ret = virGetDomain(conn, domname, uuid))) {
        return (NULL);
    }

    /* Ensure its marked inactive, because may be cached
       handle to a previously active domain */
    ret->id = -1;

    return (ret);
}


/*
 * Hash table iterator to search for a domain based on UUID
 */
static int xenXMDomainSearchForUUID(const void *payload, const char *name ATTRIBUTE_UNUSED, const void *data) {
    unsigned char uuid[VIR_UUID_BUFLEN];
    const unsigned char *wantuuid = (const unsigned char *)data;
    const xenXMConfCachePtr entry = (const xenXMConfCachePtr)payload;

    if (xenXMConfigGetUUID(entry->conf, "uuid", uuid) < 0) {
        return (0);
    }

    if (!memcmp(uuid, wantuuid, VIR_UUID_BUFLEN))
        return (1);

    return (0);
}

/*
 * Find an inactive domain based on its UUID
 */
virDomainPtr xenXMDomainLookupByUUID(virConnectPtr conn,
                                     const unsigned char *uuid) {
    xenXMConfCachePtr entry;
    virDomainPtr ret;
    const char *domname;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (uuid == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(entry = virHashSearch(configCache, xenXMDomainSearchForUUID, (const void *)uuid))) {
        return (NULL);
    }

    if (xenXMConfigGetString(entry->conf, "name", &domname) < 0) {
        return (NULL);
    }

    if (!(ret = virGetDomain(conn, domname, uuid))) {
        return (NULL);
    }

    /* Ensure its marked inactive, because may be cached
       handle to a previously active domain */
    ret->id = -1;

    return (ret);
}


/*
 * Start a domain from an existing defined config file
 */
int xenXMDomainCreate(virDomainPtr domain) {
    char *xml;
    char *sexpr;
    int ret;
    unsigned char uuid[VIR_UUID_BUFLEN];
    xenUnifiedPrivatePtr priv;

    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }

    if (domain->id != -1)
        return (-1);
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);

    if (!(xml = xenXMDomainDumpXML(domain, 0)))
        return (-1);

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;

    if (!(sexpr = virDomainParseXMLDesc(domain->conn, xml, NULL, priv->xendConfigVersion))) {
        free(xml);
        return (-1);
    }
    free(xml);

    ret = xenDaemonDomainCreateLinux(domain->conn, sexpr);
    free(sexpr);
    if (ret != 0) {
        return (-1);
    }

    if ((ret = xenDaemonDomainLookupByName_ids(domain->conn, domain->name, uuid)) < 0) {
        return (-1);
    }
    domain->id = ret;

    if ((ret = xend_wait_for_devices(domain->conn, domain->name)) < 0)
        goto cleanup;

    if ((ret = xenDaemonDomainResume(domain)) < 0)
        goto cleanup;

    return (0);

 cleanup:
    if (domain->id != -1) {
        xenDaemonDomainDestroy(domain);
        domain->id = -1;
    }
    return (-1);
}


static
int xenXMConfigSetInt(virConfPtr conf, const char *setting, long l) {
    virConfValuePtr value = NULL;

    if (!(value = malloc(sizeof(*value))))
        return -1;

    value->type = VIR_CONF_LONG;
    value->next = NULL;
    value->l = l;

    return virConfSetValue(conf, setting, value);
}


static
int xenXMConfigSetString(virConfPtr conf, const char *setting, const char *str) {
    virConfValuePtr value = NULL;

    if (!(value = malloc(sizeof(*value))))
        return -1;

    value->type = VIR_CONF_STRING;
    value->next = NULL;
    if (!(value->str = strdup(str))) {
        free(value);
        return -1;
    }

    return virConfSetValue(conf, setting, value);
}


/*
 * Convenience method to set an int config param
 * based on an XPath expression
 */
static
int xenXMConfigSetIntFromXPath(virConnectPtr conn,
                               virConfPtr conf, xmlXPathContextPtr ctxt,
                               const char *setting, const char *xpath,
                               long scale, int allowMissing, const char *error) {
    xmlXPathObjectPtr obj;
    long intval;
    char *strend;
    int ret = -1;

    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        if (allowMissing)
            ret = 0;
        else
            xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    intval = strtol((char *)obj->stringval, &strend, 10);
    if (strend == (char *)obj->stringval) {
        xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    if (scale > 0)
        intval = intval * scale;
    else if (scale < 0)
        intval = intval / (-1*scale);

    if (xenXMConfigSetInt(conf, setting, intval) < 0)
        goto error;

    ret = 0;

 error:
    xmlXPathFreeObject(obj);

    return ret;
}

/*
 * Convenience method to set a string param
 * based on an XPath expression
 */
static
int xenXMConfigSetStringFromXPath(virConnectPtr conn,
                                  virConfPtr conf, xmlXPathContextPtr ctxt,
                                  const char *setting, const char *xpath,
                                  int allowMissing, const char *error) {
    xmlXPathObjectPtr obj;
    int ret = -1;

    obj = xmlXPathEval(BAD_CAST xpath, ctxt);

    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        if (allowMissing)
            ret = 0;
        else
            xenXMError(conn, VIR_ERR_XML_ERROR, error);
        goto error;
    }

    if (xenXMConfigSetString(conf, setting, (const char *)obj->stringval) < 0)
        goto error;

    ret = 0;

 error:
    xmlXPathFreeObject(obj);

    return ret;
}

static int xenXMParseXMLDisk(xmlNodePtr node, int hvm, int xendConfigVersion, char **disk) {
    xmlNodePtr cur;
    xmlChar *type = NULL;
    xmlChar *device = NULL;
    xmlChar *source = NULL;
    xmlChar *target = NULL;
    xmlChar *drvName = NULL;
    xmlChar *drvType = NULL;
    int readonly = 0;
    int shareable = 0;
    int typ = 0;
    int cdrom = 0;
    int ret = -1;
    int buflen = 0;
    char *buf = NULL;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "file"))
            typ = 0;
        else if (xmlStrEqual(type, BAD_CAST "block"))
            typ = 1;
        xmlFree(type);
    }
    device = xmlGetProp(node, BAD_CAST "device");

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                if (typ == 0)
                    source = xmlGetProp(cur, BAD_CAST "file");
                else
                    source = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((drvName == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "driver"))) {
                drvName = xmlGetProp(cur, BAD_CAST "name");
                if (drvName && !strcmp((const char *)drvName, "tap"))
                    drvType = xmlGetProp(cur, BAD_CAST "type");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                readonly = 1;
            } else if (xmlStrEqual(cur->name, BAD_CAST "shareable")) {
                shareable = 1;
            }
        }
        cur = cur->next;
    }

    if (target == NULL) {
        if (source != NULL)
            xmlFree(source);
        if (device != NULL)
            xmlFree(device);
        return (-1);
    }

    /* Xend (all versions) put the floppy device config
     * under the hvm (image (os)) block
     */
    if (hvm &&
        device &&
        !strcmp((const char *)device, "floppy")) {
        ret = 0;
        goto cleanup;
    }

    /* Xend <= 3.0.2 doesn't include cdrom config here */
    if (hvm &&
        device &&
        !strcmp((const char *)device, "cdrom")) {
        if (xendConfigVersion == 1) {
            ret = 0;
            goto cleanup;
        } else {
            cdrom = 1;
        }
    }

    if (source == NULL && !cdrom) {
        if (target != NULL)
            xmlFree(target);
        if (device != NULL)
            xmlFree(device);
        return (-1);
    }

    if (drvName) {
        buflen += strlen((const char*)drvName) + 1;
        if (!strcmp((const char*)drvName, "tap")) {
            if (drvType)
                buflen += strlen((const char*)drvType) + 1;
            else
                buflen += 4;
        }
    } else {
        if (typ == 0)
            buflen += 5;
        else
            buflen += 4;
    }

    if(source)
        buflen += strlen((const char*)source) + 1;
    else
        buflen += 1;
    buflen += strlen((const char*)target) + 1;
    if (hvm && xendConfigVersion == 1) /* ioemu: */
        buflen += 6;

    if (cdrom) /* :cdrom */
        buflen += 6;

    buflen += 2; /* mode */

    if (!(buf = malloc(buflen)))
        goto cleanup;

    if(source) {
        if (drvName) {
            strcpy(buf, (const char*)drvName);
            if (!strcmp((const char*)drvName, "tap")) {
                strcat(buf, ":");
                if (drvType)
                    strcat(buf, (const char*)drvType);
                else
                    strcat(buf, "aio");
            }
        } else {
            if (typ == 0)
                strcpy(buf, "file");
            else
                strcpy(buf, "phy");
        }
        strcat(buf, ":");
        strcat(buf, (const char*)source);
    } else {
        strcpy(buf, "");
    }
    strcat(buf, ",");
    if (hvm && xendConfigVersion == 1)
        strcat(buf, "ioemu:");
    strcat(buf, (const char*)target);
    if (cdrom)
        strcat(buf, ":cdrom");

    if (readonly)
        strcat(buf, ",r");
    else if (shareable)
        strcat(buf, ",!");
    else
        strcat(buf, ",w");
    ret = 0;
 cleanup:
    xmlFree(drvType);
    xmlFree(drvName);
    xmlFree(device);
    xmlFree(target);
    if(source)
        xmlFree(source);
    *disk = buf;

    return (ret);
}
static char *xenXMParseXMLVif(virConnectPtr conn, xmlNodePtr node, int hvm) {
    xmlNodePtr cur;
    xmlChar *type = NULL;
    xmlChar *source = NULL;
    xmlChar *mac = NULL;
    xmlChar *script = NULL;
    xmlChar *ip = NULL;
    int typ = 0;
    char *buf = NULL;
    int buflen = 0;
    char *bridge = NULL;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "bridge"))
            typ = 0;
        else if (xmlStrEqual(type, BAD_CAST "ethernet"))
            typ = 1;
        else if (xmlStrEqual(type, BAD_CAST "network"))
            typ = 2;
        xmlFree(type);
    }
    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {

                if (typ == 0)
                    source = xmlGetProp(cur, BAD_CAST "bridge");
                else if (typ == 1)
                    source = xmlGetProp(cur, BAD_CAST "dev");
                else
                    source = xmlGetProp(cur, BAD_CAST "network");
            } else if ((mac == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                mac = xmlGetProp(cur, BAD_CAST "address");
            } else if ((ip == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "ip"))) {
                ip = xmlGetProp(cur, BAD_CAST "address");
            } else if ((script == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "script"))) {
                script = xmlGetProp(cur, BAD_CAST "path");
            }
        }
        cur = cur->next;
    }

    if (!mac) {
        goto cleanup;
    }
    buflen += 5 + strlen((const char *)mac);
    if (source) {
        if (typ == 0) {
            buflen += 8 + strlen((const char *)source);
        } else if (typ == 1) {
            buflen += 5 + strlen((const char *)source);
        } else {
            virNetworkPtr network = virNetworkLookupByName(conn, (const char *) source);
            if (!network || !(bridge = virNetworkGetBridgeName(network))) {
                if (network)
                    virNetworkFree(network);
                goto cleanup;
            }
            virNetworkFree(network);
            buflen += 8 + strlen(bridge);
        }
    }
    if (hvm)
        buflen += 11;
    if (script)
        buflen += 8 + strlen((const char*)script);
    if (ip)
        buflen += 4 + strlen((const char*)ip);

    if (!(buf = malloc(buflen)))
        goto cleanup;

    strcpy(buf, "mac=");
    strcat(buf, (const char*)mac);
    if (source) {
        if (typ == 0) {
            strcat(buf, ",bridge=");
            strcat(buf, (const char*)source);
        } else if (typ == 1) {
            strcat(buf, ",dev=");
            strcat(buf, (const char*)source);
        } else {
            strcat(buf, ",bridge=");
            strcat(buf, bridge);
        }
    }
    if (hvm) {
        strcat(buf, ",type=ioemu");
    }
    if (script) {
        strcat(buf, ",script=");
        strcat(buf, (const char*)script);
    }
    if (ip) {
        strcat(buf, ",ip=");
        strcat(buf, (const char*)ip);
    }

 cleanup:
    free(bridge);
    if (mac != NULL)
        xmlFree(mac);
    if (source != NULL)
        xmlFree(source);
    if (script != NULL)
        xmlFree(script);
    if (ip != NULL)
        xmlFree(ip);

    return buf;
}


virConfPtr xenXMParseXMLToConfig(virConnectPtr conn, const char *xml) {
    xmlDocPtr doc = NULL;
    xmlNodePtr node;
    xmlXPathObjectPtr obj = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlChar *prop = NULL;
    virConfPtr conf = NULL;
    int hvm = 0, i;
    xenUnifiedPrivatePtr priv;
    char *cpus;

    doc = xmlReadDoc((const xmlChar *) xml, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (doc == NULL) {
        xenXMError(conn, VIR_ERR_XML_ERROR,
                   _("cannot read XML domain definition"));
        return (NULL);
    }
    node = xmlDocGetRootElement(doc);
    if ((node == NULL) || (!xmlStrEqual(node->name, BAD_CAST "domain"))) {
        xenXMError(conn, VIR_ERR_XML_ERROR,
                   _("missing top level domain element"));
        goto error;
    }

    prop = xmlGetProp(node, BAD_CAST "type");
    if (prop != NULL) {
        if (!xmlStrEqual(prop, BAD_CAST "xen")) {
            xenXMError(conn, VIR_ERR_XML_ERROR,
                       _("domain type is invalid"));
            goto error;
        }
        xmlFree(prop);
        prop = NULL;
    }
    if (!(ctxt = xmlXPathNewContext(doc))) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("cannot create XPath context"));
        goto error;
    }
    if (!(conf = virConfNew()))
        goto error;


    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "name", "string(/domain/name)", 0,
                                      "domain name element missing") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "uuid", "string(/domain/uuid)", 0,
                                      "domain uuid element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "maxmem", "string(/domain/memory)", -1024, 0,
                                   "domain memory element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "memory", "string(/domain/memory)", -1024, 0,
                                   "domain memory element missing") < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "memory", "string(/domain/currentMemory)", -1024, 1,
                                   "domain currentMemory element missing") < 0)
        goto error;

    if (xenXMConfigSetInt(conf, "vcpus", 1) < 0)
        goto error;

    if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vcpus", "string(/domain/vcpu)", 0, 1,
                                   "cannot set vcpus config parameter") < 0)
        goto error;

    cpus = virXPathString("string(/domain/vcpu/@cpuset)", ctxt);
    if (cpus != NULL) {
        char *ranges;

        ranges = virConvertCpuSet(conn, cpus, 0);
        if (ranges != NULL) {
            free(cpus);
            if (xenXMConfigSetString(conf, "cpus", ranges) < 0) {
                free(ranges);
                goto error;
            }
            free(ranges);
        } else {
            if (xenXMConfigSetString(conf, "cpus", cpus) < 0) {
                free(cpus);
                goto error;
            }
            free(cpus);
        }
    }

    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type)", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && !strcmp((char*)obj->stringval, "hvm"))
        hvm = 1;
    xmlXPathFreeObject(obj);

    priv = (xenUnifiedPrivatePtr) conn->privateData;

    if (hvm) {
        const char *boot = "c";
        int clockLocal = 0;
        if (xenXMConfigSetString(conf, "builder", "hvm") < 0)
            goto error;

        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "kernel", "string(/domain/os/loader)", 1,
                                          "cannot set the os loader parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/os/boot/@dev)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            if (!strcmp((const char*)obj->stringval, "fd"))
                boot = "a";
            else if (!strcmp((const char*)obj->stringval, "hd"))
                boot = "c";
            else if (!strcmp((const char*)obj->stringval, "cdrom"))
                boot = "d";
        }
        xmlXPathFreeObject(obj);
        if (xenXMConfigSetString(conf, "boot", boot) < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "pae", "string(count(/domain/features/pae))", 0, 0,
                                       "cannot set the pae parameter") < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "acpi", "string(count(/domain/features/acpi))", 0, 0,
                                       "cannot set the acpi parameter") < 0)
            goto error;

        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "apic", "string(count(/domain/features/apic))", 0, 0,
                                       "cannot set the apic parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/clock/@offset)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            if (!strcmp((const char*)obj->stringval, "localtime"))
                clockLocal = 1;
        }
        xmlXPathFreeObject(obj);
        if (xenXMConfigSetInt(conf, "localtime", clockLocal) < 0)
            goto error;

        if (priv->xendConfigVersion == 1) {
            if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "cdrom", "string(/domain/devices/disk[@device='cdrom']/source/@file)", 1,
                                              "cannot set the cdrom parameter") < 0)
                goto error;
        }
    } else {
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "bootloader", "string(/domain/bootloader)", 1,
                                          "cannot set the bootloader parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "bootargs", "string(/domain/bootloader_args)", 1,
                                          "cannot set the bootloader_args parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "kernel", "string(/domain/os/kernel)", 1,
                                          "cannot set the kernel parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "ramdisk", "string(/domain/os/initrd)", 1,
                                          "cannot set the ramdisk parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "extra", "string(/domain/os/cmdline)", 1,
                                          "cannot set the cmdline parameter") < 0)
            goto error;

    }

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_poweroff", "string(/domain/on_poweroff)", 1,
                                      "cannot set the on_poweroff parameter") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_reboot", "string(/domain/on_reboot)", 1,
                                      "cannot set the on_reboot parameter") < 0)
        goto error;

    if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "on_crash", "string(/domain/on_crash)", 1,
                                      "cannot set the on_crash parameter") < 0)
        goto error;


    if (hvm) {
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "device_model", "string(/domain/devices/emulator)", 1,
                                          "cannot set the device_model parameter") < 0)
            goto error;

        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "usbdevice", "string(/domain/devices/input[@bus='usb' or (not(@bus) and @type='tablet')]/@type)", 1,
                                          "cannot set the usbdevice parameter") < 0)
            goto error;
    }

    if (hvm || priv->xendConfigVersion < 3) {
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "sdl", "string(count(/domain/devices/graphics[@type='sdl']))", 0, 0,
                                       "cannot set the sdl parameter") < 0)
            goto error;
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vnc", "string(count(/domain/devices/graphics[@type='vnc']))", 0, 0,
                                       "cannot set the vnc parameter") < 0)
            goto error;
        if (xenXMConfigSetIntFromXPath(conn, conf, ctxt, "vncunused", "string(count(/domain/devices/graphics[@type='vnc' and @port='-1']))", 0, 0,
                                       "cannot set the vncunused parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "vnclisten", "string(/domain/devices/graphics[@type='vnc']/@listen)", 1,
                                          "cannot set the vnclisten parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "vncpasswd", "string(/domain/devices/graphics[@type='vnc']/@passwd)", 1,
                                          "cannot set the vncpasswd parameter") < 0)
            goto error;
        if (xenXMConfigSetStringFromXPath(conn, conf, ctxt, "keymap", "string(/domain/devices/graphics[@type='vnc']/@keymap)", 1,
                                          "cannot set the keymap parameter") < 0)
            goto error;

        obj = xmlXPathEval(BAD_CAST "string(/domain/devices/graphics[@type='vnc']/@port)", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_STRING) &&
            (obj->stringval != NULL)) {
            int port = strtol((const char *)obj->stringval, NULL, 10);
            if (port != -1) {
                char portstr[50];
                snprintf(portstr, sizeof(portstr), "%d", port-5900);
                if (xenXMConfigSetString(conf, "vncdisplay", portstr) < 0)
                    goto error;
            }
        }
        xmlXPathFreeObject(obj);
    } else {
        virConfValuePtr vfb;
        obj = xmlXPathEval(BAD_CAST "/domain/devices/graphics", ctxt);
        if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
            (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
            if (!(vfb = malloc(sizeof(*vfb)))) {
                xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                goto error;
            }
            vfb->type = VIR_CONF_LIST;
            vfb->list = NULL;
            for (i = obj->nodesetval->nodeNr -1 ; i >= 0 ; i--) {
                xmlChar *type;
                char *val = NULL;

                if (!(type = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "type"))) {
                    continue;
                }
                if (!strcmp((const char*)type, "sdl")) {
                    val = strdup("type=sdl");
                } else if (!strcmp((const char*)type, "vnc")) {
                    int len = 8 + 1; /* type=vnc & NULL */
                    xmlChar *vncport = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "port");
                    xmlChar *vnclisten = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "listen");
                    xmlChar *vncpasswd = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "passwd");
                    xmlChar *keymap = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "keymap");
                    int vncunused = vncport ? (!strcmp((const char*)vncport, "-1") ? 1 : 0) : 1;
                    if (vncunused)
                        len += 12;
                    else
                        len += 12 + strlen((const char*)vncport);/* vncdisplay= */
                    if (vnclisten)
                        len += 11 + strlen((const char*)vnclisten);
                    if (vncpasswd)
                        len += 11 + strlen((const char*)vncpasswd);
                    if (keymap)
                        len += 8 + strlen((const char*)keymap);
                    if ((val = malloc(len)) != NULL) {
                        strcpy(val, "type=vnc");
                        if (vncunused) {
                            strcat(val, ",vncunused=1");
                        } else {
                            char portstr[50];
                            int port = atoi((const char*)vncport);
                            snprintf(portstr, sizeof(portstr), "%d", port-5900);
                            strcat(val, ",vncdisplay=");
                            strcat(val, portstr);
                        }
                        if (vncport)
                            xmlFree(vncport);
                        if (vnclisten) {
                            strcat(val, ",vnclisten=");
                            strcat(val, (const char*)vnclisten);
                            xmlFree(vnclisten);
                        }
                        if (vncpasswd) {
                            strcat(val, ",vncpasswd=");
                            strcat(val, (const char*)vncpasswd);
                            xmlFree(vncpasswd);
                        }
                        if (keymap) {
                            strcat(val, ",keymap=");
                            strcat(val, (const char*)keymap);
                            xmlFree(keymap);
                        }
                    }
                }
                xmlFree(type);
                if (val) {
                    virConfValuePtr disp;
                    if (!(disp = malloc(sizeof(*disp)))) {
                        free(val);
                        xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                        goto error;
                    }
                    disp->type = VIR_CONF_STRING;
                    disp->str = val;
                    disp->next = vfb->list;
                    vfb->list = disp;
                }
            }
            if (virConfSetValue(conf, "vfb", vfb) < 0)
                goto error;
        }
        xmlXPathFreeObject(obj);
    }

    /* analyze of the devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/disk", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        virConfValuePtr disks;
        if (!(disks = malloc(sizeof(*disks)))) {
            xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
            goto error;
        }
        disks->type = VIR_CONF_LIST;
        disks->list = NULL;
        for (i = obj->nodesetval->nodeNr -1 ; i >= 0 ; i--) {
            virConfValuePtr thisDisk;
            char *disk = NULL;
            if (xenXMParseXMLDisk(obj->nodesetval->nodeTab[i], hvm, priv->xendConfigVersion, &disk) < 0)
                goto error;
            if (disk) {
                if (!(thisDisk = malloc(sizeof(*thisDisk)))) {
                    free(disk);
                    xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                    goto error;
                }
                thisDisk->type = VIR_CONF_STRING;
                thisDisk->str = disk;
                thisDisk->next = disks->list;
                disks->list = thisDisk;
            }
        }
        if (virConfSetValue(conf, "disk", disks) < 0)
            goto error;
    }
    xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "/domain/devices/interface", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        virConfValuePtr vifs;
        if (!(vifs = malloc(sizeof(*vifs)))) {
            xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
            goto error;
        }
        vifs->type = VIR_CONF_LIST;
        vifs->list = NULL;
        for (i = obj->nodesetval->nodeNr - 1; i >= 0; i--) {
            virConfValuePtr thisVif;
            char *vif = xenXMParseXMLVif(conn, obj->nodesetval->nodeTab[i], hvm);
            if (!vif)
                goto error;
            if (!(thisVif = malloc(sizeof(*thisVif)))) {
                free(vif);
                xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
                goto error;
            }
            thisVif->type = VIR_CONF_STRING;
            thisVif->str = vif;
            thisVif->next = vifs->list;
            vifs->list = thisVif;
        }
        if (virConfSetValue(conf, "vif", vifs) < 0)
            goto error;
    }
    xmlXPathFreeObject(obj);
    obj = NULL;

    if (hvm) {
        xmlNodePtr cur;
        cur = virXPathNode("/domain/devices/parallel[1]", ctxt);
        if (cur != NULL) {
            char scratch[PATH_MAX];

            if (virDomainParseXMLOSDescHVMChar(conn, scratch, sizeof(scratch), cur) < 0) {
                goto error;
            }

            if (xenXMConfigSetString(conf, "parallel", scratch) < 0)
                goto error;
        } else {
            if (xenXMConfigSetString(conf, "parallel", "none") < 0)
                goto error;
        }

        cur = virXPathNode("/domain/devices/serial[1]", ctxt);
        if (cur != NULL) {
            char scratch[PATH_MAX];
            if (virDomainParseXMLOSDescHVMChar(conn, scratch, sizeof(scratch), cur) < 0)
                goto error;
            if (xenXMConfigSetString(conf, "serial", scratch) < 0)
                goto error;
        } else {
            if (virXPathBoolean("count(/domain/devices/console) > 0", ctxt)) {
                if (xenXMConfigSetString(conf, "serial", "pty") < 0)
                    goto error;
            } else {
                if (xenXMConfigSetString(conf, "serial", "none") < 0)
                    goto error;
            }
        }
    }

    xmlFreeDoc(doc);
    xmlXPathFreeContext(ctxt);

    return conf;

 error:
    if (conf)
        virConfFree(conf);
    if (prop != NULL)
        xmlFree(prop);
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (doc != NULL)
        xmlFreeDoc(doc);
    return (NULL);
}

/*
 * Create a config file for a domain, based on an XML
 * document describing its config
 */
virDomainPtr xenXMDomainDefineXML(virConnectPtr conn, const char *xml) {
    virDomainPtr ret;
    virDomainPtr olddomain;
    char filename[PATH_MAX];
    const char * oldfilename;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virConfPtr conf = NULL;
    xenXMConfCachePtr entry = NULL;
    virConfValuePtr value;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (NULL);
    }
    if (xml == NULL) {
        xenXMError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
        return (NULL);
    }
    if (conn->flags & VIR_CONNECT_RO)
        return (NULL);

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (NULL);

    if (!(conf = xenXMParseXMLToConfig(conn, xml)))
        goto error;

    if (!(value = virConfGetValue(conf, "name")) ||
        value->type != VIR_CONF_STRING ||
        value->str == NULL) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("name config parameter is missing"));
        goto error;
    }

    if (virHashLookup(nameConfigMap, value->str)) {
        /* domain exists, we will overwrite it */

        if (!(oldfilename = (char *)virHashLookup(nameConfigMap, value->str))) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("can't retrieve config filename for domain to overwrite"));
            goto error;
        }

        if (!(entry = virHashLookup(configCache, oldfilename))) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("can't retrieve config entry for domain to overwrite"));
            goto error;
        }

        if (xenXMConfigGetUUID(entry->conf, "uuid", uuid) < 0) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("uuid config parameter is missing"));
            goto error;
        }

        if (!(olddomain = virGetDomain(conn, value->str, uuid)))
            goto error;

        /* Remove the name -> filename mapping */
        if (virHashRemoveEntry(nameConfigMap, value->str, NULL) < 0) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("failed to remove old domain from config map"));
            goto error;
        }

        /* Remove the config record itself */
        if (virHashRemoveEntry(configCache, oldfilename, xenXMConfigFree) < 0) {
            xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                       _("failed to remove old domain from config map"));
            goto error;
        }

        entry = NULL;
    }

    if ((strlen(configDir) + 1 + strlen(value->str) + 1) > PATH_MAX) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("config file name is too long"));
        goto error;
    }

    strcpy(filename, configDir);
    strcat(filename, "/");
    strcat(filename, value->str);

    if (virConfWriteFile(filename, conf) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to write config file"));
        goto error;
    }

    if (!(entry = calloc(1, sizeof(*entry)))) {
        xenXMError(conn, VIR_ERR_NO_MEMORY, _("config"));
        goto error;
    }

    if ((entry->refreshedAt = time(NULL)) == ((time_t)-1)) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to get current time"));
        goto error;
    }

    memmove(entry->filename, filename, PATH_MAX);
    entry->conf = conf;

    if (xenXMConfigGetUUID(conf, "uuid", uuid) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("uuid config parameter is missing"));
        goto error;
    }

    if (virHashAddEntry(configCache, filename, entry) < 0) {
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to store config file handle"));
        goto error;
    }

    if (virHashAddEntry(nameConfigMap, value->str, entry->filename) < 0) {
        virHashRemoveEntry(configCache, filename, NULL);
        xenXMError(conn, VIR_ERR_INTERNAL_ERROR,
                   _("unable to store config file handle"));
        goto error;
    }

    entry = NULL;

    if (!(ret = virGetDomain(conn, value->str, uuid)))
        goto error;
    ret->id = -1;

    return (ret);

 error:
    free(entry);
    if (conf)
        virConfFree(conf);
    return (NULL);
}

/*
 * Delete a domain from disk
 */
int xenXMDomainUndefine(virDomainPtr domain) {
    const char *filename;
    xenXMConfCachePtr entry;
    if ((domain == NULL) || (domain->conn == NULL) || (domain->name == NULL)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        return (-1);
    }

    if (domain->id != -1)
        return (-1);
    if (domain->conn->flags & VIR_CONNECT_RO)
        return (-1);

    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        return (-1);

    if (!(entry = virHashLookup(configCache, filename)))
        return (-1);

    if (unlink(entry->filename) < 0)
        return (-1);

    /* Remove the name -> filename mapping */
    if (virHashRemoveEntry(nameConfigMap, domain->name, NULL) < 0)
        return(-1);

    /* Remove the config record itself */
    if (virHashRemoveEntry(configCache, entry->filename, xenXMConfigFree) < 0)
        return (-1);

    return (0);
}

struct xenXMListIteratorContext {
    virConnectPtr conn;
    int max;
    int count;
    char ** names;
};

static void xenXMListIterator(const void *payload ATTRIBUTE_UNUSED, const char *name, const void *data) {
    struct xenXMListIteratorContext *ctx = (struct xenXMListIteratorContext *)data;
    virDomainPtr dom = NULL;

    if (ctx->count == ctx->max)
        return;

    dom = xenDaemonLookupByName(ctx->conn, name);
    if (!dom) {
        ctx->names[ctx->count] = strdup(name);
        ctx->count++;
    } else {
        virDomainFree(dom);
    }
}


/*
 * List all defined domains, filtered to remove any which
 * are currently running
 */
int xenXMListDefinedDomains(virConnectPtr conn, char **const names, int maxnames) {
    struct xenXMListIteratorContext ctx;

    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (-1);

    if (maxnames > virHashSize(configCache))
        maxnames = virHashSize(configCache);

    ctx.conn = conn;
    ctx.count = 0;
    ctx.max = maxnames;
    ctx.names = names;

    virHashForEach(nameConfigMap, xenXMListIterator, &ctx);
    return (ctx.count);
}

/*
 * Return the maximum number of defined domains - not filtered
 * based on number running
 */
int xenXMNumOfDefinedDomains(virConnectPtr conn) {
    if (!VIR_IS_CONNECT(conn)) {
        xenXMError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }

    if (xenXMConfigCacheRefresh (conn) < 0)
        return (-1);

    return virHashSize(nameConfigMap);
}

/**
 * xenXMDomainAttachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Create a virtual device attachment to backend.
 * XML description is translated into config file.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenXMDomainAttachDevice(virDomainPtr domain, const char *xml) {
    const char *filename = NULL;
    xenXMConfCachePtr entry = NULL;
    xmlDocPtr doc = NULL;
    xmlNodePtr node = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlXPathObjectPtr obj = NULL;
    char *domxml = NULL;
    int ret = -1, hvm = 0;

    if ((!domain) || (!domain->conn) || (!domain->name) || (!xml)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        goto cleanup;
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        goto cleanup;
    if (domain->id != -1)
        goto cleanup;
    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        goto cleanup;
    if (!(entry = virHashLookup(configCache, filename)))
        goto cleanup;
    if (!(entry->conf))
        goto cleanup;

    if (!(domxml = xenXMDomainDumpXML(domain, 0)))
        goto cleanup;

    doc = xmlReadDoc((const xmlChar *) domxml, "domain.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("cannot read XML domain definition"));
        goto cleanup;
    }
    if (!(ctxt = xmlXPathNewContext(doc))) {
        xenXMError(domain->conn, VIR_ERR_INTERNAL_ERROR,
                   _("cannot create XPath context"));
        goto cleanup;
    }
    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type)", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval) && (STREQ((char *)obj->stringval, "hvm")))
        hvm = 1;

    xmlXPathFreeContext(ctxt);
    ctxt = NULL;
    if (doc)
        xmlFreeDoc(doc);
    doc = xmlReadDoc((const xmlChar *) xml, "device.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("cannot read XML domain definition"));
        goto cleanup;
    }
    if (!(ctxt = xmlXPathNewContext(doc))) {
        xenXMError(domain->conn, VIR_ERR_INTERNAL_ERROR,
                   _("cannot create XPath context"));
        goto cleanup;
    }

    if ((node = virXPathNode("/disk", ctxt))) {
        if (xenXMAttachDisk(domain, ctxt, hvm, node, entry))
            goto cleanup;
    } else if ((node = virXPathNode("/interface", ctxt))) {
        if (xenXMAttachInterface(domain, ctxt, hvm, node, entry))
            goto cleanup;
    } else {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("unknown device"));
        goto cleanup;
    }

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    free(domxml);
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);
    if (doc)
        xmlFreeDoc(doc);

    return ret;
}

static int
xenXMAttachDisk(virDomainPtr domain, xmlXPathContextPtr ctxt, int hvm,
                xmlNodePtr node, xenXMConfCachePtr entry) {
    virConfValuePtr list_item = NULL, list_val = NULL, prev = NULL;
    xenUnifiedPrivatePtr priv = NULL;
    xmlChar *type = NULL, *source = NULL, *target = NULL;
    int ret = -1;
    char *dev;

    priv = (xenUnifiedPrivatePtr) domain->conn->privateData;
    xenXMParseXMLDisk(node, hvm, ((xenUnifiedPrivatePtr) domain->conn->privateData)->xendConfigVersion, &dev);
    if (!dev)
        goto cleanup;

    if (!(type = xmlGetProp(node, BAD_CAST "type"))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }
    if (!(node = virXPathNode("/disk/source", ctxt))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }
    if (!strcmp((const char *) type, "block"))
        source = xmlGetProp(node, BAD_CAST "dev");
    else if (!strcmp((const char *) type, "file"))
        source = xmlGetProp(node, BAD_CAST "file");
    else {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }
    if (!(node = virXPathNode("/disk/target", ctxt))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }
    target = xmlGetProp(node, BAD_CAST "dev");

    list_item = virConfGetValue(entry->conf, "disk");
    if (list_item && list_item->type == VIR_CONF_LIST) {
        prev = list_item;
        list_val = list_item->list;
        while (list_val) {
            if ((list_val->type != VIR_CONF_STRING) || (!list_val->str))
                goto skip;
            char domdev[NAME_MAX];
            char *head;
            char *offset;
            char *tmp;

            head = list_val->str;

            /* Extract the source */
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skip;
            if ((offset - head) >= (PATH_MAX-1))
                goto skip;
            head = offset + 1;

            /* Extract the dest */
            if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                goto skip;
            if ((offset - head) >= (PATH_MAX-1))
                goto skip;
            strncpy(domdev, head, (offset - head));
            domdev[(offset-head)] = '\0';
            head = offset + 1;

            /* Remove legacy ioemu: junk */
            if (!strncmp(domdev, "ioemu:", 6)) {
                memmove(domdev, domdev+6, strlen(domdev)-5);
            }

            /* Check for a :cdrom/:disk postfix */
            if ((tmp = strchr(domdev, ':')))
                tmp[0] = '\0';

            if (!(strcmp(domdev, (const char *) target)))
                break;
         skip:
            prev = list_val;
            list_val = list_val->next;
        }
    } else if (!list_item) {
        if (!(list_item = calloc(1, sizeof(virConfValue))))
            goto cleanup;
        list_item->type = VIR_CONF_LIST;
        if(virConfSetValue(entry->conf, "disk", list_item)) {
            free(list_item);
            goto cleanup;
        }
        list_val = NULL;
        prev = list_item;
    } else
        goto cleanup;

    if (!list_val) {
        /* insert */
        if (!(list_val = malloc(sizeof(virConfValue))))
            goto cleanup;
        list_val->type = VIR_CONF_STRING;
        list_val->next = NULL;
        list_val->str = dev;
        if (prev->type == VIR_CONF_LIST)
            prev->list = list_val;
        else
            prev->next = list_val;
    } else {
        /* configure */
        free(list_val->str);
        list_val->str = dev;
    }

    ret = 0;
    goto cleanup;

 cleanup:
    free(type);
    free(source);
    free(target);

    return (ret);
}

static int
xenXMAttachInterface(virDomainPtr domain, xmlXPathContextPtr ctxt, int hvm,
                    xmlNodePtr node, xenXMConfCachePtr entry) {
    virConfValuePtr list_item = NULL, list_val = NULL, prev = NULL;
    xmlChar *type = NULL, *source = NULL, *mac = NULL;
    int ret = -1, autoassign = 0;
    char *dev;

    xmlNodePtr node_cur = NULL, node_tmp = NULL;
    xmlAttrPtr attr_node = NULL;
    xmlNodePtr text_node = NULL;

    if(!(type = xmlGetProp(node, BAD_CAST "type"))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }

    if (!(node = virXPathNode("/interface/source", ctxt))) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR, XM_XML_ERROR);
        goto cleanup;
    }
    source = xmlGetProp(node, BAD_CAST type);

    if ((node = virXPathNode("/interface/mac", ctxt)))
        mac = xmlGetProp(node, BAD_CAST "address");
    if (!node || !mac) {
        if (!(mac = (xmlChar *)xenXMAutoAssignMac()))
            goto cleanup;
        autoassign = 1;
    }

    list_item = virConfGetValue(entry->conf, "vif");
    if (list_item && list_item->type == VIR_CONF_LIST) {
        prev = list_item;
        list_val = list_item->list;
        while (list_val) {
            if ((list_val->type != VIR_CONF_STRING) || (!list_val->str))
                goto skip;
            char dommac[18];
            char *key;

            dommac[0] = '\0';

            key = list_val->str;
            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');

                if (!(data = strchr(key, '=')) || (data[0] == '\0'))
                    goto skip;
                data++;

                if (!strncmp(key, "mac=", 4)) {
                    int len = nextkey ? (nextkey - data) : 17;
                    if (len > 17)
                        len = 17;
                    strncpy(dommac, data, len);
                    dommac[len] = '\0';
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }

            if (virMacAddrCompare (dommac, (const char *) mac) == 0) {
                if (autoassign) {
                    free(mac);
                    mac = NULL;
                    if (!(mac = (xmlChar *)xenXMAutoAssignMac()))
                        goto cleanup;
                    /* initialize the list */
                    list_item = virConfGetValue(entry->conf, "vif");
                    prev = list_item;
                    list_val = list_item->list;
                    continue;
                } else
                    break;
            }
        skip:
            prev = list_val;
            list_val = list_val->next;
        }
    } else if (!list_item) {
        if (!(list_item = calloc(1, sizeof(virConfValue))))
            goto cleanup;
        list_item->type = VIR_CONF_LIST;
        if(virConfSetValue(entry->conf, "vif", list_item)) {
            free(list_item);
            goto cleanup;
        }
        list_val = NULL;
        prev = list_item;
    } else
        goto cleanup;

    if ((node = virXPathNode("/interface", ctxt))) {
        if (autoassign) {
            node_cur = node->children;

            while (node_cur->next)
                node_cur = node_cur->next;

            if (!(node_tmp = calloc(1, sizeof(xmlNode))))
                goto node_cleanup;
            node_tmp->type = XML_ELEMENT_NODE;
            if (!(node_tmp->name = malloc(4)))
                goto node_cleanup;
            strcpy((char *)node_tmp->name, "mac");
            node_tmp->children = NULL;

            if (!(attr_node = calloc(1, sizeof(xmlAttr))))
                goto node_cleanup;
            attr_node->type = XML_ATTRIBUTE_NODE;
            attr_node->ns = NULL;
            if (!(attr_node->name = malloc(8)))
                goto node_cleanup;
            strcpy((char *) attr_node->name, "address");
            node_tmp->properties = attr_node;

            if (!(text_node = calloc(1, sizeof(xmlNode))))
                goto node_cleanup;
            text_node->type = XML_TEXT_NODE;
            text_node->_private = NULL;
            if (!(text_node->name = malloc(8)))
                goto node_cleanup;
            strcpy((char *) text_node->name, "text");
            text_node->children = NULL;
            text_node->parent = (xmlNodePtr)attr_node;
            text_node->content = mac;
            mac = NULL;
            attr_node->children = text_node;
            attr_node->last = text_node;
            attr_node->parent = node_tmp;

            node_cur->next = node_tmp;
        }
        if (!(dev = xenXMParseXMLVif(domain->conn, node, hvm)))
            goto cleanup;
    } else
        goto cleanup;

    if (!list_val) {
        /* insert */
        if (!(list_val = malloc(sizeof(virConfValue))))
            goto cleanup;
        list_val->type = VIR_CONF_STRING;
        list_val->next = NULL;
        list_val->str = dev;
        if (prev->type == VIR_CONF_LIST)
            prev->list = list_val;
        else
            prev->next = list_val;
    } else {
        /* configure */
        free(list_val->str);
        list_val->str = dev;
    }

    ret = 0;
    goto cleanup;

 node_cleanup:
    if (node_tmp)
        xmlFree(node_tmp);
    if (attr_node)
        xmlFree(attr_node);
    if (text_node)
        xmlFree(text_node);
 cleanup:
    free(type);
    free(source);
    free(mac);

    return (ret);
}

/**
 * xenXMAutoAssignMac:
 * @mac: pointer to Mac String
 *
 * a mac is assigned automatically.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
char *
xenXMAutoAssignMac() {
    char *buf;

    if (!(buf = malloc(18)))
        return 0;
    srand((unsigned)time(NULL));
    sprintf(buf, "00:16:3e:%02x:%02x:%02x"
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0)))
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0)))
            ,1 + (int)(256*(rand()/(RAND_MAX+1.0))));
    return buf;
}

/**
 * xenXMDomainDetachDevice:
 * @domain: pointer to domain object
 * @xml: pointer to XML description of device
 *
 * Destroy a virtual device attachment to backend.
 *
 * Returns 0 in case of success, -1 in case of failure.
 */
static int
xenXMDomainDetachDevice(virDomainPtr domain, const char *xml) {
    const char *filename = NULL;
    char device[8], *domdevice = NULL;
    xenXMConfCachePtr entry = NULL;
    virConfValuePtr prev = NULL, list_ptr = NULL, list_val = NULL;
    xmlDocPtr doc = NULL;
    xmlNodePtr node = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlChar *key = NULL;
    int ret = -1;

    if ((!domain) || (!domain->conn) || (!domain->name) || (!xml)) {
        xenXMError((domain ? domain->conn : NULL), VIR_ERR_INVALID_ARG,
                   __FUNCTION__);
        goto cleanup;
    }
    if (domain->conn->flags & VIR_CONNECT_RO)
        goto cleanup;
    if (domain->id != -1)
        goto cleanup;
    if (!(filename = virHashLookup(nameConfigMap, domain->name)))
        goto cleanup;

    doc = xmlReadDoc((const xmlChar *) xml, "device.xml", NULL,
                     XML_PARSE_NOENT | XML_PARSE_NONET |
                     XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        xenXMError(domain->conn, VIR_ERR_XML_ERROR,
                   _("cannot read XML domain definition"));
        goto cleanup;
    }
    if (!(ctxt = xmlXPathNewContext(doc))) {
        xenXMError(domain->conn, VIR_ERR_INTERNAL_ERROR,
                   _("cannot create XPath context"));
        goto cleanup;
    }

    if ((node = virXPathNode("/disk", ctxt))) {
        strcpy(device, "disk");
        if (!(node = virXPathNode("/disk/target", ctxt)))
            goto cleanup;
        key = xmlGetProp(node, BAD_CAST "dev");
    } else if ((node = virXPathNode("/interface", ctxt))) {
        strcpy(device, "vif");
        if (!(node = virXPathNode("/interface/mac", ctxt)))
            goto cleanup;
        key = xmlGetProp(node, BAD_CAST "address");
    } else
        goto cleanup;
    if (!key || (strlen((char *)key) == 0))
        goto cleanup;

    if (!(entry = virHashLookup(configCache, filename)))
        goto cleanup;
    if (!entry->conf)
        goto cleanup;

    list_ptr = virConfGetValue(entry->conf, device);
    if (!list_ptr)
        goto cleanup;
    else if (list_ptr && list_ptr->type == VIR_CONF_LIST) {
        list_val = list_ptr->list;
        while (list_val) {
            if (!(strcmp(device, "disk"))) {
                char domdev[NAME_MAX];
                char *head;
                char *offset;
                char *tmp;

                if ((list_val->type != VIR_CONF_STRING) || (!list_val->str))
                    goto skip;
                head = list_val->str;

                /* Extract the source */
                if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                    goto skip;
                if ((offset - head) >= (PATH_MAX-1))
                    goto skip;
                head = offset + 1;

                /* Extract the dest */
                if (!(offset = strchr(head, ',')) || offset[0] == '\0')
                    goto skip;
                if ((offset - head) >= (PATH_MAX-1))
                    goto skip;
                strncpy(domdev, head, (offset - head));
                domdev[(offset-head)] = '\0';
                head = offset + 1;

                /* Remove legacy ioemu: junk */
                if (!strncmp(domdev, "ioemu:", 6)) {
                    memmove(domdev, domdev+6, strlen(domdev)-5);
                }

                /* Check for a :cdrom/:disk postfix */
                if ((tmp = strchr(domdev, ':')))
                    tmp[0] = '\0';

                if (!(strcmp(domdev, (const char *) key)))
                    break;
            } else {
                char dommac[18];
                char *mac;

                dommac[0] = '\0';

                if ((list_val->type != VIR_CONF_STRING) || (!list_val->str))
                    goto skip;

                mac = list_val->str;
                while (mac) {
                    char *data;
                    char *nextmac = strchr(mac, ',');

                    if (!(data = strchr(mac, '=')) || (data[0] == '\0'))
                        goto skip;
                    data++;

                    if (!strncmp(mac, "mac=", 4)) {
                        int len = nextmac ? (nextmac - data) : 17;
                        if (len > 17)
                            len = 17;
                        strncpy(dommac, data, len);
                        dommac[len] = '\0';
                    }

                    while (nextmac && (nextmac[0] == ',' ||
                                       nextmac[0] == ' ' ||
                                       nextmac[0] == '\t'))
                        nextmac++;
                    mac = nextmac;
                }

                if (virMacAddrCompare (dommac, (const char *) key) == 0)
                    break;
            }
        skip:
            prev = list_val;
            list_val = list_val->next;
        }
    }

    if (!list_val)
        goto cleanup;
    else {
        if (!prev) {
            virConfValuePtr value;
            if (!(value = calloc(1, sizeof(virConfValue))))
                goto cleanup;
            value->type = VIR_CONF_LIST;
            value->list = list_val->next;
            list_val->next = NULL;
            if (virConfSetValue(entry->conf, device, value)) {
                free(value);
                goto cleanup;
            }
        } else
            prev->next = list_val->next;
    }

    /* If this fails, should we try to undo our changes to the
     * in-memory representation of the config file. I say not!
     */
    if (virConfWriteFile(entry->filename, entry->conf) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    xmlXPathFreeContext(ctxt);
    if (doc)
        xmlFreeDoc(doc);
    free(domdevice);
    free(key);
    free(list_val);

    return (ret);
}

#endif /* WITH_XEN */
