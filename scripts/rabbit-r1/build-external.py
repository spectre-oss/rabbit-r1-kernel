#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build audited external-module groups locally; never install or load them.

Only the explicitly listed audited groups are supported. This is not yet the
complete R1 hardware module set. Keep build outputs private during review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('group', choices=('audio', 'core-helpers', 'board', 'cpu', 'display', 'bluetooth-voice', 'storage', 'xfrm', 'rfcomm', 'jpeg-helpers', 'media', 'ion', 'powervr', 'connectivity', 'modem', 'video-decoder', 'video', 'bluetooth-transport', 'gps', 'camera'))
    parser.add_argument('kernel_output', type=Path)
    parser.add_argument('module_output', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    receiver = parser.add_mutually_exclusive_group()
    receiver.add_argument('--receiver-table', type=Path)
    receiver.add_argument('--receiver-calibration', type=Path)
    parser.add_argument('--receiver-sha256')
    parser.add_argument('--encoder-modes', type=Path)
    parser.add_argument('--encoder-sha256')
    args = parser.parse_args()
    if args.group == 'video':
        if args.encoder_modes is None and args.encoder_sha256 is None:
            args.encoder_modes = ROOT / 'external/rabbit-r1/video/encoder-modes.json'
            args.encoder_sha256 = 'c73a15dd1c862c52f6fb3a522d544becb3046f572ad6ce7286d38fb70bb3306d'
        elif args.encoder_modes is None or args.encoder_sha256 is None:
            parser.error('Override encoder modes requires both input and recorded SHA256')
    if args.group == 'camera' and ((args.receiver_table is None and args.receiver_calibration is None) or args.receiver_sha256 is None):
        parser.error('Camera requires a private receiver table or calibration JSON and its SHA256')
    if args.jobs < 1 or not args.kernel_output.is_absolute() or not args.module_output.is_absolute():
        parser.error('Use absolute output paths and a positive job count')
    kernel, output = args.kernel_output.resolve(), args.module_output.resolve()
    if output.is_relative_to(ROOT) or output.is_relative_to(kernel):
        parser.error('Keep module output outside the source and prepared kernel directories')
    if output.exists() or args.module_output.is_symlink():
        parser.error('Module output must be a new directory')
    if digest(kernel / '.config') != digest(ROOT / 'Documentation/rabbit-r1/kernel49.config'):
        parser.error('Prepared kernel configuration differs from the recorded target')
    if (kernel / 'include/config/kernel.release').read_text().strip() != '7.1.0-rabbit-r1+':
        parser.error('Prepared kernel release differs from the recorded target')
    if not (kernel / 'Module.symvers').is_file():
        parser.error('Build the kernel and its modules before external groups')
    output.mkdir(parents=True)
    inputs = []

    def copy(source, target):
        target = output / target
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / source, target)
        inputs.append({'source': str(source), 'target': str(target.relative_to(output)),
                       'sha256': digest(target)})

    config = []
    if args.group == 'audio':
        for source in sorted((ROOT / 'sound/core').iterdir()):
            if source.suffix in ('.c', '.h') or source.name == 'Makefile':
                copy(source.relative_to(ROOT), Path('core') / source.name)
        copy(Path('sound/sound_core.c'), 'sound_core.c')
        for name in ('r1_audio.c', 'r1-audio-analog.h'):
            copy(Path('external/rabbit-r1/audio') / name, name)
        kbuild = ('obj-m += soundcore.o core/ r1_audio.o\nsoundcore-y := sound_core.o\n'
                  'subdir-ccflags-y += -DCONFIG_SND=1 -DCONFIG_SND_PCM=1 '
                  '-DCONFIG_SND_TIMER=1 -DCONFIG_SND_PROC_FS=1 -DCONFIG_SND_PCM_TIMER=1\n')
        config = ['CONFIG_SND=m', 'CONFIG_SND_PCM=m', 'CONFIG_SND_TIMER=m',
                  'CONFIG_SND_PROC_FS=y', 'CONFIG_SND_PCM_TIMER=y']
    elif args.group == 'core-helpers':
        copy(Path('drivers/iio/adc/mt6359-auxadc.c'), 'mt6359_auxadc.c')
        for name in ('r1_vcodec_owner.c', 'r1_vcodec_owner.h'):
            copy(Path('drivers/media/platform/mediatek/jpeg') / name, name)
        kbuild = 'obj-m += mt6359_auxadc.o r1-vcodec-owner.o\nr1-vcodec-owner-y := r1_vcodec_owner.o\n'
    elif args.group == 'modem':
        directory = ROOT / 'external/rabbit-r1/modem'
        for source in sorted(directory.rglob('*')):
            if source.is_file():
                copy(source.relative_to(ROOT), source.relative_to(directory))
        kbuild = (output / 'Makefile').read_text()
        config = ['LLVM=1', 'CONFIG_CC_IS_CLANG=y', 'CONFIG_CC_IS_GCC=',
                  'CONFIG_GCC_VERSION=0', 'CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=',
                  'CONFIG_CC_IMPLICIT_FALLTHROUGH=-Wimplicit-fallthrough']
    elif args.group in ('connectivity', 'bluetooth-transport', 'gps'):
        directory = ROOT / 'external/rabbit-r1/connectivity'
        for source in sorted(directory.rglob('*')):
            if source.is_file():
                copy(source.relative_to(ROOT), source.relative_to(directory))
        kbuild = 'obj-m += btif/ wmt/\n'
        if args.group == 'bluetooth-transport':
            directory = ROOT / 'external/rabbit-r1/bluetooth-transport'
            for source in sorted(directory.rglob('*')):
                if source.is_file():
                    relative = source.relative_to(directory)
                    target = Path('bt', *relative.parts[1:]) if relative.parts[0] == 'wmt' else relative
                    copy(source.relative_to(ROOT), target)
            kbuild += 'obj-m += bt/\n'

        if args.group == 'gps':
            directory = ROOT / 'external/rabbit-r1/gps'
            for source in sorted(directory.rglob('*')):
                if source.is_file():
                    copy(source.relative_to(ROOT), source.relative_to(directory))
            with (output / 'wmt/Kbuild').open('a') as stream:
                stream.write('\nccflags-y += -DCONFIG_MTK_COMBO_GPS=1\nwmt_drv-objs += r1_gps_lna.o\n')
            kbuild += 'obj-m += driver/\n'

        config = ['LLVM=1', 'CONFIG_CC_IS_CLANG=y', 'CONFIG_CC_IS_GCC=',
                  'CONFIG_GCC_VERSION=0', 'CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=',
                  'CONFIG_CC_IMPLICIT_FALLTHROUGH=-Wimplicit-fallthrough',
                  'CONFIG_MTK_COMBO=y', 'CONFIG_MTK_COMBO_CHIP_CONSYS_6765=y',
                  'MTK_PLATFORM=mt6765', 'MTK_CONSYS_ADIE=MT6631',
                  'CONFIG_MTK_BTIF=m', 'TARGET_BUILD_VARIANT=user']
        if args.group == 'bluetooth-transport':
            config.append('BT_PLATFORM=connac1x')
        if args.group == 'gps':
            config.extend(['CONFIG_MTK_GPS_SUPPORT=y', 'CONFIG_MTK_PLATFORM=mt6765'])
    elif args.group == 'powervr':
        directory = ROOT / 'external/rabbit-r1/powervr'
        for source in sorted(directory.rglob('*')):
            if source.is_file():
                copy(source.relative_to(ROOT), source.relative_to(directory))
        kbuild = (output / 'Makefile').read_text()
        config = ['CONFIG_MTK_GPU_SUPPORT=m', 'MTK_PLATFORM=mt6765']
    elif args.group == 'ion':
        for name in ('r1_ion.c', 'r1_system_heap.c'):
            copy(Path('external/rabbit-r1/ion') / name, name)
        kbuild = 'obj-m += r1_ion.o\n'
    elif args.group in ('media', 'video-decoder', 'video'):
        directories = {'mc': 'drivers/media/mc', 'v4l2': 'drivers/media/v4l2-core',
                       'vb2': 'drivers/media/common/videobuf2',
                       'jpeg': 'drivers/media/platform/mediatek/jpeg'}
        for target, directory in directories.items():
            for source in sorted((ROOT / directory).iterdir()):
                if source.suffix in ('.c', '.h') or source.name == 'Makefile':
                    copy(source.relative_to(ROOT), Path(target) / source.name)
        jpeg_makefile = output / 'jpeg/Makefile'
        with jpeg_makefile.open('a') as stream:
            for name in ('mtk_jpeg_core', 'mtk_jpeg_dec_parse', 'r1_vcodec_owner'):
                stream.write(f'\nCFLAGS_{name}.o += -fmacro-prefix-map={ROOT}=/tmp/r1-jpeg-kernel '
                             '-fmacro-prefix-map=jpeg/=\n')
        copy(Path('drivers/iommu/mtk_iommu.c'), 'mtk_iommu.c')
        copy(Path('drivers/memory/mtk-smi.c'), 'mtk-smi.c')
        values = {'MEDIA_CONTROLLER': 'y', 'MEDIA_PLATFORM_DRIVERS': 'y',
                  'MEDIA_PLATFORM_SUPPORT': 'y', 'MEDIA_SUPPORT': 'm',
                  'MEDIA_SUPPORT_FILTER': 'y', 'MTK_IOMMU': 'm', 'MTK_SMI': 'm',
                  'V4L2_MEM2MEM_DEV': 'm', 'V4L_MEM2MEM_DRIVERS': 'y',
                  'VIDEOBUF2_CORE': 'm', 'VIDEOBUF2_DMA_CONTIG': 'm',
                  'VIDEOBUF2_MEMOPS': 'm', 'VIDEOBUF2_V4L2': 'm',
                  'VIDEO_DEV': 'm', 'VIDEO_MEDIATEK_JPEG': 'm', 'VIDEO_V4L2_I2C': 'y'}
        if args.group in ('video-decoder', 'video'):
            for source in sorted((ROOT / 'external/rabbit-r1/video').iterdir()):
                if source.is_file():
                    copy(source.relative_to(ROOT), source.name)
            copy(Path('drivers/media/platform/mediatek/jpeg/r1_vcodec_owner.h'), 'r1_vcodec_owner.h')
        config = [f'CONFIG_{key}={value}' for key, value in values.items()]
        definitions = [f'-DCONFIG_{key}' + ('_MODULE' if value == 'm' else '') + '=1'
                       for key, value in values.items()]
        kbuild = ('obj-m += mc/ v4l2/ vb2/ jpeg/ mtk_iommu.o mtk-smi.o\n'
                  'subdir-ccflags-y += ' + ' '.join(definitions) + '\n')
        if args.group in ('video-decoder', 'video'):
            kbuild += 'obj-m += r1_vdec_probe.o r1_venc_nodes.o\n'
            kbuild += f'CFLAGS_r1_vdec_probe.o += -fmacro-prefix-map={ROOT}=/tmp/r1-jpeg-kernel\n'
        if args.group == 'video':
            subprocess.run(['python3', str(ROOT / 'scripts/rabbit-r1/prepare-encoder-modes.py'),
                            str(args.encoder_modes.resolve()), str(output),
                            '--expected-sha256', args.encoder_sha256], check=True)
            inputs.append({'source': ('external/rabbit-r1/video/encoder-modes.json'
                                      if args.encoder_modes.resolve() == (ROOT / 'external/rabbit-r1/video/encoder-modes.json').resolve()
                                      else 'external encoder mode override (not bundled)'),
                           'input_sha256': args.encoder_sha256,
                           'sha256': digest(output / 'include/generated/rabbit-r1-encoder-modes.h'),
                           'target': 'include/generated/rabbit-r1-encoder-modes.h'})
            kbuild += ('obj-m += r1_venc.o\nccflags-y += -I$(src)/include\n'
                       f'CFLAGS_r1_venc.o += -fmacro-prefix-map={ROOT}=/tmp/r1-jpeg-kernel\n')

    elif args.group == 'jpeg-helpers':
        names = ('nodes', 'iommu_nodes', 'power_nodes', 'power_clocks',
                 'power_links', 'selector_nodes')
        for name in names:
            copy(Path('external/rabbit-r1/jpeg-helpers') / f'r1_jpeg_{name}.c',
                 f'r1_jpeg_{name}.c')
        copy(Path('external/rabbit-r1/jpeg-helpers/vcodec-power.dts'), 'vcodec-power.dts')
        subprocess.run(['dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o',
                        str(output / 'power.dtbo'), str(output / 'vcodec-power.dts')], check=True)
        blob = (output / 'power.dtbo').read_bytes()
        rows = [', '.join(f'0x{x:02x}' for x in blob[i:i+12]) for i in range(0, len(blob), 12)]
        header = output / 'vcodec-power-blob.h'
        header.write_text('/* Generated from vcodec-power.dts; do not edit. */\n'
                          'static const unsigned char vcodec_power_blob[] __aligned(8) = {\n\t'
                          + ',\n\t'.join(rows) + '\n};\n')
        inputs.append({'source': 'generated power overlay', 'target': header.name,
                       'sha256': digest(header)})
        kbuild = 'obj-m += ' + ' '.join(f'r1_jpeg_{name}.o' for name in names) + '\n'
    elif args.group == 'xfrm':
        copy(Path('net/xfrm/xfrm_user.c'), 'xfrm_user.c')
        kbuild = 'obj-m += xfrm_user.o\n'
    elif args.group == 'rfcomm':
        copy(Path('external/rabbit-r1/rfcomm/historical-config.h'), 'historical-config.h')
        for name in ('core.c', 'sock.c', 'tty.c'):
            copy(Path('net/bluetooth/rfcomm') / name, name)
        kbuild = ('obj-m := rfcomm.o\nrfcomm-y := core.o sock.o tty.o\n'
                  'ccflags-y += -DCONFIG_BT_RFCOMM_TTY=1\n')
        config = ['LLVM=1', 'CONFIG_CC_IS_CLANG=y', 'CONFIG_CC_IS_GCC=',
                  'CONFIG_GCC_VERSION=0', 'CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=',
                  'CONFIG_CC_IMPLICIT_FALLTHROUGH=-Wimplicit-fallthrough']
    elif args.group == 'storage':
        copy(Path('external/rabbit-r1/storage/r1_emmc.c'), 'r1_emmc.c')
        for name in ('cqhci.h', 'mmc_hsq.h'):
            copy(Path('drivers/mmc/host') / name, name)
        copy(Path('drivers/mmc/host/mtk-sd.c'), 'r1_msdc.c')
        source = (output / 'r1_msdc.c').read_text()
        start = source.index('static const struct mtk_mmc_compatible mt6765_compat')
        end = source.index('\n};', start)
        if '.support_64g = true' not in source[start:end]:
            raise ValueError('MT6765 requires its recorded 36-bit DMA correction')
        source = source.replace('.name = "mtk-msdc"', '.name = "r1-msdc"')
        needle = '\tret = msdc_of_clock_parse(pdev, host);'
        if source.count(needle) != 1:
            raise ValueError('Storage adaptation target changed')
        source = source.replace(needle,
            '\t/* R1 bring-up: VEMC measured enabled at 3.0V. Retain LK rails. */\n'
            '\tif (!mmc->ocr_avail && of_machine_is_compatible("mediatek,MT6765"))\n'
            '\t\tmmc->ocr_avail = MMC_VDD_29_30 | MMC_VDD_30_31;\n\n' + needle)
        (output / 'r1_msdc.c').write_text(source)
        inputs.append({'source': 'generated storage adaptation', 'target': 'r1_msdc.c',
                       'sha256': digest(output / 'r1_msdc.c')})
        kbuild = 'obj-m += r1_emmc.o r1_msdc.o\n'
        config = ['LLVM=1', 'CONFIG_CC_IS_CLANG=y', 'CONFIG_CC_IS_GCC=',
                  'CONFIG_GCC_VERSION=0', 'CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=',
                  'CONFIG_CC_IMPLICIT_FALLTHROUGH=-Wimplicit-fallthrough']
    elif args.group == 'bluetooth-voice':
        copy(Path('external/rabbit-r1/bluetooth-voice/r1_sco.c'), 'r1_sco.c')
        kbuild = 'obj-m += r1_sco.o\n'
    elif args.group == 'display':
        for name in ('r1_backlight.c', 'r1_display_nodes.c'):
            copy(Path('external/rabbit-r1/display') / name, name)
        subprocess.run(['python3', str(ROOT / 'scripts/rabbit-r1/prepare-display-overlays.py'),
                        str(output / 'overlays')], check=True)
        shutil.copy2(output / 'overlays/display-overlays.h', output / 'display-overlays.h')
        inputs.append({'source': 'generated runtime overlays', 'target': 'display-overlays.h',
                       'sha256': digest(output / 'display-overlays.h')})
        kbuild = 'obj-m += r1_backlight.o r1_display_nodes.o\n'
    elif args.group == 'cpu':
        for source in sorted((ROOT / 'external/rabbit-r1/cpu').iterdir()):
            if source.suffix in ('.c', '.h'):
                copy(source.relative_to(ROOT), source.name)
        kbuild = ('obj-m += r1_cpu_dvfs.o r1_cpu_idle_smp.o\n'
                  'obj-m += r1_cpu_hotplug.o r1_cpu_power.o\n')
    elif args.group == 'board':
        for source in sorted((ROOT / 'external/rabbit-r1/board').glob('*.c')):
            copy(source.relative_to(ROOT), source.name)
        kbuild = ('obj-m += r1_battery.o r1_haptics.o r1_icm42607.o r1_ms35774.o\n'
                  'obj-m += r1_input_power.o r1_keypad.o r1_usb_tcpc.o r1_typec_bus.o\n'
                  'obj-m += r1_usb_session_v2.o r1_usb_host_speed.o\n'
                  'r1_usb_tcpc-y := tcpc.o\nr1_typec_bus-y := bus.o i2c-algo-bit.o\n'
                  'r1_usb_session_v2-y := session-v2.o\nr1_usb_host_speed-y := host-speed.o\n'
                  'ccflags-y += -I$(srctree)/drivers/usb/musb\n'
                  'CFLAGS_r1_ms35774.o += -DMAX_STEPS=1080 -DMOVE_BUDGET=256 -DGUARD_MS=1800\n')
    else:
        copy(Path('drivers/i2c/algos/i2c-algo-bit.c'), 'i2c-algo-bit.c')
        for name in ('r1_camera_probe.c', 'r1_camera_capture.h', 'r1_camera_pipeline.h',
                     'r1_camera_stream.h', 'sensor-tables.h'):
            copy(Path('external/rabbit-r1/camera') / name, name)
        receiver_input = args.receiver_calibration or args.receiver_table
        generator = 'prepare-camera-calibration.py' if args.receiver_calibration else 'prepare-camera-receiver.py'
        if args.receiver_calibration:
            inputs.append({'source': 'scripts/rabbit-r1/camera-receiver-template.json',
                           'sha256': digest(ROOT / 'scripts/rabbit-r1/camera-receiver-template.json')})
        subprocess.run(['python3', str(ROOT / 'scripts/rabbit-r1' / generator),
                        str(receiver_input.absolute()), str(output),
                        '--expected-sha256', args.receiver_sha256], check=True)
        inputs.append({'source': 'private receiver input (not bundled)',
                       'input_sha256': args.receiver_sha256,
                       'sha256': digest(output / 'include/generated/rabbit-r1-camera-receiver.h'),
                       'target': 'include/generated/rabbit-r1-camera-receiver.h'})
        kbuild = ('obj-m += i2c-algo-bit.o r1_camera_live.o\n'
                  'r1_camera_live-y := r1_camera_probe.o\n'
                  'ccflags-y += -I$(src)/include\n')
    (output / 'Makefile').write_text(kbuild)
    (output / 'build-inputs.json').write_text(json.dumps({
        'group': args.group, 'kernel_config_sha256': digest(kernel / '.config'),
        'inputs': inputs, 'kbuild_sha256': digest(output / 'Makefile'),
        'config_overrides': config}, indent=2) + '\n')
    prefix_flags = ' '.join([
        f'-fmacro-prefix-map={ROOT}=/usr/src/linux',
        f'-fmacro-prefix-map={output}=/usr/src/rabbit-r1/{args.group}',
        f'-fdebug-prefix-map={ROOT}=/usr/src/linux',
        f'-fdebug-prefix-map={kernel}=/usr/src/linux-build',
        f'-fdebug-prefix-map={output}=/usr/src/rabbit-r1/{args.group}',
    ])
    if args.group in ('connectivity', 'bluetooth-transport', 'gps'):
        prefix_flags += ' -fmacro-prefix-map=btif/= -fmacro-prefix-map=wmt/= -fmacro-prefix-map=adaptor/= -fmacro-prefix-map=bt/= -fmacro-prefix-map=driver/='
    if args.group == 'powervr':
        prefix_flags += (f' -I{output / "vendor-headers"} -Wno-declaration-after-statement'
                         f' -include {output / "r1_linux71_compat.h"}')
    if args.group in ('media', 'video-decoder', 'video'):
        prefix_flags += f' -fmacro-prefix-map={ROOT}=.'
        for old, new in [('mc/', 'drivers/media/mc/'),
                         ('v4l2/', 'drivers/media/v4l2-core/'),
                         ('vb2/', 'drivers/media/common/videobuf2/'),
                         ('jpeg/', 'drivers/media/platform/mediatek/jpeg/'),
                         ('mtk_iommu.c', 'drivers/iommu/mtk_iommu.c'),
                         ('mtk-smi.c', 'drivers/memory/mtk-smi.c')]:
            prefix_flags += f' -fmacro-prefix-map={old}={new}'
    if args.group == 'rfcomm':
        prefix_flags += f' -include {output / "historical-config.h"}'
    command = ['make', '-C', str(ROOT), f'O={kernel}', 'ARCH=arm64',
               'CROSS_COMPILE=' + os.environ.get('CROSS_COMPILE', 'aarch64-linux-gnu-'),
               'LOCALVERSION=+', f'M={output}', 'KCFLAGS=' + prefix_flags,
               f'-j{args.jobs}', *config, 'modules']
    subprocess.run(command, check=True)
    print('Local modules built. No installation or hardware validation performed.')


if __name__ == '__main__':
    main()
