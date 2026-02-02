//
// Alias Transform MAC
// Fixed-point 3x4 matrix multiply + lighting for Quake alias model vertices.
// Register-only peripheral (no SDRAM access needed).
//
// Per vertex: CPU writes packed vertex (4 bytes) to VERT_IN register.
// Hardware computes 3 DotProducts (viewspace XYZ) + lighting in ~8 cycles.
// CPU reads results and does perspective division with hardware FPU.
//
// Pipeline: products registered (prod_r), then 32-bit sum registered (vert_sum_r),
// then translation added from registered sum. This breaks the critical path into:
//   Stage 1: multiply (DSP) → register products
//   Stage 2: 3-way 32-bit sum → register sum
//   Stage 3: sum + translation → register result
// Overlapped so total is still 8 cycles per vertex.
//
// Register map (reg_addr = byte_offset[6:2]):
//   0x00-0x0C: MAT_ROW0[0..3] (W) - Matrix row 0, Q16.16
//   0x10-0x1C: MAT_ROW1[0..3] (W) - Matrix row 1, Q16.16
//   0x20-0x2C: MAT_ROW2[0..3] (W) - Matrix row 2, Q16.16
//   0x30-0x38: LIGHT_VEC[0..2] (W) - Light direction, Q16.16
//   0x3C:      LIGHT_PARAMS   (W) - ambient[15:0], shadelight[31:16]
//   0x40:      VERT_IN        (W) - v0[7:0],v1[15:8],v2[23:16],normalidx[31:24] - triggers compute
//   0x44:      RESULT_VX      (R) - Viewspace X, Q16.16
//   0x48:      RESULT_VY      (R) - Viewspace Y, Q16.16
//   0x4C:      RESULT_VZ      (R) - Viewspace Z, Q16.16
//   0x50:      RESULT_LIGHT   (R) - Lighting value (integer, clamped >= 0)
//   0x54:      STATUS         (R) - bit0=busy
//
// Normal table (norm_wr=1, norm_addr[7:0] = entry index 0..161):
//   Written at init. Each entry: 3 x Q1.15 (16-bit signed) stored as 32-bit words.
//   Word 0: nx[15:0] | ny[31:16]
//   Word 1: nz[15:0] | unused[31:16]
//   Address: norm_addr[7:0] selects entry (0..161), norm_addr[8] selects word (0 or 1)
//

`default_nettype none

module alias_transform_mac (
    input wire        clk,
    input wire        reset_n,

    // CPU register interface
    input wire        reg_wr,
    input wire [4:0]  reg_addr,       // For control registers (0x00-0x54)
    input wire [31:0] reg_wdata,
    output reg [31:0] reg_rdata,

    // Normal table write interface (separate from control regs)
    input wire        norm_wr,        // Write strobe for normal table
    input wire [8:0]  norm_addr,      // [7:0]=entry index, [8]=word select (0=nx/ny, 1=nz)
    input wire [31:0] norm_wdata,

    // Busy flag (active while computing)
    output wire       busy_o
);

// ============================================
// Configuration registers
// ============================================

// 3x4 transformation matrix (Q16.16 signed)
reg signed [31:0] mat [0:2][0:3];

// Lighting parameters
reg signed [31:0] light_vec [0:2];  // Light direction (Q16.16)
reg signed [15:0] ambient;          // Ambient light level (integer)
reg signed [15:0] shadelight;       // Shade light scale (integer)

// ============================================
// Normal table BRAM (162 entries x 3 x Q1.15)
// Store as: word0 = {ny[15:0], nx[15:0]}, word1 = {16'b0, nz[15:0]}
// ============================================
reg [31:0] norm_mem0 [0:161];  // {ny, nx} packed
reg [31:0] norm_mem1 [0:161];  // {0, nz} packed

// Normal table read (1 cycle latency)
reg [31:0] norm_rd0, norm_rd1;
reg [7:0]  norm_rd_addr;

always @(posedge clk) begin
    norm_rd0 <= norm_mem0[norm_rd_addr];
    norm_rd1 <= norm_mem1[norm_rd_addr];
end

// Normal table write
always @(posedge clk) begin
    if (norm_wr) begin
        if (!norm_addr[8])
            norm_mem0[norm_addr[7:0]] <= norm_wdata;
        else
            norm_mem1[norm_addr[7:0]] <= norm_wdata;
    end
end

// ============================================
// Pipeline state machine
// ============================================
// Cycle 0: CPU writes VERT_IN → latch vertex, start BRAM read       (→ ST_NORM_RD)
// Cycle 1: BRAM read completes. Set mul inputs for row 0.            (→ ST_PROD0)
// Cycle 2: Register row 0 products. Set mul for row 1.               (→ ST_SUM0)
// Cycle 3: Register row 0 sum. Register row 1 products. Set row 2.   (→ ST_ACC0)
// Cycle 4: result_vx = sum_r + trans. Reg row 1 sum. Reg row 2 prod. (→ ST_ACC1)
//          Set mul for lighting.
// Cycle 5: result_vy. Reg row 2 sum. Reg lighting products.          (→ ST_ACC2)
// Cycle 6: result_vz. Compute lightcos from registered products.     (→ ST_SHADE)
// Cycle 7: result_light = shade computation.                          (→ ST_IDLE)

localparam ST_IDLE    = 4'd0;
localparam ST_NORM_RD = 4'd1;  // Wait for normal BRAM read
localparam ST_PROD0   = 4'd2;  // Register products for row 0
localparam ST_SUM0    = 4'd3;  // Register row 0 sum, register row 1 products
localparam ST_ACC0    = 4'd4;  // Translate row 0, register row 1 sum, register row 2 products
localparam ST_ACC1    = 4'd5;  // Translate row 1, register row 2 sum, register lighting products
localparam ST_ACC2    = 4'd6;  // Translate row 2, compute lightcos
localparam ST_SHADE   = 4'd7;  // Apply shading + clamp

reg [3:0] state;

// Latched vertex input
reg [7:0] vert_v0, vert_v1, vert_v2;

// Normal components extracted from BRAM (Q1.15)
wire signed [15:0] norm_nx = norm_rd0[15:0];
wire signed [15:0] norm_ny = norm_rd0[31:16];
wire signed [15:0] norm_nz = norm_rd1[15:0];

// ============================================
// MAC datapath (pipelined)
// Stage 1: combinational multiply from registered inputs
// Stage 2: registered products → 32-bit sum → register
// Stage 3: registered sum + translation → result register
// ============================================

// Multiplier inputs (registered)
reg signed [17:0] mul_a0, mul_a1, mul_a2;  // 18-bit signed
reg signed [31:0] mul_b0, mul_b1, mul_b2;  // 32-bit Q16.16

// Combinational products (18 x 32 = 50 bits)
wire signed [49:0] prod0 = mul_a0 * mul_b0;
wire signed [49:0] prod1 = mul_a1 * mul_b1;
wire signed [49:0] prod2 = mul_a2 * mul_b2;

// Registered products (pipeline stage 1)
reg signed [49:0] prod0_r, prod1_r, prod2_r;

// Registered 32-bit sum for vertex transform (pipeline stage 2)
// v(8-bit) * mat(Q16.16) products are Q16.16 in lower 32 bits
reg signed [31:0] vert_sum_r;

// Result registers (Q16.16)
reg signed [31:0] result_vx;
reg signed [31:0] result_vy;
reg signed [31:0] result_vz;
reg signed [31:0] result_light;

// Lighting intermediate
reg signed [31:0] lightcos_q16;

wire busy = (state != ST_IDLE);
assign busy_o = busy;

// ============================================
// Shading multiply (combinational, used in ST_SHADE)
// shadelight (16-bit int) * lightcos_q16 (Q16.16) >> 16 = integer delta
// ============================================
wire signed [47:0] shade_product = $signed(shadelight) * lightcos_q16;
wire signed [31:0] shade_delta = shade_product[47:16];
wire signed [31:0] shaded_light = $signed({{16{ambient[15]}}, ambient}) + shade_delta;
wire signed [31:0] clamped_light = (shaded_light[31]) ? 32'd0 : shaded_light;

// ============================================
// Register read mux
// ============================================
always @(*) begin
    case (reg_addr[4:0])
        5'h11: reg_rdata = result_vx;       // 0x44
        5'h12: reg_rdata = result_vy;       // 0x48
        5'h13: reg_rdata = result_vz;       // 0x4C
        5'h14: reg_rdata = result_light;    // 0x50
        5'h15: reg_rdata = {31'd0, busy};   // 0x54
        default: reg_rdata = 32'd0;
    endcase
end

// ============================================
// Register write + state machine
// ============================================
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE;
        result_vx <= 0;
        result_vy <= 0;
        result_vz <= 0;
        result_light <= 0;
    end else begin
        // Pipeline state machine
        case (state)
            ST_IDLE: begin
                // VERT_IN trigger
                if (reg_wr && reg_addr[4:0] == 5'h10) begin
                    vert_v0 <= reg_wdata[7:0];
                    vert_v1 <= reg_wdata[15:8];
                    vert_v2 <= reg_wdata[23:16];
                    norm_rd_addr <= reg_wdata[31:24];
                    state <= ST_NORM_RD;
                end
            end

            ST_NORM_RD: begin
                // Normal BRAM read in progress (1 cycle latency).
                // Set up multiplier inputs for row 0.
                mul_a0 <= {10'd0, vert_v0};
                mul_a1 <= {10'd0, vert_v1};
                mul_a2 <= {10'd0, vert_v2};
                mul_b0 <= mat[0][0];
                mul_b1 <= mat[0][1];
                mul_b2 <= mat[0][2];
                state <= ST_PROD0;
            end

            ST_PROD0: begin
                // Row 0 products ready (combinational). Register them.
                prod0_r <= prod0;
                prod1_r <= prod1;
                prod2_r <= prod2;
                // Set up multiplier inputs for row 1.
                mul_b0 <= mat[1][0];
                mul_b1 <= mat[1][1];
                mul_b2 <= mat[1][2];
                state <= ST_SUM0;
            end

            ST_SUM0: begin
                // Register 32-bit sum of row 0 products (from prod_r)
                vert_sum_r <= prod0_r[31:0] + prod1_r[31:0] + prod2_r[31:0];
                // Row 1 products ready. Register them.
                prod0_r <= prod0;
                prod1_r <= prod1;
                prod2_r <= prod2;
                // Set up multiplier inputs for row 2.
                mul_b0 <= mat[2][0];
                mul_b1 <= mat[2][1];
                mul_b2 <= mat[2][2];
                state <= ST_ACC0;
            end

            ST_ACC0: begin
                // Row 0: translate from registered sum
                result_vx <= vert_sum_r + mat[0][3];
                // Register 32-bit sum of row 1 products
                vert_sum_r <= prod0_r[31:0] + prod1_r[31:0] + prod2_r[31:0];
                // Row 2 products ready. Register them.
                prod0_r <= prod0;
                prod1_r <= prod1;
                prod2_r <= prod2;
                // Set up lighting multiply: normal (Q1.15) * light_vec (Q16.16)
                mul_a0 <= {{2{norm_nx[15]}}, norm_nx};
                mul_a1 <= {{2{norm_ny[15]}}, norm_ny};
                mul_a2 <= {{2{norm_nz[15]}}, norm_nz};
                mul_b0 <= light_vec[0];
                mul_b1 <= light_vec[1];
                mul_b2 <= light_vec[2];
                state <= ST_ACC1;
            end

            ST_ACC1: begin
                // Row 1: translate from registered sum
                result_vy <= vert_sum_r + mat[1][3];
                // Register 32-bit sum of row 2 products
                vert_sum_r <= prod0_r[31:0] + prod1_r[31:0] + prod2_r[31:0];
                // Lighting products ready. Register them.
                prod0_r <= prod0;
                prod1_r <= prod1;
                prod2_r <= prod2;
                state <= ST_ACC2;
            end

            ST_ACC2: begin
                // Row 2: translate from registered sum
                result_vz <= vert_sum_r + mat[2][3];
                // Lighting DotProduct from registered products:
                // Q1.15 * Q16.16 = Q17.31. Sum is Q19.31. Shift right 15 → Q16.16.
                // Use [46:15] of each product individually then sum (avoids 50-bit carry)
                lightcos_q16 <= prod0_r[46:15] + prod1_r[46:15] + prod2_r[46:15];
                state <= ST_SHADE;
            end

            ST_SHADE: begin
                // lightcos < 0: ambient + shadelight*lightcos/65536 (clamped >= 0)
                // lightcos >= 0: just ambient
                if (lightcos_q16[31]) begin
                    result_light <= clamped_light;
                end else begin
                    result_light <= {{16{ambient[15]}}, ambient};
                end
                state <= ST_IDLE;
            end

            default: state <= ST_IDLE;
        endcase

        // Register writes (active during any state except VERT_IN which is in ST_IDLE)
        if (reg_wr) begin
            case (reg_addr[4:0])
                // Matrix row 0
                5'h00: mat[0][0] <= reg_wdata;
                5'h01: mat[0][1] <= reg_wdata;
                5'h02: mat[0][2] <= reg_wdata;
                5'h03: mat[0][3] <= reg_wdata;
                // Matrix row 1
                5'h04: mat[1][0] <= reg_wdata;
                5'h05: mat[1][1] <= reg_wdata;
                5'h06: mat[1][2] <= reg_wdata;
                5'h07: mat[1][3] <= reg_wdata;
                // Matrix row 2
                5'h08: mat[2][0] <= reg_wdata;
                5'h09: mat[2][1] <= reg_wdata;
                5'h0A: mat[2][2] <= reg_wdata;
                5'h0B: mat[2][3] <= reg_wdata;
                // Light vector
                5'h0C: light_vec[0] <= reg_wdata;
                5'h0D: light_vec[1] <= reg_wdata;
                5'h0E: light_vec[2] <= reg_wdata;
                // Light params
                5'h0F: begin
                    ambient    <= reg_wdata[15:0];
                    shadelight <= reg_wdata[31:16];
                end
                default: ;
            endcase
        end
    end
end

endmodule
