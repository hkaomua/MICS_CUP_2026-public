module arithmetic_tb;
  city_accelerator_nowater dut();
  city_accelerator_reference ref_dut();
  integer n, rf, s, a, j, raw, weighted;
  reg [31:0] rng = 32'h65021008;
  reg [7:0] v;
  reg [2:0] w;
  reg [4:0] sea, mountain, natural, urban, house, shop, tall;
  reg signed [15:0] expected_bias, expected_delta;
  reg signed [9:0] actual_bias;
  reg signed [2:0] actual_delta;
  initial begin
    for (n = 0; n <= 16575; n = n+1)
      if (dut.divide65(n[14:0]) !== n/65) $fatal(1, "divide65 n=%0d", n);
    for (rf = -130; rf <= 195; rf = rf+1)
      if ($signed(dut.force_delta(rf[8:0])) !== (rf-20)/20)
        $fatal(1, "force_delta rf=%0d", rf);
    // Every self/average byte pair, with reproducible coherent category counts.
    for (s = 0; s < 256; s = s+1) begin
      for (a = 0; a < 256; a = a+1) begin
        sea=0; mountain=0; natural=0; urban=0; house=0; shop=0; tall=0; raw=0;
        for (j = 0; j < 25; j = j+1) begin
          rng = rng * 32'd1664525 + 32'd1013904223;
          v = rng[31:24];
          w = ref_dut.weight_by_index(j[4:0]);
          raw = raw + $signed(ref_dut.influence_of_simple(v,w));
          if (v == 0) sea=sea+1;
          if (v == 1) mountain=mountain+1;
          if (v >= 1 && v <= 24) natural=natural+1;
          if (v >= 35 && v <= 100) urban=urban+1;
          if (v >= 35 && v <= 62) house=house+1;
          if (v >= 63 && v <= 78) shop=shop+1;
          if (v >= 79 && v <= 100) tall=tall+1;
        end
        expected_bias = ref_dut.calc_bias_simple(s[7:0],raw[15:0],a[7:0],
            {1'b0,sea},{1'b0,mountain},{1'b0,natural},{1'b0,urban},{1'b0,house},{1'b0,shop},{1'b0,tall});
        actual_bias = dut.calc_bias(s[7:0],raw[8:0],a[7:0],sea,mountain,natural,urban,house,shop,tall);
        if ($signed(actual_bias) !== $signed(expected_bias)) $fatal(1,"bias self=%0d avg=%0d",s,a);
        expected_delta = ref_dut.calc_delta_simple(s[7:0],raw[15:0],a[7:0],expected_bias);
        actual_delta = dut.calc_delta(s[7:0],raw[8:0],a[7:0],actual_bias);
        if ($signed(actual_delta) !== $signed(expected_delta)) $fatal(1,"delta self=%0d avg=%0d",s,a);
      end
      for (j=-3; j<=1; j=j+1)
        if (dut.calc_new_value(s[7:0],j[2:0]) !== ref_dut.calc_new_value_simple(s[7:0],j[15:0]))
          $fatal(1,"clamp self=%0d delta=%0d",s,j);
    end
    $display("PASS arithmetic: /65=16576, force=326, bias/delta=65536, clamp=1280");
    $finish;
  end
endmodule
