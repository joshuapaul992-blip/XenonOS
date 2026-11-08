/* SPDX-License-Identifier: MIT */
/*
 * HAL - ACPI static tables.
 *
 * Reads the tables the kernel needs without an AML interpreter:
 *   MADT ("APIC")  CPUs, local APIC, IOAPICs, ISA interrupt overrides
 *   FADT ("FACP")  PM timer, CMOS century register, legacy device flags
 *   HPET           high precision event timer address
 *   MCFG           PCI Express configuration space (ECAM) regions
 */
#include "hal.h"
#include "mm.h"
#include "ke.h"
#include "rtl.h"
#include "loader.h"
#include "console.h"

#define MAX_TABLES 64

HAL_ACPI_INFO HalAcpi;
static ACPI_TABLE_HEADER *g_tables[MAX_TABLES];
static unsigned g_table_count;

typedef struct PACKED {
    char     Signature[8];
    uint8_t  Checksum;
    char     OemId[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t  ExtendedChecksum;
    uint8_t  Reserved[3];
} ACPI_RSDP;

static bool checksum_ok(const void *p, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += ((const uint8_t *)p)[i];
    return sum == 0;
}

static ACPI_TABLE_HEADER *map_table(uint64_t pa)
{
    ACPI_TABLE_HEADER *h = MmMapIoSpace(pa, sizeof(ACPI_TABLE_HEADER), MmCached);
    if (!h)
        return NULL;
    uint32_t len = h->Length;
    if (len < sizeof(ACPI_TABLE_HEADER) || len > (16u << 20))
        return NULL;
    if (len > PAGE_SIZE - (pa & (PAGE_SIZE - 1))) {
        MmUnmapIoSpace(h, sizeof(ACPI_TABLE_HEADER));
        h = MmMapIoSpace(pa, len, MmCached);
    }
    return h;
}

ACPI_TABLE_HEADER *HalAcpiFindTable(const char sig[4], unsigned n)
{
    for (unsigned i = 0; i < g_table_count; i++)
        if (!memcmp(g_tables[i]->Signature, sig, 4) && n-- == 0)
            return g_tables[i];
    return NULL;
}

HAL_IRQ_OVERRIDE HalAcpiIsaIrq(uint8_t irq)
{
    for (unsigned i = 0; i < HalAcpi.OverrideCount; i++)
        if (HalAcpi.Overrides[i].Source == irq)
            return HalAcpi.Overrides[i];
    return (HAL_IRQ_OVERRIDE){ .Source = irq, .Gsi = irq, .ActiveLow = false, .LevelTriggered = false };
}

static void parse_madt(ACPI_TABLE_HEADER *t)
{
    uint8_t *p = (uint8_t *)t;
    HalAcpi.LocalApicPhysical = *(uint32_t *)(p + 36);
    HalAcpi.HasLegacyPic = *(uint32_t *)(p + 40) & 1;

    for (uint32_t off = 44; off + 2 <= t->Length;) {
        uint8_t type = p[off], len = p[off + 1];
        if (len < 2 || off + len > t->Length)
            break;
        uint8_t *e = p + off;
        switch (type) {
        case 0: {   /* processor local APIC */
            uint32_t flags = *(uint32_t *)(e + 4);
            if ((flags & 3) && HalAcpi.CpuCount < HAL_MAX_CPUS)
                HalAcpi.CpuApicIds[HalAcpi.CpuCount++] = e[3];
            break;
        }
        case 9: {   /* processor local x2APIC */
            uint32_t flags = *(uint32_t *)(e + 8);
            if ((flags & 3) && HalAcpi.CpuCount < HAL_MAX_CPUS)
                HalAcpi.CpuApicIds[HalAcpi.CpuCount++] = *(uint32_t *)(e + 4);
            break;
        }
        case 1:     /* IOAPIC */
            if (HalAcpi.IoApicCount < HAL_MAX_IOAPICS) {
                unsigned i = HalAcpi.IoApicCount++;
                HalAcpi.IoApics[i].Id = e[2];
                HalAcpi.IoApics[i].Physical = *(uint32_t *)(e + 4);
                HalAcpi.IoApics[i].GsiBase = *(uint32_t *)(e + 8);
            }
            break;
        case 2:     /* interrupt source override */
            if (HalAcpi.OverrideCount < HAL_MAX_OVERRIDES) {
                uint16_t flags = *(uint16_t *)(e + 8);
                HalAcpi.Overrides[HalAcpi.OverrideCount++] = (HAL_IRQ_OVERRIDE){
                    .Source = e[3],
                    .Gsi = *(uint32_t *)(e + 4),
                    .ActiveLow = (flags & 3) == 3,
                    .LevelTriggered = ((flags >> 2) & 3) == 3,
                };
            }
            break;
        case 5:     /* local APIC address override */
            HalAcpi.LocalApicPhysical = *(uint64_t *)(e + 4);
            break;
        default:
            break;
        }
        off += len;
    }
}

static void parse_fadt(ACPI_TABLE_HEADER *t)
{
    uint8_t *p = (uint8_t *)t;
    HalAcpi.HasFadt = true;
    if (t->Length >= 80)
        HalAcpi.PmTimerPort = *(uint32_t *)(p + 76);
    if (t->Length >= 109)
        HalAcpi.CenturyRegister = p[108];
    if (t->Length >= 111 && t->Revision >= 2)
        HalAcpi.BootArchFlags = *(uint16_t *)(p + 109);
    if (t->Length >= 116)
        HalAcpi.PmTimer32Bit = (*(uint32_t *)(p + 112) >> 8) & 1;
    /* Prefer the 64-bit X_PM_TMR_BLK if it describes an I/O port. */
    if (t->Length >= 220 && p[208] == 1 && *(uint64_t *)(p + 212))
        HalAcpi.PmTimerPort = (uint32_t)*(uint64_t *)(p + 212);
}

static void parse_hpet(ACPI_TABLE_HEADER *t)
{
    uint8_t *p = (uint8_t *)t;
    if (t->Length >= 52 && p[40] == 0)          /* address space: system memory */
        HalAcpi.HpetPhysical = *(uint64_t *)(p + 44);
}

static void parse_mcfg(ACPI_TABLE_HEADER *t)
{
    uint8_t *p = (uint8_t *)t;
    for (uint32_t off = 44; off + 16 <= t->Length && HalAcpi.EcamCount < HAL_MAX_ECAM; off += 16) {
        HAL_ECAM_REGION *r = &HalAcpi.Ecam[HalAcpi.EcamCount];
        r->Base = *(uint64_t *)(p + off);
        r->Segment = *(uint16_t *)(p + off + 8);
        r->StartBus = p[off + 10];
        r->EndBus = p[off + 11];
        if (r->Base)
            HalAcpi.EcamCount++;
    }
}

void HalInitAcpi(void)
{
    uint64_t rsdp_pa = KeLoaderBlock.RsdpPhysical;
    if (!rsdp_pa) {
        DbgPrint("ACPI: no RSDP - running without ACPI");
        return;
    }
    ACPI_RSDP *rsdp = MmMapIoSpace(rsdp_pa, sizeof(ACPI_RSDP), MmCached);
    if (!rsdp || memcmp(rsdp->Signature, "RSD PTR ", 8) || !checksum_ok(rsdp, 20)) {
        DbgPrint("ACPI: RSDP at 0x%llx is invalid", (unsigned long long)rsdp_pa);
        return;
    }
    HalAcpi.Revision = rsdp->Revision;
    memcpy(HalAcpi.OemId, rsdp->OemId, 6);
    HalAcpi.OemId[6] = 0;

    bool xsdt = rsdp->Revision >= 2 && rsdp->XsdtAddress && checksum_ok(rsdp, rsdp->Length);
    ACPI_TABLE_HEADER *root = map_table(xsdt ? rsdp->XsdtAddress : rsdp->RsdtAddress);
    if (!root || !checksum_ok(root, root->Length)) {
        DbgPrint("ACPI: %s is missing or damaged", xsdt ? "XSDT" : "RSDT");
        return;
    }
    HalAcpi.Present = true;

    unsigned entry_size = xsdt ? 8 : 4;
    unsigned entries = (root->Length - sizeof(ACPI_TABLE_HEADER)) / entry_size;
    uint8_t *list = (uint8_t *)(root + 1);
    char names[MAX_TABLES * 5 + 1];
    size_t pos = 0;

    for (unsigned i = 0; i < entries && g_table_count < MAX_TABLES; i++) {
        uint64_t pa = xsdt ? *(uint64_t *)(list + i * 8) : *(uint32_t *)(list + i * 4);
        ACPI_TABLE_HEADER *t = pa ? map_table(pa) : NULL;
        if (!t)
            continue;
        if (!checksum_ok(t, t->Length)) {
            DbgPrint("ACPI: table %.4s at 0x%llx has a bad checksum - ignored", t->Signature,
                     (unsigned long long)pa);
            continue;
        }
        g_tables[g_table_count++] = t;
        pos += snprintf(names + pos, sizeof(names) - pos, " %.4s", t->Signature);
    }
    HalAcpi.TableCount = g_table_count;

    ACPI_TABLE_HEADER *t;
    if ((t = HalAcpiFindTable("APIC", 0))) parse_madt(t);
    if ((t = HalAcpiFindTable("FACP", 0))) parse_fadt(t);
    if ((t = HalAcpiFindTable("HPET", 0))) parse_hpet(t);
    if ((t = HalAcpiFindTable("MCFG", 0))) parse_mcfg(t);

    DbgPrint("ACPI: revision %u, OEM \"%s\", %u tables:%s", HalAcpi.Revision, HalAcpi.OemId,
             g_table_count, names);
}
