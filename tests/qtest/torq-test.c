// Copyright 2026 Synaptics Inc.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/qos-smmuv3.h"
#include "qobject/qdict.h"

#define TORQ_BASE UINT64_C(0xf7600000)
#define SMMU_BASE UINT64_C(0xf7800000)

static QTestState *torq_start(void)
{
    return qtest_init("-machine astra_sl,memory-backend=ram -m 512M "
                       "-object memory-backend-memfd,id=ram,size=512M,share=on "
                       "-nodefaults -display none");
}

static void torq_registers(void)
{
    QTestState *qts = torq_start();

    qtest_writeq(qts, TORQ_BASE, UINT64_C(0x12345678abcdef01));
    g_assert_cmphex(qtest_readq(qts, TORQ_BASE), ==,
                    UINT64_C(0x12345678abcdef01));
    qtest_quit(qts);
}

static void torq_cdma(void)
{
    QTestState *qts = torq_start();

    qtest_writel(qts, TORQ_BASE, 0x89abcdef);
    qtest_writel(qts, TORQ_BASE + 0x1ff010, 0);
    qtest_writel(qts, TORQ_BASE + 0x1ff018, 4);
    qtest_writel(qts, TORQ_BASE + 0x1ff020, 0x00040004);
    qtest_writel(qts, TORQ_BASE + 0x1ff008, 1);
    qtest_writel(qts, TORQ_BASE + 0x1fe20c, 1);
    qtest_writel(qts, TORQ_BASE + 0x1fc004, 1 << 12);
    qtest_writel(qts, TORQ_BASE + 0x1ff000, 1);
    g_assert_cmphex(qtest_readl(qts, TORQ_BASE + 4), ==, 0x89abcdef);
    g_assert_true(qtest_readl(qts, TORQ_BASE + 0x1ff004) & (1U << 16));
    qtest_quit(qts);
}

static void torq_snapshot(bool alias)
{
    QTestState *qts = torq_start();
    uint64_t table = QSMMU_SPACE_OFFS_NS + QSMMU_L2_PTE_VAL;
    uint64_t target = QSMMU_SPACE_OFFS_NS + QSMMU_L3_PTE_VAL;
    uint8_t empty_tables[4 * 4096] = { 0 };
    QDict *response;

    g_assert_cmpuint(qsmmu_build_translation(qts, QSMMU_TM_S1_ONLY,
                                             QSMMU_SPACE_NONSECURE, 1), ==, 0);
    qtest_memwrite(qts, QSMMU_SPACE_OFFS_NS + QSMMU_VTTB,
                    empty_tables, sizeof(empty_tables));
    qtest_writeq(qts, QSMMU_SPACE_OFFS_NS + QSMMU_VTTB,
                  (QSMMU_SPACE_OFFS_NS + QSMMU_L0_PTE_VAL) |
                  QSMMU_NON_LEAF_PTE_MASK);
    qtest_writeq(qts, QSMMU_SPACE_OFFS_NS + QSMMU_L0_PTE_VAL,
                  (QSMMU_SPACE_OFFS_NS + QSMMU_L1_PTE_VAL) |
                  QSMMU_NON_LEAF_PTE_MASK);
    qtest_writeq(qts, QSMMU_SPACE_OFFS_NS + QSMMU_L1_PTE_VAL,
                  table | QSMMU_NON_LEAF_PTE_MASK);
    qtest_writeq(qts, table + sizeof(uint64_t),
                  target | QSMMU_LEAF_PTE_RW_MASK);
    if (alias) {
        qtest_writeq(qts, table + 2 * sizeof(uint64_t),
                      target | QSMMU_LEAF_PTE_RW_MASK);
    }
    qsmmu_program_regs(qts, SMMU_BASE, QSMMU_SPACE_NONSECURE);
    qtest_readl(qts, TORQ_BASE + 0x1f000c);
    response = qtest_qmp(qts, "{'execute': 'query-status'}");
    g_assert_nonnull(qdict_get_qdict(response, "return"));
    if (alias) {
        g_assert_cmpstr(qdict_get_str(qdict_get_qdict(response, "return"),
                                       "status"), ==, "io-error");
    } else {
        g_assert_cmpstr(qdict_get_str(qdict_get_qdict(response, "return"),
                                       "status"), !=, "io-error");
        g_assert_cmpuint(qtest_readl(qts, SMMU_BASE + 0x100a8), ==, 0);
    }
    qobject_unref(response);
    qtest_quit(qts);
}

static void torq_snapshot_valid(void)
{
    torq_snapshot(false);
}

static void torq_snapshot_alias(void)
{
    torq_snapshot(true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/torq/registers", torq_registers);
    qtest_add_func("/torq/cdma", torq_cdma);
    qtest_add_func("/torq/snapshot", torq_snapshot_valid);
    qtest_add_func("/torq/reject-alias", torq_snapshot_alias);
    return g_test_run();
}
