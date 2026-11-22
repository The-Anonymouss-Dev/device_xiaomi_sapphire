#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)

from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixups,
    lib_fixups_user_type,
)

from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/xiaomi/sapphire',
    'hardware/qcom-caf/wlan',
    'hardware/xiaomi',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/qcom/opensource/dataservices',
    'vendor/qcom/opensource/display',
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs): # fmt: skip
    return f'{lib}_{partition}' if partition == 'vendor' else None # fmt: skip


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,    
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'vendor.qti.diaghal@1.0',
        'vendor.qti.hardware.fm@1.0',
        'vendor.qti.hardware.qccsyshal@1.0',
        'vendor.qti.hardware.qccsyshal@1.1',
        'vendor.qti.hardware.qccsyshal@1.2',
        'vendor.qti.hardware.qccvndhal@1.0',
        'vendor.qti.imsrtpservice@3.0',
        'vendor.qti.imsrtpservice@3.1',
    ): lib_fixup_vendor_suffix,
}

blob_fixups: blob_fixups_user_type = {
    ('vendor/bin/hw/android.hardware.security.keymint-service-qti', 'vendor/lib64/libqtikeymint.so'): blob_fixup()
        .add_needed('android.hardware.security.rkp-V3-ndk.so'),
    'vendor/etc/qcril_database/upgrade/config/6.0_config.sql': blob_fixup()
        .binary_regex_replace(rb'persist\.vendor\.radio\.poweron_opt', rb'persist.vendor.radio.poweron_ign'),
    ('vendor/lib64/libqcrilNr.so', 'vendor/lib64/libril-db.so'): blob_fixup()
        .binary_regex_replace(rb'persist\.vendor\.radio\.poweron_opt', rb'persist.vendor.radio.poweron_ign'),
    'vendor/lib64/vendor.libdpmframework.so': blob_fixup()
        .add_needed('libhidlbase_shim.so'),
    'vendor/etc/seccomp_policy/c2audio.vendor.ext-arm64.policy': blob_fixup()
        .add_line_if_missing('setsockopt: 1'),
   ('vendor/lib64/hw/com.qti.chi.override.so',
    'vendor/lib64/libcamxcommonutils.so',
    'vendor/lib64/libmialgoengine.so',
    'vendor/lib64/hw/camera.qcom.so',): blob_fixup()
        .add_needed('libprocessgroup_shim.so'),
    'vendor/etc/init/android.hardware.gnss-aidl-service-qti.rc': blob_fixup()
        .regex_replace('group system gps radio vendor_qti_diag vendor_ssgtzd', 'group system gps radio vendor_qti_diag'),
    'vendor/etc/init/vendor.sensors.sscrpcd.rc': blob_fixup()
        .regex_replace('class early_hal', 'disabled'),
    'vendor/etc/init/vendor.sensors.qti.rc': blob_fixup()
        .regex_replace('class core', 'disabled'),
    'vendor/etc/vintf/manifest/c2_manifest_vendor.xml': blob_fixup()
        .regex_replace('.+DOLBY.+\n', ''),
    'vendor/bin/STFlashTool': blob_fixup()
        .add_needed('libbase_shim.so'),
    'system_ext/lib64/vendor.qti.hardware.qccsyshal@1.2-halimpl.so': blob_fixup()
        .replace_needed('libprotobuf-cpp-full.so', 'libprotobuf-cpp-full-fromvendor.so'),
    'system_ext/lib64/libprotobuf-cpp-full-fromvendor.so': blob_fixup()
        .replace_needed('libprotobuf-cpp-lite.so','libprotobuf-cpp-lite-fromvendor.so'),
    'vendor/etc/init/hw/init.qcom.usb.rc': blob_fixup()
        .regex_replace('on charger', 'on property:init.svc.vendor.charger=running'),
    # Same patch as device/xiaomi/miuicamera-sapphire: skip the UBSan abort
    # checks in the portrait bokeh node, so this copy matches the patched one.
    'vendor/lib64/camera/components/com.jigan.node.realtimebokeh.so': blob_fixup()
        .sig_replace('B1 02 40 B9 0A 02 11 6B 23 09 00 54',
                     'B1 02 40 B9 0A 02 11 6B 1F 20 03 D5')
        .sig_replace('B6 06 40 B9 0C 00 16 6B A3 08 00 54',
                     'B6 06 40 B9 0C 00 16 6B 1F 20 03 D5')
        .sig_replace('3F C1 29 EB 61 08 00 54',
                     '3F C1 29 EB 1F 20 03 D5')
        .sig_replace('6B 01 01 6B E6 07 00 54',
                     '6B 01 01 6B 1F 20 03 D5')
        .sig_replace('DF 7D 60 F2 61 07 00 54',
                     'DF 7D 60 F2 1F 20 03 D5')
        .sig_replace('C0 03 5F D6 E5 0A 00 94 D4 0A 00 94',
                     'C0 03 5F D6 F9 FF FF 17 F8 FF FF 17'),
}

module = ExtractUtilsModule(
    'sapphire',
    'xiaomi',
    blob_fixups=blob_fixups,
    check_elf=False,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
