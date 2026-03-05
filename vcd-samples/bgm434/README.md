The `bgm434` folder contains a SystemVerilog project that simulates a pipelined multiplier with credit-based flow control.

Credits:

https://github.com/yuri-panchul/basics-graphics-music/tree/main/labs/4_microarchitecture/4_3_pipelines_2/4_3_4_pow_5_pipelined_with_credit_counter


## Run Simulation

```bash
iverilog -g2005-sv -o sim tb.sv \
  pow_5_pipelined_with_credit_counter.sv \
  pow_5_pipelined_without_flow_control.sv \
  reg_without_flow_control.sv \
  ff_fifo_wrapped_in_valid_ready.sv \
  flip_flop_fifo_with_counter.sv \
  flip_flop_fifo_empty_full_optimized.sv
vvp sim
```

## View Waveform

https://wavedrom.live/?github=wavedrom/vcd-samples/trunk/bgm434/dump.vcd.br&github=wavedrom/vcd-samples/trunk/bgm434/dump.waveql

