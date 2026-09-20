// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_PRCM_FIXTURE_H
#define QSOC_PRCM_FIXTURE_H

inline const char *qsocPrcmDeclaration()
{
    return R"(
clock:
  - name: clock
    input: {aon_clk: {}}
    target:
      periph_clk:
        icg: {enable: gate_en, reset: por_n}
        link: {aon_clk: {}}
reset:
  - name: reset
    source: {por_n: {active: low}, hold_n: {active: low}}
    target:
      periph_n:
        active: low
        async: {clock: periph_clk, stage: 2}
        link: {por_n: {}, hold_n: {}}
prcm:
  version: 1
  controller:
    clock: {controller: clock, input: aon_clk}
    reset: {controller: reset, source: por_n}
    supply: aon
  mmio: {bus: apb4, data_width: 32, address_width: 12}
  supply:
    aon: {always_on: true}
    periph: {request: power_en, valid: {signal: pgood, sample_clock: aon_clk}}
  domain:
    periph:
      supply: periph
      clock: {controller: clock, target: periph_clk, stage: target.icg}
      reset: {controller: reset, source: hold_n, target: periph_n}
      quiesce: {request: stop_req, ack: {signal: idle, sample_clock: aon_clk}}
      isolation: {request: iso_req, active: {signal: iso_active, sample_clock: aon_clk}}
      reset_mode: 'OFF'
      mode:
        'OFF': {code: 0, power: 'off', clock: stopped, reset: asserted, isolation: enabled}
        'RUN': {code: 1, power: 'on', clock: running, reset: released, isolation: disabled}
      transition: [{from: 'OFF', to: 'RUN'}, {from: 'RUN', to: 'OFF'}]
)";
}

#endif // QSOC_PRCM_FIXTURE_H
