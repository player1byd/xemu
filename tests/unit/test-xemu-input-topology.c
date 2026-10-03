/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "ui/xemu-input.h"
#include "hw/xbox/xid.h"

static void test_host_detach_keeps_guest_topology(void)
{
    ControllerState physical = { .bound = -1 };
    ControllerState replacement = { .bound = -1 };
    int hub, gamepad, xmu;
    XemuVirtualControllerPort port = {
        .connected = true,
        .hub = &hub,
        .gamepad = &gamepad,
        .peripheral_types = { PERIPHERAL_XMU, PERIPHERAL_NONE },
        .peripherals = { &xmu, NULL },
    };

    g_assert_null(xemu_input_port_assign_provider(&port, &physical, 2));
    g_assert_cmpint(physical.bound, ==, 2);

    g_assert_true(xemu_input_port_assign_provider(&port, NULL, 2) == &physical);
    g_assert_cmpint(physical.bound, ==, -1);
    g_assert_true(port.connected);
    g_assert_true(port.hub == &hub);
    g_assert_true(port.gamepad == &gamepad);
    g_assert_true(port.peripherals[0] == &xmu);
    g_assert_cmpint(port.peripheral_types[0], ==, PERIPHERAL_XMU);

    g_assert_null(xemu_input_port_assign_provider(&port, &replacement, 2));
    g_assert_true(port.provider == &replacement);
    g_assert_cmpint(replacement.bound, ==, 2);
    g_assert_true(port.hub == &hub);
    g_assert_true(port.peripherals[0] == &xmu);
}

static void test_reassignment_does_not_touch_guest_devices(void)
{
    ControllerState first = { .bound = -1 };
    ControllerState second = { .bound = -1 };
    int hub;
    XemuVirtualControllerPort port = { .connected = true, .hub = &hub };

    xemu_input_port_assign_provider(&port, &first, 0);
    g_assert_true(xemu_input_port_assign_provider(&port, &second, 0) == &first);
    g_assert_cmpint(first.bound, ==, -1);
    g_assert_cmpint(second.bound, ==, 0);
    g_assert_true(port.connected);
    g_assert_true(port.hub == &hub);
}

static void test_legacy_presence_migration(void)
{
    g_assert_true(xemu_input_port_should_exist(-1, "keyboard"));
    g_assert_false(xemu_input_port_should_exist(-1, ""));
    g_assert_false(xemu_input_port_should_exist(-1, NULL));
    g_assert_true(xemu_input_port_should_exist(1, NULL));
    g_assert_false(xemu_input_port_should_exist(0, "keyboard"));

    g_assert_cmpint(xemu_input_normalize_virtual_presence(-100), ==, -1);
    g_assert_cmpint(xemu_input_normalize_virtual_presence(-1), ==, -1);
    g_assert_cmpint(xemu_input_normalize_virtual_presence(0), ==, 0);
    g_assert_cmpint(xemu_input_normalize_virtual_presence(1), ==, 1);
    g_assert_cmpint(xemu_input_normalize_virtual_presence(100), ==, -1);
}

static void test_absent_provider_preference_round_trip(void)
{
    XemuInputProviderPreference preference =
        xemu_input_provider_preference(true, true, NULL);
    char saved_identifier[16];
    int saved_presence = -1;

    if (preference.write_identifier) {
        g_strlcpy(saved_identifier, preference.identifier,
                  sizeof(saved_identifier));
    }
    if (preference.write_presence) {
        saved_presence = preference.presence;
    }

    g_assert_cmpstr(saved_identifier, ==, "");
    g_assert_cmpint(saved_presence, ==, 1);
    g_assert_true(xemu_input_port_should_exist(saved_presence,
                                               saved_identifier));
}

static void test_automatic_detach_preserves_preference(void)
{
    XemuInputProviderPreference preference =
        xemu_input_provider_preference(false, true, NULL);
    char saved_identifier[32] = "saved-physical-guid";
    int saved_presence = 1;

    if (preference.write_identifier) {
        g_strlcpy(saved_identifier, preference.identifier,
                  sizeof(saved_identifier));
    }
    if (preference.write_presence) {
        saved_presence = preference.presence;
    }

    g_assert_cmpstr(saved_identifier, ==, "saved-physical-guid");
    g_assert_cmpint(saved_presence, ==, 1);
}

typedef struct TopologyFixture TopologyFixture;

typedef struct TopologyFixtureDevice {
    TopologyFixture *owner;
} TopologyFixtureDevice;

struct TopologyFixture {
    TopologyFixtureDevice hub;
    TopologyFixtureDevice gamepad;
    bool fail_hub;
    bool fail_gamepad;
    bool fail_remove;
    unsigned int create_hub_calls;
    unsigned int create_gamepad_calls;
    unsigned int remove_hub_calls;
    unsigned int release_calls;
};

static void *fixture_create_hub(void *opaque, Error **errp)
{
    TopologyFixture *fixture = opaque;
    fixture->create_hub_calls++;
    if (fixture->fail_hub) {
        error_setg(errp, "hub failure");
        return NULL;
    }
    return &fixture->hub;
}

static void *fixture_create_gamepad(void *opaque, Error **errp)
{
    TopologyFixture *fixture = opaque;
    fixture->create_gamepad_calls++;
    if (fixture->fail_gamepad) {
        error_setg(errp, "gamepad failure");
        return NULL;
    }
    return &fixture->gamepad;
}

static bool fixture_remove_hub(void *opaque, void *hub, Error **errp)
{
    TopologyFixture *fixture = opaque;
    g_assert_true(hub == &fixture->hub);
    fixture->remove_hub_calls++;
    if (fixture->fail_remove) {
        error_setg(errp, "remove failure");
        return false;
    }
    return true;
}

static void fixture_release_device(void *device)
{
    TopologyFixtureDevice *fixture_device = device;
    fixture_device->owner->release_calls++;
}

static const XemuInputTopologyOps topology_ops = {
    .create_hub = fixture_create_hub,
    .create_gamepad = fixture_create_gamepad,
    .remove_hub = fixture_remove_hub,
    .release_device = fixture_release_device,
};

static void test_topology_construction_success(void)
{
    TopologyFixture fixture = { 0 };
    XemuVirtualControllerPort port = { 0 };
    Error *err = NULL;
    fixture.hub.owner = fixture.gamepad.owner = &fixture;

    g_assert_true(xemu_input_create_virtual_devices(
        &port, &topology_ops, &fixture, &err));
    g_assert_null(err);
    g_assert_true(port.connected);
    g_assert_true(port.hub == &fixture.hub);
    g_assert_true(port.gamepad == &fixture.gamepad);
    g_assert_cmpuint(fixture.create_hub_calls, ==, 1);
    g_assert_cmpuint(fixture.create_gamepad_calls, ==, 1);
    g_assert_cmpuint(fixture.remove_hub_calls, ==, 0);
    g_assert_cmpuint(fixture.release_calls, ==, 2);
}

static void test_topology_hub_failure_is_recoverable(void)
{
    TopologyFixture fixture = { .fail_hub = true };
    XemuVirtualControllerPort port = { 0 };
    Error *err = NULL;
    fixture.hub.owner = fixture.gamepad.owner = &fixture;

    g_assert_false(xemu_input_create_virtual_devices(
        &port, &topology_ops, &fixture, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==, "hub failure");
    error_free(err);
    g_assert_false(port.connected);
    g_assert_null(port.hub);
    g_assert_null(port.gamepad);
    g_assert_cmpuint(fixture.create_gamepad_calls, ==, 0);
    g_assert_cmpuint(fixture.release_calls, ==, 0);
}

static void test_topology_gamepad_failure_rolls_back(void)
{
    TopologyFixture fixture = { .fail_gamepad = true };
    XemuVirtualControllerPort port = { 0 };
    Error *err = NULL;
    fixture.hub.owner = fixture.gamepad.owner = &fixture;

    g_assert_false(xemu_input_create_virtual_devices(
        &port, &topology_ops, &fixture, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==, "gamepad failure");
    error_free(err);
    g_assert_false(port.connected);
    g_assert_null(port.hub);
    g_assert_null(port.gamepad);
    g_assert_cmpuint(fixture.remove_hub_calls, ==, 1);
    g_assert_cmpuint(fixture.release_calls, ==, 1);
}

static void test_topology_failed_rollback_retains_identity(void)
{
    TopologyFixture fixture = {
        .fail_gamepad = true,
        .fail_remove = true,
    };
    XemuVirtualControllerPort port = { 0 };
    Error *err = NULL;
    fixture.hub.owner = fixture.gamepad.owner = &fixture;

    g_assert_false(xemu_input_create_virtual_devices(
        &port, &topology_ops, &fixture, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "hub rollback failed"));
    error_free(err);
    g_assert_false(port.connected);
    g_assert_true(port.hub == &fixture.hub);
    g_assert_null(port.gamepad);
    g_assert_cmpuint(fixture.release_calls, ==, 1);
}

typedef struct XmuFixture {
    int drive;
    int device;
    bool fail_drive;
    bool fail_device;
    unsigned int drive_calls;
    unsigned int device_calls;
    unsigned int remove_drive_calls;
} XmuFixture;

static void *fixture_create_drive(void *opaque, Error **errp)
{
    XmuFixture *fixture = opaque;
    fixture->drive_calls++;
    if (fixture->fail_drive) {
        error_setg(errp, "drive failure");
        return NULL;
    }
    return &fixture->drive;
}

static void *fixture_create_storage(void *opaque, void *drive, Error **errp)
{
    XmuFixture *fixture = opaque;
    g_assert_true(drive == &fixture->drive);
    fixture->device_calls++;
    if (fixture->fail_device) {
        error_setg(errp, "storage failure");
        return NULL;
    }
    return &fixture->device;
}

static void fixture_remove_drive(void *opaque, void *drive)
{
    XmuFixture *fixture = opaque;
    g_assert_true(drive == &fixture->drive);
    fixture->remove_drive_calls++;
}

static const XemuInputXmuOps xmu_ops = {
    .create_drive = fixture_create_drive,
    .create_device = fixture_create_storage,
    .remove_drive = fixture_remove_drive,
};

static void test_xmu_drive_failure_is_recoverable(void)
{
    XmuFixture fixture = { .fail_drive = true };
    Error *err = NULL;
    void *device = NULL;

    g_assert_false(xemu_input_create_xmu_device(&xmu_ops, &fixture, &device,
                                                &err));
    g_assert_cmpstr(error_get_pretty(err), ==, "drive failure");
    error_free(err);
    g_assert_null(device);
    g_assert_cmpuint(fixture.device_calls, ==, 0);
    g_assert_cmpuint(fixture.remove_drive_calls, ==, 0);
}

static void test_xmu_storage_failure_removes_drive(void)
{
    XmuFixture fixture = { .fail_device = true };
    Error *err = NULL;
    void *device = NULL;

    g_assert_false(xemu_input_create_xmu_device(&xmu_ops, &fixture, &device,
                                                &err));
    g_assert_cmpstr(error_get_pretty(err), ==, "storage failure");
    error_free(err);
    g_assert_null(device);
    g_assert_cmpuint(fixture.remove_drive_calls, ==, 1);
}

static void test_xmu_construction_success(void)
{
    XmuFixture fixture = { 0 };
    Error *err = NULL;
    void *device = NULL;

    g_assert_true(xemu_input_create_xmu_device(&xmu_ops, &fixture, &device,
                                               &err));
    g_assert_null(err);
    g_assert_true(device == &fixture.device);
    g_assert_cmpuint(fixture.drive_calls, ==, 1);
    g_assert_cmpuint(fixture.device_calls, ==, 1);
    g_assert_cmpuint(fixture.remove_drive_calls, ==, 0);
}

static void test_providerless_xid_report_is_neutral(void)
{
    XIDGamepadReport report;
    memset(&report, 0xff, sizeof(report));

    xid_gamepad_report_neutral(&report);

    g_assert_cmpuint(sizeof(report), ==, 20);
    g_assert_cmpuint(report.bReportId, ==, 0);
    g_assert_cmpuint(report.bLength, ==, 20);
    g_assert_cmpuint(report.wButtons, ==, 0);
    for (size_t i = 0; i < sizeof(report.bAnalogButtons); i++) {
        g_assert_cmpuint(report.bAnalogButtons[i], ==, 0);
    }
    g_assert_cmpint(report.sThumbLX, ==, 0);
    g_assert_cmpint(report.sThumbLY, ==, 0);
    g_assert_cmpint(report.sThumbRX, ==, 0);
    g_assert_cmpint(report.sThumbRY, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xemu/input/host-detach-keeps-guest-topology",
                    test_host_detach_keeps_guest_topology);
    g_test_add_func("/xemu/input/provider-reassignment",
                    test_reassignment_does_not_touch_guest_devices);
    g_test_add_func("/xemu/input/legacy-presence-migration",
                    test_legacy_presence_migration);
    g_test_add_func("/xemu/input/absent-provider-preference-round-trip",
                    test_absent_provider_preference_round_trip);
    g_test_add_func("/xemu/input/automatic-detach-preserves-preference",
                    test_automatic_detach_preserves_preference);
    g_test_add_func("/xemu/input/providerless-xid-report",
                    test_providerless_xid_report_is_neutral);
    g_test_add_func("/xemu/input/topology-construction-success",
                    test_topology_construction_success);
    g_test_add_func("/xemu/input/topology-hub-failure",
                    test_topology_hub_failure_is_recoverable);
    g_test_add_func("/xemu/input/topology-gamepad-failure",
                    test_topology_gamepad_failure_rolls_back);
    g_test_add_func("/xemu/input/topology-rollback-failure",
                    test_topology_failed_rollback_retains_identity);
    g_test_add_func("/xemu/input/xmu-drive-failure",
                    test_xmu_drive_failure_is_recoverable);
    g_test_add_func("/xemu/input/xmu-storage-failure",
                    test_xmu_storage_failure_removes_drive);
    g_test_add_func("/xemu/input/xmu-construction-success",
                    test_xmu_construction_success);
    return g_test_run();
}
