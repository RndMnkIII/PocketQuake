//
// DMA Clear/Blit Engine
// Fast SDRAM fill and copy operations for framebuffer/zbuffer clearing
//
// Register map (active_addr[4:2] selects register):
//   0x00: DMA_SRC_ADDR   (RW) - Source SDRAM byte address (for copy mode)
//   0x04: DMA_DST_ADDR   (RW) - Destination SDRAM byte address
//   0x08: DMA_LENGTH     (RW) - Transfer length in bytes (must be 4-byte aligned)
//   0x0C: DMA_FILL_DATA  (RW) - 32-bit fill pattern
//   0x10: DMA_CONTROL    (W)  - bit0=start, bit1=mode (0=fill, 1=copy)
//   0x14: DMA_STATUS     (R)  - bit0=busy
//

`default_nettype none

module dma_clear_blit (
    input wire        clk,
    input wire        reset_n,

    // CPU register interface
    input wire        reg_wr,          // Write strobe (active for 1 cycle)
    input wire [4:0]  reg_addr,        // Register address = byte_offset[6:2]
    input wire [31:0] reg_wdata,       // Write data
    output reg [31:0] reg_rdata,       // Read data (active same cycle)

    // SDRAM word interface (active for 1 cycle each)
    output reg        sdram_rd,
    output reg        sdram_wr,
    output reg [23:0] sdram_addr,      // 24-bit word address (byte_addr >> 2)
    output reg [31:0] sdram_wdata,
    output reg [3:0]  sdram_wstrb,
    input wire [31:0] sdram_rdata,
    input wire        sdram_busy,
    input wire        sdram_rdata_valid,

    // Status
    output wire       active           // DMA is running (blocks CPU SDRAM access)
);

// Configuration registers
reg [31:0] src_addr_reg;    // Source byte address
reg [31:0] dst_addr_reg;    // Destination byte address
reg [31:0] length_reg;      // Length in bytes
reg [31:0] fill_data_reg;   // Fill pattern
reg        copy_mode;       // 0=fill, 1=copy

// DMA state machine
localparam ST_IDLE       = 3'd0;
localparam ST_FILL_ISSUE = 3'd1;
localparam ST_FILL_WAIT  = 3'd2;
localparam ST_COPY_READ  = 3'd3;
localparam ST_COPY_RWAIT = 3'd4;
localparam ST_COPY_WRITE = 3'd5;
localparam ST_COPY_WWAIT = 3'd6;

reg [2:0]  state;
reg [31:0] cur_src;         // Current source byte address
reg [31:0] cur_dst;         // Current destination byte address
reg [31:0] remaining;       // Remaining bytes to transfer
reg [31:0] copy_buf;        // Temporary buffer for copy read data
reg        cmd_issued;      // Command was issued, waiting for busy to assert
reg        seen_busy;       // Busy has asserted for current write command

assign active = (state != ST_IDLE);

// Register read mux (active same cycle)
always @(*) begin
    case (reg_addr[2:0])
        3'd0: reg_rdata = src_addr_reg;
        3'd1: reg_rdata = dst_addr_reg;
        3'd2: reg_rdata = length_reg;
        3'd3: reg_rdata = fill_data_reg;
        3'd4: reg_rdata = 32'd0;           // CONTROL is write-only
        3'd5: reg_rdata = {31'd0, active};  // STATUS
        default: reg_rdata = 32'd0;
    endcase
end

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        src_addr_reg <= 32'd0;
        dst_addr_reg <= 32'd0;
        length_reg <= 32'd0;
        fill_data_reg <= 32'd0;
        copy_mode <= 1'b0;
        state <= ST_IDLE;
        cur_src <= 32'd0;
        cur_dst <= 32'd0;
        remaining <= 32'd0;
        copy_buf <= 32'd0;
        cmd_issued <= 1'b0;
        seen_busy <= 1'b0;
        sdram_rd <= 1'b0;
        sdram_wr <= 1'b0;
        sdram_addr <= 24'd0;
        sdram_wdata <= 32'd0;
        sdram_wstrb <= 4'b0;
    end else begin
        // Default: deassert single-cycle SDRAM signals
        sdram_rd <= 1'b0;
        sdram_wr <= 1'b0;

        // Register writes (only when idle)
        if (reg_wr && !active) begin
            case (reg_addr[2:0])
                3'd0: src_addr_reg <= reg_wdata;
                3'd1: dst_addr_reg <= reg_wdata;
                3'd2: length_reg <= reg_wdata;
                3'd3: fill_data_reg <= reg_wdata;
                3'd4: begin
                    // CONTROL: start transfer
                    if (reg_wdata[0] && length_reg != 0) begin
                        copy_mode <= reg_wdata[1];
                        cur_src <= src_addr_reg;
                        cur_dst <= dst_addr_reg;
                        remaining <= length_reg;
                        cmd_issued <= 1'b0;
                        seen_busy <= 1'b0;
                        if (reg_wdata[1])
                            state <= ST_COPY_READ;
                        else
                            state <= ST_FILL_ISSUE;
                    end
                end
                default: ;
            endcase
        end

        // DMA state machine
        case (state)
            ST_IDLE: begin
                // Nothing to do
            end

            // ---- Fill mode ----
            ST_FILL_ISSUE: begin
                if (!sdram_busy) begin
                    sdram_wr <= 1'b1;
                    sdram_addr <= cur_dst[25:2];
                    sdram_wdata <= fill_data_reg;
                    sdram_wstrb <= 4'b1111;
                    cmd_issued <= 1'b1;
                    seen_busy <= 1'b0;
                    state <= ST_FILL_WAIT;
                end
            end

            ST_FILL_WAIT: begin
                // Wait for write to complete (must observe busy high then low)
                if (sdram_busy) begin
                    seen_busy <= 1'b1;
                end
                if (cmd_issued && seen_busy && !sdram_busy) begin
                    // Write accepted and completed
                    cmd_issued <= 1'b0;
                    cur_dst <= cur_dst + 32'd4;
                    remaining <= remaining - 32'd4;
                    if (remaining <= 32'd4)
                        state <= ST_IDLE;
                    else
                        state <= ST_FILL_ISSUE;
                end
            end

            // ---- Copy mode ----
            ST_COPY_READ: begin
                if (!sdram_busy) begin
                    sdram_rd <= 1'b1;
                    sdram_addr <= cur_src[25:2];
                    cmd_issued <= 1'b1;
                    state <= ST_COPY_RWAIT;
                end
            end

            ST_COPY_RWAIT: begin
                if (sdram_rdata_valid) begin
                    copy_buf <= sdram_rdata;
                    cmd_issued <= 1'b0;
                    state <= ST_COPY_WRITE;
                end
            end

            ST_COPY_WRITE: begin
                if (!sdram_busy) begin
                    sdram_wr <= 1'b1;
                    sdram_addr <= cur_dst[25:2];
                    sdram_wdata <= copy_buf;
                    sdram_wstrb <= 4'b1111;
                    cmd_issued <= 1'b1;
                    seen_busy <= 1'b0;
                    state <= ST_COPY_WWAIT;
                end
            end

            ST_COPY_WWAIT: begin
                if (sdram_busy) begin
                    seen_busy <= 1'b1;
                end
                if (cmd_issued && seen_busy && !sdram_busy) begin
                    cmd_issued <= 1'b0;
                    cur_src <= cur_src + 32'd4;
                    cur_dst <= cur_dst + 32'd4;
                    remaining <= remaining - 32'd4;
                    if (remaining <= 32'd4)
                        state <= ST_IDLE;
                    else
                        state <= ST_COPY_READ;
                end
            end

            default: state <= ST_IDLE;
        endcase
    end
end

endmodule
