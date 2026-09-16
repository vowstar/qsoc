// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#pragma once

#include <QString>

inline QString iomuxBusFeatureSource(const QString &bus)
{
    return QString(R"(generator:
  kind: iomux
  bus: %1
  data_width: 128
  address_width: 16
  pin_count: 3
  hs_slots: 1
  option: {interrupt: true}
  integration:
    instance: u_mux
    clock: clk
    reset: rst_n
    control: control
    interrupt: irq
    pad: {input_value: pad_in, input_enable: pad_ie, output_value: pad_out, output_enable: pad_oe}
  route: []
  ls:
    main:
      pins: [0, 2]
      reset: 0
      channel:
        - {channel: 0, function: serial, signal: low, input_enable: 1, output_value: 0, output_enable: 1}
        - {channel: 2, function: serial, signal: high, input_value: {link: rx}, input_enable: 1, output_value: 1, output_enable: 1}
    other:
      pins: [1]
      reset: 1
      channel:
        - {channel: 1, function: gpio, signal: out, input_enable: 1, output_value: 1, output_enable: 1}
)")
        .arg(bus);
}

inline QString iomuxBusFeatureOperations(const QString &pass)
{
    return QString(R"(
initial begin
    repeat (3) @(negedge clk_i); rst_ni = 1;
    pad_input_value_i = 3;
    repeat (4) @(negedge clk_i);
    // P=3, H=1: nine 8-byte HS/IRQ banks precede LS TX at 0x148 and RX at 0x150.
    // Discard low events recorded while the input synchronizers filled after reset.
    write_byte('h130, 3, 0);
    read_byte('h128, 3, 0);
    read_byte('h130, 4, 0);
    if (irq_o !== 0) $fatal(1, "FEATURE_IRQ_DISABLED");
    write_byte('h108, 1, 0);
    if (irq_o !== 1) $fatal(1, "FEATURE_IRQ_ENABLED");
    write_byte('h120, 4, 0);
    pad_input_value_i = 2;
    repeat (4) @(negedge clk_i);
    // The clear byte is in the upper half of the word containing fall enable.
    write_byte('h128, 1, 0);
    read_byte('h128, 2, 0);
    read_byte('h120, 4, 0);
    read_byte('h130, 5, 0);
    if (irq_o !== 0) $fatal(1, "FEATURE_IRQ_CLEARED");
    write_byte('h108, 0, 0);

    pad_input_value_i = 3;
    if (pad_output_value_o !== 2 || pad_output_enable_o !== 7)
        $fatal(1, "FEATURE_LS_RESET");
    write_byte('h148, 2, 0);
    if (pad_output_value_o !== 3 || pad_output_enable_o !== 7)
        $fatal(1, "FEATURE_LS_TX");
    write_byte('h14a, 2, 0);
    if (pad_output_value_o !== 7 || pad_output_enable_o !== 7)
        $fatal(1, "FEATURE_LS_NEIGHBOR");
    write_byte('h148, 1, 0);
    if (pad_output_value_o !== 6 || pad_output_enable_o !== 6)
        $fatal(1, "FEATURE_LS_FOREIGN_TX");
    write_byte('h148, 3, 0);
    if (pad_output_value_o !== 6 || pad_output_enable_o !== 6)
        $fatal(1, "FEATURE_LS_INVALID_TX");
    read_byte('h14a, 2, 0);
    read_byte('h140, 1, 0);
    write_byte('h148, 0, 0);

    write_byte('h152, 0, 0);
    if (ls_c2_input_value_o !== 1) $fatal(1, "FEATURE_LS_RX_ZERO");
    write_byte('h152, 2, 0);
    if (ls_c2_input_value_o !== 0) $fatal(1, "FEATURE_LS_RX_TWO");
    write_byte('h152, 1, 0);
    if (ls_c2_input_value_o !== 0) $fatal(1, "FEATURE_LS_FOREIGN_RX");
    write_byte('h152, 3, 0);
    if (ls_c2_input_value_o !== 0) $fatal(1, "FEATURE_LS_INVALID_RX");
    read_byte('h152, 3, 0);
    read_byte('h14a, 2, 0);

    @(negedge clk_i); rst_ni = 0;
    repeat (2) @(negedge clk_i); rst_ni = 1;
    read_byte('h108, 0, 0);
    read_byte('h120, 0, 0);
    read_byte('h148, 0, 0);
    read_byte('h149, 1, 0);
    read_byte('h14a, 0, 0);
    read_byte('h152, 0, 0);
    if (pad_output_value_o !== 2 || pad_output_enable_o !== 7 || irq_o !== 0)
        $fatal(1, "FEATURE_RESET");
    $display("@PASS@ features transactions=%0d", transactions);
    $finish;
end
)")
        .replace("@PASS@", pass);
}
