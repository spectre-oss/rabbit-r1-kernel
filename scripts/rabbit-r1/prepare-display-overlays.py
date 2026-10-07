#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build ordered runtime-only native display overlays. Never change a boot DTB."""
from pathlib import Path
import argparse
import subprocess

KERNEL = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.output.is_relative_to(KERNEL):
        parser.error('Generated overlays must remain outside the source tree')
    args.output.mkdir(parents=True, exist_ok=False)
    stages = []

    def stage(name, target, content):
        index = len(stages)
        source = f'''/dts-v1/;
/plugin/;
#include <dt-bindings/clock/mt6765-clk.h>
/ {{ fragment@0 {{ target-path = "{target}";
    __overlay__ {{ {content} }};
}}; }};
'''
        dts = args.output / f'stage-{index}.dts'
        pp = args.output / f'stage-{index}.pp.dts'
        blob = args.output / f'stage-{index}.dtbo'
        dts.write_text(source)
        subprocess.run(['aarch64-linux-gnu-gcc', '-E', '-P', '-nostdinc', '-undef',
                        '-D__DTS__', '-x', 'assembler-with-cpp', '-I', str(KERNEL / 'include'),
                        str(dts), '-o', str(pp)], check=True)
        subprocess.run(['dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(blob), str(pp)], check=True)
        # Providers are resolved by live path in the loader; no global symbols
        # need to be attached to the immutable boot tree's __symbols__ node.
        if subprocess.run(['fdtget', '-p', str(blob), '/__symbols__'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            subprocess.run(['fdtput', '-r', str(blob), '/__symbols__'], check=True)
        data = blob.read_bytes()
        stages.append((name, data))

    stage('mmsys', '/', '''
      r1-display {
        compatible = "simple-bus"; #address-cells = <2>; #size-cells = <2>; ranges;
        r1_mm: clock-controller@14000000 {
          phandle = <1>;
          compatible = "mediatek,mt6765-mmsys", "syscon";
          reg = <0 0x14000000 0 0x1000>; #clock-cells = <1>; #reset-cells = <1>;
        };
      };
    ''')
    stage('mutex', '/r1-display', '''
      r1_mutex: mutex@14001000 {
        compatible = "mediatek,mt6765-disp-mutex";
        reg = <0 0x14001000 0 0x1000>;
      };
    ''')
    stage('phy', '/r1-display', '''
      r1_phy: phy@11c80000 {
        phandle = <1>;
        compatible = "mediatek,mt6765-mipi-tx", "mediatek,mt8183-mipi-tx";
        reg = <0 0x11c80000 0 0x10000>;
        clocks = <&apmixed CLK_APMIXED_MIPID0_26M>;
        #clock-cells = <0>; #phy-cells = <0>; clock-output-names = "r1_dsi_mppll";
      };
    ''')
    for name, address, irq, fallback, clock in [
        ('ovl', 0x1400b000, 217, 'mt8192-disp-ovl', 'DISP_OVL0'),
        ('ovl-2l', 0x1400c000, 229, 'mt8192-disp-ovl-2l', 'DISP_OVL0_2L'),
        ('rdma', 0x1400d000, 218, 'mt8183-disp-rdma', 'DISP_RDMA0'),
        ('color', 0x1400f000, 220, 'mt8173-disp-color', 'DISP_COLOR0'),
        ('ccorr', 0x14010000, 221, 'mt8183-disp-ccorr', 'DISP_CCORR0'),
        ('aal', 0x14011000, 222, 'mt8183-disp-aal', 'DISP_AAL0'),
        ('gamma', 0x14012000, 223, 'mt8183-disp-gamma', 'DISP_GAMMA0'),
        ('dither', 0x14013000, 224, 'mt8183-disp-dither', 'DISP_DITHER0'),
    ]:
        stage(name, '/r1-display', f'''
          {name}@{address:x} {{
            compatible = "mediatek,mt6765-disp-{name}", "mediatek,{fallback}";
            reg = <0 0x{address:x} 0 0x1000>;
            interrupts = <0 {irq} 8>;
            clocks = <0xf0000001 CLK_MM_{clock}>;
          }};
        ''')
    stage('dsi-panel', '/r1-display', '''
      dsi@14014000 {
        compatible = "mediatek,mt6765-dsi", "mediatek,mt8183-dsi";
        reg = <0 0x14014000 0 0x1000>; interrupts = <0 225 8>;
        clocks = <0xf0000001 CLK_MM_DSI0>, <0xf0000001 CLK_MM_DIG_DSI>, <0xf0000002>;
        clock-names = "engine", "digital", "hs";
        phys = <0xf0000002>; phy-names = "dphy";
        #address-cells = <1>; #size-cells = <0>;
        port { r1_dsi_out: endpoint { remote-endpoint = <&r1_panel_in>; }; };
        panel@0 {
          compatible = "rabbit,r1-st7701-panel"; reg = <0>;
          reset-gpios = <&pio 45 1>;
          port { r1_panel_in: endpoint { remote-endpoint = <&r1_dsi_out>; }; };
        };
      };
    ''')
    stage('backlight', '/r1-display', '''
      backlight@1100e000 {
        compatible = "rabbit,r1-disp-backlight";
        reg = <0 0x1100e000 0 0x1000>;
        clocks = <&infracfg_ao CLK_IFR_DISP_PWM>, <&topckgen CLK_TOP_DISP_PWM_SEL>;
        clock-names = "gate", "source";
      };
    ''')
    text = ['/* Generated runtime overlay blobs; not boot-image DTBs. */']
    for i, (_, data) in enumerate(stages):
        text.append(f'static const unsigned char stage_{i}[] __aligned(8) = {{')
        for offset in range(0, len(data), 16):
            text.append('  ' + ', '.join(f'0x{b:02x}' for b in data[offset:offset+16]) + ',')
        text.append('};')
    for i, (_, data) in enumerate(stages):
        patches = [(offset, int.from_bytes(data[offset:offset+4], 'big') - 0xf0000000)
                   for offset in range(0, len(data)-3, 4)
                   if data[offset:offset+4] in (bytes.fromhex('f0000001'), bytes.fromhex('f0000002'))]
        text.append(f'static const struct stage_patch patch_{i}[] = {{')
        text.extend(f'  {{ {offset}, {provider} }},' for offset, provider in patches)
        text.append('};')
    text.append('static const struct stage_blob stages[] = {')
    for i, (name, _) in enumerate(stages):
        text.append(f'  {{ "{name}", stage_{i}, sizeof(stage_{i}), patch_{i}, ARRAY_SIZE(patch_{i}) }},')
    text.append('};')
    (args.output / 'display-overlays.h').write_text('\n'.join(text) + '\n')
    print(f'{len(stages)} runtime stages built in {args.output}')


if __name__ == '__main__':
    main()
