/***************************************************************************************************
 * city_accelerator_nowater.v -- 10 MHz line-buffered implementation for MAX 10
 *
 * Purpose
 *   A resource-bounded hardware implementation of the active nescity.c rule.
 *   The CPU/MMIO side remains in the NES/cart clock domain (clk_cpu, nominally 100 MHz),
 *   while the simulation engine runs from a PLL-generated 10 MHz clock (clk_calc).
 *
 * Implementation
 *   - Four old rows are stored in one 32 x 32 simple-dual-port M9K history.
 *   - Fetch one new grid byte per column; retain five column summaries in FFs.
 *   - Pipeline one arithmetic lane at one output every 10 MHz clock.
 *   - Cache four prefix summaries; prefetch the next row during wraparound.
 *   - Split the 2 KiB grid into two dual-clock banks, preserving CPU/MMIO.
 *   - All byte inputs, torus boundaries, and C signed division remain exact.
 *   See eval/README.md for verification and resource estimates.
 *
 * Current simulation rule (same as active nescity.c)
 *   - 32 x 30 grid, 8-bit cells
 *   - 5 x 5 Manhattan-weighted neighborhood, torus boundary
 *   - simultaneous update using private double buffers
 *   - compute_water_penalty() is disabled, so water_penalty == 0
 *
 * CPU map after CITY unlock
 *   $6000-$63BF : active grid[960]
 *   $7F00       : 'C'
 *   $7F01       : 'A'
 *   $7F02       : interface version = 05h
 *   $7F03       : display/batch interval (01..FF=1..255, 00=256)
 *   $7F10       : control, bit0 START, bit1 soft reset (only accepted while idle)
 *   $7F11       : status, bit0 BUSY, bit1 DONE
 *   $7F12/$7F13 : completed step count, low/high
 *   $7F14       : water penalty (always 0)
 *
 * Unlock sequence while disabled
 *   $7FF0='C', $7FF1='I', $7FF2='T', $7FF3='Y'
 *
 * CPU grid behavior while BUSY
 *   read  -> FFh
 *   write -> ignored
 ***************************************************************************************************/

module city_accelerator_nowater
(
  input  wire        clk_cpu,// 100 MHz
  input  wire        clk_calc,// 10 MHz
  input  wire        reset,

  input  wire        cpu_sel,
  input  wire        cpu_we,
  input  wire        cpu_re,
  input  wire [12:0] cpu_addr,
  input  wire [ 7:0] cpu_din,
  output reg  [ 7:0] cpu_dout,
  output wire        cpu_dout_oe,
  output wire        cpu_claim,
  output wire        busy_out
);

// Number of accelerator steps executed between screen redraws by the NES program.
// 1..255 are returned directly from $7F03; 256 is encoded as 00h.
localparam integer DISPLAY_STEP_INTERVAL = 256;
localparam [7:0] DISPLAY_STEP_INTERVAL_CODE =
    (DISPLAY_STEP_INTERVAL == 256) ? 8'h00 : DISPLAY_STEP_INTERVAL[7:0];

// -------------------------------------------------------------------------------------------------
// CPU / MMIO clock domain (clk_cpu)
// -------------------------------------------------------------------------------------------------
reg        accel_enabled;
reg [1:0]  unlock_state;
reg        cpu_unlock_write_d;

reg        active_bank_cpu;
reg        busy_cpu;
reg        done_flag_cpu;
reg [15:0] step_count_cpu;

// START request crossing CPU -> calculator.
reg start_toggle_cpu;
reg start_bank_cpu;

// DONE event crossing calculator -> CPU.
reg done_toggle_sync1;
reg done_toggle_sync2;
reg done_toggle_seen_cpu;
// Generated in clk_calc domain; declared here because the CPU synchronizer reads it.
reg done_toggle_calc;
wire done_event_cpu = done_toggle_sync2 ^ done_toggle_seen_cpu;

wire cpu_reg_sel  = cpu_sel && accel_enabled && (cpu_addr[12:8] == 5'h1F);
wire cpu_grid_sel = cpu_sel && accel_enabled && (cpu_addr < 13'h03C0);
wire [9:0] cpu_grid_index = cpu_addr[9:0];

wire cpu_unlock_write =
    cpu_sel && cpu_we && !accel_enabled && (cpu_addr[12:4] == 9'h1FF);
wire cpu_unlock_write_pulse = cpu_unlock_write && !cpu_unlock_write_d;

assign busy_out = accel_enabled && busy_cpu;
assign cpu_claim = cpu_reg_sel || cpu_grid_sel;
assign cpu_dout_oe = cpu_sel && cpu_re && cpu_claim;

// -------------------------------------------------------------------------------------------------
// Two physical 1024x8 banks, one M9K each. Both keep their fixed CPU and
// calculator clocks. A source read and destination write can happen together;
// no clock mux or third RAM port is needed. CPU indices remain 0..959.
// -------------------------------------------------------------------------------------------------
reg source_bank_calc;
wire calc_grid_we;
wire [9:0] calc_grid_raddr, calc_grid_waddr;
wire [7:0] calc_grid_din, calc_grid_dout;
wire [7:0] calc_q0, calc_q1, cpu_q0, cpu_q1;
wire cpu_grid_ram_we = cpu_grid_sel && cpu_we && !busy_cpu;
wire [7:0] cpu_grid_ram_dout = active_bank_cpu ? cpu_q1 : cpu_q0;
assign calc_grid_dout = source_bank_calc ? calc_q1 : calc_q0;

city_grid_dpram_dc #(.ADDR_WIDTH(10), .DATA_WIDTH(8)) grid_bank0 (
  .clk_a(clk_calc), .we_a(calc_grid_we && source_bank_calc),
  .addr_a(source_bank_calc ? calc_grid_waddr : calc_grid_raddr),
  .data_a(calc_grid_din), .q_a(calc_q0),
  .clk_b(clk_cpu), .we_b(cpu_grid_ram_we && !active_bank_cpu),
  .addr_b(cpu_grid_index), .data_b(cpu_din), .q_b(cpu_q0)
);
city_grid_dpram_dc #(.ADDR_WIDTH(10), .DATA_WIDTH(8)) grid_bank1 (
  .clk_a(clk_calc), .we_a(calc_grid_we && !source_bank_calc),
  .addr_a(source_bank_calc ? calc_grid_raddr : calc_grid_waddr),
  .data_a(calc_grid_din), .q_a(calc_q1),
  .clk_b(clk_cpu), .we_b(cpu_grid_ram_we && active_bank_cpu),
  .addr_b(cpu_grid_index), .data_b(cpu_din), .q_b(cpu_q1)
);

// -------------------------------------------------------------------------------------------------
// CPU read mux
// -------------------------------------------------------------------------------------------------
always @(*) begin
  cpu_dout = 8'h00;

  if (cpu_sel && cpu_re && accel_enabled) begin
    if (cpu_reg_sel) begin
      case (cpu_addr[7:0])
        8'h00: cpu_dout = 8'h43; // 'C'
        8'h01: cpu_dout = 8'h41; // 'A'
        8'h02: cpu_dout = 8'h05; // interface version kept compatible with current NES software
        8'h03: cpu_dout = DISPLAY_STEP_INTERVAL_CODE;
        8'h10: cpu_dout = 8'h00;
        8'h11: cpu_dout = {6'b000000, done_flag_cpu, busy_cpu};
        8'h12: cpu_dout = step_count_cpu[7:0];
        8'h13: cpu_dout = step_count_cpu[15:8];
        8'h14: cpu_dout = 8'h00; // water_penalty
        default: cpu_dout = 8'h00;
      endcase
    end else if (cpu_grid_sel) begin
      cpu_dout = busy_cpu ? 8'hFF : cpu_grid_ram_dout;
    end
  end
end

// CPU-domain control, unlock, and completion bookkeeping.
always @(posedge clk_cpu or posedge reset) begin
  if (reset) begin
    accel_enabled        <= 1'b0;
    unlock_state         <= 2'd0;
    cpu_unlock_write_d   <= 1'b0;
    active_bank_cpu      <= 1'b0;
    busy_cpu             <= 1'b0;
    done_flag_cpu        <= 1'b0;
    step_count_cpu       <= 16'd0;
    start_toggle_cpu     <= 1'b0;
    start_bank_cpu       <= 1'b0;
    done_toggle_sync1    <= 1'b0;
    done_toggle_sync2    <= 1'b0;
    done_toggle_seen_cpu <= 1'b0;
  end else begin
    cpu_unlock_write_d <= cpu_unlock_write;

    // Synchronize DONE toggle from the 10 MHz domain.
    done_toggle_sync1 <= done_toggle_calc;
    done_toggle_sync2 <= done_toggle_sync1;

    if (done_event_cpu) begin
      done_toggle_seen_cpu <= done_toggle_sync2;
      active_bank_cpu <= ~active_bank_cpu;
      step_count_cpu <= step_count_cpu + 16'd1;
      busy_cpu <= 1'b0;
      done_flag_cpu <= 1'b1;
    end

    // CITY unlock is entirely in the CPU clock domain.
    if (cpu_unlock_write_pulse) begin
      case (unlock_state)
        2'd0: begin
          if (cpu_addr[3:0] == 4'h0 && cpu_din == 8'h43) unlock_state <= 2'd1;
          else                                            unlock_state <= 2'd0;
        end
        2'd1: begin
          if (cpu_addr[3:0] == 4'h1 && cpu_din == 8'h49) unlock_state <= 2'd2;
          else                                            unlock_state <= 2'd0;
        end
        2'd2: begin
          if (cpu_addr[3:0] == 4'h2 && cpu_din == 8'h54) unlock_state <= 2'd3;
          else                                            unlock_state <= 2'd0;
        end
        2'd3: begin
          if (cpu_addr[3:0] == 4'h3 && cpu_din == 8'h59) begin
            accel_enabled <= 1'b1;
            unlock_state <= 2'd0;
          end else begin
            unlock_state <= 2'd0;
          end
        end
        default: unlock_state <= 2'd0;
      endcase
    end

    // Control register.  START is converted to a toggle request so no pulse-width
    // assumption exists between the 100 MHz and 10 MHz domains.
    if (accel_enabled && cpu_sel && cpu_we && cpu_reg_sel && cpu_addr[7:0] == 8'h10) begin
      if (!busy_cpu && cpu_din[1]) begin
        active_bank_cpu <= 1'b0;
        step_count_cpu  <= 16'd0;
        done_flag_cpu   <= 1'b0;
      end

      if (!busy_cpu && cpu_din[0]) begin
        start_bank_cpu   <= active_bank_cpu;
        start_toggle_cpu <= ~start_toggle_cpu;
        busy_cpu         <= 1'b1;
        done_flag_cpu    <= 1'b0;
      end
    end
  end
end

// -------------------------------------------------------------------------------------------------
// Line-buffered 10 MHz calculator: one output cell every clock after filling.
// -------------------------------------------------------------------------------------------------
localparam [2:0] ST_IDLE = 3'd0, ST_SEED = 3'd1, ST_SEED_DRAIN = 3'd2,
                 ST_RUN = 3'd3, ST_FINISH = 3'd4;
reg [2:0] calc_state;
reg start_toggle_sync1, start_toggle_sync2, start_toggle_seen_calc;
reg start_bank_sync1, start_bank_sync2;
wire start_event_calc = start_toggle_sync2 ^ start_toggle_seen_calc;

// Seed history with rows 28,29,0,1, four bytes per x. Its contents need no reset:
// every word is completely initialized before the first streaming read.
reg [6:0] seed_issue, seed_tag;
reg seed_valid;
reg [31:0] seed_word;
wire [4:0] seed_y = {3'd0, seed_issue[1:0]} + 5'd28;
wire [4:0] seed_wrapped_y = (seed_y >= 5'd30) ? seed_y - 5'd30 : seed_y;
reg [4:0] stream_y, bottom_y;
reg [5:0] stream_pos;
reg feed_active;
wire [4:0] stream_x = stream_pos[4:0] + 5'd30;
wire [4:0] next_bottom_y = bottom_y == 5'd29 ? 5'd0 : bottom_y + 5'd1;
wire tail_issue = stream_pos >= 6'd32;
wire stream_issue = calc_state == ST_RUN && feed_active;
wire stream_read = stream_issue && (!tail_issue || stream_y != 5'd29);
wire [4:0] read_y = tail_issue ? next_bottom_y : bottom_y;
reg data_valid, data_has_sample;
reg [4:0] data_y, data_x;
reg [5:0] data_pos;

// Four prefix summaries are reused for the horizontal wrap. While an OLD
// summary is consumed at the row tail, the same slot captures the NEXT row's
// prefix. Four parallel reads at position 4 preload the next row's window.
reg [60:0] wrap_summary [0:3];
reg [10:0] col_sum [0:4];
reg [11:0] col_weight [0:4];
reg signed [4:0] col_force [0:4];
reg signed [6:0] col_force_weight [0:4];
reg [7:0] col_self [0:4];
reg [2:0] col_sea [0:4];
reg [2:0] col_mountain [0:4];
reg [2:0] col_natural [0:4];
reg [2:0] col_house [0:4];
reg [2:0] col_shop [0:4];
reg [2:0] col_tall [0:4];
reg column_valid;
reg [9:0] column_index;

// SUM -> AVG -> BIAS -> NEW_VALUE -> RAM write, one cell each clock.
// Each stage owns its value, category counts, and output index.
reg sum_valid, avg_valid, bias_valid, write_valid;
reg [9:0] sum_index, avg_index, bias_index, write_index;
reg [14:0] weighted_sum_acc; // <= 65*255 = 16575
reg signed [8:0] raw_force_acc; // -130..195
reg [4:0] sea_count, mountain_count, natural_count, house_count, shop_count, tall_count;
reg [4:0] sea_avg, mountain_avg, natural_avg, house_avg, shop_avg, tall_avg, urban_avg;
reg [7:0] self_value, self_avg, avg_value, self_b, avg_b, next_value;
reg signed [8:0] force_avg, force_b;
reg signed [9:0] bias_value;
integer c;

wire history_seed_write = seed_valid && seed_tag[1:0] == 2'd3;
// Each source column is read once per row, then its history advances. The
// cached summary, not the updated history, supplies this row's repeated prefix.
wire history_stream_write = data_valid && data_has_sample;
wire history_we = !reset && (history_seed_write || history_stream_write);
wire [4:0] history_waddr = history_seed_write ? seed_tag[6:2] : data_x;
wire [31:0] history_dout;
wire [31:0] history_din = history_seed_write ?
    {seed_word[23:0], calc_grid_dout} : {history_dout[23:0], calc_grid_dout};
city_row_history history_ram (
  .clk(clk_calc), .we(history_we), .waddr(history_waddr),
  .raddr(stream_x), .din(history_din), .q(history_dout)
);

assign calc_grid_we = !reset && calc_state == ST_RUN && write_valid;
assign calc_grid_waddr = write_index;
assign calc_grid_raddr = calc_state == ST_SEED ?
    {seed_wrapped_y, seed_issue[6:2]} : {read_y, stream_x};
assign calc_grid_din = next_value;

function signed [2:0] force_coefficient;
  input [7:0] v;
  begin
    if (v == 8'd0)      force_coefficient = 3'sd3;
    else if (v <= 8'd14) force_coefficient = -3'sd2;
    else if (v <= 8'd24) force_coefficient = 3'sd0;
    else if (v <= 8'd34) force_coefficient = 3'sd1;
    else if (v <= 8'd49) force_coefficient = 3'sd2;
    else if (v <= 8'd62) force_coefficient = 3'sd3;
    else if (v <= 8'd78) force_coefficient = 3'sd2;
    else                force_coefficient = 3'sd1;
  end
endfunction
// The five bytes of one vertical column are available together: four old rows
// from M9K history, and the newly fetched bottom row from the source grid.
wire [7:0] sample_v0 = history_dout[31:24];
wire signed [2:0] sample_f0 = force_coefficient(sample_v0);
wire sample_special0 = sample_v0 >= 8'd15 && sample_v0 <= 8'd24;
wire sample_sea0 = sample_v0 == 8'd0;
wire sample_mountain0 = sample_v0 == 8'd1;
wire sample_natural0 = sample_v0 >= 8'd1 && sample_v0 <= 8'd24;
wire sample_house0 = sample_v0 >= 8'd35 && sample_v0 <= 8'd62;
wire sample_shop0 = sample_v0 >= 8'd63 && sample_v0 <= 8'd78;
wire sample_tall0 = sample_v0 >= 8'd79 && sample_v0 <= 8'd100;
wire [7:0] sample_v1 = history_dout[23:16];
wire signed [2:0] sample_f1 = force_coefficient(sample_v1);
wire sample_special1 = sample_v1 >= 8'd15 && sample_v1 <= 8'd24;
wire sample_sea1 = sample_v1 == 8'd0;
wire sample_mountain1 = sample_v1 == 8'd1;
wire sample_natural1 = sample_v1 >= 8'd1 && sample_v1 <= 8'd24;
wire sample_house1 = sample_v1 >= 8'd35 && sample_v1 <= 8'd62;
wire sample_shop1 = sample_v1 >= 8'd63 && sample_v1 <= 8'd78;
wire sample_tall1 = sample_v1 >= 8'd79 && sample_v1 <= 8'd100;
wire [7:0] sample_v2 = history_dout[15:8];
wire signed [2:0] sample_f2 = force_coefficient(sample_v2);
wire sample_special2 = sample_v2 >= 8'd15 && sample_v2 <= 8'd24;
wire sample_sea2 = sample_v2 == 8'd0;
wire sample_mountain2 = sample_v2 == 8'd1;
wire sample_natural2 = sample_v2 >= 8'd1 && sample_v2 <= 8'd24;
wire sample_house2 = sample_v2 >= 8'd35 && sample_v2 <= 8'd62;
wire sample_shop2 = sample_v2 >= 8'd63 && sample_v2 <= 8'd78;
wire sample_tall2 = sample_v2 >= 8'd79 && sample_v2 <= 8'd100;
wire [7:0] sample_v3 = history_dout[7:0];
wire signed [2:0] sample_f3 = force_coefficient(sample_v3);
wire sample_special3 = sample_v3 >= 8'd15 && sample_v3 <= 8'd24;
wire sample_sea3 = sample_v3 == 8'd0;
wire sample_mountain3 = sample_v3 == 8'd1;
wire sample_natural3 = sample_v3 >= 8'd1 && sample_v3 <= 8'd24;
wire sample_house3 = sample_v3 >= 8'd35 && sample_v3 <= 8'd62;
wire sample_shop3 = sample_v3 >= 8'd63 && sample_v3 <= 8'd78;
wire sample_tall3 = sample_v3 >= 8'd79 && sample_v3 <= 8'd100;
wire [7:0] sample_v4 = calc_grid_dout;
wire signed [2:0] sample_f4 = force_coefficient(sample_v4);
wire sample_special4 = sample_v4 >= 8'd15 && sample_v4 <= 8'd24;
wire sample_sea4 = sample_v4 == 8'd0;
wire sample_mountain4 = sample_v4 == 8'd1;
wire sample_natural4 = sample_v4 >= 8'd1 && sample_v4 <= 8'd24;
wire sample_house4 = sample_v4 >= 8'd35 && sample_v4 <= 8'd62;
wire sample_shop4 = sample_v4 >= 8'd63 && sample_v4 <= 8'd78;
wire sample_tall4 = sample_v4 >= 8'd79 && sample_v4 <= 8'd100;
wire [10:0] sample_sum =
    {3'd0, sample_v0}
    + {3'd0, sample_v1}
    + {3'd0, sample_v2}
    + {3'd0, sample_v3}
    + {3'd0, sample_v4};
wire [11:0] sample_weight =
    {4'd0, sample_v0}
    + ({4'd0, sample_v1} << 1)
    + ({4'd0, sample_v2} << 1)
    + {4'd0, sample_v2}
    + ({4'd0, sample_v3} << 1)
    + {4'd0, sample_v4};
wire signed [4:0] sample_force_sum =
    $signed({{2{sample_f0[2]}}, sample_f0})
    + $signed({{2{sample_f1[2]}}, sample_f1})
    + $signed({{2{sample_f2[2]}}, sample_f2})
    + $signed({{2{sample_f3[2]}}, sample_f3})
    + $signed({{2{sample_f4[2]}}, sample_f4});
wire signed [6:0] sample_force_weight =
    $signed({{4{sample_f0[2]}}, sample_f0})
    + ($signed({{4{sample_f1[2]}}, sample_f1}) << 1)
    + ($signed({{4{sample_f2[2]}}, sample_f2}) << 1)
    + $signed({{4{sample_f2[2]}}, sample_f2})
    + ($signed({{4{sample_f3[2]}}, sample_f3}) << 1)
    + $signed({{4{sample_f4[2]}}, sample_f4})
    + $signed({5'd0, sample_special0, 1'b0})
    + $signed({5'd0, sample_special1, 1'b0})
    + $signed({5'd0, sample_special2, 1'b0})
    + $signed({5'd0, sample_special3, 1'b0})
    + $signed({5'd0, sample_special4, 1'b0});
wire [2:0] sample_sea_sum =
    {2'd0, sample_sea0}
    + {2'd0, sample_sea1}
    + {2'd0, sample_sea2}
    + {2'd0, sample_sea3}
    + {2'd0, sample_sea4};
wire [2:0] sample_mountain_sum =
    {2'd0, sample_mountain0}
    + {2'd0, sample_mountain1}
    + {2'd0, sample_mountain2}
    + {2'd0, sample_mountain3}
    + {2'd0, sample_mountain4};
wire [2:0] sample_natural_sum =
    {2'd0, sample_natural0}
    + {2'd0, sample_natural1}
    + {2'd0, sample_natural2}
    + {2'd0, sample_natural3}
    + {2'd0, sample_natural4};
wire [2:0] sample_house_sum =
    {2'd0, sample_house0}
    + {2'd0, sample_house1}
    + {2'd0, sample_house2}
    + {2'd0, sample_house3}
    + {2'd0, sample_house4};
wire [2:0] sample_shop_sum =
    {2'd0, sample_shop0}
    + {2'd0, sample_shop1}
    + {2'd0, sample_shop2}
    + {2'd0, sample_shop3}
    + {2'd0, sample_shop4};
wire [2:0] sample_tall_sum =
    {2'd0, sample_tall0}
    + {2'd0, sample_tall1}
    + {2'd0, sample_tall2}
    + {2'd0, sample_tall3}
    + {2'd0, sample_tall4};

wire [60:0] sample_summary = {sample_sum, sample_weight, sample_force_sum, sample_force_weight, sample_v2, sample_sea_sum, sample_mountain_sum, sample_natural_sum, sample_house_sum, sample_shop_sum, sample_tall_sum};
wire [60:0] incoming_summary = data_pos >= 6'd32 ?
    wrap_summary[data_pos[1:0]] : sample_summary;

// Exact /65 over 0..16575: one correction after a shift/subtract estimate.
function [7:0] divide65;
  input [14:0] n;
  reg [14:0] reduced, remainder;
  reg [7:0] q0;
  begin
    reduced = n - {6'd0, n[14:6]};
    q0 = reduced[13:6];
    remainder = n - {1'b0, q0, 6'd0} - {7'd0, q0};
    divide65 = q0 + ((remainder >= 15'd65) ? 8'd1 : 8'd0);
  end
endfunction

// Exact signed truncation of (raw_force - 20)/20 for raw_force in -130..195.
function signed [4:0] force_delta;
  input signed [8:0] rf;
  begin
    if      (rf >=  9'sd180) force_delta =  5'sd8;
    else if (rf >=  9'sd160) force_delta =  5'sd7;
    else if (rf >=  9'sd140) force_delta =  5'sd6;
    else if (rf >=  9'sd120) force_delta =  5'sd5;
    else if (rf >=  9'sd100) force_delta =  5'sd4;
    else if (rf >=  9'sd80)  force_delta =  5'sd3;
    else if (rf >=  9'sd60)  force_delta =  5'sd2;
    else if (rf >=  9'sd40)  force_delta =  5'sd1;
    else if (rf >   9'sd0)   force_delta =  5'sd0;
    else if (rf >  -9'sd20)  force_delta = -5'sd1;
    else if (rf >  -9'sd40)  force_delta = -5'sd2;
    else if (rf >  -9'sd60)  force_delta = -5'sd3;
    else if (rf >  -9'sd80)  force_delta = -5'sd4;
    else if (rf >  -9'sd100) force_delta = -5'sd5;
    else if (rf >  -9'sd120) force_delta = -5'sd6;
    else                    force_delta = -5'sd7;
  end
endfunction

function signed [9:0] calc_bias;
  input [7:0] self;
  input signed [8:0] rf;
  input [7:0] avg;
  input [4:0] sea, mountain, natural, urban, house, shop, tall;
  reg signed [9:0] b;
  reg [6:0] natural_pressure;
  reg [7:0] over_capacity, capacity_quotient;
  begin
    b = 10'sd0;
    natural_pressure = {2'd0, natural} + {1'b0, sea, 1'b0} + {1'b0, mountain, 1'b0};
    over_capacity = (avg > 8'd55) ? avg - 8'd55 : 8'd0;
    // Deliberately unsigned and byte-wide: no 32-bit signed divider.
    capacity_quotient = over_capacity / 8'd3;
    if (self <= 8'd1) b = b - 10'sd4;
    if (self >= 8'd1 && self <= 8'd34) begin
      if (house >= 5'd2) b = b + 10'sd2;
      if (house >= 5'd4) b = b + 10'sd2;
      if (house >= 5'd7) b = b + 10'sd2;
      if (shop >= 5'd1 && house >= 5'd3) b = b + 10'sd1;
      if (sea >= 5'd4) b = b - 10'sd2;
      if (mountain >= 5'd3 && house < 5'd4) b = b - 10'sd3;
      if (house < 5'd2) b = b - 10'sd3;
    end
    if (self >= 8'd35 && self <= 8'd62) begin
      if (shop >= 5'd1) b = b + 10'sd2;
      if (tall >= 5'd2) b = b + 10'sd1;
    end
    if (self >= 8'd63 && ({1'b0, shop} + {1'b0, tall}) >= 6'd4) b = b + 10'sd2;
    if (self >= 8'd35) begin
      if (urban < 5'd5) b = b - $signed({5'd0, (5'd5 - urban)});
      b = b - $signed({7'd0, natural_pressure[6:4]});
    end
    b = b - $signed({capacity_quotient, 2'b0});
    if (self >= 8'd25 && self < 8'd35 && urban < 5'd3 && rf < 9'sd12) b = b - 10'sd1;
    if (rf < 9'sd15 && self < 8'd35) b = b - 10'sd1;
    calc_bias = b;
  end
endfunction

function signed [2:0] calc_delta;
  input [7:0] self;
  input signed [8:0] rf;
  input [7:0] avg;
  input signed [9:0] bias;
  reg signed [9:0] d;
  reg signed [4:0] f;
  begin
    f = force_delta(rf);
    d = $signed({{5{f[4]}}, f}) + bias;
    if ({1'b0, avg} > {1'b0, self} + 9'd12) d = d + 10'sd1;
    if ({1'b0, avg} + 9'd20 < {1'b0, self}) d = d - 10'sd1;
    if (d > 10'sd1)       calc_delta = 3'sd1;
    else if (d < -10'sd3) calc_delta = -3'sd3;
    else                  calc_delta = d[2:0];
  end
endfunction

function [7:0] calc_new_value;
  input [7:0] self;
  input signed [2:0] delta;
  reg signed [9:0] nv;
  begin
    nv = $signed({2'b0, self}) + $signed({{7{delta[2]}}, delta});
    if (self == 8'd0)  calc_new_value = 8'd0;
    else if (nv < 10'sd1)   calc_new_value = 8'd1;
    else if (nv > 10'sd100) calc_new_value = 8'd100;
    else calc_new_value = nv[7:0];
    // self <= 1 can rise by at most 1, so the reference's >8 cap is unreachable.
  end
endfunction


always @(posedge clk_calc or posedge reset) begin
  if (reset) begin
    start_toggle_sync1 <= 1'b0;
    start_toggle_sync2 <= 1'b0;
    start_toggle_seen_calc <= 1'b0;
    start_bank_sync1 <= 1'b0;
    start_bank_sync2 <= 1'b0;
    source_bank_calc <= 1'b0;
    done_toggle_calc <= 1'b0;
    calc_state <= ST_IDLE;
    seed_issue <= 7'd0;
    seed_tag <= 7'd0;
    seed_valid <= 1'b0;
    seed_word <= 32'd0;
    stream_y <= 5'd0;
    bottom_y <= 5'd2;
    stream_pos <= 6'd0;
    feed_active <= 1'b0;
    data_valid <= 1'b0;
    data_has_sample <= 1'b0;
    data_y <= 5'd0;
    data_x <= 5'd0;
    data_pos <= 6'd0;
    column_valid <= 1'b0;
    column_index <= 10'd0;
    weighted_sum_acc <= 15'd0;
    raw_force_acc <= 9'sd0;
    force_avg <= 9'sd0;
    force_b <= 9'sd0;
    bias_value <= 10'sd0;
    sum_valid <= 1'b0;
    sum_index <= 10'd0;
    avg_valid <= 1'b0;
    avg_index <= 10'd0;
    bias_valid <= 1'b0;
    bias_index <= 10'd0;
    write_valid <= 1'b0;
    write_index <= 10'd0;
    sea_count <= 5'd0;
    sea_avg <= 5'd0;
    mountain_count <= 5'd0;
    mountain_avg <= 5'd0;
    natural_count <= 5'd0;
    natural_avg <= 5'd0;
    house_count <= 5'd0;
    house_avg <= 5'd0;
    shop_count <= 5'd0;
    shop_avg <= 5'd0;
    tall_count <= 5'd0;
    tall_avg <= 5'd0;
    urban_avg <= 5'd0;
    self_value <= 8'd0;
    self_avg <= 8'd0;
    avg_value <= 8'd0;
    self_b <= 8'd0;
    avg_b <= 8'd0;
    next_value <= 8'd0;
    // Both summary caches are filled before a valid window can use them.
  end else begin
    start_toggle_sync1 <= start_toggle_cpu;
    start_toggle_sync2 <= start_toggle_sync1;
    start_bank_sync1 <= start_bank_cpu;
    start_bank_sync2 <= start_bank_sync1;
    seed_valid <= calc_state == ST_SEED;
    seed_tag <= seed_issue;
    if (seed_valid) seed_word <= {seed_word[23:0], calc_grid_dout};
    // One synchronous RAM latency, including at row boundaries.
    data_valid <= stream_issue;
    data_has_sample <= stream_read;
    data_y <= stream_y;
    data_x <= stream_x;
    data_pos <= stream_pos;
    case (calc_state)
      ST_IDLE: if (start_event_calc) begin
        start_toggle_seen_calc <= start_toggle_sync2;
        source_bank_calc <= start_bank_sync2;
        seed_issue <= 7'd0;
        stream_y <= 5'd0;
        bottom_y <= 5'd2;
        stream_pos <= 6'd0;
        feed_active <= 1'b1;
        column_valid <= 1'b0;
        sum_valid <= 1'b0;
        avg_valid <= 1'b0;
        bias_valid <= 1'b0;
        write_valid <= 1'b0;
        calc_state <= ST_SEED;
      end
      ST_SEED: begin
        seed_issue <= seed_issue + 7'd1;
        if (seed_issue == 7'd127) calc_state <= ST_SEED_DRAIN;
      end
      ST_SEED_DRAIN: calc_state <= ST_RUN;
      ST_RUN: begin
        if (feed_active) begin
          if (stream_pos == 6'd35) begin
            // The prefix was prefetched during the previous row's tail.
            stream_pos <= 6'd4;
            if (stream_y == 5'd29) feed_active <= 1'b0;
            else begin
              stream_y <= stream_y + 5'd1;
              bottom_y <= next_bottom_y;
            end
          end else stream_pos <= stream_pos + 6'd1;
        end

        column_valid <= data_valid && data_pos >= 6'd4;
        if (data_valid) begin
          if (data_has_sample && (data_pos < 6'd4 || data_pos >= 6'd32))
            wrap_summary[data_pos[1:0]] <= sample_summary;
          if (data_pos >= 6'd4) begin
            column_index <= {data_y, (data_pos[4:0] - 5'd4)};
            for (c=0; c<4; c=c+1) begin
              if (data_pos == 6'd4)
                {col_sum[c], col_weight[c], col_force[c], col_force_weight[c], col_self[c], col_sea[c], col_mountain[c], col_natural[c], col_house[c], col_shop[c], col_tall[c]} <= wrap_summary[c];
              else
                {col_sum[c], col_weight[c], col_force[c], col_force_weight[c], col_self[c], col_sea[c], col_mountain[c], col_natural[c], col_house[c], col_shop[c], col_tall[c]} <= {col_sum[c+1], col_weight[c+1], col_force[c+1], col_force_weight[c+1], col_self[c+1], col_sea[c+1], col_mountain[c+1], col_natural[c+1], col_house[c+1], col_shop[c+1], col_tall[c+1]};
            end
            {col_sum[4], col_weight[4], col_force[4], col_force_weight[4], col_self[4], col_sea[4], col_mountain[4], col_natural[4], col_house[4], col_shop[4], col_tall[4]} <= incoming_summary;
          end
        end

        sum_valid <= column_valid;
        if (column_valid) begin
          sum_index <= column_index;
            weighted_sum_acc <= {3'd0, col_weight[0]} + {3'd0, col_weight[1]} + {3'd0, col_weight[2]}
                + {3'd0, col_weight[3]} + {3'd0, col_weight[4]} + {4'd0, col_sum[1]}
                + ({4'd0, col_sum[2]} << 1) + {4'd0, col_sum[3]};
            raw_force_acc <= $signed({{2{col_force_weight[0][6]}}, col_force_weight[0]})
                + $signed({{2{col_force_weight[1][6]}}, col_force_weight[1]})
                + $signed({{2{col_force_weight[2][6]}}, col_force_weight[2]})
                + $signed({{2{col_force_weight[3][6]}}, col_force_weight[3]})
                + $signed({{2{col_force_weight[4][6]}}, col_force_weight[4]})
                + $signed({{4{col_force[1][4]}}, col_force[1]})
                + ($signed({{4{col_force[2][4]}}, col_force[2]}) <<< 1)
                + $signed({{4{col_force[3][4]}}, col_force[3]});
            self_value <= col_self[2];
            sea_count <= {2'd0, col_sea[0]} + {2'd0, col_sea[1]} + {2'd0, col_sea[2]} + {2'd0, col_sea[3]} + {2'd0, col_sea[4]};
            mountain_count <= {2'd0, col_mountain[0]} + {2'd0, col_mountain[1]} + {2'd0, col_mountain[2]} + {2'd0, col_mountain[3]} + {2'd0, col_mountain[4]};
            natural_count <= {2'd0, col_natural[0]} + {2'd0, col_natural[1]} + {2'd0, col_natural[2]} + {2'd0, col_natural[3]} + {2'd0, col_natural[4]};
            house_count <= {2'd0, col_house[0]} + {2'd0, col_house[1]} + {2'd0, col_house[2]} + {2'd0, col_house[3]} + {2'd0, col_house[4]};
            shop_count <= {2'd0, col_shop[0]} + {2'd0, col_shop[1]} + {2'd0, col_shop[2]} + {2'd0, col_shop[3]} + {2'd0, col_shop[4]};
            tall_count <= {2'd0, col_tall[0]} + {2'd0, col_tall[1]} + {2'd0, col_tall[2]} + {2'd0, col_tall[3]} + {2'd0, col_tall[4]};
        end
        avg_valid <= sum_valid;
        if (sum_valid) begin
          avg_index <= sum_index;
          avg_value <= divide65(weighted_sum_acc);
          urban_avg <= house_count + shop_count + tall_count;
          self_avg <= self_value;
          force_avg <= raw_force_acc;
          sea_avg <= sea_count;
          mountain_avg <= mountain_count;
          natural_avg <= natural_count;
          house_avg <= house_count;
          shop_avg <= shop_count;
          tall_avg <= tall_count;
        end
        bias_valid <= avg_valid;
        if (avg_valid) begin
          bias_index <= avg_index;
          self_b <= self_avg;
          avg_b <= avg_value;
          force_b <= force_avg;
          bias_value <= calc_bias(self_avg, force_avg, avg_value,
              sea_avg, mountain_avg, natural_avg, urban_avg, house_avg, shop_avg, tall_avg);
        end
        write_valid <= bias_valid;
        if (bias_valid) begin
          write_index <= bias_index;
          next_value <= calc_new_value(self_b, calc_delta(self_b, force_b, avg_b, bias_value));
        end
        // The final destination write commits on this edge, before DONE.
        if (write_valid && write_index == 10'd959) calc_state <= ST_FINISH;
      end
      ST_FINISH: begin
        done_toggle_calc <= ~done_toggle_calc;
        calc_state <= ST_IDLE;
      end
      default: calc_state <= ST_IDLE;
    endcase
  end
end

endmodule

// -------------------------------------------------------------------------------------------------
// 2 KiB dual-clock true-dual-port RAM.
// Port A and port B use independent clocks.  Cross-port same-address read/write behavior is not
// relied upon: CPU grid access is blocked while the calculator is active.
// -------------------------------------------------------------------------------------------------
module city_grid_dpram_dc
#(
  parameter ADDR_WIDTH = 11,
  parameter DATA_WIDTH = 8
)
(
  input  wire                  clk_a,
  input  wire                  we_a,
  input  wire [ADDR_WIDTH-1:0] addr_a,
  input  wire [DATA_WIDTH-1:0] data_a,
  output reg  [DATA_WIDTH-1:0] q_a,

  input  wire                  clk_b,
  input  wire                  we_b,
  input  wire [ADDR_WIDTH-1:0] addr_b,
  input  wire [DATA_WIDTH-1:0] data_b,
  output reg  [DATA_WIDTH-1:0] q_b
);

(* ramstyle = "M9K" *) reg [DATA_WIDTH-1:0] ram [0:(1<<ADDR_WIDTH)-1];

always @(posedge clk_a) begin
  if (we_a)
    ram[addr_a] <= data_a;
  q_a <= ram[addr_a];
end

always @(posedge clk_b) begin
  if (we_b)
    ram[addr_b] <= data_b;
  q_b <= ram[addr_b];
end

endmodule

// Four rows x 32 columns, packed by column, in one simple-dual-port M9K.
// Concurrent reads and writes use different column addresses while streaming.
// Read-during-write collisions during seeding have no valid consumer.
module city_row_history (
  input wire clk,
  input wire we,
  input wire [4:0] waddr, raddr,
  input wire [31:0] din,
  output reg [31:0] q
);
(* ramstyle = "M9K, no_rw_check" *) reg [31:0] ram [0:31];
always @(posedge clk) begin
  if (we) ram[waddr] <= din;
  q <= ram[raddr];
end
endmodule
