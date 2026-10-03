/***************************************************************************************************
 * city_accelerator_nowater.v -- simple 10 MHz contest/reference implementation
 *
 * Purpose
 *   A deliberately straightforward hardware implementation of the active nescity.c rule.
 *   The CPU/MMIO side remains in the NES/cart clock domain (clk_cpu, nominally 100 MHz),
 *   while the simulation engine runs from a PLL-generated 10 MHz clock (clk_calc).
 *
 * Design philosophy
 *   - Keep the cell-update equations visibly close to the C source.
 *   - Use ordinary Verilog *, /, +, -, and if statements rather than hand-optimized
 *     shifts, lookup divisions, or long micro-optimized arithmetic FSMs.
 *   - Spend several 10 MHz clocks per cell so the design is not sensitive to a 100 MHz
 *     critical path.
 *   - Use a small toggle handshake for START/DONE clock-domain crossing.
 *   - Use a dual-clock true-dual-port RAM: calculator on port A, CPU on port B.
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

localparam integer W = 32;
localparam integer H = 30;
localparam integer N = 960;
localparam integer WEIGHT_SUM = 65;

localparam integer SEA                  = 0;
localparam integer FORCE_THRESHOLD      = 20;
localparam integer FORCE_SCALE          = 20;
localparam integer MAX_RISE             = 1;
localparam integer MAX_FALL             = 3;
localparam integer DECLINE_STRENGTH     = 1;
localparam integer NATURE_DECAY         = 1;
localparam integer NATURE_RECOVERY      = 1;
localparam integer CAPACITY_LIMIT       = 55;
localparam integer CAPACITY_SCALE       = 3;
localparam integer OVERCROWDING_DECAY   = 4;

// Number of accelerator steps executed between screen redraws by the NES program.
// 1..255 are returned directly from $7F03; 256 is encoded as 00h.
localparam integer DISPLAY_STEP_INTERVAL = 256;
localparam [7:0] DISPLAY_STEP_INTERVAL_CODE =
    (DISPLAY_STEP_INTERVAL == 256) ? 8'h00 : DISPLAY_STEP_INTERVAL;

localparam [10:0] GRID_BANK0_BASE = 11'd0;
localparam [10:0] GRID_BANK1_BASE = 11'd960;

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
// Private grid RAM
//   Port A: clk_calc, calculator
//   Port B: clk_cpu, CPU grid window
// -------------------------------------------------------------------------------------------------
reg        calc_grid_we;
reg [10:0] calc_grid_addr;
reg [7:0]  calc_grid_din;
wire [7:0] calc_grid_dout;

wire       cpu_grid_ram_we;
wire [10:0] cpu_grid_ram_addr;
wire [7:0] cpu_grid_ram_dout;

function [10:0] bank_base;
  input bank;
  begin
    bank_base = bank ? GRID_BANK1_BASE : GRID_BANK0_BASE;
  end
endfunction

function [10:0] bank_addr;
  input bank;
  input [9:0] index;
  begin
    bank_addr = bank_base(bank) + {1'b0, index};
  end
endfunction

assign cpu_grid_ram_addr = bank_addr(active_bank_cpu, cpu_grid_index);
assign cpu_grid_ram_we   = cpu_grid_sel && cpu_we && !busy_cpu;

city_grid_dpram_dc #(.ADDR_WIDTH(11), .DATA_WIDTH(8)) city_grid_ram
(
  .clk_a  (clk_calc),
  .we_a   (calc_grid_we),
  .addr_a (calc_grid_addr),
  .data_a (calc_grid_din),
  .q_a    (calc_grid_dout),

  .clk_b  (clk_cpu),
  .we_b   (cpu_grid_ram_we),
  .addr_b (cpu_grid_ram_addr),
  .data_b (cpu_din),
  .q_b    (cpu_grid_ram_dout)
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
// Simple 10 MHz calculator
// -------------------------------------------------------------------------------------------------
localparam [3:0] ST_IDLE       = 4'd0;
localparam [3:0] ST_CELL_INIT  = 4'd1;
localparam [3:0] ST_NEIGH_ADDR = 4'd2;
localparam [3:0] ST_NEIGH_WAIT = 4'd3;
localparam [3:0] ST_NEIGH_DATA = 4'd4;
localparam [3:0] ST_AVG        = 4'd5;
localparam [3:0] ST_BIAS       = 4'd6;
localparam [3:0] ST_DELTA      = 4'd7;
localparam [3:0] ST_NEW_VALUE  = 4'd8;
localparam [3:0] ST_WRITE      = 4'd9;
localparam [3:0] ST_FINISH     = 4'd10;

reg [3:0] calc_state;

// START synchronizer.
reg start_toggle_sync1;
reg start_toggle_sync2;
reg start_toggle_seen_calc;
reg start_bank_sync1;
reg start_bank_sync2;
reg source_bank_calc;

// DONE toggle is declared with the CPU synchronizer declarations above.
reg [9:0] cell_index;
reg [4:0] neigh_index;
reg signed [15:0] raw_force_acc;
reg [15:0] weighted_sum_acc;
reg [5:0] sea_count;
reg [5:0] mountain_count;
reg [5:0] natural_count;
reg [5:0] urban_count;
reg [5:0] house_count;
reg [5:0] shop_count;
reg [5:0] tall_count;
reg [7:0] self_value;
reg [7:0] avg_value;
reg signed [15:0] bias_value;
reg signed [15:0] delta_value;
reg [7:0] next_value;

wire start_event_calc = start_toggle_sync2 ^ start_toggle_seen_calc;

function [4:0] idx_to_x;
  input [9:0] index;
  begin
    idx_to_x = index[4:0];
  end
endfunction

function [4:0] idx_to_y;
  input [9:0] index;
  begin
    idx_to_y = index[9:5];
  end
endfunction

function signed [3:0] neigh_dx;
  input [4:0] ni;
  begin
    case (ni)
      5'd0, 5'd5, 5'd10, 5'd15, 5'd20: neigh_dx = -2;
      5'd1, 5'd6, 5'd11, 5'd16, 5'd21: neigh_dx = -1;
      5'd2, 5'd7, 5'd12, 5'd17, 5'd22: neigh_dx =  0;
      5'd3, 5'd8, 5'd13, 5'd18, 5'd23: neigh_dx =  1;
      default:                           neigh_dx =  2;
    endcase
  end
endfunction

function signed [3:0] neigh_dy;
  input [4:0] ni;
  begin
    case (ni)
      5'd0,  5'd1,  5'd2,  5'd3,  5'd4:  neigh_dy = -2;
      5'd5,  5'd6,  5'd7,  5'd8,  5'd9:  neigh_dy = -1;
      5'd10, 5'd11, 5'd12, 5'd13, 5'd14: neigh_dy =  0;
      5'd15, 5'd16, 5'd17, 5'd18, 5'd19: neigh_dy =  1;
      default:                             neigh_dy =  2;
    endcase
  end
endfunction

function [4:0] wrap_x;
  input signed [6:0] x;
  begin
    wrap_x = x[4:0]; // W=32
  end
endfunction

function [4:0] wrap_y;
  input signed [6:0] y;
  begin
    if (y < 0)       wrap_y = y + H;
    else if (y >= H) wrap_y = y - H;
    else             wrap_y = y[4:0];
  end
endfunction

function [9:0] neighbor_cell_index;
  input [9:0] center_index;
  input [4:0] ni;
  reg [4:0] cx;
  reg [4:0] cy;
  reg [4:0] nx;
  reg [4:0] ny;
  begin
    cx = idx_to_x(center_index);
    cy = idx_to_y(center_index);
    nx = wrap_x($signed({1'b0, cx}) + neigh_dx(ni));
    ny = wrap_y($signed({1'b0, cy}) + neigh_dy(ni));
    neighbor_cell_index = ({5'd0, ny} << 5) + {5'd0, nx};
  end
endfunction

function [2:0] weight_by_index;
  input [4:0] ni;
  begin
    case (ni)
       0,  4, 20, 24: weight_by_index = 3'd1;
       1,  3,  5,  9, 15, 19, 21, 23: weight_by_index = 3'd2;
       2,  6,  8, 10, 14, 16, 18, 22: weight_by_index = 3'd3;
       7, 11, 13, 17: weight_by_index = 3'd4;
      12: weight_by_index = 3'd5;
      default: weight_by_index = 3'd1;
    endcase
  end
endfunction

// Intentionally written like nescity.c.  Multiplication is expressed as '*'
// and left to Quartus to map to DSP/multiplier resources or logic.
function signed [15:0] influence_of_simple;
  input [7:0] v;
  input [2:0] w;
  integer f;
  begin
    if (v == SEA)       f =  3 * w;
    else if (v <= 1)    f = -2 * w;
    else if (v <= 14)   f = -2 * w;
    else if (v <= 24)   f =  2;
    else if (v <= 34)   f =  1 * w;
    else if (v <= 49)   f =  2 * w;
    else if (v <= 62)   f =  3 * w;
    else if (v <= 78)   f =  2 * w;
    else                f =  1 * w;
    influence_of_simple = f;
  end
endfunction

function [15:0] weighted_product_simple;
  input [7:0] v;
  input [2:0] w;
  integer p;
  begin
    p = v * w;
    weighted_product_simple = p[15:0];
  end
endfunction

// C-like bias calculation.  Constant divisions are deliberately written as '/'.
function signed [15:0] calc_bias_simple;
  input [7:0] self;
  input signed [15:0] raw_force;
  input [7:0] avg;
  input [5:0] sea;
  input [5:0] mountain;
  input [5:0] natural;
  input [5:0] urban;
  input [5:0] house;
  input [5:0] shop;
  input [5:0] tall;
  integer b;
  integer isolation;
  integer natural_pressure;
  integer over_capacity;
  integer rf;
  begin
    b = 0;
    rf = raw_force;

    if (self <= 1)
      b = b - 4;

    if (self >= 1 && self <= 34) begin
      if (house >= 2) b = b + 2;
      if (house >= 4) b = b + 2;
      if (house >= 7) b = b + 2;
      if (shop >= 1 && house >= 3) b = b + 1;
      if (sea >= 4) b = b - 2;
      if (mountain >= 3 && house < 4) b = b - 3;
      if (house < 2) b = b - 3;
    end

    if (self >= 35 && self <= 62) begin
      if (shop >= 1) b = b + 2;
      if (tall >= 2) b = b + 1;
    end

    if (self >= 63) begin
      if ((shop + tall) >= 4) b = b + 2;
    end

    if (self >= 35) begin
      isolation = (urban < 5) ? (5 - urban) : 0;
      natural_pressure = natural + sea * 2 + mountain * 2;
      b = b - DECLINE_STRENGTH * isolation;
      b = b - NATURE_DECAY * (natural_pressure / 16);
      // water_penalty is zero in the active C source.
    end

    over_capacity = avg - CAPACITY_LIMIT;
    if (over_capacity > 0)
      b = b - (over_capacity / CAPACITY_SCALE) * OVERCROWDING_DECAY;

    if (self >= 25 && self < 35 && urban < 3 && rf < 12)
      b = b - NATURE_RECOVERY;

    if (rf < 15 && self < 35)
      b = b - 1;

    calc_bias_simple = b;
  end
endfunction

function signed [15:0] calc_delta_simple;
  input [7:0] self;
  input signed [15:0] raw_force;
  input [7:0] avg;
  input signed [15:0] bias;
  integer d;
  integer rf;
  integer b;
  integer a;
  integer s;
  begin
    rf = raw_force;
    b = bias;
    a = avg;
    s = self;

    // Verilog signed integer division truncates toward zero, matching C here.
    d = (rf - FORCE_THRESHOLD) / FORCE_SCALE + b;
    if (a > s + 12) d = d + 1;
    if (a < s - 20) d = d - 1;

    if (d > MAX_RISE)  d = MAX_RISE;
    if (d < -MAX_FALL) d = -MAX_FALL;

    calc_delta_simple = d;
  end
endfunction

function [7:0] calc_new_value_simple;
  input [7:0] self;
  input signed [15:0] delta;
  integer nv;
  integer d;
  begin
    d = delta;
    if (self == SEA) begin
      calc_new_value_simple = 8'd0;
    end else begin
      nv = self + d;
      if (nv < 1)   nv = 1;
      if (nv > 100) nv = 100;
      if (self <= 1 && nv > 8) nv = 8;
      calc_new_value_simple = nv[7:0];
    end
  end
endfunction

always @(posedge clk_calc or posedge reset) begin
  if (reset) begin
    start_toggle_sync1     <= 1'b0;
    start_toggle_sync2     <= 1'b0;
    start_toggle_seen_calc <= 1'b0;
    start_bank_sync1       <= 1'b0;
    start_bank_sync2       <= 1'b0;
    source_bank_calc       <= 1'b0;
    done_toggle_calc       <= 1'b0;

    calc_state       <= ST_IDLE;
    cell_index       <= 10'd0;
    neigh_index      <= 5'd0;
    raw_force_acc    <= 16'sd0;
    weighted_sum_acc <= 16'd0;
    sea_count        <= 6'd0;
    mountain_count   <= 6'd0;
    natural_count    <= 6'd0;
    urban_count      <= 6'd0;
    house_count      <= 6'd0;
    shop_count       <= 6'd0;
    tall_count       <= 6'd0;
    self_value       <= 8'd0;
    avg_value        <= 8'd0;
    bias_value       <= 16'sd0;
    delta_value      <= 16'sd0;
    next_value       <= 8'd0;
    calc_grid_we     <= 1'b0;
    calc_grid_addr   <= 11'd0;
    calc_grid_din    <= 8'd0;
  end else begin
    calc_grid_we <= 1'b0;

    // Synchronize the START toggle and associated stable bank bit.
    start_toggle_sync1 <= start_toggle_cpu;
    start_toggle_sync2 <= start_toggle_sync1;
    start_bank_sync1   <= start_bank_cpu;
    start_bank_sync2   <= start_bank_sync1;

    case (calc_state)
      ST_IDLE: begin
        if (start_event_calc) begin
          start_toggle_seen_calc <= start_toggle_sync2;
          source_bank_calc <= start_bank_sync2;
          cell_index <= 10'd0;
          neigh_index <= 5'd0;
          calc_state <= ST_CELL_INIT;
        end
      end

      ST_CELL_INIT: begin
        neigh_index      <= 5'd0;
        raw_force_acc    <= 16'sd0;
        weighted_sum_acc <= 16'd0;
        sea_count        <= 6'd0;
        mountain_count   <= 6'd0;
        natural_count    <= 6'd0;
        urban_count      <= 6'd0;
        house_count      <= 6'd0;
        shop_count       <= 6'd0;
        tall_count       <= 6'd0;
        self_value       <= 8'd0;

        calc_grid_addr <= bank_addr(source_bank_calc,
                                    neighbor_cell_index(cell_index, 5'd0));
        calc_state <= ST_NEIGH_WAIT;
      end

      ST_NEIGH_ADDR: begin
        calc_grid_addr <= bank_addr(source_bank_calc,
                                    neighbor_cell_index(cell_index, neigh_index));
        calc_state <= ST_NEIGH_WAIT;
      end

      // Port A RAM is synchronous-read.  Give it one complete 10 MHz cycle after
      // changing the address before consuming q_a.
      ST_NEIGH_WAIT: begin
        calc_state <= ST_NEIGH_DATA;
      end

      ST_NEIGH_DATA: begin
        raw_force_acc <= raw_force_acc +
                         influence_of_simple(calc_grid_dout, weight_by_index(neigh_index));
        weighted_sum_acc <= weighted_sum_acc +
                            weighted_product_simple(calc_grid_dout, weight_by_index(neigh_index));

        if (calc_grid_dout == SEA)                         sea_count      <= sea_count + 6'd1;
        if (calc_grid_dout <= 1 && calc_grid_dout != SEA) mountain_count <= mountain_count + 6'd1;
        if (calc_grid_dout >= 1  && calc_grid_dout <= 24) natural_count  <= natural_count + 6'd1;
        if (calc_grid_dout >= 35 && calc_grid_dout <=100) urban_count    <= urban_count + 6'd1;
        if (calc_grid_dout >= 35 && calc_grid_dout <= 62) house_count    <= house_count + 6'd1;
        if (calc_grid_dout >= 63 && calc_grid_dout <= 78) shop_count     <= shop_count + 6'd1;
        if (calc_grid_dout >= 79 && calc_grid_dout <=100) tall_count     <= tall_count + 6'd1;

        if (neigh_index == 5'd12)
          self_value <= calc_grid_dout;

        if (neigh_index == 5'd24) begin
          calc_state <= ST_AVG;
        end else begin
          neigh_index <= neigh_index + 5'd1;
          calc_state <= ST_NEIGH_ADDR;
        end
      end

      // The accumulators already include neighbor #24 when this state executes.
      ST_AVG: begin
        avg_value <= weighted_sum_acc / WEIGHT_SUM;
        calc_state <= ST_BIAS;
      end

      ST_BIAS: begin
        bias_value <= calc_bias_simple(
            self_value, raw_force_acc, avg_value,
            sea_count, mountain_count, natural_count, urban_count,
            house_count, shop_count, tall_count);
        calc_state <= ST_DELTA;
      end

      ST_DELTA: begin
        delta_value <= calc_delta_simple(self_value, raw_force_acc, avg_value, bias_value);
        calc_state <= ST_NEW_VALUE;
      end

      ST_NEW_VALUE: begin
        next_value <= calc_new_value_simple(self_value, delta_value);
        calc_state <= ST_WRITE;
      end

      ST_WRITE: begin
        calc_grid_we   <= 1'b1;
        calc_grid_addr <= bank_addr(~source_bank_calc, cell_index);
        calc_grid_din  <= next_value;

        if (cell_index == N-1) begin
          calc_state <= ST_FINISH;
        end else begin
          cell_index <= cell_index + 10'd1;
          calc_state <= ST_CELL_INIT;
        end
      end

      ST_FINISH: begin
        // All writes are complete before this event crosses back to clk_cpu.
        done_toggle_calc <= ~done_toggle_calc;
        calc_state <= ST_IDLE;
      end

      default: begin
        calc_state <= ST_IDLE;
      end
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

reg [DATA_WIDTH-1:0] ram [0:(1<<ADDR_WIDTH)-1];

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
