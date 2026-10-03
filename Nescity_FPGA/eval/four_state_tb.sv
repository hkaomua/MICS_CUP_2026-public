`timescale 1ns/1ps
module four_state_tb;
  reg clk_cpu=0, clk_calc=0, reset=0;
  always #5 clk_cpu=~clk_cpu;
  initial begin #3; forever #50 clk_calc=~clk_calc; end
  reg cpu_sel=0, cpu_we=0, cpu_re=0;
  reg [12:0] cpu_addr=0;
  reg [7:0] cpu_din=0;
  wire [7:0] cpu_dout;
  wire cpu_dout_oe, cpu_claim, busy_out;
  city_accelerator_nowater dut(.*);
  reg [7:0] expected [0:959];
  reg [7:0] value;
  reg [31:0] header;
  integer f, size, i, step, steps=2;
  reg [4095:0] path;
  task write_byte(input [12:0] addr, input [7:0] data);
    begin
      @(negedge clk_cpu); cpu_sel=1; cpu_we=1; cpu_re=0; cpu_addr=addr; cpu_din=data;
      repeat(3) @(negedge clk_cpu);
      cpu_sel=0; cpu_we=0;
      repeat(2) @(negedge clk_cpu);
    end
  endtask
  task read_byte(input [12:0] addr, output [7:0] data);
    begin
      @(negedge clk_cpu); cpu_sel=1; cpu_we=0; cpu_re=1; cpu_addr=addr;
      repeat(3) @(negedge clk_cpu);
      if (cpu_claim !== 1 || cpu_dout_oe !== 1) $fatal(1,"read not claimed");
      data=cpu_dout; cpu_sel=0; cpu_re=0;
    end
  endtask
  initial begin
    #100000000; $fatal(1,"four-state timeout");
  end
  initial begin
    if (!$value$plusargs("vectors=%s",path)) $fatal(1,"missing vectors");
    if ($value$plusargs("steps=%d",steps)) begin end
    f=$fopen(path,"rb"); if (!f) $fatal(1,"open vectors");
    size=$fread(header,f); size=$fread(header,f); // case count and first step count
    size=$fread(expected,f); if (size!=960) $fatal(1,"initial grid truncated");
    #1; reset=1; #250; reset=0; #250;
    write_byte(13'h1ff0,"C"); write_byte(13'h1ff1,"I");
    write_byte(13'h1ff2,"T"); write_byte(13'h1ff3,"Y");
    for (i=0;i<960;i=i+1) write_byte(i[12:0],expected[i]);
    for (step=1;step<=steps;step=step+1) begin
      size=$fread(expected,f); if (size!=960) $fatal(1,"expected grid truncated");
      write_byte(13'h1f10,8'd1);
      if (busy_out !== 1) $fatal(1,"start busy");
      wait(busy_out === 0);
      read_byte(13'h1f11,value); if(value !== 2) $fatal(1,"done flag");
      for(i=0;i<960;i=i+1) begin
        read_byte(i[12:0],value);
        if(value !== expected[i]) $fatal(1,"four-state step=%0d cell=%0d expected=%0d got=%0d",step,i,expected[i],value);
      end
    end
    $display("PASS four-state: %0d complete generations / both RAM banks, no X outputs",steps);
    $finish;
  end
endmodule
