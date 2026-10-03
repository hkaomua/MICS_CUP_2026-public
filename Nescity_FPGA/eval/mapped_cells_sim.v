// Functional models for the two restricted cell modes emitted by resources.py.
// This is a logic check, not an Altera timing simulation. resources.py checks
// every cell's parameters, unused controls, and outputs before using these models.
// The bundled Yosys MAX10 dffeas model ignores ena and does not model clrn;
// use the active-low-clear, enable-controlled behavior required by ff_map.v.
module dffeas #(
  parameter power_up = "low",
  parameter is_wysiwyg = "TRUE"
) (
  output reg q,
  input d, clk, clrn, prn, ena, asdata, aload, sclr, sload
);
  initial q = power_up == "high" ? 1'b1 : 1'b0;
  always @(posedge clk or negedge clrn)
    if (!clrn) q <= 1'b0;
    else if (ena) q <= d;
endmodule

module fiftyfivenm_lcell_comb #(
  parameter [15:0] lut_mask = 16'hffff,
  parameter sum_lutc_input = "datac"
) (
  output combout, cout,
  input dataa, datab, datac, datad, cin
);
  // Conditional merging preserves a known output when unused address bits
  // are X (e.g. an uninitialized RAM behind an unselected CPU read mux).
  wire [7:0] s3 = datad ? lut_mask[15:8] : lut_mask[7:0];
  wire [3:0] s2 = datac ? s3[7:4] : s3[3:0];
  wire [1:0] s1 = datab ? s2[3:2] : s2[1:0];
  assign combout = dataa ? s1[1] : s1[0];
  assign cout = 1'bx; // Carry mode is rejected; this output must be disconnected.
endmodule
